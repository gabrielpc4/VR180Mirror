# Installs the VR180Mirror OBS profile + scene collection from .\obs\ into the
# user's OBS Studio config. Safe: only creates/overwrites the "VR180Mirror"
# profile and collection, nothing else. Run while OBS is closed (or at least
# while the VR180Mirror profile is not the active one).
$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "obs"
$profileDir = "$env:APPDATA\obs-studio\basic\profiles\VR180Mirror"
$sceneFile  = "$env:APPDATA\obs-studio\basic\scenes\VR180Mirror.json"

if (-not (Test-Path "$env:APPDATA\obs-studio")) {
    throw "OBS Studio config folder not found - install and run OBS Studio once first."
}

New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item "$src\basic.ini","$src\service.json","$src\streamEncoder.json" $profileDir -Force
Copy-Item "$src\scene-collection.json" $sceneFile -Force
Write-Host "Installed OBS profile 'VR180Mirror' and scene collection 'VR180Mirror'."
