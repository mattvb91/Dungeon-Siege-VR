#include "vr_overlay.h"

#include <dxgi1_2.h>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace dsvr
{
    namespace
    {
        const float kPi = 3.14159265f;

        const DWORD kVrRetryMs = 3000;

        bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
    }

    void VrOverlay::Start(FrameRing* ring, float screenWidth, float screenDistance)
    {
        m_ring = ring;
        m_screenWidth = screenWidth;
        m_screenDistance = screenDistance;
        InitializeCriticalSection(&m_luidLock);
        m_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, &VrOverlay::ThreadMain, this, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            Log("CreateThread failed (error %lu)", GetLastError());
    }

    void VrOverlay::SetAdapterLuid(LUID luid)
    {
        EnterCriticalSection(&m_luidLock);
        m_luid = luid;
        m_hasLuid = true;
        LeaveCriticalSection(&m_luidLock);
    }

    DWORD WINAPI VrOverlay::ThreadMain(void* self)
    {
        static_cast<VrOverlay*>(self)->Run();
    }

    void VrOverlay::Run()
    {
        Log("VR thread started");
        for (;;)
        {
            if (!m_vrReady && GetTickCount() >= m_nextVrAttempt)
            {
                if (!TryInitVr())
                    m_nextVrAttempt = GetTickCount() + kVrRetryMs;
            }

            WaitForSingleObject(m_ring->NewFrameEvent(), 100);

            if (m_vrReady)
                PumpVrEvents();

            FrameRing::Frame frame;
            if (!m_ring->TakeLatest(frame))
                continue;

            if (m_vrReady && EnsureD3D11() && EnsureOverlay())
                PresentFrame(frame);

            m_ring->EndRead();
            CloseHandle(frame.sharedHandle);
            frame.fence->Release();
        }
    }

    bool VrOverlay::TryInitVr()
    {
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, vr::VRApplication_Overlay);
        if (err != vr::VRInitError_None)
        {
            static vr::EVRInitError lastErr = vr::VRInitError_None;
            if (err != lastErr)
                Log("VR_Init failed: %s (is SteamVR running?)", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            lastErr = err;
            return false;
        }

        int32_t adapterIndex = -1;
        vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);
        Log("SteamVR initialised (HMD on DXGI adapter %d)", adapterIndex);
        AcquireSRWLockExclusive(&m_vrLock);
        m_vrReady = true;
        ReleaseSRWLockExclusive(&m_vrLock);
        return true;
    }

    bool VrOverlay::GetHeadPosition(float out[3])
    {
        // Rough render-to-photon latency of the game frame being set up.
        const float kPredictSeconds = 0.03f;

        bool ok = false;
        AcquireSRWLockShared(&m_vrLock);
        if (m_vrReady)
        {
            vr::TrackedDevicePose_t pose;
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, kPredictSeconds, &pose, 1);
            if (pose.bPoseIsValid)
            {
                out[0] = pose.mDeviceToAbsoluteTracking.m[0][3];
                out[1] = pose.mDeviceToAbsoluteTracking.m[1][3];
                out[2] = pose.mDeviceToAbsoluteTracking.m[2][3];
                ok = true;
            }
        }
        ReleaseSRWLockShared(&m_vrLock);
        return ok;
    }

    void VrOverlay::ShutdownVr()
    {
        if (!m_vrReady)
            return;
        if (m_overlay != vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlay()->DestroyOverlay(m_overlay);
            m_overlay = vr::k_ulOverlayHandleInvalid;
        }
        AcquireSRWLockExclusive(&m_vrLock);
        m_vrReady = false;
        vr::VR_Shutdown();
        ReleaseSRWLockExclusive(&m_vrLock);
        Log("SteamVR shut down");
    }

    void VrOverlay::PumpVrEvents()
    {
        vr::VREvent_t ev;
        while (m_vrReady && vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
        {
            if (ev.eventType == vr::VREvent_Quit)
            {
                Log("SteamVR is quitting");
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                ShutdownVr();
                m_nextVrAttempt = GetTickCount() + kVrRetryMs;
            }
        }
    }

    bool VrOverlay::EnsureD3D11()
    {
        EnterCriticalSection(&m_luidLock);
        bool hasLuid = m_hasLuid;
        LUID luid = m_luid;
        LeaveCriticalSection(&m_luidLock);

        if (!hasLuid)
            return false;
        if (m_device && SameLuid(luid, m_deviceLuid))
            return true;

        ReleaseD3D11();

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (SameLuid(desc.AdapterLuid, luid))
            {
                Log("D3D11 on DXGI adapter %u: %ls", i, desc.Description);
                break;
            }
            adapter.Reset();
        }
        if (!adapter)
        {
            Log("no DXGI adapter matches dgVoodoo's D3D12 device LUID");
            return false;
        }

        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &m_context);
        if (FAILED(hr) || FAILED(device.As(&m_device)))
        {
            Log("D3D11CreateDevice failed: 0x%08lX", hr);
            ReleaseD3D11();
            return false;
        }

        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        m_device->CreateQuery(&qd, &m_copyDone);
        m_deviceLuid = luid;
        return true;
    }

    void VrOverlay::ReleaseD3D11()
    {
        if (m_vrReady && m_overlay != vr::k_ulOverlayHandleInvalid)
            vr::VROverlay()->ClearOverlayTexture(m_overlay);
        for (auto& t : m_shared)
            t.Reset();
        for (auto& t : m_overlayTex)
            t.Reset();
        m_generation = ~0u;
        m_copyDone.Reset();
        m_context.Reset();
        m_device.Reset();
    }

    bool VrOverlay::EnsureOverlay()
    {
        if (m_overlay != vr::k_ulOverlayHandleInvalid)
            return true;

        vr::IVROverlay* overlay = vr::VROverlay();
        vr::EVROverlayError err = overlay->CreateOverlay("dsvr.cinema", "Dungeon Siege", &m_overlay);
        if (err != vr::VROverlayError_None)
        {
            Log("CreateOverlay failed: %s", overlay->GetOverlayErrorNameFromEnum(err));
            m_overlay = vr::k_ulOverlayHandleInvalid;
            return false;
        }

        vr::HmdMatrix34_t transform = { {
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, -m_screenDistance },
        } };
        overlay->SetOverlayWidthInMeters(m_overlay, m_screenWidth);
        overlay->SetOverlayCurvature(m_overlay, m_screenWidth / (2.0f * kPi * m_screenDistance));
        overlay->SetOverlayTransformAbsolute(m_overlay, vr::TrackingUniverseSeated, &transform);
        overlay->ShowOverlay(m_overlay);
        Log("overlay created: %.1f m wide at %.1f m", m_screenWidth, m_screenDistance);
        return true;
    }

    void VrOverlay::PresentFrame(FrameRing::Frame& frame)
    {
        // Wait until dgVoodoo's GPU work (including our copy into the slot) has finished.
        if (frame.fence->GetCompletedValue() < frame.fenceValue)
        {
            frame.fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent);
            if (WaitForSingleObject(m_fenceEvent, 250) != WAIT_OBJECT_0)
                return;
        }

        if (frame.generation != m_generation)
        {
            for (auto& t : m_shared)
                t.Reset();
            for (auto& t : m_overlayTex)
                t.Reset();

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = frame.texWidth;
            desc.Height = frame.texHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = frame.format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            for (auto& t : m_overlayTex)
            {
                HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &t);
                if (FAILED(hr))
                {
                    Log("CreateTexture2D(overlay %ux%u fmt %d) failed: 0x%08lX", desc.Width, desc.Height, desc.Format, hr);
                    return;
                }
            }
            m_generation = frame.generation;
        }

        ComPtr<ID3D11Texture2D>& shared = m_shared[frame.slot];
        if (!shared)
        {
            HRESULT hr = m_device->OpenSharedResource1(frame.sharedHandle, IID_PPV_ARGS(&shared));
            if (FAILED(hr))
            {
                if (m_openFailures++ < 3)
                    Log("OpenSharedResource1 failed: 0x%08lX", hr);
                return;
            }
        }

        ID3D11Texture2D* target = m_overlayTex[m_nextOverlayTex].Get();
        m_nextOverlayTex ^= 1;

        D3D11_BOX box = { 0, 0, 0, frame.width, frame.height, 1 };
        m_context->CopySubresourceRegion(target, 0, 0, 0, 0, shared.Get(), 0, &box);
        m_context->End(m_copyDone.Get());
        m_context->Flush();

        // The ring slot can be rewritten as soon as we return, so finish reading it first.
        DWORD start = GetTickCount();
        while (m_context->GetData(m_copyDone.Get(), nullptr, 0, 0) == S_FALSE && GetTickCount() - start < 50)
            SwitchToThread();

        vr::VRTextureBounds_t bounds = { 0.0f, 0.0f,
            float(frame.width) / float(frame.texWidth), float(frame.height) / float(frame.texHeight) };
        vr::Texture_t texture = { target, vr::TextureType_DirectX, vr::ColorSpace_Gamma };

        vr::IVROverlay* overlay = vr::VROverlay();
        overlay->SetOverlayTextureBounds(m_overlay, &bounds);
        vr::EVROverlayError err = overlay->SetOverlayTexture(m_overlay, &texture);
        if (err != vr::VROverlayError_None)
        {
            Log("SetOverlayTexture failed: %s", overlay->GetOverlayErrorNameFromEnum(err));
            return;
        }

        if (m_framesShown++ == 0)
            Log("first frame on overlay (%ux%u)", frame.width, frame.height);
    }
}
