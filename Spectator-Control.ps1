[CmdletBinding()]
param([string]$Serial = "")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$createdNew = $false
$singleInstance = New-Object System.Threading.Mutex($true, "Local\VR180SpectatorControl", [ref]$createdNew)
if (-not $createdNew) {
    [System.Windows.Forms.MessageBox]::Show("VR Spectator Control is already open.", "VR Spectator Control") | Out-Null
    exit 0
}

function Get-AdbPath {
    $command = Get-Command adb -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $candidate = Get-ChildItem `
        "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe", `
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    return $candidate
}

function Get-SoleDevice([string]$adb) {
    if (-not $adb) { return "" }
    $devices = @((& $adb devices) -split "`n" | Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" } | ForEach-Object { ($_ -split "\t")[0] })
    if ($devices.Count -eq 1) { return $devices[0] }
    return ""
}

function Read-RuntimeSettings {
    $path = Join-Path $root "bin\runtime.json"
    if (-not (Test-Path -LiteralPath $path)) { return @{} }
    try {
        $result = @{}
        $object = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        foreach ($property in $object.PSObject.Properties) { $result[$property.Name] = $property.Value }
        return $result
    } catch { return @{} }
}

function Write-RuntimeSettings([bool]$stabilization, [bool]$dome) {
    $path = Join-Path $root "bin\runtime.json"
    $settings = Read-RuntimeSettings
    $settings.stabilization = $stabilization
    $settings.dome = $dome
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($path, ($settings | ConvertTo-Json -Compress), $utf8)
}

$adb = Get-AdbPath
if (-not $Serial) { $Serial = Get-SoleDevice $adb }
$script:appLaunchRequested = $false
$script:startProcess = $null
$script:stopping = $false

$runtime = Read-RuntimeSettings
$initialStabilization = if ($runtime.ContainsKey("stabilization")) {
    [bool]$runtime.stabilization
} else {
    $true
}
$initialDome = if ($runtime.ContainsKey("dome")) {
    [bool]$runtime.dome
} else {
    $false
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "VR Spectator Control"
$form.ClientSize = New-Object System.Drawing.Size(520, 360)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(25, 27, 31)
$form.ForeColor = [System.Drawing.Color]::White
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Text = "VR SPECTATOR"
$title.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 20)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(24, 18)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "6144 x 3264 HEVC  |  72 Hz  |  USB"
$subtitle.ForeColor = [System.Drawing.Color]::FromArgb(160, 168, 180)
$subtitle.AutoSize = $true
$subtitle.Location = New-Object System.Drawing.Point(27, 58)
$form.Controls.Add($subtitle)

$stabilizationBox = New-Object System.Windows.Forms.CheckBox
$stabilizationBox.Text = "Image stabilization"
$stabilizationBox.Checked = $initialStabilization
$stabilizationBox.AutoSize = $true
$stabilizationBox.Location = New-Object System.Drawing.Point(30, 99)
$form.Controls.Add($stabilizationBox)

$domeBox = New-Object System.Windows.Forms.CheckBox
$domeBox.Text = "VR180 dome (lower sharpness)"
$domeBox.Checked = $initialDome
$domeBox.AutoSize = $true
$domeBox.Location = New-Object System.Drawing.Point(30, 132)
$form.Controls.Add($domeBox)

$defaultLabel = New-Object System.Windows.Forms.Label
$defaultLabel.Text = "Dome mode is OFF by default, remembered when you enable it. Changes apply live."
$defaultLabel.ForeColor = [System.Drawing.Color]::FromArgb(160, 168, 180)
$defaultLabel.AutoSize = $true
$defaultLabel.Location = New-Object System.Drawing.Point(48, 158)
$form.Controls.Add($defaultLabel)

$startButton = New-Object System.Windows.Forms.Button
$startButton.Text = "START / RECONNECT"
$startButton.Size = New-Object System.Drawing.Size(218, 44)
$startButton.Location = New-Object System.Drawing.Point(28, 196)
$startButton.FlatStyle = "Flat"
$startButton.BackColor = [System.Drawing.Color]::FromArgb(41, 121, 255)
$startButton.ForeColor = [System.Drawing.Color]::White
$startButton.FlatAppearance.BorderSize = 0
$form.Controls.Add($startButton)

$stopButton = New-Object System.Windows.Forms.Button
$stopButton.Text = "STOP EVERYTHING"
$stopButton.Size = New-Object System.Drawing.Size(218, 44)
$stopButton.Location = New-Object System.Drawing.Point(270, 196)
$stopButton.FlatStyle = "Flat"
$stopButton.BackColor = [System.Drawing.Color]::FromArgb(190, 48, 56)
$stopButton.ForeColor = [System.Drawing.Color]::White
$stopButton.FlatAppearance.BorderSize = 0
$form.Controls.Add($stopButton)

$status = New-Object System.Windows.Forms.Label
$status.Text = "Starting..."
$status.BorderStyle = "FixedSingle"
$status.BackColor = [System.Drawing.Color]::FromArgb(18, 19, 22)
$status.ForeColor = [System.Drawing.Color]::FromArgb(196, 204, 216)
$status.Size = New-Object System.Drawing.Size(460, 78)
$status.Location = New-Object System.Drawing.Point(28, 258)
$status.Padding = New-Object System.Windows.Forms.Padding(10)
$form.Controls.Add($status)

function Apply-Toggles {
    Write-RuntimeSettings $stabilizationBox.Checked $domeBox.Checked
}

function Start-Workflow {
    if ($script:startProcess -and -not $script:startProcess.HasExited) {
        $status.Text = "The startup workflow is already running..."
        return
    }
    Apply-Toggles
    $script:appLaunchRequested = $false
    $startScript = Join-Path $root "Start-Spectator.ps1"
    $stdout = Join-Path $root "bin\control-start.log"
    $stderr = Join-Path $root "bin\control-start.err.log"
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$startScript`"")
    if ($Serial) { $arguments += @("-Serial", "`"$Serial`"") }
    if ($stabilizationBox.Checked) { $arguments += "-Stabilization" }
    if ($domeBox.Checked) { $arguments += "-VR180Dome" }
    $script:startProcess = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments `
        -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $status.Text = "Starting the mirror, OBS, stream server, and spectator app..."
}

$startButton.Add_Click({
    try { Start-Workflow } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "Could not start") | Out-Null
    }
})

$stabilizationBox.Add_CheckedChanged({
    try { Apply-Toggles } catch { $status.Text = "Could not update stabilization: $($_.Exception.Message)" }
})
$domeBox.Add_CheckedChanged({
    try { Apply-Toggles } catch { $status.Text = "Could not update dome mode: $($_.Exception.Message)" }
})

$stopButton.Add_Click({
    if ($script:stopping) { return }
    $script:stopping = $true
    $startButton.Enabled = $false
    $stopButton.Enabled = $false
    $status.Text = "Stopping the headset app, mirror, OBS, and streaming services..."
    $form.Refresh()
    try {
        if ($script:startProcess -and -not $script:startProcess.HasExited) {
            Stop-Process -Id $script:startProcess.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
        }
        $stopScript = Join-Path $root "Stop-Spectator.ps1"
        $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$stopScript`"")
        if ($Serial) { $arguments += @("-Serial", "`"$Serial`"") }
        Start-Process -FilePath "powershell.exe" -ArgumentList $arguments `
            -WorkingDirectory $root -WindowStyle Hidden -Wait
    } finally {
        $form.Close()
    }
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
    if ($script:stopping) { return }
    try {
        $info = Invoke-RestMethod -Uri "http://127.0.0.1:9080/info" -TimeoutSec 1
        $mode = if ($info.dome) { "VR180 dome" } else { "$($info.hspan)x$($info.vspan) degree viewport" }
        $stabilized = if ($info.stabilization) { "ON" } else { "OFF" }
        $stream = if ($info.ready) { "LIVE" } else { "waiting for OBS" }
        $status.Text = "$stream  |  $mode`r`nStabilization $stabilized  |  source $($info.srcFps) fps  |  device $(if ($Serial) { $Serial } else { 'not found' })"

        if ($info.ready -and -not $script:appLaunchRequested -and $adb -and $Serial) {
            $script:appLaunchRequested = $true
            & $adb -s $Serial shell am start -W -a android.intent.action.MAIN `
                -c android.intent.category.LAUNCHER com.gabriel.vr180native | Out-Null
        }
    } catch {
        $status.Text = "Pipeline is starting...`r`nDevice: $(if ($Serial) { $Serial } else { 'connect one Quest over USB' })"
    }
})

$form.Add_Shown({
    # Keep the safe first-run default, but preserve an explicit user choice across panel restarts.
    Apply-Toggles
    Start-Workflow
    $timer.Start()
})
$form.Add_FormClosed({
    $timer.Stop()
    try { $singleInstance.ReleaseMutex() } catch {}
    $singleInstance.Dispose()
})

[void]$form.ShowDialog()
