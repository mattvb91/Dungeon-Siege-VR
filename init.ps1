# Fetches the third-party dependencies into third_party/ (see third_party/README.md), verifying each file's SHA-256.
# Already-present dependencies are skipped; -Force re-downloads them.
param([switch]$Force)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # Invoke-WebRequest's progress bar makes downloads very slow in PowerShell 5.1
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$thirdParty = "$PSScriptRoot\third_party"
$tmp = Join-Path ([IO.Path]::GetTempPath()) "dsvr-init"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Get-Verified([string]$Url, [string]$Dest, [string]$Sha256) {
    New-Item -ItemType Directory -Force (Split-Path $Dest) | Out-Null
    Invoke-WebRequest -UseBasicParsing $Url -OutFile $Dest
    $actual = (Get-FileHash $Dest -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) {
        Remove-Item $Dest
        throw "SHA-256 mismatch for $Url`n  expected $Sha256`n  got      $actual"
    }
}

# OpenVR SDK v2.15.6: only the files the build needs, straight from the tag
$openvr = "$thirdParty\openvr"
if ($Force -or -not (Test-Path "$openvr\lib\win32\openvr_api.lib")) {
    "fetching OpenVR SDK v2.15.6"
    $base = "https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6"
    $files = [ordered]@{
        "headers/openvr.h"          = "1E6ED57199896CC1F7C5484E50FA18955E97BE15BE690BEB28D998C877EAD7FD"
        "lib/win32/openvr_api.lib"  = "9EC5921313D97EB88D7FB98371E31CC0EB73F792266F8EA4EF387031FE0D3A3D"
        "bin/win32/openvr_api.dll"  = "AB696E4F218A95B3E396BC310F9FE6485DF48C99C0969762083212B1E1F025A6"
        "bin/win32/openvr_api.pdb"  = "A1632380EDDE615480927B36842BBC44EE5E9F0F62C446F4D585E03368A94D9E"
        "LICENSE"                   = "F56FF606104D4EF18E617921A75C73AD73B5A1A1D70C69590C29DE16919E04AD"
    }
    foreach ($f in $files.Keys) { Get-Verified "$base/$f" (Join-Path $openvr $f) $files[$f] }
} else { "OpenVR SDK already present" }

# dgVoodoo2 v2.87.5 and its add-on API SDK: release zips, extracted as-is
$zips = @(
    @{ Name = "dgVoodoo2";   Check = "MS\x86\DDraw.dll";          Zip = "dgVoodoo2_87_5.zip"
       Sha256 = "5FFDE6927F7355CA3FDD5D785B581256A8E6539FA13E395A891ADE6BA1040850" },
    @{ Name = "dgVoodooAPI"; Check = "Lib\x86\dgVoodooAddon.lib"; Zip = "dgVoodooAPI_287_5.zip"
       Sha256 = "12709F30209248C50E6FE37B838D16C5B55FEA1363C0BCAD21FA82015AE16B57" }
)
foreach ($z in $zips) {
    $dir = Join-Path $thirdParty $z.Name
    if ($Force -or -not (Test-Path (Join-Path $dir $z.Check))) {
        "fetching $($z.Zip)"
        $zip = Join-Path $tmp $z.Zip
        Get-Verified "https://github.com/dege-diosg/dgVoodoo2/releases/download/v2.87.5/$($z.Zip)" $zip $z.Sha256
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        Expand-Archive $zip $dir
    } else { "$($z.Name) already present" }
}

Remove-Item -Recurse -Force $tmp
"done; next: .\build.ps1"
