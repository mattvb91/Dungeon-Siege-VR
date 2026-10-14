# Configures and builds dsvr (32-bit) with the VS 2022 Build Tools' bundled CMake.
param([ValidateSet("Debug", "Release")][string]$Config = "Release")

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

& $cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -G "Visual Studio 17 2022" -A Win32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build "$PSScriptRoot\build" --config $Config
exit $LASTEXITCODE
