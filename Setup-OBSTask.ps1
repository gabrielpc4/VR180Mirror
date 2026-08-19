# One-time setup: registers a scheduled task that launches OBS ELEVATED for
# the spectator pipeline. Elevated processes get higher GPU scheduling
# priority on Windows, which stops OBS render/encode starvation while the
# game saturates the GPU (the standard "run OBS as administrator" fix, minus
# the per-launch UAC prompt). Self-elevates once via UAC.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator rights to register the OBS task (UAC prompt)..."
    Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`""
    exit
}

$action = New-ScheduledTaskAction `
    -Execute "C:\Program Files\obs-studio\bin\64bit\obs64.exe" `
    -Argument "--multi --only-bundled-plugins --disable-shutdown-check --disable-updater --profile VR180Mirror --collection VR180Mirror --startstreaming --minimize-to-tray" `
    -WorkingDirectory "C:\Program Files\obs-studio\bin\64bit"
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -RunLevel Highest -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName "VR180Mirror OBS" -Action $action -Principal $principal `
    -Settings $settings -Force | Out-Null
Write-Host "Task 'VR180Mirror OBS' registered (runs OBS elevated, on demand)."
Start-Sleep 3
