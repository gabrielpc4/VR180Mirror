# Stops only the VR180Mirror pipeline processes (matched by command line),
# leaving any other OBS/MediaMTX/node instances on the system untouched.
$ErrorActionPreference = "SilentlyContinue"

function Stop-Ours([string]$name, [string]$cmdLike) {
    Get-CimInstance Win32_Process -Filter "Name = '$name'" |
        Where-Object { $_.CommandLine -like "*$cmdLike*" } |
        ForEach-Object {
            Write-Host "Stopping $($_.Name) (pid $($_.ProcessId))"
            Stop-Process -Id $_.ProcessId -Force -Confirm:$false
        }
}

Stop-Ours "obs64.exe"       "--profile VR180Mirror"
Stop-Ours "VR180Mirror.exe" "VR180Mirror"
Stop-Ours "node.exe"        "VR180Mirror\web\server.js"
Stop-Ours "mediamtx.exe"    "VR180Mirror"
Write-Host "Done."
