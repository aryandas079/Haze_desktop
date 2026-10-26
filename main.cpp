#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Microsoft::WRL;
using namespace Gdiplus;

// Constants
constexpr UINT WM_TRAYICON = WM_USER + 100;

// Spacious heights and layout
constexpr int TOP_BAR_HEIGHT = 120;
constexpr int TAB_BAR_HEIGHT = 54;
constexpr int TAB_WIDTH = 260;

constexpr int SEARCH_BAR_HEIGHT = 48;
constexpr int NAV_BUTTONS_WIDTH = 132;

constexpr int IDC_EDIT_URL = 101;
constexpr int IDC_BTN_SEARCH = 102;
constexpr int IDC_BTN_BACK = 103;
constexpr int IDC_BTN_FORWARD = 104;
constexpr int IDC_BTN_RELOAD = 105;
constexpr int IDC_BTN_ENGINE = 106;
constexpr int IDC_BTN_SETTINGS = 107;

constexpr int MIN_WINDOW_WIDTH = 1405;
constexpr int MIN_WINDOW_HEIGHT = 850;

constexpr const wchar_t* APP_VERSION = L"v1.0.0";
constexpr const wchar_t* APP_NAME = L"HAZE";

// Injected CSS to enforce clean modern font across websites
const wchar_t* INJECTED_INTER_SCRIPT = LR"(
(function() {
  if (document.getElementById('__haze_font_style__')) return;
  const style = document.createElement('style');
  style.id = '__haze_font_style__';
  style.textContent = `
    * {
      font-family: 'Google Sans', 'Segoe UI Variable Text', 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif !important;
    }
  `;
  document.head.appendChild(style);
})();
)";

// Search Engine definition
struct SearchEngineInfo {
    std::wstring name;
    std::wstring queryPrefix;
    std::wstring placeholder;
    std::wstring iconFile;
    Gdiplus::Bitmap* pBitmap = nullptr;
};

std::vector<SearchEngineInfo> g_searchEngines = {
    { L"Google", L"https://www.google.com/search?q=", L"Search or enter web address", L"google.png", nullptr },
    { L"Bing", L"https://www.bing.com/search?q=", L"Search Bing or type a URL", L"bing.png", nullptr },
    { L"DuckDuckGo", L"https://duckduckgo.com/?q=", L"Search DuckDuckGo or type a URL", L"duckduckgo.png", nullptr },
    { L"Perplexity", L"https://www.perplexity.ai/search?q=", L"Search Perplexity or type a URL", L"perplexity.png", nullptr },
    { L"Wikipedia (en)", L"https://en.wikipedia.org/wiki/Special:Search?search=", L"Search Wikipedia or type a URL", L"wikipedia.png", nullptr }
};

int g_selectedEngineIndex = 0;

// Tab Data Model
struct BrowserTab {
    std::wstring title;
    std::wstring url;
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> webview;
    bool isLoaded = false;
};

// Globals
HINSTANCE g_hInstance = nullptr;
HWND g_hWnd = nullptr;
HWND g_hEdit = nullptr;
HWND g_hBtnSearch = nullptr;
HWND g_hBtnEngine = nullptr;
HWND g_hBtnBack = nullptr;
HWND g_hBtnForward = nullptr;
HWND g_hBtnReload = nullptr;
HWND g_hBtnSettings = nullptr;
HWND g_hMenuWnd = nullptr;
HWND g_hEngineDropdownWnd = nullptr;

WNDPROC g_originalEditProc = nullptr;
WNDPROC g_originalSearchBtnProc = nullptr;
WNDPROC g_originalEngineBtnProc = nullptr;
WNDPROC g_originalNavBtnProc = nullptr;
WNDPROC g_originalSettingsBtnProc = nullptr;

HICON g_hAppIcon = nullptr;
ULONG_PTR g_gdiplusToken = 0;
NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };

std::vector<std::unique_ptr<BrowserTab>> g_tabs;
int g_activeTabIndex = -1;
int g_hoveredTabIndex = -1;
int g_hoveredCloseTabIndex = -1;
bool g_isHoveringNewTab = false;
bool g_isSearchBtnHovered = false;
bool g_isEngineBtnHovered = false;
bool g_isSettingsBtnHovered = false;
bool g_isEditFocused = false;

std::wstring g_homePageHtml;
std::wstring g_homePageUrl;
std::wstring g_settingsPageUrl;
bool g_isDarkMode = false;
static HBRUSH s_editBrushLight = nullptr;
static HBRUSH s_editBrushDark = nullptr;
static HBRUSH s_pillBrushLight = nullptr;
static HBRUSH s_pillBrushDark = nullptr;

// Custom Menu Items
enum MenuAction {
    ACTION_OPEN_HAZE = 1,
    ACTION_NEW_TAB,
    ACTION_NEW_WINDOW,
    ACTION_ZOOM_IN,
    ACTION_ZOOM_OUT,
    ACTION_ZOOM_RESET,
    ACTION_HISTORY,
    ACTION_DOWNLOADS,
    ACTION_PRINT,
    ACTION_SETTINGS,
    ACTION_VERSION,
    ACTION_QUIT
};

struct WinUI3MenuItem {
    std::wstring text;
    std::wstring shortcut;
    MenuAction action;
    bool isSeparator;
    bool isHeader;
    bool isEnabled;
};

std::vector<WinUI3MenuItem> g_menuItems;
int g_hoveredIndex = -1;
int g_engineDropdownHoveredIndex = -1;

void ResizeControls(int width, int height);
void SwitchToTab(int index);
void CreateNewTab(const std::wstring& initialUrl = L"about:blank");
void CloseTab(int index);
void UpdateOmniboxCueBanner();

const wchar_t* GetModernFontFamilyName() {
    static const wchar_t* s_fontName = nullptr;
    if (!s_fontName) {
        // Try Google Sans or Segoe UI Variable Text (Windows 11 modern Chrome font), fallback to Segoe UI
        FontFamily testGoogleSans(L"Google Sans");
        if (testGoogleSans.GetLastStatus() == Ok) {
            s_fontName = L"Google Sans";
        } else {
            FontFamily testSegoeVar(L"Segoe UI Variable Text");
            if (testSegoeVar.GetLastStatus() == Ok) {
                s_fontName = L"Segoe UI Variable Text";
            } else {
                s_fontName = L"Segoe UI";
            }
        }
    }
    return s_fontName;
}

void AddRoundedRect(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void LoadSearchEngineIcons() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    std::wstring basePath = (pos != std::wstring::npos) ? dir.substr(0, pos + 1) : L"";

    for (auto& eng : g_searchEngines) {
        std::wstring paths[] = {
            basePath + L"icons\\" + eng.iconFile,
            basePath + L"..\\icons\\" + eng.iconFile,
            basePath + L"..\\..\\icons\\" + eng.iconFile,
            L"d:\\Projects\\Haze_desktop\\Haze_desktop\\icons\\" + eng.iconFile
        };
        for (const auto& p : paths) {
            DWORD dwAttrib = GetFileAttributesW(p.c_str());
            if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
                eng.pBitmap = Gdiplus::Bitmap::FromFile(p.c_str());
                if (eng.pBitmap && eng.pBitmap->GetLastStatus() == Ok) {
                    break;
                }
            }
        }
    }
}

std::wstring LoadHomePageHtml() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    std::wstring basePath = (pos != std::wstring::npos) ? dir.substr(0, pos + 1) : L"";

    std::wstring possibleFiles[] = {
        basePath + L"homepage.html",
        basePath + L"..\\homepage.html",
        basePath + L"..\\..\\homepage.html",
        L"d:\\Projects\\Haze_desktop\\Haze_desktop\\homepage.html"
    };

    for (const auto& path : possibleFiles) {
        DWORD dwAttrib = GetFileAttributesW(path.c_str());
        if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t fullPath[MAX_PATH] = { 0 };
            GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr);
            std::wstring fileUri = fullPath;
            for (auto& ch : fileUri) {
                if (ch == L'\\') ch = L'/';
            }
            g_homePageUrl = L"file:///" + fileUri;

            std::ifstream file(path);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string content = buffer.str();
                int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    std::wstring wcontent(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, &wcontent[0], wlen);
                    return wcontent;
                }
            }
        }
    }
    return L"<html><body style='background:#fff;'></body></html>";
}

void InitSettingsPageUrl() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    std::wstring basePath = (pos != std::wstring::npos) ? dir.substr(0, pos + 1) : L"";

    std::wstring possibleFiles[] = {
        basePath + L"settings.html",
        basePath + L"..\\settings.html",
        basePath + L"..\\..\\settings.html",
        L"d:\\Projects\\Haze_desktop\\Haze_desktop\\settings.html"
    };

    for (const auto& path : possibleFiles) {
        DWORD dwAttrib = GetFileAttributesW(path.c_str());
        if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t fullPath[MAX_PATH] = { 0 };
            GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr);
            std::wstring fileUri = fullPath;
            for (auto& ch : fileUri) {
                if (ch == L'\\') ch = L'/';
            }
            g_settingsPageUrl = L"file:///" + fileUri;
            return;
        }
    }
}

bool IsLikelyUrl(const std::wstring& str) {
    if (str.find(L' ') != std::wstring::npos) return false;

    if (str.find(L"http://") == 0 || str.find(L"https://") == 0 ||
        str.find(L"file:///") == 0 || str.find(L"ftp://") == 0 ||
        str.find(L"edge://") == 0 || str.find(L"chrome://") == 0 ||
        str.find(L"about:") == 0) {
        return true;
    }

    if (str == L"localhost" || str.find(L"localhost:") == 0) {
        return true;
    }

    int dotCount = 0;
    bool allDigitsAndDots = true;
    for (wchar_t c : str) {
        if (c == L'.') dotCount++;
        else if (c == L':') break;
        else if (!iswdigit(c)) { allDigitsAndDots = false; }
    }
    if (dotCount == 3 && allDigitsAndDots) {
        return true;
    }

    size_t lastDot = str.rfind(L'.');
    if (lastDot != std::wstring::npos && lastDot + 1 < str.length()) {
        std::wstring tld = str.substr(lastDot + 1);
        size_t slashPos = tld.find_first_of(L"/:?#");
        if (slashPos != std::wstring::npos) {
            tld = tld.substr(0, slashPos);
        }

        if (tld.length() >= 2) {
            bool validTld = true;
            for (wchar_t c : tld) {
                if (!iswalpha(c)) {
                    validTld = false;
                    break;
                }
            }
            if (validTld) return true;
        }
    }

    return false;
}

