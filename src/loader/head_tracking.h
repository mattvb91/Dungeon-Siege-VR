#pragma once

#include <windows.h>

namespace dsvr
{
    // Drives the main game camera from the headset. The game's own camera fields are never modified;
    // only the values SiegeCamera's per-frame render setup (0x66192A) passes on are replaced:
    //   0x6619EB -> 0x6799C1  frustum setup (culling, and stored in the camera for picking)
    //   0x661A31 -> 0x653D06  Rapi::SetPerspective -> SetTransform(PROJECTION)   [stereo only]
    //   0x661A49 -> 0x653E96  Rapi::SetViewLookAt  -> SetTransform(VIEW)
    // Mono (M3): head-position parallax. Stereo (M4, dsvr.ini [Stereo] Enabled=1): alternate-eye rendering
    // with the full eye pose and off-centre projection from SampleAddon.dll; 0x47A0F9 (end of a rendered
    // frame) is also redirected to stamp the eye/frame marker (common/stereo_abi.h).
    // Settings come from dsvr.ini next to `module`.
    bool InstallHeadTracking(HMODULE module);
}
