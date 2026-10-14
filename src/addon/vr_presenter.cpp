#include "vr_presenter.h"

#include <dxgi1_2.h>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace dsvr
{
    namespace
    {
        const float kPi = 3.14159265f;
        const DWORD kVrRetryMs = 3000;
        const DWORD kStatsIntervalMs = 10000;

        bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

        void CopyMatrix(float out[3][4], const vr::HmdMatrix34_t& m)
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r][c] = m.m[r][c];
        }
    }

    void VrPresenter::Start(FrameRing* ring, const PresenterSettings& settings)
    {
        m_ring = ring;
        m_settings = settings;
        InitializeCriticalSection(&m_luidLock);
        m_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        Log("VR presenter: %s mode", settings.stereo ? "stereo (scene)" : "mono (overlay)");
        HANDLE thread = CreateThread(nullptr, 0, &VrPresenter::ThreadMain, this, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            Log("CreateThread failed (error %lu)", GetLastError());
    }

    void VrPresenter::SetAdapterLuid(LUID luid)
    {
        EnterCriticalSection(&m_luidLock);
        m_luid = luid;
        m_hasLuid = true;
        LeaveCriticalSection(&m_luidLock);
    }

    DWORD WINAPI VrPresenter::ThreadMain(void* self)
    {
        static_cast<VrPresenter*>(self)->Run();
    }

    void VrPresenter::Run()
    {
        Log("VR thread started");
        for (;;)
        {
            if (!m_vrReady && GetTickCount() >= m_nextVrAttempt)
            {
                if (!TryInitVr())
                    m_nextVrAttempt = GetTickCount() + kVrRetryMs;
            }

            if (m_vrReady && m_settings.stereo)
            {
                // Paces this loop to the headset's refresh and marks the start of a compositor frame.
                vr::EVRCompositorError err = vr::VRCompositor()->WaitGetPoses(nullptr, 0, nullptr, 0);
                if (err != vr::VRCompositorError_None)
                    WaitForSingleObject(m_ring->NewFrameEvent(), 10);
            }
            else
            {
                WaitForSingleObject(m_ring->NewFrameEvent(), 100);
            }

            if (m_vrReady)
                PumpVrEvents();

            FrameRing::Frame frame;
            while (m_ring->TakeOldest(frame))
            {
                if (m_vrReady && EnsureD3D11())
                {
                    if (m_settings.stereo)
                        ConsumeStereo(frame);
                    else if (EnsureOverlay())
                        PresentMono(frame);
                }
                m_ring->EndRead(frame);
                CloseHandle(frame.sharedHandle);
                frame.fence->Release();
            }

            if (m_vrReady && m_settings.stereo)
                SubmitStereo();

            if (m_settings.stereo && GetTickCount() - m_lastStatsLog >= kStatsIntervalMs && (m_stereoFrames || m_monoFrames))
            {
                Log("stereo stats: %u stereo, %u mono, %u without pose, %u dropped frames",
                    m_stereoFrames, m_monoFrames, m_staleFrames, m_ring->DroppedFrames());
                m_lastStatsLog = GetTickCount();
            }
        }
    }

    bool VrPresenter::TryInitVr()
    {
        vr::EVRInitError err = vr::VRInitError_None;
        vr::VR_Init(&err, m_settings.stereo ? vr::VRApplication_Scene : vr::VRApplication_Overlay);
        if (err != vr::VRInitError_None)
        {
            static vr::EVRInitError lastErr = vr::VRInitError_None;
            if (err != lastErr)
                Log("VR_Init failed: %s (is SteamVR running?)", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            lastErr = err;
            return false;
        }

        vr::IVRSystem* system = vr::VRSystem();
        int32_t adapterIndex = -1;
        system->GetDXGIOutputInfo(&adapterIndex);
        Log("SteamVR initialised (HMD on DXGI adapter %d)", adapterIndex);

        if (m_settings.stereo)
        {
            vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);
            float hz = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
            m_frameSeconds = hz > 0.0f ? 1.0f / hz : 1.0f / 90.0f;
            m_vsyncToPhotons = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);
            uint32_t rw = 0, rh = 0;
            system->GetRecommendedRenderTargetSize(&rw, &rh);
            Log("HMD: %.1f Hz, vsync->photons %.4f s, recommended %ux%u per eye", hz, m_vsyncToPhotons, rw, rh);
            for (int eye = 0; eye < 2; ++eye)
            {
                float l, r, t, b;
                system->GetProjectionRaw(vr::EVREye(eye), &l, &r, &t, &b);
                vr::HmdMatrix34_t e = system->GetEyeToHeadTransform(vr::EVREye(eye));
                Log("  eye %d: raw l %.3f r %.3f t %.3f b %.3f, eye->head offset %.4f %.4f %.4f",
                    eye, l, r, t, b, e.m[0][3], e.m[1][3], e.m[2][3]);
            }
        }

        AcquireSRWLockExclusive(&m_vrLock);
        m_vrReady = true;
        ReleaseSRWLockExclusive(&m_vrLock);
        return true;
    }

    void VrPresenter::ShutdownVr()
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

        AcquireSRWLockExclusive(&m_viewLock);
        for (IssuedView& v : m_views)
            v.valid = false;
        ReleaseSRWLockExclusive(&m_viewLock);
        m_eyeHasFrame[0] = m_eyeHasFrame[1] = false;
        Log("SteamVR shut down");
    }

    void VrPresenter::PumpVrEvents()
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

    bool VrPresenter::GetHeadPosition(float out[3])
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

    bool VrPresenter::GetStereoView(unsigned frameId, StereoView* out)
    {
        IssuedView& slot = m_views[frameId % kMarkerFrameIds];

        AcquireSRWLockShared(&m_viewLock);
        bool cached = slot.valid && slot.frameId == frameId;
        if (cached)
            *out = slot.view;
        ReleaseSRWLockShared(&m_viewLock);
        if (cached)
            return true;

        bool ok = false;
        StereoView view = {};
        AcquireSRWLockShared(&m_vrLock);
        if (m_vrReady && m_settings.stereo)
        {
            vr::IVRSystem* system = vr::VRSystem();
            view.eye = int(frameId & 1);

            // This frame is captured, submitted after the next WaitGetPoses, then scanned out.
            float sinceVsync = 0.0f;
            uint64_t vsyncCounter = 0;
            system->GetTimeSinceLastVsync(&sinceVsync, &vsyncCounter);
            float predict = (m_frameSeconds - sinceVsync) + m_frameSeconds + m_vsyncToPhotons;

            vr::TrackedDevicePose_t pose;
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, predict, &pose, 1);
            if (pose.bPoseIsValid)
            {
                CopyMatrix(view.headPose, pose.mDeviceToAbsoluteTracking);
                CopyMatrix(view.eyeToHead, system->GetEyeToHeadTransform(vr::EVREye(view.eye)));
                float l, r, t, b;
                system->GetProjectionRaw(vr::EVREye(view.eye), &l, &r, &t, &b);
                // Valve's own projection math maps pfBottom to the top of the image (ndc +y), pfTop to the bottom.
                view.tanLeft = l;
                view.tanRight = r;
                view.tanUp = b;
                view.tanDown = t;
                ok = true;
            }
        }
        ReleaseSRWLockShared(&m_vrLock);

        if (ok)
        {
            AcquireSRWLockExclusive(&m_viewLock);
            slot.valid = true;
            slot.frameId = frameId;
            slot.view = view;
            ReleaseSRWLockExclusive(&m_viewLock);
            *out = view;
        }
        return ok;
    }

    bool VrPresenter::EnsureD3D11()
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

    void VrPresenter::ReleaseD3D11()
    {
        if (m_vrReady && m_overlay != vr::k_ulOverlayHandleInvalid)
            vr::VROverlay()->ClearOverlayTexture(m_overlay);
        for (auto& t : m_shared)
            t.Reset();
        for (auto& t : m_overlayTex)
            t.Reset();
        for (auto& t : m_eyeTex)
            t.Reset();
        m_eyeHasFrame[0] = m_eyeHasFrame[1] = false;
        m_markerStaging.Reset();
        m_generation = ~0u;
        m_copyDone.Reset();
        m_context.Reset();
        m_device.Reset();
    }

    bool VrPresenter::EnsureOverlay()
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

        float width = m_settings.screenWidth;
        float distance = m_settings.screenDistance;
        vr::HmdMatrix34_t transform = { {
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, -distance },
        } };
        overlay->SetOverlayWidthInMeters(m_overlay, width);
        overlay->SetOverlayCurvature(m_overlay, width / (2.0f * kPi * distance));
        overlay->SetOverlayTransformAbsolute(m_overlay, vr::TrackingUniverseSeated, &transform);
        overlay->ShowOverlay(m_overlay);
        Log("overlay created: %.1f m wide at %.1f m", width, distance);
        return true;
    }

    bool VrPresenter::EnsureTextures(const FrameRing::Frame& frame)
    {
        if (frame.generation == m_generation)
            return true;

        for (auto& t : m_shared)
            t.Reset();
        for (auto& t : m_overlayTex)
            t.Reset();
        for (auto& t : m_eyeTex)
            t.Reset();
        m_eyeHasFrame[0] = m_eyeHasFrame[1] = false;
        m_markerStaging.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = frame.texWidth;
        desc.Height = frame.texHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = frame.format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D>* targets = m_settings.stereo ? m_eyeTex : m_overlayTex;
        for (int i = 0; i < 2; ++i)
        {
            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &targets[i]);
            if (FAILED(hr))
            {
                Log("CreateTexture2D(%ux%u fmt %d) failed: 0x%08lX", desc.Width, desc.Height, desc.Format, hr);
                return false;
            }
        }

        if (m_settings.stereo)
        {
            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Width = 1;
            staging.Height = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(m_device->CreateTexture2D(&staging, nullptr, &m_markerStaging)))
                return false;
        }

        m_generation = frame.generation;
        return true;
    }

    ID3D11Texture2D* VrPresenter::OpenFrame(FrameRing::Frame& frame)
    {
        // Wait until dgVoodoo's GPU work (including our copy into the slot) has finished.
        if (frame.fence->GetCompletedValue() < frame.fenceValue)
        {
            frame.fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent);
            if (WaitForSingleObject(m_fenceEvent, 250) != WAIT_OBJECT_0)
                return nullptr;
        }

        if (!EnsureTextures(frame))
            return nullptr;

        ComPtr<ID3D11Texture2D>& shared = m_shared[frame.slot];
        if (!shared)
        {
            HRESULT hr = m_device->OpenSharedResource1(frame.sharedHandle, IID_PPV_ARGS(&shared));
            if (FAILED(hr))
            {
                if (m_openFailures++ < 3)
                    Log("OpenSharedResource1 failed: 0x%08lX", hr);
                return nullptr;
            }
        }
        return shared.Get();
    }

    void VrPresenter::PresentMono(FrameRing::Frame& frame)
    {
        ID3D11Texture2D* shared = OpenFrame(frame);
        if (!shared)
            return;

        ID3D11Texture2D* target = m_overlayTex[m_nextOverlayTex].Get();
        m_nextOverlayTex ^= 1;

        D3D11_BOX box = { 0, 0, 0, frame.width, frame.height, 1 };
        m_context->CopySubresourceRegion(target, 0, 0, 0, 0, shared, 0, &box);
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

    bool VrPresenter::ReadMarker(ID3D11Texture2D* source, const FrameRing::Frame& frame, unsigned& frameId, int& eye)
    {
        if (!m_markerStaging || frame.width < kMarkerSize || frame.height < kMarkerSize)
            return false;

        D3D11_BOX box = { frame.width - 1, frame.height - 1, 0, frame.width, frame.height, 1 };
        m_context->CopySubresourceRegion(m_markerStaging.Get(), 0, 0, 0, 0, source, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_markerStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        const BYTE* p = static_cast<const BYTE*>(mapped.pData);
        unsigned r, g, b;
        bool known = true;
        switch (frame.format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            r = p[0]; g = p[1]; b = p[2];
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            r = p[2]; g = p[1]; b = p[0];
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        {
            UINT32 v = *reinterpret_cast<const UINT32*>(p);
            r = (v >> 2) & 0xFF; g = (v >> 12) & 0xFF; b = (v >> 22) & 0xFF;
            break;
        }
        default:
            known = false;
            r = g = b = 0;
            break;
        }
        m_context->Unmap(m_markerStaging.Get(), 0);
        return known && DecodeMarker(r, g, b, frameId, eye);
    }

    void VrPresenter::ConsumeStereo(FrameRing::Frame& frame)
    {
        ID3D11Texture2D* shared = OpenFrame(frame);
        if (!shared)
            return;

        D3D11_BOX box = { 0, 0, 0, frame.width, frame.height, 1 };
        vr::VRTextureBounds_t bounds = { 0.0f, 0.0f,
            float(frame.width) / float(frame.texWidth), float(frame.height) / float(frame.texHeight) };

        unsigned frameId;
        int eye;
        if (ReadMarker(shared, frame, frameId, eye))
        {
            bool found = false;
            AcquireSRWLockShared(&m_viewLock);
            const IssuedView& issued = m_views[frameId % kMarkerFrameIds];
            if (issued.valid && issued.view.eye == eye)
            {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        m_eyePose[eye].m[r][c] = issued.view.headPose[r][c];
                found = true;
            }
            ReleaseSRWLockShared(&m_viewLock);

            m_context->CopySubresourceRegion(m_eyeTex[eye].Get(), 0, 0, 0, 0, shared, 0, &box);
            m_eyeHasFrame[eye] = true;
            m_eyeHasPose[eye] = found;
            m_eyeBounds[eye] = bounds;
            if (!found)
                ++m_staleFrames;
            if (m_stereoFrames++ == 0)
                Log("first stereo frame: eye %d, frame id %u, %ux%u", eye, frameId, frame.width, frame.height);
        }
        else
        {
            // Not rendered by the stereo camera (menus, loading screens): show it head-locked in both eyes.
            for (int e = 0; e < 2; ++e)
            {
                m_context->CopySubresourceRegion(m_eyeTex[e].Get(), 0, 0, 0, 0, shared, 0, &box);
                m_eyeHasFrame[e] = true;
                m_eyeHasPose[e] = false;
                m_eyeBounds[e] = bounds;
            }
            ++m_monoFrames;
        }

        // The ring slot can be rewritten as soon as we return, so finish reading it first.
        m_context->End(m_copyDone.Get());
        m_context->Flush();
        DWORD start = GetTickCount();
        while (m_context->GetData(m_copyDone.Get(), nullptr, 0, 0) == S_FALSE && GetTickCount() - start < 50)
            SwitchToThread();
    }

    void VrPresenter::SubmitStereo()
    {
        if (!m_eyeHasFrame[0] && !m_eyeHasFrame[1])
            return;

        for (int e = 0; e < 2; ++e)
        {
            // Until both eyes have had a frame, show the other eye's image.
            int src = m_eyeHasFrame[e] ? e : 1 - e;

            vr::VRTextureWithPose_t texture = {};
            texture.handle = m_eyeTex[src].Get();
            texture.eType = vr::TextureType_DirectX;
            texture.eColorSpace = vr::ColorSpace_Gamma;
            vr::EVRSubmitFlags flags = vr::Submit_Default;
            if (m_eyeHasPose[src])
            {
                texture.mDeviceToAbsoluteTracking = m_eyePose[src];
                flags = vr::Submit_TextureWithPose;
            }

            vr::EVRCompositorError err = vr::VRCompositor()->Submit(vr::EVREye(e), &texture, &m_eyeBounds[src], flags);
            static unsigned submitErrors = 0;
            if (err != vr::VRCompositorError_None && submitErrors++ < 5)
                Log("Submit(eye %d) failed: %d", e, err);
        }
    }
}
