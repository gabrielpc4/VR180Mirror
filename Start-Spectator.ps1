# =============================================================================
# Start-Spectator.ps1 - one-click start for the VR180 spectator pipeline
#
#   VR180Mirror.exe  (SteamVR mirror -> VR180 SBS window)
#     -> OBS (Game Capture -> NVENC HEVC -> RTMP)
#       -> MediaMTX (LL-HLS)
#         -> Quest browser, over USB only (adb reverse tcp:9080)
#
# USB cable requirement: this pipeline pushes ~150 Mbps of video over the adb
# tunnel, so the cable and the PC port both need to be USB 3.0 (a USB 2.0
# cable/port caps out around 480 Mbps shared and will starve the stream).
#
# Usage:
#   .\Start-Spectator.ps1              normal run
#   .\Start-Spectator.ps1 -TestGrid    render calibration grid (no SteamVR needed)
#   .\Start-Spectator.ps1 -NoOBS       start everything except OBS
# =============================================================================
[CmdletBinding()]
param(
    [switch]$TestGrid,
    # Capture VaM's native Oculus submission through Virtual Desktop instead
    # of connecting to SteamVR.  Start this first, then launch
    # "VaM (Virtual Desktop).bat" so the hook is armed before Unity resolves
    # the LibOVR runtime.
    [switch]$OculusVaM,
    [switch]$NoOBS,
    # hevc (default): 6144x3264@72 native, hardware decode via the page's
    # buffered WebCodecs path. av1/h264 remain valid OBS encoder choices at
    # lower canvases if you need to trade resolution for headroom.
    [ValidateSet("av1", "h264", "hevc")]
    [string]$Codec = "hevc",
    # target bitrate in kbps (also the floor when -MaxBitrate is set).
    # 0 = codec default (hevc/av1: 150000, h264: 80000)
    [int]$Bitrate = 0,
    # optional ceiling in kbps: when > Bitrate, the encoder runs VBR between
    # the two; omitted = constant bitrate at -Bitrate
    [int]$MaxBitrate = 0,
    # source render rate as a multiple of the stream fps (see below)
    [double]$SourceFpsScale = 1.0,
    # Strong source-pose stabilization. The desktop control panel enables this
    # by default; direct script launches remain explicit.
    [switch]$Stabilization,
    # Classic 180x180 dome. Off by default because exact FOV-fit is sharper.
    [switch]$VR180Dome,
    # adb device serial to target when more than one Quest is plugged in over
    # USB (see `adb devices`); default picks the sole device if only one is attached
    [string]$Serial = "",
    # do the setup (OBS config, USB tunnels) but start nothing:
    # VR180Console.exe owns the processes so it can guarantee they all die with it
    [switch]$ProvisionOnly
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Establish deterministic runtime defaults before the mirror starts. The same
# file is watched live, so the desktop UI can change either option later.
$runtimePath = Join-Path $root "bin\runtime.json"
$runtime = @{}
if (Test-Path -LiteralPath $runtimePath) {
    try {
        $existing = Get-Content -LiteralPath $runtimePath -Raw | ConvertFrom-Json
        foreach ($property in $existing.PSObject.Properties) { $runtime[$property.Name] = $property.Value }
    } catch { $runtime = @{} }
}
$runtime.stabilization = [bool]$Stabilization
$runtime.dome = [bool]$VR180Dome
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($runtimePath, ($runtime | ConvertTo-Json -Compress), $utf8NoBom)

if ($OculusVaM -and $TestGrid) {
    throw "-OculusVaM and -TestGrid are mutually exclusive."
}

# A native-Oculus/Virtual Desktop run must not leave a SteamVR runtime behind:
# VaM can otherwise select its OpenVR rig and make a seemingly-good capture
# test an accidental SteamVR fallback.
if ($OculusVaM) {
    $steamVrProcesses = @(Get-Process -Name 'vrserver', 'vrcompositor' -ErrorAction SilentlyContinue)
    if ($steamVrProcesses.Count -gt 0) {
        $names = ($steamVrProcesses | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
        throw "-OculusVaM requires SteamVR to be fully exited. Still running: $names"
    }
}

# OBS Game Capture and RivaTuner Statistics Server both hook DXGI Present.
# Co-injection freezes OBS on one frame, so refuse to launch or reuse our
# mirror unless RTSS is excluded from this executable.
function Test-RtssMirrorExclusion([string]$profilePath) {
    if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) { return $false }

    $inHooking = $false
    $hookingSections = 0
    $hookingAssignments = 0
    try { $profileLines = Get-Content -LiteralPath $profilePath } catch { return $false }
    foreach ($line in $profileLines) {
        $setting = $line.Trim()
        if ($setting.StartsWith("[") -and $setting.EndsWith("]")) {
            $inHooking = $setting -ieq "[Hooking]"
            if ($inHooking) {
                $hookingSections++
                if ($setting -cne "[Hooking]") { return $false }
            }
            continue
        }
        if ($inHooking -and $setting -imatch '^EnableHooking\s*=') {
            $hookingAssignments++
            if ($setting -cne "EnableHooking=0") { return $false }
        }
    }
    # RTSS INI names are case-insensitive. Require exactly one canonical
    # section and assignment so a duplicate or case-variant cannot override it.
    return $hookingSections -eq 1 -and $hookingAssignments -eq 1
}

function Get-RepoMirrorProcesses([string]$mirrorExe) {
    $targetPath = [IO.Path]::GetFullPath($mirrorExe)
    foreach ($candidate in @(Get-Process -Name "VR180Mirror" -ErrorAction SilentlyContinue)) {
        try { $candidatePath = $candidate.Path } catch { $candidatePath = $null }
        if (-not $candidatePath) {
            if (Get-Process -Id $candidate.Id -ErrorAction SilentlyContinue) {
                throw "Cannot verify the executable path for VR180Mirror PID $($candidate.Id). Stop it, rerun this launcher at the same privilege level, and try again."
            }
            continue
        }
        try { $candidatePath = [IO.Path]::GetFullPath($candidatePath) } catch {
            throw "Cannot normalize the executable path for VR180Mirror PID $($candidate.Id). Stop it and try again."
        }
        if ($candidatePath -ieq $targetPath) { $candidate }
    }
}

function Assert-RtssCaptureSafe([string]$repoRoot) {
    $mirrorExe = Join-Path $repoRoot "bin\VR180Mirror.exe"

    foreach ($mirrorProcess in (Get-RepoMirrorProcesses $mirrorExe)) {
        try {
            $rtssHook = $mirrorProcess.Modules |
                Where-Object { $_.ModuleName -ieq "RTSSHooks64.dll" } |
                Select-Object -First 1
        } catch {
            # Ignore a normal exit race. If the process is still alive, fail
            # closed because its injected modules could not be verified.
            if (Get-Process -Id $mirrorProcess.Id -ErrorAction SilentlyContinue) {
                throw "Cannot verify whether repo VR180Mirror PID $($mirrorProcess.Id) has the RTSS hook loaded. Stop it, rerun this launcher at the same privilege level, and try again."
            }
            continue
        }
        if ($rtssHook) {
            throw @"
Repo VR180Mirror PID $($mirrorProcess.Id) already has RTSSHooks64.dll loaded. RTSS exclusions apply only when a process starts.
Gracefully close that VR180Mirror process, confirm its RTSS profile uses Application detection level 'None', then rerun this launcher.
"@
        }
    }

    $rtssInjectors = @(Get-Process -Name "RTSS", "RTSSHooksLoader64", "RTSSHooksLoader" `
        -ErrorAction SilentlyContinue)
    if (-not $rtssInjectors) { return }

    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    if (-not $programFilesX86) { $programFilesX86 = $env:ProgramFiles }
    $profilePath = Join-Path $programFilesX86 "RivaTuner Statistics Server\Profiles\VR180Mirror.exe.cfg"
    if (-not (Test-RtssMirrorExclusion $profilePath)) {
        throw @"
RTSS or its hook loader is running without the required VR180Mirror exclusion. Its DXGI hook freezes OBS Game Capture on one frame.
Either exit RTSS (and its hook loader) before rerunning, or add '$mirrorExe' in RTSS and set Application detection level to 'None'.
Expected profile: '$profilePath'
Expected exact setting under [Hooking]: EnableHooking=0
RTSS can remain enabled for every other application.
"@
    }
}

Assert-RtssCaptureSafe $root

# Our OBS may run elevated (scheduled task) - its command line is invisible to
# non-admin WMI, so check the task state first.
function Test-OurObs {
    $t = Get-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue
    if ($t -and $t.State -eq "Running") { return $true }
    return [bool](Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" |
        Where-Object { $_.CommandLine -like "*--profile VR180Mirror*" })
}

$fps = 72
# 6144x3264 is the native target: per-eye 3072x3264, measured via
# IVRSystem::GetRecommendedRenderTargetSize() as exactly what SteamVR asks the
# game to render at with Virtual Desktop's Godlike profile and the SteamVR
# Video tab at 100% (VR180Mirror.exe logs this on connect: "SteamVR recommended
# render target: WxH per eye" - re-measure if you change either setting). Using
# that exact size means our canvas neither crops nor stretches the game's own
# render before encoding - an earlier guess of 3072 tall (this game's actual
# per-eye height is 3264) was silently cropping ~6% of vertical resolution.
# (The old 4096 cap came from XRMediaBinding's fast path; we render the dome
# ourselves now, so that limit no longer applies.)
# Bitrate is 150 Mbps, not higher, and that is a decoder decision rather than a
# bandwidth one: the USB tunnel moves a 24MB segment in 0.12s (~1.6 Gbps), but
# HEVC entropy decoding scales with bits, and at 200-240 Mbps the headset
# decoder fell behind 72fps - the backlog grew and latency went 0.6s -> 1.7-2.6s.
# At 150 Mbps it holds 72fps with ~0.6s latency at the same native resolution.
if ($Codec -eq "av1")      { $canvasW = 4096; $canvasH = 2048; $encoderId = "obs_nvenc_av1_tex";  $defBitrate = 150000 }
elseif ($Codec -eq "hevc") { $canvasW = 6144; $canvasH = 3264; $encoderId = "obs_nvenc_hevc_tex"; $defBitrate = 150000 }
else                  { $canvasW = 3840; $canvasH = 1920; $encoderId = "obs_nvenc_h264_tex"; $defBitrate = 100000 }
if ($Bitrate -le 0) { $Bitrate = $defBitrate }

# ---- first-run provisioning ----------------------------------------------------
if ($OculusVaM -or -not (Test-Path "$root\bin\VR180Mirror.exe")) {
    $buildDetail = if ($OculusVaM) { " with the VaM Oculus capture hook" } else { "" }
    Write-Host "Building VR180Mirror$buildDetail..."
    & "$root\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw "VR180Mirror build failed" }
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

# ---- USB only: no LAN IP, no cert, no firewall rules needed ------------------
# The web server is reached at http://localhost:9080 through an adb-reverse
# tunnel, so there is no Wi-Fi listener to advertise or open a port for.
$lanIp = "127.0.0.1"

# ---- helpers ------------------------------------------------------------------
function Get-OurProcess([string]$nameLike, [string]$cmdLike) {
    Get-CimInstance Win32_Process -Filter "Name = '$nameLike'" |
        Where-Object { $_.CommandLine -like "*$cmdLike*" }
}

function Get-OurMirrorProcess {
    $mirrorPath = [IO.Path]::GetFullPath((Join-Path $root 'bin\VR180Mirror.exe'))
    Get-CimInstance Win32_Process -Filter "Name = 'VR180Mirror.exe'" |
        Where-Object { $_.ExecutablePath -and
            ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $mirrorPath) }
}

# ---- USB spectator: establish the adb reverse tunnel for the target headset ----
# Must run before the -ProvisionOnly early-exit below: it's what makes
# http://localhost:9080 on the Quest resolve to this PC's web server at all.
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) {
    $adb = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
$targetSerial = $Serial
if ($adb) {
    try {
        $dev = (& $adb devices) -split "`n" | Select-Object -Skip 1 | Where-Object { $_ -match "\tdevice$" } |
            ForEach-Object { ($_ -split "\t")[0] }
        if (-not $targetSerial) {
            if (@($dev).Count -eq 1) { $targetSerial = $dev }
            elseif (@($dev).Count -gt 1) {
                Write-Host "Multiple Quest headsets on USB - pass -Serial <id> to pick one. Devices: $($dev -join ', ')" -ForegroundColor Yellow
            }
        }
        if ($targetSerial) {
            $sArgs = @("-s", $targetSerial)
            & $adb @sArgs reverse tcp:9080 tcp:9080 | Out-Null
            Write-Host "USB spectator tunnel active (adb -s $targetSerial reverse tcp:9080)"
        } else {
            Write-Host "No USB Quest headset detected - plug it in and reverse the tunnel manually if needed" -ForegroundColor Yellow
        }
    } catch { }
} else {
    Write-Host "adb not found - USB spectator tunnel not established" -ForegroundColor Yellow
}

if ($ProvisionOnly) {
    Write-Host "Provisioned: $Codec ${canvasW}x${canvasH}@${fps} ${Bitrate}kbps$(if ($Serial) { ", device $Serial" })"
    exit 0
}

# ---- MediaMTX ------------------------------------------------------------------
if (Get-OurProcess "mediamtx.exe" "VR180Mirror") {
    Write-Host "MediaMTX (VR180Mirror) already running"
} else {
    Start-Process -FilePath "$root\tools\mediamtx\mediamtx.exe" `
        -ArgumentList "`"$root\tools\mediamtx\mediamtx.yml`"" `
        -WorkingDirectory "$root\tools\mediamtx" -WindowStyle Hidden
    Write-Host "MediaMTX started (RTMP ingest :1936, LL-HLS :9888)"
}

# ---- web server -----------------------------------------------------------------
if (Get-OurProcess "node.exe" "VR180Mirror\web\server.js") {
    Write-Host "Web server already running"
} else {
    $webArgs = @("`"$root\web\server.js`"")
    if ($Serial) { $webArgs += "--serial", $Serial }
    Start-Process -FilePath "node" -ArgumentList $webArgs `
        -WorkingDirectory "$root\web" -WindowStyle Hidden
    Write-Host "Web server started (http :9080)"
}

# ---- VR180Mirror -----------------------------------------------------------------
$activeMirror = @(Get-OurMirrorProcess)
if ($activeMirror) {
    $wantedMode = if ($OculusVaM) { "Oculus/Virtual Desktop" } else { "SteamVR" }
    $hasOculusMode = @($activeMirror | Where-Object { $_.CommandLine -match '(^|\s)--oculus-vam(\s|$)' }).Count -gt 0
    if (($OculusVaM -and -not $hasOculusMode) -or (-not $OculusVaM -and $hasOculusMode)) {
        throw "VR180Mirror is already running in the other capture mode. Gracefully close it (or run Stop-Spectator.ps1), then rerun this launcher for $wantedMode."
    }
    Write-Host "VR180Mirror already running ($wantedMode)"
} else {
    # Source render rate. It matched the stream rate originally, was raised to 2x
    # while the render loop still had stalls (a stalled loop makes the capture
    # sample the same frame twice), and is back to 1x now that the stalls are
    # gone: both our pacing and OBS derive from QPC, so at equal rates the phase
    # barely drifts. 2x also costs the game GPU time - the capture hook copies
    # the full 4096x2048 backbuffer on every present. Raise -SourceFpsScale if
    # held frames ever reappear.
    $mirrorArgs = @("--size","${canvasW}x${canvasH}","--fps","$([int]($fps * $SourceFpsScale))","--preview","1280")
    if ($TestGrid) { $mirrorArgs += "--test-grid" }
    if ($OculusVaM) { $mirrorArgs += "--oculus-vam" }
    Start-Process -FilePath "$root\bin\VR180Mirror.exe" -ArgumentList $mirrorArgs `
        -WorkingDirectory "$root\bin"
    Write-Host "VR180Mirror started $(if ($OculusVaM) { '(VaM native Oculus / Virtual Desktop)' } elseif ($TestGrid) { '(TEST GRID)' })"
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

# ---- summary -----------------------------------------------------------------------
Write-Host ""
Write-Host "================= VR180 SPECTATOR READY ================="  -ForegroundColor Green
Write-Host ""
Write-Host " Stream: $Codec ${canvasW}x${canvasH} @ ${fps}fps (1.6x native on the Quest)"
Write-Host ""
Write-Host " USB cable ONLY - requires a USB 3.0 cable and a USB 3.0 port on the PC" -ForegroundColor Yellow
Write-Host "   (USB 2.0 cannot carry the ~150 Mbps stream reliably)."                -ForegroundColor Yellow
Write-Host ""
Write-Host " SPECTATOR:"
Write-Host "   plug the spectator Quest into a USB 3.0 port, run .\Connect-SpectatorUSB.ps1,"
Write-Host "   then open  http://localhost:9080/"                       -ForegroundColor Cyan
Write-Host "   (video rides the cable only; Connect -> Enter VR)"
Write-Host ""
if ($OculusVaM) {
    Write-Host " On the PLAYER Quest: launch VaM (Virtual Desktop).bat with SteamVR fully closed" -ForegroundColor Cyan
    Write-Host "   VR180Mirror is armed first and will attach to VaM's native Oculus eye textures."
} else {
    Write-Host " On the PLAYER Quest: play PCVR through SteamVR"
    Write-Host "   (Virtual Desktop: set Streaming > OpenXR runtime = SteamVR)"
}
Write-Host ""
Write-Host " Stop everything:  .\Stop-Spectator.ps1"
Write-Host "=========================================================="
