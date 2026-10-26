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
#include <cmath>
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
constexpr int TAB_HEIGHT = 36;
constexpr int TAB_TOP = (TAB_BAR_HEIGHT - TAB_HEIGHT) / 2; // 9px: perfectly centered with equal 9px gap above and below

constexpr int SEARCH_BAR_HEIGHT = 46; // Spacious 46px pill for clear, unclipped 15-16pt typography
constexpr int NAV_BUTTONS_WIDTH = 132;

constexpr int IDC_EDIT_URL = 101;
constexpr int IDC_BTN_SEARCH = 102;
constexpr int IDC_BTN_BACK = 103;
constexpr int IDC_BTN_FORWARD = 104;
constexpr int IDC_BTN_RELOAD = 105;
constexpr int IDC_BTN_ENGINE = 106;
constexpr int IDC_BTN_SETTINGS = 107;
constexpr int IDC_BTN_PROFILE = 108;

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
    Gdiplus::Bitmap* pFaviconBitmap = nullptr;
    EventRegistrationToken faviconToken{};

    ~BrowserTab() {
        if (pFaviconBitmap) {
            delete pFaviconBitmap;
            pFaviconBitmap = nullptr;
        }
    }
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
HWND g_hBtnProfile = nullptr;
HWND g_hBtnSettings = nullptr;
HWND g_hTrayMenuWnd = nullptr;
HWND g_hInAppMenuWnd = nullptr;
HWND g_hProfileMenuWnd = nullptr;
HWND g_hEngineDropdownWnd = nullptr;

WNDPROC g_originalEditProc = nullptr;
WNDPROC g_originalSearchBtnProc = nullptr;
WNDPROC g_originalEngineBtnProc = nullptr;
WNDPROC g_originalNavBtnProc = nullptr;
WNDPROC g_originalSettingsBtnProc = nullptr;
WNDPROC g_originalProfileBtnProc = nullptr;

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
bool g_isProfileBtnHovered = false;
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

enum GridMenuIconType {
    GRID_ICON_NEW_TAB = 0,
    GRID_ICON_NEW_WINDOW,
    GRID_ICON_HOME,
    GRID_ICON_HISTORY,
    GRID_ICON_DOWNLOADS,
    GRID_ICON_PRINT,
    GRID_ICON_ZOOM,
    GRID_ICON_SETTINGS,
    GRID_ICON_QUIT
};

struct GridMenuItem {
    std::wstring text;
    MenuAction action;
    GridMenuIconType iconType;
};

