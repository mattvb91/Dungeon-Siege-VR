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

Static only — to be confirmed at runtime in M3.

## Config
Camera tuning is data-driven from `config:camera_settings` (gas): `orbit_degrees_per_second`,
`azimuth_degrees_per_second`, `zoom_meters_per_second`, `camera_mode`, `camera_inverse_x/y`, …
