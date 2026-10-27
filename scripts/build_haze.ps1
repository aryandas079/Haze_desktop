<#
.SYNOPSIS
    Builds the Haze Browser using the configured Chromium tree and depot_tools.
.PARAMETER ChromiumDir
    Chromium base directory (containing src and depot_tools). Defaults to D:\Chromium.
.PARAMETER OutDirName
    Output folder name under src/out. Defaults to WinDefault.
.PARAMETER Jobs
    Number of parallel compilation workers. Defaults to 8.
#>
param(
    [string]$ChromiumDir = "D:\Chromium",
    [string]$OutDirName = "WinDefault",
    [int]$Jobs = 8
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ChromiumSrc = Join-Path $ChromiumDir "src"
$DepotTools = "D:\depot_tools"

if (-not (Test-Path $DepotTools)) {
    $DepotTools = Join-Path $ChromiumDir "depot_tools"
}

if (-not (Test-Path $DepotTools)) {
    throw "depot_tools directory not found at $DepotTools"
}

Write-Host "==> Preparing build environment..." -ForegroundColor Cyan
$env:PATH = "$DepotTools;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
if (Test-Path "C:\Program Files\Microsoft Visual Studio\18\Community") {
    $env:vs2022_install = "C:\Program Files\Microsoft Visual Studio\18\Community"
}
if (Test-Path "D:\Windows Kits\10") {
    $env:WINDOWSSDKDIR = "D:\Windows Kits\10"
}

# 1. Apply patches & branding
& (Join-Path $PSScriptRoot "apply_patches.ps1") -ChromiumSrc $ChromiumSrc

# 2. Setup GN args
$OutDir = Join-Path $ChromiumSrc "out\$OutDirName"
if (-not (Test-Path $OutDir)) {
    New-Item -Path $OutDir -ItemType Directory -Force | Out-Null
}
Copy-Item (Join-Path $RepoRoot "config\args.gn") (Join-Path $OutDir "args.gn") -Force

# 3. Generate Ninja build targets
Write-Host "==> Generating GN targets in $OutDir..." -ForegroundColor Cyan
Set-Location $ChromiumSrc
cmd.exe /c "gn.bat gen out/$OutDirName"
if ($LASTEXITCODE -ne 0) {
    throw "gn gen failed with exit code $LASTEXITCODE"
}

# 4. Compile chrome (Haze)
Write-Host "==> Compiling Haze browser target (jobs: $Jobs)..." -ForegroundColor Cyan
cmd.exe /c "autoninja.bat -C out/$OutDirName -j $Jobs chrome"
if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed with exit code $LASTEXITCODE"
}

# 5. Provide haze.exe binary link
$BuiltExe = Join-Path $OutDir "chrome.exe"
$HazeExe = Join-Path $OutDir "haze.exe"
if (Test-Path $BuiltExe) {
    Copy-Item $BuiltExe $HazeExe -Force
    Write-Host "==> Build Succeeded! Haze browser executable ready at: $HazeExe" -ForegroundColor Green
}
