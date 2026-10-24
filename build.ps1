$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakeSource = if ($cmake) { $cmake.Source.Replace("\", "/") }

# MSYS/Cygwin CMake rewrites Windows paths passed by PowerShell. Prefer a
# native CMake, including the copy bundled with Visual Studio.
if ($cmake -and $cmakeSource -notmatch "/(msys[^/]*|cygwin[^/]*)/" -and
    $cmakeSource -notmatch "/Git/usr/bin/") {
    $cmakePath = $cmake.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $cmakePath = & $vswhere -latest -products * `
            -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" |
            Select-Object -First 1
    }
}

if (-not $cmakePath) {
    throw "Native Windows CMake 3.20+ was not found. Install CMake or the Visual Studio C++ CMake tools, then run this script again."
}

& $cmakePath -S $root -B "$root/build"
& $cmakePath --build "$root/build" --config Release --parallel
