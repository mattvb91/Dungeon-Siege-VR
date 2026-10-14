# Copies the mod (and dgVoodoo2) into the Dungeon Siege folder, or removes it with -Uninstall.
param(
    [string]$GameDir = "${env:ProgramFiles(x86)}\Steam\steamapps\common\Dungeon Siege 1",
    [ValidateSet("Debug", "Release")][string]$Config = "Release",
    [switch]$Uninstall
)

$files = [ordered]@{
    "version.dll"     = "$PSScriptRoot\build\$Config\version.dll"
    "SampleAddon.dll" = "$PSScriptRoot\build\$Config\SampleAddon.dll"
    "openvr_api.dll"  = "$PSScriptRoot\build\$Config\openvr_api.dll"
    "DDraw.dll"     = "$PSScriptRoot\third_party\dgVoodoo2\MS\x86\DDraw.dll"
    "D3DImm.dll"    = "$PSScriptRoot\third_party\dgVoodoo2\MS\x86\D3DImm.dll"
    "dgVoodoo.conf" = "$PSScriptRoot\deploy\dgVoodoo.conf"
}

if (-not (Test-Path "$GameDir\DungeonSiege.exe")) { throw "DungeonSiege.exe not found in $GameDir" }

foreach ($name in $files.Keys) {
    $dest = Join-Path $GameDir $name
    if ($Uninstall) {
        if (Test-Path $dest) { Remove-Item $dest; "removed  $name" }
    } else {
        if (-not (Test-Path $files[$name])) { throw "missing $($files[$name]) (build first / see third_party/README.md)" }
        Copy-Item $files[$name] $dest -Force
        "deployed $name"
    }
}
