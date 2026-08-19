# Builds VR180Console.exe with the .NET Framework compiler that ships with
# Windows - no SDK, no packages.
$ErrorActionPreference = "Stop"
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw ".NET Framework compiler not found" }
& $csc /nologo /target:winexe /optimize+ `
    /out:"$PSScriptRoot\VR180Console.exe" `
    /win32manifest:"$PSScriptRoot\vr180console.manifest" `
    /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll `
    "$PSScriptRoot\VR180Console.cs"
if ($LASTEXITCODE -ne 0) { throw "csc failed" }
Write-Host "Built: $PSScriptRoot\VR180Console.exe"
