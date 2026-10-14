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
    // Module whose folder receives diagnostic dumps (set by addon_main.cpp).
    extern HMODULE g_dumpModule;

    struct PresenterSettings
    {
        bool stereo = false;
        float screenWidth = 2.0f;     // overlay screen (mono mode, stereo menus), metres: ~44 deg at 2.5 m
        float screenDistance = 2.5f;
        float hudWidth = 1.6f;        // stereo: interface panel, metres (~56 deg at 1.5 m)
        float hudDistance = 1.5f;
        float hudFollowDegrees = 0.0f;   // panel drifts to the gaze beyond this angle; 0 = fixed (the VR pointer reaches past it)
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

        // Stereo mode: the pose and projection the game should render frame `frameId` with for `eye`.
        // Repeated calls with the same id and eye return the same view. Called from the game thread.
        bool GetStereoView(unsigned frameId, int eye, StereoView* out);

        // Stereo: current interface panel direction in seated space (radians; yaw about up, then pitch).
        void GetPanelOrientation(float* yaw, float* pitch);

        // Stereo: the loader's mouse-driven VR pointer; a dot is drawn there while it is off the panel.
        void SetPointer(float yaw, float pitch, bool visible);

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
        // Interface frames: key colour -> transparent, shown on the head-locked panel.
        void PresentInterface(const FrameRing::Frame& frame, ID3D11Texture2D* shared);
        bool EnsureHudPipeline();
        bool EnsureHudOverlay();
        void SetHudVisible(bool visible);
        void UpdatePanelFollow(const vr::HmdMatrix34_t& head);
        void PlacePanel();
        void UpdatePointerDot();
        void DumpFrame(ID3D11Texture2D* source, const FrameRing::Frame& frame, const char* fileName);
        void SubmitStereo();
        bool ReadMarker(ID3D11Texture2D* source, const FrameRing::Frame& frame, unsigned& frameId, int& eye);
        bool LooksLikeInterface(ID3D11Texture2D* source, const FrameRing::Frame& frame);

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
        float m_rawTan[2][4] = {};  // per eye: real frustum tangents left, right, up, down
        float m_symTan[2][2] = {};  // per eye: smallest symmetric half-width / half-height covering it
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
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_blackTex;       // stereo: scene behind the menu screen
        bool m_showingMenu = false;                               // stereo: last frame was unmarked (menu)

        // Stereo interface panel.
        vr::VROverlayHandle_t m_hud = vr::k_ulOverlayHandleInvalid;
        bool m_hudVisible = false;
        SRWLOCK m_panelLock = SRWLOCK_INIT;
        float m_panelYaw = 0.0f;
        float m_panelPitch = 0.0f;
        bool m_panelFollowing = false;
        DWORD m_lastFollowTick = 0;

        // Pointer dot (written by the game thread through SetPointer, drawn by the VR thread).
        SRWLOCK m_pointerLock = SRWLOCK_INIT;
        float m_pointerYaw = 0.0f;
        float m_pointerPitch = 0.0f;
        bool m_pointerVisible = false;
        DWORD m_pointerTick = 0;
        vr::VROverlayHandle_t m_dot = vr::k_ulOverlayHandleInvalid;
        bool m_dotVisible = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_dotTex;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_keyVs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_keyPs;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pointSampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_keyConstants;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sharedSrv[FrameRing::kSlots];
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hudTex[2];
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_hudRtv[2];
        int m_nextHudTex = 0;
        unsigned m_interfaceFrames = 0;
        bool m_eyeHasFrame[2] = {};
        bool m_eyeHasPose[2] = {};
        vr::HmdMatrix34_t m_eyePose[2];
        vr::VRTextureBounds_t m_eyeBounds[2];

        unsigned m_framesShown = 0;
        unsigned m_openFailures = 0;
        unsigned m_stereoFrames = 0;
        unsigned m_monoFrames = 0;
        unsigned m_staleFrames = 0;
        unsigned m_misreadFrames = 0;  // eye-marked frames that were really interface frames
        unsigned m_unmarkedInPlay = 0; // unmarked frames amid stereo play (skipped)
        DWORD m_lastMarkedTick = 0;
        char m_lastMarkerCells[96] = {};
        unsigned m_waitErrors = 0;     // diagnostic: WaitGetPoses failures since the last stats line
        int m_lastWaitError = 0;
        DWORD m_lastStatsLog = 0;
    };
}
