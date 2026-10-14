# DungeonSiege.exe — static analysis

Target: `C:\Program Files (x86)\Steam\steamapps\common\Dungeon Siege 1\DungeonSiege.exe`
- File version `1.11.1.1486 (msqa:03.07.0202)`, Gas Powered Games
- SHA256 `C408EF77B39484D8AD82BA17859CF1E60B24D3BAF6D429283A52B886D67F33AB`
- Size 3,821,628 bytes

Reproduce with the scripts in [`tools/re/`](../tools/re).

## PE header
```
Machine        : 0x014C (i386)          Opt magic : PE32
TimeDateStamp  : 2011-01-13 00:42:02Z   Linker    : 6.0 (MSVC 6)
ImageBase      : 0x400000               EntryPoint RVA: 0x9DCB
Characteristics: 0x010F  (LARGE_ADDRESS_AWARE = False)
DllCharacteristics: 0x0000 (no ASLR, no NX)
```
Sections: `.text .rdata .data .tls .rsrc` — no `.bind` (SteamStub) or packer sections.

## Imports
| DLL | Count | Notes |
|---|---|---|
| KERNEL32 | 165 | `LoadLibraryA`, `GetProcAddress`, … |
| USER32 | 94 | `GetAsyncKeyState`, `GetCursorPos`, `PeekMessageA`, … (input path) |
| mss32 | 68 | Miles Sound System incl. `_AIL_set_3D_orientation@28` |
| GDI32 | 20 | |
| binkw32 | 10 | Bink video |
| ADVAPI32, ole32, WSOCK32 | 6 each | |
| VERSION | 3 | `GetFileVersionInfoA`, `GetFileVersionInfoSizeA`, `VerQueryValueA` → **proxy injection point** |
| DDRAW | 2 | `DirectDrawCreateEx`, `DirectDrawEnumerateExA` |
| WINMM | 1 | `timeGetTime` |

No `d3d8`/`d3d9`/`dinput`/`steam_api` anywhere in the file.

`DirectDrawCreateEx` IAT slot `0x00720020`, reached via thunk `jmp [0x720020]` at `0x0071EB72`.

## Direct3D version — COM IIDs found in `.rdata`
| IID | File offset |
|---|---|
| IID_IDirect3DTnLHalDevice | 0x325C90 |
| IID_IDirect3DHALDevice | 0x325CA0 |
| IID_IDirect3D7 | 0x325CB0 |
| IID_IDirectDraw7 | 0x325CD0 |
| IID_IDirect3D8 / IID_IDirect3D9 | absent |

Strings include the full D3D7-era `D3DERR_*` / `DDERR_*` tables.

## Engine modules (embedded `__FILE__` paths, `C:\work\ds1\update1\gpg\…`)
`LIB Projects\Rapi` (renderer: `Rapi.cpp`, `RapiAppModule.cpp`, `RapiMouse.cpp`, `RapiOwner.cpp`),
`Siege`, `NEMA` (animation), `Skrit`, `FuBi`, `GPCore`, `NetPipe`, `ime`, and `projects\tattoo` (game).

## Exports (FuBi)
3239 named, MSVC-decorated exports — the scripting bridge for Skrit. Top classes:
`Go` 203, `GoMind` 188, `GoDb` 112, `UIWindow` 110, `GoInventory` 104, `MoodSetting` 97 …

Camera-related:
```
0x00473434 ?FUBI_GetClassSingleton@CameraAgent@@CAPAV1@XZ
0x0047D93C ?FUBI_GetClassSingleton@SiegeEngine@siege@@CAPAV12@XZ
0x0047521B ?FUBI_GetClassSingleton@UICamera@@CAPAV1@XZ
0x00473430 ?GetCamera@SiegeEngine@siege@@QBEABVSiegeCamera@2@XZ
0x004733D8 ?GetFieldOfView@SiegeCamera@siege@@QAEMXZ
0x00473425 ?GetQuatOrientation@SiegeCamera@siege@@QAEABUQuat@@XZ
0x005946D8 ?SubmitCameraPosition@CameraAgent@@...CameraPosition...
0x00594BC3 ?SubmitOrder@CameraAgent@@...
0x004751E3 ?SetCameraMode@UICamera@@QAEXW4eCameraMode@@@Z
```
Note: identical-code folding means some exports share an address
(`GetCamera` and `CameraAgent::GetInterpolationMode` both at `0x473430`).

