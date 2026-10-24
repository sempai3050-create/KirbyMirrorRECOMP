$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$android = Join-Path $root "android"
$sdk = $env:ANDROID_SDK_ROOT
if (-not $sdk) {
    $sdk = $env:ANDROID_HOME
}
if (-not $sdk) {
    $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
}

$java = Get-Command java.exe -ErrorAction SilentlyContinue
$jdk17 = Get-ChildItem "C:\Program Files\Eclipse Adoptium" `
    -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "jdk-17*" } |
    Select-Object -First 1
if ($jdk17) {
    $env:JAVA_HOME = $jdk17.FullName
} elseif (-not $env:JAVA_HOME -and $java) {
    $candidate = Split-Path -Parent (Split-Path -Parent $java.Source)
    if (Test-Path (Join-Path $candidate "bin\javac.exe")) {
        $env:JAVA_HOME = $candidate
    }
}

if (-not (Test-Path (Join-Path $sdk "platforms\android-35\android.jar"))) {
    throw "Android SDK Platform 35 is missing from $sdk. Install the packages listed in android\README.md."
}
if (-not (Test-Path (Join-Path $sdk "ndk\27.1.12297006\source.properties"))) {
    throw "Android NDK 27.1.12297006 is missing from $sdk."
}

$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
Push-Location $android
try {
    & .\gradlew.bat :app:assembleDebug -PgbaAbis=arm64-v8a -PgbaNativeJobs=2
    if ($LASTEXITCODE -ne 0) {
        throw "Android build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