std::vector<GridMenuItem> g_gridItems;
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
        // Modern clean browser UI font: Segoe UI Variable Text (Windows 11) or Segoe UI, with perfect font metrics
        FontFamily testSegoeVar(L"Segoe UI Variable Text");
        if (testSegoeVar.GetLastStatus() == Ok) {
            s_fontName = L"Segoe UI Variable Text";
        } else {
            FontFamily testSegoe(L"Segoe UI");
            if (testSegoe.GetLastStatus() == Ok) {
                s_fontName = L"Segoe UI";
            } else {
                s_fontName = L"Arial";
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
    int startX = 126; // Clean spacing after Reload button (ends at x=118)
    int settingsBtnSize = 32;
    int settingsMarginRight = 14;
    int settingsBtnX = clientWidth - settingsBtnSize - settingsMarginRight;
    int profileBtnX = settingsBtnX - 38; // 38px spacing before settings button
    int endX = profileBtnX - 12; // Omnibox cleanly ends 12px before profile button
    if (endX < startX + 250) {
        endX = startX + 250;
    }
    int top = TAB_BAR_HEIGHT + (TOP_BAR_HEIGHT - TAB_BAR_HEIGHT - SEARCH_BAR_HEIGHT) / 2;
    return RECT{ startX, top, endX, top + SEARCH_BAR_HEIGHT };
}

void UpdateOmniboxCueBanner() {
    if (g_hEdit && g_selectedEngineIndex >= 0 && g_selectedEngineIndex < (int)g_searchEngines.size()) {
        const auto& engine = g_searchEngines[g_selectedEngineIndex];
        SendMessage(g_hEdit, EM_SETCUEBANNER, TRUE, (LPARAM)engine.placeholder.c_str());
    }
}

void ResizeControls(int width, int height) {
    if (width <= 0 || height <= 0) return;

    int navBtnSize = 32;
    int navY = TAB_BAR_HEIGHT + ((TOP_BAR_HEIGHT - TAB_BAR_HEIGHT - navBtnSize) / 2);
    if (g_hBtnBack)    SetWindowPos(g_hBtnBack, nullptr, 14, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnForward) SetWindowPos(g_hBtnForward, nullptr, 50, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnReload)  SetWindowPos(g_hBtnReload, nullptr, 86, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);

    // Search Bar layout
    RECT sb = GetSearchBarOuterRect(width);

    // Engine selector button with icon + dropdown caret inside pill
    int engineBtnW = 50;
    int engineBtnH = 32;
    int engineBtnX = sb.left + 8;
    int engineBtnY = sb.top + (SEARCH_BAR_HEIGHT - engineBtnH) / 2;

    if (g_hBtnEngine) {
        SetWindowPos(g_hBtnEngine, nullptr, engineBtnX, engineBtnY, engineBtnW, engineBtnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

        // Edit box - 28px height with plenty of vertical breathing room for g, p, y descenders
    int editX = engineBtnX + engineBtnW + 8;
    int editW = sb.right - editX - 18;
    if (editW < 50) editW = 50;

    int editH = 26;
    int editY = sb.top + 10;

    if (g_hEdit) {
        SetWindowPos(g_hEdit, nullptr, editX, editY, editW, editH, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessage(g_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    }
    if (g_hBtnSearch) {
        ShowWindow(g_hBtnSearch, SW_HIDE);
    }

    // Dropdown for settings put at the end of the screen, vertically centered
    int settingsBtnSize = 32;
    int settingsMarginRight = 14;
    int settingsBtnX = width - settingsBtnSize - settingsMarginRight;
    int settingsBtnY = sb.top + (SEARCH_BAR_HEIGHT - settingsBtnSize) / 2;
    if (g_hBtnSettings) {
        SetWindowPos(g_hBtnSettings, nullptr, settingsBtnX, settingsBtnY, settingsBtnSize, settingsBtnSize, SWP_NOZORDER | SWP_SHOWWINDOW);
        ShowWindow(g_hBtnSettings, SW_SHOW);
    }

    // Profile Avatar Button located directly to the left of settings button
    int profileBtnX = settingsBtnX - 38;
    int profileBtnY = settingsBtnY;
    if (g_hBtnProfile) {
        SetWindowPos(g_hBtnProfile, nullptr, profileBtnX, profileBtnY, settingsBtnSize, settingsBtnSize, SWP_NOZORDER | SWP_SHOWWINDOW);
        ShowWindow(g_hBtnProfile, SW_SHOW);
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
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        return TRUE;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            wchar_t buffer[2048] = { 0 };
            GetWindowTextW(hWnd, buffer, ARRAYSIZE(buffer));
            NavigateToInput(buffer);
            return 0;
        }
        else if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Full control: Ctrl+A Select All
            SendMessage(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        else if (wParam == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Full control: Ctrl+Backspace deletes previous word without inserting unprintable 0x7F box glyph
            DWORD start = 0, end = 0;
            SendMessage(hWnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start != end) {
                SendMessage(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            }
            else if (start > 0) {
                int len = GetWindowTextLengthW(hWnd);
                std::vector<wchar_t> text(len + 1, 0);
                GetWindowTextW(hWnd, text.data(), len + 1);
                int pos = (int)start;
                // Skip trailing spaces before word
                while (pos > 0 && iswspace(text[pos - 1])) {
                    pos--;
                }
                // Skip previous word characters
                while (pos > 0 && !iswspace(text[pos - 1])) {
                    pos--;
                }
                SendMessage(hWnd, EM_SETSEL, pos, start);
                SendMessage(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            }
            return 0;
        }
        else if (wParam == VK_ESCAPE) {
            // Restore current page URL or clear on Escape
            if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) {
                SetWindowTextW(hWnd, g_tabs[g_activeTabIndex]->url.c_str());
                SendMessage(hWnd, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        // Suppress Ctrl+A (1) and Ctrl+Backspace (127 / 0x7F) so they don't insert tofu/box characters or beep
        if (wParam == 1 || wParam == 127 || wParam == 10) {
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        // If not focused, select entire text on first click
        if (GetFocus() != hWnd) {
            SetFocus(hWnd);
            SendMessage(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        return CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
    }
    case WM_SETFOCUS: {
        g_isEditFocused = true;
        LRESULT res = CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
        
        // Full-height 2px caret that aligns with text baseline and caps
        POINT pt;
        GetCaretPos(&pt);
        CreateCaret(hWnd, nullptr, 2, 21);
        SetCaretPos(pt.x, 1);
        ShowCaret(hWnd);

        if (g_hWnd) {
            RECT rc;
            GetClientRect(g_hWnd, &rc);
            RECT sb = GetSearchBarOuterRect(rc.right - rc.left);
            sb.left -= 4; sb.top -= 4; sb.right += 4; sb.bottom += 4;
            InvalidateRect(g_hWnd, &sb, FALSE);
        }
        return res;
    }
    case WM_KILLFOCUS: {
        g_isEditFocused = false;
        DestroyCaret();
        if (g_hWnd) {
            RECT rc;
            GetClientRect(g_hWnd, &rc);
            RECT sb = GetSearchBarOuterRect(rc.right - rc.left);
            sb.left -= 4; sb.top -= 4; sb.right += 4; sb.bottom += 4;
            InvalidateRect(g_hWnd, &sb, FALSE);
        }
        return CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
    }
    }
    return CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
}

// Forward declarations
void ShowSettingsDropdown();
void ShowProfileDropdown();
void SaveUserProfile();
void LoadUserProfile();

// --------------------------------------------------------------------------
// User Profile Data & Storage
// --------------------------------------------------------------------------
struct UserProfile {
    bool isLoggedIn = true;
    std::wstring name = L"Aryan";
    std::wstring email = L"aryandas.dev@gmail.com";
    std::wstring avatarFileName = L"profile_avatar.png";
    Gdiplus::Bitmap* pAvatarBitmap = nullptr;
} g_userProfile;

std::wstring GetProfileFilePath(const std::wstring& filename) {
    wchar_t localAppData[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring dir = std::wstring(localAppData) + L"\\Programs\\Haze";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\" + filename;
    }
    return filename;
}

void LoadUserProfile() {
    std::wstring avatarPath = GetProfileFilePath(g_userProfile.avatarFileName);
    if (GetFileAttributesW(avatarPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (g_userProfile.pAvatarBitmap) {
            delete g_userProfile.pAvatarBitmap;
        }
        g_userProfile.pAvatarBitmap = Gdiplus::Bitmap::FromFile(avatarPath.c_str());
    }
    else {
        // Fallback to local executable folder
        if (GetFileAttributesW(L"profile_avatar.png") != INVALID_FILE_ATTRIBUTES) {
            g_userProfile.pAvatarBitmap = Gdiplus::Bitmap::FromFile(L"profile_avatar.png");
        }
    }

    std::wstring jsonPath = GetProfileFilePath(L"user_profile.json");
    std::wifstream f(jsonPath);
    if (f.is_open()) {
        std::wstringstream buffer;
        buffer << f.rdbuf();
        std::wstring content = buffer.str();

        if (content.find(L"\"isLoggedIn\": false") != std::wstring::npos) {
            g_userProfile.isLoggedIn = false;
        } else if (content.find(L"\"isLoggedIn\": true") != std::wstring::npos) {
            g_userProfile.isLoggedIn = true;
        }

        size_t nPos = content.find(L"\"name\": \"");
        if (nPos != std::wstring::npos) {
            size_t nEnd = content.find(L"\"", nPos + 9);
            if (nEnd != std::wstring::npos) {
                g_userProfile.name = content.substr(nPos + 9, nEnd - (nPos + 9));
            }
        }

        size_t ePos = content.find(L"\"email\": \"");
        if (ePos != std::wstring::npos) {
            size_t eEnd = content.find(L"\"", ePos + 10);
            if (eEnd != std::wstring::npos) {
                g_userProfile.email = content.substr(ePos + 10, eEnd - (ePos + 10));
            }
        }
    }
}

void SaveUserProfile() {
    std::wstring jsonPath = GetProfileFilePath(L"user_profile.json");
    std::wofstream f(jsonPath);
    if (f.is_open()) {
        f << L"{\n";
        f << L"  \"isLoggedIn\": " << (g_userProfile.isLoggedIn ? L"true" : L"false") << L",\n";
        f << L"  \"name\": \"" << g_userProfile.name << L"\",\n";
        f << L"  \"email\": \"" << g_userProfile.email << L"\",\n";
        f << L"  \"avatarPath\": \"" << g_userProfile.avatarFileName << L"\"\n";
        f << L"}\n";
    }
}

// --------------------------------------------------------------------------
// Profile Avatar Button located next to Settings Chevron
// --------------------------------------------------------------------------
LRESULT CALLBACK ProfileButtonProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = (uMsg == WM_PRINTCLIENT) ? (HDC)wParam : BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        g.Clear(bgCol);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        REAL cx = (rc.left + rc.right) / 2.0f;
        REAL cy = (rc.top + rc.bottom) / 2.0f;
        REAL sz = 28.0f;
        REAL r = sz / 2.0f;

        // Hover circle
        if (g_isProfileBtnHovered) {
            SolidBrush hoverBrush(g_isDarkMode ? Color(255, 52, 52, 62) : Color(255, 238, 232, 210));
            g.FillEllipse(&hoverBrush, cx - r - 2.0f, cy - r - 2.0f, sz + 4.0f, sz + 4.0f);
        }

        if (g_userProfile.isLoggedIn && g_userProfile.pAvatarBitmap && g_userProfile.pAvatarBitmap->GetLastStatus() == Ok) {
            // Neon cyan/blue ring (as in user screenshot 2)
            Pen ringPen(Color(255, 56, 138, 246), 1.8f);
            g.DrawEllipse(&ringPen, cx - r, cy - r, sz, sz);

            GraphicsPath circleClip;
            circleClip.AddEllipse(cx - r + 1.5f, cy - r + 1.5f, sz - 3.0f, sz - 3.0f);
            Region oldClip;
            g.GetClip(&oldClip);
            g.SetClip(&circleClip, CombineModeReplace);

            g.DrawImage(g_userProfile.pAvatarBitmap, cx - r + 1.5f, cy - r + 1.5f, sz - 3.0f, sz - 3.0f);
            g.SetClip(&oldClip, CombineModeReplace);
        }
        else {
            // Modern silhouette user avatar icon (logged out)
            SolidBrush circleBg(g_isDarkMode ? Color(255, 55, 55, 65) : Color(255, 226, 222, 204));
            g.FillEllipse(&circleBg, cx - r, cy - r, sz, sz);

            Color iconCol = g_isDarkMode ? Color(255, 200, 200, 210) : Color(255, 100, 96, 88);
            SolidBrush iconBrush(iconCol);

            // Head circle
            REAL headR = 4.2f;
            REAL headCy = cy - 2.5f;
            g.FillEllipse(&iconBrush, cx - headR, headCy - headR, headR * 2.0f, headR * 2.0f);

            // Shoulders arc clipped inside the circle
            GraphicsPath clipP;
            clipP.AddEllipse(cx - r, cy - r, sz, sz);
            Region origClip;
            g.GetClip(&origClip);
            g.SetClip(&clipP, CombineModeReplace);

            REAL bodyW = 15.0f;
            REAL bodyH = 12.0f;
            g.FillEllipse(&iconBrush, cx - bodyW / 2.0f, cy + 3.0f, bodyW, bodyH);
            g.SetClip(&origClip, CombineModeReplace);
        }

        if (uMsg == WM_PAINT) EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isProfileBtnHovered) {
            g_isProfileBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_isProfileBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        ShowProfileDropdown();
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Settings Chevron Dropdown Button located outside the address bar
LRESULT CALLBACK SettingsButtonProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = (uMsg == WM_PRINTCLIENT) ? (HDC)wParam : BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);

        // Fill background with toolbar color
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        g.Clear(bgCol);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

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

        if (uMsg == WM_PAINT) EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isSettingsBtnHovered) {
            g_isSettingsBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_isSettingsBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        ShowSettingsDropdown();
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Crisp arrow search button
LRESULT CALLBACK SearchButtonProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);

        // Fill background with omnibox pill warm beige
        Color bgCol = Color(255, 254, 249, 235);
        g.Clear(bgCol);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

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
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_isSearchBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        if (PtInRect(&rc, pt)) {
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDC_BTN_SEARCH, 0), (LPARAM)hWnd);
        }
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void ShowEngineDropdown();

// Search Engine Selector Button inside Omnibox
LRESULT CALLBACK EngineButtonProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = (uMsg == WM_PRINTCLIENT) ? (HDC)wParam : BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);

        // Fill background with omnibox pill cream / dark grey
        Color bgCol = g_isDarkMode ? Color(255, 40, 40, 50) : Color(255, 254, 249, 235);
        g.Clear(bgCol);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        if (g_isEngineBtnHovered) {
            GraphicsPath btnPath;
            AddRoundedRect(btnPath, (REAL)rc.left + 1.0f, (REAL)rc.top + 1.0f, (REAL)(rc.right - rc.left - 2), (REAL)(rc.bottom - rc.top - 2), 6.0f);
            SolidBrush hoverBrush(g_isDarkMode ? Color(255, 60, 60, 75) : Color(255, 238, 232, 210));
            g.FillPath(&hoverBrush, &btnPath);
        }

        // Draw Selected Engine Icon (scaled to fill icon area cleanly without internal padding)
        if (g_selectedEngineIndex >= 0 && g_selectedEngineIndex < (int)g_searchEngines.size()) {
            auto pBmp = g_searchEngines[g_selectedEngineIndex].pBitmap;
            if (pBmp) {
                REAL glyphSrcX = (REAL)pBmp->GetWidth() * 0.16f;
                REAL glyphSrcY = (REAL)pBmp->GetHeight() * 0.16f;
                REAL glyphSrcW = (REAL)pBmp->GetWidth() * 0.68f;
                REAL glyphSrcH = (REAL)pBmp->GetHeight() * 0.68f;

                REAL iconSize = 22.0f;
                REAL iconX = 6.0f;
                REAL iconY = ((rc.bottom - rc.top) - iconSize) / 2.0f;
                g.DrawImage(pBmp, RectF(iconX, iconY, iconSize, iconSize), glyphSrcX, glyphSrcY, glyphSrcW, glyphSrcH, UnitPixel);
            }
        }

        // Draw clear dropdown caret to select the search engine
        Color caretCol = g_isDarkMode ? Color(255, 185, 185, 195) : Color(255, 118, 112, 27);
        Pen caretPen(caretCol, 1.8f);
        caretPen.SetStartCap(LineCapRound);
        caretPen.SetEndCap(LineCapRound);
        caretPen.SetLineJoin(LineJoinRound);

        REAL caretX = (REAL)rc.right - 10.0f;
        REAL caretY = (rc.top + rc.bottom) / 2.0f;
        PointF caretPts[] = {
            PointF(caretX - 3.5f, caretY - 1.8f),
            PointF(caretX, caretY + 2.0f),
            PointF(caretX + 3.5f, caretY - 1.8f)
        };
        g.DrawLines(&caretPen, caretPts, 3);

        if (uMsg == WM_PAINT) EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_isEngineBtnHovered) {
            g_isEngineBtnHovered = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_isEngineBtnHovered = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        ShowEngineDropdown();
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Crisp smooth vector navigation buttons (←, →, ↻) using GDI+
LRESULT CALLBACK NavButtonProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HWND s_hoveredBtn = nullptr;

    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (s_hoveredBtn != hWnd) {
            s_hoveredBtn = hWnd;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (s_hoveredBtn == hWnd) {
            s_hoveredBtn = nullptr;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        if (PtInRect(&rc, pt)) {
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
            if (enabled) {
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(id, 0), (LPARAM)hWnd);
            }
        }
        return 0;
    }
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = (uMsg == WM_PRINTCLIENT) ? (HDC)wParam : BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);

        // Fill background with toolbar cream color / dark grey
        Color bgCol = g_isDarkMode ? Color(255, 34, 34, 40) : Color(255, 254, 249, 235);
        g.Clear(bgCol);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

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
            // Modern reload icon matching uploaded reference (media 2):
            // Clockwise open circular arc with south-pointing arrowhead
            REAL radius = 6.5f;
            RectF arcRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
            // Arc sweeps clockwise from 95 degrees around to 355 degrees (260 deg sweep)
            g.DrawArc(&navPen, arcRect, 95.0f, 260.0f);

            // Arrowhead at the right edge pointing downward / inward
            REAL tipX = cx + radius - 0.5f;
            REAL tipY = cy + 2.5f;
            REAL w = 4.0f;

            // Wing 1: Up-left (connects smoothly into the circular arc trajectory)
            g.DrawLine(&navPen, tipX, tipY, tipX - w * 0.75f, tipY - w * 0.70f);
            // Wing 2: Up-right (outer arrowhead barb)
            g.DrawLine(&navPen, tipX, tipY, tipX + w * 0.70f, tipY - w * 0.75f);
        }

        if (uMsg == WM_PAINT) EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
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
    return RECT{ x, TAB_TOP, x + TAB_WIDTH, TAB_TOP + TAB_HEIGHT };
}

RECT GetNewTabBtnRect(int clientWidth) {
    int x = 12 + (int)g_tabs.size() * (TAB_WIDTH + 8) + 16;
    constexpr int btnSize = 28;
    constexpr int btnY = (TAB_BAR_HEIGHT - btnSize) / 2; // 13px: vertically centered
    return RECT{ x, btnY, x + btnSize, btnY + btnSize };
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
                                                    // Broadcast engine selection to all open tabs
                                                    std::wstring engMsg = L"{\"type\":\"sync_state\",\"engine\":" + std::to_wstring(g_selectedEngineIndex) + L"}";
                                                    for (auto& t : g_tabs) {
                                                        if (t->webview) {
                                                            t->webview->PostWebMessageAsJson(engMsg.c_str());
                                                        }
                                                    }
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

                                            // Auto-detect Google sign-in / account access
                                            if (uri && (wcsstr(uri, L"accounts.google.com") != nullptr || 
                                                        wcsstr(uri, L"myaccount.google.com") != nullptr ||
                                                        wcsstr(uri, L"google.com") != nullptr)) {
                                                if (!g_userProfile.isLoggedIn) {
                                                    g_userProfile.isLoggedIn = true;
                                                    SaveUserProfile();
                                                    if (g_hBtnProfile) InvalidateRect(g_hBtnProfile, nullptr, TRUE);
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

                            // Listen for Favicon changes via ICoreWebView2_15
                            BrowserTab* pTabRaw = tab.get();
                            wil::com_ptr<ICoreWebView2_15> webview15;
                            if (SUCCEEDED(tab->webview->QueryInterface(IID_PPV_ARGS(&webview15)))) {
                                webview15->add_FaviconChanged(
                                    Callback<ICoreWebView2FaviconChangedEventHandler>(
                                        [hWnd, pTabRaw](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                            wil::com_ptr<ICoreWebView2_15> sender15;
                                            if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&sender15)))) {
                                                sender15->GetFavicon(
                                                    COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                                                    Callback<ICoreWebView2GetFaviconCompletedHandler>(
                                                        [hWnd, pTabRaw](HRESULT errorCode, IStream* faviconStream) -> HRESULT {
                                                            if (SUCCEEDED(errorCode) && faviconStream) {
                                                                // Verify pTabRaw is still a valid active tab in g_tabs
                                                                bool valid = false;
                                                                for (const auto& t : g_tabs) {
                                                                    if (t.get() == pTabRaw) {
                                                                        valid = true;
                                                                        break;
                                                                    }
                                                                }
                                                                if (valid) {
                                                                    Gdiplus::Bitmap* pBmp = Gdiplus::Bitmap::FromStream(faviconStream);
                                                                    if (pBmp && pBmp->GetLastStatus() == Gdiplus::Ok) {
                                                                        if (pTabRaw->pFaviconBitmap) {
                                                                            delete pTabRaw->pFaviconBitmap;
                                                                        }
                                                                        pTabRaw->pFaviconBitmap = pBmp;
                                                                        RECT topRc{ 0, 0, 2000, TAB_BAR_HEIGHT };
                                                                        InvalidateRect(hWnd, &topRc, FALSE);
                                                                    } else if (pBmp) {
                                                                        delete pBmp;
                                                                    }
                                                                }
                                                            }
                                                            return S_OK;
                                                        }).Get());
                                            }
                                            return S_OK;
                                        }).Get(), &tab->faviconToken);
                            }

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
        if (g_tabs[0]->pFaviconBitmap) {
            delete g_tabs[0]->pFaviconBitmap;
            g_tabs[0]->pFaviconBitmap = nullptr;
        }
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
// System Tray WinUI 3 Context Menu Implementation (media 1)
// --------------------------------------------------------------------------
constexpr int TRAY_MENU_WIDTH = 224;

enum TrayActionId {
    TRAY_ACT_NONE = 0,
    TRAY_ACT_UPDATE,
    TRAY_ACT_QUICK_ACCESS,
    TRAY_ACT_SETTINGS,
    TRAY_ACT_DOCS,
    TRAY_ACT_REPORT_BUG,
    TRAY_ACT_CLOSE
};

struct TrayMenuItem {
    std::wstring label;
    std::wstring hint;
    bool isSeparator;
    TrayActionId action;
    int height;
};

std::vector<TrayMenuItem> g_trayItems;
int g_trayHoveredIndex = -1;

void SetupTrayMenuItems() {
    g_trayItems = {
        { L"Update available", L"", false, TRAY_ACT_UPDATE, 36 },
        { L"", L"", true, TRAY_ACT_NONE, 8 },
        { L"Quick access", L"Left-click", false, TRAY_ACT_QUICK_ACCESS, 32 },
        { L"Settings", L"Double-click", false, TRAY_ACT_SETTINGS, 32 },
        { L"", L"", true, TRAY_ACT_NONE, 8 },
        { L"Documentation", L"", false, TRAY_ACT_DOCS, 32 },
        { L"Report bug", L"", false, TRAY_ACT_REPORT_BUG, 32 },
        { L"", L"", true, TRAY_ACT_NONE, 8 },
        { L"Close", L"", false, TRAY_ACT_CLOSE, 34 }
    };
}

int CalculateTrayMenuHeight() {
    int total = 10;
    for (const auto& item : g_trayItems) {
        total += item.height;
    }
    return total;
}

RECT GetTrayItemRect(int index) {
    if (index < 0 || index >= (int)g_trayItems.size()) return RECT{ 0, 0, 0, 0 };
    int y = 5;
    for (int i = 0; i < index; ++i) {
        y += g_trayItems[i].height;
    }
    return RECT{ 4, y, TRAY_MENU_WIDTH - 4, y + g_trayItems[index].height };
}

int GetTrayItemFromPoint(POINT pt) {
    for (int i = 0; i < (int)g_trayItems.size(); ++i) {
        if (g_trayItems[i].isSeparator) continue;
        RECT rc = GetTrayItemRect(i);
        if (PtInRect(&rc, pt)) return i;
    }
    return -1;
}

void ExecuteTrayAction(TrayActionId action) {
    switch (action) {
    case TRAY_ACT_UPDATE:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"https://github.com");
        }
        break;
    case TRAY_ACT_QUICK_ACCESS:
        RestoreMainWindow(false);
        break;
    case TRAY_ACT_SETTINGS:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            if (!g_settingsPageUrl.empty()) {
                g_tabs[g_activeTabIndex]->webview->Navigate(g_settingsPageUrl.c_str());
            } else {
                g_tabs[g_activeTabIndex]->webview->Navigate(L"haze://settings");
            }
        }
        break;
    case TRAY_ACT_DOCS:
        RestoreMainWindow(false);
        CreateNewTab(L"https://github.com");
        break;
    case TRAY_ACT_REPORT_BUG:
        RestoreMainWindow(false);
        CreateNewTab(L"https://github.com/issues");
        break;
    case TRAY_ACT_CLOSE:
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK TrayMenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // Background: Clean white matching media 1
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0, 0, rcClient.right, rcClient.bottom);

        Pen borderPen(Color(255, 222, 222, 222), 1.0f);
        g.DrawRectangle(&borderPen, 0, 0, rcClient.right - 1, rcClient.bottom - 1);

        FontFamily* pFontFamily = nullptr;
        FontFamily testFam(L"Segoe UI");
        if (testFam.GetLastStatus() == Ok) {
            pFontFamily = new FontFamily(L"Segoe UI");
        } else {
            pFontFamily = FontFamily::GenericSansSerif()->Clone();
        }

        Gdiplus::Font fontLabel(pFontFamily, 9.8f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontHint(pFontFamily, 9.2f, FontStyleRegular, UnitPoint);

        SolidBrush textBrush(Color(255, 26, 26, 26));
        SolidBrush hintBrush(Color(255, 105, 105, 105));
        SolidBrush hoverBrush(Color(255, 242, 242, 242));
        Pen sepPen(Color(255, 234, 234, 234), 1.0f);

        StringFormat strLeft;
        strLeft.SetAlignment(StringAlignmentNear);
        strLeft.SetLineAlignment(StringAlignmentCenter);

        StringFormat strRight;
        strRight.SetAlignment(StringAlignmentFar);
        strRight.SetLineAlignment(StringAlignmentCenter);

        for (int i = 0; i < (int)g_trayItems.size(); ++i) {
            const auto& item = g_trayItems[i];
            RECT rc = GetTrayItemRect(i);
            REAL rx = (REAL)rc.left;
            REAL ry = (REAL)rc.top;
            REAL rw = (REAL)(rc.right - rc.left);
            REAL rh = (REAL)(rc.bottom - rc.top);

            if (item.isSeparator) {
                REAL midY = ry + (rh / 2.0f);
                g.DrawLine(&sepPen, rx + 8.0f, midY, rx + rw - 8.0f, midY);
                continue;
            }

            bool isHover = (i == g_trayHoveredIndex);
            if (isHover) {
                GraphicsPath hoverPath;
                REAL hx = rx + 3.0f;
                REAL hy = ry + 1.0f;
                REAL hw = rw - 6.0f;
                REAL hh = rh - 2.0f;
                REAL cr = 4.0f;
                hoverPath.AddArc(hx, hy, cr * 2.0f, cr * 2.0f, 180.0f, 90.0f);
                hoverPath.AddArc(hx + hw - cr * 2.0f, hy, cr * 2.0f, cr * 2.0f, 270.0f, 90.0f);
                hoverPath.AddArc(hx + hw - cr * 2.0f, hy + hh - cr * 2.0f, cr * 2.0f, cr * 2.0f, 0.0f, 90.0f);
                hoverPath.AddArc(hx, hy + hh - cr * 2.0f, cr * 2.0f, cr * 2.0f, 90.0f, 90.0f);
                hoverPath.CloseFigure();
                g.FillPath(&hoverBrush, &hoverPath);
            }

            RectF textRc(rx + 14.0f, ry, rw - 28.0f, rh);
            g.DrawString(item.label.c_str(), -1, &fontLabel, textRc, &strLeft, &textBrush);

            if (!item.hint.empty()) {
                RectF hintRc(rx + 14.0f, ry, rw - 28.0f, rh);
                g.DrawString(item.hint.c_str(), -1, &fontHint, hintRc, &strRight, &hintBrush);
            }
        }

        delete pFontFamily;
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int newHover = GetTrayItemFromPoint(pt);
        if (newHover != g_trayHoveredIndex) {
            g_trayHoveredIndex = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_trayHoveredIndex != -1) {
            g_trayHoveredIndex = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int clickedIdx = GetTrayItemFromPoint(pt);
        ShowWindow(hWnd, SW_HIDE);
        if (clickedIdx >= 0 && clickedIdx < (int)g_trayItems.size()) {
            ExecuteTrayAction(g_trayItems[clickedIdx].action);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    }
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowWinUI3Menu() {
    if (!g_hTrayMenuWnd) return;
    POINT cursor;
    GetCursorPos(&cursor);

    int menuW = TRAY_MENU_WIDTH;
    int menuH = CalculateTrayMenuHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int posX = cursor.x - (menuW / 2);
    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = cursor.y - menuH - 8;
    if (posY < workArea.top) posY = cursor.y + 8;

    g_trayHoveredIndex = -1;

    SetWindowPos(g_hTrayMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hTrayMenuWnd);
    SetFocus(g_hTrayMenuWnd);
    InvalidateRect(g_hTrayMenuWnd, nullptr, TRUE);
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
 
         FontFamily* pFontFamily = new FontFamily(L"Inter");
         if (pFontFamily->GetLastStatus() != Ok) {
             delete pFontFamily;
             pFontFamily = new FontFamily(L"Segoe UI");
         }
         Gdiplus::Font headerFont(pFontFamily, 10.0f, FontStyleRegular, UnitPoint);
         Gdiplus::Font itemFont(pFontFamily, 11.0f, FontStyleRegular, UnitPoint);
         Gdiplus::Font itemSelectedFont(pFontFamily, 11.0f, FontStyleBold, UnitPoint);
 
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

// --------------------------------------------------------------------------
// WinUI 3 Google Profile Dropdown Flyout Implementation (media 1 & 2)
// --------------------------------------------------------------------------
constexpr int PROFILE_MENU_WIDTH = 340;

enum ProfileMenuAction {
    PMA_NONE = 0,
    PMA_SIGN_IN_GOOGLE,
    PMA_PASSWORDS,
    PMA_MANAGE_ACCOUNT,
    PMA_CUSTOMIZE,
    PMA_SIGN_OUT,
    PMA_BROWSE_GUEST,
    PMA_ADD_PROFILE
};

struct ProfileMenuItem {
    std::wstring label;
    std::wstring subtext;
    ProfileMenuAction action;
    bool isSeparator;
    bool hasChevron;
    int height;
};

std::vector<ProfileMenuItem> g_profileItems;
int g_profileHoveredIndex = -1;
bool g_profileBtnContinueHovered = false;

void SetupProfileMenuItems() {
    g_profileItems.clear();
    if (g_userProfile.isLoggedIn) {
        g_profileItems.push_back({ L"Passwords and autofill", L"", PMA_PASSWORDS, false, false, 38 });
        g_profileItems.push_back({ L"Manage your Google Account", L"", PMA_MANAGE_ACCOUNT, false, false, 38 });
        g_profileItems.push_back({ L"Customize profile", L"", PMA_CUSTOMIZE, false, false, 38 });
        g_profileItems.push_back({ L"Sign out of Google", L"", PMA_SIGN_OUT, false, false, 38 });
        g_profileItems.push_back({ L"", L"", PMA_NONE, true, false, 9 });
        g_profileItems.push_back({ L"Browse as guest", L"", PMA_BROWSE_GUEST, false, false, 38 });
        g_profileItems.push_back({ L"Add profile", L"", PMA_ADD_PROFILE, false, true, 38 });
    }
    else {
        g_profileItems.push_back({ L"", L"", PMA_NONE, true, false, 9 });
        g_profileItems.push_back({ L"Browse as guest", L"", PMA_BROWSE_GUEST, false, false, 38 });
        g_profileItems.push_back({ L"Profile settings", L"", PMA_CUSTOMIZE, false, false, 38 });
    }
}

int GetProfileHeaderHeight() {
    return g_userProfile.isLoggedIn ? 150 : 160;
}

int CalculateProfileMenuHeight() {
    int total = GetProfileHeaderHeight() + 10;
    for (const auto& item : g_profileItems) {
        total += item.height;
    }
    return total;
}

int GetProfileItemFromPoint(POINT pt) {
    int curY = GetProfileHeaderHeight() + 6;
    for (size_t i = 0; i < g_profileItems.size(); ++i) {
        if (g_profileItems[i].isSeparator) {
            curY += g_profileItems[i].height;
            continue;
        }
        if (pt.y >= curY && pt.y < curY + g_profileItems[i].height) {
            return (int)i;
        }
        curY += g_profileItems[i].height;
    }
    return -1;
}

LRESULT CALLBACK ProfileMenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // WinUI 3 Clean Card Background (White / Dark #1F1F24)
        Color bgCol = g_isDarkMode ? Color(255, 31, 31, 36) : Color(255, 255, 255, 255);
        SolidBrush bgBrush(bgCol);
        g.FillRectangle(&bgBrush, 0, 0, w, h);

        // Subtle 1px boundary border
        Color borderCol = g_isDarkMode ? Color(255, 55, 55, 65) : Color(255, 226, 228, 233);
        Pen borderPen(borderCol, 1.0f);
        g.DrawRectangle(&borderPen, 0, 0, w - 1, h - 1);

        FontFamily* pFontFamily = new FontFamily(GetModernFontFamilyName());
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI");
        }

        // ================= HEADER SECTION =================
        int headerH = GetProfileHeaderHeight();
        REAL centerCX = (REAL)w / 2.0f;

        if (g_userProfile.isLoggedIn) {
            // Large circular avatar (64x64) with cyan neon glowing ring
            REAL avatarSize = 62.0f;
            REAL avatarR = avatarSize / 2.0f;
            REAL avatarCY = 44.0f;

            // Draw glowing cyan ring (matching user screenshot 2)
            Pen ringPen(Color(255, 56, 138, 246), 2.2f);
            g.DrawEllipse(&ringPen, centerCX - avatarR, avatarCY - avatarR, avatarSize, avatarSize);

            if (g_userProfile.pAvatarBitmap && g_userProfile.pAvatarBitmap->GetLastStatus() == Ok) {
                GraphicsPath clipP;
                clipP.AddEllipse(centerCX - avatarR + 2.0f, avatarCY - avatarR + 2.0f, avatarSize - 4.0f, avatarSize - 4.0f);
                Region oldReg;
                g.GetClip(&oldReg);
                g.SetClip(&clipP, CombineModeReplace);
                g.DrawImage(g_userProfile.pAvatarBitmap, centerCX - avatarR + 2.0f, avatarCY - avatarR + 2.0f, avatarSize - 4.0f, avatarSize - 4.0f);
                g.SetClip(&oldReg, CombineModeReplace);
            }

            // Pro badge pill in bottom right of avatar
            REAL badgeW = 28.0f;
            REAL badgeH = 14.0f;
            REAL badgeX = centerCX + avatarR - 18.0f;
            REAL badgeY = avatarCY + avatarR - 12.0f;
            GraphicsPath badgePath;
            AddRoundedRect(badgePath, badgeX, badgeY, badgeW, badgeH, 6.0f);
            SolidBrush badgeBg(Color(255, 15, 56, 102)); // Deep navy blue pill
            g.FillPath(&badgeBg, &badgePath);
            Gdiplus::Font badgeFont(pFontFamily, 7.0f, FontStyleBold, UnitPoint);
            SolidBrush badgeText(Color(255, 255, 255, 255));
            StringFormat badgeFmt;
            badgeFmt.SetAlignment(StringAlignmentCenter);
            badgeFmt.SetLineAlignment(StringAlignmentCenter);
            RectF badgeRc(badgeX, badgeY, badgeW, badgeH);
            g.DrawString(L"Pro", -1, &badgeFont, badgeRc, &badgeFmt, &badgeText);

            // User Name: e.g. "Aryan"
            Gdiplus::Font nameFont(pFontFamily, 13.0f, FontStyleBold, UnitPoint);
            Color nameCol = g_isDarkMode ? Color(255, 245, 245, 250) : Color(255, 25, 25, 30);
            SolidBrush nameBrush(nameCol);
            StringFormat centerFmt;
            centerFmt.SetAlignment(StringAlignmentCenter);
            centerFmt.SetLineAlignment(StringAlignmentCenter);
            RectF nameRc(16.0f, 82.0f, (REAL)w - 32.0f, 24.0f);
            g.DrawString(g_userProfile.name.c_str(), -1, &nameFont, nameRc, &centerFmt, &nameBrush);

            // User Email: e.g. "aryandas.dev@gmail.com"
            Gdiplus::Font emailFont(pFontFamily, 9.5f, FontStyleRegular, UnitPoint);
            Color emailCol = g_isDarkMode ? Color(255, 160, 160, 175) : Color(255, 95, 99, 104);
            SolidBrush emailBrush(emailCol);
            RectF emailRc(16.0f, 107.0f, (REAL)w - 32.0f, 18.0f);
            g.DrawString(g_userProfile.email.c_str(), -1, &emailFont, emailRc, &centerFmt, &emailBrush);

            // Sync status subtitle (as in reference image 1)
            Gdiplus::Font syncFont(pFontFamily, 8.5f, FontStyleRegular, UnitPoint);
            Color syncCol = Color(255, 24, 128, 56); // Green sync check
            SolidBrush syncBrush(syncCol);
            RectF syncRc(16.0f, 126.0f, (REAL)w - 32.0f, 16.0f);
            g.DrawString(L"✓ Synced with Google Account", -1, &syncFont, syncRc, &centerFmt, &syncBrush);
        }
        else {
            // Logged out header: Silhouette avatar + "Sign in to HAZE" + "Continue with Google" primary button
            REAL avatarSize = 54.0f;
            REAL avatarR = avatarSize / 2.0f;
            REAL avatarCY = 36.0f;

            SolidBrush circleBg(g_isDarkMode ? Color(255, 55, 55, 65) : Color(255, 235, 232, 222));
            g.FillEllipse(&circleBg, centerCX - avatarR, avatarCY - avatarR, avatarSize, avatarSize);

            Color iconCol = g_isDarkMode ? Color(255, 190, 190, 200) : Color(255, 110, 105, 95);
            SolidBrush iconBrush(iconCol);
            REAL headR = 7.5f;
            REAL headCy = avatarCY - 4.5f;
            g.FillEllipse(&iconBrush, centerCX - headR, headCy - headR, headR * 2.0f, headR * 2.0f);

            GraphicsPath clipP;
            clipP.AddEllipse(centerCX - avatarR, avatarCY - avatarR, avatarSize, avatarSize);
            Region origClip;
            g.GetClip(&origClip);
            g.SetClip(&clipP, CombineModeReplace);
            REAL bodyW = 28.0f;
            REAL bodyH = 22.0f;
            g.FillEllipse(&iconBrush, centerCX - bodyW / 2.0f, avatarCY + 5.0f, bodyW, bodyH);
            g.SetClip(&origClip, CombineModeReplace);

            // Heading: "Sign in to HAZE"
            Gdiplus::Font signinFont(pFontFamily, 11.5f, FontStyleBold, UnitPoint);
            Color signinCol = g_isDarkMode ? Color(255, 240, 240, 245) : Color(255, 32, 33, 36);
            SolidBrush signinBrush(signinCol);
            StringFormat centerFmt;
            centerFmt.SetAlignment(StringAlignmentCenter);
            centerFmt.SetLineAlignment(StringAlignmentCenter);
            RectF signinRc(16.0f, 68.0f, (REAL)w - 32.0f, 20.0f);
            g.DrawString(L"Sign in to HAZE", -1, &signinFont, signinRc, &centerFmt, &signinBrush);

            // Primary Google Action Button: "Continue with Google"
            REAL btnW = (REAL)w - 48.0f;
            REAL btnH = 36.0f;
            REAL btnX = 24.0f;
            REAL btnY = 98.0f;
            GraphicsPath btnPath;
            AddRoundedRect(btnPath, btnX, btnY, btnW, btnH, 8.0f);

            Color btnBg = g_profileBtnContinueHovered ? 
                (g_isDarkMode ? Color(255, 30, 95, 185) : Color(255, 26, 115, 232)) : 
                (g_isDarkMode ? Color(255, 26, 85, 165) : Color(255, 10, 102, 214));
            SolidBrush btnBgBrush(btnBg);
            g.FillPath(&btnBgBrush, &btnPath);

            // Google 'G' icon / text
            Gdiplus::Font btnFont(pFontFamily, 9.5f, FontStyleBold, UnitPoint);
            SolidBrush btnText(Color(255, 255, 255, 255));
            RectF btnTextRc(btnX, btnY, btnW, btnH);
            g.DrawString(L"G  Continue with Google", -1, &btnFont, btnTextRc, &centerFmt, &btnText);
        }

        // Horizontal divider under header
        REAL divY = (REAL)headerH + 2.0f;
        Color divCol = g_isDarkMode ? Color(255, 48, 48, 56) : Color(255, 230, 233, 238);
        Pen divPen(divCol, 1.0f);
        g.DrawLine(&divPen, 16.0f, divY, (REAL)w - 16.0f, divY);

        // ================= MENU ITEMS LIST =================
        Gdiplus::Font itemFont(pFontFamily, 9.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font chevronFont(pFontFamily, 9.0f, FontStyleRegular, UnitPoint);

        int curY = headerH + 6;
        for (size_t i = 0; i < g_profileItems.size(); ++i) {
            const auto& item = g_profileItems[i];
            if (item.isSeparator) {
                int lineY = curY + item.height / 2;
                g.DrawLine(&divPen, 16.0f, (REAL)lineY, (REAL)w - 16.0f, (REAL)lineY);
                curY += item.height;
                continue;
            }

            bool isHover = ((int)i == g_profileHoveredIndex);
            if (isHover) {
                GraphicsPath itemPath;
                AddRoundedRect(itemPath, 8.0f, (REAL)curY, (REAL)w - 16.0f, (REAL)item.height, 6.0f);
                Color hoverCol = g_isDarkMode ? Color(255, 46, 46, 54) : Color(255, 243, 244, 246);
                SolidBrush hoverBrush(hoverCol);
                g.FillPath(&hoverBrush, &itemPath);
            }

            // Draw Icon
            REAL iconCX = 28.0f;
            REAL iconCY = (REAL)curY + (REAL)item.height / 2.0f;
            Color itemIconCol = g_isDarkMode ? Color(255, 185, 185, 195) : Color(255, 95, 99, 104);
            Pen iconPen(itemIconCol, 1.4f);
            iconPen.SetStartCap(LineCapRound);
            iconPen.SetEndCap(LineCapRound);
            SolidBrush iconBrush(itemIconCol);

            switch (item.action) {
            case PMA_PASSWORDS: {
                // Key icon
                g.DrawEllipse(&iconPen, iconCX - 6.0f, iconCY - 3.5f, 7.0f, 7.0f);
                g.DrawLine(&iconPen, iconCX + 1.0f, iconCY, iconCX + 7.0f, iconCY);
                g.DrawLine(&iconPen, iconCX + 5.0f, iconCY, iconCX + 5.0f, iconCY + 3.0f);
                break;
            }
            case PMA_MANAGE_ACCOUNT: {
                // Google circular shield / badge icon
                g.DrawEllipse(&iconPen, iconCX - 5.5f, iconCY - 5.5f, 11.0f, 11.0f);
                Gdiplus::Font gFont(pFontFamily, 7.0f, FontStyleBold, UnitPoint);
                StringFormat gFmt;
                gFmt.SetAlignment(StringAlignmentCenter);
                gFmt.SetLineAlignment(StringAlignmentCenter);
                RectF gRc(iconCX - 6.0f, iconCY - 6.0f, 12.0f, 12.0f);
                g.DrawString(L"G", -1, &gFont, gRc, &gFmt, &iconBrush);
                break;
            }
            case PMA_CUSTOMIZE: {
                // Pencil icon
                g.DrawLine(&iconPen, iconCX - 5.0f, iconCY + 5.0f, iconCX + 4.0f, iconCY - 4.0f);
                g.DrawLine(&iconPen, iconCX + 4.0f, iconCY - 4.0f, iconCX + 6.0f, iconCY - 2.0f);
                g.DrawLine(&iconPen, iconCX + 6.0f, iconCY - 2.0f, iconCX - 3.0f, iconCY + 7.0f);
                break;
            }
            case PMA_SIGN_OUT: {
                // Sign out / exit icon
                g.DrawRectangle(&iconPen, iconCX - 6.0f, iconCY - 5.5f, 7.0f, 11.0f);
                g.DrawLine(&iconPen, iconCX - 2.0f, iconCY, iconCX + 6.0f, iconCY);
                g.DrawLine(&iconPen, iconCX + 3.0f, iconCY - 3.0f, iconCX + 6.0f, iconCY);
                g.DrawLine(&iconPen, iconCX + 3.0f, iconCY + 3.0f, iconCX + 6.0f, iconCY);
                break;
            }
            case PMA_BROWSE_GUEST: {
                // Guest silhouette mask / glasses icon
                g.DrawEllipse(&iconPen, iconCX - 6.0f, iconCY - 2.5f, 5.0f, 5.0f);
                g.DrawEllipse(&iconPen, iconCX + 1.0f, iconCY - 2.5f, 5.0f, 5.0f);
                g.DrawLine(&iconPen, iconCX - 1.0f, iconCY, iconCX + 1.0f, iconCY);
                break;
            }
            case PMA_ADD_PROFILE: {
                // Plus icon
                g.DrawLine(&iconPen, iconCX - 5.0f, iconCY, iconCX + 5.0f, iconCY);
                g.DrawLine(&iconPen, iconCX, iconCY - 5.0f, iconCX, iconCY + 5.0f);
                break;
            }
            default:
                break;
            }

            // Draw Label
            Color textCol = g_isDarkMode ? Color(255, 235, 235, 240) : Color(255, 32, 33, 36);
            if (item.action == PMA_SIGN_OUT) {
                textCol = g_isDarkMode ? Color(255, 245, 110, 110) : Color(255, 217, 48, 37); // Subtle red for sign out
            }
            SolidBrush textBrush(textCol);
            StringFormat strFmt;
            strFmt.SetAlignment(StringAlignmentNear);
            strFmt.SetLineAlignment(StringAlignmentCenter);
            RectF textRc(46.0f, (REAL)curY, (REAL)w - 80.0f, (REAL)item.height);
            g.DrawString(item.label.c_str(), -1, &itemFont, textRc, &strFmt, &textBrush);

            // Optional chevron right: >
            if (item.hasChevron) {
                Color chevCol = g_isDarkMode ? Color(255, 140, 140, 150) : Color(255, 160, 165, 175);
                SolidBrush chevBrush(chevCol);
                StringFormat rightFmt;
                rightFmt.SetAlignment(StringAlignmentFar);
                rightFmt.SetLineAlignment(StringAlignmentCenter);
                RectF chevRc((REAL)w - 40.0f, (REAL)curY, 24.0f, (REAL)item.height);
                g.DrawString(L">", -1, &chevronFont, chevRc, &rightFmt, &chevBrush);
            }

            curY += item.height;
        }

        delete pFontFamily;
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;

        // Check continue button hover if logged out
        bool overContinue = false;
        if (!g_userProfile.isLoggedIn) {
            REAL btnW = (REAL)w - 48.0f;
            REAL btnH = 36.0f;
            REAL btnX = 24.0f;
            REAL btnY = 98.0f;
            if (pt.x >= btnX && pt.x <= btnX + btnW && pt.y >= btnY && pt.y <= btnY + btnH) {
                overContinue = true;
            }
        }
        if (overContinue != g_profileBtnContinueHovered) {
            g_profileBtnContinueHovered = overContinue;
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        int newHover = GetProfileItemFromPoint(pt);
        if (newHover != g_profileHoveredIndex) {
            g_profileHoveredIndex = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_profileHoveredIndex != -1 || g_profileBtnContinueHovered) {
            g_profileHoveredIndex = -1;
            g_profileBtnContinueHovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;

        // Check "Continue with Google" click
        if (!g_userProfile.isLoggedIn) {
            REAL btnW = (REAL)w - 48.0f;
            REAL btnH = 36.0f;
            REAL btnX = 24.0f;
            REAL btnY = 98.0f;
            if (pt.x >= btnX && pt.x <= btnX + btnW && pt.y >= btnY && pt.y <= btnY + btnH) {
                ShowWindow(hWnd, SW_HIDE);
                NavigateToInput(L"https://accounts.google.com/signin");
                return 0;
            }
        }

        int clickedIdx = GetProfileItemFromPoint(pt);
        if (clickedIdx >= 0 && clickedIdx < (int)g_profileItems.size()) {
            ProfileMenuAction act = g_profileItems[clickedIdx].action;
            ShowWindow(hWnd, SW_HIDE);

            switch (act) {
            case PMA_SIGN_IN_GOOGLE:
                NavigateToInput(L"https://accounts.google.com/signin");
                break;
            case PMA_MANAGE_ACCOUNT:
                NavigateToInput(L"https://myaccount.google.com");
                break;
            case PMA_PASSWORDS:
                NavigateToInput(L"https://passwords.google.com");
                break;
            case PMA_CUSTOMIZE:
                NavigateToInput(L"haze://settings");
                break;
            case PMA_SIGN_OUT:
                g_userProfile.isLoggedIn = false;
                SaveUserProfile();
                if (g_hBtnProfile) InvalidateRect(g_hBtnProfile, nullptr, TRUE);
                break;
            case PMA_BROWSE_GUEST:
                CreateNewTab(L"https://www.google.com");
                break;
            case PMA_ADD_PROFILE:
                NavigateToInput(L"https://accounts.google.com/AddSession");
                break;
            default:
                break;
            }
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    }
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowProfileDropdown() {
    if (!g_hProfileMenuWnd || !g_hBtnProfile) return;

    SetupProfileMenuItems();

    RECT btnRc;
    GetWindowRect(g_hBtnProfile, &btnRc);

    int menuW = PROFILE_MENU_WIDTH;
    int menuH = CalculateProfileMenuHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int posX = btnRc.right - menuW + 16;
    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = btnRc.bottom + 6;
    if (posY + menuH > workArea.bottom) posY = btnRc.top - menuH - 6;

    g_profileHoveredIndex = -1;
    g_profileBtnContinueHovered = false;

    SetWindowPos(g_hProfileMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hProfileMenuWnd);
    SetFocus(g_hProfileMenuWnd);
    InvalidateRect(g_hProfileMenuWnd, nullptr, TRUE);
}


// --------------------------------------------------------------------------
// In-App Microsoft Edge Style Menu Implementation (Light Theme, media 2)
// --------------------------------------------------------------------------
constexpr int INAPP_MENU_WIDTH = 340;

enum InAppItemType {
    INAPP_ITEM_NORMAL,
    INAPP_ITEM_ZOOM_ROW,
    INAPP_ITEM_SEPARATOR
};

enum InAppIcon {
    ICON_NONE = 0,
    ICON_NEW_TAB,
    ICON_NEW_WINDOW,
    ICON_NEW_INPRIVATE,
    ICON_ZOOM,
    ICON_FAVORITES,
    ICON_HISTORY,
    ICON_TAB_GROUPS,
    ICON_DOWNLOADS,
    ICON_EXTENSIONS,
    ICON_PASSWORDS,
    ICON_DELETE_DATA,
    ICON_PRINT,
    ICON_TRANSLATE,
    ICON_SCREENSHOT,
    ICON_FIND,
    ICON_SETTINGS,
    ICON_HELP
};

enum InAppActionId {
    INAPP_ACT_NONE = 0,
    INAPP_ACT_NEW_TAB,
    INAPP_ACT_NEW_WINDOW,
    INAPP_ACT_NEW_INPRIVATE,
    INAPP_ACT_FAVORITES,
    INAPP_ACT_HISTORY,
    INAPP_ACT_TAB_GROUPS,
    INAPP_ACT_DOWNLOADS,
    INAPP_ACT_EXTENSIONS,
    INAPP_ACT_PASSWORDS,
    INAPP_ACT_DELETE_DATA,
    INAPP_ACT_PRINT,
    INAPP_ACT_TRANSLATE,
    INAPP_ACT_SCREENSHOT,
    INAPP_ACT_FIND,
    INAPP_ACT_MORE_TOOLS,
    INAPP_ACT_SETTINGS,
    INAPP_ACT_HELP,
    INAPP_ACT_CLOSE
};

struct InAppMenuItem {
    InAppItemType type;
    InAppIcon icon;
    std::wstring label;
    std::wstring shortcutOrFlyout;
    InAppActionId action;
    int height;
};

std::vector<InAppMenuItem> g_inAppItems;
int g_inAppHoveredIndex = -1;
int g_zoomHoveredBtn = 0; // 1: minus, 2: plus, 3: fullscreen

void SetupInAppMenuItems() {
    g_inAppItems = {
        // Section 1: Tabs & Windows
        { INAPP_ITEM_NORMAL, ICON_NEW_TAB, L"New tab", L"", INAPP_ACT_NEW_TAB, 36 },
        { INAPP_ITEM_NORMAL, ICON_NEW_WINDOW, L"New window", L"", INAPP_ACT_NEW_WINDOW, 36 },
        { INAPP_ITEM_NORMAL, ICON_NEW_INPRIVATE, L"New InPrivate window", L"", INAPP_ACT_NEW_INPRIVATE, 36 },
        { INAPP_ITEM_SEPARATOR, ICON_NONE, L"", L"", INAPP_ACT_NONE, 10 },

        // Section 2: Zoom Row
        { INAPP_ITEM_ZOOM_ROW, ICON_ZOOM, L"Zoom", L"", INAPP_ACT_NONE, 40 },
        { INAPP_ITEM_SEPARATOR, ICON_NONE, L"", L"", INAPP_ACT_NONE, 10 },

        // Section 3: Navigation & Data
        { INAPP_ITEM_NORMAL, ICON_FAVORITES, L"Favorites", L"", INAPP_ACT_FAVORITES, 36 },
        { INAPP_ITEM_NORMAL, ICON_HISTORY, L"History", L"", INAPP_ACT_HISTORY, 36 },
        { INAPP_ITEM_NORMAL, ICON_TAB_GROUPS, L"Tab groups", L"", INAPP_ACT_TAB_GROUPS, 36 },
        { INAPP_ITEM_NORMAL, ICON_DOWNLOADS, L"Downloads", L"", INAPP_ACT_DOWNLOADS, 36 },
        { INAPP_ITEM_NORMAL, ICON_EXTENSIONS, L"Extensions", L"", INAPP_ACT_EXTENSIONS, 36 },
        { INAPP_ITEM_NORMAL, ICON_PASSWORDS, L"Passwords", L"", INAPP_ACT_PASSWORDS, 36 },
        { INAPP_ITEM_SEPARATOR, ICON_NONE, L"", L"", INAPP_ACT_NONE, 10 },

        // Section 4: Tools & Actions
        { INAPP_ITEM_NORMAL, ICON_DELETE_DATA, L"Delete browsing data", L"", INAPP_ACT_DELETE_DATA, 36 },
        { INAPP_ITEM_NORMAL, ICON_PRINT, L"Print", L"", INAPP_ACT_PRINT, 36 },
        { INAPP_ITEM_NORMAL, ICON_TRANSLATE, L"Translate", L"", INAPP_ACT_TRANSLATE, 36 },
        { INAPP_ITEM_NORMAL, ICON_SCREENSHOT, L"Screenshot", L"", INAPP_ACT_SCREENSHOT, 36 },
        { INAPP_ITEM_NORMAL, ICON_FIND, L"Find on page", L"", INAPP_ACT_FIND, 36 },
        { INAPP_ITEM_NORMAL, ICON_NONE, L"More tools", L"", INAPP_ACT_MORE_TOOLS, 36 },
        { INAPP_ITEM_SEPARATOR, ICON_NONE, L"", L"", INAPP_ACT_NONE, 10 },

        // Section 5: Settings & Help
        { INAPP_ITEM_NORMAL, ICON_SETTINGS, L"Settings", L"", INAPP_ACT_SETTINGS, 36 },
        { INAPP_ITEM_NORMAL, ICON_HELP, L"Help and feedback", L"", INAPP_ACT_HELP, 36 },
        { INAPP_ITEM_SEPARATOR, ICON_NONE, L"", L"", INAPP_ACT_NONE, 10 },

        // Section 6: Close
        { INAPP_ITEM_NORMAL, ICON_NONE, L"Close HAZE", L"", INAPP_ACT_CLOSE, 38 }
    };
}
int CalculateInAppMenuHeight() {
    int total = 14;
    for (const auto& item : g_inAppItems) {
        total += item.height;
    }
    return total;
}

RECT GetInAppItemRect(int index) {
    if (index < 0 || index >= (int)g_inAppItems.size()) return RECT{ 0, 0, 0, 0 };
    int y = 7;
    for (int i = 0; i < index; ++i) {
        y += g_inAppItems[i].height;
    }
    return RECT{ 6, y, INAPP_MENU_WIDTH - 6, y + g_inAppItems[index].height };
}

int GetInAppItemFromPoint(POINT pt) {
    for (int i = 0; i < (int)g_inAppItems.size(); ++i) {
        if (g_inAppItems[i].type == INAPP_ITEM_SEPARATOR) continue;
        RECT rc = GetInAppItemRect(i);
        if (PtInRect(&rc, pt)) return i;
    }
    return -1;
}

void DrawInAppIcon(Graphics& g, InAppIcon icon, REAL cx, REAL cy, const Color& color) {
    Pen pen(color, 1.35f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);

    switch (icon) {
    case ICON_NEW_TAB:
        g.DrawRectangle(&pen, cx - 6.5f, cy - 6.0f, 13.0f, 12.0f);
        g.DrawLine(&pen, cx - 3.5f, cy, cx + 3.5f, cy);
        g.DrawLine(&pen, cx, cy - 3.5f, cx, cy + 3.5f);
        break;
    case ICON_NEW_WINDOW:
        g.DrawRectangle(&pen, cx - 6.5f, cy - 6.0f, 13.0f, 12.0f);
        g.DrawLine(&pen, cx - 6.5f, cy - 2.5f, cx + 6.5f, cy - 2.5f);
        break;
    case ICON_NEW_INPRIVATE:
        g.DrawRectangle(&pen, cx - 4.5f, cy - 6.5f, 11.0f, 8.5f);
        g.DrawRectangle(&pen, cx - 6.5f, cy - 3.5f, 11.0f, 8.5f);
        break;
    case ICON_ZOOM:
        g.DrawEllipse(&pen, cx - 6.0f, cy - 6.0f, 9.0f, 9.0f);
        g.DrawLine(&pen, cx + 1.0f, cy + 1.0f, cx + 6.0f, cy + 6.0f);
        break;
    case ICON_FAVORITES: {
        PointF starPts[10];
        REAL rOuter = 6.5f;
        REAL rInner = 2.8f;
        for (int i = 0; i < 10; ++i) {
            REAL a = (i * 36.0f - 90.0f) * 3.14159265f / 180.0f;
            REAL r = (i % 2 == 0) ? rOuter : rInner;
            starPts[i] = PointF(cx + r * cosf(a), cy + r * sinf(a));
        }
        g.DrawPolygon(&pen, starPts, 10);
        break;
    }
    case ICON_HISTORY:
        g.DrawEllipse(&pen, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
        g.DrawLine(&pen, cx, cy, cx, cy - 3.5f);
        g.DrawLine(&pen, cx, cy, cx + 2.8f, cy);
        break;
    case ICON_TAB_GROUPS:
        g.DrawRectangle(&pen, cx - 6.5f, cy - 4.5f, 13.0f, 9.0f);
        g.DrawLine(&pen, cx - 1.5f, cy - 4.5f, cx - 1.5f, cy + 4.5f);
        break;
    case ICON_DOWNLOADS:
        g.DrawLine(&pen, cx, cy - 6.0f, cx, cy + 1.5f);
        g.DrawLine(&pen, cx, cy + 1.5f, cx - 3.0f, cy - 1.5f);
        g.DrawLine(&pen, cx, cy + 1.5f, cx + 3.0f, cy - 1.5f);
        g.DrawLine(&pen, cx - 5.5f, cy + 5.0f, cx + 5.5f, cy + 5.0f);
        break;
    case ICON_EXTENSIONS:
        g.DrawRectangle(&pen, cx - 5.5f, cy - 5.5f, 5.0f, 5.0f);
        g.DrawRectangle(&pen, cx + 0.5f, cy - 5.5f, 5.0f, 5.0f);
        g.DrawRectangle(&pen, cx - 5.5f, cy + 0.5f, 5.0f, 5.0f);
        g.DrawRectangle(&pen, cx + 0.5f, cy + 0.5f, 5.0f, 5.0f);
        break;
    case ICON_PASSWORDS:
        g.DrawEllipse(&pen, cx - 5.5f, cy - 3.5f, 6.5f, 6.5f);
        g.DrawLine(&pen, cx + 1.0f, cy, cx + 6.5f, cy);
        g.DrawLine(&pen, cx + 4.0f, cy, cx + 4.0f, cy + 3.0f);
        g.DrawLine(&pen, cx + 6.5f, cy, cx + 6.5f, cy + 2.0f);
        break;
    case ICON_DELETE_DATA:
        g.DrawLine(&pen, cx - 5.5f, cy - 4.5f, cx + 5.5f, cy - 4.5f);
        g.DrawRectangle(&pen, cx - 4.0f, cy - 3.5f, 8.0f, 9.0f);
        g.DrawLine(&pen, cx - 1.5f, cy - 1.5f, cx - 1.5f, cy + 3.5f);
        g.DrawLine(&pen, cx + 1.5f, cy - 1.5f, cx + 1.5f, cy + 3.5f);
        break;
    case ICON_PRINT:
        g.DrawRectangle(&pen, cx - 6.0f, cy - 2.5f, 12.0f, 6.5f);
        g.DrawRectangle(&pen, cx - 4.0f, cy - 6.0f, 8.0f, 3.5f);
        g.DrawLine(&pen, cx - 3.5f, cy + 6.0f, cx + 3.5f, cy + 6.0f);
        break;
    case ICON_TRANSLATE:
        g.DrawRectangle(&pen, cx - 6.5f, cy - 6.0f, 8.0f, 8.0f);
        g.DrawRectangle(&pen, cx - 1.5f, cy - 1.5f, 8.0f, 8.0f);
        break;
    case ICON_SCREENSHOT:
        g.DrawLine(&pen, cx - 5.5f, cy - 2.5f, cx - 5.5f, cy - 5.5f);
        g.DrawLine(&pen, cx - 5.5f, cy - 5.5f, cx - 2.5f, cy - 5.5f);
        g.DrawLine(&pen, cx + 5.5f, cy - 2.5f, cx + 5.5f, cy - 5.5f);
        g.DrawLine(&pen, cx + 5.5f, cy - 5.5f, cx + 2.5f, cy - 5.5f);
        g.DrawLine(&pen, cx - 5.5f, cy + 2.5f, cx - 5.5f, cy + 5.5f);
        g.DrawLine(&pen, cx - 5.5f, cy + 5.5f, cx - 2.5f, cy + 5.5f);
        g.DrawLine(&pen, cx + 5.5f, cy + 2.5f, cx + 5.5f, cy + 5.5f);
        g.DrawLine(&pen, cx + 5.5f, cy + 5.5f, cx + 2.5f, cy + 5.5f);
        break;
    case ICON_FIND:
        g.DrawRectangle(&pen, cx - 5.5f, cy - 5.5f, 8.0f, 11.0f);
        g.DrawEllipse(&pen, cx + 0.5f, cy + 0.5f, 4.5f, 4.5f);
        break;
    case ICON_SETTINGS: {
        g.DrawEllipse(&pen, cx - 2.8f, cy - 2.8f, 5.6f, 5.6f);
        for (int a = 0; a < 6; ++a) {
            REAL rad = a * 60.0f * 3.14159265f / 180.0f;
            g.DrawLine(&pen, cx + cosf(rad) * 4.2f, cy + sinf(rad) * 4.2f, cx + cosf(rad) * 7.0f, cy + sinf(rad) * 7.0f);
        }
        break;
    }
    case ICON_HELP:
        g.DrawEllipse(&pen, cx - 5.5f, cy - 5.5f, 11.0f, 11.0f);
        g.DrawLine(&pen, cx, cy - 2.5f, cx + 1.5f, cy - 2.5f);
        g.DrawLine(&pen, cx + 1.5f, cy - 2.5f, cx, cy);
        g.DrawLine(&pen, cx, cy + 2.0f, cx, cy + 3.0f);
        break;
    default:
        break;
    }
}

void ExecuteInAppAction(InAppActionId action) {
    switch (action) {
    case INAPP_ACT_NEW_TAB:
        CreateNewTab(L"about:blank");
        break;
    case INAPP_ACT_NEW_WINDOW: {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case INAPP_ACT_NEW_INPRIVATE:
        CreateNewTab(L"about:blank");
        break;
    case INAPP_ACT_FAVORITES:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"edge://favorites");
        }
        break;
    case INAPP_ACT_HISTORY:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"edge://history");
        }
        break;
    case INAPP_ACT_DOWNLOADS:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"edge://downloads");
        }
        break;
    case INAPP_ACT_PRINT:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->ExecuteScript(L"window.print();", nullptr);
        }
        break;
    case INAPP_ACT_FIND:
        if (g_hEdit) {
            SetFocus(g_hEdit);
            SendMessage(g_hEdit, EM_SETSEL, 0, -1);
        }
        break;
    case INAPP_ACT_SETTINGS:
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            if (!g_settingsPageUrl.empty()) {
                g_tabs[g_activeTabIndex]->webview->Navigate(g_settingsPageUrl.c_str());
            } else {
                g_tabs[g_activeTabIndex]->webview->Navigate(L"haze://settings");
            }
        }
        break;
    case INAPP_ACT_CLOSE:
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK InAppMenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // WinUI 3 Pure White card background with subtle inner border
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0, 0, rcClient.right, rcClient.bottom);

        Pen borderPen(Color(255, 228, 230, 235), 1.0f);
        g.DrawRectangle(&borderPen, 0, 0, rcClient.right - 1, rcClient.bottom - 1);

        FontFamily* pFontFamily = nullptr;
        FontFamily testFam(L"Segoe UI Variable Text");
        if (testFam.GetLastStatus() == Ok) {
            pFontFamily = new FontFamily(L"Segoe UI Variable Text");
        } else {
            FontFamily testFam2(L"Segoe UI");
            if (testFam2.GetLastStatus() == Ok) {
                pFontFamily = new FontFamily(L"Segoe UI");
            } else {
                pFontFamily = FontFamily::GenericSansSerif()->Clone();
            }
        }

        Gdiplus::Font fontLabel(pFontFamily, 9.75f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontShortcut(pFontFamily, 9.0f, FontStyleRegular, UnitPoint);

        // WinUI 3 Light Theme typography & state colors
        SolidBrush textBrush(Color(255, 26, 26, 26));       // TextPrimary #1A1A1A
        SolidBrush shortcutBrush(Color(255, 100, 100, 100)); // Subdued secondary text
        SolidBrush hoverBrush(Color(255, 243, 244, 246));    // Subtle hover gray #F3F4F6
        SolidBrush subBtnHoverBrush(Color(255, 232, 234, 238));
        Pen sepPen(Color(255, 234, 236, 240), 1.0f);        // Subtle divider
        Color iconCol(255, 60, 64, 72);                     // Modern neutral dark icon color

        StringFormat strLeft;
        strLeft.SetAlignment(StringAlignmentNear);
        strLeft.SetLineAlignment(StringAlignmentCenter);

        StringFormat strRight;
        strRight.SetAlignment(StringAlignmentFar);
        strRight.SetLineAlignment(StringAlignmentCenter);

        StringFormat strCenter;
        strCenter.SetAlignment(StringAlignmentCenter);
        strCenter.SetLineAlignment(StringAlignmentCenter);

        for (int i = 0; i < (int)g_inAppItems.size(); ++i) {
            const auto& item = g_inAppItems[i];
            RECT rc = GetInAppItemRect(i);
            REAL rx = (REAL)rc.left;
            REAL ry = (REAL)rc.top;
            REAL rw = (REAL)(rc.right - rc.left);
            REAL rh = (REAL)(rc.bottom - rc.top);

            if (item.type == INAPP_ITEM_SEPARATOR) {
                REAL midY = ry + (rh / 2.0f);
                g.DrawLine(&sepPen, rx + 10.0f, midY, rx + rw - 10.0f, midY);
                continue;
            }

            if (item.type == INAPP_ITEM_ZOOM_ROW) {
                // Zoom icon
                DrawInAppIcon(g, ICON_ZOOM, rx + 20.0f, ry + (rh / 2.0f), iconCol);

                // "Zoom" text
                RectF zoomTextRc(rx + 42.0f, ry, 68.0f, rh);
                g.DrawString(L"Zoom", -1, &fontLabel, zoomTextRc, &strLeft, &textBrush);

                // Controls area
                REAL btnW = 32.0f;
                REAL btnH = 28.0f;
                REAL btnY = ry + (rh - btnH) / 2.0f;

                REAL xMinus = rx + rw - 164.0f;
                REAL xPct = xMinus + btnW + 2.0f;
                REAL xPlus = xPct + 56.0f + 2.0f;
                REAL xFull = xPlus + btnW + 10.0f;

                // Minus button [-]
                if (g_zoomHoveredBtn == 1) {
                    GraphicsPath bp;
                    bp.AddArc(xMinus, btnY, 6.0f, 6.0f, 180.0f, 90.0f);
                    bp.AddArc(xMinus + btnW - 6.0f, btnY, 6.0f, 6.0f, 270.0f, 90.0f);
                    bp.AddArc(xMinus + btnW - 6.0f, btnY + btnH - 6.0f, 6.0f, 6.0f, 0.0f, 90.0f);
                    bp.AddArc(xMinus, btnY + btnH - 6.0f, 6.0f, 6.0f, 90.0f, 90.0f);
                    bp.CloseFigure();
                    g.FillPath(&subBtnHoverBrush, &bp);
                }
                Pen ctlPen(iconCol, 1.4f);
                ctlPen.SetStartCap(LineCapRound);
                ctlPen.SetEndCap(LineCapRound);
                g.DrawLine(&ctlPen, xMinus + (btnW / 2.0f) - 5.0f, ry + (rh / 2.0f), xMinus + (btnW / 2.0f) + 5.0f, ry + (rh / 2.0f));

                // Zoom percentage text
                int zoomPct = 100;
                if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
                    double z = 1.0;
                    g_tabs[g_activeTabIndex]->controller->get_ZoomFactor(&z);
                    zoomPct = (int)round(z * 100.0);
                }
                std::wstring zoomStr = std::to_wstring(zoomPct) + L"%";
                RectF pctRc(xPct, ry, 56.0f, rh);
                StringFormat strZoomCenter;
                strZoomCenter.SetAlignment(StringAlignmentCenter);
                strZoomCenter.SetLineAlignment(StringAlignmentCenter);
                strZoomCenter.SetFormatFlags(StringFormatFlagsNoWrap);
                g.DrawString(zoomStr.c_str(), -1, &fontLabel, pctRc, &strZoomCenter, &textBrush);

                // Plus button [+]
                if (g_zoomHoveredBtn == 2) {
                    GraphicsPath bp;
                    bp.AddArc(xPlus, btnY, 6.0f, 6.0f, 180.0f, 90.0f);
                    bp.AddArc(xPlus + btnW - 6.0f, btnY, 6.0f, 6.0f, 270.0f, 90.0f);
                    bp.AddArc(xPlus + btnW - 6.0f, btnY + btnH - 6.0f, 6.0f, 6.0f, 0.0f, 90.0f);
                    bp.AddArc(xPlus, btnY + btnH - 6.0f, 6.0f, 6.0f, 90.0f, 90.0f);
                    bp.CloseFigure();
                    g.FillPath(&subBtnHoverBrush, &bp);
                }
                REAL pcx = xPlus + (btnW / 2.0f), pcy = ry + (rh / 2.0f);
                g.DrawLine(&ctlPen, pcx - 5.0f, pcy, pcx + 5.0f, pcy);
                g.DrawLine(&ctlPen, pcx, pcy - 5.0f, pcx, pcy + 5.0f);

                // Fullscreen button
                REAL btnFullW = 24.0f;
                if (g_zoomHoveredBtn == 3) {
                    GraphicsPath bp;
                    bp.AddArc(xFull, btnY, 6.0f, 6.0f, 180.0f, 90.0f);
                    bp.AddArc(xFull + btnFullW - 6.0f, btnY, 6.0f, 6.0f, 270.0f, 90.0f);
                    bp.AddArc(xFull + btnFullW - 6.0f, btnY + btnH - 6.0f, 6.0f, 6.0f, 0.0f, 90.0f);
                    bp.AddArc(xFull, btnY + btnH - 6.0f, 6.0f, 6.0f, 90.0f, 90.0f);
                    bp.CloseFigure();
                    g.FillPath(&subBtnHoverBrush, &bp);
                }
                REAL fcx = xFull + (btnFullW / 2.0f), fcy = ry + (rh / 2.0f);
                g.DrawLine(&ctlPen, fcx - 4.5f, fcy + 4.5f, fcx + 4.5f, fcy - 4.5f);
                g.DrawLine(&ctlPen, fcx + 1.5f, fcy - 4.5f, fcx + 4.5f, fcy - 4.5f);
                g.DrawLine(&ctlPen, fcx + 4.5f, fcy - 4.5f, fcx + 4.5f, fcy - 1.5f);
                g.DrawLine(&ctlPen, fcx - 1.5f, fcy + 4.5f, fcx - 4.5f, fcy + 4.5f);
                g.DrawLine(&ctlPen, fcx - 4.5f, fcy + 4.5f, fcx - 4.5f, fcy + 1.5f);
                continue;
            }

            // Normal Item
            bool isHover = (i == g_inAppHoveredIndex);
            if (isHover) {
                GraphicsPath hoverPath;
                REAL hx = rx + 2.0f;
                REAL hy = ry + 1.5f;
                REAL hw = rw - 4.0f;
                REAL hh = rh - 3.0f;
                REAL cr = 5.0f; // WinUI 3 rounded pill hover
                hoverPath.AddArc(hx, hy, cr * 2.0f, cr * 2.0f, 180.0f, 90.0f);
                hoverPath.AddArc(hx + hw - cr * 2.0f, hy, cr * 2.0f, cr * 2.0f, 270.0f, 90.0f);
                hoverPath.AddArc(hx + hw - cr * 2.0f, hy + hh - cr * 2.0f, cr * 2.0f, cr * 2.0f, 0.0f, 90.0f);
                hoverPath.AddArc(hx, hy + hh - cr * 2.0f, cr * 2.0f, cr * 2.0f, 90.0f, 90.0f);
                hoverPath.CloseFigure();
                g.FillPath(&hoverBrush, &hoverPath);
            }

            // Draw Icon if present
            if (item.icon != ICON_NONE) {
                DrawInAppIcon(g, item.icon, rx + 20.0f, ry + (rh / 2.0f), iconCol);
            }

            // Draw Label
            REAL textLeft = (item.icon != ICON_NONE) ? rx + 42.0f : rx + 16.0f;
            if (item.action == INAPP_ACT_CLOSE) textLeft = rx + 16.0f;
            RectF textRc(textLeft, ry, rw - textLeft - 8.0f, rh);
            g.DrawString(item.label.c_str(), -1, &fontLabel, textRc, &strLeft, &textBrush);

            // Draw Sub-menu / chevron if present (no keyboard shortcuts)
            if (!item.shortcutOrFlyout.empty()) {
                RectF scRc(rx + 14.0f, ry, rw - 24.0f, rh);
                g.DrawString(item.shortcutOrFlyout.c_str(), -1, &fontShortcut, scRc, &strRight, &shortcutBrush);
            }
        }

        delete pFontFamily;
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int newHover = GetInAppItemFromPoint(pt);
        int newZoomBtn = 0;

        if (newHover >= 0 && newHover < (int)g_inAppItems.size() && g_inAppItems[newHover].type == INAPP_ITEM_ZOOM_ROW) {
            RECT rc = GetInAppItemRect(newHover);
            int rw = rc.right - rc.left;
            int xMinus = rc.left + rw - 164;
            int xPlus = xMinus + 32 + 2 + 56 + 2;
            int xFull = xPlus + 32 + 10;
            if (pt.x >= xMinus && pt.x <= xMinus + 32) newZoomBtn = 1;
            else if (pt.x >= xPlus && pt.x <= xPlus + 32) newZoomBtn = 2;
            else if (pt.x >= xFull && pt.x <= xFull + 28) newZoomBtn = 3;
        }

        if (newHover != g_inAppHoveredIndex || newZoomBtn != g_zoomHoveredBtn) {
            g_inAppHoveredIndex = newHover;
            g_zoomHoveredBtn = newZoomBtn;
            InvalidateRect(hWnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_inAppHoveredIndex != -1 || g_zoomHoveredBtn != 0) {
            g_inAppHoveredIndex = -1;
            g_zoomHoveredBtn = 0;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ LOWORD(lParam), HIWORD(lParam) };
        int clickedIdx = GetInAppItemFromPoint(pt);
        if (clickedIdx >= 0 && clickedIdx < (int)g_inAppItems.size()) {
            const auto& item = g_inAppItems[clickedIdx];
            if (item.type == INAPP_ITEM_ZOOM_ROW) {
                RECT rc = GetInAppItemRect(clickedIdx);
                int rw = rc.right - rc.left;
                int xMinus = rc.left + rw - 164;
                int xPlus = xMinus + 32 + 2 + 56 + 2;
                int xFull = xPlus + 32 + 10;
                if (pt.x >= xMinus && pt.x <= xMinus + 32) {
                    // Zoom Out
                    if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
                        double z = 1.0;
                        g_tabs[g_activeTabIndex]->controller->get_ZoomFactor(&z);
                        z -= 0.1;
                        if (z < 0.25) z = 0.25;
                        g_tabs[g_activeTabIndex]->controller->put_ZoomFactor(z);
                        InvalidateRect(hWnd, nullptr, FALSE);
                    }
                    return 0;
                }
                else if (pt.x >= xPlus && pt.x <= xPlus + 32) {
                    // Zoom In
                    if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->controller) {
                        double z = 1.0;
                        g_tabs[g_activeTabIndex]->controller->get_ZoomFactor(&z);
                        z += 0.1;
                        if (z > 5.0) z = 5.0;
                        g_tabs[g_activeTabIndex]->controller->put_ZoomFactor(z);
                        InvalidateRect(hWnd, nullptr, FALSE);
                    }
                    return 0;
                }
                else if (pt.x >= xFull && pt.x <= xFull + 28) {
                    // Fullscreen
                    ShowWindow(hWnd, SW_HIDE);
                    ShowWindow(g_hWnd, IsZoomed(g_hWnd) ? SW_RESTORE : SW_MAXIMIZE);
                    return 0;
                }
                return 0;
            }

            ShowWindow(hWnd, SW_HIDE);
            ExecuteInAppAction(item.action);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    }
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowSettingsDropdown() {
    if (!g_hInAppMenuWnd) return;

    POINT cursor;
    GetCursorPos(&cursor);

    int menuW = INAPP_MENU_WIDTH;
    int menuH = CalculateInAppMenuHeight();

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int posX = cursor.x - menuW / 2;
    if (g_hBtnSettings) {
        RECT btnRc;
        GetWindowRect(g_hBtnSettings, &btnRc);
        posX = btnRc.right - menuW;
        int posY = btnRc.bottom + 6;
        if (posY + menuH > workArea.bottom) posY = btnRc.top - menuH - 6;

        if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
        if (posX < workArea.left) posX = workArea.left + 10;

        g_inAppHoveredIndex = -1;
        g_zoomHoveredBtn = 0;

        SetWindowPos(g_hInAppMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
        SetForegroundWindow(g_hInAppMenuWnd);
        SetFocus(g_hInAppMenuWnd);
        InvalidateRect(g_hInAppMenuWnd, nullptr, TRUE);
        return;
    }

    if (posX + menuW > workArea.right) posX = workArea.right - menuW - 10;
    if (posX < workArea.left) posX = workArea.left + 10;

    int posY = cursor.y + 10;
    if (posY + menuH > workArea.bottom) posY = cursor.y - menuH - 10;

    g_inAppHoveredIndex = -1;
    g_zoomHoveredBtn = 0;

    SetWindowPos(g_hInAppMenuWnd, HWND_TOPMOST, posX, posY, menuW, menuH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hInAppMenuWnd);
    SetFocus(g_hInAppMenuWnd);
    InvalidateRect(g_hInAppMenuWnd, nullptr, TRUE);
}

// --------------------------------------------------------------------------
// Main Window Procedure
// --------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        static HFONT s_editFont = nullptr;
        if (!s_editFont) {
            s_editFont = CreateFontW(-MulDiv(13, 96, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, GetModernFontFamilyName());
        }

        g_hBtnBack = CreateWindowExW(
            0, L"HazeNavBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            14, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_BACK, g_hInstance, nullptr);

        g_hBtnForward = CreateWindowExW(
            0, L"HazeNavBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            50, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_FORWARD, g_hInstance, nullptr);

        g_hBtnReload = CreateWindowExW(
            0, L"HazeNavBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            86, TAB_BAR_HEIGHT + 10, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_RELOAD, g_hInstance, nullptr);

        // Search engine selector button inside omnibox
        g_hBtnEngine = CreateWindowExW(
            0, L"HazeEngineBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            140, TAB_BAR_HEIGHT + 13, 42, 34,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_ENGINE, g_hInstance, nullptr);

        // Omnibox Edit Control
        g_hEdit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            190, TAB_BAR_HEIGHT + 14, 400, 32,
            hWnd, (HMENU)(INT_PTR)IDC_EDIT_URL, g_hInstance, nullptr);

        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
        UpdateOmniboxCueBanner();
        g_originalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // Arrow Action Button (hidden)
        g_hBtnSearch = CreateWindowExW(
            0, L"HazeSearchBtnClass", L"",
            WS_CHILD,
            550, TAB_BAR_HEIGHT + 11, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SEARCH, g_hInstance, nullptr);

        // Profile Avatar Button
        g_hBtnProfile = CreateWindowExW(
            0, L"HazeProfileBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            550, TAB_BAR_HEIGHT + 11, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_PROFILE, g_hInstance, nullptr);

        // Chevron Settings Dropdown Button at the end of the address bar
        g_hBtnSettings = CreateWindowExW(
            0, L"HazeSettingsBtnClass", L"",
            WS_CHILD | WS_VISIBLE,
            586, TAB_BAR_HEIGHT + 11, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, g_hInstance, nullptr);

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
        else if (wmId == IDC_BTN_PROFILE) {
            ShowProfileDropdown();
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
            SetTextColor(hdc, RGB(36, 35, 30)); // Deep crisp charcoal for optimal legibility
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
            REAL ty = (REAL)tabRc.top;
            REAL tw = (REAL)(tabRc.right - tabRc.left);
            REAL th = (REAL)(tabRc.bottom - tabRc.top);

            GraphicsPath inPath;
            AddRoundedRect(inPath, tx, ty, tw, th, tabRadius);

            if (isHover) {
                Color inHoverCol = g_isDarkMode ? Color(255, 48, 48, 56) : (isMaximized ? Color(255, 235, 230, 185) : Color(255, 242, 238, 208));
                SolidBrush hoverBrush(inHoverCol);
                g.FillPath(&hoverBrush, &inPath);
            }

            if (g_tabs[i]->pFaviconBitmap) {
                REAL iconY = (REAL)tabRc.top + (REAL)((tabRc.bottom - tabRc.top - 18) / 2.0f);
                g.DrawImage(g_tabs[i]->pFaviconBitmap, (REAL)tabRc.left + 16.0f, iconY, 18.0f, 18.0f);
            } else if (g_hAppIcon) {
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
            REAL ty = (REAL)tabRc.top;
            REAL tw = (REAL)(tabRc.right - tabRc.left);
            REAL th = (REAL)(tabRc.bottom - tabRc.top);

            GraphicsPath activeTabPath;
            AddRoundedRect(activeTabPath, tx, ty, tw, th, tabRadius);

            // Fill active tab seamlessly with toolbar brush
            g.FillPath(&toolbarBrush, &activeTabPath);

            if (g_tabs[i]->pFaviconBitmap) {
                REAL iconY = (REAL)tabRc.top + (REAL)((tabRc.bottom - tabRc.top - 18) / 2.0f);
                g.DrawImage(g_tabs[i]->pFaviconBitmap, (REAL)tabRc.left + 16.0f, iconY, 18.0f, 18.0f);
            } else if (g_hAppIcon) {
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

        // 4. Draw Pill-Shaped Rounded Search Bar (borderless soft warm-beige pill)
        RECT searchRc = GetSearchBarOuterRect(width);
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

        // Outline around the address bar is visible ONLY when active/focused
        if (g_isEditFocused) {
            Color borderColor = g_isDarkMode ? Color(255, 176, 167, 56) : Color(255, 118, 112, 27);
            Pen borderPen(borderColor, 1.8f);
            g.DrawPath(&borderPen, &sbPath);
        }

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
    case WM_SETCURSOR: {
        WORD hitTest = LOWORD(lParam);
        switch (hitTest) {
        case HTLEFT:
        case HTRIGHT:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
        case HTTOP:
        case HTBOTTOM:
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        case HTTOPLEFT:
        case HTBOTTOMRIGHT:
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            return TRUE;
        case HTTOPRIGHT:
        case HTBOTTOMLEFT:
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
            return TRUE;
        case HTCAPTION:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        case HTCLIENT: {
            if ((HWND)wParam == hWnd) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hWnd, &pt);
                RECT rc;
                GetClientRect(hWnd, &rc);
                RECT sb = GetSearchBarOuterRect(rc.right - rc.left);
                if (PtInRect(&sb, pt)) {
                    SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                    return TRUE;
                }
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        }
        default:
            break;
        }
        break;
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
        } else if (pt.y < TOP_BAR_HEIGHT) {
            RECT sb = GetSearchBarOuterRect(width);
            if (PtInRect(&sb, pt)) {
                if (g_hEdit) {
                    SetFocus(g_hEdit);
                    SendMessage(g_hEdit, EM_SETSEL, 0, -1);
                }
                return 0;
            }
        }
        break;
    }
    case WM_NCCALCSIZE: {
        if (wParam) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(hWnd)) {
                // When maximized, Windows expands the window by sizing border (typically 8px).
                // Clamp rgrc[0] to the monitor's work area so the title bar / tab strip is never clipped off-screen!
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfo(hMon, &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
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

        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfo(hMon, &mi)) {
            lpMMI->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            lpMMI->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            lpMMI->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            lpMMI->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
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
    LoadUserProfile();
    SetupTrayMenuItems();
    SetupInAppMenuItems();
    SetupProfileMenuItems();

    // Register WinUI 3 Tray Menu Class (media 1)
    WNDCLASSEXW wmc{ sizeof(WNDCLASSEXW) };
    wmc.style = CS_HREDRAW | CS_VREDRAW;
    wmc.lpfnWndProc = TrayMenuWndProc;
    wmc.hInstance = hInstance;
    wmc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wmc.lpszClassName = L"HazeTrayMenuClass";
    RegisterClassExW(&wmc);

    g_hTrayMenuWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeTrayMenuClass", L"",
        WS_POPUP,
        0, 0, TRAY_MENU_WIDTH, CalculateTrayMenuHeight(),
        nullptr, nullptr, hInstance, nullptr);

    if (g_hTrayMenuWnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hTrayMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    // Register In-App Microsoft Edge Style Menu Class (media 2)
    WNDCLASSEXW winc{ sizeof(WNDCLASSEXW) };
    winc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    winc.lpfnWndProc = InAppMenuWndProc;
    winc.hInstance = hInstance;
    winc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    winc.lpszClassName = L"HazeInAppMenuClass";
    RegisterClassExW(&winc);

    g_hInAppMenuWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeInAppMenuClass", L"",
        WS_POPUP,
        0, 0, INAPP_MENU_WIDTH, CalculateInAppMenuHeight(),
        nullptr, nullptr, hInstance, nullptr);

    if (g_hInAppMenuWnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hInAppMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    // Register WinUI 3 Profile Menu Class (media 1 & 2)
    WNDCLASSEXW wprof{ sizeof(WNDCLASSEXW) };
    wprof.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wprof.lpfnWndProc = ProfileMenuWndProc;
    wprof.hInstance = hInstance;
    wprof.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wprof.lpszClassName = L"HazeProfileMenuClass";
    RegisterClassExW(&wprof);

    g_hProfileMenuWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeProfileMenuClass", L"",
        WS_POPUP,
        0, 0, PROFILE_MENU_WIDTH, CalculateProfileMenuHeight(),
        nullptr, nullptr, hInstance, nullptr);

    if (g_hProfileMenuWnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hProfileMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
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

    // Register Navigation Button Class (Clean custom window, no 3D bevels, focus rects or layout marks)
    WNDCLASSEXW wnav{ sizeof(WNDCLASSEXW) };
    wnav.style = CS_HREDRAW | CS_VREDRAW;
    wnav.lpfnWndProc = NavButtonProc;
    wnav.hInstance = hInstance;
    wnav.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wnav.lpszClassName = L"HazeNavBtnClass";
    RegisterClassExW(&wnav);

    // Register Engine Toggle Button Class
    WNDCLASSEXW weng{ sizeof(WNDCLASSEXW) };
    weng.style = CS_HREDRAW | CS_VREDRAW;
    weng.lpfnWndProc = EngineButtonProc;
    weng.hInstance = hInstance;
    weng.hCursor = LoadCursor(nullptr, IDC_ARROW);
    weng.lpszClassName = L"HazeEngineBtnClass";
    RegisterClassExW(&weng);

    // Register Profile Button Class
    WNDCLASSEXW wprbtn{ sizeof(WNDCLASSEXW) };
    wprbtn.style = CS_HREDRAW | CS_VREDRAW;
    wprbtn.lpfnWndProc = ProfileButtonProc;
    wprbtn.hInstance = hInstance;
    wprbtn.hCursor = LoadCursor(nullptr, IDC_HAND);
    wprbtn.lpszClassName = L"HazeProfileBtnClass";
    RegisterClassExW(&wprbtn);

    // Register Settings Dropdown Button Class
    WNDCLASSEXW wset{ sizeof(WNDCLASSEXW) };
    wset.style = CS_HREDRAW | CS_VREDRAW;
    wset.lpfnWndProc = SettingsButtonProc;
    wset.hInstance = hInstance;
    wset.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wset.lpszClassName = L"HazeSettingsBtnClass";
    RegisterClassExW(&wset);

    // Register Search Action Button Class
    WNDCLASSEXW wsch{ sizeof(WNDCLASSEXW) };
    wsch.style = CS_HREDRAW | CS_VREDRAW;
    wsch.lpfnWndProc = SearchButtonProc;
    wsch.hInstance = hInstance;
    wsch.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wsch.lpszClassName = L"HazeSearchBtnClass";
    RegisterClassExW(&wsch);

    // Register Main Browser Window Class
    WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = g_hAppIcon;
    wcex.hIconSm = g_hAppIcon;
    wcex.hCursor = nullptr;
    wcex.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcex.lpszClassName = L"HazeDesktopBrowserClass";
    RegisterClassExW(&wcex);

    g_hWnd = CreateWindowExW(
        0, L"HazeDesktopBrowserClass", APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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

    if (g_hTrayMenuWnd) {
        DestroyWindow(g_hTrayMenuWnd);
    }
    if (g_hInAppMenuWnd) {
        DestroyWindow(g_hInAppMenuWnd);
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
