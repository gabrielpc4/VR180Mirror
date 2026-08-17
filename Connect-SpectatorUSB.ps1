# Routes the spectator stream over the Quest's USB cable via adb reverse
# tunnels, instead of Wi-Fi. Requires Developer Mode enabled on the spectator
# headset (Meta Horizon app -> headset settings -> Developer Mode) and
# accepting the "Allow USB debugging" prompt in the headset once.
#
# After this succeeds, open  http://localhost:9080/  in the Quest browser.
# localhost is a secure context, so WebXR works with no certificate warning,
# and the player page pins the WebRTC media to the loopback tunnel (the cable).
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
if (@($ready).Count -gt 1) {
    Write-Host "Multiple adb devices found - unplug the player headset or keep only the spectator connected." -ForegroundColor Yellow
    & $adb devices
    exit 1
}

$serial = ($ready[0] -split "\t")[0]
Write-Host "Spectator headset: $serial"

# page + WHEP signaling (http server) and WebRTC media over ICE-TCP
& $adb -s $serial reverse tcp:9080 tcp:9080
& $adb -s $serial reverse tcp:9189 tcp:9189
Write-Host ""
Write-Host "USB tunnels active. On the spectator Quest, open:" -ForegroundColor Green
Write-Host ""
Write-Host "    http://localhost:9080/" -ForegroundColor Cyan
Write-Host ""
Write-Host "then Connect to stream -> Enter VR. Re-run this script if the cable"
Write-Host "is re-plugged or the headset reboots."
