# Routes the spectator stream over the Quest's USB cable via an adb reverse
# tunnel. This is the ONLY supported spectator transport - it requires a
# USB 3.0 cable and a USB 3.0 port on the PC (USB 2.0 cannot carry the
# ~150 Mbps stream). Requires Developer Mode enabled on the spectator
# headset (Meta Horizon app -> headset settings -> Developer Mode) and
# accepting the "Allow USB debugging" prompt in the headset once.
#
# After this succeeds, open  http://localhost:9080/  in the Quest browser.
# localhost is a secure context, so WebXR works with no certificate warning.
#
# Usage: .\Connect-SpectatorUSB.ps1 [-Serial <adb device id>]
param([string]$Serial = "")
$ErrorActionPreference = "Stop"

# find adb (install Android platform-tools via winget if missing)
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($cand) { $adb = $cand }
}
if (-not $adb) {
    Write-Host "adb not found - installing Android platform-tools (winget)..."
    winget install --id Google.PlatformTools --accept-source-agreements --accept-package-agreements --silent | Out-Null
    $adb = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe" |
        Select-Object -First 1 -ExpandProperty FullName
}
Write-Host "adb: $adb"

& $adb start-server | Out-Null
$devices = (& $adb devices) -split "`n" | Select-Object -Skip 1 | Where-Object { $_ -match "\S" }
$ready   = $devices | Where-Object { $_ -match "\tdevice$" }
$unauth  = $devices | Where-Object { $_ -match "unauthorized" }

if ($unauth) {
    Write-Host ""
    Write-Host "Headset detected but UNAUTHORIZED." -ForegroundColor Yellow
    Write-Host "Put the headset on and accept the 'Allow USB debugging' dialog"
    Write-Host "(check 'Always allow'), then run this script again."
    exit 1
}
if (-not $ready) {
    Write-Host ""
    Write-Host "No headset found via adb." -ForegroundColor Yellow
    Write-Host " - plug the SPECTATOR Quest into a USB 3 port with a data cable"
    Write-Host " - Developer Mode must be enabled for the headset"
    Write-Host "   (Meta Horizon phone app -> Devices -> headset -> Developer Mode)"
    exit 1
}
$serials = $ready | ForEach-Object { ($_ -split "\t")[0] }
if (@($serials).Count -gt 1 -and -not $Serial) {
    Write-Host "Multiple adb devices found - pass -Serial <id> to pick the spectator headset:" -ForegroundColor Yellow
    & $adb devices
    exit 1
}
$serial = if ($Serial) { $Serial } else { $serials }
Write-Host "Spectator headset: $serial"

& $adb -s $serial reverse tcp:9080 tcp:9080
Write-Host ""
Write-Host "USB tunnels active. On the spectator Quest, open:" -ForegroundColor Green
Write-Host ""
Write-Host "    http://localhost:9080/" -ForegroundColor Cyan
Write-Host ""
Write-Host "then Connect to stream -> Enter VR. Re-run this script if the cable"
Write-Host "is re-plugged or the headset reboots."
