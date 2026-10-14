#pragma once

// M2 "VR cinema": shows the game's presented image on a curved SteamVR overlay.
// Runs on its own thread so the compositor (not the game's frame rate) drives head tracking.

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <openvr.h>

#include "frame_ring.h"

namespace dsvr
{
    // The thread runs until process exit (see FrameCapture in addon_main.cpp for why it is never stopped).
    class VrOverlay
    {
    public:
        // Screen placement in the seated tracking space (metres).
        void Start(FrameRing* ring, float screenWidth, float screenDistance);

        // LUID of the GPU dgVoodoo renders on; our D3D11 device must live on the same one.
        void SetAdapterLuid(LUID luid);

        // HMD position in the seated tracking space (metres, OpenVR axes: +x right, +y up, -z forward),
        // predicted slightly ahead. Callable from any thread; false if SteamVR or the pose isn't available.
        bool GetHeadPosition(float out[3]);

    private:
        static DWORD WINAPI ThreadMain(void* self);
        [[noreturn]] void Run();

        bool TryInitVr();
        void ShutdownVr();
        void PumpVrEvents();

        bool EnsureD3D11();
        void ReleaseD3D11();
        bool EnsureOverlay();
        void PresentFrame(FrameRing::Frame& frame);

        FrameRing* m_ring = nullptr;
        float m_screenWidth = 3.0f;
        float m_screenDistance = 2.2f;

        CRITICAL_SECTION m_luidLock;
        LUID m_luid = {};
        bool m_hasLuid = false;
        LUID m_deviceLuid = {};

        SRWLOCK m_vrLock = SRWLOCK_INIT;  // exclusive: VR init/shutdown; shared: pose queries
        bool m_vrReady = false;
        DWORD m_nextVrAttempt = 0;
        vr::VROverlayHandle_t m_overlay = vr::k_ulOverlayHandleInvalid;

        Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<ID3D11Query> m_copyDone;
        HANDLE m_fenceEvent = nullptr;

        unsigned m_generation = ~0u;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shared[FrameRing::kSlots];
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_overlayTex[2];
        int m_nextOverlayTex = 0;
        unsigned m_framesShown = 0;
        unsigned m_openFailures = 0;
    };
}
