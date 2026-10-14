#pragma once

// Stereo mode presents alternating left-eye, right-eye and interface frames, which makes the game window on
// the monitor flicker. MonitorMirror replaces dgVoodoo's presentation input with a texture that is only
// updated by left-eye frames (and unmarked ones, e.g. menus): a full-screen draw on dgVoodoo's command list
// reads the frame's corner marker (common/stereo_abi.h) on the GPU and discards everything else.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "Addon/ID3D12RootObserver.hpp"

namespace dsvr
{
    class MonitorMirror
    {
    public:
        bool BeginAdapter(ID3D12Root* root, UInt32 adapterID);
        void EndAdapter(ID3D12Root* root, UInt32 adapterID);

        // Records the conditional copy and fills `out` with the mirror. False = leave presentation alone.
        bool Present(ID3D12Root* root, UInt32 adapterID, DXGI_FORMAT format,
                     const dgVoodoo::ID3D12RootObserver::PresentBeginContextInput& in,
                     dgVoodoo::ID3D12RootObserver::PresentBeginContextOutput& out);

    private:
        bool EnsureTexture(ID3D12Root* root, UInt32 adapterID, UINT width, UINT height, DXGI_FORMAT format);
        void ReleaseTexture(ID3D12Root* root, UInt32 adapterID);

        ID3D12Device* m_device = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vs;
        Microsoft::WRL::ComPtr<ID3DBlob> m_ps;
        ID3D12RootSignature* m_rootSignature = nullptr;  // owned; released via GPLRootSignatureReleased
        ID3D12Root::GraphicsPLDesc m_pipeline = {};

        Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
        UINT m_width = 0;
        UINT m_height = 0;
        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
        UInt32 m_rtvHandle = 0;
        UInt32 m_srvHandle = 0;
        bool m_haveDescriptors = false;
        bool m_failed = false;
    };
}
