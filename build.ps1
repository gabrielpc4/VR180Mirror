# Builds VR180Mirror.exe using MSVC Build Tools (no IDE needed).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# fetch the OpenVR SDK (headers + import lib + dll) on first build
if (-not (Test-Path "$root\third_party\openvr\headers\openvr.h")) {
    Write-Host "Fetching OpenVR SDK..."
    git clone --depth 1 https://github.com/ValveSoftware/openvr "$root\third_party\openvr"
    if ($LASTEXITCODE -ne 0) { throw "OpenVR SDK clone failed" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsroot = $null
if (Test-Path $vswhere) {
    $vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsroot) {
    $cands = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools",
               "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
               "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community")
    foreach ($c in $cands) { if (Test-Path "$c\VC\Auxiliary\Build\vcvars64.bat") { $vsroot = $c; break } }
}
if (-not $vsroot) { throw "MSVC Build Tools not found" }
$vcvars = "$vsroot\VC\Auxiliary\Build\vcvars64.bat"
Write-Host "Using MSVC at: $vsroot"

$out = "$root\bin"
New-Item -ItemType Directory -Force $out | Out-Null

$cmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE " +
       "/I `"$root\third_party\openvr\headers`" " +
       "`"$root\src\main.cpp`" " +
       "/Fe:`"$out\VR180Mirror.exe`" /Fo:`"$out\main.obj`" " +
       "/link `"$root\third_party\openvr\lib\win64\openvr_api.lib`" /SUBSYSTEM:CONSOLE && " +
       "cl /nologo /O2 /EHsc /std:c++17 /W3 /LD /DUNICODE /D_UNICODE " +
       "`"$root\src\oculus_hook.cpp`" " +
       "/Fe:`"$out\VR180OculusHook.dll`" /Fo:`"$out\oculus_hook.obj`" " +
       "/link /SUBSYSTEM:WINDOWS"

cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Copy-Item "$root\third_party\openvr\bin\win64\openvr_api.dll" $out -Force
Write-Host "Build OK -> $out\VR180Mirror.exe + VR180OculusHook.dll"
