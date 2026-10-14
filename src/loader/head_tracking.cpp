#include "head_tracking.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "common/log.h"

namespace dsvr
{
    namespace
    {
        const DWORD kSetViewCallSite = 0x00661A49;  // call Rapi::SetViewLookAt inside SiegeCamera 0x66192A
        const DWORD kSetViewFunc     = 0x00653E96;  // thiscall (pos*, target*, up*), ret 0Ch
        const DWORD kSiegeEnginePtr  = 0x007A05FC;
        const DWORD kEngineCamera    = 0x3C;
        const DWORD kCameraUpVector  = 0x160;       // SiegeCamera::m_UpVector

        // Game units are metres; amplify head motion so leaning gives a useful orbit.
        const float kHeadScale = 6.0f;
        // Never bring the eye closer to the target than this (metres) or past it.
        const float kMinEyeDistance = 1.5f;

        struct Vec3 { float x, y, z; };

        Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        Vec3 Mul(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

        // __fastcall with a dummy edx matches MSVC thiscall for a callee-cleaned member function.
        typedef void(__fastcall* SetViewFn)(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up);
        typedef bool(*GetHeadPositionFn)(float out[3]);

        const SetViewFn g_originalSetView = reinterpret_cast<SetViewFn>(kSetViewFunc);
        GetHeadPositionFn g_getHeadPosition = nullptr;
        unsigned g_trackedFrames = 0;

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

        bool IsMainCamera(const Vec3* up)
        {
            DWORD engine = *reinterpret_cast<const DWORD*>(kSiegeEnginePtr);
            if (!engine)
                return false;
            DWORD camera = *reinterpret_cast<const DWORD*>(engine + kEngineCamera);
            return camera && reinterpret_cast<DWORD>(up) == camera + kCameraUpVector;
        }

        void __fastcall HookSetView(void* rapi, void* edx, const Vec3* pos, const Vec3* target, const Vec3* up)
        {
            float head[3];
            if (!IsMainCamera(up) || !GetHeadPosition(head))
            {
                g_originalSetView(rapi, edx, pos, target, up);
                return;
            }

            // Camera basis (D3D left-handed look-at): forward towards the target, right = up x forward.
            Vec3 toTarget = Sub(*target, *pos);
            float distance = Length(toTarget);
            if (distance < 1e-3f)
            {
                g_originalSetView(rapi, edx, pos, target, up);
                return;
            }
            Vec3 forward = Mul(toTarget, 1.0f / distance);
            Vec3 right = Cross(*up, forward);
            float rightLen = Length(right);
            if (rightLen < 1e-3f)
            {
                g_originalSetView(rapi, edx, pos, target, up);
                return;
            }
            right = Mul(right, 1.0f / rightLen);
            Vec3 camUp = Cross(forward, right);

            // OpenVR seated space: +x right, +y up, -z forward (towards the virtual screen).
            Vec3 offset = Add(Add(Mul(right, head[0] * kHeadScale), Mul(camUp, head[1] * kHeadScale)),
                              Mul(forward, -head[2] * kHeadScale));

            // Keep the eye in front of the target by at least kMinEyeDistance along the view direction.
            float along = Dot(offset, forward);
            float maxAlong = distance - kMinEyeDistance;
            if (along > maxAlong)
                offset = Add(offset, Mul(forward, maxAlong - along));

            Vec3 eye = Add(*pos, offset);
            g_originalSetView(rapi, edx, &eye, target, up);

            if (g_trackedFrames++ == 0)
                Log("head tracking: first tracked frame (head %.3f %.3f %.3f m, eye offset %.2f %.2f %.2f)",
                    head[0], head[1], head[2], offset.x, offset.y, offset.z);
        }
    }

    bool InstallHeadTracking()
    {
        BYTE* site = reinterpret_cast<BYTE*>(kSetViewCallSite);
        LONG rel;
        memcpy(&rel, site + 1, sizeof(rel));
        if (site[0] != 0xE8 || kSetViewCallSite + 5 + rel != kSetViewFunc)
        {
            Log("head tracking: unexpected bytes at 0x%08lX, not patching", kSetViewCallSite);
            return false;
        }

        DWORD oldProtect;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        LONG newRel = static_cast<LONG>(reinterpret_cast<DWORD>(&HookSetView) - (kSetViewCallSite + 5));
        memcpy(site + 1, &newRel, sizeof(newRel));
        VirtualProtect(site, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 5);

        Log("head tracking: patched call at 0x%08lX (scale %.1f)", kSetViewCallSite, kHeadScale);
        return true;
    }
}
