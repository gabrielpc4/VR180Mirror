# =============================================================================
# Start-Spectator.ps1 - one-click start for the VR180 spectator pipeline
#
#   VR180Mirror.exe  (SteamVR mirror -> VR180 SBS window)
#     -> OBS (Game Capture -> NVENC H264 -> WHIP)
#       -> MediaMTX (WebRTC/WHEP + LL-HLS)
#         -> Quest 2 browser / DeoVR
#
# Usage:
#   .\Start-Spectator.ps1              normal run
#   .\Start-Spectator.ps1 -TestGrid    render calibration grid (no SteamVR needed)
#   .\Start-Spectator.ps1 -NoOBS       start everything except OBS
# =============================================================================
param(
    [switch]$TestGrid,
    [switch]$NoOBS
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# ---- first-run provisioning ----------------------------------------------------
if (-not (Test-Path "$root\bin\VR180Mirror.exe")) {
    Write-Host "VR180Mirror.exe not built yet - running build.ps1..."
    & "$root\build.ps1"
}
if (-not (Test-Path "$root\tools\mediamtx\mediamtx.exe")) {
    Write-Host "Downloading MediaMTX (WebRTC/HLS server)..."
    $rel = Invoke-RestMethod "https://api.github.com/repos/bluenviron/mediamtx/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -match "windows_amd64\.zip$" } | Select-Object -First 1
    $zip = "$env:TEMP\mediamtx_dl.zip"
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip
    $tmp = "$env:TEMP\mediamtx_dl"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force -Confirm:$false }
    Expand-Archive $zip -DestinationPath $tmp
    Copy-Item "$tmp\mediamtx.exe" "$root\tools\mediamtx\" -Force   # keep our mediamtx.yml
    Remove-Item $zip, $tmp -Recurse -Force -Confirm:$false
    Write-Host "MediaMTX $($rel.tag_name) installed."
}
if (-not (Test-Path "$env:APPDATA\obs-studio\basic\profiles\VR180Mirror\basic.ini")) {
    & "$root\Install-OBSProfile.ps1"
}

# ---- LAN IP -----------------------------------------------------------------
$lanIp = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.InterfaceAlias -notmatch 'Loopback|vEthernet' -and $_.IPAddress -notmatch '^169\.' -and $_.PrefixOrigin -ne 'WellKnown' } |
    Sort-Object -Property { $_.InterfaceAlias -notmatch 'Ethernet' } |
    Select-Object -First 1).IPAddress
if (-not $lanIp) { throw "No LAN IPv4 address found" }
Write-Host "LAN IP: $lanIp"

