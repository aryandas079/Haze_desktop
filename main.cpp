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
constexpr int TOP_BAR_HEIGHT = 100;
constexpr int TAB_BAR_HEIGHT = 44; // Bigger tabs area
constexpr int TAB_WIDTH = 230;

constexpr int SEARCH_BAR_HEIGHT = 42; // Bigger comfortable search pill
constexpr int NAV_BUTTONS_WIDTH = 126;

constexpr int IDC_EDIT_URL = 101;
constexpr int IDC_BTN_SEARCH = 102;
constexpr int IDC_BTN_BACK = 103;
constexpr int IDC_BTN_FORWARD = 104;
constexpr int IDC_BTN_RELOAD = 105;

constexpr const wchar_t* APP_VERSION = L"v1.0.0";
constexpr const wchar_t* APP_NAME = L"HAZE";

// Injected CSS to enforce 'Inter' as font across websites
const wchar_t* INJECTED_INTER_SCRIPT = LR"(
(function() {
  if (document.getElementById('__haze_inter_font__')) return;
  const link = document.createElement('link');
  link.id = '__haze_inter_font__';
  link.rel = 'stylesheet';
  link.href = 'https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap';
  document.head.appendChild(link);

  const style = document.createElement('style');
  style.id = '__haze_inter_style__';
  style.textContent = `
    * {
      font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif !important;
    }
  `;
  document.head.appendChild(style);
})();
)";

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
HWND g_hBtnBack = nullptr;
HWND g_hBtnForward = nullptr;
HWND g_hBtnReload = nullptr;
HWND g_hMenuWnd = nullptr;

WNDPROC g_originalEditProc = nullptr;
WNDPROC g_originalSearchBtnProc = nullptr;
WNDPROC g_originalNavBtnProc = nullptr;

HICON g_hAppIcon = nullptr;
ULONG_PTR g_gdiplusToken = 0;
NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };

std::vector<std::unique_ptr<BrowserTab>> g_tabs;
int g_activeTabIndex = -1;
int g_hoveredTabIndex = -1;
int g_hoveredCloseTabIndex = -1;
bool g_isHoveringNewTab = false;
bool g_isSearchBtnHovered = false;
bool g_isEditFocused = false;

std::wstring g_homePageHtml;

// Custom Menu Items
enum MenuAction {
    ACTION_OPEN_HAZE = 1,
    ACTION_OPEN_SEARCH,
    ACTION_SETTINGS,
    ACTION_VERSION,
    ACTION_QUIT
};

struct WinUI3MenuItem {
    std::wstring text;
    MenuAction action;
    bool isSeparator;
    bool isHeader;
    bool isEnabled;
};

std::vector<WinUI3MenuItem> g_menuItems;
int g_hoveredIndex = -1;

void ResizeControls(int width, int height);
void SwitchToTab(int index);
void CreateNewTab(const std::wstring& initialUrl = L"about:blank");
void CloseTab(int index);

void AddRoundedRect(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
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
    return L"<html><body style='background:#fff;'></body></html>";
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
    if (trimmed.find(L"http://") == 0 || trimmed.find(L"https://") == 0) {
        url = trimmed;
    }
    else if (trimmed.find(L" ") == std::wstring::npos && trimmed.find(L".") != std::wstring::npos) {
        url = L"https://" + trimmed;
    }
    else {
        url = L"https://www.google.com/search?q=";
        for (wchar_t c : trimmed) {
            if (c == L' ') {
                url += L'+';
            }
            else if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                     c == L'-' || c == L'_' || c == L'.' || c == L'~') {
                url += c;
            }
            else {
                wchar_t buf[10];
                swprintf_s(buf, L"%%%02X", (unsigned int)(c & 0xFF));
                url += buf;
            }
        }
    }

    tab->webview->Navigate(url.c_str());
}

RECT GetSearchBarOuterRect(int clientWidth) {
    int startX = NAV_BUTTONS_WIDTH;
    int endX = clientWidth - 14;
    return RECT{ startX, TAB_BAR_HEIGHT + 7, endX, TAB_BAR_HEIGHT + 7 + SEARCH_BAR_HEIGHT };
}

