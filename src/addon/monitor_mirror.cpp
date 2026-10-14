#include "monitor_mirror.h"

#include <d3dcompiler.h>

#include "common/log.h"
#include "common/stereo_abi.h"

using Microsoft::WRL::ComPtr;

namespace dsvr
{
    namespace
    {
        // Root constants: uvOffset, uvScale (source region of the frame), the centres of marker cells 0
        // (signature) and 1 (kind), see common/stereo_abi.h, then for the interface: the marker strip's top-left,
        // the key level sample and the parked cursor's rect (all in source texture uv). Marker channels are
        // on/off, read with a threshold. Output pass: the panel as seen from the left eye (PanelProjection;
        // w components: the image's frustum tangents and the panel's half size; panel.w = 0: flat).
        // t0 = the frame (output pass: world), t1 = output pass: interface (premultiplied alpha).
        const UINT kConstantCount = 32;
        const char kShader[] =
            "Texture2D src : register(t0);\n"
            "Texture2D hud : register(t1);\n"
            "SamplerState smp : register(s0);\n"
            "SamplerState smpLinear : register(s1);\n"
            "cbuffer C : register(b0) { float2 uvOffset; float2 uvScale; float2 cell0; float2 cell1;\n"
            "                           float2 markerStart; float2 keyUV; float4 parkRect;\n"
            "                           float4 eyeOrigin; float4 row0; float4 row1; float4 row2; };\n"
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) {\n"
            "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
            "  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uvOffset + t * uvScale; return o; }\n"
            // -1 = unmarked, else FrameKind (left eye 0, right eye 1, interface 2).
            "int Kind() {\n"
            "  float3 s = src.SampleLevel(smp, cell0, 0).rgb;\n"
            "  float3 k = src.SampleLevel(smp, cell1, 0).rgb;\n"
            "  if (!(s.r > 0.39 && s.g > 0.39 && s.b < 0.39 && k.b > 0.39)) return -1;\n"
            "  return (k.r > 0.39 ? 1 : 0) + (k.g > 0.39 ? 2 : 0); }\n"
            "float4 psWorld(V i) : SV_Target {\n"
            "  int kind = Kind();\n"
            "  if (kind > 0) discard;\n"  // right eye or interface: keep previous image
            "  return float4(src.SampleLevel(smp, i.uv, 0).rgb, 1); }\n"
            // As the VR panel's key shader (vr_presenter.cpp): marker strip and parked cursor transparent, magenta
            // key un-blended from translucent pixels.
            "float4 psInterface(V i) : SV_Target {\n"
            "  int kind = Kind();\n"
            "  if (kind < 0) return 0;\n"  // unmarked (menus): the interface is part of the world image
            "  if (kind != 2) discard;\n"
            "  if (i.uv.x >= markerStart.x && i.uv.y >= markerStart.y) return 0;\n"
            "  if (all(i.uv >= parkRect.xy) && all(i.uv < parkRect.zw)) return 0;\n"
            "  float3 c = src.SampleLevel(smp, i.uv, 0).rgb;\n"
            "  float3 k = src.SampleLevel(smp, keyUV, 0).rgb;\n"
            "  float key = (k.g < 0.12 && k.r > 0.5 && k.b > 0.5) ? 0.5 * (k.r + k.b) : 0.894;\n"
            "  float spill = saturate((min(c.r, c.b) - c.g) / key);\n"
            "  float a = 1.0 - spill;\n"
            "  if (a < 0.04) return 0;\n"
            "  return float4(saturate((c - spill * float3(key, 0, key)) / a) * a, a); }\n"
            // The left eye's ray through this pixel (eyeOrigin.w / row0.w: the image's frustum tangents) meets the
            // panel plane (row1.w / row2.w: half width / height) at the interface pixel shown there.
            "float4 psOutput(V i) : SV_Target {\n"
            "  float3 w = src.SampleLevel(smp, i.uv, 0).rgb;\n"
            "  float2 huv = i.uv;\n"
            "  if (row1.w > 0) {\n"
            "    float3 d = float3((2 * i.uv.x - 1) * eyeOrigin.w, (1 - 2 * i.uv.y) * row0.w, -1);\n"
            "    d = float3(dot(row0.xyz, d), dot(row1.xyz, d), dot(row2.xyz, d));\n"
            "    if (d.z >= 0 || eyeOrigin.z <= 0) return float4(w, 1);\n"
            "    float3 p = eyeOrigin.xyz - (eyeOrigin.z / d.z) * d;\n"
            "    huv = float2(0.5 + 0.5 * p.x / row1.w, 0.5 - 0.5 * p.y / row2.w);\n"
            "    if (any(huv < 0) || any(huv > 1)) return float4(w, 1);\n"
            "  }\n"
            "  float4 h = hud.SampleLevel(smpLinear, huv, 0);\n"
            "  return float4(w * (1 - h.a) + h.rgb, 1); }\n";

        const char* const kPixelEntry[] = { "psWorld", "psInterface", "psOutput" };

        // The interface keeps its own alpha whatever the swapchain format.
        const DXGI_FORMAT kInterfaceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

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

        if (!m_vs)
        {
            ComPtr<ID3DBlob> errors;
            HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_mirror", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &m_vs, &errors);
            for (int t = 0; t < kTargetCount && SUCCEEDED(hr); ++t)
                hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_mirror", nullptr, nullptr, kPixelEntry[t], "ps_5_0", 0, 0, &m_ps[t], &errors);
            if (FAILED(hr))
            {
                Log("monitor mirror: shader compile failed 0x%08lX %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                m_vs.Reset();
                m_failed = true;
                return false;
            }
        }

        D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = kConstantCount;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for (UINT s = 0; s < 2; ++s)
        {
            samplers[s].Filter = s == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[s].AddressU = samplers[s].AddressV = samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[s].ShaderRegister = s;
            samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rsDesc = { 2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
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

        for (int t = 0; t < kTargetCount; ++t)
        {
            ID3D12Root::GraphicsPLDesc& p = m_pipeline[t];
            p = {};
            p.pRootSignature = m_rootSignature;
            p.pVS = m_vs.Get();
            p.pPS = m_ps[t].Get();
            p.pBlendState = root->PLCacheGetBlend4Desc(adapterID, blend);
            p.SampleMask = 0xFFFFFFFF;
            p.pRasterizerState = root->PLCacheGetRasterizerDesc(adapterID, raster);
            p.pDepthStencilState = root->PLCacheGetDepthStencilDesc(adapterID, depth);
            p.pInputLayout = root->PLCacheGetInputLayoutDesc(adapterID, 0, nullptr);
            p.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets = 1;
            p.RTVFormats[0] = t == kInterface ? kInterfaceFormat : DXGI_FORMAT_UNKNOWN;  // else per swapchain format
            p.DSVFormat = DXGI_FORMAT_UNKNOWN;
            p.SampleDesc = { 1, 0 };
        }
        return true;
    }

    void MonitorMirror::EndAdapter(ID3D12Root* root, UInt32 adapterID)
    {
        ReleaseTextures(root, adapterID);
        if (m_rootSignature)
        {
            root->GPLRootSignatureReleased(adapterID, m_rootSignature);
            m_rootSignature->Release();
            m_rootSignature = nullptr;
        }
        m_device = nullptr;
    }

    void MonitorMirror::ReleaseTextures(ID3D12Root* root, UInt32 adapterID)
    {
        for (RenderTarget& target : m_targets)
        {
            if (target.texture)
            {
                // The textures have been through ResourceBarrier on dgVoodoo's command list, so they are tracked.
                root->RTResourceDestroyed(target.texture.Get(), true);
                target.texture.Reset();
            }
            if (m_haveTextures)
            {
                root->GetRTV_DescAllocator(adapterID)->DeallocDescriptorGroup(target.rtvHandle, 1);
                root->GetCBV_SRV_UAV_DescAllocator(adapterID)->DeallocDescriptorGroup(target.srvHandle, 1);
            }
        }
        m_haveTextures = false;
        m_width = m_height = 0;
    }

    bool MonitorMirror::EnsureTextures(ID3D12Root* root, UInt32 adapterID, UINT width, UINT height, DXGI_FORMAT format)
    {
        if (m_haveTextures && width == m_width && height == m_height && format == m_format)
            return true;
        ReleaseTextures(root, adapterID);

        ID3D12ResourceDescAllocator* rtvs = root->GetRTV_DescAllocator(adapterID);
        ID3D12ResourceDescAllocator* srvs = root->GetCBV_SRV_UAV_DescAllocator(adapterID);
        for (int t = 0; t < kTargetCount; ++t)
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = t == kInterface ? kInterfaceFormat : format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = desc.Format;
            RenderTarget& target = m_targets[t];
            HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&target.texture));
            if (FAILED(hr))
            {
                Log("monitor mirror: texture creation failed 0x%08lX", hr);
                for (int u = 0; u < t; ++u)
                {
                    rtvs->DeallocDescriptorGroup(m_targets[u].rtvHandle, 1);
                    srvs->DeallocDescriptorGroup(m_targets[u].srvHandle, 1);
                    m_targets[u].texture.Reset();
                }
                return false;
            }
            target.rtvHandle = rtvs->AllocDescriptorGroup(1);
            target.srvHandle = srvs->AllocDescriptorGroup(1);
            m_device->CreateRenderTargetView(target.texture.Get(), nullptr, rtvs->GetCPUDescHandle(target.rtvHandle));
            m_device->CreateShaderResourceView(target.texture.Get(), nullptr, srvs->GetCPUDescHandle(target.srvHandle));
        }

        m_haveTextures = true;
        m_cleared = false;
        m_width = width;
        m_height = height;
        m_format = format;
        Log("monitor mirror: %ux%u format %d", width, height, format);
        return true;
    }

