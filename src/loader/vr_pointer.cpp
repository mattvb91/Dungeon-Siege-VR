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
        // The game runs the mouse in relative mode: each frame (0x414642) it takes GetCursorPos (client
        // coordinates) minus an anchor as the movement, moves its own cursor by it, and re-centres the OS
        // cursor with SetCursorPos (0x41592A, 0x416EB4). Its cursor is what it draws and picks with.
        const DWORD kAppCursorX   = 0x12C;       // AppModule::GetCursorX (0x47DD14)
        const DWORD kAppCursorY   = 0x130;       // AppModule::GetCursorY (0x47DD28)
        const DWORD kAppAnchorX   = 0x134;       // client point the movement is measured from (relative mode)
        const DWORD kAppAnchorY   = 0x138;
        const DWORD kAppWindowLeft = 0xC0, kAppWindowTop = 0xC4, kAppWindowRight = 0xC8, kAppWindowBottom = 0xCC;
        // Inputs of 0x4158F7, which decides relative mode (see RelativeMode).
        const DWORD kAppFlags48    = 0x48;
        const DWORD kAppFlags4C    = 0x4C;
        const DWORD kAppCaptured   = 0x74;       // byte
        const DWORD kAppCursorShow = 0x78;
        const DWORD kAppButtons    = 0x10C;      // word; 0xF80 = mouse buttons held
        // The cursor is drawn by the Rapi mouse object's own thread (0x656E02): it copies its position
        // (+0x114/+0x118), runs the app's cursor update on the copy (vfunc 0x1C -> 0x414642, so GetCursorPos is
        // called from that thread too), stores the result and draws the cursor there.
        const DWORD kRapiMousePtr  = 0x007A0680;
        const DWORD kMousePosX     = 0x114;
        const DWORD kMousePosY     = 0x118;
        // Mouse sensitivity (floats, set from the options at 0x4130D8): the cursor update multiplies movement
        // by one of these pairs (0x4146EE) before moving the cursor and emitting the movement as input events
        // (which drive UIShell hover/clicks via 0x6E0C74/0x6E0C83 and camera rotation).
        const DWORD kAppSensitivity[4] = { 0x17C, 0x180, 0x188, 0x18C };
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
        DWORD g_mainThread = 0;                       // game thread (calls UpdatePointer)
        volatile LONG g_hideDrawnCursor = 0;          // current frame is an eye frame
        DWORD g_lastUpdate = 0;                       // UpdatePointer tick; frames without a world camera time out
        bool g_onPanel = true;
        float g_yaw = 0.0f, g_pitch = 0.0f;           // pointer, seated space
        float g_panelYaw = 0.0f, g_panelPitch = 0.0f;
        int g_cursorX = 0, g_cursorY = 0;             // where the game's cursor should be, game pixels
        int g_width = 1024, g_height = 768;           // game render size (the image shown on the panel)
        LONG g_movedX = 0, g_movedY = 0;              // diagnostic: real mouse movement since the last log
        DWORD g_lastStateLog = 0;
        int g_stateLogs = 0;

        // Cursor coordinates: dgVoodoo virtualises the mouse for the game, so GetCursorPos / SetCursorPos /
        // ClientToScreen / mouse messages all work in the game's own resolution even when the real window is
        // larger (log: game 1024x768 in a 3840x2160 client). Window sizes from GetClientRect are NOT in that
        // space (using them sent the cursor off the virtual screen: the camera edge-rotated). So client pixels
        // are game pixels throughout this file.

        DWORD AppModule() { return *reinterpret_cast<const DWORD*>(kAppModulePtr); }

        HWND GameWindow()
        {
            DWORD app = AppModule();
            return app ? *reinterpret_cast<const HWND*>(app + kAppWindow) : nullptr;
        }

        int AppInt(DWORD offset)
        {
            DWORD app = AppModule();
            return app ? *reinterpret_cast<const int*>(app + offset) : 0;
        }

        // The player's sensitivity while the pointer has it set to 1 (see NeutralSensitivity).
        float g_userSensitivity[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool g_sensitivityNeutral = false;

        // Pointer moves: sensitivity 1, so the movement we report is exactly the movement the cursor makes and
        // the game emits (other code reads the movement events, e.g. UIShell's hover and clicks).
        void NeutralSensitivity()
        {
            DWORD app = AppModule();
            if (!app)
                return;
            for (int i = 0; i < 4; ++i)
            {
                float* s = reinterpret_cast<float*>(app + kAppSensitivity[i]);
                if (*s != 1.0f)
                {
                    g_userSensitivity[i] = *s;  // also picks up changes made in the options meanwhile
                    *s = 1.0f;
                }
            }
            g_sensitivityNeutral = true;
        }

        // Middle-drag camera rotation, or pointer inactive: the player's own sensitivity.
        void RestoreSensitivity()
        {
            DWORD app = AppModule();
            if (!app || !g_sensitivityNeutral)
                return;
            for (int i = 0; i < 4; ++i)
                *reinterpret_cast<float*>(app + kAppSensitivity[i]) = g_userSensitivity[i];
            g_sensitivityNeutral = false;
        }

        void SetAppInt(DWORD offset, int value)
        {
            DWORD app = AppModule();
            if (app)
                *reinterpret_cast<int*>(app + offset) = value;
        }

        // Mirror of the test in 0x4158F7: the cursor update measures movement from the window centre.
        bool RelativeMode()
        {
            DWORD app = AppModule();
            if (!app || *reinterpret_cast<const int*>(app + kAppCursorShow) <= 0)
                return false;
            bool captured = *reinterpret_cast<const BYTE*>(app + kAppCaptured) != 0;
            if (captured && !(*reinterpret_cast<const DWORD*>(app + kAppFlags4C) & 1))
                return true;
            if (*reinterpret_cast<const WORD*>(app + kAppButtons) & 0x0F80)
                return true;
            return ((*reinterpret_cast<const DWORD*>(app + kAppFlags48) >> 28) & 1) != 0;
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

        // Reads and re-centres the real mouse; returns its movement since the last call (0 if unfocused).
        POINT SampleMouse()
        {
            POINT moved = { 0, 0 };
            HWND hwnd = GameWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (!hwnd || pid != GetCurrentProcessId())
                return moved;
            POINT centre = { g_width / 2, g_height / 2 };
            ClientToScreen(hwnd, &centre);
            POINT real;
            if (g_realGetCursorPos(&real))
            {
                moved.x = real.x - centre.x;
                moved.y = real.y - centre.y;
                g_realSetCursorPos(centre.x, centre.y);
            }
            g_movedX += moved.x;
            g_movedY += moved.y;
            return moved;
        }

        // Diagnostic: follows a few cursor updates end to end. Each line shows the game's cursor when the hook
        // ran, the client point we meant, the screen point returned, what the game's own ScreenToClient (exe
        // import, possibly dgVoodoo's) turns that back into, and where the previous update left the cursor.
        int g_traceLines = 0;
        int g_tracePrevTargetX = 0, g_tracePrevTargetY = 0;
        void TraceCursor(int cursorX, int cursorY, POINT client, POINT screen)
        {
            if (g_traceLines >= 40 || (g_tracePrevTargetX == client.x && g_tracePrevTargetY == client.y))
                return;
            typedef BOOL(WINAPI* ScreenToClientFn)(HWND, LPPOINT);
            ScreenToClientFn gameScreenToClient = *reinterpret_cast<ScreenToClientFn*>(0x0072041C);
            POINT back = screen;
            gameScreenToClient(GameWindow(), &back);
            Log("cursor trace: game cursor %d,%d (previous request %d,%d), request client %ld,%ld -> screen %ld,%ld -> "
                "game ScreenToClient %ld,%ld, sensitivity %.3f %.3f %.3f %.3f",
                cursorX, cursorY, g_tracePrevTargetX, g_tracePrevTargetY, client.x, client.y, screen.x, screen.y,
                back.x, back.y, *reinterpret_cast<const float*>(AppModule() + kAppSensitivity[0]),
                *reinterpret_cast<const float*>(AppModule() + kAppSensitivity[1]),
                *reinterpret_cast<const float*>(AppModule() + kAppSensitivity[2]),
                *reinterpret_cast<const float*>(AppModule() + kAppSensitivity[3]));
            g_tracePrevTargetX = client.x;
            g_tracePrevTargetY = client.y;
            ++g_traceLines;
        }

        // Called by the game once per frame from its cursor update (0x414675), so the mouse is sampled in
        // step with the game's own input read.
        BOOL WINAPI HookGetCursorPos(LPPOINT pt)
        {
            if (!Live() || !pt)
            {
                if (GetCurrentThreadId() == g_mainThread)
                    RestoreSensitivity();
                return g_realGetCursorPos(pt);
            }

            if (GetCurrentThreadId() != g_mainThread)
            {
                // The cursor-drawing thread updating its own copy: steer that copy onto the game's cursor, or
                // into the top-right corner (sprite off-screen) while eye frames are being made. (Answering it as
                // the game thread made a second cursor that moved about twice as far, and stole mouse movement.)
                DWORD mouse = *reinterpret_cast<const DWORD*>(kRapiMousePtr);
                if (!mouse)
                    return g_realGetCursorPos(pt);
                int drawnX = *reinterpret_cast<const int*>(mouse + kMousePosX);
                int drawnY = *reinterpret_cast<const int*>(mouse + kMousePosY);
                POINT want = { AppInt(kAppCursorX), AppInt(kAppCursorY) };
                if (g_hideDrawnCursor)
                {
                    want.x = g_width - 1;
                    want.y = 0;
                }
                POINT q = { drawnX, drawnY };
                if (RelativeMode())
                {
                    q.x = (AppInt(kAppWindowRight) - AppInt(kAppWindowLeft)) / 2;
                    q.y = (AppInt(kAppWindowBottom) - AppInt(kAppWindowTop)) / 2;
                }
                q.x += want.x - drawnX;
                q.y += want.y - drawnY;
                ClientToScreen(GameWindow(), &q);
                *pt = q;
                return TRUE;
            }

            POINT moved = SampleMouse();
            // The cursor update (0x41469C) measures movement from the cursor itself in absolute mode (menus) and
            // from the window centre in relative mode (gameplay): 0x4158F7 re-centres the anchor to half the
            // client size before the movement is taken (cursor trace: in-game the cursor moved by "request -
            // 512,384" every frame). Report the point that gives the wanted movement in the current mode.
            int cursorX = AppInt(kAppCursorX), cursorY = AppInt(kAppCursorY);
            POINT origin = { cursorX, cursorY };
            if (RelativeMode())
            {
                origin.x = (AppInt(kAppWindowRight) - AppInt(kAppWindowLeft)) / 2;
                origin.y = (AppInt(kAppWindowBottom) - AppInt(kAppWindowTop)) / 2;
                SetAppInt(kAppAnchorX, origin.x);  // what 0x4158F7 sets when it re-centres
                SetAppInt(kAppAnchorY, origin.y);
            }
            POINT p = origin;
            if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)
            {
                // Middle-drag rotates the camera from mouse movement: hand the game the real movement at the
                // player's sensitivity and keep the pointer where it is.
                RestoreSensitivity();
                p.x += moved.x;
                p.y += moved.y;
            }
            else
            {
                // Move the pointer, then report the position that moves the game's cursor exactly onto its
                // target. The game emits that movement as input events, so the interface's hover and clicks
                // follow (writing the cursor directly with zero movement left UIShell's mouse behind).
                // Sensitivity is held at 1 (its default) in case the player changed it in the options.
                const float kDegToRad = 0.0174532925f;
                float k = g_settings.degreesPerPixel * kDegToRad;
                g_yaw -= float(moved.x) * k;
                g_pitch -= float(moved.y) * k;
                g_pitch = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, g_pitch));
                PlaceGameCursor();
                NeutralSensitivity();
                p.x += g_cursorX - cursorX;
                p.y += g_cursorY - cursorY;
            }
            POINT client = p;
            ClientToScreen(GameWindow(), &p);
            *pt = p;
            TraceCursor(cursorX, cursorY, client, p);
            return TRUE;
        }

        BOOL WINAPI HookSetCursorPos(int x, int y)
        {
            if (!Live())
                return g_realSetCursorPos(x, y);
            // The game only re-centres the cursor for its relative mode; we hold the real cursor ourselves
            // and following these calls snapped the pointer back to the panel centre every frame.
            return TRUE;
        }

        void RewriteMouse(LPMSG msg)
        {
            if (Live() && msg && IsMouseMessage(msg->message))
                msg->lParam = MAKELPARAM(g_cursorX, g_cursorY);
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
        g_mainThread = GetCurrentThreadId();
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

        // The mouse itself is sampled in HookGetCursorPos; here only the panel may have moved.
        PlaceGameCursor();

        // Diagnostic: pointer state every 2 s for the first two minutes of each session.
        DWORD now = GetTickCount();
        if (now - g_lastStateLog >= 2000 && g_stateLogs < 60)
        {
            const float kRadToDeg = 57.2957795f;
            Log("vr pointer: yaw %.1f pitch %.1f deg, %s, target %d,%d, game cursor %d,%d, mouse moved %ld,%ld, "
                "panel yaw %.1f pitch %.1f, middle button %s",
                g_yaw * kRadToDeg, g_pitch * kRadToDeg, g_onPanel ? "on panel" : "OFF panel", g_cursorX, g_cursorY,
                AppInt(kAppCursorX), AppInt(kAppCursorY), g_movedX, g_movedY,
                g_panelYaw * kRadToDeg, g_panelPitch * kRadToDeg, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? "down" : "up");
            g_movedX = g_movedY = 0;
            g_lastStateLog = now;
            ++g_stateLogs;
        }
    }

    void SetDrawnCursorHidden(bool hidden) { InterlockedExchange(&g_hideDrawnCursor, hidden ? 1 : 0); }

    bool PointerActive() { return Live(); }
    bool PointerOnPanel() { return g_onPanel; }

    void GetPointerDirection(float* yaw, float* pitch)
    {
        *yaw = g_yaw;
        *pitch = g_pitch;
    }

    void GetGameCursor(int* x, int* y, int* width, int* height)
    {
        // The game's own cursor (what it picks with), which follows g_cursorX/Y a frame behind.
        *x = AppModule() ? AppInt(kAppCursorX) : g_cursorX;
        *y = AppModule() ? AppInt(kAppCursorY) : g_cursorY;
        *width = g_width;
        *height = g_height;
    }
}