void ResizeControls(int width, int height) {
    if (width <= 0 || height <= 0) return;

    // Center Navigation Buttons (←, →, ↻) vertically in the toolbar
    int navY = TAB_BAR_HEIGHT + ((TOP_BAR_HEIGHT - TAB_BAR_HEIGHT - 32) / 2);
    int navBtnSize = 32;
    if (g_hBtnBack)    SetWindowPos(g_hBtnBack, nullptr, 14, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnForward) SetWindowPos(g_hBtnForward, nullptr, 50, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnReload)  SetWindowPos(g_hBtnReload, nullptr, 86, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);

    // Search Bar & Search Action Button
    RECT sb = GetSearchBarOuterRect(width);
    int innerMargin = 18;
    int btnMargin = 5;
    int btnSize = SEARCH_BAR_HEIGHT - (btnMargin * 2);
    int editW = (sb.right - sb.left) - innerMargin - btnSize - 18;
    if (editW < 50) editW = 50;

    int editH = 26; // Bigger clear edit control
    int editY = sb.top + ((SEARCH_BAR_HEIGHT - editH) / 2);

    if (g_hEdit) {
        SetWindowPos(g_hEdit, nullptr, sb.left + innerMargin, editY, editW, editH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hBtnSearch) {
        SetWindowPos(g_hBtnSearch, nullptr, sb.right - btnMargin - btnSize - 4, sb.top + btnMargin, btnSize, btnSize, SWP_NOZORDER | SWP_NOACTIVATE);
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

// Crisp anti-aliased magnifying glass button with GDI+
LRESULT CALLBACK SearchButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        if (g_isSearchBtnHovered) {
            SolidBrush hoverBrush(Color(255, 232, 235, 242));
            g.FillEllipse(&hoverBrush, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
        }

        Pen iconPen(Color(255, 66, 133, 244), 2.4f);
        REAL cx = (rc.left + rc.right) / 2.0f - 1.5f;
        REAL cy = (rc.top + rc.bottom) / 2.0f - 1.5f;
        REAL r = 5.5f;

        g.DrawEllipse(&iconPen, cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.DrawLine(&iconPen, cx + 4.0f, cy + 4.0f, cx + 8.5f, cy + 8.5f);

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

// Crisp smooth vector navigation buttons (←, →, ↻) using GDI+
LRESULT CALLBACK NavButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

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

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        Color iconCol = enabled ? Color(255, 90, 94, 102) : Color(255, 195, 198, 204);
        Pen navPen(iconCol, 2.0f);
        navPen.SetStartCap(LineCapRound);
        navPen.SetEndCap(LineCapRound);

        REAL cx = (rc.left + rc.right) / 2.0f;
        REAL cy = (rc.top + rc.bottom) / 2.0f;

        if (id == IDC_BTN_BACK) {
            g.DrawLine(&navPen, cx + 5.5f, cy, cx - 5.5f, cy);
            g.DrawLine(&navPen, cx - 5.5f, cy, cx - 0.5f, cy - 5.0f);
            g.DrawLine(&navPen, cx - 5.5f, cy, cx - 0.5f, cy + 5.0f);
        }
        else if (id == IDC_BTN_FORWARD) {
            g.DrawLine(&navPen, cx - 5.5f, cy, cx + 5.5f, cy);
            g.DrawLine(&navPen, cx + 5.5f, cy, cx + 0.5f, cy - 5.0f);
            g.DrawLine(&navPen, cx + 5.5f, cy, cx + 0.5f, cy + 5.0f);
        }
        else if (id == IDC_BTN_RELOAD) {
            RectF arcRect(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
            g.DrawArc(&navPen, arcRect, 45.0f, 275.0f);

            SolidBrush arrowBrush(iconCol);
            PointF pt1(cx + 5.0f, cy - 6.0f);
            PointF pt2(cx + 9.5f, cy - 2.5f);
            PointF pt3(cx + 4.5f, cy - 1.0f);
            PointF pts[] = { pt1, pt2, pt3 };
            g.FillPolygon(&arrowBrush, pts, 3);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return CallWindowProc(g_originalNavBtnProc, hWnd, uMsg, wParam, lParam);
}

// --------------------------------------------------------------------------
// Tab Coordinates & Geometry (Bigger, spacious layout)
// --------------------------------------------------------------------------
RECT GetTabRect(int index, int clientWidth) {
    int x = 8 + index * (TAB_WIDTH + 4);
    int y = 6;
    return RECT{ x, y, x + TAB_WIDTH, y + TAB_BAR_HEIGHT };
}

RECT GetNewTabBtnRect(int clientWidth) {
    int x = 8 + (int)g_tabs.size() * (TAB_WIDTH + 4) + 6;
    int y = 10;
    return RECT{ x, y, x + 28, y + 28 };
}

RECT GetTabCloseRect(const RECT& tabRc) {
    int w = 20;
    int h = 20;
    int right = tabRc.right - 10;
    int top = tabRc.top + ((tabRc.bottom - tabRc.top - h) / 2) - 3;
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
        if (activeTab->url.find(L"about:blank") == 0 || activeTab->url.empty() || activeTab->url.find(L"data:text/html") == 0) {
            SetWindowTextW(g_hEdit, L"");
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

                            // Inject Inter font into every page loaded
                            tab->webview->AddScriptToExecuteOnDocumentCreated(INJECTED_INTER_SCRIPT, nullptr);

                            // Source change listener
                            tab->webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [newIndex](ICoreWebView2* webview, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(webview->get_Source(&uri)) && uri) {
                                            if (newIndex < (int)g_tabs.size()) {
                                                g_tabs[newIndex]->url = uri;
                                                if (g_activeTabIndex == newIndex && g_hEdit) {
                                                    if (wcscmp(uri, L"about:blank") == 0 || wcsstr(uri, L"data:text/html") != nullptr) {
                                                        SetWindowTextW(g_hEdit, L"");
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

                            // Document Title listener
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

                            // If blank or about:blank, navigate to beautiful centered HAZE homepage
                            if (initialUrl.empty() || initialUrl == L"about:blank") {
                                tab->title = L"New Tab";
                                tab->webview->NavigateToString(g_homePageHtml.c_str());
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
            g_tabs[0]->webview->NavigateToString(g_homePageHtml.c_str());
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
constexpr int MENU_WIDTH = 230;
constexpr int ITEM_HEIGHT = 36;
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
    int y = MENU_PADDING_Y;
    for (size_t i = 0; i < g_menuItems.size(); ++i) {
        int itemH = g_menuItems[i].isSeparator ? SEPARATOR_HEIGHT : ITEM_HEIGHT;
        if (pt.y >= y && pt.y < y + itemH && pt.x >= MENU_PADDING_X && pt.x <= (MENU_WIDTH - MENU_PADDING_X)) {
            if (g_menuItems[i].isSeparator || g_menuItems[i].isHeader || !g_menuItems[i].isEnabled) {
                return -1;
            }
            return (int)i;
        }
        y += itemH;
    }
    return -1;
}

void ExecuteMenuAction(MenuAction act) {
    switch (act) {
    case ACTION_OPEN_HAZE:
        RestoreMainWindow(false);
        break;
    case ACTION_OPEN_SEARCH:
        RestoreMainWindow(true);
        break;
    case ACTION_SETTINGS:
        RestoreMainWindow(false);
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size() && g_tabs[g_activeTabIndex]->webview) {
            g_tabs[g_activeTabIndex]->webview->Navigate(L"https://www.google.com/preferences");
        }
        break;
    case ACTION_QUIT:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
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

        SolidBrush bgBrush(Color(255, 252, 252, 254));
        g.FillRectangle(&bgBrush, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left), (REAL)(rcClient.bottom - rcClient.top));

        GraphicsPath path;
        AddRoundedRect(path, (REAL)rcClient.left, (REAL)rcClient.top, (REAL)(rcClient.right - rcClient.left - 1), (REAL)(rcClient.bottom - rcClient.top - 1), 10.0f);

        Pen borderPen(Color(255, 226, 226, 229), 1.0f);
        g.DrawPath(&borderPen, &path);

        // Try Inter or Segoe UI Variable
        FontFamily* pFontFamily = new FontFamily(L"Inter");
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI Variable Text");
        }
        Gdiplus::Font fontRegular(pFontFamily, 10.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontBold(pFontFamily, 10.5f, FontStyleBold, UnitPoint);

        StringFormat strFormat;
        strFormat.SetAlignment(StringAlignmentNear);
        strFormat.SetLineAlignment(StringAlignmentCenter);

        for (size_t i = 0; i < g_menuItems.size(); ++i) {
            const auto& item = g_menuItems[i];
            RECT itemRc = GetItemRect((int)i);

            if (item.isSeparator) {
                REAL midY = (itemRc.top + itemRc.bottom) / 2.0f;
                Pen sepPen(Color(255, 232, 232, 235), 1.0f);
                g.DrawLine(&sepPen, (REAL)itemRc.left + 8.0f, midY, (REAL)itemRc.right - 8.0f, midY);
                continue;
            }

            if ((int)i == g_hoveredIndex && item.isEnabled && !item.isHeader) {
                GraphicsPath hoverPath;
                AddRoundedRect(hoverPath, (REAL)itemRc.left + 2, (REAL)itemRc.top + 2, (REAL)(itemRc.right - itemRc.left - 4), (REAL)(itemRc.bottom - itemRc.top - 4), 6.0f);
                SolidBrush hoverBrush(Color(255, 240, 240, 243));
                g.FillPath(&hoverBrush, &hoverPath);
            }

            Color textCol = item.isHeader ? Color(255, 20, 20, 20) : (item.isEnabled ? Color(255, 30, 30, 32) : Color(255, 130, 130, 136));
            SolidBrush textBrush(textCol);

            RectF layoutRect((REAL)itemRc.left + 16.0f, (REAL)itemRc.top, (REAL)(itemRc.right - itemRc.left - 32.0f), (REAL)(itemRc.bottom - itemRc.top));
            g.DrawString(item.text.c_str(), -1, item.isHeader ? &fontBold : &fontRegular, layoutRect, &strFormat, &textBrush);
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

void SetupMenuItems() {
    g_menuItems.clear();
    g_menuItems.push_back({ std::wstring(APP_NAME) + L" " + APP_VERSION, ACTION_VERSION, false, true, false });
    g_menuItems.push_back({ L"", (MenuAction)0, true, false, false });
    g_menuItems.push_back({ L"Open HAZE", ACTION_OPEN_HAZE, false, false, true });
    g_menuItems.push_back({ L"Open Search", ACTION_OPEN_SEARCH, false, false, true });
    g_menuItems.push_back({ L"Settings", ACTION_SETTINGS, false, false, true });
    g_menuItems.push_back({ L"", (MenuAction)0, true, false, false });
    g_menuItems.push_back({ L"Quit", ACTION_QUIT, false, false, true });
}

// --------------------------------------------------------------------------
// Main Window Procedure
// --------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Inter");
        if (!hFont) {
            hFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

        g_hEdit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            140, TAB_BAR_HEIGHT + 14, 400, 26,
            hWnd, (HMENU)(INT_PTR)IDC_EDIT_URL, g_hInstance, nullptr);

        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(g_hEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search or enter address");
        g_originalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        g_hBtnSearch = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            550, TAB_BAR_HEIGHT + 11, 32, 32,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SEARCH, g_hInstance, nullptr);

        g_originalSearchBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnSearch, GWLP_WNDPROC, (LONG_PTR)SearchButtonSubclassProc);

        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hAppIcon;
        wcscpy_s(g_nid.szTip, L"HAZE Browser");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        // Load HomePage HTML and start on Blank Home Page with centered logo
        g_homePageHtml = LoadHomePageHtml();
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
        SetTextColor(hdc, RGB(32, 33, 36));
        SetBkColor(hdc, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
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

        // 1. Tab strip background (#DEE1E6)
        SolidBrush stripBrush(Color(255, 222, 225, 230));
        g.FillRectangle(&stripBrush, 0.0f, 0.0f, (REAL)width, (REAL)TAB_BAR_HEIGHT);

        // 2. Toolbar & Navigation row background (#FFFFFF)
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&whiteBrush, 0.0f, (REAL)TAB_BAR_HEIGHT, (REAL)width, (REAL)(TOP_BAR_HEIGHT - TAB_BAR_HEIGHT));

        FontFamily* pFontFamily = new FontFamily(L"Inter");
        if (pFontFamily->GetLastStatus() != Ok) {
            delete pFontFamily;
            pFontFamily = new FontFamily(L"Segoe UI");
        }
        Gdiplus::Font tabFont(pFontFamily, 10.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font tabBoldFont(pFontFamily, 10.5f, FontStyleBold, UnitPoint);

        StringFormat strFormat;
        strFormat.SetAlignment(StringAlignmentNear);
        strFormat.SetLineAlignment(StringAlignmentCenter);
        strFormat.SetTrimming(StringTrimmingEllipsisCharacter);
        strFormat.SetFormatFlags(StringFormatFlagsNoWrap);

        // 3. Render Anti-Aliased Chrome-style tabs (Bigger & spacious)
        for (size_t i = 0; i < g_tabs.size(); ++i) {
            RECT tabRc = GetTabRect((int)i, width);
            bool isActive = ((int)i == g_activeTabIndex);
            bool isHover = ((int)i == g_hoveredTabIndex);

            REAL tRadius = 8.0f;
            REAL tx = (REAL)tabRc.left;
            REAL ty = (REAL)tabRc.top;
            REAL tw = (REAL)(tabRc.right - tabRc.left);
            REAL th = (REAL)(tabRc.bottom - tabRc.top + 6);

            GraphicsPath tabPath;
            tabPath.AddArc(tx, ty, tRadius * 2.0f, tRadius * 2.0f, 180.0f, 90.0f);
            tabPath.AddArc(tx + tw - tRadius * 2.0f, ty, tRadius * 2.0f, tRadius * 2.0f, 270.0f, 90.0f);
            tabPath.AddLine(tx + tw, ty + th, tx, ty + th);
            tabPath.CloseFigure();

            if (isActive) {
                g.FillPath(&whiteBrush, &tabPath);
            }
            else if (isHover) {
                SolidBrush hoverBrush(Color(255, 232, 235, 239));
                g.FillPath(&hoverBrush, &tabPath);
            }

            // Draw crisp icon (20x20)
            if (g_hAppIcon) {
                DrawIconEx(hdc, tabRc.left + 14, tabRc.top + 10, g_hAppIcon, 20, 20, 0, nullptr, DI_NORMAL);
            }

            // Draw crisp tab title
            Color textCol = isActive ? Color(255, 28, 30, 33) : Color(255, 90, 94, 100);
            SolidBrush textBrush(textCol);
            RectF titleRect((REAL)tabRc.left + 42.0f, (REAL)tabRc.top, (REAL)(tabRc.right - tabRc.left - 72.0f), (REAL)(tabRc.bottom - tabRc.top));
            g.DrawString(g_tabs[i]->title.c_str(), -1, isActive ? &tabBoldFont : &tabFont, titleRect, &strFormat, &textBrush);

            // Draw tab close button vector "×"
            RECT closeRc = GetTabCloseRect(tabRc);
            bool isCloseHover = ((int)i == g_hoveredCloseTabIndex);
            if (isCloseHover) {
                SolidBrush closeBg(Color(255, 226, 229, 235));
                g.FillEllipse(&closeBg, (REAL)closeRc.left, (REAL)closeRc.top, (REAL)(closeRc.right - closeRc.left), (REAL)(closeRc.bottom - closeRc.top));
            }

            Pen closePen(isCloseHover ? Color(255, 217, 48, 37) : Color(255, 95, 99, 104), 1.6f);
            closePen.SetStartCap(LineCapRound);
            closePen.SetEndCap(LineCapRound);

            REAL cx = (closeRc.left + closeRc.right) / 2.0f;
            REAL cy = (closeRc.top + closeRc.bottom) / 2.0f;
            REAL sz = 4.0f;
            g.DrawLine(&closePen, cx - sz, cy - sz, cx + sz, cy + sz);
            g.DrawLine(&closePen, cx + sz, cy - sz, cx - sz, cy + sz);
        }

        // Draw crisp "+" New Tab Button (Spacious circle button)
        RECT newTabRc = GetNewTabBtnRect(width);
        if (g_isHoveringNewTab) {
            SolidBrush ntBrush(Color(255, 230, 233, 238));
            g.FillEllipse(&ntBrush, (REAL)newTabRc.left, (REAL)newTabRc.top, (REAL)(newTabRc.right - newTabRc.left), (REAL)(newTabRc.bottom - newTabRc.top));
        }
        Pen plusPen(Color(255, 60, 64, 67), 1.8f);
        plusPen.SetStartCap(LineCapRound);
        plusPen.SetEndCap(LineCapRound);
        REAL ntcx = (newTabRc.left + newTabRc.right) / 2.0f;
        REAL ntcy = (newTabRc.top + newTabRc.bottom) / 2.0f;
        g.DrawLine(&plusPen, ntcx - 5.5f, ntcy, ntcx + 5.5f, ntcy);
        g.DrawLine(&plusPen, ntcx, ntcy - 5.5f, ntcx, ntcy + 5.5f);

        // 4. Draw Pill-Shaped Rounded Search Bar with Smooth Anti-Aliasing
        RECT searchRc = GetSearchBarOuterRect(width);
        Color borderColor = g_isEditFocused ? Color(255, 26, 115, 232) : Color(255, 66, 133, 244);
        Pen borderPen(borderColor, 2.0f);

        REAL sx = (REAL)searchRc.left;
        REAL sy = (REAL)searchRc.top;
        REAL sw = (REAL)(searchRc.right - searchRc.left);
        REAL sh = (REAL)(searchRc.bottom - searchRc.top);
        REAL sbRadius = sh / 2.0f;

        GraphicsPath sbPath;
        sbPath.AddArc(sx, sy, sbRadius * 2.0f, sbRadius * 2.0f, 90.0f, 180.0f);
        sbPath.AddArc(sx + sw - sbRadius * 2.0f, sy, sbRadius * 2.0f, sbRadius * 2.0f, 270.0f, 180.0f);
        sbPath.CloseFigure();

        g.FillPath(&whiteBrush, &sbPath);
        g.DrawPath(&borderPen, &sbPath);

        // 5. Divider Line separating top bar from WebView
        Pen divPen(Color(255, 228, 231, 236), 1.0f);
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

        if (pt.y < TAB_BAR_HEIGHT) {
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

        if (newHoverTab != g_hoveredTabIndex || newHoverClose != g_hoveredCloseTabIndex || newHoverNewTab != g_isHoveringNewTab) {
            g_hoveredTabIndex = newHoverTab;
            g_hoveredCloseTabIndex = newHoverClose;
            g_isHoveringNewTab = newHoverNewTab;

            RECT topRc{ 0, 0, width, TAB_BAR_HEIGHT };
            InvalidateRect(hWnd, &topRc, FALSE);

            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hoveredTabIndex != -1 || g_hoveredCloseTabIndex != -1 || g_isHoveringNewTab) {
            g_hoveredTabIndex = -1;
            g_hoveredCloseTabIndex = -1;
            g_isHoveringNewTab = false;

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

    SetupMenuItems();

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
        CW_USEDEFAULT, CW_USEDEFAULT, 1150, 780,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        return 0;
    }

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

    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
    }

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return (int)msg.wParam;
}
