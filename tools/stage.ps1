# Copies the built loader into dist/ (the install layout) and, with -Game <Binaries\Win64 folder>, into the game.
#   dist\dwmapi.dll            the proxy (next to kards-Win64-Shipping.exe)
#   dist\kmod\kmod_core.dll    the core; kmod\ also receives mods\, data\, logs\ when the game runs
param([string]$Game = "", [string]$Config = "Release")
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config"
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force "$dist\kmod" | Out-Null
Copy-Item "$bin\dwmapi.dll" "$dist\dwmapi.dll" -Force
Copy-Item "$bin\kmod_core.dll" "$dist\kmod\kmod_core.dll" -Force
if ($Game) {
    if (Get-Process -Name "kards-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "the game is running: close it first" }
    New-Item -ItemType Directory -Force "$Game\kmod" | Out-Null
    Copy-Item "$dist\dwmapi.dll" "$Game\dwmapi.dll" -Force
    Copy-Item "$dist\kmod\kmod_core.dll" "$Game\kmod\kmod_core.dll" -Force
    Write-Host "installed into $Game"
}