std::wstring UrlEncodeQuery(const std::wstring& input) {
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return L"";

    std::string utf8Str(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, &utf8Str[0], utf8Len, nullptr, nullptr);

    std::wstring encoded;
    for (size_t i = 0; i < (size_t)(utf8Len - 1); ++i) {
        unsigned char c = (unsigned char)utf8Str[i];
        if (c == ' ') {
            encoded += L'+';
        }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (wchar_t)c;
        }
        else {
            wchar_t hex[8];
            swprintf_s(hex, L"%%%02X", (unsigned int)c);
            encoded += hex;
        }
    }
    return encoded;
}

void NavigateToInput(const std::wstring& input) {
    if (g_activeTabIndex < 0 || g_activeTabIndex >= (int)g_tabs.size()) return;
    auto& tab = g_tabs[g_activeTabIndex];
    if (!tab->webview) return;

    std::wstring trimmed = input;
    size_t first = trimmed.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return;
    size_t last = trimmed.find_last_not_of(L" \t\r\n");
    trimmed = trimmed.substr(first, (last - first + 1));

    std::wstring url;
    if (trimmed == L"haze://settings" || trimmed == L"about:settings" || trimmed == L"edge://settings" || trimmed == L"chrome://settings") {
        if (!g_settingsPageUrl.empty()) {
            url = g_settingsPageUrl;
        } else {
            url = L"about:blank";
        }
    }
    else if (trimmed == L"haze://home" || trimmed == L"about:home" || trimmed == L"about:newtab") {
        if (!g_homePageUrl.empty()) {
            url = g_homePageUrl;
        } else {
            url = L"about:blank";
        }
    }
    else if (trimmed.find(L"http://") == 0 || trimmed.find(L"https://") == 0 ||
        trimmed.find(L"file:///") == 0 || trimmed.find(L"edge://") == 0 ||
        trimmed.find(L"about:") == 0) {
        url = trimmed;
    }
    else if (IsLikelyUrl(trimmed)) {
        url = L"https://" + trimmed;
    }
    else {
        const auto& engine = g_searchEngines[g_selectedEngineIndex];
        url = engine.queryPrefix + UrlEncodeQuery(trimmed);
    }

    tab->webview->Navigate(url.c_str());
}

RECT GetSearchBarOuterRect(int clientWidth) {
    int startX = NAV_BUTTONS_WIDTH;
    // Keep address bar proportional - enlarged to extend generously across the toolbar
    int maxOmniWidth = 1150;
    int availableWidth = clientWidth - startX - 80;
    int omniWidth = (availableWidth > maxOmniWidth) ? maxOmniWidth : availableWidth;
    if (omniWidth < 280) omniWidth = 280;
    int endX = startX + omniWidth;
    return RECT{ startX, TAB_BAR_HEIGHT + 7, endX, TAB_BAR_HEIGHT + 7 + SEARCH_BAR_HEIGHT };
}

void UpdateOmniboxCueBanner() {
    if (g_hEdit && g_selectedEngineIndex >= 0 && g_selectedEngineIndex < (int)g_searchEngines.size()) {
        const auto& engine = g_searchEngines[g_selectedEngineIndex];
        SendMessage(g_hEdit, EM_SETCUEBANNER, TRUE, (LPARAM)engine.placeholder.c_str());
    }
}

void ResizeControls(int width, int height) {
    if (width <= 0 || height <= 0) return;

    int navY = TAB_BAR_HEIGHT + ((TOP_BAR_HEIGHT - TAB_BAR_HEIGHT - 32) / 2);
    int navBtnSize = 32;
    if (g_hBtnBack)    SetWindowPos(g_hBtnBack, nullptr, 14, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnForward) SetWindowPos(g_hBtnForward, nullptr, 50, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnReload)  SetWindowPos(g_hBtnReload, nullptr, 86, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);

    // Search Bar layout
    RECT sb = GetSearchBarOuterRect(width);
    int engineBtnW = 32;
    int engineBtnH = 32;
    int engineBtnX = sb.left + 10;
    int engineBtnY = sb.top + ((SEARCH_BAR_HEIGHT - engineBtnH) / 2);

    if (g_hBtnEngine) {
        SetWindowPos(g_hBtnEngine, nullptr, engineBtnX, engineBtnY, engineBtnW, engineBtnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int editX = engineBtnX + engineBtnW + 8;
    int editW = sb.right - editX - 16;
    if (editW < 50) editW = 50;

    int editH = 24;
    int editY = sb.top + ((SEARCH_BAR_HEIGHT - editH) / 2);

    if (g_hEdit) {
        SetWindowPos(g_hEdit, nullptr, editX, editY, editW, editH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hBtnSearch) {
        ShowWindow(g_hBtnSearch, SW_HIDE);
    }

    // Place Settings Button cleanly right next to the address bar (with pleasant padding)
    int settingsBtnSize = 32;
    int settingsBtnX = sb.right + 12;
    int settingsBtnY = navY;
    if (g_hBtnSettings) {
        SetWindowPos(g_hBtnSettings, nullptr, settingsBtnX, settingsBtnY, settingsBtnSize, settingsBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) {
        auto& tab = g_tabs[g_activeTabIndex];
        if (tab->controller) {
            RECT bounds{ 0, TOP_BAR_HEIGHT, width, height };
            tab->controller->put_Bounds(bounds);
        }
    }
}

void RestoreMainWindow(bool focusSearch = false) {
    ShowWindow(g_hWnd, SW_RESTORE);
    SetForegroundWindow(g_hWnd);
    if (focusSearch && g_hEdit) {
        SetFocus(g_hEdit);
        SendMessage(g_hEdit, EM_SETSEL, 0, -1);
    }
}

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            wchar_t buffer[2048] = { 0 };
            GetWindowTextW(hWnd, buffer, ARRAYSIZE(buffer));
            NavigateToInput(buffer);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        g_isEditFocused = true;
        if (g_hWnd) {
            RECT rc;
            GetClientRect(g_hWnd, &rc);
            RECT sb = GetSearchBarOuterRect(rc.right - rc.left);
            sb.left -= 4; sb.top -= 4; sb.right += 4; sb.bottom += 4;
            InvalidateRect(g_hWnd, &sb, FALSE);
        }
        break;
    case WM_KILLFOCUS:
        g_isEditFocused = false;
        if (g_hWnd) {
            RECT rc;
            GetClientRect(g_hWnd, &rc);
            RECT sb = GetSearchBarOuterRect(rc.right - rc.left);
            sb.left -= 4; sb.top -= 4; sb.right += 4; sb.bottom += 4;
            InvalidateRect(g_hWnd, &sb, FALSE);
        }
        break;
    }
    return CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
}

void ShowSettingsDropdown();

// Settings Chevron Dropdown Button located outside the address bar
LRESULT CALLBACK SettingsButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        // Fill background with toolbar color
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        SolidBrush bgBrush(bgCol);
        g.FillRectangle(&bgBrush, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

        if (g_isSettingsBtnHovered) {
            SolidBrush hoverBrush(g_isDarkMode ? Color(255, 52, 52, 62) : Color(255, 238, 232, 210));
            g.FillEllipse(&hoverBrush, (REAL)rc.left + 2.0f, (REAL)rc.top + 2.0f, (REAL)(rc.right - rc.left - 4.0f), (REAL)(rc.bottom - rc.top - 4.0f));
        }

        // Draw chevron down icon matching the uploaded image: ∨
        Pen chevronPen(g_isDarkMode ? Color(255, 220, 220, 230) : Color(255, 60, 58, 54), 1.8f);
        chevronPen.SetStartCap(LineCapRound);
        chevronPen.SetEndCap(LineCapRound);
        chevronPen.SetLineJoin(LineJoinRound);

        REAL cx = (rc.left + rc.right) / 2.0f;
        REAL cy = (rc.top + rc.bottom) / 2.0f;
        REAL sz = 4.8f;

        // Symmetric rounded 'V' chevron
        PointF pts[] = {
            PointF(cx - sz, cy - 2.2f),
            PointF(cx, cy + 2.2f),
            PointF(cx + sz, cy - 2.2f)
        };
        g.DrawLines(&chevronPen, pts, 3);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isSettingsBtnHovered) {
            g_isSettingsBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_isSettingsBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        ShowSettingsDropdown();
        return 0;
    }
    }
    return CallWindowProc(g_originalSettingsBtnProc, hWnd, uMsg, wParam, lParam);
}

// Crisp arrow search button
LRESULT CALLBACK SearchButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        // Fill background with omnibox pill warm beige
        SolidBrush bgBrush(Color(255, 254, 249, 235));
        g.FillRectangle(&bgBrush, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

        if (g_isSearchBtnHovered) {
            SolidBrush hoverBrush(Color(255, 239, 230, 129));
            g.FillEllipse(&hoverBrush, (REAL)rc.left + 2.0f, (REAL)rc.top + 2.0f, (REAL)(rc.right - rc.left - 4.0f), (REAL)(rc.bottom - rc.top - 4.0f));
        }

        Pen arrowPen(Color(255, 118, 112, 27), 2.2f);
        arrowPen.SetStartCap(LineCapRound);
        arrowPen.SetEndCap(LineCapRound);

        REAL cx = (rc.left + rc.right) / 2.0f;
        REAL cy = (rc.top + rc.bottom) / 2.0f;

        g.DrawLine(&arrowPen, cx - 6.0f, cy, cx + 5.0f, cy);
        g.DrawLine(&arrowPen, cx + 5.0f, cy, cx + 0.5f, cy - 4.5f);
        g.DrawLine(&arrowPen, cx + 5.0f, cy, cx + 0.5f, cy + 4.5f);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isSearchBtnHovered) {
            g_isSearchBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_isSearchBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    }
    return CallWindowProc(g_originalSearchBtnProc, hWnd, uMsg, wParam, lParam);
}

void ShowEngineDropdown();

