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
    //
    // The captured image has the game's gamma ramp applied (measured: 255 -> 228, 165 -> 160), so every
    // channel is fully on or off and read with a threshold. Four square cells sit along the bottom edge,
    // right to left, each 1/256 of the image width (so they scale with dgVoodoo's internal resolution):
    //   cell 0: signature, yellow (R on, G on, B off)
    //   cell 1: kind: R = bit 0, G = bit 1, B on
    //   cell 2: frame id bits 0-2 (R, G, B)
    //   cell 3: frame id bits 3-5 (R, G, B)
    const unsigned kMarkerFrameIds = 64;
    const unsigned kMarkerCells = 4;
    const unsigned kMarkerCellDivisor = 256;  // cell size = image width / 256 (minimum 2 game pixels)

    inline unsigned MarkerCellSize(unsigned imageWidth)
    {
        unsigned size = imageWidth / kMarkerCellDivisor;
        return size < 2 ? 2 : size;
    }

    inline unsigned MarkerBitsColor(unsigned bits)  // bit 0 = R, 1 = G, 2 = B -> D3DCOLOR (ARGB)
    {
        return 0xFF000000u | ((bits & 1) ? 0x00FF0000u : 0) | ((bits & 2) ? 0x0000FF00u : 0) | ((bits & 4) ? 0x000000FFu : 0);
    }

    inline unsigned MarkerCellBits(unsigned cell, unsigned frameId, int kind)
    {
        unsigned id = frameId % kMarkerFrameIds;
        switch (cell)
        {
        case 0:  return 1 | 2;             // yellow signature
        case 1:  return unsigned(kind) | 4;
        case 2:  return id & 7;
        default: return (id >> 3) & 7;
        }
    }

    inline unsigned MarkerBitsFromRgb(unsigned r, unsigned g, unsigned b)
    {
        const unsigned threshold = 100;
        return (r > threshold ? 1u : 0u) | (g > threshold ? 2u : 0u) | (b > threshold ? 4u : 0u);
    }

    // rgb[cell] for cells 0..3 (8-bit values as captured).
    inline bool DecodeMarker(const unsigned rgb[kMarkerCells][3], unsigned& frameId, int& kind)
    {
        unsigned bits[kMarkerCells];
        for (unsigned i = 0; i < kMarkerCells; ++i)
            bits[i] = MarkerBitsFromRgb(rgb[i][0], rgb[i][1], rgb[i][2]);
        if (bits[0] != 3 || !(bits[1] & 4))
            return false;
        kind = int(bits[1] & 3);
        if (kind > kFrameInterface)
            return false;
        frameId = bits[2] | (bits[3] << 3);
        return true;
    }
}
