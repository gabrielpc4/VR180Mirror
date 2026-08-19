# Stops only the VR180Mirror pipeline processes (matched by command line),
# leaving any other OBS/MediaMTX/node instances on the system untouched.
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

# elevated OBS runs under this scheduled task; stopping the task stops it cleanly
try { Stop-ScheduledTask -TaskName "VR180Mirror OBS" -ErrorAction SilentlyContinue } catch {}
Stop-Ours "obs64.exe"       "--profile VR180Mirror"
Stop-Ours "VR180Mirror.exe" "VR180Mirror"
Stop-Ours "node.exe"        "VR180Mirror\web\server.js"
Stop-Ours "mediamtx.exe"    "VR180Mirror"

# elevated OBS (scheduled task) can't be killed from a normal shell - re-run elevated
if ($failed) {
    Write-Host "Elevated OBS needs a UAC confirmation to stop..."
    Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy","Bypass","-Command",
        "Get-CimInstance Win32_Process -Filter `"Name = 'obs64.exe'`" | Where-Object { `$_.CommandLine -like '*--profile VR180Mirror*' } | ForEach-Object { Stop-Process -Id `$_.ProcessId -Force -Confirm:`$false }"
}
Write-Host "Done."
