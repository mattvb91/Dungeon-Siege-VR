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

### M1 — dgVoodoo2 bridge  (deployed 2026-10-14, awaiting play test)
- dgVoodoo2 v2.87.5 `DDraw.dll`/`D3DImm.dll` + default `dgVoodoo.conf` deployed via `deploy.ps1`.
  Watermark left ON for this test as visual proof dgVoodoo is rendering.
- **Done when:** watermark visible, and ~5 min of play (a level, combat, spell effects) looks and performs like vanilla.

### Note for M2 — dgVoodoo add-on API
The dgVoodoo API SDK ships an official add-on interface (`Inc/Addon/ID3D12Root.hpp`, `ID3D12RootObserver.hpp`,
sample `D3D12 Addon/Presenter.cpp` that draws over the presented image). This is a supported way to reach
the final frame without hooking `IDXGISwapChain::Present` ourselves — evaluate it first for M2
(implies `OutputAPI = d3d12_fl12_0`).

### M2 — VR "cinema" (mono, big screen)
- Init OpenVR as `VRApplication_Scene`; hook `IDXGISwapChain::Present` in dgVoodoo's device.
- Copy back buffer to a shared texture; render a curved quad per eye; `IVRCompositor::Submit` both eyes.
- **Done when:** game is playable on a virtual screen in the Index with stable 90 Hz compositor timing.

### M3 — Head-tracked "tabletop" camera
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
