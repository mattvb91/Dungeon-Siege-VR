#include "frame_ring.h"

#include "common/log.h"

namespace dsvr
{
    bool FrameRing::EnsureTextures(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, ID3D12Fence* fence, UINT64 pendingValue)
    {
        std::lock_guard<std::mutex> guard(m_lock);

        if (m_slots[0].texture && width <= m_texWidth && height <= m_texHeight && format == m_format)
            return true;

        RetireSlots(pendingValue);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Simultaneous access: no transition barriers needed (implicit promotion/decay from COMMON),
        // which also keeps these textures out of dgVoodoo's resource tracking.
        // Render target: without it D3D11's OpenSharedResource1 rejects the handle with E_INVALIDARG
        // (verified on RTX 5080 with tools/sharetest).
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        for (int i = 0; i < kSlots; ++i)
        {
            Slot& slot = m_slots[i];
            HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.texture));
            if (FAILED(hr))
            {
                Log("CreateCommittedResource(%ux%u fmt %d) failed: 0x%08lX", width, height, format, hr);
                RetireSlots(pendingValue);
                return false;
            }
            hr = device->CreateSharedHandle(slot.texture.Get(), nullptr, GENERIC_ALL, nullptr, &slot.sharedHandle);
            if (FAILED(hr))
            {
                Log("CreateSharedHandle failed: 0x%08lX", hr);
                RetireSlots(pendingValue);
                return false;
            }
        }

        m_fence = fence;
        m_texWidth = width;
        m_texHeight = height;
        m_format = format;
        ++m_generation;
        Log("frame ring textures: %ux%u format %d (generation %u)", width, height, format, m_generation);
        return true;
    }

    void FrameRing::RetireSlots(UINT64 pendingValue)
    {
        for (Slot& slot : m_slots)
        {
            if (slot.texture)
                m_retired.push_back({ slot.texture, slot.sharedHandle, pendingValue });
            slot = Slot();  // reader detects the generation change and drops its handles
        }
        m_texWidth = m_texHeight = 0;
    }

    int FrameRing::AcquireWriteSlot()
    {
        std::lock_guard<std::mutex> guard(m_lock);
        int oldest = -1;
        for (int i = 0; i < kSlots; ++i)
        {
            const Slot& s = m_slots[i];
            if (!s.texture)
                continue;
            if (s.state == Free)
                return i;
            if (s.state == Published && (oldest < 0 || s.sequence < m_slots[oldest].sequence))
                oldest = i;
        }
        if (oldest >= 0)
        {
            m_slots[oldest].state = Free;
            ++m_dropped;
        }
        return oldest;
    }

    void FrameRing::Publish(int slot, ID3D12Fence* fence, UINT64 fenceValue, UINT width, UINT height)
    {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            Slot& s = m_slots[slot];
            s.fenceValue = fenceValue;
            s.width = width;
            s.height = height;
            s.sequence = ++m_sequence;
            s.state = Published;
            m_fence = fence;
        }
        SetEvent(m_newFrame);
    }

    void FrameRing::CollectGarbage(ID3D12Fence* fence)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_retired.empty())
            return;

        UINT64 completed = fence->GetCompletedValue();
        for (size_t i = 0; i < m_retired.size();)
        {
            if (m_retired[i].fenceValue <= completed)
            {
                CloseHandle(m_retired[i].sharedHandle);
                m_retired.erase(m_retired.begin() + i);
            }
            else
            {
                ++i;
            }
        }
    }

    void FrameRing::ReleaseAll(ID3D12Fence* fence, DWORD timeoutMs)
    {
        std::lock_guard<std::mutex> guard(m_lock);

        UINT64 waitValue = 0;
        for (const Slot& slot : m_slots)
            waitValue = slot.fenceValue > waitValue ? slot.fenceValue : waitValue;
        for (const Retired& r : m_retired)
            waitValue = r.fenceValue > waitValue ? r.fenceValue : waitValue;

        if (fence && fence->GetCompletedValue() < waitValue)
        {
            HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(fence->SetEventOnCompletion(waitValue, done)))
                WaitForSingleObject(done, timeoutMs);
            CloseHandle(done);
        }

        RetireSlots(0);
        for (Retired& r : m_retired)
            CloseHandle(r.sharedHandle);
        m_retired.clear();
        m_fence.Reset();
        m_format = DXGI_FORMAT_UNKNOWN;
        ++m_generation;
    }

    bool FrameRing::TakeOldest(Frame& frame)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (!m_fence)
            return false;

        int oldest = -1;
        for (int i = 0; i < kSlots; ++i)
        {
            if (m_slots[i].state == Published && (oldest < 0 || m_slots[i].sequence < m_slots[oldest].sequence))
                oldest = i;
        }
        if (oldest < 0)
            return false;

        Slot& slot = m_slots[oldest];
        frame.slot = oldest;
        frame.generation = m_generation;
        frame.sharedHandle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), slot.sharedHandle, GetCurrentProcess(), &frame.sharedHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return false;
        frame.fence = m_fence.Get();
        frame.fence->AddRef();
        frame.fenceValue = slot.fenceValue;
        frame.width = slot.width;
        frame.height = slot.height;
        frame.texWidth = m_texWidth;
        frame.texHeight = m_texHeight;
        frame.format = m_format;

        slot.state = Reading;
        return true;
    }

    void FrameRing::EndRead(const Frame& frame)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        // Textures recreated meanwhile: the slot was already reset.
        if (frame.generation == m_generation && m_slots[frame.slot].state == Reading)
            m_slots[frame.slot].state = Free;
    }
}
