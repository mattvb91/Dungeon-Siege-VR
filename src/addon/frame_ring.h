#pragma once

// Hands finished game frames from dgVoodoo's D3D12 presenter (writer) to the VR thread (reader).
//
// Frames live in a small ring of shared D3D12 textures. The writer copies each presented image
// into a slot that the reader isn't using, then publishes it together with the dgVoodoo fence value
// that signals when the copy has completed on the GPU. The reader takes the newest published slot,
// waits for that fence on the CPU, and opens the slot's shared NT handle on its own D3D11 device.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

namespace dsvr
{
    using Microsoft::WRL::ComPtr;

    class FrameRing
    {
    public:
        static const int kSlots = 3;

        struct Frame
        {
            int slot;
            unsigned generation;   // bumps whenever the slot textures are recreated
            HANDLE sharedHandle;   // duplicate owned by the reader; CloseHandle when done
            ID3D12Fence* fence;    // AddRef'd for the reader; Release when done
            UINT64 fenceValue;
            UINT width;            // valid image region, starting at (0,0)
            UINT height;
            UINT texWidth;         // full texture size
            UINT texHeight;
            DXGI_FORMAT format;
        };

        FrameRing() { m_newFrame = CreateEventA(nullptr, FALSE, FALSE, nullptr); }
        ~FrameRing() { CloseHandle(m_newFrame); }

        HANDLE NewFrameEvent() const { return m_newFrame; }

        // --- writer side (dgVoodoo's D3D12 backend thread) ---

        // Ensures slot textures exist with at least the given size and the given format.
        // Old textures are retired and freed once the GPU is done with them.
        bool EnsureTextures(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, ID3D12Fence* fence, UINT64 pendingValue);

        // Returns a slot index the reader isn't using, or -1.
        int AcquireWriteSlot();
        ID3D12Resource* SlotTexture(int slot) const { return m_slots[slot].texture.Get(); }
        void Publish(int slot, ID3D12Fence* fence, UINT64 fenceValue, UINT width, UINT height);

        // Frees retired textures whose GPU work has completed.
        void CollectGarbage(ID3D12Fence* fence);
        // Releases everything (adapter shutdown). Waits up to timeoutMs for outstanding GPU work.
        void ReleaseAll(ID3D12Fence* fence, DWORD timeoutMs);

        // --- reader side (VR thread) ---

        // Takes the newest unread frame, marking its slot as in use. Returns false if none.
        bool TakeLatest(Frame& frame);
        void EndRead();

    private:
        struct Slot
        {
            ComPtr<ID3D12Resource> texture;
            HANDLE sharedHandle = nullptr;
            UINT64 fenceValue = 0;
            UINT width = 0;
            UINT height = 0;
        };

        struct Retired
        {
            ComPtr<ID3D12Resource> texture;
            HANDLE sharedHandle;
            UINT64 fenceValue;
        };

        void RetireSlots(UINT64 pendingValue);

        mutable std::mutex m_lock;
        Slot m_slots[kSlots];
        std::vector<Retired> m_retired;
        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_newFrame = nullptr;
        int m_latest = -1;
        int m_reading = -1;
        unsigned m_generation = 0;
        UINT m_texWidth = 0;
        UINT m_texHeight = 0;
        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    };
}
