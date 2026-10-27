<#
.SYNOPSIS
    Applies Haze custom branding and patches to the Chromium source tree.
.PARAMETER ChromiumSrc
    The path to the Chromium src directory. Defaults to D:\Chromium\src.
#>
param(
    [string]$ChromiumSrc = "D:\Chromium\src"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

Write-Host "==> Applying Haze customization to $ChromiumSrc..." -ForegroundColor Cyan

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium src directory not found at $ChromiumSrc"
}

# 1. Apply patches
$Patches = Get-ChildItem -Path (Join-Path $RepoRoot "patches") -Filter "*.patch" | Sort-Object Name
foreach ($patch in $Patches) {
    Write-Host "  -> Applying patch: $($patch.Name)" -ForegroundColor Yellow
    git -C $ChromiumSrc apply --ignore-whitespace --whitespace=nowarn $patch.FullName
}

# 2. Copy branding assets
Write-Host "  -> Installing Haze branding and icons..." -ForegroundColor Yellow
$BrandingTarget = Join-Path $ChromiumSrc "chrome\app\theme\chromium"
Copy-Item (Join-Path $RepoRoot "branding\BRANDING") (Join-Path $BrandingTarget "BRANDING") -Force
Copy-Item (Join-Path $RepoRoot "branding\haze.ico") (Join-Path $BrandingTarget "win\chromium.ico") -Force

Write-Host "==> Haze patches and branding successfully applied!" -ForegroundColor Green
