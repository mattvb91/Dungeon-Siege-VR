# Dungeon Siege VR — Plan

Goal: play Dungeon Siege 1 (Steam, v1.11.1.1486) on a Valve Index via SteamVR.

See [EXE-ANALYSIS.md](EXE-ANALYSIS.md) for the evidence behind every decision below.

## Constraints that shape the design

| Fact | Consequence |
|---|---|
| Renderer is **Direct3D 7** (`DDRAW.dll!DirectDrawCreateEx`, `IID_IDirect3DTnLHalDevice`) | OpenVR cannot take D3D7 surfaces. We need a D3D7 → D3D11 bridge. |
| Exe is **32-bit**, no ASLR, no DRM | Fixed addresses; we link the **win32** `openvr_api.dll`. 2 GB address-space limit. |
| Exe imports `VERSION.dll` | A proxy `version.dll` in the game dir is auto-loaded → our injection point. No launcher needed. |
| Exe exports 3239 named C++ symbols (FuBi) | We can locate engine functions (camera, UI, world) by name instead of signature scanning. |
| No DirectInput; input is Win32 messages + `GetAsyncKeyState` | Controller → mouse/keyboard mapping can be done with SteamVR Input bindings or injected Win32 input. |

## Architecture

```
DungeonSiege.exe (D3D7)
   │  loads
   ├── version.dll  (ours: dsvr)  ── forwards real exports to System32\version.dll
   │      ├─ OpenVR (win32 openvr_api.dll): HMD pose, compositor submit
   │      ├─ engine hooks: SiegeCamera fields / CameraAgent orders (by export name)
   │      └─ D3D11 hook on dgVoodoo's swapchain → copy back buffer → IVRCompositor::Submit
   └── ddraw.dll + D3DImm.dll (dgVoodoo2: D3D7 → D3D11)
```

## Milestones

### M0 — Loader skeleton  ✅ verified 2026-10-14 (launched to main menu, clean exit; log below)
```
dsvr attach
forwarding to C:\WINDOWS\system32\version.dll
host: ...\DungeonSiege.exe (base 0x00400000, timestamp 0x4D2E4A5A)
SiegeEngine singleton accessor: 0x0047D93C (expected 0x0047D93C)
dsvr detach
```
- `version.dll` proxy that forwards all exports to the system DLL and writes `dsvr.log`.
- **Done when:** game launches normally with our DLL in its folder and the log shows attach + exe version.

### M1 — dgVoodoo2 bridge  ✅ verified 2026-10-14 (watermark visible, gameplay looks and runs normally)
- dgVoodoo2 v2.87.5 `DDraw.dll`/`D3DImm.dll` + default `dgVoodoo.conf` deployed via `deploy.ps1`.
  Watermark left ON for this test as visual proof dgVoodoo is rendering.
- **Done when:** watermark visible, and ~5 min of play (a level, combat, spell effects) looks and performs like vanilla.

### M2 — VR "cinema" (mono, big screen)  ✅ verified 2026-10-14 in the Index (log: `first frame on overlay (800x600)`; survives 640x480/1024x768 mode switches)
Lessons: dgVoodoo FreeLibrary's the add-on on every deactivation (game probes DirectDraw at startup) → DLL is pinned
and the VR thread is process-lifetime. D3D11 can only open D3D12 shared textures with `ALLOW_RENDER_TARGET`
(`tools/sharetest`). Launching via Steam also shows Steam's own Desktop Game Theatre — test by running the exe directly.
Design (replaces the original "hook Present + Submit per eye" idea):
- **Frame capture via dgVoodoo's official add-on API.** dgVoodoo 2.87 loads an add-on only if it is named
  `SampleAddon.dll` (exports `AddOnInit`/`AddOnExit` — note capital O, the docs say `AddonInit`), and only with
  `OutputAPI = d3d12_fl12_0`. `ID3D12RootObserver::D3D12SwapchainPresentBegin` gives us the presented image;
  we record a `CopyTextureRegion` into a ring of 3 shared, simultaneous-access D3D12 textures on dgVoodoo's
  auto command list and publish the slot with the fence value `GetFenceValue()` returns. We return `false`,
  so the monitor image is untouched. (`src/addon/addon_main.cpp`, `frame_ring.*`)
