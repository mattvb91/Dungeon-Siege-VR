#include "head_tracking.h"

#include <cmath>
#include <cstring>

#include "common/config.h"
#include "common/log.h"

namespace dsvr
{
    namespace
    {
        const DWORD kFrustumCallSite = 0x006619EB;  // call SiegeCamera frustum setup inside 0x66192A
        const DWORD kFrustumFunc     = 0x006799C1;  // thiscall(camera; w, h, fov, near, far, pos*, matrix3x3*), ret 1Ch
        const DWORD kSetViewCallSite = 0x00661A49;  // call Rapi::SetViewLookAt inside 0x66192A
        const DWORD kSetViewFunc     = 0x00653E96;  // thiscall(rapi; pos*, target*, up*), ret 0Ch

        const DWORD kSiegeEnginePtr  = 0x007A05FC;
        const DWORD kEngineCamera    = 0x3C;
        const DWORD kCameraPosition  = 0x148;       // world-space eye, recomputed each frame
        const DWORD kTargetPosition  = 0x154;       // world-space target
        const DWORD kCameraUpVector  = 0x160;       // m_UpVector

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
        typedef void(__fastcall* SetViewFn)(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up);
        typedef bool(*GetHeadPositionFn)(float out[3]);

        const FrustumFn g_originalFrustum = reinterpret_cast<FrustumFn>(kFrustumFunc);
        const SetViewFn g_originalSetView = reinterpret_cast<SetViewFn>(kSetViewFunc);

        struct Settings
        {
            bool enabled;
            float scale;
            float minEyeDistance;
            int toggleKey;
            int recenterKey;
        };
        Settings g_settings;

        GetHeadPositionFn g_getHeadPosition = nullptr;
        float g_baseline[3] = {};      // head position treated as neutral (Ctrl+recenter)
        bool g_toggleDown = false;
        bool g_recenterDown = false;
        unsigned g_trackedFrames = 0;

        // Eye computed by the frustum hook, reused by the view hook in the same frame.
        DWORD g_eyeCamera = 0;
        Vec3 g_eye;

        DWORD MainCamera()
        {
            DWORD engine = *reinterpret_cast<const DWORD*>(kSiegeEnginePtr);
            return engine ? *reinterpret_cast<const DWORD*>(engine + kEngineCamera) : 0;
        }

        bool GetHeadPosition(float out[3])
        {
            if (!g_getHeadPosition)
            {
                // SampleAddon.dll is loaded by dgVoodoo and pinned, so the pointer stays valid once found.
                HMODULE addon = GetModuleHandleA("SampleAddon.dll");
                if (!addon)
                    return false;
                g_getHeadPosition = reinterpret_cast<GetHeadPositionFn>(GetProcAddress(addon, "DsvrGetHeadPosition"));
                if (!g_getHeadPosition)
                    return false;
                Log("head tracking: connected to SampleAddon.dll");
            }
            return g_getHeadPosition(out);
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
                if (GetHeadPosition(head))
                {
                    memcpy(g_baseline, head, sizeof(g_baseline));
                    Log("head tracking: recentred at %.3f %.3f %.3f m", head[0], head[1], head[2]);
                }
            }
        }

        // Head-shifted eye for a camera looking from `pos` at `target`. False = leave the camera alone.
        bool ComputeEye(const Vec3& pos, const Vec3& target, const Vec3& up, Vec3& eye)
        {
            float head[3];
            if (!g_settings.enabled || !GetHeadPosition(head))
                return false;

            Vec3 toTarget = Sub(target, pos);
            float distance = Length(toTarget);
            if (distance < 1e-3f)
                return false;
            Vec3 forward = Mul(toTarget, 1.0f / distance);
            Vec3 right = Cross(up, forward);  // D3D left-handed look-at basis
            float rightLen = Length(right);
            if (rightLen < 1e-3f)
                return false;
            right = Mul(right, 1.0f / rightLen);
            Vec3 camUp = Cross(forward, right);

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

        // SiegeCamera orientation layout (m_MatrixOrientation): columns are right, up, forward.
        void LookAtMatrix(const Vec3& eye, const Vec3& target, const Vec3& up, float m[9])
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

        void __fastcall HookFrustum(void* camera, void* edx, int width, int height, float fov,
                                    float nearZ, float farZ, const Vec3* pos, const float* matrix)
        {
            DWORD cam = reinterpret_cast<DWORD>(camera);
            g_eyeCamera = 0;
            if (cam == MainCamera())
            {
                PollHotkeys();
                const Vec3& target = *reinterpret_cast<const Vec3*>(cam + kTargetPosition);
                const Vec3& up = *reinterpret_cast<const Vec3*>(cam + kCameraUpVector);
                if (ComputeEye(*pos, target, up, g_eye))
                {
                    g_eyeCamera = cam;
                    float shifted[9];
                    LookAtMatrix(g_eye, target, up, shifted);
                    g_originalFrustum(camera, edx, width, height, fov, nearZ, farZ, &g_eye, shifted);
                    return;
                }
            }
            g_originalFrustum(camera, edx, width, height, fov, nearZ, farZ, pos, matrix);
        }

        void __fastcall HookSetView(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up)
        {
            // The frustum hook ran earlier in the same SiegeCamera::0x66192A call for this camera.
            DWORD cam = reinterpret_cast<DWORD>(up) - kCameraUpVector;
            if (g_eyeCamera && cam == g_eyeCamera && reinterpret_cast<DWORD>(pos) == cam + kCameraPosition)
            {
                g_eyeCamera = 0;
                g_originalSetView(rapi, edx, &g_eye, target, up);
                return;
            }
            g_originalSetView(rapi, edx, pos, target, up);
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
    }

    bool InstallHeadTracking(HMODULE module)
    {
        Config config(module);
        g_settings.enabled        = config.Bool("HeadTracking", "Enabled", true);
        g_settings.scale          = config.Float("HeadTracking", "Scale", 6.0f);
        g_settings.minEyeDistance = config.Float("HeadTracking", "MinEyeDistance", 1.5f);
        g_settings.toggleKey      = config.Int("HeadTracking", "ToggleKey", VK_F9);
        g_settings.recenterKey    = config.Int("HeadTracking", "RecenterKey", VK_F10);

        // Verify both sites before touching either.
        BYTE* f = reinterpret_cast<BYTE*>(kFrustumCallSite);
        BYTE* v = reinterpret_cast<BYTE*>(kSetViewCallSite);
        if (f[0] != 0xE8 || v[0] != 0xE8)
        {
            Log("head tracking: call sites not found, not patching");
            return false;
        }
        if (!PatchCall(kSetViewCallSite, kSetViewFunc, &HookSetView) ||
            !PatchCall(kFrustumCallSite, kFrustumFunc, &HookFrustum))
            return false;

        Log("head tracking: patched 0x%08lX and 0x%08lX (%s, scale %.1f, min eye distance %.1f, keys Ctrl+0x%02X toggle / Ctrl+0x%02X recentre)",
            kFrustumCallSite, kSetViewCallSite, g_settings.enabled ? "on" : "off", g_settings.scale,
            g_settings.minEyeDistance, g_settings.toggleKey, g_settings.recenterKey);
        return true;
    }
}
