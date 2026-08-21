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

function Get-PercentSetting([hashtable]$settings, [string]$name, [int]$fallback) {
    if (-not $settings.ContainsKey($name)) { return $fallback }
    try { return [Math]::Max(0, [Math]::Min(100, [int][Math]::Round([double]$settings[$name]))) }
    catch { return $fallback }
}

function Write-RuntimeSettings([bool]$stabilization, [bool]$dome, [int]$vibrance, [int]$sharpening, [string]$colorProfile) {
    $path = Join-Path $root "bin\runtime.json"
    $settings = Read-RuntimeSettings
    $settings.stabilization = $stabilization
    $settings.dome = $dome
    $settings.vibrance = [Math]::Max(0, [Math]::Min(100, $vibrance))
    $settings.sharpening = [Math]::Max(0, [Math]::Min(100, $sharpening))
    $settings.colorProfile = if ($colorProfile -eq "rec2020") { "rec2020" } else { "rec709" }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($path, ($settings | ConvertTo-Json -Compress), $utf8)
}

$adb = Get-AdbPath
if (-not $Serial) { $Serial = Get-SoleDevice $adb }
$script:appLaunchRequested = $false
$script:startProcess = $null
$script:stopping = $false
$script:lastStartupError = ""

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
$initialVibrance = Get-PercentSetting $runtime "vibrance" 0
$initialSharpening = Get-PercentSetting $runtime "sharpening" 0
$initialColorProfile = if ($runtime.ContainsKey("colorProfile") -and "$($runtime.colorProfile)".ToLowerInvariant() -eq "rec2020") { "rec2020" } else { "rec709" }

$form = New-Object System.Windows.Forms.Form
$form.Text = "VR Spectator Control"
$form.ClientSize = New-Object System.Drawing.Size(520, 540)
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

function New-PercentSlider([string]$caption, [int]$value, [int]$top, [string]$hint) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "$caption  $value%"
    $label.AutoSize = $true
    $label.Location = New-Object System.Drawing.Point(30, $top)
    $form.Controls.Add($label)

    $slider = New-Object System.Windows.Forms.TrackBar
    $slider.Minimum = 0
    $slider.Maximum = 100
    $slider.TickFrequency = 10
    $slider.SmallChange = 1
    $slider.LargeChange = 5
    $slider.Value = $value
    $slider.Size = New-Object System.Drawing.Size(275, 35)
    $slider.Location = New-Object System.Drawing.Point(200, ($top - 8))
    $form.Controls.Add($slider)

    $help = New-Object System.Windows.Forms.Label
    $help.Text = $hint
    $help.ForeColor = [System.Drawing.Color]::FromArgb(160, 168, 180)
    $help.AutoSize = $true
    $help.Location = New-Object System.Drawing.Point(48, ($top + 24))
    $form.Controls.Add($help)
    return @{ Slider = $slider; Label = $label }
}

$colorProfileLabel = New-Object System.Windows.Forms.Label
$colorProfileLabel.Text = "Color profile"
$colorProfileLabel.AutoSize = $true
$colorProfileLabel.Location = New-Object System.Drawing.Point(30, 194)
$form.Controls.Add($colorProfileLabel)

$colorProfileBox = New-Object System.Windows.Forms.ComboBox
$colorProfileBox.DropDownStyle = [System.Windows.Forms.ComboBoxStyle]::DropDownList
[void]$colorProfileBox.Items.Add("Rec.709 (reference)")
[void]$colorProfileBox.Items.Add("Rec.2020 (VD-style)")
$colorProfileBox.SelectedIndex = if ($initialColorProfile -eq "rec2020") { 1 } else { 0 }
$colorProfileBox.Size = New-Object System.Drawing.Size(210, 30)
$colorProfileBox.Location = New-Object System.Drawing.Point(200, 190)
$form.Controls.Add($colorProfileBox)

$colorProfileHelp = New-Object System.Windows.Forms.Label
$colorProfileHelp.Text = "Rec.2020 emulates Virtual Desktop's vivid profile; stream remains SDR Rec.709."
$colorProfileHelp.ForeColor = [System.Drawing.Color]::FromArgb(160, 168, 180)
$colorProfileHelp.AutoSize = $true
$colorProfileHelp.Location = New-Object System.Drawing.Point(48, 220)
$form.Controls.Add($colorProfileHelp)

$vibranceControl = New-PercentSlider "Color vibrance" $initialVibrance 252 "Selective color boost. 0% preserves the source Rec.709 color."
$vibranceSlider = $vibranceControl.Slider
$vibranceLabel = $vibranceControl.Label

$sharpeningControl = New-PercentSlider "Experimental CAS sharpening" $initialSharpening 315 "Off by default. Higher values can create halos; use only for A/B tests."
$sharpeningSlider = $sharpeningControl.Slider
$sharpeningLabel = $sharpeningControl.Label

$startButton = New-Object System.Windows.Forms.Button
$startButton.Text = "START / RECONNECT"
$startButton.Size = New-Object System.Drawing.Size(218, 44)
$startButton.Location = New-Object System.Drawing.Point(28, 377)
$startButton.FlatStyle = "Flat"
$startButton.BackColor = [System.Drawing.Color]::FromArgb(41, 121, 255)
$startButton.ForeColor = [System.Drawing.Color]::White
$startButton.FlatAppearance.BorderSize = 0
$form.Controls.Add($startButton)

