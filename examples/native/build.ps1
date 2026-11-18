# Builds one native example (or any native mod folder that uses kmod_mod.cmake) for Windows x64:  .\build.ps1 lua_functions
# The DLL lands in <example>\native\windows-x64\. With -Install <kmod folder> the mod (mod.json, main.lua, native\) is copied to <kmod>\mods\<name>.
# The Android build of the same source is the Android loader repository's examples\native\build.ps1 (NDK toolchain), same CMakeLists.
param([Parameter(Mandatory = $true)][string]$Name, [string]$Install = "")
$here = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $Name
$cm = if (Get-Command cmake -ErrorAction SilentlyContinue) { 'cmake' } else { 'C:\Program Files\CMake\bin\cmake.exe' }
& $cm -S $here -B "$here\build" -A x64
& $cm --build "$here\build" --config Release
if ($Install) {
    $dst = Join-Path $Install "mods\$Name"
    New-Item -ItemType Directory -Force "$dst\native\windows-x64" | Out-Null
    Copy-Item "$here\mod.json" $dst -Force
    if (Test-Path "$here\main.lua") { Copy-Item "$here\main.lua" $dst -Force }
    Copy-Item "$here\native\windows-x64\*" "$dst\native\windows-x64" -Force
    Write-Host "installed $Name into $dst"
}
Write-Host "Done: $here\native\windows-x64"
