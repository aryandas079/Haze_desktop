# Haze Browser

<p align="center">
  <img src="branding/haze_icon.png" alt="Haze Browser Logo" width="128" height="128" />
</p>

<p align="center">
  <b>A minimalist, modern, privacy-oriented desktop browser engineered on custom Chromium.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20(x64)-blue" alt="Platform Windows" />
  <img src="https://img.shields.io/badge/Engine-Chromium%20Core-teal" alt="Engine Chromium" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B%20%7C%20PowerShell%20%7C%20WebUI-orange" alt="Languages" />
  <img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0" alt="License" />
</p>

---

## Overview

**Haze** is a bespoke, high-performance desktop web browser built by applying custom branding, UX simplifications, and feature patches directly onto the native **Chromium source engine**.

Rather than relying on embedded wrappers or WebView containers, Haze compiles directly against Chromium's native rendering (`Blink`) and JavaScript (`V8`) engines, giving it full Web standards compliance, native hardware acceleration, extension compatibility, and maximum browsing performance.

---

## Architecture and Project Structure

The project maintains a strict separation between upstream Chromium source code and Haze-specific customization:

```text
D:\
├── Chromium\
│   └── src\                         <- Upstream Chromium source tree (clean base)
│       └── out\WinDefault\          <- Local compilation workspace
│           └── haze.exe             <- Compiled native browser executable
│
└── Projects\
    └── Haze_desktop\                <- Haze source repository (Version Controlled)
        ├── branding\                <- Official Haze logos, icons, and metadata
        ├── config\                  <- GN compiler configurations (args.gn)
        ├── docs\                    <- Setup guides and third-party notices
        ├── patches\                 <- Versioned diff patches applied onto Chromium
        ├── scripts\                 <- Automated patch, build, and launch scripts
        ├── LICENSE
        ├── README.md
        └── .gitignore
```

- **`Chromium/`**: Holds the multi-gigabyte Chromium checkout and build outputs. Stays clean and upstream-compatible.
- **`Haze_desktop/`**: Lightweight Git repository containing all custom code, assets, UI patches, and build automation.

---

## Features Implemented So Far

### 1. Native Haze Branding and Visual Identity
- **Custom Application Identity**: Product names configured across binary resource tables, process identifiers, and window classes (`IDS_PRODUCT_NAME` set to `Haze`).
- **Multi-Resolution Vector Icons**: High-DPI icons embedded in `chrome.dll` and `haze.exe` ensuring sharp taskbar, Alt+Tab, and title bar icons across Windows scaling levels (100% to 250%).
- **Fixed App Icon Theme**: Brand icon colors remain stable regardless of Windows dark/light system theme switches.

### 2. Minimalist UI and Window Layout
- **Single-Bar Compact Titlebar**: Streamlined top frame inset integrating window caption buttons directly with the navigation bar to maximize vertical webpage real estate.
- **Custom Dropdown Tab Strip**: Refactored tab management providing a clean dropdown selector in place of bulky tab strips.
- **Menu Overhaul and Hover Stabilization**: Re-engineered application menu with increased spacing, clean typography, and flickering-free hover interaction.

### 3. Bespoke New Tab Page (NTP)
- **Haze Vector Hero Logo**: Custom high-resolution vector emblem centered on the homepage.
- **Streamlined Search**: Direct search integration without intrusive promotional tiles or cluttered widgets.
- **Clean Settings Integration**: Cleaned out redundant search engine toggles in favor of unified omnibox search routing.

### 4. Authentication and Account Integration
- **Google and Firebase Sign-In**: Integration supporting Google OAuth sign-in flow.
- **Dynamic Profile Avatar**: Automatically pulls and renders the authenticated user's account name and profile photo in the top navigation bar.

### 5. Windows SDK and Build Modernization
- **Windows SDK Compatibility Patch**: Added build compatibility for Windows 10/11 SDK (`10.0.26100.0+` / Windows 11 24H2) resolving legacy MSVC build breakages.
- **Component Build Acceleration**: GN configuration optimized for sub-second incremental rebuilds during feature development.

---

## Prerequisites and Build Requirements

To compile Haze from source on Windows:

1. **Operating System**: Windows 10 or Windows 11 (64-bit).
2. **Visual Studio**: Visual Studio 2022 (Community, Professional, or Enterprise):
   - Desktop development with C++
   - Windows 11 SDK (`10.0.26100.0` or higher)
   - MSVC v143 C++ x64/x86 build tools
   - C++ ATL and MFC support
3. **Chromium depot_tools**: Installed and present on system `PATH` (provides `ninja`, `gn`, and `python3`).
4. **PowerShell 7+** or Windows PowerShell 5.1.

---

## Building and Running Haze

All build steps are orchestrated through PowerShell scripts located in `scripts/`:

### Step 1: Apply Patches and Assets
Applies all 23 custom patches and branding assets onto the Chromium tree:
```powershell
.\scripts\apply_patches.ps1 -ChromiumSrc "D:\Chromium\src"
```

### Step 2: Compile Haze
Compiles the browser using `autoninja` and places `haze.exe` in `out\WinDefault\`:
```powershell
.\scripts\build_haze.ps1 -ChromiumDir "D:\Chromium" -Jobs 8
```

### Step 3: Launch Haze Browser
Launches the freshly compiled browser using an isolated development profile:
```powershell
.\scripts\launch_haze.ps1
```

---

## Packaging and Distributing

Haze produces a portable runtime directly in `D:\Chromium\src\out\WinDefault\`:
- **Core Binary**: `haze.exe`
- **Engine Core**: `chrome.dll`
- **Resources**: `resources.pak`, `icudtl.dat`, `v8_context_snapshot.bin`, and `locales/`

### Creating an Installer:
1. **Windows Installer (Inno Setup / NSIS)**: Package the contents of `out\WinDefault\` into a single setup executable with desktop shortcuts and uninstall support.
2. **Native Chromium `mini_installer`**:
   Set `is_component_build = false` in `config/args.gn`, then run:
   ```powershell
   cd D:\Chromium\src
   autoninja -C out\WinDefault mini_installer
   ```
   This generates `D:\Chromium\src\out\WinDefault\mini_installer.exe`.

---

## Roadmap and In-Progress Features

- [x] Chromium engine base setup and build automation
- [x] Windows SDK 26100 compatibility patches
- [x] Custom Haze branding, app IDs, and high-DPI icon assets
- [x] Custom New Tab Page (NTP) layout and hero branding
- [x] Google / Firebase login integration with live avatar display
- [x] Menu spacing optimization and hover flicker elimination
- [ ] Built-in ad-blocking and tracking protection engine
- [ ] Enhanced privacy telemetry stripping
- [ ] Vertical tab strip toggle
- [ ] Automated CI/CD release workflow for Windows installer binaries

---

## License and Attribution

- **Haze**: Licensed under the [Apache License 2.0](LICENSE).
- **Chromium Engine**: Licensed under the [Chromium BSD-style license](docs/THIRD_PARTY_NOTICES.md).
