// dsvr dgVoodoo add-on. dgVoodoo 2.87 only loads an add-on named SampleAddon.dll from the game folder,
// and only with a D3D12 output API. We observe its D3D12 backend, copy every presented game image into
// a shared texture ring, and let the VR thread put it on a SteamVR overlay. Presentation on the monitor
// is left untouched.

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include "Addon/AddonDefs.hpp"
#include "Addon/IAddonMainCallback.hpp"
#include "Addon/ID3D12RootObserver.hpp"

#include "common/log.h"
#include "frame_ring.h"
#include "vr_overlay.h"

namespace
{
    using namespace dsvr;

    // Our copy textures must have a concrete format so D3D11 and SteamVR can use them.
    DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:    return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:                               return format;
        }
    }

    class FrameCapture final : public dgVoodoo::ID3D12RootObserver
    {
    public:
        explicit FrameCapture(dgVoodoo::IAddonMainCallback* host) : m_host(host) {}

        bool Init()
        {
            if (!m_host->RegisterForCallback(IID_D3D12RootObserver, static_cast<dgVoodoo::ID3D12RootObserver*>(this)))
            {
                Log("RegisterForCallback(IID_D3D12RootObserver) failed");
                return false;
            }
            m_vr.Start(&m_ring);
            return true;
        }

        void Exit()
        {
            m_vr.Stop(2000);
            m_host->UnregisterForCallback(IID_D3D12RootObserver, static_cast<dgVoodoo::ID3D12RootObserver*>(this));
        }

        // --- ID3D12RootObserver ---

        bool D3D12RootCreated(HMODULE, ID3D12Root* root) override
        {
            Log("D3D12 root created");
            m_root = root;
            return true;
        }

        void D3D12RootReleased(const ID3D12Root*) override
        {
            Log("D3D12 root released");
            m_root = nullptr;
        }

        bool D3D12BeginUsingAdapter(UInt32 adapterID) override
        {
            if (m_adapterID != kNoAdapter)
            {
                Log("ignoring additional adapter %u (using %u)", adapterID, m_adapterID);
                return true;
            }
            ID3D12Device* device = m_root->GetDevice(adapterID);
            LUID luid = device->GetAdapterLuid();
            Log("using D3D12 adapter %u (LUID %08lX:%08lX)", adapterID, luid.HighPart, luid.LowPart);
            m_adapterID = adapterID;
            m_device = device;
            m_vr.SetAdapterLuid(luid);
            return true;
        }

        void D3D12EndUsingAdapter(UInt32 adapterID) override
        {
            if (adapterID != m_adapterID)
                return;
            ID3D12GraphicsCommandListAuto* cmd = m_root->GetGraphicsCommandListAuto(adapterID);
            cmd->AFlush(false);
            m_ring.ReleaseAll(cmd->AGetFence(), 1000);
            m_device = nullptr;
            m_adapterID = kNoAdapter;
            Log("released D3D12 adapter %u", adapterID);
        }

        bool D3D12CreateSwapchainHook(UInt32, IDXGIFactory1*, IUnknown*, const DXGI_SWAP_CHAIN_DESC&, IDXGISwapChain**) override
        {
            return false;  // let dgVoodoo create the swapchain
        }

        void D3D12SwapchainCreated(UInt32, ID3D12Swapchain*, const ID3D12Root::SwapchainData& data) override
        {
            Log("swapchain created: %ldx%ld (presented %ldx%ld), format %d",
                data.imageSize.cx, data.imageSize.cy, data.imagePresentationSize.cx, data.imagePresentationSize.cy, data.format);
        }

        void D3D12SwapchainChanged(UInt32, ID3D12Swapchain*, const ID3D12Root::SwapchainData& data) override
        {
            Log("swapchain changed: %ldx%ld (presented %ldx%ld), format %d",
                data.imageSize.cx, data.imageSize.cy, data.imagePresentationSize.cx, data.imagePresentationSize.cy, data.format);
        }

        void D3D12SwapchainReleased(UInt32, ID3D12Swapchain*) override
        {
            Log("swapchain released");
        }

        bool D3D12SwapchainPresentBegin(UInt32 adapterID, const PresentBeginContextInput& in, PresentBeginContextOutput& out) override
        {
            // We never replace dgVoodoo's presentation input; returning false keeps the monitor image as-is.
            out.pOutputTexture = nullptr;
            out.outputTexSRVCPUHandle.ptr = 0;
            out.outputTextureExpectedState = (UINT)-1;

            if (adapterID != m_adapterID || !in.pSrcTexture)
                return false;

            UINT width = UINT(in.srcRect.right - in.srcRect.left);
            UINT height = UINT(in.srcRect.bottom - in.srcRect.top);
            if (width == 0 || height == 0)
                return false;

            ID3D12GraphicsCommandListAuto* cmd = m_root->GetGraphicsCommandListAuto(adapterID);
            ID3D12Fence* fence = cmd->AGetFence();
            m_ring.CollectGarbage(fence);

            D3D12_RESOURCE_DESC srcDesc = in.pSrcTexture->GetDesc();
            if (!m_ring.EnsureTextures(m_device, width, height, TypedFormat(srcDesc.Format), fence, cmd->GetFenceValue()))
                return false;

            int slot = m_ring.AcquireWriteSlot();
            if (slot < 0)
                return false;

            cmd->ChangeId(this);
            cmd->AFlushLock();
            ID3D12GraphicsCommandList* list = cmd->GetCommandListInterface();

            const UINT copySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bool needTransition = (in.srcTextureState & copySource) == 0;
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = in.pSrcTexture;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (needTransition)
            {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATES(in.srcTextureState);
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &barrier);
            }

            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = m_ring.SlotTexture(slot);
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = in.pSrcTexture;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_BOX box = { UINT(in.srcRect.left), UINT(in.srcRect.top), 0, UINT(in.srcRect.right), UINT(in.srcRect.bottom), 1 };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

            if (needTransition)
            {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATES(in.srcTextureState);
                list->ResourceBarrier(1, &barrier);
            }

            cmd->AFlushUnlock();

            // Signalled once the commands recorded so far (including our copy) finish on the GPU.
            m_ring.Publish(slot, fence, cmd->GetFenceValue(), width, height);

            if (m_framesCaptured++ == 0)
                Log("first frame captured: %ux%u, source format %d, state 0x%X", width, height, srcDesc.Format, in.srcTextureState);
            return false;
        }

        void D3D12SwapchainPresentEnd(UInt32, const PresentEndContextInput&) override {}

    private:
        static const UInt32 kNoAdapter = 0xFFFFFFFF;

        dgVoodoo::IAddonMainCallback* m_host;
        ID3D12Root* m_root = nullptr;
        ID3D12Device* m_device = nullptr;
        UInt32 m_adapterID = kNoAdapter;
        unsigned m_framesCaptured = 0;

        FrameRing m_ring;
        VrOverlay m_vr;
    };

    HMODULE g_module = nullptr;
    FrameCapture* g_capture = nullptr;
}

extern "C" __declspec(dllexport) bool AddOnInit(dgVoodoo::IAddonMainCallback* host)
{
    UInt32 version = host->GetVersion();
    Log("AddOnInit (dgVoodoo version %u.%u%u)", (version >> 8) & 0xF, (version >> 4) & 0xF, version & 0xF);

    g_capture = new FrameCapture(host);
    if (!g_capture->Init())
    {
        delete g_capture;
        g_capture = nullptr;
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void AddOnExit()
{
    Log("AddOnExit");
    if (g_capture)
    {
        g_capture->Exit();
        delete g_capture;
        g_capture = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        dsvr::OpenLog(module, "dsvr_vr.log");
        Log("dsvr addon attach");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Log("dsvr addon detach");
        dsvr::CloseLog();
    }
    return TRUE;
}
