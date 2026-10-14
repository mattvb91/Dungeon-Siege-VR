#include "head_tracking.h"

#include <cmath>
#include <cstring>

#include "common/config.h"
#include "common/log.h"
#include "common/stereo_abi.h"

namespace dsvr
{
    namespace
    {
        // Calls inside SiegeCamera's per-frame render setup (0x66192A), in execution order.
        const DWORD kFrustumCallSite = 0x006619EB;  // -> 0x6799C1 thiscall(camera; w, h, fov, near, far, pos*, matrix3x3*), ret 1Ch
        const DWORD kFrustumFunc     = 0x006799C1;
        const DWORD kPerspCallSite   = 0x00661A31;  // -> Rapi::SetPerspective thiscall(rapi; fov, near, far), ret 0Ch
        const DWORD kPerspFunc       = 0x00653D06;
        const DWORD kSetViewCallSite = 0x00661A49;  // -> Rapi::SetViewLookAt thiscall(rapi; pos*, target*, up*), ret 0Ch
        const DWORD kSetViewFunc     = 0x00653E96;
        // Last call of a rendered frame in TattooGame's frame function 0x4799B1, after the UI.
        const DWORD kEndFrameCallSite = 0x0047A0F9; // -> engine 0x662CA5 thiscall(engine; a, b), ret 8 -> Rapi 0x650F13
        const DWORD kEndFrameFunc     = 0x00662CA5;
        // Terrain mouse picking (engine+0x48 object, thiscall(float, float), ret 8). It builds the cursor ray
        // from the main camera's own m_fieldOfView (+0x124), world position (+0x148) and m_MatrixOrientation
        // (+0x178) instead of the frustum, so item hover was right but click-to-move was not.
        const DWORD kPickCallSites[] = { 0x00479E88, 0x00479F88 };
        const DWORD kPickFunc         = 0x00672701;
        const DWORD kCameraFov        = 0x124;
        const DWORD kCameraMatrix     = 0x178;

        const DWORD kSiegeEnginePtr  = 0x007A05FC;
        const DWORD kEngineCamera    = 0x3C;
        const DWORD kEngineRapi      = 0x44;
        const DWORD kCameraPosition  = 0x148;       // world-space eye, recomputed each frame
        const DWORD kTargetPosition  = 0x154;       // world-space target
        const DWORD kTargetNode      = 0x144;       // m_TargetPosition node guid; 0 = no world loaded (menus)
        const DWORD kCameraUpVector  = 0x160;       // m_UpVector

        // Rapi (renderer) fields used by SetPerspective (0x653D06).
        const DWORD kRapiDevice      = 0x600;       // IDirect3DDevice7*
        const DWORD kRapiWidth       = 0x674;       // render target size used for the aspect ratio
        const DWORD kRapiHeight      = 0x678;
        const DWORD kRapiProjection  = 0xCC8;       // D3DMATRIX passed to SetTransform(PROJECTION)
        const DWORD kRapiProjection2 = 0xD08;       // same with a slightly pushed-out near plane
        const DWORD kNearShiftConst  = 0x007265DC;  // float: near' = near + (far - near) * k

        // IDirect3DDevice7 vtable slots.
        const int kDeviceClear = 10;
        const int kDeviceSetTransform = 11;
        const DWORD kD3DTransformProjection = 3;
        const DWORD kD3DClearTarget = 1;

