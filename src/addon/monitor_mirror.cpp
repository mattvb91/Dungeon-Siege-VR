#include "monitor_mirror.h"

#include <d3dcompiler.h>

#include "common/log.h"
#include "common/stereo_abi.h"

using Microsoft::WRL::ComPtr;

namespace dsvr
{
    namespace
    {
        // Root constants: uvOffset, uvScale (source region of the frame), markerUV (its bottom-right pixel).
        const char kShader[] =
            "Texture2D src : register(t0);\n"
            "SamplerState smp : register(s0);\n"
            "cbuffer C : register(b0) { float2 uvOffset; float2 uvScale; float2 markerUV; float2 pad; };\n"
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) {\n"
            "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
            "  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uvOffset + t * uvScale; return o; }\n"
            "float4 ps(V i) : SV_Target {\n"
            "  float3 m = src.SampleLevel(smp, markerUV, 0).rgb * 255.0;\n"
            "  bool marked = abs(m.r - 165.0) <= 6.0 && (m.b <= 48.0 || m.b >= 200.0 || (m.b >= 92.0 && m.b <= 156.0));\n"
            "  if (marked && m.b > 48.0) discard;\n"  // right eye or interface frame: keep the previous image
            "  return float4(src.SampleLevel(smp, i.uv, 0).rgb, 1); }\n";

        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT before, UINT after)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATES(before);
            b.Transition.StateAfter = D3D12_RESOURCE_STATES(after);
            return b;
        }
    }

    bool MonitorMirror::BeginAdapter(ID3D12Root* root, UInt32 adapterID)
    {
        m_device = root->GetDevice(adapterID);
        m_failed = false;

        if (!m_ps)
        {
            ComPtr<ID3DBlob> errors;
            HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_mirror", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &m_vs, &errors);
            if (SUCCEEDED(hr))
                hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_mirror", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &m_ps, &errors);
            if (FAILED(hr))
            {
                Log("monitor mirror: shader compile failed 0x%08lX %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                m_ps.Reset();
                m_failed = true;
                return false;
            }
        }

        D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 8;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsDesc = { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* errors = nullptr;
        m_rootSignature = root->SerializeAndCreateRootSignature(adapterID, D3D_ROOT_SIGNATURE_VERSION_1, &rsDesc, &errors);
        if (errors)
            errors->Release();
        if (!m_rootSignature)
        {
            Log("monitor mirror: root signature creation failed");
            m_failed = true;
            return false;
        }

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        D3D12_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;
        D3D12_DEPTH_STENCIL_DESC depth = {};

        m_pipeline = {};
        m_pipeline.pRootSignature = m_rootSignature;
        m_pipeline.pVS = m_vs.Get();
        m_pipeline.pPS = m_ps.Get();
        m_pipeline.pBlendState = root->PLCacheGetBlend4Desc(adapterID, blend);
        m_pipeline.SampleMask = 0xFFFFFFFF;
        m_pipeline.pRasterizerState = root->PLCacheGetRasterizerDesc(adapterID, raster);
        m_pipeline.pDepthStencilState = root->PLCacheGetDepthStencilDesc(adapterID, depth);
        m_pipeline.pInputLayout = root->PLCacheGetInputLayoutDesc(adapterID, 0, nullptr);
        m_pipeline.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        m_pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m_pipeline.NumRenderTargets = 1;
        m_pipeline.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;  // set per swapchain format in Present
        m_pipeline.DSVFormat = DXGI_FORMAT_UNKNOWN;
        m_pipeline.SampleDesc = { 1, 0 };
        return true;
    }

    void MonitorMirror::EndAdapter(ID3D12Root* root, UInt32 adapterID)
    {
        ReleaseTexture(root, adapterID);
        if (m_rootSignature)
        {
            root->GPLRootSignatureReleased(adapterID, m_rootSignature);
            m_rootSignature->Release();
            m_rootSignature = nullptr;
        }
        m_device = nullptr;
    }

    void MonitorMirror::ReleaseTexture(ID3D12Root* root, UInt32 adapterID)
    {
        if (m_texture)
        {
            // The texture has been through ResourceBarrier on dgVoodoo's command list, so it is tracked.
            root->RTResourceDestroyed(m_texture.Get(), true);
            m_texture.Reset();
        }
        if (m_haveDescriptors)
        {
            root->GetRTV_DescAllocator(adapterID)->DeallocDescriptorGroup(m_rtvHandle, 1);
            root->GetCBV_SRV_UAV_DescAllocator(adapterID)->DeallocDescriptorGroup(m_srvHandle, 1);
            m_haveDescriptors = false;
        }
        m_width = m_height = 0;
    }

    bool MonitorMirror::EnsureTexture(ID3D12Root* root, UInt32 adapterID, UINT width, UINT height, DXGI_FORMAT format)
    {
        if (m_texture && width == m_width && height == m_height && format == m_format)
            return true;
        ReleaseTexture(root, adapterID);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = format;
        HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_texture));
        if (FAILED(hr))
        {
            Log("monitor mirror: texture creation failed 0x%08lX", hr);
            return false;
        }

        ID3D12ResourceDescAllocator* rtvs = root->GetRTV_DescAllocator(adapterID);
        ID3D12ResourceDescAllocator* srvs = root->GetCBV_SRV_UAV_DescAllocator(adapterID);
        m_rtvHandle = rtvs->AllocDescriptorGroup(1);
        m_srvHandle = srvs->AllocDescriptorGroup(1);
        m_haveDescriptors = true;
        m_device->CreateRenderTargetView(m_texture.Get(), nullptr, rtvs->GetCPUDescHandle(m_rtvHandle));
        m_device->CreateShaderResourceView(m_texture.Get(), nullptr, srvs->GetCPUDescHandle(m_srvHandle));

        m_width = width;
        m_height = height;
        m_format = format;
        Log("monitor mirror: %ux%u format %d", width, height, format);
        return true;
    }

    bool MonitorMirror::Present(ID3D12Root* root, UInt32 adapterID, DXGI_FORMAT format,
                                const dgVoodoo::ID3D12RootObserver::PresentBeginContextInput& in,
                                dgVoodoo::ID3D12RootObserver::PresentBeginContextOutput& out)
    {
        if (m_failed || !m_rootSignature || !in.pSrcTexture)
            return false;

        UINT width = UINT(in.srcRect.right - in.srcRect.left);
        UINT height = UINT(in.srcRect.bottom - in.srcRect.top);
        if (!width || !height || !EnsureTexture(root, adapterID, width, height, format))
            return false;

        if (m_pipeline.RTVFormats[0] != format)
            m_pipeline.RTVFormats[0] = format;
        ID3D12PipelineState* pso = root->PLCacheGetGraphicsPipeline(adapterID, m_pipeline);
        if (!pso)
        {
            Log("monitor mirror: pipeline creation failed");
            m_failed = true;
            return false;
        }

        ID3D12GraphicsCommandListAuto* cmd = root->GetGraphicsCommandListAuto(adapterID);
        ID3D12ResourceDescRingBuffer* ring = root->GetCBV_SRV_UAV_RingBuffer(adapterID);
        ID3D12ResourceDescRingBuffer::AllocData srv;
        if (!ring->Alloc(1, cmd->AGetFence(), cmd->GetFenceValue(), srv))
            return false;
        m_device->CopyDescriptorsSimple(1, srv.cpuDescHandle, in.srvCPUHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_RESOURCE_DESC srcDesc = in.pSrcTexture->GetDesc();
        float texW = float(srcDesc.Width), texH = float(srcDesc.Height);
        float constants[8] = {
            in.srcRect.left / texW, in.srcRect.top / texH,
            width / texW, height / texH,
            (in.srcRect.right - 0.5f) / texW, (in.srcRect.bottom - 0.5f) / texH,
            0.0f, 0.0f };

        cmd->ChangeId(this);
        cmd->AFlushLock();
        ID3D12GraphicsCommandList* list = cmd->GetCommandListInterface();

        const UINT psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const UINT rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bool srcNeedsTransition = (in.srcTextureState & psr) == 0;
        D3D12_RESOURCE_BARRIER before[2] = { Transition(m_texture.Get(), psr, rt), Transition(in.pSrcTexture, in.srcTextureState, psr) };
        list->ResourceBarrier(srcNeedsTransition ? 2 : 1, before);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = root->GetRTV_DescAllocator(adapterID)->GetCPUDescHandle(m_rtvHandle);
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, LONG(width), LONG(height) };
        list->SetGraphicsRootSignature(m_rootSignature);
        list->SetPipelineState(pso);
        list->SetDescriptorHeaps(1, &srv.pHeap);
        list->SetGraphicsRootDescriptorTable(0, srv.gpuDescHandle);
        list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER after[2] = { Transition(m_texture.Get(), rt, psr), Transition(in.pSrcTexture, psr, in.srcTextureState) };
        list->ResourceBarrier(srcNeedsTransition ? 2 : 1, after);
        cmd->AFlushUnlock();

        out.pOutputTexture = m_texture.Get();
        out.outputTexSRVCPUHandle = root->GetCBV_SRV_UAV_DescAllocator(adapterID)->GetCPUDescHandle(m_srvHandle);
        out.outputTextureExpectedState = psr;
        return true;
    }
}
