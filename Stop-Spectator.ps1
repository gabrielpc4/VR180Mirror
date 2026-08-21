# Stops only the VR180Mirror pipeline processes (matched by command line),
# leaving any other OBS/MediaMTX/node instances on the system untouched.
param([string]$Serial = "")
$ErrorActionPreference = "SilentlyContinue"

$failed = $false
function Stop-Ours([string]$name, [string]$cmdLike) {
    Get-CimInstance Win32_Process -Filter "Name = '$name'" |
        Where-Object { $_.CommandLine -like "*$cmdLike*" } |
        ForEach-Object {
            Write-Host "Stopping $($_.Name) (pid $($_.ProcessId))"
            try {
                Stop-Process -Id $_.ProcessId -Force -Confirm:$false -ErrorAction Stop
            } catch {
                $script:failed = $true
            }
        }
}

# Stop the headset client first so it does not sit on a stale last frame.
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if ($adb) {
    $devices = @((& $adb devices) -split "`n" | Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" } | ForEach-Object { ($_ -split "\t")[0] })
    $target = $Serial
    if (-not $target -and $devices.Count -eq 1) { $target = $devices[0] }
    if ($target) {
        Write-Host "Stopping spectator app on $target"
        & $adb -s $target shell am force-stop com.gabriel.vr180native | Out-Null
    }
}

# Elevated OBS runs under this scheduled task; ending it requires no UI prompt.
try { Stop-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue } catch {}
Stop-Ours "obs64.exe"       "--profile VR180Mirror"
Stop-Ours "VR180Mirror.exe" "VR180Mirror"
Stop-Ours "node.exe"        "VR180Mirror\web\server.js"
Stop-Ours "mediamtx.exe"    "VR180Mirror"

# Do not open a UAC or confirmation prompt during one-click shutdown. The
# scheduled-task stop normally owns the elevated OBS process; report a failure
# only if an independently elevated matching process survived.
if ($failed) {
    Write-Warning "One or more matching processes could not be stopped without elevation."
}
Write-Host "Done."