        struct Vec3 { float x, y, z; };

        Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        Vec3 Mul(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

        // __fastcall with a dummy edx matches MSVC thiscall for callee-cleaned member functions.
        typedef void(__fastcall* FrustumFn)(void* camera, void* edx, int width, int height, float fov,
                                            float nearZ, float farZ, const Vec3* pos, const float* matrix);
        typedef void(__fastcall* PerspectiveFn)(void* rapi, void* edx, float fov, float nearZ, float farZ);
        typedef void(__fastcall* SetViewFn)(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up);
        typedef void(__fastcall* EndFrameFn)(void* engine, void* edx, DWORD a, DWORD b);
        typedef void(__fastcall* PickFn)(void* self, void* edx, float a, float b);
        typedef bool(*GetHeadPositionFn)(float out[3]);
        typedef bool(*GetStereoViewFn)(unsigned frameId, StereoView* out);
        typedef HRESULT(__stdcall* DeviceClearFn)(void* device, DWORD count, const LONG* rects, DWORD flags, DWORD color, float z, DWORD stencil);
        typedef HRESULT(__stdcall* DeviceSetTransformFn)(void* device, DWORD state, const float* matrix);

        const FrustumFn g_originalFrustum = reinterpret_cast<FrustumFn>(kFrustumFunc);
        const PerspectiveFn g_originalPerspective = reinterpret_cast<PerspectiveFn>(kPerspFunc);
        const SetViewFn g_originalSetView = reinterpret_cast<SetViewFn>(kSetViewFunc);
        const EndFrameFn g_originalEndFrame = reinterpret_cast<EndFrameFn>(kEndFrameFunc);
        const PickFn g_originalPick = reinterpret_cast<PickFn>(kPickFunc);

        // Camera the terrain pick should use this frame: what the frustum hook gave the game's frustum.
        struct PickCamera
        {
            bool valid;
            DWORD camera;
            float fov;
            Vec3 pos;
            float matrix[9];
        };
        PickCamera g_pick = {};

        struct Settings
        {
            bool enabled;
            bool stereo;
            float scale;
            float minEyeDistance;
            int toggleKey;
            int recenterKey;
        };
        Settings g_settings;

        GetHeadPositionFn g_getHeadPosition = nullptr;
        GetStereoViewFn g_getStereoView = nullptr;
        float g_baseline[3] = {};      // head position treated as neutral (Ctrl+recenter)
        bool g_toggleDown = false;
        bool g_recenterDown = false;
        unsigned g_trackedFrames = 0;

        // State of the current SiegeCamera render setup, set by the frustum hook (first of the three calls)
        // and consumed by the perspective and view hooks that follow it.
        struct Pass
        {
            DWORD camera;      // 0 = not ours, leave the calls alone
            Vec3 eye;
            Vec3 center;       // stereo: midpoint between the eyes (head), for the game's frustum
            Vec3 target;
            Vec3 up;
            bool stereo;
            StereoView view;
        };
        Pass g_pass = {};

        unsigned g_frameId = 0;        // rendered frames; stereo eye = parity
        int g_frameEye = -1;           // eye rendered this frame, -1 = none (no marker)
        unsigned g_stereoFrames = 0;

        DWORD MainCamera()
        {
            DWORD engine = *reinterpret_cast<const DWORD*>(kSiegeEnginePtr);
            return engine ? *reinterpret_cast<const DWORD*>(engine + kEngineCamera) : 0;
        }

        // SampleAddon.dll is loaded by dgVoodoo and pinned, so pointers stay valid once found.
        bool ConnectAddon()
        {
            if (g_getHeadPosition)
                return true;
            HMODULE addon = GetModuleHandleA("SampleAddon.dll");
            if (!addon)
                return false;
            g_getHeadPosition = reinterpret_cast<GetHeadPositionFn>(GetProcAddress(addon, "DsvrGetHeadPosition"));
            g_getStereoView = reinterpret_cast<GetStereoViewFn>(GetProcAddress(addon, "DsvrGetStereoView"));
            if (!g_getHeadPosition || !g_getStereoView)
            {
                g_getHeadPosition = nullptr;
                return false;
            }
            Log("head tracking: connected to SampleAddon.dll");
            return true;
        }

        bool GameHasFocus()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }

