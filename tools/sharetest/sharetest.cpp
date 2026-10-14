// Standalone check: which D3D12 shared-texture settings can a D3D11 device open?
// Mirrors FrameRing::EnsureTextures (D3D12 side) and VrOverlay::PresentFrame (D3D11 side).
#include <windows.h>
#include <d3d12.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

int main()
{
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (wcsstr(d.Description, L"NVIDIA")) { wprintf(L"adapter: %s\n", d.Description); break; }
        adapter.Reset();
    }
    if (!adapter) { printf("no NVIDIA adapter\n"); return 1; }

    ComPtr<ID3D12Device> d12;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)))) { printf("D3D12CreateDevice failed\n"); return 1; }
    ComPtr<ID3D11Device> d11base;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &d11base, nullptr, nullptr))) { printf("D3D11CreateDevice failed\n"); return 1; }
    ComPtr<ID3D11Device1> d11;
    d11base.As(&d11);

    struct Case { const char* name; D3D12_RESOURCE_FLAGS flags; };
    const Case cases[] = {
        { "SIMULTANEOUS_ACCESS (current)",          D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS },
        { "SIMULTANEOUS_ACCESS | RENDER_TARGET",    D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
        { "RENDER_TARGET",                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET },
        { "NONE",                                   D3D12_RESOURCE_FLAG_NONE },
    };
    const DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };

    for (DXGI_FORMAT fmt : formats)
    for (const Case& c : cases)
    {
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 800; desc.Height = 600; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = fmt; desc.SampleDesc.Count = 1; desc.Flags = c.flags;

        ComPtr<ID3D12Resource> tex;
        HRESULT hr = d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex));
        HANDLE h = nullptr;
        if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(tex.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        HRESULT open = E_FAIL;
        if (SUCCEEDED(hr))
        {
            ComPtr<ID3D11Texture2D> t11;
            open = d11->OpenSharedResource1(h, IID_PPV_ARGS(&t11));
            CloseHandle(h);
        }
        printf("fmt %2d  %-38s create/share 0x%08lX  D3D11 open 0x%08lX  %s\n", fmt, c.name, hr, open, SUCCEEDED(open) ? "OK" : "FAIL");
    }
    return 0;
}
