# Android build

This is an arm64 Android application using the shared SDL2/GBARecomp Android
runtime. It does not bundle a ROM or BIOS. On first launch, choose your own
Kirby ROM and a clean GBA BIOS. HLE BIOS services are enabled, but this runtime
still requires the BIOS image file.

## Requirements

- JDK 17
- Android SDK Platform 35
- Android NDK 27.1.12297006
- Android CMake 3.22.1

Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` to your SDK directory, then run the
build script from PowerShell in the `port` directory:

```powershell
.\build-android.ps1
```

The script uses JDK 17 if it is installed and runs Gradle with two native
compile jobs. If local script execution is blocked, use a process-scoped
policy override:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build-android.ps1
```

The APK is created at `android\app\build\outputs\apk\debug\app-debug.apk`.
Install it with `adb install -r` or copy it to the phone and install it there.
The app asks you to choose a local ROM and BIOS on first launch; it does not
package or distribute either file.

The game runs at its native 240x160 GBA resolution. SDL scales the image
proportionally in landscape, preserving its aspect ratio instead of stretching
it to 1920x1080. The supplied touch-control artwork from `..\assets` is shown
around the game image and supports multitouch.