        // Edge-detected Ctrl+key; called once per frame from the game thread.
        bool Pressed(int key, bool& wasDown)
        {
            bool down = key && (GetAsyncKeyState(key) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
            bool pressed = down && !wasDown;
            wasDown = down;
            return pressed;
        }

        void PollHotkeys()
        {
            if (!GameHasFocus())
                return;
            if (Pressed(g_settings.toggleKey, g_toggleDown))
            {
                g_settings.enabled = !g_settings.enabled;
                Log("head tracking: %s", g_settings.enabled ? "on" : "off");
            }
            if (Pressed(g_settings.recenterKey, g_recenterDown))
            {
                float head[3];
                if (ConnectAddon() && g_getHeadPosition(head))
                {
                    memcpy(g_baseline, head, sizeof(g_baseline));
                    Log("head tracking: recentred at %.3f %.3f %.3f m", head[0], head[1], head[2]);
                }
            }
        }

        // Game camera basis as it appears on screen. Rapi's look-at (0x653858) takes the view x axis as
        // forward x up (via 0x43FA4B, which is a x b), and its projection (0x6539DE) is left-handed, so
        // screen-right in the world is forward x up. (Not up x forward: that mirrored head yaw and swapped
        // the eyes in the first stereo test.)
        bool CameraBasis(const Vec3& pos, const Vec3& target, const Vec3& up, Vec3& right, Vec3& camUp, Vec3& forward, float& distance)
        {
            Vec3 toTarget = Sub(target, pos);
            distance = Length(toTarget);
            if (distance < 1e-3f)
                return false;
            forward = Mul(toTarget, 1.0f / distance);
            right = Cross(forward, up);
            float rightLen = Length(right);
            if (rightLen < 1e-3f)
                return false;
            right = Mul(right, 1.0f / rightLen);
            camUp = Cross(right, forward);
            return true;
        }

        // Mono parallax (M3): offset the eye by the head position, keep looking at the target.
        bool ComputeParallaxEye(const Vec3& pos, const Vec3& target, const Vec3& up, Vec3& eye)
        {
            float head[3];
            if (!ConnectAddon() || !g_getHeadPosition(head))
                return false;

            Vec3 right, camUp, forward;
            float distance;
            if (!CameraBasis(pos, target, up, right, camUp, forward, distance))
                return false;

            // OpenVR seated space: +x right, +y up, -z forward (towards the virtual screen).
            float s = g_settings.scale;
            Vec3 offset = Add(Add(Mul(right, (head[0] - g_baseline[0]) * s), Mul(camUp, (head[1] - g_baseline[1]) * s)),
                              Mul(forward, -(head[2] - g_baseline[2]) * s));

            // Keep the eye in front of the target by at least minEyeDistance along the view direction.
            float along = Dot(offset, forward);
            float maxAlong = distance - g_settings.minEyeDistance;
            if (along > maxAlong)
                offset = Add(offset, Mul(forward, maxAlong - along));

            eye = Add(pos, offset);
            if (g_trackedFrames++ == 0)
                Log("head tracking: first tracked frame (head %.3f %.3f %.3f m, eye offset %.2f %.2f %.2f)",
                    head[0], head[1], head[2], offset.x, offset.y, offset.z);
            return true;
        }

        // Stereo (M4): place the eye where the headset's eye is, relative to the game camera, which acts as
        // the seated origin. OpenVR is right-handed (-z forward); the game is left-handed (+z forward), so
        // vectors flip z. Head translation is scaled so the world appears 1/scale size ("tabletop").
        bool ComputeStereoEye(const Vec3& pos, const Vec3& target, const Vec3& up, Pass& pass)
        {
            if (!ConnectAddon() || !g_getStereoView(g_frameId, &pass.view))
                return false;

            Vec3 right, camUp, forward;
            float distance;
            if (!CameraBasis(pos, target, up, right, camUp, forward, distance))
                return false;

            const float (*h)[4] = pass.view.headPose;
            const float (*e)[4] = pass.view.eyeToHead;
            // Eye pose = head pose * eye-to-head.
            float rot[3][3];
            Vec3 eyePos;
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 3; ++c)
                    rot[r][c] = h[r][0] * e[0][c] + h[r][1] * e[1][c] + h[r][2] * e[2][c];
            }
            eyePos.x = h[0][0] * e[0][3] + h[0][1] * e[1][3] + h[0][2] * e[2][3] + h[0][3];
            eyePos.y = h[1][0] * e[0][3] + h[1][1] * e[1][3] + h[1][2] * e[2][3] + h[1][3];
            eyePos.z = h[2][0] * e[0][3] + h[2][1] * e[1][3] + h[2][2] * e[2][3] + h[2][3];
            Vec3 baseline = { g_baseline[0], g_baseline[1], g_baseline[2] };
            eyePos = Sub(eyePos, baseline);
            Vec3 headPos = Sub(Vec3{ h[0][3], h[1][3], h[2][3] }, baseline);

            // Camera-local (left-handed) eye axes and position: flip z of OpenVR vectors.
            Vec3 localUp      = { rot[0][1], rot[1][1], -rot[2][1] };
            Vec3 localForward = { -rot[0][2], -rot[1][2], rot[2][2] };
            Vec3 localPos     = { eyePos.x, eyePos.y, -eyePos.z };

