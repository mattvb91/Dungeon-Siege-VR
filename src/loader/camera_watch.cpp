#include "camera_watch.h"

#include <tlhelp32.h>

#include "common/log.h"

namespace dsvr
{
    namespace
    {
        // Watched SiegeCamera fields (first dword of each).
        struct Watch { DWORD offset; const char* name; };
        const Watch kWatches[] = {
            { 0x148, "camera pos" },
            { 0x178, "matrix" },
            { 0x19C, "quat" },
        };
        const int kNumWatches = sizeof(kWatches) / sizeof(kWatches[0]);

        // DungeonSiege.exe .text range: return addresses inside it are worth logging.
        const DWORD kTextBegin = 0x00401000;
        const DWORD kTextEnd   = 0x00720000;

        const int kMaxSites = 32;
        const int kStackRefs = 6;

        struct Site
        {
            volatile LONG used;
            DWORD eip;          // instruction after the write (data breakpoints trap after execution)
            DWORD threadId;
            int watch;
            volatile LONG hits;
            DWORD stack[kStackRefs];
        };

        Site g_sites[kMaxSites];
        volatile LONG g_armed = 0;
        PVOID g_handler = nullptr;

        void RecordHit(int watch, const CONTEXT* ctx)
        {
            DWORD eip = ctx->Eip;
            for (int i = 0; i < kMaxSites; ++i)
            {
                Site& s = g_sites[i];
                if (s.used && s.eip == eip && s.watch == watch)
                {
                    InterlockedIncrement(&s.hits);
                    return;
                }
                if (InterlockedCompareExchange(&s.used, 1, 0) == 0)
                {
                    s.eip = eip;
                    s.watch = watch;
                    s.threadId = GetCurrentThreadId();
                    // Scan the stack for return addresses into the game's code (no frame pointers assumed).
                    const DWORD* sp = reinterpret_cast<const DWORD*>(ctx->Esp);
                    int found = 0;
                    for (int k = 0; k < 256 && found < kStackRefs; ++k)
                    {
                        DWORD v = sp[k];
                        if (v >= kTextBegin && v < kTextEnd)
                            s.stack[found++] = v;
                    }
                    InterlockedIncrement(&s.hits);
                    return;
                }
            }
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_armed)
                return EXCEPTION_CONTINUE_SEARCH;

            CONTEXT* ctx = info->ContextRecord;
            DWORD dr6 = ctx->Dr6;
            if ((dr6 & 0x7) == 0)
                return EXCEPTION_CONTINUE_SEARCH;  // not ours

            for (int i = 0; i < kNumWatches; ++i)
            {
                if (dr6 & (1u << i))
                    RecordHit(i, ctx);
            }
            ctx->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Applies (or clears, when camera == 0) the watchpoints on all other threads in this process.
        int SetWatchpoints(DWORD camera)
        {
            DWORD self = GetCurrentThreadId();
            DWORD pid = GetCurrentProcessId();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return 0;

            int count = 0;
            THREADENTRY32 te = { sizeof(te) };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                    continue;
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!thread)
                    continue;

                if (SuspendThread(thread) != (DWORD)-1)
                {
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &ctx))
                    {
                        DWORD dr7 = ctx.Dr7 & ~0x0FFF00FFu;  // clear L/G enables and RW/LEN for DR0-DR3
                        if (camera)
                        {
                            ctx.Dr0 = camera + kWatches[0].offset;
                            ctx.Dr1 = camera + kWatches[1].offset;
                            ctx.Dr2 = camera + kWatches[2].offset;
                            for (int i = 0; i < kNumWatches; ++i)
                                dr7 |= (1u << (i * 2)) | (0xDu << (16 + i * 4));  // local enable, RW=write, LEN=4 bytes
                        }
                        ctx.Dr7 = dr7;
                        ctx.Dr6 = 0;
                        if (SetThreadContext(thread, &ctx))
                            ++count;
                    }
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            }
            CloseHandle(snap);
            return count;
        }
    }

    void WatchCameraWrites(DWORD camera, DWORD durationMs)
    {
        if (!g_handler)
            g_handler = AddVectoredExceptionHandler(1, &OnException);

        InterlockedExchange(&g_armed, 1);
        int threads = SetWatchpoints(camera);
        Log("camera watch armed on %d threads for %lu ms (camera 0x%08lX)", threads, durationMs, camera);

        Sleep(durationMs);

        SetWatchpoints(0);
        InterlockedExchange(&g_armed, 0);
        Log("camera watch disarmed; write sites:");

        for (int i = 0; i < kMaxSites; ++i)
        {
            const Site& s = g_sites[i];
            if (!s.used)
                continue;
            Log("  %-10s after 0x%08lX  hits %5ld  thread %5lu  stack %08lX %08lX %08lX %08lX %08lX %08lX",
                kWatches[s.watch].name, s.eip, s.hits, s.threadId,
                s.stack[0], s.stack[1], s.stack[2], s.stack[3], s.stack[4], s.stack[5]);
        }
        ZeroMemory(g_sites, sizeof(g_sites));
    }
}
