// dsvr — Dungeon Siege VR loader.
// Built as version.dll and placed next to DungeonSiege.exe, which imports VERSION.dll.
// All real exports are forwarded to the system version.dll via jmp stubs.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

#include "common/log.h"
#include "camera_probe.h"
#include "head_tracking.h"

// ---------------------------------------------------------------------------
// version.dll forwarding

#define DSVR_VERSION_EXPORTS(X)        \
    X(0,  GetFileVersionInfoA)         \
    X(1,  GetFileVersionInfoByHandle)  \
    X(2,  GetFileVersionInfoExA)       \
    X(3,  GetFileVersionInfoExW)       \
    X(4,  GetFileVersionInfoSizeA)     \
    X(5,  GetFileVersionInfoSizeExA)   \
    X(6,  GetFileVersionInfoSizeExW)   \
    X(7,  GetFileVersionInfoSizeW)     \
    X(8,  GetFileVersionInfoW)         \
    X(9,  VerFindFileA)                \
    X(10, VerFindFileW)                \
    X(11, VerInstallFileA)             \
    X(12, VerInstallFileW)             \
    X(13, VerLanguageNameA)            \
    X(14, VerLanguageNameW)            \
    X(15, VerQueryValueA)              \
    X(16, VerQueryValueW)

static const int kNumExports = 17;
static FARPROC g_real[kNumExports];

#define DSVR_STUB(i, name) \
    extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_real + (i) * 4] } }
DSVR_VERSION_EXPORTS(DSVR_STUB)
#undef DSVR_STUB

static bool LoadRealVersionDll()
{
    char path[MAX_PATH];
    UINT len = GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for a 32-bit process
    if (len == 0 || len > MAX_PATH - 16)
        return false;
    strcat_s(path, "\\version.dll");

    HMODULE real = LoadLibraryA(path);
    if (!real)
    {
        dsvr::Log("failed to load %s (error %lu)", path, GetLastError());
        return false;
    }

#define DSVR_RESOLVE(i, name) g_real[i] = GetProcAddress(real, #name);
    DSVR_VERSION_EXPORTS(DSVR_RESOLVE)
#undef DSVR_RESOLVE

    dsvr::Log("forwarding to %s", path);
    return true;
}

// ---------------------------------------------------------------------------
// Target validation

// PE TimeDateStamp of the Steam v1.11.1.1486 build analysed in docs/EXE-ANALYSIS.md.
static const DWORD kExpectedExeTimestamp = 0x4D2E4A5A;

static bool CheckTargetExe()
{
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    dsvr::Log("host: %s (base 0x%p, timestamp 0x%08lX)", exe, base, nt->FileHeader.TimeDateStamp);

    if (nt->FileHeader.TimeDateStamp != kExpectedExeTimestamp)
    {
        dsvr::Log("unsupported exe build (expected timestamp 0x%08lX); VR features disabled", kExpectedExeTimestamp);
        return false;
    }

    // FuBi exports give us named engine entry points. Resolve one to prove the path works.
    FARPROC engineSingleton = GetProcAddress(reinterpret_cast<HMODULE>(base),
        "?FUBI_GetClassSingleton@SiegeEngine@siege@@CAPAV12@XZ");
    dsvr::Log("SiegeEngine singleton accessor: 0x%p (expected 0x0047D93C)", engineSingleton);
    return engineSingleton != nullptr;
}

// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        dsvr::OpenLog(module, "dsvr.log");
        dsvr::Log("dsvr attach");

        if (!LoadRealVersionDll())
            return FALSE;

        if (CheckTargetExe())
        {
            dsvr::InstallHeadTracking();
            dsvr::StartCameraProbe();
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        dsvr::Log("dsvr detach");
        dsvr::CloseLog();
    }
    return TRUE;
}
