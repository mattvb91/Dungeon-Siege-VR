#pragma once

#include <windows.h>

namespace dsvr
{
    // Stereo mode with the interface panel: the mouse drives a pointer that can aim anywhere in seated space,
    // instead of a cursor confined to the game's 2D interface image (which is shown on the panel).
    //  - The game's mouse input is redirected (exe imports GetCursorPos / SetCursorPos / PeekMessageA /
    //    GetMessageA): the real cursor is held at the window centre and its movement turns the pointer.
    //  - Pointer on the panel: the game's cursor is placed at the matching panel pixel (UI works as usual).
    //  - Pointer off the panel: the game's cursor is parked away from the edges (kParkX; the add-on hides it)
    //    and the click camera is turned so the parked cursor's ray follows the pointer; the add-on draws a
    //    ring there.
    struct PointerSettings
    {
        float degreesPerPixel = 0.08f;
        float panelWidth = 1.6f;      // metres, as the add-on's interface panel
        float panelDistance = 1.5f;
    };

    bool InstallPointer(const PointerSettings& settings);

    // Called once per game frame from the camera hooks. `active` = stereo, in a world, interface on a panel.
    // gameWidth/Height: the game's render size (the interface image); the window may be larger (dgVoodoo).
    void UpdatePointer(bool active, float panelYaw, float panelPitch, int gameWidth, int gameHeight);

    // Eye frames: keep the game's separately drawn cursor out of the picture (it belongs on the panel).
    void SetDrawnCursorHidden(bool hidden);

    // Pointer direction in seated space (OpenVR conventions: yaw about +y, pitch up; radians).
    bool PointerActive();
    bool PointerOnPanel();
    void GetPointerDirection(float* yaw, float* pitch);

    // Game pixel the game's cursor is at, and the game render size it relates to.
    void GetGameCursor(int* x, int* y, int* width, int* height);
}
