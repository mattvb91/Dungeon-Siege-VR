#pragma once

namespace dsvr
{
    // Where the interface panel sits relative to an eye, for drawing it into that eye's flat image (monitor
    // mirror). Panel space: centre at the origin, x right, y up, the panel in the z = 0 plane facing +z.
    // Eye space as OpenVR: x right, y up, looking along -z.
    struct PanelProjection
    {
        float eyeOrigin[3];    // eye position in panel space
        float eyeToPanel[3][3];  // rotation: eye-space direction -> panel-space direction
        float tanHalfWidth;    // the eye's symmetric frustum before fitting to the image (FitSymmetricFrustum)
        float tanHalfHeight;
        float panelHalfWidth;  // metres; the half height follows from the interface image's aspect
    };
}
