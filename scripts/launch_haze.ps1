<#
.SYNOPSIS
    Launches the compiled Haze Browser.
.PARAMETER ChromiumDir
    Chromium base directory. Defaults to D:\Chromium.
.PARAMETER OutDirName
    Output folder name. Defaults to WinDefault.
.PARAMETER ProfileDir
    User profile directory. If omitted, uses an isolated Haze development profile.
#>
param(
    [string]$ChromiumDir = "D:\Chromium",
    [string]$OutDirName = "WinDefault",
    [string]$ProfileDir = "D:\Chromium\haze_profile",
    [string]$Url = ""
)

$ExePath = Join-Path $ChromiumDir "src\out\$OutDirName\haze.exe"
if (-not (Test-Path $ExePath)) {
    $ExePath = Join-Path $ChromiumDir "src\out\$OutDirName\chrome.exe"
}

if (-not (Test-Path $ExePath)) {
    throw "Haze executable not found at $ExePath. Please run scripts\build_haze.ps1 first."
}

Write-Host "==> Launching Haze Browser ($ExePath)..." -ForegroundColor Green
$LaunchArgs = @(
    "--user-data-dir=$ProfileDir",
    "--no-first-run",
    "--no-default-browser-check"
)

if ($Url) {
    $LaunchArgs += $Url
}

Start-Process -FilePath $ExePath -ArgumentList $LaunchArgs