// Search Engine Selector Button inside Omnibox
LRESULT CALLBACK EngineButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // Fill background with omnibox pill warm beige / dark grey
        Color bgCol = g_isDarkMode ? Color(255, 40, 40, 50) : Color(255, 254, 249, 235);
        SolidBrush bgBrush(bgCol);
        g.FillRectangle(&bgBrush, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

        if (g_isEngineBtnHovered) {
            GraphicsPath btnPath;
            AddRoundedRect(btnPath, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top), 8.0f);
            SolidBrush hoverBrush(g_isDarkMode ? Color(255, 60, 60, 75) : Color(255, 239, 230, 129));
            g.FillPath(&hoverBrush, &btnPath);
        }

        // Draw Selected Engine Icon
        if (g_selectedEngineIndex >= 0 && g_selectedEngineIndex < (int)g_searchEngines.size()) {
            auto pBmp = g_searchEngines[g_selectedEngineIndex].pBitmap;
            if (pBmp) {
                g.DrawImage(pBmp, 6.0f, ((rc.bottom - rc.top) - 20.0f) / 2.0f, 20.0f, 20.0f);
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isEngineBtnHovered) {
            g_isEngineBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_isEngineBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        ShowEngineDropdown();
        return 0;
    }
    }
    return CallWindowProc(g_originalEngineBtnProc, hWnd, uMsg, wParam, lParam);
}

// Crisp smooth vector navigation buttons (←, →, ↻) using GDI+
LRESULT CALLBACK NavButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HWND s_hoveredBtn = nullptr;

    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (s_hoveredBtn != hWnd) {
            s_hoveredBtn = hWnd;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        if (s_hoveredBtn == hWnd) {
            s_hoveredBtn = nullptr;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        // Fill background with toolbar cream color / dark grey
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        SolidBrush bgBrush(bgCol);
        g.FillRectangle(&bgBrush, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

        int id = (int)GetWindowLongPtr(hWnd, GWLP_ID);

        bool enabled = true;
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) {
            auto& tab = g_tabs[g_activeTabIndex];
            if (tab->webview) {
                BOOL can = FALSE;
                if (id == IDC_BTN_BACK && SUCCEEDED(tab->webview->get_CanGoBack(&can))) enabled = can;
                if (id == IDC_BTN_FORWARD && SUCCEEDED(tab->webview->get_CanGoForward(&can))) enabled = can;
            }
        }

        // Circular hover pill highlight if enabled
        bool isHovered = (s_hoveredBtn == hWnd);
        if (enabled && isHovered) {
            SolidBrush hoverBrush(g_isDarkMode ? Color(255, 52, 52, 62) : Color(255, 238, 232, 210));
            g.FillEllipse(&hoverBrush, (REAL)rc.left + 2.0f, (REAL)rc.top + 2.0f, (REAL)(rc.right - rc.left - 4.0f), (REAL)(rc.bottom - rc.top - 4.0f));
        }

        Color iconCol;
        if (g_isDarkMode) {
            iconCol = enabled ? Color(255, 220, 220, 230) : Color(255, 90, 90, 105);
        } else {
            iconCol = enabled ? Color(255, 60, 58, 54) : Color(255, 185, 182, 160);
        }
        Pen navPen(iconCol, 1.8f);
        navPen.SetStartCap(LineCapRound);
        navPen.SetEndCap(LineCapRound);
        navPen.SetLineJoin(LineJoinRound);

        REAL cx = (rc.left + rc.right) / 2.0f;
        REAL cy = (rc.top + rc.bottom) / 2.0f;

        if (id == IDC_BTN_BACK) {
            // Modern rounded back arrow matching reference image: ←
            REAL shaftLen = 6.5f;
            REAL wing = 5.0f;
            // Central horizontal stem
            g.DrawLine(&navPen, cx - shaftLen, cy, cx + shaftLen, cy);
            // Upper and lower diagonal wings
            g.DrawLine(&navPen, cx - shaftLen, cy, cx - shaftLen + wing, cy - wing);
            g.DrawLine(&navPen, cx - shaftLen, cy, cx - shaftLen + wing, cy + wing);
        }
        else if (id == IDC_BTN_FORWARD) {
            // Modern rounded forward arrow matching reference image: →
            REAL shaftLen = 6.5f;
            REAL wing = 5.0f;
            // Central horizontal stem
            g.DrawLine(&navPen, cx - shaftLen, cy, cx + shaftLen, cy);
            // Upper and lower diagonal wings
            g.DrawLine(&navPen, cx + shaftLen, cy, cx + shaftLen - wing, cy - wing);
            g.DrawLine(&navPen, cx + shaftLen, cy, cx + shaftLen - wing, cy + wing);
        }
        else if (id == IDC_BTN_RELOAD) {
            // Modern rounded circular arrow matching standard browser reload icon: ↻
            REAL radius = 6.0f;
            RectF arcRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
            // Draw smooth 290 degree clockwise arc starting from top-right (35 deg)
            g.DrawArc(&navPen, arcRect, 35.0f, 290.0f);

            // Arc ends at 325 degrees (approx cx + 4.9f, cy - 3.4f)
            // Clockwise arrowhead pointing inward/downwards
            REAL tipX = cx + 4.9f;
            REAL tipY = cy - 3.4f;

            GraphicsPath arrowHead;
            arrowHead.AddLine(tipX - 1.0f, tipY - 5.0f, tipX + 0.5f, tipY + 0.5f);
            arrowHead.AddLine(tipX + 0.5f, tipY + 0.5f, tipX + 5.5f, tipY + 0.5f);
            g.DrawPath(&navPen, &arrowHead);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return CallWindowProc(g_originalNavBtnProc, hWnd, uMsg, wParam, lParam);
}

// --------------------------------------------------------------------------
// Tab Coordinates & Geometry
// --------------------------------------------------------------------------
constexpr int CAPTION_BTN_WIDTH = 46;
constexpr int CAPTION_BTN_HEIGHT = TAB_BAR_HEIGHT;

enum CaptionButtonType {
    CAPTION_BTN_NONE = 0,
    CAPTION_BTN_MINIMIZE,
    CAPTION_BTN_MAXIMIZE,
    CAPTION_BTN_CLOSE
};

CaptionButtonType g_hoveredCaptionBtn = CAPTION_BTN_NONE;

RECT GetCaptionBtnRect(CaptionButtonType type, int clientWidth) {
    int right = clientWidth;
    int top = 0;
    int bottom = TAB_BAR_HEIGHT;
    switch (type) {
    case CAPTION_BTN_CLOSE:
        return RECT{ right - CAPTION_BTN_WIDTH, top, right, bottom };
    case CAPTION_BTN_MAXIMIZE:
        return RECT{ right - (CAPTION_BTN_WIDTH * 2), top, right - CAPTION_BTN_WIDTH, bottom };
    case CAPTION_BTN_MINIMIZE:
        return RECT{ right - (CAPTION_BTN_WIDTH * 3), top, right - (CAPTION_BTN_WIDTH * 2), bottom };
    default:
        return RECT{ 0, 0, 0, 0 };
    }
}

RECT GetTabRect(int index, int clientWidth) {
    int x = 12 + index * (TAB_WIDTH + 8);
    int y = 8;
    return RECT{ x, y, x + TAB_WIDTH, TAB_BAR_HEIGHT - 6 };
}

RECT GetNewTabBtnRect(int clientWidth) {
    int x = 12 + (int)g_tabs.size() * (TAB_WIDTH + 8) + 16;
    int y = 13;
    return RECT{ x, y, x + 28, y + 28 };
}

RECT GetTabCloseRect(const RECT& tabRc) {
    int w = 22;
    int h = 22;
    int right = tabRc.right - 14;
    int top = tabRc.top + ((tabRc.bottom - tabRc.top - h) / 2);
    return RECT{ right - w, top, right, top + h };
}

void SwitchToTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;

    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i]->controller) {
            g_tabs[i]->controller->put_IsVisible(i == index ? TRUE : FALSE);
        }
    }

    g_activeTabIndex = index;
    auto& activeTab = g_tabs[index];

    if (g_hEdit) {
        if (activeTab->url.find(L"about:blank") == 0 || activeTab->url.empty() || 
            activeTab->url.find(L"data:text/html") == 0 ||
            (!g_homePageUrl.empty() && activeTab->url.find(g_homePageUrl) == 0)) {
            SetWindowTextW(g_hEdit, L"");
        }
        else if (!g_settingsPageUrl.empty() && activeTab->url.find(g_settingsPageUrl) == 0) {
            SetWindowTextW(g_hEdit, L"haze://settings");
        }
        else {
            SetWindowTextW(g_hEdit, activeTab->url.c_str());
        }
    }

    RECT rc;
    GetClientRect(g_hWnd, &rc);
    ResizeControls(rc.right - rc.left, rc.bottom - rc.top);

    RECT topRc{ 0, 0, rc.right, TOP_BAR_HEIGHT };
    InvalidateRect(g_hWnd, &topRc, FALSE);
}

