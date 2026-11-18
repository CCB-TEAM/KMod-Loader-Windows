# Packs the built loader into dist/: kmod-loader-windows-<version>-<flavor>.zip (flavor: dev | release).
#   .\tools\package.ps1            builds nothing: run cmake --build for build\ (dev) and build-release\ (release) first
param([string]$Version = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = ((Select-String -Path "$root\core\src\kmod_version.h" -Pattern 'KMOD_VERSION "([^"]+)"').Matches[0].Groups[1].Value) }
$out = Join-Path $root "dist"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in @(@{ Name = "dev"; Dir = "build" }, @{ Name = "release"; Dir = "build-release" })) {
    $bin = Join-Path $root "$($f.Dir)\Release"
    $stage = Join-Path $out "stage-$($f.Name)"
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force "$stage\kmod" | Out-Null
    Copy-Item "$bin\dwmapi.dll" "$stage\dwmapi.dll"
    Copy-Item "$bin\kmod_core.dll" "$stage\kmod\kmod_core.dll"
    Copy-Item "$root\LICENSE" "$stage\LICENSE" -ErrorAction SilentlyContinue
    $readme = @"
kmod loader $Version ($($f.Name)) for KARDS (Windows)

Install: copy dwmapi.dll and the kmod folder next to kards-Win64-Shipping.exe
(<game>\kards\Binaries\Win64\). If a dwmapi.dll is already there (UE4SS), rename it first.
Put mods into kmod\mods\ (folders or .kmod packages); enable them on the Mods tab of the game settings.
Log: kmod\logs\kmod.log.   Docs: https://ccb-team.github.io/kmod-docs/
"@
    Set-Content -Path "$stage\README.txt" -Value $readme -Encoding UTF8
    $zip = Join-Path $out "kmod-loader-windows-$Version-$($f.Name).zip"
    Remove-Item $zip -ErrorAction SilentlyContinue
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Write-Host "$zip  $((Get-Item $zip).Length) bytes"
}
