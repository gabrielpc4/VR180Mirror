# One-time setup for the VR180 Spectator console.
#
# The console runs elevated: OBS then inherits higher GPU scheduling priority,
# and - more importantly - every process it starts lives in its job object, so
# closing (or force-killing) the console terminates the whole pipeline.
#
# The desktop shortcut points directly at the exe. Its manifest requests
# requireAdministrator, so Windows still shows one UAC prompt each time you
# launch it - the user chose that over the alternative (an on-demand
# scheduled task with RunLevel Highest, which skips the prompt entirely but
# makes the shortcut's target a schtasks command instead of the exe itself).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exe  = Join-Path $root "tools\VR180Console.exe"

if (-not (Test-Path $exe)) { & "$root\tools\build-console.ps1" }

# desktop shortcut -> the exe directly (one UAC prompt per launch)
$desktop = [Environment]::GetFolderPath("Desktop")
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut("$desktop\VR180 Spectator.lnk")
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = Split-Path $exe
$lnk.IconLocation = "$env:SystemRoot\System32\shell32.dll,15"
$lnk.Description = "VR180 Spectator - one window; closing it stops the whole pipeline"
$lnk.Save()
Write-Host "Desktop shortcut updated -> VR180 Spectator"

# remove the old scheduled-task-based launch path if it was ever registered
if (Get-ScheduledTask -TaskName "VR180 Console" -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName "VR180 Console" -Confirm:$false
    Write-Host "Removed the old 'VR180 Console' scheduled task."
}
Write-Host ""
Write-Host "Done. Use the desktop shortcut from now on." -ForegroundColor Green
Start-Sleep 4
