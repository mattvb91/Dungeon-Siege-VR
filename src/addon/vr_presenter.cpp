#include "vr_presenter.h"

#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "common/log.h"

using Microsoft::WRL::ComPtr;

namespace dsvr
{
    HMODULE g_dumpModule = nullptr;

    namespace
    {
        const float kPi = 3.14159265f;
        const DWORD kVrRetryMs = 3000;
        // Enough for the marker strip up to 8192-wide images (4 cells of width/256).
        const UINT kMarkerStagingWidth = 160;
        const UINT kMarkerStagingHeight = 40;
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
                vr::TrackedDevicePose_t hmd;
                vr::EVRCompositorError err = vr::VRCompositor()->WaitGetPoses(&hmd, 1, nullptr, 0);
                if (err != vr::VRCompositorError_None)
                {
                    ++m_waitErrors;
                    m_lastWaitError = int(err);
                    WaitForSingleObject(m_ring->NewFrameEvent(), 10);
                }
                else if (hmd.bPoseIsValid)
                    UpdatePanelFollow(hmd.mDeviceToAbsoluteTracking);
                UpdatePointerDot();
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
                if (m_vrReady)
                {
                    // Diagnostic: does SteamVR consider us the scene application?
                    vr::IVRCompositor* compositor = vr::VRCompositor();
                    Log("compositor: scene focus pid %u (we are %lu), last renderer pid %u, can render %d, "
                        "WaitGetPoses errors %u (last %d)", compositor->GetCurrentSceneFocusProcess(), GetCurrentProcessId(),
                        compositor->GetLastFrameRenderer(), compositor->CanRenderScene() ? 1 : 0, m_waitErrors, m_lastWaitError);
                    m_waitErrors = 0;
                }
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