            auto toWorld = [&](const Vec3& v) { return Add(Add(Mul(right, v.x), Mul(camUp, v.y)), Mul(forward, v.z)); };
            Vec3 eyeForward = toWorld(localForward);
            pass.eye = Add(pos, Mul(toWorld(localPos), g_settings.scale));
            pass.center = Add(pos, Mul(toWorld(Vec3{ headPos.x, headPos.y, -headPos.z }), g_settings.scale));
            pass.up = toWorld(localUp);
            pass.target = Add(pass.eye, Mul(eyeForward, distance));
            return true;
        }

        // SiegeCamera orientation layout (m_MatrixOrientation), matching what the game itself stores (probe
        // sample: column 0 = up x forward, i.e. screen-left), then up, forward.
        void OrientationMatrix(const Vec3& eye, const Vec3& target, const Vec3& up, float m[9])
        {
            Vec3 f = Sub(target, eye);
            f = Mul(f, 1.0f / Length(f));
            Vec3 r = Cross(up, f);
            r = Mul(r, 1.0f / Length(r));
            Vec3 u = Cross(f, r);
            const Vec3 cols[3] = { r, u, f };
            for (int c = 0; c < 3; ++c)
            {
                m[0 * 3 + c] = cols[c].x;
                m[1 * 3 + c] = cols[c].y;
                m[2 * 3 + c] = cols[c].z;
            }
        }

        // Left-handed perspective (row vectors, z in [0,1]) from view-space edge tangents. Handles off-centre
        // frusta, though the eye frusta passed in are now symmetric (see FitSymmetricFrustum).
        void OffCentreProjection(const StereoView& v, float nearZ, float farZ, float m[16])
        {
            memset(m, 0, 16 * sizeof(float));
            m[0]  = 2.0f / (v.tanRight - v.tanLeft);
            m[5]  = 2.0f / (v.tanUp - v.tanDown);
            m[8]  = -(v.tanRight + v.tanLeft) / (v.tanRight - v.tanLeft);
            m[9]  = -(v.tanUp + v.tanDown) / (v.tanUp - v.tanDown);
            m[10] = farZ / (farZ - nearZ);
            m[11] = 1.0f;
            m[14] = -nearZ * farZ / (farZ - nearZ);
        }

        void* Device(DWORD rapi) { return *reinterpret_cast<void**>(rapi + kRapiDevice); }

        template <typename Fn>
        Fn DeviceMethod(void* device, int slot) { return reinterpret_cast<Fn>((*reinterpret_cast<void***>(device))[slot]); }

        void __fastcall HookFrustum(void* camera, void* edx, int width, int height, float fov,
                                    float nearZ, float farZ, const Vec3* pos, const float* matrix)
        {
            DWORD cam = reinterpret_cast<DWORD>(camera);
            g_pass.camera = 0;
            bool mainCamera = cam == MainCamera();
            if (mainCamera)
                PollHotkeys();
            if (mainCamera && g_settings.enabled)
            {
                const Vec3& target = *reinterpret_cast<const Vec3*>(cam + kTargetPosition);
                const Vec3& up = *reinterpret_cast<const Vec3*>(cam + kCameraUpVector);

                bool ok;
                if (g_settings.stereo)
                {
                    // The main menu runs this camera too (target on no node). Leave those frames unmarked so
                    // the add-on shows them on the flat screen.
                    bool inWorld = *reinterpret_cast<const DWORD*>(cam + kTargetNode) != 0;
                    ok = inWorld && ComputeStereoEye(*pos, target, up, g_pass);
                    g_pass.stereo = ok;
                }
                else
                {
                    ok = ComputeParallaxEye(*pos, target, up, g_pass.eye);
                    g_pass.target = target;
                    g_pass.up = up;
                    g_pass.stereo = false;
                }

                if (ok)
                {
                    g_pass.camera = cam;
                    float shifted[9];
                    OrientationMatrix(g_pass.eye, g_pass.target, g_pass.up, shifted);
                    float eyeFov = fov;
                    float pickFov = fov;
                    if (g_pass.stereo && height > 0)
                    {
                        // Render exactly what the game's frustum describes (horizontal fov + w/h), so its
                        // culling and mouse picking match the image; the add-on crops to the real eye frustum.
                        float th, tv;
                        FitSymmetricFrustum(g_pass.view.tanRight, g_pass.view.tanUp, float(width) / float(height), th, tv);
                        g_pass.view.tanLeft = -th;
                        g_pass.view.tanRight = th;
                        g_pass.view.tanUp = tv;
                        g_pass.view.tanDown = -tv;
                        eyeFov = 2.0f * std::atan(th);
                        // The terrain pick (0x672701) derives its focal length from the viewport *height*
                        // ((h/2) / tan(fov/2)), i.e. reads the fov as vertical, unlike the projection.
                        pickFov = 2.0f * std::atan(tv);
                    }
                    // Stereo: the game's frustum also drives mouse picking. The cursor is drawn at the same
                    // pixel in both eyes (seen at infinity), so pick from between the eyes rather than from
                    // whichever eye this frame renders. Culling tolerates the half-IPD shift: the rendered
                    // frustum is wider than the cropped view.
                    const Vec3* frustumPos = g_pass.stereo ? &g_pass.center : &g_pass.eye;
                    g_pick.valid = true;
                    g_pick.camera = cam;
                    g_pick.fov = pickFov;
                    g_pick.pos = *frustumPos;
                    memcpy(g_pick.matrix, shifted, sizeof(shifted));
                    g_originalFrustum(camera, edx, width, height, eyeFov, nearZ, farZ, frustumPos, shifted);
                    return;
                }
            }
            if (mainCamera)
                g_pick.valid = false;
            g_originalFrustum(camera, edx, width, height, fov, nearZ, farZ, pos, matrix);
        }

