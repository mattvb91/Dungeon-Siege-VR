#include "vr_pointer.h"

#include <cmath>
#include <cstring>

#include "common/log.h"
#include "common/stereo_abi.h"

namespace dsvr
{
    namespace
    {
        const DWORD kAppModulePtr = 0x0079CF7C;  // RapiAppModule singleton (FuBi accessor 0x4012BF)
        const DWORD kAppWindow    = 0x70;        // HWND used by the cursor update (0x414642: ScreenToClient)
        const float kMaxPitch     = 1.48f;       // ~85 degrees

        typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT);
        typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
        typedef BOOL(WINAPI* PeekMessageAFn)(LPMSG, HWND, UINT, UINT, UINT);
        typedef BOOL(WINAPI* GetMessageAFn)(LPMSG, HWND, UINT, UINT);

        GetCursorPosFn g_realGetCursorPos = nullptr;
        SetCursorPosFn g_realSetCursorPos = nullptr;
        PeekMessageAFn g_realPeekMessageA = nullptr;
        GetMessageAFn g_realGetMessageA = nullptr;

        PointerSettings g_settings;
        bool g_active = false;
        DWORD g_lastUpdate = 0;                       // UpdatePointer tick; frames without a world camera time out
        bool g_onPanel = true;
        float g_yaw = 0.0f, g_pitch = 0.0f;           // pointer, seated space
        float g_panelYaw = 0.0f, g_panelPitch = 0.0f;
        int g_cursorX = 0, g_cursorY = 0;             // game cursor, game pixels
        int g_width = 1024, g_height = 768;           // game render size (the image shown on the panel)
        LONG g_movedX = 0, g_movedY = 0;              // diagnostic: real mouse movement since the last log
        DWORD g_lastStateLog = 0;
        int g_stateLogs = 0;

        // Cursor coordinates: dgVoodoo virtualises the mouse for the game, so GetCursorPos / SetCursorPos /
        // ClientToScreen / mouse messages all work in the game's own resolution even when the real window is
        // larger (log: game 1024x768 in a 3840x2160 client). Window sizes from GetClientRect are NOT in that
        // space (using them sent the cursor off the virtual screen: the camera edge-rotated). So client pixels
        // are game pixels here.
        void GameToClient(int gx, int gy, int& cx, int& cy) { cx = gx; cy = gy; }
        void ClientToGame(int cx, int cy, int& gx, int& gy) { gx = cx; gy = cy; }

        HWND GameWindow()
        {
            DWORD app = *reinterpret_cast<const DWORD*>(kAppModulePtr);
            return app ? *reinterpret_cast<const HWND*>(app + kAppWindow) : nullptr;
        }

        struct V3 { float x, y, z; };

        // Rotation Ry(yaw) * Rx(pitch) (OpenVR seated space), applied to v / its transpose applied to v.
        V3 Rotate(float yaw, float pitch, V3 v)
        {
            float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
            return { cy * v.x + sy * sp * v.y + sy * cp * v.z,
                     cp * v.y - sp * v.z,
                     -sy * v.x + cy * sp * v.y + cy * cp * v.z };
        }

        V3 Unrotate(float yaw, float pitch, V3 v)
        {
            float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
            return { cy * v.x - sy * v.z,
                     sy * sp * v.x + cp * v.y + cy * sp * v.z,
                     sy * cp * v.x - sp * v.y + cy * cp * v.z };
        }

        V3 PointerVector() { return Rotate(g_yaw, g_pitch, { 0.0f, 0.0f, -1.0f }); }

        // Where the pointer ray from the seated origin meets the panel, in game pixels.
        bool HitPanel(int& x, int& y)
        {
            V3 local = Unrotate(g_panelYaw, g_panelPitch, PointerVector());
            if (local.z > -1e-3f)
                return false;
            float t = g_settings.panelDistance / -local.z;
            float u = local.x * t, v = local.y * t;
            float halfW = 0.5f * g_settings.panelWidth;
            float halfH = halfW * float(g_height) / float(g_width);
            if (std::fabs(u) > halfW || std::fabs(v) > halfH)
                return false;
            x = int((u / halfW * 0.5f + 0.5f) * float(g_width));
            y = int((0.5f - v / halfH * 0.5f) * float(g_height));
            x = x < 0 ? 0 : x >= g_width ? g_width - 1 : x;
            y = y < 0 ? 0 : y >= g_height ? g_height - 1 : y;
            return true;
        }

