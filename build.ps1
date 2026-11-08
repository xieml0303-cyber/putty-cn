# build.ps1 - putty-cn build helper (MSYS2 MinGW-w64 + CMake + Ninja).
# Encapsulates the known pitfalls:
#   1. Git Bash's MSYS-style PATH makes native cc1.exe fail silently
#      (missing libmpfr-6.dll); we set a Windows-style PATH first.
#   2. ninja does not track version.h as a dependency of .rc files;
#      use -CleanRc whenever the version string changes.
# Full release process: reports/2026-11-01-发布v0.85-cn3.md

param(
    [string[]]$Targets = @("putty"),
    [switch]$CleanRc,
    [switch]$VersionOnly,
    [string]$BuildDir = (Join-Path $PSScriptRoot "src\build")
)

$ErrorActionPreference = "Stop"
$Ninja = "C:\msys64\mingw64\bin\ninja.exe"

if (-not (Test-Path $Ninja)) {
    throw "ninja.exe not found at $Ninja. Install MSYS2 (mingw-w64-x86_64-ninja)."
}

# cn: 版本号从 src\version.h 的 TEXTVER 读，不再依赖 plink.exe -V
# （plink 已从构建中移除；且 putty.exe 是 GUI 程序，-V 不会打印到控制台）。
function Get-TextVer {
    $vh = Join-Path $PSScriptRoot "src\version.h"
    $m = Select-String -Path $vh -Pattern '#define\s+TEXTVER\s+"([^"]*)"'
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return "(version.h 中未找到 TEXTVER)"
}

if ($VersionOnly) {
    Write-Host (Get-TextVer)
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

Write-Host (Get-TextVer)