                // Each eye renders a symmetric frustum covering its asymmetric one (fitted to the render
                // target's aspect, see FitSymmetricFrustum) and SubmitStereo crops back to the real one.
                // Valve's projection math maps pfBottom to the top of the image, pfTop to the bottom.
                m_rawTan[eye][0] = l;
                m_rawTan[eye][1] = r;
                m_rawTan[eye][2] = b;  // up
                m_rawTan[eye][3] = t;  // down
                m_symTan[eye][0] = std::fmax(std::fabs(l), std::fabs(r));
                m_symTan[eye][1] = std::fmax(std::fabs(b), std::fabs(t));
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
        if (m_hud != vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlay()->DestroyOverlay(m_hud);
            m_hud = vr::k_ulOverlayHandleInvalid;
            m_hudVisible = false;
        }
        if (m_dot != vr::k_ulOverlayHandleInvalid)
        {
            vr::VROverlay()->DestroyOverlay(m_dot);
            m_dot = vr::k_ulOverlayHandleInvalid;
            m_dotVisible = false;
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
        m_showingMenu = false;
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

    bool VrPresenter::GetStereoView(unsigned frameId, int eye, StereoView* out)
    {
        IssuedView& slot = m_views[frameId % kMarkerFrameIds];

        AcquireSRWLockShared(&m_viewLock);
        bool cached = slot.valid && slot.frameId == frameId && slot.view.eye == eye;
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
            view.eye = eye & 1;

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
                // Symmetric frustum; SubmitStereo crops to the real one (see TryInitVr).
                view.tanLeft = -m_symTan[view.eye][0];
                view.tanRight = m_symTan[view.eye][0];
                view.tanUp = m_symTan[view.eye][1];
                view.tanDown = -m_symTan[view.eye][1];
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
        m_blackTex.Reset();
        for (auto& v : m_sharedSrv)
            v.Reset();
        for (auto& t : m_hudTex)
            t.Reset();
        for (auto& v : m_hudRtv)
            v.Reset();
        m_keyVs.Reset();
        m_keyPs.Reset();
        m_pointSampler.Reset();
        m_keyConstants.Reset();
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
        for (auto& v : m_sharedSrv)
            v.Reset();
        for (auto& t : m_hudTex)
            t.Reset();
        for (auto& v : m_hudRtv)
            v.Reset();
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

        // Overlay textures are needed in both modes: stereo shows menus on the overlay screen too.
        for (int i = 0; i < 2; ++i)
        {
            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_overlayTex[i]);
            if (SUCCEEDED(hr) && m_settings.stereo)
                hr = m_device->CreateTexture2D(&desc, nullptr, &m_eyeTex[i]);
            if (FAILED(hr))
            {
                Log("CreateTexture2D(%ux%u fmt %d) failed: 0x%08lX", desc.Width, desc.Height, desc.Format, hr);
                return false;
            }
        }

        if (m_settings.stereo)
        {
            // Interface panel: RGBA with alpha from the key colour.
            D3D11_TEXTURE2D_DESC hud = desc;
            hud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            hud.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            for (int i = 0; i < 2; ++i)
            {
                if (FAILED(m_device->CreateTexture2D(&hud, nullptr, &m_hudTex[i])) ||
                    FAILED(m_device->CreateRenderTargetView(m_hudTex[i].Get(), nullptr, &m_hudRtv[i])))
                {
                    Log("interface texture creation failed");
                    return false;
                }
            }

            // Shown in both eyes behind the menu screen.
            if (!m_blackTex)
            {
                D3D11_TEXTURE2D_DESC black = desc;
                black.Width = black.Height = 16;
                static const UINT32 zeros[16 * 16] = {};
                D3D11_SUBRESOURCE_DATA init = { zeros, 16 * sizeof(UINT32), 0 };
                if (FAILED(m_device->CreateTexture2D(&black, &init, &m_blackTex)))
                    return false;
            }

            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Width = kMarkerStagingWidth;
            staging.Height = kMarkerStagingHeight;
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
        // Cells are 1/256 of the image width (see stereo_abi.h); copy the bottom-right strip holding them.
        float cell = float(frame.width) / float(kMarkerCellDivisor);
        UINT stripW = UINT(std::ceil(cell * kMarkerCells));
        UINT stripH = UINT(std::ceil(cell));
        if (!m_markerStaging || stripW > kMarkerStagingWidth || stripH > kMarkerStagingHeight ||
            frame.width < stripW || frame.height < stripH || cell < 2.0f)
            return false;

        D3D11_BOX box = { frame.width - stripW, frame.height - stripH, 0, frame.width, frame.height, 1 };
        m_context->CopySubresourceRegion(m_markerStaging.Get(), 0, 0, 0, 0, source, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_markerStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;

        unsigned rgb[kMarkerCells][3] = {};
        bool known = true;
        UINT y = UINT(float(stripH) - 0.5f * cell);
        for (unsigned c = 0; c < kMarkerCells && known; ++c)
        {
            UINT x = UINT(float(stripW) - (float(c) + 0.5f) * cell);
            const BYTE* p = static_cast<const BYTE*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
            switch (frame.format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                rgb[c][0] = p[0]; rgb[c][1] = p[1]; rgb[c][2] = p[2];
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
                rgb[c][0] = p[2]; rgb[c][1] = p[1]; rgb[c][2] = p[0];
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            {
                UINT32 v = *reinterpret_cast<const UINT32*>(p);
                rgb[c][0] = (v >> 2) & 0xFF; rgb[c][1] = (v >> 12) & 0xFF; rgb[c][2] = (v >> 22) & 0xFF;
                break;
            }
            default:
                known = false;
                break;
            }
        }
        m_context->Unmap(m_markerStaging.Get(), 0);
        return known && DecodeMarker(rgb, frameId, eye);
    }

    void VrPresenter::ConsumeStereo(FrameRing::Frame& frame)
    {
        ID3D11Texture2D* shared = OpenFrame(frame);
        if (!shared)
            return;

        D3D11_BOX box = { 0, 0, 0, frame.width, frame.height, 1 };

        unsigned frameId;
        int eye;
        bool marked = ReadMarker(shared, frame, frameId, eye);
        if (marked && eye == kFrameInterface)
        {
            PresentInterface(frame, shared);
            if (m_showingMenu)
            {
                vr::VROverlay()->HideOverlay(m_overlay);
                m_showingMenu = false;
            }
        }
        else if (marked)
        {
            // The image fills frame.width x frame.height of the texture and spans the fitted symmetric
            // frustum the loader rendered; crop that to the eye's real frustum.
            float th, tv;
            FitSymmetricFrustum(m_symTan[eye][0], m_symTan[eye][1], float(frame.width) / float(frame.height), th, tv);
            const float* raw = m_rawTan[eye];
            float su = float(frame.width) / float(frame.texWidth);
            float sv = float(frame.height) / float(frame.texHeight);
            vr::VRTextureBounds_t bounds = {
                su * (raw[0] + th) / (2.0f * th), sv * (tv - raw[2]) / (2.0f * tv),
                su * (raw[1] + th) / (2.0f * th), sv * (tv - raw[3]) / (2.0f * tv) };
            if (m_stereoFrames == 0)
                Log("eye %d: rendered +-%.3f x +-%.3f, crop u %.3f..%.3f v %.3f..%.3f (texture fraction)", eye, th, tv,
                    bounds.uMin, bounds.uMax, bounds.vMin, bounds.vMax);

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

            if (m_stereoFrames == 400)
                DumpFrame(shared, frame, "dsvr_eye.bmp");
            m_context->CopySubresourceRegion(m_eyeTex[eye].Get(), 0, 0, 0, 0, shared, 0, &box);
            m_eyeHasFrame[eye] = true;
            m_eyeHasPose[eye] = found;
            m_eyeBounds[eye] = bounds;
            if (!found)
                ++m_staleFrames;
            if (m_stereoFrames++ == 0)
                Log("first stereo frame: eye %d, frame id %u, %ux%u", eye, frameId, frame.width, frame.height);
            if (m_showingMenu)
            {
                vr::VROverlay()->HideOverlay(m_overlay);
                m_showingMenu = false;
            }
        }
        else
        {
            // Not rendered by the stereo camera (menus, loading screens): a flat image can't be fused if it
            // fills each eye's (differently aimed) field of view, so show it on the world-fixed screen instead.
            ++m_monoFrames;
            SetHudVisible(false);
            if (EnsureOverlay())
            {
                if (!m_showingMenu)
                {
                    vr::VROverlay()->ShowOverlay(m_overlay);
                    m_showingMenu = true;
                }
                PresentMono(frame);
            }
            return;  // PresentMono already waited for its copy
        }

        // The ring slot can be rewritten as soon as we return, so finish reading it first.
        m_context->End(m_copyDone.Get());
        m_context->Flush();
        DWORD start = GetTickCount();
        while (m_context->GetData(m_copyDone.Get(), nullptr, 0, 0) == S_FALSE && GetTickCount() - start < 50)
            SwitchToThread();
    }

    // Diagnostic: writes the captured image (downsampled to at most ~1024 wide) as a 24-bit BMP next to the DLL.
    void VrPresenter::DumpFrame(ID3D11Texture2D* source, const FrameRing::Frame& frame, const char* fileName)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = frame.width;
        desc.Height = frame.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = frame.format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &staging)))
            return;
        D3D11_BOX box = { 0, 0, 0, frame.width, frame.height, 1 };
        m_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return;

