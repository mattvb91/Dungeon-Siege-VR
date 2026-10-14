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
    class VrOverlay
    {
    public:
        void Start(FrameRing* ring);
        void Stop(DWORD timeoutMs);

        // LUID of the GPU dgVoodoo renders on; our D3D11 device must live on the same one.
        void SetAdapterLuid(LUID luid);

    private:
        static DWORD WINAPI ThreadMain(void* self);
        void Run();

        bool TryInitVr();
        void ShutdownVr();
        void PumpVrEvents();

        bool EnsureD3D11();
        void ReleaseD3D11();
        bool EnsureOverlay();
        void PresentFrame(FrameRing::Frame& frame);

        FrameRing* m_ring = nullptr;
        HANDLE m_thread = nullptr;
        volatile LONG m_stop = 0;

        CRITICAL_SECTION m_luidLock;
        LUID m_luid = {};
        bool m_hasLuid = false;
        LUID m_deviceLuid = {};

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
    };
}