void CreateNewTab(const std::wstring& initialUrl) {
    auto tab = std::make_unique<BrowserTab>();
    tab->title = L"New Tab";
    tab->url = initialUrl;
    g_tabs.push_back(std::move(tab));
    int newIndex = (int)g_tabs.size() - 1;

    HWND hWnd = g_hWnd;
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd, newIndex, initialUrl](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;
                if (newIndex >= (int)g_tabs.size()) return S_OK;

                env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd, newIndex, initialUrl](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;
                            if (newIndex >= (int)g_tabs.size()) return S_OK;

                            auto& tab = g_tabs[newIndex];
                            tab->controller = controller;
                            tab->controller->get_CoreWebView2(&tab->webview);

                            wil::com_ptr<ICoreWebView2Settings> settings;
                            tab->webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                            }

                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(tab->controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                COREWEBVIEW2_COLOR whiteColor{ 255, 255, 255, 255 };
                                controller2->put_DefaultBackgroundColor(whiteColor);
                            }

                            tab->webview->AddScriptToExecuteOnDocumentCreated(INJECTED_INTER_SCRIPT, nullptr);

                            // Auto-grant microphone permissions so Web Speech and voice search work smoothly
                            tab->webview->add_PermissionRequested(
                                Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_PERMISSION_KIND kind;
                                        if (SUCCEEDED(args->get_PermissionKind(&kind))) {
                                            if (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) {
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                            }
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // Handle WebMessages from the center homepage search bar and settings page
                            tab->webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR messageRaw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&messageRaw)) && messageRaw) {
                                            std::wstring jsonStr = messageRaw;
                                            CoTaskMemFree(messageRaw);

                                            // Search query from homepage
                                            size_t qPos = jsonStr.find(L"\"query\":");
                                            if (qPos != std::wstring::npos) {
                                                size_t start = jsonStr.find(L"\"", qPos + 8);
                                                if (start != std::wstring::npos) {
                                                    size_t end = jsonStr.find(L"\"", start + 1);
                                                    if (end != std::wstring::npos) {
                                                        std::wstring query = jsonStr.substr(start + 1, end - start - 1);
                                                        NavigateToInput(query);
                                                    }
                                                }
                                            }

                                            // Theme selection from settings page: "set_theme"
                                            if (jsonStr.find(L"\"set_theme\"") != std::wstring::npos) {
                                                bool dark = (jsonStr.find(L"\"dark\"") != std::wstring::npos);
                                                if (g_isDarkMode != dark) {
                                                    g_isDarkMode = dark;
                                                    if (g_hWnd) {
                                                        InvalidateRect(g_hWnd, nullptr, TRUE);
                                                        if (g_hBtnBack) InvalidateRect(g_hBtnBack, nullptr, TRUE);
                                                        if (g_hBtnForward) InvalidateRect(g_hBtnForward, nullptr, TRUE);
                                                        if (g_hBtnReload) InvalidateRect(g_hBtnReload, nullptr, TRUE);
                                                        if (g_hBtnEngine) InvalidateRect(g_hBtnEngine, nullptr, TRUE);
                                                        if (g_hBtnSettings) InvalidateRect(g_hBtnSettings, nullptr, TRUE);
                                                        if (g_hEdit) InvalidateRect(g_hEdit, nullptr, TRUE);
                                                    }
                                                    // Broadcast theme change to all open tabs
                                                    std::wstring msg = L"{\"type\":\"sync_state\",\"theme\":\"" + std::wstring(dark ? L"dark" : L"light") + L"\"}";
                                                    for (auto& t : g_tabs) {
                                                        if (t->webview) {
                                                            t->webview->PostWebMessageAsJson(msg.c_str());
                                                        }
                                                    }
                                                }
                                            }

                                            // Engine selection from settings page: "set_engine"
                                            size_t engPos = jsonStr.find(L"\"index\":");
                                            if (engPos != std::wstring::npos && jsonStr.find(L"\"set_engine\"") != std::wstring::npos) {
                                                int idx = _wtoi(jsonStr.c_str() + engPos + 8);
                                                if (idx >= 0 && idx < (int)g_searchEngines.size()) {
                                                    g_selectedEngineIndex = idx;
                                                    UpdateOmniboxCueBanner();
                                                    if (g_hBtnEngine) InvalidateRect(g_hBtnEngine, nullptr, TRUE);
                                                }
                                            }

                                            // Page ready handshake: sync theme and engine
                                            if (jsonStr.find(L"\"settings_ready\"") != std::wstring::npos ||
                                                jsonStr.find(L"\"homepage_ready\"") != std::wstring::npos) {
                                                std::wstring syncMsg = L"{\"type\":\"sync_state\",\"theme\":\"" + 
                                                    std::wstring(g_isDarkMode ? L"dark" : L"light") + 
                                                    L"\",\"engine\":" + std::to_wstring(g_selectedEngineIndex) + L"}";
                                                sender->PostWebMessageAsJson(syncMsg.c_str());
                                            }
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab->webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [newIndex](ICoreWebView2* webview, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(webview->get_Source(&uri)) && uri) {
                                            if (newIndex < (int)g_tabs.size()) {
                                                g_tabs[newIndex]->url = uri;
                                                if (g_activeTabIndex == newIndex && g_hEdit) {
                                                    if (wcscmp(uri, L"about:blank") == 0 || 
                                                        wcsstr(uri, L"data:text/html") != nullptr ||
                                                        (!g_homePageUrl.empty() && wcsstr(uri, g_homePageUrl.c_str()) != nullptr)) {
                                                        SetWindowTextW(g_hEdit, L"");
                                                    }
                                                    else if (!g_settingsPageUrl.empty() && wcsstr(uri, g_settingsPageUrl.c_str()) != nullptr) {
                                                        SetWindowTextW(g_hEdit, L"haze://settings");
                                                    }
                                                    else {
                                                        SetWindowTextW(g_hEdit, uri);
                                                    }
                                                }
                                            }
                                            CoTaskMemFree(uri);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab->webview->add_DocumentTitleChanged(
                                Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                    [hWnd, newIndex](ICoreWebView2* webview, IUnknown*) -> HRESULT {
                                        LPWSTR title = nullptr;
                                        if (SUCCEEDED(webview->get_DocumentTitle(&title)) && title) {
                                            if (newIndex < (int)g_tabs.size()) {
                                                g_tabs[newIndex]->title = (wcslen(title) > 0) ? title : L"New Tab";
                                                RECT topRc{ 0, 0, 2000, TAB_BAR_HEIGHT };
                                                InvalidateRect(hWnd, &topRc, FALSE);
                                            }
                                            CoTaskMemFree(title);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab->isLoaded = true;

                            if (initialUrl.empty() || initialUrl == L"about:blank") {
                                tab->title = L"New Tab";
                                if (!g_homePageUrl.empty()) {
                                    tab->webview->Navigate(g_homePageUrl.c_str());
                                }
                                else {
                                    tab->webview->NavigateToString(g_homePageHtml.c_str());
                                }
                            }
                            else {
                                tab->webview->Navigate(initialUrl.c_str());
                            }

                            SwitchToTab(newIndex);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void CloseTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;

    if (g_tabs.size() == 1) {
        g_tabs[0]->title = L"New Tab";
        g_tabs[0]->url = L"about:blank";
        if (g_tabs[0]->webview) {
            if (!g_homePageUrl.empty()) {
                g_tabs[0]->webview->Navigate(g_homePageUrl.c_str());
            }
            else {
                g_tabs[0]->webview->NavigateToString(g_homePageHtml.c_str());
            }
        }
        if (g_hEdit) SetWindowTextW(g_hEdit, L"");
        RECT topRc{ 0, 0, 2000, TOP_BAR_HEIGHT };
        InvalidateRect(g_hWnd, &topRc, FALSE);
        return;
    }

    if (g_tabs[index]->controller) {
        g_tabs[index]->controller->Close();
    }
    g_tabs.erase(g_tabs.begin() + index);

    if (g_activeTabIndex >= (int)g_tabs.size()) {
        g_activeTabIndex = (int)g_tabs.size() - 1;
    }
    SwitchToTab(g_activeTabIndex);
}

// --------------------------------------------------------------------------
// WinUI 3 Style Rounded Tray Context Menu Implementation (Anti-Aliased GDI+)
// --------------------------------------------------------------------------
constexpr int MENU_WIDTH = 260;
constexpr int ITEM_HEIGHT = 34;
constexpr int SEPARATOR_HEIGHT = 9;
constexpr int MENU_PADDING_Y = 6;
constexpr int MENU_PADDING_X = 6;

int CalculateMenuHeight() {
    int h = MENU_PADDING_Y * 2;
    for (const auto& item : g_menuItems) {
        if (item.isSeparator) h += SEPARATOR_HEIGHT;
        else h += ITEM_HEIGHT;
    }
    return h;
}

RECT GetItemRect(int index) {
    int y = MENU_PADDING_Y;
    for (int i = 0; i < index; ++i) {
        if (g_menuItems[i].isSeparator) y += SEPARATOR_HEIGHT;
        else y += ITEM_HEIGHT;
    }
    int itemH = g_menuItems[index].isSeparator ? SEPARATOR_HEIGHT : ITEM_HEIGHT;
    return RECT{ MENU_PADDING_X, y, MENU_WIDTH - MENU_PADDING_X, y + itemH };
}

int GetItemFromPoint(POINT pt) {
    for (size_t i = 0; i < g_menuItems.size(); ++i) {
        RECT rc = GetItemRect((int)i);
        if (PtInRect(&rc, pt)) {
            if (g_menuItems[i].isSeparator || g_menuItems[i].isHeader || !g_menuItems[i].isEnabled) {
                return -1;
            }
            return (int)i;
        }
    }
    return -1;
}

void ExecuteMenuAction(MenuAction action) {
    switch (action) {
    case ACTION_OPEN_HAZE:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            if (!g_homePageUrl.empty()) {
                g_tabs[g_activeTabIndex]->webview->Navigate(g_homePageUrl.c_str());
            } else {
                g_tabs[g_activeTabIndex]->webview->NavigateToString(g_homePageHtml.c_str());
            }
        }
        break;
    case ACTION_NEW_TAB:
        RestoreMainWindow(false);
        CreateNewTab(L"about:blank");
        break;
    case ACTION_NEW_WINDOW:
        RestoreMainWindow(false);
        CreateNewTab(L"about:blank");
        break;
    case ACTION_ZOOM_IN:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
            double zoom = 1.0;
            g_tabs[g_activeTabIndex]->controller->get_ZoomFactor(&zoom);
            g_tabs[g_activeTabIndex]->controller->put_ZoomFactor(zoom + 0.1);
        }
        break;
    case ACTION_ZOOM_OUT:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
            double zoom = 1.0;
            g_tabs[g_activeTabIndex]->controller->get_ZoomFactor(&zoom);
            if (zoom > 0.3) g_tabs[g_activeTabIndex]->controller->put_ZoomFactor(zoom - 0.1);
        }
        break;
    case ACTION_ZOOM_RESET:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
            g_tabs[g_activeTabIndex]->controller->put_ZoomFactor(1.0);
        }
        break;
    case ACTION_HISTORY:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"edge://history");
        }
        break;
    case ACTION_DOWNLOADS:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"edge://downloads");
        }
        break;
    case ACTION_PRINT:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->ExecuteScript(L"window.print();", nullptr);
        }
        break;
    case ACTION_SETTINGS:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            if (!g_settingsPageUrl.empty()) {
                g_tabs[g_activeTabIndex]->webview->Navigate(g_settingsPageUrl.c_str());
            } else {
                g_tabs[g_activeTabIndex]->webview->Navigate(L"haze://settings");
            }
        }
        break;
    case ACTION_QUIT:
        DestroyWindow(g_hWnd);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // Menu background according to theme
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 255, 255, 255);
        SolidBrush bgBrush(bgCol);
        g.FillRectangle(&bgBrush, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left), (REAL)(rcClient.bottom - rcClient.top));

        GraphicsPath path;
        AddRoundedRect(path, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left - 1), (REAL)(rcClient.bottom - rcClient.top - 1), 10.0f);

        Color borderCol = g_isDarkMode ? Color(255, 58, 58, 70) : Color(255, 226, 226, 229);
        Pen borderPen(borderCol, 1.0f);
        g.DrawPath(&borderPen, &path);

        FontFamily* pFontFamily = new FontFamily(GetModernFontFamilyName());
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI");
        }
        Gdiplus::Font fontRegular(pFontFamily, 9.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontBold(pFontFamily, 9.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontShortcut(pFontFamily, 8.5f, FontStyleRegular, UnitPoint);

        StringFormat strFormat;
        strFormat.SetAlignment(StringAlignmentNear);
        strFormat.SetLineAlignment(StringAlignmentCenter);

        StringFormat shortcutFormat;
        shortcutFormat.SetAlignment(StringAlignmentFar);
        shortcutFormat.SetLineAlignment(StringAlignmentCenter);

        for (size_t i = 0; i < g_menuItems.size(); ++i) {
            const auto& item = g_menuItems[i];
            RECT itemRc = GetItemRect((int)i);

            if (item.isSeparator) {
                REAL midY = (itemRc.top + itemRc.bottom) / 2.0f;
                Color sepCol = g_isDarkMode ? Color(255, 50, 50, 60) : Color(255, 235, 235, 238);
                Pen sepPen(sepCol, 1.0f);
                g.DrawLine(&sepPen, (REAL)itemRc.left + 8.0f, midY, (REAL)itemRc.right - 8.0f, midY);
                continue;
            }

            if ((int)i == g_hoveredIndex && item.isEnabled && !item.isHeader) {
                GraphicsPath hoverPath;
                AddRoundedRect(hoverPath, (REAL)itemRc.left + 4, (REAL)itemRc.top + 2, (REAL)(itemRc.right - itemRc.left - 8), (REAL)(itemRc.bottom - itemRc.top - 4), 6.0f);
                Color hoverCol = g_isDarkMode ? Color(255, 50, 50, 60) : Color(255, 243, 243, 246);
                SolidBrush hoverBrush(hoverCol);
                g.FillPath(&hoverBrush, &hoverPath);
            }

            Color textCol;
            if (g_isDarkMode) {
                textCol = item.isHeader ? Color(255, 176, 167, 56) : (item.isEnabled ? Color(255, 235, 235, 240) : Color(255, 120, 120, 130));
            } else {
                textCol = item.isHeader ? Color(255, 118, 112, 27) : (item.isEnabled ? Color(255, 36, 36, 38) : Color(255, 150, 150, 155));
            }
            SolidBrush textBrush(textCol);

            // Left text
            RectF layoutRect((REAL)itemRc.left + 16.0f, (REAL)itemRc.top, (REAL)(itemRc.right - itemRc.left - 100.0f), (REAL)(itemRc.bottom - itemRc.top));
            g.DrawString(item.text.c_str(), -1, item.isHeader ? &fontBold : &fontRegular, layoutRect, &strFormat, &textBrush);

            // Right shortcut text
            if (!item.shortcut.empty()) {
                Color scCol = g_isDarkMode ? Color(255, 140, 140, 150) : Color(255, 130, 130, 135);
                SolidBrush scBrush(scCol);
                RectF scRect((REAL)itemRc.left + 100.0f, (REAL)itemRc.top, (REAL)(itemRc.right - itemRc.left - 116.0f), (REAL)(itemRc.bottom - itemRc.top));
                g.DrawString(item.shortcut.c_str(), -1, &fontShortcut, scRect, &shortcutFormat, &scBrush);
            }
        }

        delete pFontFamily;

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int newHover = GetItemFromPoint(pt);
        if (newHover != g_hoveredIndex) {
            g_hoveredIndex = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hoveredIndex != -1) {
            g_hoveredIndex = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int clickedIdx = GetItemFromPoint(pt);
        ShowWindow(hWnd, SW_HIDE);
        if (clickedIdx >= 0 && clickedIdx < (int)g_menuItems.size()) {
            ExecuteMenuAction(g_menuItems[clickedIdx].action);
        }
        return 0;
    }
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void ShowWinUI3Menu() {
    POINT cursor;
    GetCursorPos(&cursor);

    int menuW = MENU_WIDTH;
    int menuH = CalculateMenuHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int posX = cursor.x - (menuW / 2);
    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = cursor.y - menuH - 8;
    if (posY < workArea.top) posY = cursor.y + 8;

    g_hoveredIndex = -1;

    SetWindowPos(g_hMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hMenuWnd);
    SetFocus(g_hMenuWnd);
    InvalidateRect(g_hMenuWnd, nullptr, TRUE);
}

// --------------------------------------------------------------------------
// Search Engine Dropdown Popup Window
// --------------------------------------------------------------------------
constexpr int ENGINE_DROPDOWN_WIDTH = 220;
constexpr int ENGINE_ITEM_HEIGHT = 40;
constexpr int ENGINE_HEADER_HEIGHT = 34;

int CalculateEngineDropdownHeight() {
    return 10 + ENGINE_HEADER_HEIGHT + (int)g_searchEngines.size() * ENGINE_ITEM_HEIGHT + 8;
}

RECT GetEngineItemRect(int index) {
    int y = 10 + ENGINE_HEADER_HEIGHT + index * ENGINE_ITEM_HEIGHT;
    return RECT{ 8, y, ENGINE_DROPDOWN_WIDTH - 8, y + ENGINE_ITEM_HEIGHT };
}

int GetEngineItemFromPoint(POINT pt) {
    for (size_t i = 0; i < g_searchEngines.size(); ++i) {
        RECT rc = GetEngineItemRect((int)i);
        if (PtInRect(&rc, pt)) {
            return (int)i;
        }
    }
    return -1;
}

LRESULT CALLBACK EngineDropdownWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // Background pure white
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left), (REAL)(rcClient.bottom - rcClient.top));

        // Rounded border
        GraphicsPath path;
        AddRoundedRect(path, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left - 1), (REAL)(rcClient.bottom - rcClient.top - 1), 12.0f);
        Pen borderPen(Color(255, 222, 224, 228), 1.2f);
        g.DrawPath(&borderPen, &path);

        FontFamily* pFontFamily = new FontFamily(GetModernFontFamilyName());
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI");
        }
        Gdiplus::Font headerFont(pFontFamily, 9.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font itemFont(pFontFamily, 10.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font itemSelectedFont(pFontFamily, 10.5f, FontStyleBold, UnitPoint);

        // Draw "Search with:" Header
        SolidBrush headerBrush(Color(255, 100, 103, 107));
        StringFormat headerFormat;
        headerFormat.SetAlignment(StringAlignmentNear);
        headerFormat.SetLineAlignment(StringAlignmentCenter);
        RectF headerRc(16.0f, 10.0f, (REAL)ENGINE_DROPDOWN_WIDTH - 32.0f, (REAL)ENGINE_HEADER_HEIGHT);
        g.DrawString(L"Search with:", -1, &headerFont, headerRc, &headerFormat, &headerBrush);

        StringFormat strFormat;
        strFormat.SetAlignment(StringAlignmentNear);
        strFormat.SetLineAlignment(StringAlignmentCenter);

        // Draw Search Engines List
        for (size_t i = 0; i < g_searchEngines.size(); ++i) {
            const auto& eng = g_searchEngines[i];
            RECT itemRc = GetEngineItemRect((int)i);
            bool isHover = ((int)i == g_engineDropdownHoveredIndex);
            bool isSelected = ((int)i == g_selectedEngineIndex);

            if (isHover || isSelected) {
                GraphicsPath hoverPath;
                AddRoundedRect(hoverPath, (REAL)itemRc.left, (REAL)itemRc.top + 2, (REAL)(itemRc.right - itemRc.left), (REAL)(itemRc.bottom - itemRc.top - 4), 8.0f);
                Color hoverCol = isSelected ? Color(255, 240, 244, 252) : Color(255, 245, 246, 248);
                SolidBrush hBrush(hoverCol);
                g.FillPath(&hBrush, &hoverPath);
            }

            // Draw Engine Logo
            if (eng.pBitmap) {
                REAL iconY = itemRc.top + ((itemRc.bottom - itemRc.top - 20) / 2.0f);
                g.DrawImage(eng.pBitmap, (REAL)itemRc.left + 10.0f, iconY, 20.0f, 20.0f);
            }

            // Draw Engine Name
            Color textCol = isSelected ? Color(255, 26, 115, 232) : Color(255, 32, 33, 36);
            SolidBrush textBrush(textCol);
            RectF layoutRect((REAL)itemRc.left + 38.0f, (REAL)itemRc.top, (REAL)(itemRc.right - itemRc.left - 46.0f), (REAL)(itemRc.bottom - itemRc.top));
            g.DrawString(eng.name.c_str(), -1, isSelected ? &itemSelectedFont : &itemFont, layoutRect, &strFormat, &textBrush);
        }

        delete pFontFamily;
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int newHover = GetEngineItemFromPoint(pt);
        if (newHover != g_engineDropdownHoveredIndex) {
            g_engineDropdownHoveredIndex = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_engineDropdownHoveredIndex != -1) {
            g_engineDropdownHoveredIndex = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int clickedIdx = GetEngineItemFromPoint(pt);
        ShowWindow(hWnd, SW_HIDE);
        if (clickedIdx >= 0 && clickedIdx < (int)g_searchEngines.size()) {
            g_selectedEngineIndex = clickedIdx;
            UpdateOmniboxCueBanner();
            if (g_hBtnEngine) InvalidateRect(g_hBtnEngine, nullptr, TRUE);
            if (g_hEdit) {
                SetFocus(g_hEdit);
                SendMessage(g_hEdit, EM_SETSEL, 0, -1);
            }
        }
        return 0;
    }
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void ShowEngineDropdown() {
    if (!g_hEngineDropdownWnd || !g_hBtnEngine) return;

    RECT btnRc;
    GetWindowRect(g_hBtnEngine, &btnRc);

    int menuW = ENGINE_DROPDOWN_WIDTH;
    int menuH = CalculateEngineDropdownHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int posX = btnRc.left - 6;
    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = btnRc.bottom + 6;
    if (posY + menuH > workArea.bottom) posY = btnRc.top - menuH - 6;

    g_engineDropdownHoveredIndex = -1;

    SetWindowPos(g_hEngineDropdownWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hEngineDropdownWnd);
    SetFocus(g_hEngineDropdownWnd);
    InvalidateRect(g_hEngineDropdownWnd, nullptr, TRUE);
}

void ShowSettingsDropdown() {
    if (!g_hMenuWnd || !g_hBtnSettings) return;

    RECT btnRc;
    GetWindowRect(g_hBtnSettings, &btnRc);

    int menuW = MENU_WIDTH;
    int menuH = CalculateMenuHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    // Align right edge of menu with button right edge
    int posX = btnRc.right - menuW + 8;
    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = btnRc.bottom + 6;
    if (posY + menuH > workArea.bottom) posY = btnRc.top - menuH - 6;

    g_hoveredIndex = -1;

    SetWindowPos(g_hMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hMenuWnd);
    SetFocus(g_hMenuWnd);
    InvalidateRect(g_hMenuWnd, nullptr, TRUE);
}

void SetupMenuItems() {
    g_menuItems.clear();
    // Header
    g_menuItems.push_back({ std::wstring(APP_NAME) + L" " + APP_VERSION, L"", ACTION_VERSION, false, true, false });
    g_menuItems.push_back({ L"", L"", (MenuAction)0, true, false, false });

    // Window & Tab operations
    g_menuItems.push_back({ L"New tab", L"Ctrl+T", ACTION_NEW_TAB, false, false, true });
    g_menuItems.push_back({ L"New window", L"Ctrl+N", ACTION_NEW_WINDOW, false, false, true });
    g_menuItems.push_back({ L"Open HAZE Home", L"", ACTION_OPEN_HAZE, false, false, true });
    g_menuItems.push_back({ L"", L"", (MenuAction)0, true, false, false });

    // Zoom Controls
    g_menuItems.push_back({ L"Zoom In", L"Ctrl++", ACTION_ZOOM_IN, false, false, true });
    g_menuItems.push_back({ L"Zoom Out", L"Ctrl+-", ACTION_ZOOM_OUT, false, false, true });
    g_menuItems.push_back({ L"Reset Zoom (100%)", L"Ctrl+0", ACTION_ZOOM_RESET, false, false, true });
    g_menuItems.push_back({ L"", L"", (MenuAction)0, true, false, false });

    // Navigation & Tools
    g_menuItems.push_back({ L"History", L"Ctrl+H", ACTION_HISTORY, false, false, true });
    g_menuItems.push_back({ L"Downloads", L"Ctrl+J", ACTION_DOWNLOADS, false, false, true });
    g_menuItems.push_back({ L"Print", L"Ctrl+P", ACTION_PRINT, false, false, true });
    g_menuItems.push_back({ L"", L"", (MenuAction)0, true, false, false });

    // Settings & Exit
    g_menuItems.push_back({ L"Settings", L"", ACTION_SETTINGS, false, false, true });
    g_menuItems.push_back({ L"", L"", (MenuAction)0, true, false, false });
    g_menuItems.push_back({ L"Close HAZE", L"", ACTION_QUIT, false, false, true });
}

// --------------------------------------------------------------------------
// Main Window Procedure
// --------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        const wchar_t* primaryFont = GetModernFontFamilyName();
        // 12pt font at standard 96 DPI: -MulDiv(12, 96, 72) = -16 (or height 20)
        HFONT hFont = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, primaryFont);
        if (!hFont) {
            hFont = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }

        g_hBtnBack = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            14, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_BACK, g_hInstance, nullptr);
        g_originalNavBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnBack, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        g_hBtnForward = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            50, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_FORWARD, g_hInstance, nullptr);
        SetWindowLongPtr(g_hBtnForward, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        g_hBtnReload = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            86, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_RELOAD, g_hInstance, nullptr);
        SetWindowLongPtr(g_hBtnReload, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        // Search engine selector button inside omnibox
        g_hBtnEngine = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            140, TAB_BAR_HEIGHT + 13, 42, 34,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_ENGINE, g_hInstance, nullptr);
        g_originalEngineBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnEngine, GWLP_WNDPROC, (LONG_PTR)EngineButtonSubclassProc);

        // Omnibox Edit Control
        g_hEdit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            190, TAB_BAR_HEIGHT + 14, 400, 32,
            hWnd, (HMENU)(INT_PTR)IDC_EDIT_URL, g_hInstance, nullptr);

        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        UpdateOmniboxCueBanner();
        g_originalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // Arrow Action Button
        g_hBtnSearch = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            550, TAB_BAR_HEIGHT + 11, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SEARCH, g_hInstance, nullptr);

        g_originalSearchBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnSearch, GWLP_WNDPROC, (LONG_PTR)SearchButtonSubclassProc);

        // Chevron Settings Dropdown Button
        g_hBtnSettings = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            586, TAB_BAR_HEIGHT + 11, 28, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, g_hInstance, nullptr);

        g_originalSettingsBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnSettings, GWLP_WNDPROC, (LONG_PTR)SettingsButtonSubclassProc);

        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hAppIcon;
        wcscpy_s(g_nid.szTip, L"HAZE Browser");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        g_homePageHtml = LoadHomePageHtml();
        InitSettingsPageUrl();
        CreateNewTab(L"about:blank");
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDC_BTN_SEARCH) {
            wchar_t buffer[2048] = { 0 };
            GetWindowTextW(g_hEdit, buffer, ARRAYSIZE(buffer));
            NavigateToInput(buffer);
        }
        else if (wmId == IDC_BTN_ENGINE) {
            ShowEngineDropdown();
        }
        else if (wmId == IDC_BTN_SETTINGS) {
            ShowSettingsDropdown();
        }
        else if (wmId == IDC_BTN_BACK) {
            if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
                g_tabs[g_activeTabIndex]->webview->GoBack();
            }
        }
        else if (wmId == IDC_BTN_FORWARD) {
            if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
                g_tabs[g_activeTabIndex]->webview->GoForward();
            }
        }
        else if (wmId == IDC_BTN_RELOAD) {
            if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
                g_tabs[g_activeTabIndex]->webview->Reload();
            }
        }
        break;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        ResizeControls(width, height);
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        if (g_isDarkMode) {
            SetTextColor(hdc, RGB(237, 237, 240));
            SetBkColor(hdc, RGB(40, 40, 50));
            if (!s_editBrushDark) s_editBrushDark = CreateSolidBrush(RGB(40, 40, 50));
            return (LRESULT)s_editBrushDark;
        } else {
            SetTextColor(hdc, RGB(60, 58, 54));
            SetBkColor(hdc, RGB(254, 249, 235));
            if (!s_editBrushLight) s_editBrushLight = CreateSolidBrush(RGB(254, 249, 235));
            return (LRESULT)s_editBrushLight;
        }
    }
    case WM_CTLCOLORBTN: {
        HWND hBtn = (HWND)lParam;
        if (hBtn == g_hBtnEngine || hBtn == g_hBtnSearch) {
            if (g_isDarkMode) {
                if (!s_pillBrushDark) s_pillBrushDark = CreateSolidBrush(RGB(40, 40, 50));
                return (LRESULT)s_pillBrushDark;
            } else {
                if (!s_pillBrushLight) s_pillBrushLight = CreateSolidBrush(RGB(254, 249, 235));
                return (LRESULT)s_pillBrushLight;
            }
        }
        if (g_isDarkMode) {
            static HBRUSH s_tbBrushDark = CreateSolidBrush(RGB(34, 34, 40));
            return (LRESULT)s_tbBrushDark;
        } else {
            static HBRUSH s_tbBrushLight = CreateSolidBrush(RGB(254, 249, 235));
            return (LRESULT)s_tbBrushLight;
        }
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);
        int width = clientRc.right - clientRc.left;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(hWnd, &wp);
        bool isMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);

        // 1. Tab strip background
        Color stripCol = g_isDarkMode ? Color(255, 24, 24, 28) : Color(255, 233, 228, 190);
        SolidBrush stripBrush(stripCol);
        g.FillRectangle(&stripBrush, 0.0f, 0.0f, (REAL)width, (REAL)TAB_BAR_HEIGHT);

        // 2. Toolbar & Navigation row background (Cream #FEF9EB / Dark #222228)
        Color toolbarCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        SolidBrush toolbarBrush(toolbarCol);
        g.FillRectangle(&toolbarBrush, 0.0f, (REAL)TAB_BAR_HEIGHT, (REAL)width, (REAL)(TOP_BAR_HEIGHT - TAB_BAR_HEIGHT));

        FontFamily* pFontFamily = new FontFamily(GetModernFontFamilyName());
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI");
        }
        Gdiplus::Font tabFont(pFontFamily, 9.5f, FontStyleRegular, UnitPoint);

        StringFormat strFormat;
        strFormat.SetAlignment(StringAlignmentNear);
        strFormat.SetLineAlignment(StringAlignmentCenter);
        strFormat.SetTrimming(StringTrimmingEllipsisCharacter);
        strFormat.SetFormatFlags(StringFormatFlagsNoWrap);

        // 3. Render Anti-Aliased Clean Rounded Rectangular tabs
        // Pass 1: Render all inactive tabs first
        REAL tabRadius = 9.0f;
        for (size_t i = 0; i < g_tabs.size(); ++i) {
            if ((int)i == g_activeTabIndex) continue;

            RECT tabRc = GetTabRect((int)i, width);
            bool isHover = ((int)i == g_hoveredTabIndex);

            REAL tx = (REAL)tabRc.left;
            REAL ty = (REAL)tabRc.top + 2.0f;
            REAL tw = (REAL)(tabRc.right - tabRc.left);
            REAL th = (REAL)(tabRc.bottom - tabRc.top - 4.0f);

            GraphicsPath inPath;
            AddRoundedRect(inPath, tx, ty, tw, th, tabRadius);

            if (isHover) {
                Color inHoverCol = g_isDarkMode ? Color(255, 48, 48, 56) : (isMaximized ? Color(255, 235, 230, 185) : Color(255, 242, 238, 208));
                SolidBrush hoverBrush(inHoverCol);
                g.FillPath(&hoverBrush, &inPath);
            }

            if (g_hAppIcon) {
                DrawIconEx(hdc, tabRc.left + 16, tabRc.top + ((tabRc.bottom - tabRc.top - 20) / 2), g_hAppIcon, 20, 20, 0, nullptr, DI_NORMAL);
            }

            Color textCol = g_isDarkMode ? Color(255, 160, 160, 176) : Color(255, 115, 112, 92);
            SolidBrush textBrush(textCol);
            RectF titleRect((REAL)tabRc.left + 46.0f, (REAL)tabRc.top, (REAL)(tabRc.right - tabRc.left - 80.0f), (REAL)(tabRc.bottom - tabRc.top));
            g.DrawString(g_tabs[i]->title.c_str(), -1, &tabFont, titleRect, &strFormat, &textBrush);

            RECT closeRc = GetTabCloseRect(tabRc);
            bool isCloseHover = ((int)i == g_hoveredCloseTabIndex);
            if (isCloseHover) {
                Color closeBgCol = g_isDarkMode ? Color(255, 60, 60, 72) : Color(255, 230, 224, 184);
                SolidBrush closeBg(closeBgCol);
                g.FillEllipse(&closeBg, (REAL)closeRc.left, (REAL)closeRc.top, (REAL)(closeRc.right - closeRc.left), (REAL)(closeRc.bottom - closeRc.top));
            }

            Color inCloseCol;
            if (isCloseHover) {
                inCloseCol = Color(255, 217, 48, 37);
            } else {
                inCloseCol = g_isDarkMode ? Color(255, 160, 160, 176) : Color(255, 115, 112, 92);
            }
            Pen closePen(inCloseCol, 1.6f);
            closePen.SetStartCap(LineCapRound);
            closePen.SetEndCap(LineCapRound);

            REAL cx = (closeRc.left + closeRc.right) / 2.0f;
            REAL cy = (closeRc.top + closeRc.bottom) / 2.0f;
            REAL sz = 4.0f;
            g.DrawLine(&closePen, cx - sz, cy - sz, cx + sz, cy + sz);
            g.DrawLine(&closePen, cx + sz, cy - sz, cx - sz, cy + sz);
        }

        // Pass 2: Render Active Tab as a perfect rounded rectangle
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) {
            int i = g_activeTabIndex;
            RECT tabRc = GetTabRect(i, width);

            REAL tx = (REAL)tabRc.left;
            REAL ty = (REAL)tabRc.top + 2.0f;
            REAL tw = (REAL)(tabRc.right - tabRc.left);
            REAL th = (REAL)(tabRc.bottom - tabRc.top - 4.0f);

            GraphicsPath activeTabPath;
            AddRoundedRect(activeTabPath, tx, ty, tw, th, tabRadius);

            // Fill active tab seamlessly with toolbar brush
            g.FillPath(&toolbarBrush, &activeTabPath);

            if (g_hAppIcon) {
                DrawIconEx(hdc, tabRc.left + 16, tabRc.top + ((tabRc.bottom - tabRc.top - 20) / 2), g_hAppIcon, 20, 20, 0, nullptr, DI_NORMAL);
            }

            Color textCol = g_isDarkMode ? Color(255, 237, 237, 240) : Color(255, 45, 45, 42);
            SolidBrush textBrush(textCol);
            RectF titleRect((REAL)tabRc.left + 46.0f, (REAL)tabRc.top, (REAL)(tabRc.right - tabRc.left - 80.0f), (REAL)(tabRc.bottom - tabRc.top));
            g.DrawString(g_tabs[i]->title.c_str(), -1, &tabFont, titleRect, &strFormat, &textBrush);

            RECT closeRc = GetTabCloseRect(tabRc);
            bool isCloseHover = (i == g_hoveredCloseTabIndex);
            if (isCloseHover) {
                Color closeBgCol = g_isDarkMode ? Color(255, 60, 60, 72) : Color(255, 230, 224, 184);
                SolidBrush closeBg(closeBgCol);
                g.FillEllipse(&closeBg, (REAL)closeRc.left, (REAL)closeRc.top, (REAL)(closeRc.right - closeRc.left), (REAL)(closeRc.bottom - closeRc.top));
            }

            Color actCloseCol;
            if (isCloseHover) {
                actCloseCol = Color(255, 217, 48, 37);
            } else {
                actCloseCol = g_isDarkMode ? Color(255, 160, 160, 176) : Color(255, 115, 112, 92);
            }
            Pen closePen(actCloseCol, 1.6f);
            closePen.SetStartCap(LineCapRound);
            closePen.SetEndCap(LineCapRound);

            REAL cx = (closeRc.left + closeRc.right) / 2.0f;
            REAL cy = (closeRc.top + closeRc.bottom) / 2.0f;
            REAL sz = 4.0f;
            g.DrawLine(&closePen, cx - sz, cy - sz, cx + sz, cy + sz);
            g.DrawLine(&closePen, cx + sz, cy - sz, cx - sz, cy + sz);
        }

        // Draw crisp "+" New Tab Button
        RECT newTabRc = GetNewTabBtnRect(width);
        if (g_isHoveringNewTab) {
            Color ntHoverCol = g_isDarkMode ? Color(255, 48, 48, 56) : Color(255, 222, 216, 175);
            SolidBrush ntBrush(ntHoverCol);
            g.FillEllipse(&ntBrush, (REAL)newTabRc.left, (REAL)newTabRc.top, (REAL)(newTabRc.right - newTabRc.left), (REAL)(newTabRc.bottom - newTabRc.top));
        }
        Pen plusPen(g_isDarkMode ? Color(255, 200, 200, 215) : Color(255, 60, 58, 54), 1.5f);
        plusPen.SetStartCap(LineCapRound);
        plusPen.SetEndCap(LineCapRound);
        REAL ntcx = (newTabRc.left + newTabRc.right) / 2.0f;
        REAL ntcy = (newTabRc.top + newTabRc.bottom) / 2.0f;
        g.DrawLine(&plusPen, ntcx - 5.0f, ntcy, ntcx + 5.0f, ntcy);
        g.DrawLine(&plusPen, ntcx, ntcy - 5.0f, ntcx, ntcy + 5.0f);

        // 4. Draw Pill-Shaped Rounded Search Bar with Olive border (#76701B / RGB(118, 112, 27))
        RECT searchRc = GetSearchBarOuterRect(width);
        Color borderColor;
        if (g_isDarkMode) {
            borderColor = g_isEditFocused ? Color(255, 176, 167, 56) : Color(255, 62, 62, 76);
        } else {
            borderColor = g_isEditFocused ? Color(255, 95, 90, 15) : Color(255, 118, 112, 27);
        }
        Pen borderPen(borderColor, 1.6f);

        REAL sx = (REAL)searchRc.left;
        REAL sy = (REAL)searchRc.top;
        REAL sw = (REAL)(searchRc.right - searchRc.left);
        REAL sh = (REAL)(searchRc.bottom - searchRc.top);
        REAL sbRadius = sh / 2.0f;

        GraphicsPath sbPath;
        sbPath.AddArc(sx, sy, sbRadius * 2.0f, sbRadius * 2.0f, 90.0f, 180.0f);
        sbPath.AddArc(sx + sw - sbRadius * 2.0f, sy, sbRadius * 2.0f, sbRadius * 2.0f, 270.0f, 180.0f);
        sbPath.CloseFigure();

        Color omniBgCol = g_isDarkMode ? Color(255, 40, 40, 50) : Color(255, 254, 249, 235);
        SolidBrush omniboxBgBrush(omniBgCol);
        g.FillPath(&omniboxBgBrush, &sbPath);
        g.DrawPath(&borderPen, &sbPath);

        // 5. Title bar Minimize, Maximize, Close Buttons on extreme right
        RECT minRc = GetCaptionBtnRect(CAPTION_BTN_MINIMIZE, width);
        RECT maxRc = GetCaptionBtnRect(CAPTION_BTN_MAXIMIZE, width);
        RECT clsRc = GetCaptionBtnRect(CAPTION_BTN_CLOSE, width);

        // Draw Close button
        if (g_hoveredCaptionBtn == CAPTION_BTN_CLOSE) {
            SolidBrush closeHover(Color(255, 232, 17, 35));
            g.FillRectangle(&closeHover, (REAL)clsRc.left, (REAL)clsRc.top, (REAL)(clsRc.right - clsRc.left), (REAL)(clsRc.bottom - clsRc.top));
        }
        Pen clsPen(g_hoveredCaptionBtn == CAPTION_BTN_CLOSE ? Color(255, 255, 255, 255) : (g_isDarkMode ? Color(255, 180, 180, 195) : Color(255, 81, 79, 66)), 1.3f);
        clsPen.SetStartCap(LineCapRound);
        clsPen.SetEndCap(LineCapRound);
        REAL clscx = (clsRc.left + clsRc.right) / 2.0f;
        REAL clscy = (clsRc.top + clsRc.bottom) / 2.0f;
        g.DrawLine(&clsPen, clscx - 5.0f, clscy - 5.0f, clscx + 5.0f, clscy + 5.0f);
        g.DrawLine(&clsPen, clscx + 5.0f, clscy - 5.0f, clscx - 5.0f, clscy + 5.0f);

        // Draw Maximize / Restore button
        if (g_hoveredCaptionBtn == CAPTION_BTN_MAXIMIZE) {
            Color maxHoverCol = g_isDarkMode ? Color(255, 48, 48, 56) : Color(255, 222, 216, 175);
            SolidBrush maxHover(maxHoverCol);
            g.FillRectangle(&maxHover, (REAL)maxRc.left, (REAL)maxRc.top, (REAL)(maxRc.right - maxRc.left), (REAL)(maxRc.bottom - maxRc.top));
        }
        Pen maxPen(g_isDarkMode ? Color(255, 180, 180, 195) : Color(255, 81, 79, 66), 1.3f);
        REAL maxcx = (maxRc.left + maxRc.right) / 2.0f;
        REAL maxcy = (maxRc.top + maxRc.bottom) / 2.0f;
        if (isMaximized) {
            // Restore symbol (two overlapping squares)
            g.DrawRectangle(&maxPen, maxcx - 3.5f, maxcy - 5.5f, 8.0f, 8.0f);
            SolidBrush fillBg(g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 218, 212, 160));
            g.FillRectangle(&fillBg, maxcx - 5.5f, maxcy - 3.5f, 8.0f, 8.0f);
            g.DrawRectangle(&maxPen, maxcx - 5.5f, maxcy - 3.5f, 8.0f, 8.0f);
        } else {
            // Single square
            g.DrawRectangle(&maxPen, maxcx - 5.0f, maxcy - 5.0f, 10.0f, 10.0f);
        }

        // Draw Minimize button
        if (g_hoveredCaptionBtn == CAPTION_BTN_MINIMIZE) {
            Color minHoverCol = g_isDarkMode ? Color(255, 48, 48, 56) : Color(255, 222, 216, 175);
            SolidBrush minHover(minHoverCol);
            g.FillRectangle(&minHover, (REAL)minRc.left, (REAL)minRc.top, (REAL)(minRc.right - minRc.left), (REAL)(minRc.bottom - minRc.top));
        }
        Pen minPen(g_isDarkMode ? Color(255, 180, 180, 195) : Color(255, 81, 79, 66), 1.4f);
        minPen.SetStartCap(LineCapRound);
        minPen.SetEndCap(LineCapRound);
        REAL mincx = (minRc.left + minRc.right) / 2.0f;
        REAL mincy = (minRc.top + minRc.bottom) / 2.0f;
        g.DrawLine(&minPen, mincx - 5.0f, mincy, mincx + 5.0f, mincy);

        // 6. Divider Line separating top bar from WebView
        Pen divPen(Color(255, 225, 220, 195), 1.0f);
        g.DrawLine(&divPen, 0.0f, (REAL)TOP_BAR_HEIGHT - 1.0f, (REAL)width, (REAL)TOP_BAR_HEIGHT - 1.0f);

        delete pFontFamily;

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right - rc.left;

        int newHoverTab = -1;
        int newHoverClose = -1;
        bool newHoverNewTab = false;
        CaptionButtonType newHoverCaption = CAPTION_BTN_NONE;

        if (pt.y < TAB_BAR_HEIGHT) {
            RECT minRc = GetCaptionBtnRect(CAPTION_BTN_MINIMIZE, width);
            RECT maxRc = GetCaptionBtnRect(CAPTION_BTN_MAXIMIZE, width);
            RECT clsRc = GetCaptionBtnRect(CAPTION_BTN_CLOSE, width);

            if (PtInRect(&clsRc, pt)) {
                newHoverCaption = CAPTION_BTN_CLOSE;
            } else if (PtInRect(&maxRc, pt)) {
                newHoverCaption = CAPTION_BTN_MAXIMIZE;
            } else if (PtInRect(&minRc, pt)) {
                newHoverCaption = CAPTION_BTN_MINIMIZE;
            } else {
                for (size_t i = 0; i < g_tabs.size(); ++i) {
                    RECT tabRc = GetTabRect((int)i, width);
                    if (PtInRect(&tabRc, pt)) {
                        newHoverTab = (int)i;
                        RECT closeRc = GetTabCloseRect(tabRc);
                        if (PtInRect(&closeRc, pt)) {
                            newHoverClose = (int)i;
                        }
                        break;
                    }
                }

                RECT ntRc = GetNewTabBtnRect(width);
                if (PtInRect(&ntRc, pt)) {
                    newHoverNewTab = true;
                }
            }
        }

        if (newHoverTab != g_hoveredTabIndex || newHoverClose != g_hoveredCloseTabIndex || newHoverNewTab != g_isHoveringNewTab || newHoverCaption != g_hoveredCaptionBtn) {
            g_hoveredTabIndex = newHoverTab;
            g_hoveredCloseTabIndex = newHoverClose;
            g_isHoveringNewTab = newHoverNewTab;
            g_hoveredCaptionBtn = newHoverCaption;

            RECT topRc{ 0, 0, width, TAB_BAR_HEIGHT };
            InvalidateRect(hWnd, &topRc, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hoveredTabIndex != -1 || g_hoveredCloseTabIndex != -1 || g_isHoveringNewTab || g_hoveredCaptionBtn != CAPTION_BTN_NONE) {
            g_hoveredTabIndex = -1;
            g_hoveredCloseTabIndex = -1;
            g_isHoveringNewTab = false;
            g_hoveredCaptionBtn = CAPTION_BTN_NONE;

            RECT rc;
            GetClientRect(hWnd, &rc);
            RECT topRc{ 0, 0, rc.right - rc.left, TAB_BAR_HEIGHT };
            InvalidateRect(hWnd, &topRc, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right - rc.left;

        if (pt.y < TAB_BAR_HEIGHT) {
            RECT minRc = GetCaptionBtnRect(CAPTION_BTN_MINIMIZE, width);
            RECT maxRc = GetCaptionBtnRect(CAPTION_BTN_MAXIMIZE, width);
            RECT clsRc = GetCaptionBtnRect(CAPTION_BTN_CLOSE, width);

            if (PtInRect(&clsRc, pt)) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (PtInRect(&maxRc, pt)) {
                WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
                GetWindowPlacement(hWnd, &wp);
                if (wp.showCmd == SW_SHOWMAXIMIZED) {
                    ShowWindow(hWnd, SW_RESTORE);
                } else {
                    ShowWindow(hWnd, SW_MAXIMIZE);
                }
                return 0;
            }
            if (PtInRect(&minRc, pt)) {
                ShowWindow(hWnd, SW_MINIMIZE);
                return 0;
            }

            RECT ntRc = GetNewTabBtnRect(width);
            if (PtInRect(&ntRc, pt)) {
                CreateNewTab(L"about:blank");
                return 0;
            }

            for (size_t i = 0; i < g_tabs.size(); ++i) {
                RECT tabRc = GetTabRect((int)i, width);
                if (PtInRect(&tabRc, pt)) {
                    RECT closeRc = GetTabCloseRect(tabRc);
                    if (PtInRect(&closeRc, pt)) {
                        CloseTab((int)i);
                    }
                    else {
                        SwitchToTab((int)i);
                    }
                    return 0;
                }
            }
        }
        break;
    }
    case WM_NCCALCSIZE: {
        if (wParam) {
            // Remove standard window frame and caption, make client area fill entire window
            return 0;
        }
        break;
    }
    case WM_NCHITTEST: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);
        int w = clientRc.right - clientRc.left;
        int h = clientRc.bottom - clientRc.top;

        // Resize borders (8px)
        const int borderThickness = 8;
        if (!IsZoomed(hWnd)) {
            if (pt.y < borderThickness) {
                if (pt.x < borderThickness) return HTTOPLEFT;
                if (pt.x >= w - borderThickness) return HTTOPRIGHT;
                return HTTOP;
            }
            if (pt.y >= h - borderThickness) {
                if (pt.x < borderThickness) return HTBOTTOMLEFT;
                if (pt.x >= w - borderThickness) return HTBOTTOMRIGHT;
                return HTBOTTOM;
            }
            if (pt.x < borderThickness) return HTLEFT;
            if (pt.x >= w - borderThickness) return HTRIGHT;
        }

        // Inside Tab/Title bar area
        if (pt.y < TAB_BAR_HEIGHT) {
            // Check Caption Buttons (Minimize, Maximize, Close)
            RECT minRc = GetCaptionBtnRect(CAPTION_BTN_MINIMIZE, w);
            RECT maxRc = GetCaptionBtnRect(CAPTION_BTN_MAXIMIZE, w);
            RECT clsRc = GetCaptionBtnRect(CAPTION_BTN_CLOSE, w);
            if (PtInRect(&minRc, pt) || PtInRect(&maxRc, pt) || PtInRect(&clsRc, pt)) {
                return HTCLIENT;
            }

            // Check Tabs and New Tab button
            for (size_t i = 0; i < g_tabs.size(); ++i) {
                RECT tabRc = GetTabRect((int)i, w);
                if (PtInRect(&tabRc, pt)) return HTCLIENT;
            }
            RECT newTabRc = GetNewTabBtnRect(w);
            if (PtInRect(&newTabRc, pt)) return HTCLIENT;

            // Otherwise, anywhere else in the tab strip is draggable window caption!
            return HTCAPTION;
        }

        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        lpMMI->ptMinTrackSize.x = MIN_WINDOW_WIDTH;
        lpMMI->ptMinTrackSize.y = MIN_WINDOW_HEIGHT;
        return 0;
    }
    case WM_CLOSE: {
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowWinUI3Menu();
        }
        else if (lParam == WM_LBUTTONDBLCLK) {
            RestoreMainWindow(false);
        }
        break;
    }
    case WM_DESTROY: {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInstance = hInstance;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icex{ sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_hAppIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_HAZE_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!g_hAppIcon) {
        g_hAppIcon = (HICON)LoadImageW(nullptr, L"app.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }

    LoadSearchEngineIcons();
    SetupMenuItems();

    // Register WinUI 3 Tray Menu Class
    WNDCLASSEXW wmc{ sizeof(WNDCLASSEXW) };
    wmc.style = CS_HREDRAW | CS_VREDRAW;
    wmc.lpfnWndProc = MenuWndProc;
    wmc.hInstance = hInstance;
    wmc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wmc.lpszClassName = L"HazeWinUI3MenuClass";
    RegisterClassExW(&wmc);

    g_hMenuWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeWinUI3MenuClass", L"",
        WS_POPUP,
        0, 0, MENU_WIDTH, CalculateMenuHeight(),
        nullptr, nullptr, hInstance, nullptr);

    if (g_hMenuWnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    // Register Search Engine Dropdown Class
    WNDCLASSEXW wedc{ sizeof(WNDCLASSEXW) };
    wedc.style = CS_HREDRAW | CS_VREDRAW;
    wedc.lpfnWndProc = EngineDropdownWndProc;
    wedc.hInstance = hInstance;
    wedc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wedc.lpszClassName = L"HazeEngineDropdownClass";
    RegisterClassExW(&wedc);

    g_hEngineDropdownWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeEngineDropdownClass", L"",
        WS_POPUP,
        0, 0, ENGINE_DROPDOWN_WIDTH, CalculateEngineDropdownHeight(),
        nullptr, nullptr, hInstance, nullptr);

    if (g_hEngineDropdownWnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hEngineDropdownWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    // Register Main Browser Window Class
    WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = g_hAppIcon;
    wcex.hIconSm = g_hAppIcon;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcex.lpszClassName = L"HazeDesktopBrowserClass";
    RegisterClassExW(&wcex);

    g_hWnd = CreateWindowExW(
        0, L"HazeDesktopBrowserClass", APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, MIN_WINDOW_WIDTH, MIN_WINDOW_HEIGHT,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        return 0;
    }

    // Enable native modern DWM shadow and rounded corners for custom frameless window
    MARGINS shadowMargins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(g_hWnd, &shadowMargins);
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    if (g_hAppIcon) {
        SendMessage(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)g_hAppIcon);
        SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hAppIcon);
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hMenuWnd) {
        DestroyWindow(g_hMenuWnd);
    }

    if (g_hEngineDropdownWnd) {
        DestroyWindow(g_hEngineDropdownWnd);
    }

    for (auto& eng : g_searchEngines) {
        if (eng.pBitmap) {
            delete eng.pBitmap;
            eng.pBitmap = nullptr;
        }
    }

    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
    }

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return (int)msg.wParam;
}