- **Display via a SteamVR overlay, not Submit.** A VR thread (`VRApplication_Overlay`) waits for the fence,
  opens the slot's NT handle on a D3D11 device on the same GPU (matched by LUID), copies into one of two
  overlay textures and calls `SetOverlayTexture`. The compositor draws the curved screen (3.0 m wide, 2.2 m
  ahead, seated space) at the HMD's refresh rate, so head tracking is smooth even if the game runs slower.
  OpenVR only documents D3D12 textures for `Submit`, hence the D3D11 hop. (`vr_overlay.*`)
- dgVoodoo config now: D3D12 FL12_0 output, windowed, watermark off.
- Logs: `dsvr_vr.log` in the game folder.
- **Done when:** game is playable on the virtual screen in the Index; screen stays put when moving the head.

### M3 — Head-tracked "tabletop" camera  ✅ v1 verified 2026-10-14 in the Index (works, no jitter; log: `first tracked frame (head -0.047 0.011 -0.046 m, eye offset 0.28 0.06 -0.27)`)
✅ Polish verified 2026-10-14: `dsvr.ini` settings, Ctrl+F9 toggle / Ctrl+F10 recentre (seen in log), culling frustum
call at `0x6619EB` also receives the shifted eye; user reports clicking and edges behave while leaning.
v1 = head **position** parallax only: the call at `0x661A49` (SiegeCamera → `Rapi::SetViewLookAt`) is
redirected to `HookSetView` (`src/loader/head_tracking.cpp`), which offsets the eye by the seated-space HMD
position × 6 in the camera's right/up/forward basis and keeps looking at the target. Main camera only
(`up == camera+0x160`). Head **rotation** is deliberately ignored: the M2 screen is world-locked, so rotating
the game camera too would double the rotation. Rotation belongs to M4 (stereo). The pose comes from
`SampleAddon.dll!DsvrGetHeadPosition`. Culling frustum (0x6799C1) still uses the unmodified eye.

Original M3 notes:
- Resolve `SiegeEngine` singleton via `?FUBI_GetClassSingleton@SiegeEngine@siege@@...` export.
- Camera = `*(SiegeEngine + 0x3C)`. Validate field offsets at runtime (FOV `+0x124`, pos `+0x148`, target `+0x154`, up `+0x160`, matrix `+0x178`, quat `+0x19C`).
- Apply HMD pose relative to the game's orbit camera; set FOV from `IVRSystem::GetProjectionRaw`.
- **Done when:** leaning/turning head moves the camera around the party smoothly with no judder.

### M4 — True stereo
- Disassemble the Rapi render loop (Ghidra) to find the per-frame scene draw.
- Intercept view/projection (`IDirect3DDevice7::SetTransform`) and render the scene twice (per-eye view + asymmetric projection), UI once onto a floating panel.
- **Done when:** correct stereo depth with UI legible, ≥ 90 fps on the target PC.

### M5 — Controls & polish
- Index controllers: laser pointer → game cursor; buttons → click/hotkeys via SteamVR Input action manifest.
- Comfort options (snap/smooth camera, world scale), config file, packaging/installer script.

## Risks / open questions
- dgVoodoo2's D3D11 device creation path must be hookable from our DLL (load-order: we load before ddraw is created — good).
- Re-rendering the scene twice per frame may break engine state (fog, culling, frame-timed effects). Fallback: alternate-eye rendering.
- 2 GB address space with OpenVR + dgVoodoo loaded; may need to set the LARGE_ADDRESS_AWARE flag on a copy of the exe.
- Field offsets are from static analysis of one build (SHA256 `C408EF77…F33AB`); refuse to run on other hashes.