    bool MonitorMirror::Present(ID3D12Root* root, UInt32 adapterID, DXGI_FORMAT format, bool cursorParked, const PanelProjection* panel,
                                const dgVoodoo::ID3D12RootObserver::PresentBeginContextInput& in,
                                dgVoodoo::ID3D12RootObserver::PresentBeginContextOutput& out)
    {
        if (m_failed || !m_rootSignature || !in.pSrcTexture)
            return false;

        UINT width = UINT(in.srcRect.right - in.srcRect.left);
        UINT height = UINT(in.srcRect.bottom - in.srcRect.top);
        if (!width || !height || !EnsureTextures(root, adapterID, width, height, format))
            return false;

        m_pipeline[kWorld].RTVFormats[0] = format;
        m_pipeline[kOutput].RTVFormats[0] = format;
        ID3D12PipelineState* pso[kTargetCount];
        for (int t = 0; t < kTargetCount; ++t)
        {
            pso[t] = root->PLCacheGetGraphicsPipeline(adapterID, m_pipeline[t]);
            if (!pso[t])
            {
                Log("monitor mirror: pipeline creation failed");
                m_failed = true;
                return false;
            }
        }

        // Shader-visible descriptors: the frame twice (t1 unused) for the world and interface passes, then
        // world + interface for the output pass.
        ID3D12GraphicsCommandListAuto* cmd = root->GetGraphicsCommandListAuto(adapterID);
        ID3D12ResourceDescRingBuffer* ring = root->GetCBV_SRV_UAV_RingBuffer(adapterID);
        ID3D12ResourceDescRingBuffer::AllocData srv;
        if (!ring->Alloc(4, cmd->AGetFence(), cmd->GetFenceValue(), srv))
            return false;
        UINT step = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12ResourceDescAllocator* srvs = root->GetCBV_SRV_UAV_DescAllocator(adapterID);
        D3D12_CPU_DESCRIPTOR_HANDLE sources[4] = { in.srvCPUHandle, in.srvCPUHandle,
            srvs->GetCPUDescHandle(m_targets[kWorld].srvHandle), srvs->GetCPUDescHandle(m_targets[kInterface].srvHandle) };
        for (UINT i = 0; i < 4; ++i)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dst = { srv.cpuDescHandle.ptr + i * step };
            m_device->CopyDescriptorsSimple(1, dst, sources[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE frameTable = srv.gpuDescHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE outputTable = { srv.gpuDescHandle.ptr + 2 * step };

        D3D12_RESOURCE_DESC srcDesc = in.pSrcTexture->GetDesc();
        float texW = float(srcDesc.Width), texH = float(srcDesc.Height);
        float left = float(in.srcRect.left), top = float(in.srcRect.top);
        float right = float(in.srcRect.right), bottom = float(in.srcRect.bottom);
        float cell = float(width) / float(kMarkerCellDivisor);
        float cellY = (bottom - 0.5f * cell) / texH;
        float mask = kParkMaskSize * float(width);
        float parkX = left + kParkX * float(width), parkY = top + kParkY * float(height);
        float frameConstants[kConstantCount] = {
            left / texW, top / texH, width / texW, height / texH,
            (right - 0.5f * cell) / texW, cellY,
            (right - 1.5f * cell) / texW, cellY,
            // Marker strip with a little margin, as on the VR panel.
            (right - (kMarkerCells + 0.5f) * cell) / texW, (bottom - 1.2f * cell) / texH,
            (left + 2.5f) / texW, (top + 2.5f) / texH,
            cursorParked ? parkX / texW : 2.0f, cursorParked ? parkY / texH : 2.0f,
            cursorParked ? (parkX + mask) / texW : 2.0f, cursorParked ? (parkY + mask) / texH : 2.0f };
        float outputConstants[kConstantCount] = { 0.0f, 0.0f, 1.0f, 1.0f };
        if (panel)
        {
            // The world image spans the eye's symmetric frustum fitted to the image's aspect (the loader renders it
            // so); the interface keeps its aspect on the panel.
            float aspect = float(width) / float(height);
            float tanX, tanY;
            FitSymmetricFrustum(panel->tanHalfWidth, panel->tanHalfHeight, aspect, tanX, tanY);
            float* c = outputConstants + 16;
            c[0] = panel->eyeOrigin[0];
            c[1] = panel->eyeOrigin[1];
            c[2] = panel->eyeOrigin[2];
            c[3] = tanX;
            const float tail[3] = { tanY, panel->panelHalfWidth, panel->panelHalfWidth / aspect };
            for (int r = 0; r < 3; ++r)
            {
                for (int k = 0; k < 3; ++k)
                    c[4 + 4 * r + k] = panel->eyeToPanel[r][k];
                c[4 + 4 * r + 3] = tail[r];
            }
        }

        cmd->ChangeId(this);
        cmd->AFlushLock();
        ID3D12GraphicsCommandList* list = cmd->GetCommandListInterface();

        const UINT psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const UINT rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bool srcNeedsTransition = (in.srcTextureState & psr) == 0;
        if (srcNeedsTransition)
        {
            D3D12_RESOURCE_BARRIER b = Transition(in.pSrcTexture, in.srcTextureState, psr);
            list->ResourceBarrier(1, &b);
        }

        D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, LONG(width), LONG(height) };
        list->SetGraphicsRootSignature(m_rootSignature);
        list->SetDescriptorHeaps(1, &srv.pHeap);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);

        ID3D12ResourceDescAllocator* rtvs = root->GetRTV_DescAllocator(adapterID);
        for (int t = 0; t < kTargetCount; ++t)
        {
            ID3D12Resource* texture = m_targets[t].texture.Get();
            D3D12_RESOURCE_BARRIER b = Transition(texture, psr, rt);
            list->ResourceBarrier(1, &b);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescHandle(m_targets[t].rtvHandle);
            if (t == kInterface && !m_cleared)
            {
                const float transparent[4] = {};
                list->ClearRenderTargetView(rtv, transparent, 0, nullptr);
                m_cleared = true;
            }
            list->SetPipelineState(pso[t]);
            list->SetGraphicsRootDescriptorTable(0, t == kOutput ? outputTable : frameTable);
            list->SetGraphicsRoot32BitConstants(1, kConstantCount, t == kOutput ? outputConstants : frameConstants, 0);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            b = Transition(texture, rt, psr);
            list->ResourceBarrier(1, &b);
        }

        if (srcNeedsTransition)
        {
            D3D12_RESOURCE_BARRIER b = Transition(in.pSrcTexture, psr, in.srcTextureState);
            list->ResourceBarrier(1, &b);
        }
        cmd->AFlushUnlock();

        out.pOutputTexture = m_targets[kOutput].texture.Get();
        out.outputTexSRVCPUHandle = srvs->GetCPUDescHandle(m_targets[kOutput].srvHandle);
        out.outputTextureExpectedState = psr;
        return true;
    }
}
