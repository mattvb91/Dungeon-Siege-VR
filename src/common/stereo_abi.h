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
        // View-space tangents of the eye's frustum edges (x right, y up, looking along +z), already
        // symmetric (tanLeft = -tanRight, tanDown = -tanUp); the loader fits them to the target's aspect.
        float tanLeft, tanRight, tanUp, tanDown;  // tanLeft/tanDown are negative
    };

    // Each eye renders a symmetric frustum with the render target's aspect (square pixels), covering the
    // eye's real asymmetric frustum; the add-on crops back to the real one when submitting. Symmetric keeps
    // screen-space UI fusable; the target's aspect lets the game's own frustum (horizontal fov + w/h, used
    // for culling and mouse picking) match what is drawn exactly. Both DLLs must compute this identically.
    inline void FitSymmetricFrustum(float tanHalfWidth, float tanHalfHeight, float aspect, float& outHalfWidth, float& outHalfHeight)
    {
        float w = tanHalfWidth;
        if (tanHalfHeight * aspect > w)
            w = tanHalfHeight * aspect;
        outHalfWidth = w;
        outHalfHeight = w / aspect;
    }

    // What a stereo-mode game frame contains.
    enum FrameKind
    {
        kFrameLeftEye = 0,
        kFrameRightEye = 1,
        kFrameInterface = 2,  // 2D interface over kUiKeyColor (loader/ui_layer.h), shown on a head-locked panel
    };

    // Marker stamped into the bottom-right corner of each stereo frame by the loader and read back by the
    // add-on after capture, so the kind and pose travel with the image (dgVoodoo may present asynchronously).
    // Values survive 16-bit (565) back buffers: R is a magic value, G carries 6 bits of frame id, B the kind.
    const unsigned kMarkerMagicR = 0xA5;
    const unsigned kMarkerFrameIds = 64;
    const unsigned kMarkerSize = 2;  // pixels

    inline unsigned MarkerColor(unsigned frameId, int kind)
    {
        unsigned g = (frameId % kMarkerFrameIds) << 2;
        unsigned b = kind == kFrameRightEye ? 0xF8 : kind == kFrameInterface ? 0x7C : 0x00;
        return 0xFF000000u | (kMarkerMagicR << 16) | (g << 8) | b;  // D3DCOLOR (ARGB)
    }

    inline bool DecodeMarker(unsigned r, unsigned g, unsigned b, unsigned& frameId, int& kind)
    {
        if (r < kMarkerMagicR - 6 || r > kMarkerMagicR + 6)
            return false;
        if (b <= 0x30)
            kind = kFrameLeftEye;
        else if (b >= 0xC8)
            kind = kFrameRightEye;
        else if (b >= 0x5C && b <= 0x9C)
            kind = kFrameInterface;
        else
            return false;  // not our marker
        frameId = (g >> 2) % kMarkerFrameIds;
        return true;
    }
}