## SiegeCamera field layout (decoded from accessor bytes)
| Accessor | Bytes | Meaning |
|---|---|---|
| `SiegeEngine::GetCamera` | `8B 41 3C C3` | `mov eax,[ecx+3Ch]` → camera ptr at engine+0x3C |
| `GetViewportNormalizedWidth` | `D9 81 1C 01 00 00` | float at +0x11C |
| `GetViewportNormalizedHeight` | | float at +0x120 |
| `GetFieldOfView` | `D9 81 24 01 00 00` | float at **+0x124** |
| `GetCameraPosition` | `8D 81 48 01 00 00` | vector_3 at **+0x148** |
| `GetTargetPosition` | `8D 81 54 01 00 00` | vector_3 at +0x154 |
| `GetUpVector` | `8D 81 60 01 00 00` | vector_3 at +0x160 |
| `GetVectorOrientation` | `8D 81 6C 01 00 00` | vector_3 at +0x16C |
| `GetMatrixOrientation` | `8D 81 78 01 00 00` | matrix_3x3 at **+0x178** |
| `GetQuatOrientation` | `8D 81 9C 01 00 00` | Quat at **+0x19C** |
| `GetMaxAzimuth` / `GetMinAzimuth` / `GetMaxDistance` | `D9 81 ..` | floats at +0x1AC / +0x1B0 / +0x1B4 |

### Confirmed at runtime (M3 camera probe, 2026-10-14)
Field names come from SiegeCamera's persistence function `0x661650` (strings next to each offset):

| Offset | Name | Live behaviour |
|---|---|---|
| +0x11C / +0x120 | `m_viewWidth` / `m_viewHeight` | 1.0 / 1.0 (normalised) |
| +0x124 | `m_fieldOfView` | 1.0472 rad = 60° |
| +0x128 | `m_CameraPosition` | SiegePos: vector_3 + node guid (+0x134), node-local |
| +0x138 | `m_TargetPosition` | SiegePos, node guid at +0x144 changes as the party walks |
| +0x148 / +0x154 | (derived) camera / target world pos | recomputed each frame from the SiegePos fields |
| +0x160 | `m_UpVector` | always (0,1,0): Y-up |
| +0x16C | `m_VectorOrientation` | target − camera |
| +0x178 | `m_MatrixOrientation` | orthonormal; 3rd column = normalised view direction |
| +0x19C | `m_QuatOrientation` | quaternion of the matrix |
| +0x1AC / +0x1B0 | `m_maxAzimuth` / `m_minAzimuth` | **degrees**: 85 / 32 (camera tilt limits) |
| +0x1B4 / +0x1B8 | `m_maxDistance` / `m_minDistance` | 18 / 4 (zoom range; probe saw 3–18) |

Singleton globals (from FuBi accessors `mov eax,[global]; ret`): `SiegeEngine*` `0x7A05FC`,
`CameraAgent*` `0x7A05BC`, `UICamera*` `0x7A0600`, `RapiAppModule*` `0x79CF7C`.

## Per-frame camera → renderer path
Found with hardware write-watchpoints (all writes on the main thread), then `dumpbin /DISASM`:

```
SiegeCamera::0x6617A2 / 0x6617CA  SetPositions(SiegePos cam, SiegePos target) → 0x661806
SiegeCamera::0x661806             node→world for +0x148/+0x154, look-at → +0x178 matrix, +0x19C quat
SiegeCamera::0x66192A             per-frame render setup (callers incl. 0x479DF1, 0x665698):
   ├ 0x6799C1(w,h,fov,near,far,&pos,&matrix)  culling frustum, stored in the camera (ret 1Ch)
   ├ 0x653229(...)                            Rapi viewport
   ├ 0x653D06(fov, near, far, ...)            Rapi projection
   └ call @0x661A49 → 0x653E96(&pos,&target,&up)   Rapi::SetViewLookAt (thiscall, ret 0Ch)
          └ 0x653858 builds the view matrix at Rapi+0xC88, then
            IDirect3DDevice7 vtable+0x2C (SetTransform) with state 2 = D3DTRANSFORMSTATE_VIEW
```
`engine+0x44` is the Rapi renderer object passed as `this` to the 0x653xxx calls.

## Config
Camera tuning is data-driven from `config:camera_settings` (gas): `orbit_degrees_per_second`,
`azimuth_degrees_per_second`, `zoom_meters_per_second`, `camera_mode`, `camera_inverse_x/y`, …
