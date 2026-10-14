#pragma once

// Stereo mode presents alternating left-eye, right-eye and interface frames, which makes the game window on
// the monitor flicker. MonitorMirror replaces dgVoodoo's presentation input with a steady picture drawn on
// dgVoodoo's command list; the frame's corner marker (common/stereo_abi.h) is read on the GPU:
//  - world: updated by left-eye frames and unmarked ones (e.g. menus);
//  - interface: updated by interface frames, magenta key made transparent as on the VR panel; cleared by
//    unmarked frames (their interface is already in the picture);
//  - output: the world with the interface drawn where the panel appears to the left eye (flat over the
//    whole picture until the first stereo view).

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "Addon/ID3D12RootObserver.hpp"
#include "panel_projection.h"

namespace dsvr
{
    class MonitorMirror
    {
    public:
        bool BeginAdapter(ID3D12Root* root, UInt32 adapterID);
        void EndAdapter(ID3D12Root* root, UInt32 adapterID);

        // Records the mirror update and fills `out` with the output. False = leave presentation alone.
        // cursorParked: the VR pointer is off the panel; the game's parked cursor is masked out.
        // panel: the panel as seen from the left eye; null = interface over the whole picture.
        bool Present(ID3D12Root* root, UInt32 adapterID, DXGI_FORMAT format, bool cursorParked, const PanelProjection* panel,
                     const dgVoodoo::ID3D12RootObserver::PresentBeginContextInput& in,
                     dgVoodoo::ID3D12RootObserver::PresentBeginContextOutput& out);

    private:
        enum Target { kWorld, kInterface, kOutput, kTargetCount };

        struct RenderTarget
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> texture;
            UInt32 rtvHandle = 0;
            UInt32 srvHandle = 0;
        };

        bool EnsureTextures(ID3D12Root* root, UInt32 adapterID, UINT width, UINT height, DXGI_FORMAT format);
        void ReleaseTextures(ID3D12Root* root, UInt32 adapterID);

        ID3D12Device* m_device = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vs;
        Microsoft::WRL::ComPtr<ID3DBlob> m_ps[kTargetCount];
        ID3D12RootSignature* m_rootSignature = nullptr;  // owned; released via GPLRootSignatureReleased
        ID3D12Root::GraphicsPLDesc m_pipeline[kTargetCount] = {};

        RenderTarget m_targets[kTargetCount];
        UINT m_width = 0;
        UINT m_height = 0;
        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
        bool m_haveTextures = false;
        bool m_cleared = false;  // interface target cleared to transparent since creation
        bool m_failed = false;
    };
}
