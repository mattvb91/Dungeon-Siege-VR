#pragma once

#include <windows.h>

namespace dsvr
{
    // M3: drives the main game camera's eye position from the headset ("tabletop" parallax).
    // SiegeCamera's per-frame render setup (0x66192A) makes two calls we redirect:
    //   0x6619EB -> 0x6799C1  frustum setup (culling, and stored in the camera for picking)
    //   0x661A49 -> 0x653E96  Rapi::SetViewLookAt -> IDirect3DDevice7::SetTransform(VIEW)
    // Both receive the same head-shifted eye; the game's own camera fields are untouched.
    // Settings come from dsvr.ini [HeadTracking] next to `module`.
    bool InstallHeadTracking(HMODULE module);
}