        bool bgra = frame.format == DXGI_FORMAT_B8G8R8A8_UNORM || frame.format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                    frame.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        UINT step = (frame.width + 1023) / 1024;
        UINT w = frame.width / step, h = frame.height / step;
        UINT rowBytes = (w * 3 + 3) & ~3u;

        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(g_dumpModule, path, MAX_PATH);
        while (len > 0 && path[len - 1] != '\\')
            --len;
        path[len] = '\0';
        strcat_s(path, fileName);

        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") == 0 && file)
        {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + rowBytes * h;
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(w);
            ih.biHeight = LONG(h);  // bottom-up
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            fwrite(&fh, sizeof(fh), 1, file);
            fwrite(&ih, sizeof(ih), 1, file);
            std::vector<BYTE> row(rowBytes);
            for (UINT y = 0; y < h; ++y)
            {
                const BYTE* src = static_cast<const BYTE*>(mapped.pData) + size_t(h - 1 - y) * step * mapped.RowPitch;
                for (UINT x = 0; x < w; ++x)
                {
                    const BYTE* p = src + size_t(x) * step * 4;
                    BYTE r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
                    row[x * 3 + 0] = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                }
                fwrite(row.data(), 1, rowBytes, file);
            }
            fclose(file);
            Log("diagnostic: wrote %s (%ux%u from %ux%u)", fileName, w, h, frame.width, frame.height);
        }
        m_context->Unmap(staging.Get(), 0);
    }

    bool VrPresenter::EnsureHudPipeline()
    {
        if (m_keyPs)
            return true;

        // Full-screen triangle; pixels matching the interface key colour (magenta, see loader/ui_layer.h)
        // become transparent. The bottom-right corner holds the frame marker, so it is cleared as well.
        static const char kShader[] =
            "Texture2D src : register(t0);\n"
            "SamplerState smp : register(s0);\n"
            "cbuffer C : register(b0) { float2 uvScale; float2 markerStart; float2 keyUV; float2 pad; float4 parkRect; };\n"
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) {\n"
            "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
            "  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t * uvScale; return o; }\n"
            "float4 ps(V i) : SV_Target {\n"
            "  if (i.uv.x >= markerStart.x && i.uv.y >= markerStart.y) return 0;\n"
            // Parked game cursor (VR pointer off the panel), see kParkX in stereo_abi.h.
            "  if (all(i.uv >= parkRect.xy) && all(i.uv < parkRect.zw)) return 0;\n"
            "  float3 c = src.Sample(smp, i.uv).rgb;\n"
            // The key arrives through the game's gamma ramp (255,0,255 -> 228,0,228); measure its level from
            // the top-left pixel when that is background.
            "  float3 k = src.Sample(smp, keyUV).rgb;\n"
            "  float key = (k.g < 0.12 && k.r > 0.5 && k.b > 0.5) ? 0.5 * (k.r + k.b) : 0.894;\n"
            // Translucent UI pixels were blended over the key by the game: c = a*ui + (1-a)*(key,0,key).
            // The key has no green, so the 'excess magenta' min(r,b) - g measures (1-a); un-blend to remove
            // pink fringes. (Genuinely purple UI comes out partly transparent.)
            "  float spill = saturate((min(c.r, c.b) - c.g) / key);\n"
            "  float a = 1.0 - spill;\n"
            "  if (a < 0.04) return 0;\n"
            "  float3 ui = saturate((c - spill * float3(key, 0, key)) / a);\n"
            "  return float4(ui, a); }\n";

        ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_key", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &errors);
        if (SUCCEEDED(hr))
            hr = D3DCompile(kShader, sizeof(kShader) - 1, "dsvr_key", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &errors);
        if (FAILED(hr))
        {
            Log("interface shader compile failed: 0x%08lX %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_keyVs)) ||
            FAILED(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_keyPs)))
            return false;

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 48;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateSamplerState(&sd, &m_pointSampler)) || FAILED(m_device->CreateBuffer(&bd, nullptr, &m_keyConstants)))
        {
            m_keyPs.Reset();
            return false;
        }
        return true;
    }

    bool VrPresenter::EnsureHudOverlay()
    {
        if (m_hud != vr::k_ulOverlayHandleInvalid)
            return true;
        vr::IVROverlay* overlay = vr::VROverlay();
        vr::EVROverlayError err = overlay->CreateOverlay("dsvr.interface", "Dungeon Siege interface", &m_hud);
        if (err != vr::VROverlayError_None)
        {
            Log("CreateOverlay(interface) failed: %s", overlay->GetOverlayErrorNameFromEnum(err));
            m_hud = vr::k_ulOverlayHandleInvalid;
            return false;
        }
        overlay->SetOverlayWidthInMeters(m_hud, m_settings.hudWidth);
        PlacePanel();
        Log("interface panel created: %.2f m wide at %.2f m, follows gaze beyond %.0f deg",
            m_settings.hudWidth, m_settings.hudDistance, m_settings.hudFollowDegrees);
        return true;
    }

    // The panel sits in seated space around the seated origin (a head-locked panel was hard to read: user
    // feedback), but drifts after the player's gaze once it is more than hudFollowDegrees away, so the cursor
    // (which lives on the panel) can reach whatever the player looks at. The loader aims clicks from the
    // seated origin through the panel's current direction (DsvrGetPanelOrientation).
    void VrPresenter::UpdatePanelFollow(const vr::HmdMatrix34_t& head)
    {
        const float kDegToRad = 0.0174532925f;
        if (m_settings.hudFollowDegrees <= 0.0f)
            return;

        DWORD now = GetTickCount();
        float dt = m_lastFollowTick ? float(now - m_lastFollowTick) * 0.001f : 0.0f;
        m_lastFollowTick = now;

        // Gaze direction: -z column of the head rotation (OpenVR, right-handed, seated space).
        float fx = -head.m[0][2], fy = -head.m[1][2], fz = -head.m[2][2];
        float headYaw = std::atan2(-fx, -fz);
        float headPitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fy)));

        float yaw = m_panelYaw, pitch = m_panelPitch;
        float dYaw = std::remainder(headYaw - yaw, 2.0f * kPi);
        float dPitch = headPitch - pitch;
        float start = m_settings.hudFollowDegrees * kDegToRad;
        if (std::fabs(dYaw) > start || std::fabs(dPitch) > start)
            m_panelFollowing = true;
        if (m_panelFollowing)
        {
            float step = std::fmin(1.0f, dt * 4.0f);  // ease in over ~1/4 s
            yaw += dYaw * step;
            pitch += dPitch * step;
            pitch = std::fmax(-50.0f * kDegToRad, std::fmin(40.0f * kDegToRad, pitch));
            if (std::fabs(dYaw) < 2.0f * kDegToRad && std::fabs(dPitch) < 2.0f * kDegToRad)
                m_panelFollowing = false;
            AcquireSRWLockExclusive(&m_panelLock);
            m_panelYaw = yaw;
            m_panelPitch = pitch;
            ReleaseSRWLockExclusive(&m_panelLock);
            PlacePanel();
        }
    }

    void VrPresenter::PlacePanel()
    {
        if (m_hud == vr::k_ulOverlayHandleInvalid)
            return;
        float yaw, pitch;
        GetPanelOrientation(&yaw, &pitch);
        // Rotation Ry(yaw) * Rx(pitch); panel centre at distance d along its -z axis.
        float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
        float d = m_settings.hudDistance;
        vr::HmdMatrix34_t t = { {
            { cy, sy * sp, sy * cp, -d * sy * cp },
            { 0.0f, cp, -sp, d * sp },
            { -sy, cy * sp, cy * cp, -d * cy * cp },
        } };
        vr::VROverlay()->SetOverlayTransformAbsolute(m_hud, vr::TrackingUniverseSeated, &t);
    }

    void VrPresenter::SetPointer(float yaw, float pitch, bool visible)
    {
        AcquireSRWLockExclusive(&m_pointerLock);
        m_pointerYaw = yaw;
        m_pointerPitch = pitch;
        m_pointerVisible = visible;
        m_pointerTick = GetTickCount();
        ReleaseSRWLockExclusive(&m_pointerLock);
    }

    // Draws the VR pointer as a small ring in seated space, slightly in front of the panel distance.
    void VrPresenter::UpdatePointerDot()
    {
        AcquireSRWLockShared(&m_pointerLock);
        float yaw = m_pointerYaw, pitch = m_pointerPitch;
        bool visible = m_pointerVisible && GetTickCount() - m_pointerTick < 250 && !m_showingMenu;
        ReleaseSRWLockShared(&m_pointerLock);

        vr::IVROverlay* overlay = vr::VROverlay();
        if (visible && m_dot == vr::k_ulOverlayHandleInvalid && m_device)
        {
            if (overlay->CreateOverlay("dsvr.pointer", "Dungeon Siege pointer", &m_dot) != vr::VROverlayError_None)
            {
                m_dot = vr::k_ulOverlayHandleInvalid;
                return;
            }
            const int size = 32;
            static UINT32 pixels[size * size];
            for (int y = 0; y < size; ++y)
            {
                for (int x = 0; x < size; ++x)
                {
                    float dx = x + 0.5f - size * 0.5f, dy = y + 0.5f - size * 0.5f;
                    float r = std::sqrt(dx * dx + dy * dy) / (size * 0.5f);
                    UINT32 c = 0;
                    if (r < 0.35f)
                        c = 0xFFFFFFFF;          // white centre
                    else if (r < 0.55f)
                        c = 0xC0000000;          // dark ring for contrast
                    else if (r < 0.8f)
                        c = 0xFF40E0FF;          // gold outer ring (ABGR in memory: R=FF G=E0 B=40)
                    pixels[y * size + x] = c;
                }
            }
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = desc.Height = size;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = { pixels, size * sizeof(UINT32), 0 };
            if (FAILED(m_device->CreateTexture2D(&desc, &init, &m_dotTex)))
                return;
            vr::Texture_t texture = { m_dotTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma };
            overlay->SetOverlayTexture(m_dot, &texture);
            overlay->SetOverlayWidthInMeters(m_dot, 0.03f * m_settings.hudDistance);
            overlay->SetOverlaySortOrder(m_dot, 10);
        }
        if (m_dot == vr::k_ulOverlayHandleInvalid)
            return;

        if (visible)
        {
            float d = m_settings.hudDistance * 0.95f;
            float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
            vr::HmdMatrix34_t t = { {
                { cy, sy * sp, sy * cp, -d * sy * cp },
                { 0.0f, cp, -sp, d * sp },
                { -sy, cy * sp, cy * cp, -d * cy * cp },
            } };
            overlay->SetOverlayTransformAbsolute(m_dot, vr::TrackingUniverseSeated, &t);
        }
        if (visible != m_dotVisible)
        {
            if (visible)
                overlay->ShowOverlay(m_dot);
            else
                overlay->HideOverlay(m_dot);
            m_dotVisible = visible;
        }
    }

    void VrPresenter::GetPanelOrientation(float* yaw, float* pitch)
    {
        AcquireSRWLockShared(&m_panelLock);
        *yaw = m_panelYaw;
        *pitch = m_panelPitch;
        ReleaseSRWLockShared(&m_panelLock);
    }

    void VrPresenter::SetHudVisible(bool visible)
    {
        if (m_hud == vr::k_ulOverlayHandleInvalid || visible == m_hudVisible)
            return;
        if (visible)
            vr::VROverlay()->ShowOverlay(m_hud);
        else
            vr::VROverlay()->HideOverlay(m_hud);
        m_hudVisible = visible;
    }

    void VrPresenter::PresentInterface(const FrameRing::Frame& frame, ID3D11Texture2D* shared)
    {
        if (!EnsureHudPipeline() || !EnsureHudOverlay())
            return;
        if (m_interfaceFrames == 60)
            DumpFrame(shared, frame, "dsvr_interface.bmp");

        ComPtr<ID3D11ShaderResourceView>& srv = m_sharedSrv[frame.slot];
        if (!srv)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.Format = frame.format;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            HRESULT hr = m_device->CreateShaderResourceView(shared, &vd, &srv);
            if (FAILED(hr))
            {
                Log("interface: CreateShaderResourceView failed: 0x%08lX", hr);
                return;
            }
        }

        int index = m_nextHudTex;
        m_nextHudTex ^= 1;

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_keyConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            float* c = static_cast<float*>(mapped.pData);
            c[0] = float(frame.width) / float(frame.texWidth);
            c[1] = float(frame.height) / float(frame.texHeight);
            // Marker strip along the bottom-right edge (stereo_abi.h), with a little margin.
            float cell = float(frame.width) / float(kMarkerCellDivisor);
            c[2] = (float(frame.width) - (kMarkerCells + 0.5f) * cell) / float(frame.texWidth);
            c[3] = (float(frame.height) - 1.2f * cell) / float(frame.texHeight);
            // Key level sample: a pixel just inside the top-left corner of the image.
            c[4] = 2.5f / float(frame.texWidth);
            c[5] = 2.5f / float(frame.texHeight);
            c[6] = c[7] = 0.0f;
            // Hide the parked game cursor while the VR pointer is off the panel (empty rect otherwise).
            AcquireSRWLockShared(&m_pointerLock);
            bool parked = m_pointerVisible && GetTickCount() - m_pointerTick < 250;
            ReleaseSRWLockShared(&m_pointerLock);
            float su = float(frame.width) / float(frame.texWidth), sv = float(frame.height) / float(frame.texHeight);
            float mask = kParkMaskSize * float(frame.width);
            c[8] = parked ? su * kParkX : 2.0f;
            c[9] = parked ? sv * kParkY : 2.0f;
            c[10] = parked ? (kParkX * float(frame.width) + mask) / float(frame.texWidth) : 2.0f;
            c[11] = parked ? (kParkY * float(frame.height) + mask) / float(frame.texHeight) : 2.0f;
            m_context->Unmap(m_keyConstants.Get(), 0);
        }

        D3D11_VIEWPORT vp = { 0.0f, 0.0f, float(frame.width), float(frame.height), 0.0f, 1.0f };
        ID3D11RenderTargetView* rtv = m_hudRtv[index].Get();
        ID3D11ShaderResourceView* srvs[] = { srv.Get() };
        ID3D11SamplerState* samplers[] = { m_pointSampler.Get() };
        ID3D11Buffer* constants[] = { m_keyConstants.Get() };
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_keyVs.Get(), nullptr, 0);
        m_context->VSSetConstantBuffers(0, 1, constants);
        m_context->PSSetShader(m_keyPs.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, srvs);
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->PSSetConstantBuffers(0, 1, constants);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv[] = { nullptr };
        m_context->PSSetShaderResources(0, 1, noSrv);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->Flush();  // the compositor reads the texture as soon as it is set

        vr::VRTextureBounds_t bounds = { 0.0f, 0.0f,
            float(frame.width) / float(frame.texWidth), float(frame.height) / float(frame.texHeight) };
        vr::Texture_t texture = { m_hudTex[index].Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma };
        vr::VROverlay()->SetOverlayTextureBounds(m_hud, &bounds);
        vr::EVROverlayError err = vr::VROverlay()->SetOverlayTexture(m_hud, &texture);
        if (err != vr::VROverlayError_None && m_interfaceFrames < 3)
            Log("SetOverlayTexture(interface) failed: %s", vr::VROverlay()->GetOverlayErrorNameFromEnum(err));
        SetHudVisible(true);

        if (m_interfaceFrames++ == 0)
            Log("first interface frame: %ux%u", frame.width, frame.height);
    }

    void VrPresenter::SubmitStereo()
    {
        if (m_showingMenu)
        {
            // Black scene behind the menu screen overlay.
            if (!m_blackTex)
                return;
            vr::Texture_t black = { m_blackTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma };
            for (int e = 0; e < 2; ++e)
                vr::VRCompositor()->Submit(vr::EVREye(e), &black, nullptr, vr::Submit_Default);
            return;
        }

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
