#pragma once

// Data shared between the loader (version.dll, game-side camera hooks) and the add-on (SampleAddon.dll,
// owns SteamVR) for M4 alternate-eye stereo. Plain C layout: both DLLs are built by this project.

namespace dsvr
{
    struct StereoView
    {
        int eye;                // 0 = left, 1 = right
        float headPose[3][4];   // HMD -> seated tracking space (OpenVR, right-handed, metres)
        float eyeToHead[3][4];  // eye -> HMD
        // View-space tangents of the eye's frustum edges (x right, y up, looking along +z):
        float tanLeft, tanRight, tanUp, tanDown;  // tanLeft/tanDown are negative
    };

    // Marker stamped into the bottom-right corner of each stereo frame by the loader and read back by the
    // add-on after capture, so the eye and pose travel with the image (dgVoodoo may present asynchronously).
    // Values survive 16-bit (565) back buffers: R is a magic value, G carries 6 bits of frame id, B the eye.
    const unsigned kMarkerMagicR = 0xA5;
    const unsigned kMarkerFrameIds = 64;
    const unsigned kMarkerSize = 2;  // pixels

    inline unsigned MarkerColor(unsigned frameId, int eye)
    {
        unsigned g = (frameId % kMarkerFrameIds) << 2;
        unsigned b = eye ? 0xF8 : 0x00;
        return 0xFF000000u | (kMarkerMagicR << 16) | (g << 8) | b;  // D3DCOLOR (ARGB)
    }

    inline bool DecodeMarker(unsigned r, unsigned g, unsigned b, unsigned& frameId, int& eye)
    {
        if (r < kMarkerMagicR - 6 || r > kMarkerMagicR + 6)
            return false;
        if (b > 0x40 && b < 0xB8)
            return false;  // neither eye: not our marker
        frameId = (g >> 2) % kMarkerFrameIds;
        eye = b >= 0xB8 ? 1 : 0;
        return true;
    }
}
