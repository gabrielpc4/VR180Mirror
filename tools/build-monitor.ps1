# Compiles BandwidthMonitor.exe with the .NET Framework compiler that ships
# with every Windows installation - no SDK or packages needed.
$ErrorActionPreference = "Stop"
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw ".NET Framework compiler not found" }
& $csc /nologo /target:winexe /optimize+ `
    /out:"$PSScriptRoot\BandwidthMonitor.exe" `
    /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll `
    "$PSScriptRoot\BandwidthMonitor.cs"
if ($LASTEXITCODE -ne 0) { throw "csc failed" }
Write-Host "Built: $PSScriptRoot\BandwidthMonitor.exe"
