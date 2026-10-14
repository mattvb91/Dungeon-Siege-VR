#pragma once

namespace dsvr
{
    // M3: drives the main game camera's eye position from the headset ("tabletop" parallax).
    // Patches the call at 0x661A49 in SiegeCamera's per-frame render setup (0x66192A), which hands
    // Rapi's look-at (0x653E96 -> IDirect3DDevice7::SetTransform(VIEW)) the camera position, target and
    // up vector. Only the position passed to the renderer changes; the game's camera state is untouched.
    bool InstallHeadTracking();
}
