# build.ps1 - putty-cn build helper (MSYS2 MinGW-w64 + CMake + Ninja).
# Encapsulates the known pitfalls:
#   1. Git Bash's MSYS-style PATH makes native cc1.exe fail silently
#      (missing libmpfr-6.dll); we set a Windows-style PATH first.
#   2. ninja does not track version.h as a dependency of .rc files;
#      use -CleanRc whenever the version string changes.
# Full release process: reports/2026-11-01-发布v0.85-cn3.md

param(
    [string[]]$Targets = @("putty", "puttygen", "plink", "pscp", "psftp", "pageant", "puttytel"),
    [switch]$CleanRc,
    [switch]$VersionOnly,
    [string]$BuildDir = (Join-Path $PSScriptRoot "src\build")
)

$ErrorActionPreference = "Stop"
$Ninja = "C:\msys64\mingw64\bin\ninja.exe"

if (-not (Test-Path $Ninja)) {
    throw "ninja.exe not found at $Ninja. Install MSYS2 (mingw-w64-x86_64-ninja)."
}

if ($VersionOnly) {
    & (Join-Path $BuildDir "plink.exe") -V
    exit 0
}

$cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $cache)) {
    Write-Host "CMakeCache.txt not found in $BuildDir."
    Write-Host "Configure first, e.g.:"
    Write-Host "  cd src; cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build"
    throw "Build directory not configured."
}

if ($CleanRc) {
    Get-ChildItem -Path $BuildDir -Recurse -Filter *.rc.obj -ErrorAction SilentlyContinue |
        ForEach-Object {
            Remove-Item $_.FullName -Force
            Write-Host "Removed $($_.FullName)"
        }
}

# Build with a Windows-style PATH (pitfall 1).
$env:PATH = "C:\msys64\mingw64\bin;C:\Program Files\Git\cmd;C:\Windows\System32;C:\Windows"
& $Ninja -C $BuildDir @Targets
if ($LASTEXITCODE -ne 0) {
    throw "ninja failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Build OK: $($Targets -join ', ')"

& (Join-Path $BuildDir "plink.exe") -V
