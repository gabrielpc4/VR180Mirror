[CmdletBinding()]
param([string]$Name = "VR Spectator Control")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::Desktop)
$shortcutPath = Join-Path $desktop "$Name.lnk"
$powershell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$control = Join-Path $root "Spectator-Control.ps1"
$icon = Join-Path $root "bin\VR180Mirror.exe"

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $powershell
$shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -STA -File `"$control`""
$shortcut.WorkingDirectory = $root
$shortcut.Description = "Start and control the VR spectator pipeline"
if (Test-Path -LiteralPath $icon) { $shortcut.IconLocation = "$icon,0" }
$shortcut.Save()

Write-Host "Desktop shortcut created: $shortcutPath"
