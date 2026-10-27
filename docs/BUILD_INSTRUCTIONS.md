# Haze Browser - Setup and Build Guide

This document describes how to configure, patch, and build the **Haze Browser** on Windows using the custom Chromium engine.

---

## 1. Prerequisites

1. **Operating System**: Windows 10/11 (64-bit).
2. **Visual Studio**: Visual Studio 2022 Community/Professional with:
   - Desktop development with C++
   - Windows 10/11 SDK (e.g. `10.0.26100.0` or newer)
   - C++ ATL and MFC support
3. **depot_tools**: Google Chromium `depot_tools` extracted to `D:\depot_tools` or added to system `PATH`.
4. **Git**: Installed and available on system PATH.

---

## 2. Chromium Source Revision Tracking

The exact base revision of Chromium used for this version of Haze is recorded in [`config/chromium_version.env`](../config/chromium_version.env):

- **Chromium Version**: `157.0.8088.0`
- **Chromium Revision Commit**: `910b5777cf875164561743e33d6f9981d0dcd0a1`
- **Upstream Repository**: `https://chromium.googlesource.com/chromium/src.git`

To fetch or sync this revision in a clean Chromium workspace:
```powershell
# In D:\Chromium:
gclient sync -r src@910b5777cf875164561743e33d6f9981d0dcd0a1 -D
```

---

## 3. Directory Layout and Architecture

Haze utilizes a **fork-and-patch** model over Chromium:
- **`branding/`**: Contains the Haze application icons, logo, and `BRANDING` specification (name, company, copyright).
- **`patches/`**: Reproducible Git patches targeting the Chromium source tree (e.g. Windows SDK definitions, toolchain adapters, brand wiring).
- **`config/`**: High-performance GN build arguments (`args.gn`) optimized for local compilation without symbol and LTO overhead.
- **`scripts/`**: Automation scripts to apply patches, generate build files, compile the binary, and launch the browser.
- **`legacy_webview2/`**: Preserved original Win32 WebView2 C++ shell implementation for reference.

---

## 4. How to Build Haze

Run the automated build script from PowerShell:

```powershell
cd D:\Projects\Haze_desktop\Haze_desktop
.\scripts\build_haze.ps1 -ChromiumDir "D:\Chromium" -Jobs 8
```

This script:
1. Applies all patches in [`patches/`](../patches/) to `D:\Chromium\src`.
2. Installs Haze branding and application icons into `chrome/app/theme/chromium`.
3. Copies [`config/args.gn`](../config/args.gn) into `D:\Chromium\src\out\WinDefault\args.gn`.
4. Runs `gn gen` to generate build targets.
5. Invokes `autoninja` with balanced CPU parallelism (`-j 8`) to build `chrome`.
6. Generates the final output `haze.exe`.

---

## 5. How to Launch Haze

To launch the compiled browser with an isolated user data directory:

```powershell
.\scripts\launch_haze.ps1
```

Or pass a specific URL:
```powershell
.\scripts\launch_haze.ps1 -Url "https://google.com"
```
