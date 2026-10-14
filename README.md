# dsvr — Dungeon Siege VR

Mod to play Dungeon Siege 1 (Steam, v1.11.1.1486) on a Valve Index via SteamVR.

- [docs/PLAN.md](docs/PLAN.md) — architecture and milestones
- [docs/EXE-ANALYSIS.md](docs/EXE-ANALYSIS.md) — static analysis of `DungeonSiege.exe` (evidence)
- [tools/re/](tools/re) — PowerShell scripts that reproduce the analysis

## Build

Requires Visual Studio 2022 Build Tools with the C++ workload.

```powershell
.\build.ps1            # -> build\Release\version.dll (32-bit)
```

## Install (manual, for now)

Copy `build\Release\version.dll` into the game folder
(`C:\Program Files (x86)\Steam\steamapps\common\Dungeon Siege 1\`) and launch the game.
`dsvr.log` appears next to it. Delete `version.dll` to uninstall.
