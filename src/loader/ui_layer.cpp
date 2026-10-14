#include "ui_layer.h"

#include <ddraw.h>
#include <d3d.h>

#include "common/log.h"

namespace dsvr
{
    bool UiLayer::SaveWorld(IDirect3DDevice7* device)
    {
        m_valid = false;
        IDirectDrawSurface7* target = nullptr;
        if (FAILED(device->GetRenderTarget(&target)) || !target)
            return false;

        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = target->GetSurfaceDesc(&desc);
        if (SUCCEEDED(hr) && (!m_saved || desc.dwWidth != m_width || desc.dwHeight != m_height))
        {
            if (m_saved)
            {
                m_saved->Release();
                m_saved = nullptr;
            }
            IDirectDraw7* ddraw = nullptr;
            hr = target->GetDDInterface(reinterpret_cast<void**>(&ddraw));
            if (SUCCEEDED(hr))
            {
                DDSURFACEDESC2 create = {};
                create.dwSize = sizeof(create);
                create.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                create.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
                create.dwWidth = desc.dwWidth;
                create.dwHeight = desc.dwHeight;
                create.ddpfPixelFormat = desc.ddpfPixelFormat;
                hr = ddraw->CreateSurface(&create, &m_saved, nullptr);
                ddraw->Release();
                if (SUCCEEDED(hr))
                {
                    m_width = desc.dwWidth;
                    m_height = desc.dwHeight;
                    Log("ui layer: world save surface %lux%lu", m_width, m_height);
                }
            }
        }

        if (SUCCEEDED(hr) && m_saved)
            hr = m_saved->Blt(nullptr, target, nullptr, DDBLT_WAIT, nullptr);
        target->Release();

        if (FAILED(hr) || !m_saved)
        {
            if (m_failures++ < 3)
                Log("ui layer: saving the world image failed: 0x%08lX", hr);
            return false;
        }
        m_valid = true;
        return true;
    }

    void UiLayer::RestoreWorld(IDirect3DDevice7* device)
    {
        if (!m_valid)
            return;
        m_valid = false;
        IDirectDrawSurface7* target = nullptr;
        if (FAILED(device->GetRenderTarget(&target)) || !target)
            return;
        HRESULT hr = target->Blt(nullptr, m_saved, nullptr, DDBLT_WAIT, nullptr);
        target->Release();
        if (FAILED(hr) && m_failures++ < 3)
            Log("ui layer: restoring the world image failed: 0x%08lX", hr);
    }

    void UiLayer::ClearToKey(IDirect3DDevice7* device)
    {
        HRESULT hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, kUiKeyColor, 1.0f, 0);
        if (FAILED(hr) && m_failures++ < 3)
            Log("ui layer: clearing to the key colour failed: 0x%08lX", hr);
    }
}
