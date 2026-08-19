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
    [switch]$NoOBS,
    # hevc (default): 4096x2048@72 FOV-fit, hardware MSE decode via the page's
    # buffered mode (~2-4s). h264: 3840x1920@72 low-latency WebRTC (~0.4s).
    # av1: alternative to hevc for the buffered path.
    [ValidateSet("av1", "h264", "hevc")]
    [string]$Codec = "hevc",
    # target bitrate in kbps (also the floor when -MaxBitrate is set).
    # 0 = codec default (av1: 150000, h264: 80000)
    [int]$Bitrate = 0,
    # optional ceiling in kbps: when > Bitrate, the encoder runs VBR between
    # the two; omitted = constant bitrate at -Bitrate
    [int]$MaxBitrate = 0,
    # source render rate as a multiple of the stream fps (see below)
    [double]$SourceFpsScale = 1.0,
    # classic full-180 canvas (needed for DeoVR; default is FOV-fit = sharper)
    [switch]$VR180
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Our OBS may run elevated (scheduled task) - its command line is invisible to
# non-admin WMI, so check the task state first.
function Test-OurObs {
    $t = Get-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue
    if ($t -and $t.State -eq "Running") { return $true }
    return [bool](Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
        Where-Object { $_.CommandLine -like "*--profile VR180Mirror*" })
}

$fps = 72
# 6144x3072 is the native target: 3072 px per eye over the game's ~114 deg is
# ~27 px/deg, past the Quest 3 panel's ~25 PPD, so the display is the limit
# rather than the stream. It is also ~1:1 with Virtual Desktop Godlike's
# 3072x3216 per-eye render, so the reprojection resamples as little as possible.
# (The old 4096 cap came from XRMediaBinding's fast path; we render the dome
# ourselves now, so that limit no longer applies.)
if ($Codec -eq "av1")      { $canvasW = 4096; $canvasH = 2048; $encoderId = "obs_nvenc_av1_tex";  $defBitrate = 150000 }
elseif ($Codec -eq "hevc") { $canvasW = 6144; $canvasH = 3072; $encoderId = "obs_nvenc_hevc_tex"; $defBitrate = 240000 }
else                  { $canvasW = 3840; $canvasH = 1920; $encoderId = "obs_nvenc_h264_tex"; $defBitrate = 100000 }
if ($Bitrate -le 0) { $Bitrate = $defBitrate }

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
# ---- sync OBS config to the chosen codec/canvas (skipped while our OBS runs) ----
if (-not (Test-OurObs)) {
    $profDir = "$env:APPDATA\obs-studio\basic\profiles\VR180Mirror"
    New-Item -ItemType Directory -Force $profDir | Out-Null
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $ini = Get-Content "$root\obs\basic.ini" -Raw
    $ini = $ini -replace "BaseCX=\d+", "BaseCX=$canvasW" -replace "BaseCY=\d+", "BaseCY=$canvasH" `
                -replace "OutputCX=\d+", "OutputCX=$canvasW" -replace "OutputCY=\d+", "OutputCY=$canvasH" `
                -replace "FPSInt=\d+", "FPSInt=$fps" -replace "Encoder=obs_nvenc_\w+_tex", "Encoder=$encoderId"
    [IO.File]::WriteAllText("$profDir\basic.ini", $ini, $utf8)
    Copy-Item "$root\obs\service.json" $profDir -Force
    $enc = Get-Content "$root\obs\streamEncoder.$Codec.json" -Raw | ConvertFrom-Json
    $enc.bitrate = $Bitrate
    if ($MaxBitrate -gt $Bitrate) {
        $enc.rate_control = "VBR"
        $enc | Add-Member -NotePropertyName max_bitrate -NotePropertyValue $MaxBitrate -Force
    }
    [IO.File]::WriteAllText("$profDir\streamEncoder.json", ($enc | ConvertTo-Json -Compress), $utf8)
    $scene = Get-Content "$root\obs\scene-collection.json" -Raw | ConvertFrom-Json
    $item = ($scene.sources | Where-Object { $_.id -eq "scene" }).settings.items[0]
    $item.scale_ref.x = $canvasW; $item.scale_ref.y = $canvasH
    $item.bounds.x = $canvasW;    $item.bounds.y = $canvasH
    $scene.resolution.x = $canvasW; $scene.resolution.y = $canvasH
    [IO.File]::WriteAllText("$env:APPDATA\obs-studio\basic\scenes\VR180Mirror.json",
        ($scene | ConvertTo-Json -Depth 100), $utf8)
    Write-Host "OBS config synced: $Codec ${canvasW}x${canvasH}@${fps}"
} else {
    Write-Host "OBS already running - config sync skipped (stop it first to change codec/resolution)"
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
        -WorkingDirectory "$root\tools\mediamtx"
    Write-Host "MediaMTX started (WHIP/WHEP :9889, LL-HLS :9888)"
}

# ---- web server -----------------------------------------------------------------
if (Get-OurProcess "node.exe" "VR180Mirror\web\server.js") {
    Write-Host "Web server already running"
} else {
    Start-Process -FilePath "node" -ArgumentList "`"$root\web\server.js`"","--ip",$lanIp `
        -WorkingDirectory "$root\web"                      # visible console
    Write-Host "Web server started (https :8443, http :9080)"
}

