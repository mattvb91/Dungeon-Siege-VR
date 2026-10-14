#pragma once

// Puts captured game frames into the headset. Two modes, chosen by dsvr.ini [Stereo] Enabled:
//  - mono (M2/M3): a curved SteamVR overlay in seated space; the compositor tracks the head.
//  - stereo (M4): a SteamVR scene app. The loader renders alternate eyes on alternate game frames using
//    the poses handed out by GetStereoView; each captured frame carries a corner marker naming its eye and
//    frame id, and is submitted to that eye together with the head pose it was rendered with.
// Runs on its own thread for the life of the process (see FrameCapture in addon_main.cpp).

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <openvr.h>

#include "common/stereo_abi.h"
#include "frame_ring.h"

namespace dsvr
{
    struct PresenterSettings
    {
        bool stereo = false;
        float screenWidth = 3.0f;     // mono overlay, metres
        float screenDistance = 2.2f;  // mono overlay, metres
    };

    class VrPresenter
    {
    public:
        void Start(FrameRing* ring, const PresenterSettings& settings);

        // LUID of the GPU dgVoodoo renders on; our D3D11 device must live on the same one.
        void SetAdapterLuid(LUID luid);

        // HMD position in the seated tracking space (metres, OpenVR axes: +x right, +y up, -z forward),
        // predicted slightly ahead. Callable from any thread; false if SteamVR or the pose isn't available.
        bool GetHeadPosition(float out[3]);

        // Stereo mode: the eye, pose and projection the game should render frame `frameId` with.
        // Repeated calls with the same id return the same view. Called from the game thread.
        bool GetStereoView(unsigned frameId, StereoView* out);

    private:
        static DWORD WINAPI ThreadMain(void* self);
        [[noreturn]] void Run();

        bool TryInitVr();
        void ShutdownVr();
        void PumpVrEvents();

        bool EnsureD3D11();
        void ReleaseD3D11();
        bool EnsureOverlay();

        // Waits for the frame's GPU copy and returns its texture opened on our D3D11 device.
        ID3D11Texture2D* OpenFrame(FrameRing::Frame& frame);
        bool EnsureTextures(const FrameRing::Frame& frame);

        void PresentMono(FrameRing::Frame& frame);
        void ConsumeStereo(FrameRing::Frame& frame);
        void SubmitStereo();
        bool ReadMarker(ID3D11Texture2D* source, const FrameRing::Frame& frame, unsigned& frameId, int& eye);

        FrameRing* m_ring = nullptr;
        PresenterSettings m_settings;

        CRITICAL_SECTION m_luidLock;
        LUID m_luid = {};
        bool m_hasLuid = false;
        LUID m_deviceLuid = {};

        SRWLOCK m_vrLock = SRWLOCK_INIT;  // exclusive: VR init/shutdown; shared: pose queries
        bool m_vrReady = false;
        DWORD m_nextVrAttempt = 0;
        vr::VROverlayHandle_t m_overlay = vr::k_ulOverlayHandleInvalid;
        float m_frameSeconds = 1.0f / 90.0f;
        float m_vsyncToPhotons = 0.0f;

        // Stereo views handed to the game, indexed by marker frame id.
        struct IssuedView
        {
            bool valid = false;
            unsigned frameId = 0;
            StereoView view;
        };
        SRWLOCK m_viewLock = SRWLOCK_INIT;
        IssuedView m_views[kMarkerFrameIds];

        Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<ID3D11Query> m_copyDone;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_markerStaging;
        HANDLE m_fenceEvent = nullptr;

        unsigned m_generation = ~0u;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shared[FrameRing::kSlots];
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_overlayTex[2];  // mono
        int m_nextOverlayTex = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_eyeTex[2];      // stereo
        bool m_eyeHasFrame[2] = {};
        bool m_eyeHasPose[2] = {};
        vr::HmdMatrix34_t m_eyePose[2];
        vr::VRTextureBounds_t m_eyeBounds[2];

        unsigned m_framesShown = 0;
        unsigned m_openFailures = 0;
        unsigned m_stereoFrames = 0;
        unsigned m_monoFrames = 0;
        unsigned m_staleFrames = 0;
        DWORD m_lastStatsLog = 0;
    };
}
