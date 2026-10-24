# Kirby & The Amazing Mirror Android port

This project builds an arm64 Android app around the generated Kirby game code
and the GBARecomp runtime. The app does not include a ROM or BIOS. On first
launch, choose your own Kirby & The Amazing Mirror (USA) ROM and a clean GBA
BIOS; the app verifies both files before running them. HLE BIOS services are
enabled, but this runtime still requires a BIOS image to be loaded.

## Build the Android APK

Install JDK 17 and Android SDK Platform 35, NDK 27.1.12297006, and CMake
3.22.1. Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` if the SDK is not installed in
the standard Windows location, then run in PowerShell:

```powershell
.\build-android.ps1
```

The script selects JDK 17 when available and builds the debug APK at
`android\app\build\outputs\apk\debug\app-debug.apk`. Install it with:

```powershell
adb install -r .\android\app\build\outputs\apk\debug\app-debug.apk
```

If PowerShell blocks local scripts, invoke it with a process-scoped policy
override:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build-android.ps1
```

## Display and controls

The game renders at its native 240x160 GBA resolution. SDL scales it
proportionally to the landscape phone display, preserving the game image rather
than stretching it to 1920x1080. The on-screen controls use the supplied button
art from `assets` and support multitouch.

The original transparent PNG artwork is in the sibling `botones` folder. The
BMP copies in `assets` are packaged for the runtime to display.