# ---- VR180Mirror -----------------------------------------------------------------
if (Get-OurProcess "VR180Mirror.exe" "VR180Mirror") {
    Write-Host "VR180Mirror already running"
} else {
    # Source render rate. It matched the stream rate originally, was raised to 2x
    # while the render loop still had stalls (a stalled loop makes the capture
    # sample the same frame twice), and is back to 1x now that the stalls are
    # gone: both our pacing and OBS derive from QPC, so at equal rates the phase
    # barely drifts. 2x also costs the game GPU time - the capture hook copies
    # the full 4096x2048 backbuffer on every present. Raise -SourceFpsScale if
    # held frames ever reappear.
    $mirrorArgs = @("--size","${canvasW}x${canvasH}","--fps","$([int]($fps * $SourceFpsScale))","--preview","1280")
    if ($VR180) { $mirrorArgs += "--vr180" }
    if ($TestGrid) { $mirrorArgs += "--test-grid" }
    Start-Process -FilePath "$root\bin\VR180Mirror.exe" -ArgumentList $mirrorArgs `
        -WorkingDirectory "$root\bin"
    Write-Host "VR180Mirror started $(if ($TestGrid) { '(TEST GRID)' })"
}

# ---- OBS --------------------------------------------------------------------------
if (-not $NoOBS) {
    if (Test-OurObs) {
        Write-Host "OBS (VR180Mirror profile) already running"
    } else {
        # Prefer the elevated scheduled task: higher GPU scheduling priority so
        # OBS keeps 72fps while the game saturates the GPU.
        if (Get-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue) {
            Start-ScheduledTask -TaskName "VR180Mirror OBS"
            Write-Host "OBS started elevated via scheduled task (GPU-priority boost)"
        } else {
            Write-Host "TIP: run Setup-OBSTask.ps1 once (UAC) so OBS gets GPU priority while gaming." -ForegroundColor Yellow
            Start-Process -FilePath "C:\Program Files\obs-studio\bin\64bit\obs64.exe" `
                -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit" `
                -ArgumentList "--multi","--only-bundled-plugins","--disable-shutdown-check","--disable-updater",
                              "--profile","VR180Mirror","--collection","VR180Mirror",
                              "--startstreaming","--minimize-to-tray"
            Write-Host "OBS started (profile VR180Mirror)"
        }
    }
}

# ---- health: if OBS is up but not publishing (gave up retrying), bounce it -----
if (-not $NoOBS) {
    $publishing = $false
    foreach ($i in 1..10) {
        Start-Sleep -Seconds 3
        try {
            $paths = Invoke-RestMethod "http://127.0.0.1:9998/v3/paths/list" -TimeoutSec 3
            $vr = $paths.items | Where-Object { $_.name -eq "vr180" }
            if ($vr -and $vr.ready) { $publishing = $true; break }
        } catch { }
    }
    if (-not $publishing) {
        Write-Host "Ingest silent - restarting OBS stream..." -ForegroundColor Yellow
        try { Stop-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue } catch {}
        Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
            Where-Object { $_.CommandLine -like "*--profile VR180Mirror*" } |
            ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -Confirm:$false -ErrorAction Stop } catch {} }
        Start-Sleep -Seconds 3
        if (Get-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue) {
            Start-ScheduledTask -TaskName "VR180Mirror OBS"
        } else {
            Start-Process -FilePath "C:\Program Files\obs-studio\bin\64bit\obs64.exe" `
                -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit" `
                -ArgumentList "--multi","--only-bundled-plugins","--disable-shutdown-check","--disable-updater",
                              "--profile","VR180Mirror","--collection","VR180Mirror",
                              "--startstreaming","--minimize-to-tray"
        }
    }
}

# ---- USB spectator: re-establish adb reverse tunnels if a headset is plugged ----
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) {
    $adb = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if ($adb) {
    try {
        $dev = (& $adb devices) -split "`n" | Select-Object -Skip 1 | Where-Object { $_ -match "\tdevice$" }
        if (@($dev).Count -eq 1) {
            & $adb reverse tcp:9080 tcp:9080 | Out-Null
            & $adb reverse tcp:9189 tcp:9189 | Out-Null
            Write-Host "USB spectator tunnels active (adb reverse 9080 + 9189)"
        }
    } catch { }
}

# ---- summary -----------------------------------------------------------------------
Write-Host ""
Write-Host "================= VR180 SPECTATOR READY ================="  -ForegroundColor Green
Write-Host ""
Write-Host " Stream: $Codec ${canvasW}x${canvasH} @ ${fps}fps"
Write-Host ""
Write-Host " SPECTATOR via USB cable (max quality/reliability):"
Write-Host "   plug the spectator Quest into the PC, run .\Connect-SpectatorUSB.ps1,"
Write-Host "   then open  http://localhost:9080/"                       -ForegroundColor Cyan
Write-Host "   (no certificate; video rides the cable; Connect -> Enter VR)"
Write-Host ""
Write-Host " SPECTATOR via Wi-Fi:"
Write-Host "   Best (WebXR, ~0.3s):  https://${lanIp}:8443/"           -ForegroundColor Cyan
Write-Host "     - accept the certificate warning (Advanced -> proceed)"
Write-Host "     - tap 'Connect to stream', then 'Enter VR'"
Write-Host "   No-cert fallback:     http://${lanIp}:9889/vr180"        -ForegroundColor Cyan
Write-Host "     - fullscreen the video, set 180 + 3D left-right in the video bar"
Write-Host "   DeoVR (HLS, 2-6s):    http://${lanIp}:9080/  in DeoVR (start with -Codec h264)"
Write-Host ""
Write-Host " On the PLAYER Quest: play PCVR through SteamVR"
Write-Host "   (Virtual Desktop: set Streaming > OpenXR runtime = SteamVR)"
Write-Host ""
Write-Host " Stop everything:  .\Stop-Spectator.ps1"
Write-Host "=========================================================="