        // Aims the pointer at a panel pixel (the game moved its cursor).
        void PointAtPixel(int x, int y)
        {
            float halfW = 0.5f * g_settings.panelWidth;
            float halfH = halfW * float(g_height) / float(g_width);
            V3 local = { ((float(x) + 0.5f) / float(g_width) * 2.0f - 1.0f) * halfW,
                         (1.0f - (float(y) + 0.5f) / float(g_height) * 2.0f) * halfH,
                         -g_settings.panelDistance };
            V3 d = Rotate(g_panelYaw, g_panelPitch, local);
            float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            g_yaw = std::atan2(-d.x, -d.z);
            g_pitch = std::asin(d.y / len);
        }

        void PlaceGameCursor()
        {
            int x, y;
            g_onPanel = HitPanel(x, y);
            if (g_onPanel)
            {
                g_cursorX = x;
                g_cursorY = y;
            }
            else
            {
                // Parked away from the screen edges (edge contact rotates the camera); the add-on hides the
                // sprite there. The click camera is rotated so this pixel's ray follows the pointer.
                g_cursorX = int(kParkX * float(g_width));
                g_cursorY = int(kParkY * float(g_height));
            }
        }

        // Active only while frames keep updating it (loading screens and menus do not run the world camera).
        bool Live() { return g_active && GetTickCount() - g_lastUpdate < 250; }

        bool IsMouseMessage(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL; }

        BOOL WINAPI HookGetCursorPos(LPPOINT pt)
        {
            if (!Live() || !pt)
                return g_realGetCursorPos(pt);
            int cx, cy;
            GameToClient(g_cursorX, g_cursorY, cx, cy);
            POINT p = { cx, cy };
            ClientToScreen(GameWindow(), &p);
            *pt = p;
            return TRUE;
        }

        BOOL WINAPI HookSetCursorPos(int x, int y)
        {
            if (!Live())
                return g_realSetCursorPos(x, y);
            POINT p = { x, y };
            ScreenToClient(GameWindow(), &p);
            int gx, gy;
            ClientToGame(p.x, p.y, gx, gy);
            if (gx >= 0 && gy >= 0 && gx < g_width && gy < g_height)
            {
                PointAtPixel(gx, gy);
                PlaceGameCursor();
            }
            return TRUE;
        }

        void RewriteMouse(LPMSG msg)
        {
            if (Live() && msg && IsMouseMessage(msg->message))
            {
                int cx, cy;
                GameToClient(g_cursorX, g_cursorY, cx, cy);
                msg->lParam = MAKELPARAM(cx, cy);
            }
        }

        BOOL WINAPI HookPeekMessageA(LPMSG msg, HWND hwnd, UINT first, UINT last, UINT remove)
        {
            BOOL got = g_realPeekMessageA(msg, hwnd, first, last, remove);
            if (got)
                RewriteMouse(msg);
            return got;
        }

        BOOL WINAPI HookGetMessageA(LPMSG msg, HWND hwnd, UINT first, UINT last)
        {
            BOOL got = g_realGetMessageA(msg, hwnd, first, last);
            if (got > 0)
                RewriteMouse(msg);
            return got;
        }