        void __fastcall HookPerspective(void* rapi, void* edx, float fov, float nearZ, float farZ)
        {
            g_originalPerspective(rapi, edx, fov, nearZ, farZ);
            if (!g_pass.camera || !g_pass.stereo)
                return;

            DWORD r = reinterpret_cast<DWORD>(rapi);
            float* projection = reinterpret_cast<float*>(r + kRapiProjection);
            float* projection2 = reinterpret_cast<float*>(r + kRapiProjection2);
            float k = *reinterpret_cast<const float*>(kNearShiftConst);
            OffCentreProjection(g_pass.view, nearZ, farZ, projection);
            OffCentreProjection(g_pass.view, nearZ + (farZ - nearZ) * k, farZ, projection2);

            void* device = Device(r);
            DeviceMethod<DeviceSetTransformFn>(device, kDeviceSetTransform)(device, kD3DTransformProjection, projection);
        }

        void __fastcall HookSetView(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up)
        {
            // The frustum hook ran earlier in the same SiegeCamera::0x66192A call for this camera.
            DWORD cam = reinterpret_cast<DWORD>(up) - kCameraUpVector;
            if (g_pass.camera && cam == g_pass.camera && reinterpret_cast<DWORD>(pos) == cam + kCameraPosition)
            {
                g_pass.camera = 0;
                if (g_pass.stereo)
                {
                    g_frameEye = g_pass.view.eye;
                    g_originalSetView(rapi, edx, &g_pass.eye, &g_pass.target, &g_pass.up);
                }
                else
                {
                    g_originalSetView(rapi, edx, &g_pass.eye, target, up);
                }
                return;
            }
            g_originalSetView(rapi, edx, pos, target, up);
        }

        void __fastcall HookEndFrame(void* engine, void* edx, DWORD a, DWORD b)
        {
            if (g_frameEye >= 0)
            {
                // Stamp the eye and frame id into the bottom-right corner for the add-on to read back.
                DWORD rapi = *reinterpret_cast<const DWORD*>(reinterpret_cast<DWORD>(engine) + kEngineRapi);
                LONG w = *reinterpret_cast<const LONG*>(rapi + kRapiWidth);
                LONG h = *reinterpret_cast<const LONG*>(rapi + kRapiHeight);
                LONG rect[4] = { w - LONG(kMarkerSize), h - LONG(kMarkerSize), w, h };
                void* device = Device(rapi);
                HRESULT hr = DeviceMethod<DeviceClearFn>(device, kDeviceClear)(device, 1, rect, kD3DClearTarget,
                    MarkerColor(g_frameId, g_frameEye), 1.0f, 0);
                if (g_stereoFrames++ == 0)
                    Log("stereo: first marked frame (id %u, eye %d, target %ldx%ld, Clear 0x%08lX)", g_frameId, g_frameEye, w, h, hr);
                g_frameEye = -1;
            }
            ++g_frameId;
            g_originalEndFrame(engine, edx, a, b);
        }

