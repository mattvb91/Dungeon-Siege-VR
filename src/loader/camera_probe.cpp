#include "camera_probe.h"

#include <windows.h>
#include <cstring>

#include "common/log.h"
#include "camera_watch.h"

namespace dsvr
{
    namespace
    {
        // Globals read by the FuBi singleton accessors (e.g. 0x47D93C: mov eax,[0x7A05FC]; ret).
        const DWORD kSiegeEnginePtr = 0x007A05FC;
        const DWORD kCameraAgentPtr = 0x007A05BC;
        const DWORD kUICameraPtr    = 0x007A0600;

        // siege::SiegeEngine::GetCamera: mov eax,[ecx+3Ch]
        const DWORD kEngineCamera = 0x3C;

        // siege::SiegeCamera field offsets, from accessor bytes.
        const DWORD kViewportNormW = 0x11C;
        const DWORD kViewportNormH = 0x120;
        const DWORD kFieldOfView   = 0x124;
        const DWORD kCameraSiegePos = 0x128;  // vector_3 + node guid
        const DWORD kTargetSiegePos = 0x138;  // vector_3 + node guid
        const DWORD kCameraPosition = 0x148;  // vector_3
        const DWORD kTargetPosition = 0x154;  // vector_3
        const DWORD kUpVector       = 0x160;  // vector_3
        const DWORD kVectorOrient   = 0x16C;  // vector_3
        const DWORD kMatrixOrient   = 0x178;  // matrix_3x3
        const DWORD kQuatOrient     = 0x19C;  // Quat
        const DWORD kMaxAzimuth     = 0x1AC;
        const DWORD kMinAzimuth     = 0x1B0;
        const DWORD kMaxDistance    = 0x1B4;
        const DWORD kCameraSize     = 0x1B8;

        const DWORD kIntervalMs = 2000;
        const int kMaxSamples = 150;  // ~5 minutes

        const int kWatchRounds = 3;
        const DWORD kWatchMs = 8000;
        const DWORD kWatchGapMs = 10000;

        // Copies game memory, tolerating bad pointers (the engine may be mid-construction).
        bool SafeRead(DWORD address, void* out, size_t size)
        {
            __try
            {
                memcpy(out, reinterpret_cast<const void*>(address), size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        DWORD ReadPtr(DWORD address)
        {
            DWORD value = 0;
            return SafeRead(address, &value, sizeof(value)) ? value : 0;
        }

        float F(const BYTE* cam, DWORD offset)
        {
            float f;
            memcpy(&f, cam + offset, sizeof(f));
            return f;
        }

        DWORD U(const BYTE* cam, DWORD offset)
        {
            DWORD u;
            memcpy(&u, cam + offset, sizeof(u));
            return u;
        }

        void LogVec3(const char* name, const BYTE* cam, DWORD offset)
        {
            Log("  %-14s (+0x%03lX) % 10.3f % 10.3f % 10.3f", name, offset, F(cam, offset), F(cam, offset + 4), F(cam, offset + 8));
        }

        void LogSample(int index, DWORD engine, DWORD camera, const BYTE* cam)
        {
            const float kRadToDeg = 57.2957795f;
            Log("camera sample %d: engine 0x%08lX camera 0x%08lX agent 0x%08lX uicamera 0x%08lX",
                index, engine, camera, ReadPtr(kCameraAgentPtr), ReadPtr(kUICameraPtr));
            Log("  fov            (+0x%03lX) %.4f rad = %.1f deg", kFieldOfView, F(cam, kFieldOfView), F(cam, kFieldOfView) * kRadToDeg);
            Log("  viewport norm  (+0x%03lX) %.3f x %.3f", kViewportNormW, F(cam, kViewportNormW), F(cam, kViewportNormH));
            Log("  camera siegepos(+0x%03lX) % 10.3f % 10.3f % 10.3f node 0x%08lX", kCameraSiegePos,
                F(cam, kCameraSiegePos), F(cam, kCameraSiegePos + 4), F(cam, kCameraSiegePos + 8), U(cam, kCameraSiegePos + 12));
            Log("  target siegepos(+0x%03lX) % 10.3f % 10.3f % 10.3f node 0x%08lX", kTargetSiegePos,
                F(cam, kTargetSiegePos), F(cam, kTargetSiegePos + 4), F(cam, kTargetSiegePos + 8), U(cam, kTargetSiegePos + 12));
            LogVec3("camera pos", cam, kCameraPosition);
            LogVec3("target pos", cam, kTargetPosition);
            LogVec3("up", cam, kUpVector);
            LogVec3("vector orient", cam, kVectorOrient);
            LogVec3("matrix row0", cam, kMatrixOrient);
            LogVec3("matrix row1", cam, kMatrixOrient + 12);
            LogVec3("matrix row2", cam, kMatrixOrient + 24);
            Log("  quat           (+0x%03lX) % 8.4f % 8.4f % 8.4f % 8.4f", kQuatOrient,
                F(cam, kQuatOrient), F(cam, kQuatOrient + 4), F(cam, kQuatOrient + 8), F(cam, kQuatOrient + 12));
            // Stored in degrees (live values 32 / 85): these are the camera tilt limits.
            Log("  tilt min/max %.1f / %.1f deg, max distance %.2f",
                F(cam, kMinAzimuth), F(cam, kMaxAzimuth), F(cam, kMaxDistance));
        }

        DWORD WINAPI ProbeThread(void*)
        {
            Log("camera probe started (every %lu ms, max %d samples)", kIntervalMs, kMaxSamples);
            BYTE cam[kCameraSize];
            BYTE last[kCameraSize] = {};
            int samples = 0;
            int watchRounds = 0;
            DWORD lastWatch = GetTickCount() - kWatchGapMs;

            while (samples < kMaxSamples)
            {
                Sleep(kIntervalMs);

                DWORD engine = ReadPtr(kSiegeEnginePtr);
                DWORD camera = engine ? ReadPtr(engine + kEngineCamera) : 0;
                if (!camera || !SafeRead(camera, cam, sizeof(cam)))
                    continue;

                // Only log when something moved, so the log follows gameplay.
                if (memcmp(cam + kFieldOfView, last + kFieldOfView, kCameraSize - kFieldOfView) == 0)
                    continue;
                memcpy(last, cam, sizeof(cam));

                LogSample(++samples, engine, camera, cam);

                // Once in-game (target sits on a real terrain node), find who writes the camera.
                if (watchRounds < kWatchRounds && U(cam, kTargetSiegePos + 12) != 0 &&
                    GetTickCount() - lastWatch >= kWatchGapMs)
                {
                    ++watchRounds;
                    Log("camera watch round %d of %d", watchRounds, kWatchRounds);
                    WatchCameraWrites(camera, kWatchMs);
                    lastWatch = GetTickCount();
                }
            }
            Log("camera probe finished");
            return 0;
        }
    }

    void StartCameraProbe()
    {
        HANDLE thread = CreateThread(nullptr, 0, &ProbeThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
}
