#pragma once

#include <windows.h>

struct IDirect3DDevice7;
struct IDirectDrawSurface7;

namespace dsvr
{
    // Separates the game's 2D interface from the 3D world in stereo mode. Everything TattooGame's frame
    // function draws after the world (from the GUI call at 0x479F27 to the end of the frame) is interface.
    //  - Eye frames: the back buffer is saved when the interface starts and put back before the frame is
    //    presented, so eyes see only the world.
    //  - Interface frames: the back buffer is cleared to kUiKeyColor when the interface starts; the add-on
    //    turns that colour transparent and shows the result on a head-locked panel.
    const DWORD kUiKeyColor = 0xFFFF00FF;  // D3DCOLOR magenta; exact in 565 back buffers too

    class UiLayer
    {
    public:
        bool SaveWorld(IDirect3DDevice7* device);
        void RestoreWorld(IDirect3DDevice7* device);
        void ClearToKey(IDirect3DDevice7* device);

    private:
        IDirectDrawSurface7* m_saved = nullptr;
        DWORD m_width = 0;
        DWORD m_height = 0;
        bool m_valid = false;
        unsigned m_failures = 0;
    };
}