        // Replaces an entry in DungeonSiege.exe's import address table; returns the original function.
        void* PatchImport(const char* dll, const char* function, void* hook)
        {
            BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
            IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            for (IMAGE_IMPORT_DESCRIPTOR* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
            {
                if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0)
                    continue;
                IMAGE_THUNK_DATA* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
                IMAGE_THUNK_DATA* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                        continue;
                    const IMAGE_IMPORT_BY_NAME* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0)
                        continue;
                    void* original = reinterpret_cast<void*>(slots->u1.Function);
                    DWORD protect;
                    VirtualProtect(&slots->u1.Function, sizeof(DWORD), PAGE_READWRITE, &protect);
                    slots->u1.Function = reinterpret_cast<DWORD>(hook);
                    VirtualProtect(&slots->u1.Function, sizeof(DWORD), protect, &protect);
                    return original;
                }
            }
            return nullptr;
        }
    }

    bool InstallPointer(const PointerSettings& settings)
    {
        g_settings = settings;
        g_realGetCursorPos = reinterpret_cast<GetCursorPosFn>(PatchImport("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&HookGetCursorPos)));
        g_realSetCursorPos = reinterpret_cast<SetCursorPosFn>(PatchImport("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&HookSetCursorPos)));
        g_realPeekMessageA = reinterpret_cast<PeekMessageAFn>(PatchImport("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&HookPeekMessageA)));
        g_realGetMessageA = reinterpret_cast<GetMessageAFn>(PatchImport("USER32.dll", "GetMessageA", reinterpret_cast<void*>(&HookGetMessageA)));
        bool ok = g_realGetCursorPos && g_realSetCursorPos && g_realPeekMessageA && g_realGetMessageA;
        Log("vr pointer: %s (%.3f deg per pixel)", ok ? "mouse input redirected" : "import patching FAILED", settings.degreesPerPixel);
        return ok;
    }

    void UpdatePointer(bool active, float panelYaw, float panelPitch, int gameWidth, int gameHeight)
    {
        HWND hwnd = GameWindow();
        if (!active || !hwnd || !g_realGetCursorPos)
        {
            g_active = false;
            return;
        }

        if (gameWidth > 0 && gameHeight > 0)
        {
            g_width = gameWidth;
            g_height = gameHeight;
        }
        g_panelYaw = panelYaw;
        g_lastUpdate = GetTickCount();
        g_panelPitch = panelPitch;

        if (!g_active)
        {
            // Start on the panel centre.
            g_yaw = panelYaw;
            g_pitch = panelPitch;
            g_active = true;
            Log("vr pointer: active (game %dx%d)", g_width, g_height);
        }

        // Mouse movement turns the pointer; the real cursor is held at the window centre.
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId())
        {
            POINT centre = { g_width / 2, g_height / 2 };
            ClientToScreen(hwnd, &centre);
            POINT real;
            if (g_realGetCursorPos(&real))
            {
                const float kDegToRad = 0.0174532925f;
                float k = g_settings.degreesPerPixel * kDegToRad;
                g_movedX += real.x - centre.x;
                g_movedY += real.y - centre.y;
                g_yaw -= float(real.x - centre.x) * k;
                g_pitch -= float(real.y - centre.y) * k;
                g_pitch = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, g_pitch));
                g_realSetCursorPos(centre.x, centre.y);
            }
        }
        PlaceGameCursor();

        // Diagnostic: pointer state every 2 s for the first two minutes of each session.
        DWORD now = GetTickCount();
        if (now - g_lastStateLog >= 2000 && g_stateLogs < 60)
        {
            const float kRadToDeg = 57.2957795f;
            Log("vr pointer: yaw %.1f pitch %.1f deg, %s, game cursor %d,%d, mouse moved %ld,%ld, panel yaw %.1f pitch %.1f",
                g_yaw * kRadToDeg, g_pitch * kRadToDeg, g_onPanel ? "on panel" : "OFF panel", g_cursorX, g_cursorY,
                g_movedX, g_movedY, g_panelYaw * kRadToDeg, g_panelPitch * kRadToDeg);
            g_movedX = g_movedY = 0;
            g_lastStateLog = now;
            ++g_stateLogs;
        }
    }

    bool PointerActive() { return Live(); }
    bool PointerOnPanel() { return g_onPanel; }

    void GetPointerDirection(float* yaw, float* pitch)
    {
        *yaw = g_yaw;
        *pitch = g_pitch;
    }

    void GetGameCursor(int* x, int* y, int* width, int* height)
    {
        *x = g_cursorX;
        *y = g_cursorY;
        *width = g_width;
        *height = g_height;
    }
}
