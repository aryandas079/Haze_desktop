# HAZE Desktop Browser

A lightweight C++ desktop browser with Google search and system tray integration.

## Features
- **Modern Chromium Core**: Powered by Microsoft WebView2.
- **Minimalist White Canvas**: Clean white background and minimalist top address/search bar.
- **Smart Search & Navigation**: Direct URLs, domain navigation, or auto-encoded Google searches.
- **System Tray Mode**: Closing the window keeps HAZE running in the system tray.
- **WinUI 3 Fluent Context Menu**: Rounded corner pop-up with Open, Open Search, Settings, Version, and Quit.
- **Custom Branding & Icons**: Integrated HAZE branding across app icons, taskbar, title bar, and system tray.

## Build Requirements
- Visual Studio 2022+ with Desktop C++ workload
- Windows 10 / 11 SDK (10.0.26100.0 or compatible)
- NuGet packages (restored automatically):
  - `Microsoft.Web.WebView2`
  - `Microsoft.Windows.ImplementationLibrary`

## Build & Run
Open `Haze_desktop.slnx` or `Haze_desktop.vcxproj` in Visual Studio and build `Release|x64` or `Debug|x64`.
