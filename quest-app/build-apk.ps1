# Builds VR180Spectator.apk with the raw Android toolchain (aapt2 + javac + d8 +
# zipalign + apksigner) - no Gradle needed since the app has zero dependencies.
# Requires: Android SDK (platforms;android-34, build-tools;34.0.0) and a JDK
# (Android Studio's bundled JBR is used if found).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# ---- locate tools ------------------------------------------------------------
$sdk = $env:ANDROID_HOME
if (-not $sdk -or -not (Test-Path $sdk)) { $sdk = "$env:LOCALAPPDATA\Android\Sdk" }
if (-not (Test-Path $sdk)) { throw "Android SDK not found - set ANDROID_HOME" }

$jbr = "D:\Program Files\Android\Android Studio\jbr"
if (-not (Test-Path "$jbr\bin\java.exe")) { $jbr = "C:\Program Files\Android\Android Studio\jbr" }
if (-not (Test-Path "$jbr\bin\java.exe")) { throw "JDK not found (Android Studio JBR)" }

$env:JAVA_HOME = $jbr
$env:PATH = "$jbr\bin;$env:PATH"

$platform = Get-ChildItem "$sdk\platforms\android-*" | Sort-Object Name -Descending | Select-Object -First 1
$bt = Get-ChildItem "$sdk\build-tools\*" -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $platform -or -not $bt) { throw "Install SDK packages first: platforms;android-34 build-tools;34.0.0" }
$androidJar = "$($platform.FullName)\android.jar"
Write-Host "SDK: $sdk"
Write-Host "platform: $($platform.Name)  build-tools: $($bt.Name)"

$out = "$root\build"
if (Test-Path $out) { Remove-Item $out -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force "$out\classes" | Out-Null

# ---- compile java ---------------------------------------------------------------
# android.jar as -classpath (not bootclasspath): lambdas need the JDK's
# LambdaMetafactory at compile time; d8 desugars them for Android afterwards.
& "$jbr\bin\javac.exe" -source 11 -target 11 -Xlint:-options -classpath $androidJar `
    -d "$out\classes" (Get-ChildItem "$root\src" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

# ---- dex ------------------------------------------------------------------------
$classFiles = Get-ChildItem "$out\classes" -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& "$($bt.FullName)\d8.bat" --min-api 29 --lib $androidJar --output $out $classFiles
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }

# ---- package --------------------------------------------------------------------
& "$($bt.FullName)\aapt2.exe" link -o "$out\base.apk" --manifest "$root\AndroidManifest.xml" -I $androidJar
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

Push-Location $out
& "$jbr\bin\jar.exe" --update --file base.apk classes.dex
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "adding classes.dex failed" }

& "$($bt.FullName)\zipalign.exe" -f 4 "$out\base.apk" "$out\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

# ---- sign (debug keystore) --------------------------------------------------------
$ks = "$env:USERPROFILE\.android\debug.keystore"
if (-not (Test-Path $ks)) {
    & "$jbr\bin\keytool.exe" -genkeypair -keystore $ks -storepass android -keypass android `
        -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -validity 10000
}
& "$($bt.FullName)\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --ks-key-alias androiddebugkey --out "$root\VR180Spectator.apk" "$out\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host ""
Write-Host "Built: $root\VR180Spectator.apk" -ForegroundColor Green
Write-Host "Install: adb install -r `"$root\VR180Spectator.apk`"  (or drag into SideQuest)"