        // Runs the terrain pick with the camera fields temporarily set to this frame's pick camera.
        void __fastcall HookPick(void* self, void* edx, float a, float b)
        {
            DWORD cam = g_pick.valid ? MainCamera() : 0;
            if (!cam || cam != g_pick.camera)
            {
                g_originalPick(self, edx, a, b);
                return;
            }

            BYTE* c = reinterpret_cast<BYTE*>(cam);
            float savedFov;
            Vec3 savedPos;
            float savedMatrix[9];
            memcpy(&savedFov, c + kCameraFov, sizeof(savedFov));
            memcpy(&savedPos, c + kCameraPosition, sizeof(savedPos));
            memcpy(savedMatrix, c + kCameraMatrix, sizeof(savedMatrix));

            memcpy(c + kCameraFov, &g_pick.fov, sizeof(float));
            memcpy(c + kCameraPosition, &g_pick.pos, sizeof(Vec3));
            memcpy(c + kCameraMatrix, g_pick.matrix, sizeof(g_pick.matrix));
            g_originalPick(self, edx, a, b);
            memcpy(c + kCameraFov, &savedFov, sizeof(savedFov));
            memcpy(c + kCameraPosition, &savedPos, sizeof(savedPos));
            memcpy(c + kCameraMatrix, savedMatrix, sizeof(savedMatrix));
        }

        bool PatchCall(DWORD site, DWORD expectedTarget, const void* hook)
        {
            BYTE* p = reinterpret_cast<BYTE*>(site);
            LONG rel;
            memcpy(&rel, p + 1, sizeof(rel));
            if (p[0] != 0xE8 || site + 5 + rel != expectedTarget)
            {
                Log("head tracking: unexpected bytes at 0x%08lX, not patching", site);
                return false;
            }
            DWORD oldProtect;
            if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;
            LONG newRel = static_cast<LONG>(reinterpret_cast<DWORD>(hook) - (site + 5));
            memcpy(p + 1, &newRel, sizeof(newRel));
            VirtualProtect(p, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            return true;
        }

        bool IsCall(DWORD site, DWORD expectedTarget)
        {
            const BYTE* p = reinterpret_cast<const BYTE*>(site);
            LONG rel;
            memcpy(&rel, p + 1, sizeof(rel));
            return p[0] == 0xE8 && site + 5 + rel == expectedTarget;
        }
    }

    bool InstallHeadTracking(HMODULE module)
    {
        Config config(module);
        g_settings.enabled        = config.Bool("HeadTracking", "Enabled", true);
        g_settings.stereo         = config.Bool("Stereo", "Enabled", false);
        g_settings.scale          = config.Float("HeadTracking", "Scale", 6.0f);
        g_settings.minEyeDistance = config.Float("HeadTracking", "MinEyeDistance", 1.5f);
        g_settings.toggleKey      = config.Int("HeadTracking", "ToggleKey", VK_F9);
        g_settings.recenterKey    = config.Int("HeadTracking", "RecenterKey", VK_F10);

        // Verify every site before touching any.
        if (!IsCall(kFrustumCallSite, kFrustumFunc) || !IsCall(kPerspCallSite, kPerspFunc) ||
            !IsCall(kSetViewCallSite, kSetViewFunc) || !IsCall(kEndFrameCallSite, kEndFrameFunc) ||
            !IsCall(kPickCallSites[0], kPickFunc) || !IsCall(kPickCallSites[1], kPickFunc))
        {
            Log("head tracking: call sites not as expected, not patching");
            return false;
        }
        PatchCall(kFrustumCallSite, kFrustumFunc, &HookFrustum);
        PatchCall(kSetViewCallSite, kSetViewFunc, &HookSetView);
        for (DWORD site : kPickCallSites)
            PatchCall(site, kPickFunc, &HookPick);
        if (g_settings.stereo)
        {
            PatchCall(kPerspCallSite, kPerspFunc, &HookPerspective);
            PatchCall(kEndFrameCallSite, kEndFrameFunc, &HookEndFrame);
        }

        Log("head tracking: installed (%s, %s, scale %.1f, min eye distance %.1f, keys Ctrl+0x%02X toggle / Ctrl+0x%02X recentre)",
            g_settings.stereo ? "STEREO" : "mono parallax", g_settings.enabled ? "on" : "off", g_settings.scale,
            g_settings.minEyeDistance, g_settings.toggleKey, g_settings.recenterKey);
        return true;
    }
}
