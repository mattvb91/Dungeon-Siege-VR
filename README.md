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

## Install

```powershell
.\deploy.ps1             # copies version.dll + dgVoodoo2 (DDraw.dll, D3DImm.dll, dgVoodoo.conf) into the game folder
.\deploy.ps1 -Uninstall  # removes them again
```

`dsvr.log` appears in the game folder. dgVoodoo2 must first be fetched into `third_party/` (see its README).
The dgVoodoo config we ship is tracked in `deploy/dgVoodoo.conf`.
