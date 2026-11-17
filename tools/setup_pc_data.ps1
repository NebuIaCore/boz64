# Sets up the Windows version's game folder (default: cod bo mobile\BOZ64 PC, next to com.activision.boz) with
# boz64.exe (tools\build_pc.bat also copies each new build there) and the game files, in the layout the game sees
# on Android:
#   data\data\app\com.activision.boz\base.apk                    the original APK (backup\apk\base.apk)
#   data\data\app\com.activision.boz\lib\arm\*.so                the APK's 32-bit engine and extensions
#   data\sdcard\Android\obb\com.activision.boz\blackops_etc.dz   the game data pack (backup\obb\...)
# and the DLLs it needs: ANGLE (libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll) from a local Chromium-based app
# (Discord by default) and zlib1.dll from Git for Windows. Large files are hard-linked when on the same drive.
# usage: powershell -ExecutionPolicy Bypass -File tools\setup_pc_data.ps1 [-Out <folder>] [-AngleDir <folder>]
param(
    [string]$Out = "",
    [string]$AngleDir = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot          # port64
$Game = Split-Path -Parent $Root                  # com.activision.boz
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Game) "BOZ64 PC" }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Root "build\win64\boz64.exe"
if (Test-Path $exe) { Copy-Item $exe (Join-Path $Out "boz64.exe") -Force; Write-Host "copied boz64.exe" }

function Place($src, $dst) {
    if (-not (Test-Path $src)) { throw "missing: $src" }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    if (Test-Path $dst) {
        if ((Get-Item $dst).Length -eq (Get-Item $src).Length) { return }
        Remove-Item $dst -Force
    }
    try { New-Item -ItemType HardLink -Path $dst -Target $src | Out-Null; Write-Host "linked $dst" }
    catch { Copy-Item $src $dst; Write-Host "copied $dst" }
}

# the APK and its libraries
$apk = Join-Path $Game "backup\apk\base.apk"
$app = Join-Path $Out "data\data\app\com.activision.boz"
Place $apk (Join-Path $app "base.apk")
$libs = Join-Path $app "lib\arm"
New-Item -ItemType Directory -Force $libs | Out-Null
# Left out as in the Android build (tools/package.py): Google Play and Samsung store code.
$drop = @("libs3eGooglePlayServices.so", "libs3eAndroidGooglePlayBilling.so", "libs3eAndroidMarketBilling.so",
          "libs3eSamsungInAppPurchasing.so")
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($apk)
try {
    foreach ($e in $zip.Entries) {
        if ($e.FullName -like "lib/armeabi/*.so" -and $drop -notcontains $e.Name) {
            $dst = Join-Path $libs $e.Name
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst, $true)
            Write-Host "extracted $dst"
        }
    }
} finally { $zip.Dispose() }

# the data pack
$obb = Join-Path $Out "data\sdcard\Android\obb\com.activision.boz"
foreach ($f in @("blackops_etc.dz", "blackops_etc.dz.dat")) {
    Place (Join-Path $Game "backup\obb\com.activision.boz\$f") (Join-Path $obb $f)
}

# ANGLE
if (-not $AngleDir) {
    $AngleDir = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Discord") -Directory -Filter "app-*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Where-Object { Test-Path (Join-Path $_.FullName "libGLESv2.dll") } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $AngleDir) { throw "no ANGLE found: pass -AngleDir <folder with libEGL.dll and libGLESv2.dll>" }
foreach ($f in @("libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll")) {
    $src = Join-Path $AngleDir $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $Out $f) -Force; Write-Host "copied $f from $AngleDir" }
}

# zlib
$z = @("C:\Program Files\Git\mingw64\bin\zlib1.dll", "C:\Program Files\Git\usr\bin\msys-z.dll") | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($z -and (Split-Path -Leaf $z) -eq "zlib1.dll") { Copy-Item $z (Join-Path $Out "zlib1.dll") -Force; Write-Host "copied zlib1.dll" }
else { throw "zlib1.dll not found (Git for Windows' mingw64\bin)" }

New-Item -ItemType Directory -Force (Join-Path $Out "data\data\data\com.activision.boz\files") | Out-Null
Write-Host "done: $Out"