$stopButton = New-Object System.Windows.Forms.Button
$stopButton.Text = "STOP EVERYTHING"
$stopButton.Size = New-Object System.Drawing.Size(218, 44)
$stopButton.Location = New-Object System.Drawing.Point(270, 377)
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
$status.Size = New-Object System.Drawing.Size(460, 88)
$status.Location = New-Object System.Drawing.Point(28, 439)
$status.Padding = New-Object System.Windows.Forms.Padding(10)
$form.Controls.Add($status)

function Apply-Toggles {
    $profile = if ($colorProfileBox.SelectedIndex -eq 1) { "rec2020" } else { "rec709" }
    Write-RuntimeSettings $stabilizationBox.Checked $domeBox.Checked $vibranceSlider.Value $sharpeningSlider.Value $profile
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

function Get-StartupError {
    if (-not $script:startProcess -or -not $script:startProcess.HasExited) { return "" }
    $stderr = Join-Path $root "bin\control-start.err.log"
    $stdout = Join-Path $root "bin\control-start.log"
    $detail = ""
    foreach ($path in @($stderr, $stdout)) {
        if (Test-Path -LiteralPath $path) {
            $text = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
            if ($text) { $detail += $text + "`n" }
        }
    }
    $detail = ($detail -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 3) -join " | "
    if (-not $detail) { $detail = "Startup stopped with exit code $($script:startProcess.ExitCode)." }
    return $detail
}

function Stop-Workflow {
    if ($script:stopping) { return }
    $script:stopping = $true
    $startButton.Enabled = $false
    $stopButton.Enabled = $false
    $status.Text = "Stopping every spectator process..."
    $form.Refresh()
    try {
        if ($script:startProcess -and -not $script:startProcess.HasExited) {
            & "$env:SystemRoot\System32\taskkill.exe" /PID $script:startProcess.Id /T /F | Out-Null
        }
        $stopScript = Join-Path $root "Stop-Spectator.ps1"
        $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$stopScript`"")
        if ($Serial) { $arguments += @("-Serial", "`"$Serial`"") }
        Start-Process -FilePath "powershell.exe" -ArgumentList $arguments `
            -WorkingDirectory $root -WindowStyle Hidden -Wait
    } catch {
        # This stays visible in the UI while it closes, and never opens a
        # separate terminal or confirmation dialog.
        $status.Text = "Shutdown warning: $($_.Exception.Message)"
    }
}

$startButton.Add_Click({
    try { Start-Workflow } catch { $status.Text = "Startup error: $($_.Exception.Message)" }
})

$stabilizationBox.Add_CheckedChanged({
    try { Apply-Toggles } catch { $status.Text = "Could not update stabilization: $($_.Exception.Message)" }
})
$domeBox.Add_CheckedChanged({
    try { Apply-Toggles } catch { $status.Text = "Could not update dome mode: $($_.Exception.Message)" }
})
$colorProfileBox.Add_SelectedIndexChanged({
    try { Apply-Toggles } catch { $status.Text = "Could not update color profile: $($_.Exception.Message)" }
})
$vibranceSlider.Add_ValueChanged({
    $vibranceLabel.Text = "Color vibrance  $($vibranceSlider.Value)%"
    try { Apply-Toggles } catch { $status.Text = "Could not update color vibrance: $($_.Exception.Message)" }
})
$sharpeningSlider.Add_ValueChanged({
    $sharpeningLabel.Text = "Experimental CAS sharpening  $($sharpeningSlider.Value)%"
    try { Apply-Toggles } catch { $status.Text = "Could not update experimental CAS sharpening: $($_.Exception.Message)" }
})

$stopButton.Add_Click({
    Stop-Workflow
    $form.Close()
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
    if ($script:stopping) { return }
    $startupError = Get-StartupError
    if ($startupError -and $startupError -ne $script:lastStartupError) {
        $script:lastStartupError = $startupError
        $status.Text = "Startup error:`r`n$startupError"
        return
    }
    try {
        $info = Invoke-RestMethod -Uri "http://127.0.0.1:9080/info" -TimeoutSec 1
        $mode = if ($info.dome) { "VR180 dome" } else { "$($info.hspan)x$($info.vspan) degree viewport" }
        $stabilized = if ($info.stabilization) { "ON" } else { "OFF" }
        $stream = if ($info.ready) { "LIVE" } else { "waiting for OBS" }
        $profileText = if ($info.colorProfile -eq "rec2020") { "Rec.2020" } else { "Rec.709" }
        $status.Text = "$stream  |  $mode`r`n$profileText  |  Vibrance $($info.vibrance)%  |  CAS $($info.sharpening)%`r`nStabilization $stabilized  |  Source $($info.srcFps) fps  |  device $(if ($Serial) { $Serial } else { 'not found' })"

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
$form.Add_FormClosing({
    if (-not $script:stopping) { Stop-Workflow }
})

[void]$form.ShowDialog()