# ---- keep mediamtx advertised host current -----------------------------------
$yml = "$root\tools\mediamtx\mediamtx.yml"
$cfg = Get-Content $yml -Raw
$new = $cfg -replace 'webrtcAdditionalHosts:.*', "webrtcAdditionalHosts: [127.0.0.1, $lanIp]"
if ($new -ne $cfg) {
    # BOM-less UTF-8: PS5's -Encoding utf8 writes a BOM, which MediaMTX's YAML parser rejects
    [IO.File]::WriteAllText($yml, $new, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "mediamtx.yml: advertised host -> $lanIp"
}

# ---- self-signed cert for the WebXR page (once) -------------------------------
if (-not (Test-Path "$root\web\cert.pem") -or -not (Test-Path "$root\web\key.pem")) {
    $openssl = "C:\Program Files\Git\usr\bin\openssl.exe"
    if (-not (Test-Path $openssl)) { $openssl = (Get-Command openssl -ErrorAction Stop).Source }
    Write-Host "Generating self-signed certificate (web\cert.pem)..."
    & $openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes `
        -keyout "$root\web\key.pem" -out "$root\web\cert.pem" `
        -subj "/CN=VR180Mirror" `
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:$lanIp" | Out-Null
}

# ---- firewall (one-time, needs a UAC confirmation) ----------------------------
if (-not (Get-NetFirewallRule -DisplayName "VR180Mirror Web (HTTPS 8443)" -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "FIREWALL: inbound rules missing - the Quest cannot connect without them." -ForegroundColor Yellow
    Write-Host "          A UAC prompt will appear: please click YES once."               -ForegroundColor Yellow
    & "$root\Setup-Firewall.ps1"
}

# ---- helpers ------------------------------------------------------------------
function Test-Listening([int]$port) {
    return [bool](Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
}
function Get-OurProcess([string]$nameLike, [string]$cmdLike) {
    Get-CimInstance Win32_Process -Filter "Name = '$nameLike'" |
        Where-Object { $_.CommandLine -like "*$cmdLike*" }
}

# ---- MediaMTX ------------------------------------------------------------------
if (Get-OurProcess "mediamtx.exe" "VR180Mirror") {
    Write-Host "MediaMTX (VR180Mirror) already running"
} else {
    if (Test-Listening 9889) { throw "Port 9889 is already in use by something else" }
    Start-Process -FilePath "$root\tools\mediamtx\mediamtx.exe" `
        -ArgumentList "`"$root\tools\mediamtx\mediamtx.yml`"" `
        -WorkingDirectory "$root\tools\mediamtx" -WindowStyle Minimized
    Write-Host "MediaMTX started (WHIP/WHEP :9889, LL-HLS :9888)"
}

# ---- web server -----------------------------------------------------------------
if (Get-OurProcess "node.exe" "VR180Mirror\web\server.js") {
    Write-Host "Web server already running"
} else {
    Start-Process -FilePath "node" -ArgumentList "`"$root\web\server.js`"","--ip",$lanIp `
        -WorkingDirectory "$root\web" -WindowStyle Minimized
    Write-Host "Web server started (https :8443, http :9080)"
}

# ---- VR180Mirror -----------------------------------------------------------------
if (Get-OurProcess "VR180Mirror.exe" "VR180Mirror") {
    Write-Host "VR180Mirror already running"
} else {
    $mirrorArgs = @("--size","4096x2048","--fps","60","--preview","1280")
    if ($TestGrid) { $mirrorArgs += "--test-grid" }
    Start-Process -FilePath "$root\bin\VR180Mirror.exe" -ArgumentList $mirrorArgs `
        -WorkingDirectory "$root\bin"
    Write-Host "VR180Mirror started $(if ($TestGrid) { '(TEST GRID)' })"
}

# ---- OBS --------------------------------------------------------------------------
if (-not $NoOBS) {
    $obsRunning = Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
        Where-Object { $_.CommandLine -like "*VR180Mirror*" }
    if ($obsRunning) {
        Write-Host "OBS (VR180Mirror profile) already running"
    } else {
        Start-Process -FilePath "C:\Program Files\obs-studio\bin\64bit\obs64.exe" `
            -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit" `
            -ArgumentList "--multi","--only-bundled-plugins","--disable-shutdown-check",
                          "--profile","VR180Mirror","--collection","VR180Mirror",
                          "--startstreaming","--minimize-to-tray"
        Write-Host "OBS started (profile VR180Mirror, streaming to WHIP)"
    }
}

# ---- health: if OBS is up but not publishing (gave up retrying), bounce it -----
if (-not $NoOBS) {
    $publishing = $false
    foreach ($i in 1..10) {
        Start-Sleep -Seconds 3
        try {
            $code = (Invoke-WebRequest "http://127.0.0.1:9888/vr180/index.m3u8" -UseBasicParsing -TimeoutSec 3).StatusCode
            if ($code -eq 200) { $publishing = $true; break }
        } catch { }
    }
    if (-not $publishing) {
        Write-Host "Ingest silent - restarting OBS stream..." -ForegroundColor Yellow
        Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
            Where-Object { $_.CommandLine -like "*--profile VR180Mirror*" } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false }
        Start-Sleep -Seconds 3
        Start-Process -FilePath "C:\Program Files\obs-studio\bin\64bit\obs64.exe" `
            -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit" `
            -ArgumentList "--multi","--only-bundled-plugins","--disable-shutdown-check",
                          "--profile","VR180Mirror","--collection","VR180Mirror",
                          "--startstreaming","--minimize-to-tray"
    }
}

# ---- summary -----------------------------------------------------------------------
Write-Host ""
Write-Host "================= VR180 SPECTATOR READY ================="  -ForegroundColor Green
Write-Host ""
Write-Host " On the SPECTATOR Quest (same Wi-Fi):"
Write-Host "   Best (WebXR, ~0.3s):  https://${lanIp}:8443/"           -ForegroundColor Cyan
Write-Host "     - accept the certificate warning (Advanced -> proceed)"
Write-Host "     - tap 'Connect to stream', then 'Enter VR'"
Write-Host "   No-cert fallback:     http://${lanIp}:9889/vr180"        -ForegroundColor Cyan
Write-Host "     - fullscreen the video, set 180 + 3D left-right in the video bar"
Write-Host "   DeoVR (HLS, 2-6s):    open  http://${lanIp}:9080/  in DeoVR"
Write-Host ""
Write-Host " On the PLAYER Quest: play PCVR through SteamVR"
Write-Host "   (Virtual Desktop: set Streaming > OpenXR runtime = SteamVR)"
Write-Host ""
Write-Host " Stop everything:  .\Stop-Spectator.ps1"
Write-Host "=========================================================="
