#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include <string>
#include <vector>
#include <memory>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;

// Constants
constexpr UINT WM_TRAYICON = WM_USER + 100;

constexpr int TOP_BAR_HEIGHT = 86;
constexpr int TAB_BAR_HEIGHT = 38;
constexpr int TAB_WIDTH = 210;

constexpr int SEARCH_BAR_HEIGHT = 36;
constexpr int NAV_BUTTONS_WIDTH = 110; // Back, Forward, Reload buttons area

constexpr int IDC_EDIT_URL = 101;
constexpr int IDC_BTN_SEARCH = 102;
constexpr int IDC_BTN_BACK = 103;
constexpr int IDC_BTN_FORWARD = 104;
constexpr int IDC_BTN_RELOAD = 105;

constexpr const wchar_t* APP_VERSION = L"v1.0.0";
constexpr const wchar_t* APP_NAME = L"HAZE";

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
NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };

std::vector<std::unique_ptr<BrowserTab>> g_tabs;
int g_activeTabIndex = -1;
int g_hoveredTabIndex = -1;
int g_hoveredCloseTabIndex = -1;
bool g_isHoveringNewTab = false;
bool g_isSearchBtnHovered = false;
bool g_isEditFocused = false;

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
void CreateNewTab(const std::wstring& initialUrl = L"https://www.google.com");
void CloseTab(int index);

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
    return RECT{ startX, TAB_BAR_HEIGHT + 6, endX, TAB_BAR_HEIGHT + 6 + SEARCH_BAR_HEIGHT };
}

void ResizeControls(int width, int height) {
    if (width <= 0 || height <= 0) return;

    // Position Navigation Buttons (←, →, ↻)
    int navY = TAB_BAR_HEIGHT + 8;
    int navBtnSize = 28;
    if (g_hBtnBack)    SetWindowPos(g_hBtnBack, nullptr, 12, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnForward) SetWindowPos(g_hBtnForward, nullptr, 44, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hBtnReload)  SetWindowPos(g_hBtnReload, nullptr, 76, navY, navBtnSize, navBtnSize, SWP_NOZORDER | SWP_NOACTIVATE);

    // Position Search Bar and Search Action Button
    RECT sb = GetSearchBarOuterRect(width);
    int innerMargin = 16;
    int btnMargin = 4;
    int btnSize = SEARCH_BAR_HEIGHT - (btnMargin * 2);
    int editW = (sb.right - sb.left) - innerMargin - btnSize - 16;
    if (editW < 50) editW = 50;

    int editH = 22;
    int editY = sb.top + ((SEARCH_BAR_HEIGHT - editH) / 2);

    if (g_hEdit) {
        SetWindowPos(g_hEdit, nullptr, sb.left + innerMargin, editY, editW, editH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hBtnSearch) {
        SetWindowPos(g_hBtnSearch, nullptr, sb.right - btnMargin - btnSize - 4, sb.top + btnMargin, btnSize, btnSize, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Resize active WebView2 viewport
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

// Subclass edit control to handle Enter key press & focus visual states
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

// Subclass Magnifying Glass Search Button (as shown inside the pill in user's image)
LRESULT CALLBACK SearchButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        COLORREF bgCol = g_isSearchBtnHovered ? RGB(232, 234, 240) : RGB(255, 255, 255);
        HBRUSH bgBrush = CreateSolidBrush(bgCol);
        HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
        HGDIOBJ oldPen = SelectObject(hdc, nullPen);

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, rc.right - rc.left, rc.bottom - rc.top);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(bgBrush);

        // Draw Magnifying Glass Icon (fluent blue magnifying glass as in the image)
        int cx = (rc.left + rc.right) / 2 - 1;
        int cy = (rc.top + rc.bottom) / 2 - 1;
        int r = 5;

        HPEN iconPen = CreatePen(PS_SOLID, 2, RGB(66, 133, 244));
        HGDIOBJ prevPen = SelectObject(hdc, iconPen);
        HGDIOBJ prevBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
        MoveToEx(hdc, cx + 3, cy + 3, nullptr);
        LineTo(hdc, cx + 7, cy + 7);

        SelectObject(hdc, prevPen);
        SelectObject(hdc, prevBrush);
        DeleteObject(iconPen);

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

// Subclass Nav Buttons (←, →, ↻)
LRESULT CALLBACK NavButtonSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        int id = (int)GetWindowLongPtr(hWnd, GWLP_ID);

        // CanGoBack / CanGoForward state
        bool enabled = true;
        if (g_activeTabIndex >= 0 && g_activeTabIndex < (int)g_tabs.size()) {
            auto& tab = g_tabs[g_activeTabIndex];
            if (tab->webview) {
                BOOL can = FALSE;
                if (id == IDC_BTN_BACK && SUCCEEDED(tab->webview->get_CanGoBack(&can))) enabled = can;
                if (id == IDC_BTN_FORWARD && SUCCEEDED(tab->webview->get_CanGoForward(&can))) enabled = can;
            }
        }

        HFONT hIconFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ prevFont = SelectObject(hdc, hIconFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, enabled ? RGB(90, 94, 102) : RGB(190, 194, 200));

        const wchar_t* sym = L"←";
        if (id == IDC_BTN_FORWARD) sym = L"→";
        else if (id == IDC_BTN_RELOAD) sym = L"↻";

        DrawTextW(hdc, sym, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, prevFont);
        DeleteObject(hIconFont);
        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return CallWindowProc(g_originalNavBtnProc, hWnd, uMsg, wParam, lParam);
}

// --------------------------------------------------------------------------
// Tab Coordinates & Geometry
// --------------------------------------------------------------------------
RECT GetTabRect(int index, int clientWidth) {
    int x = 8 + index * (TAB_WIDTH + 4);
    int y = 5;
    return RECT{ x, y, x + TAB_WIDTH, y + TAB_BAR_HEIGHT };
}

RECT GetNewTabBtnRect(int clientWidth) {
    int x = 8 + (int)g_tabs.size() * (TAB_WIDTH + 4) + 6;
    int y = 8;
    return RECT{ x, y, x + 24, y + 24 };
}

RECT GetTabCloseRect(const RECT& tabRc) {
    int w = 18;
    int h = 18;
    int right = tabRc.right - 10;
    int top = tabRc.top + ((tabRc.bottom - tabRc.top - h) / 2) - 2;
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

    if (g_hEdit && !activeTab->url.empty()) {
        SetWindowTextW(g_hEdit, activeTab->url.c_str());
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

                            // Source change listener
                            tab->webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [newIndex](ICoreWebView2* webview, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(webview->get_Source(&uri)) && uri) {
                                            if (newIndex < (int)g_tabs.size()) {
                                                g_tabs[newIndex]->url = uri;
                                                if (g_activeTabIndex == newIndex && g_hEdit) {
                                                    SetWindowTextW(g_hEdit, uri);
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
                                                g_tabs[newIndex]->title = (wcslen(title) > 0) ? title : L"Google";
                                                RECT topRc{ 0, 0, 2000, TAB_BAR_HEIGHT };
                                                InvalidateRect(hWnd, &topRc, FALSE);
                                            }
                                            CoTaskMemFree(title);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab->isLoaded = true;
                            tab->webview->Navigate(initialUrl.c_str());

                            SwitchToTab(newIndex);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void CloseTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;

    if (g_tabs.size() == 1) {
        g_tabs[0]->title = L"Google";
        g_tabs[0]->url = L"https://www.google.com";
        if (g_tabs[0]->webview) {
            g_tabs[0]->webview->Navigate(L"https://www.google.com");
        }
        if (g_hEdit) SetWindowTextW(g_hEdit, L"https://www.google.com");
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
// WinUI 3 Style Rounded Tray Context Menu Implementation
// --------------------------------------------------------------------------
constexpr int MENU_WIDTH = 220;
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

        // WinUI3 Light Theme background (#FCFCFC)
        HBRUSH bgBrush = CreateSolidBrush(RGB(252, 252, 254));
        FillRect(hdc, &rcClient, bgBrush);
        DeleteObject(bgBrush);

        // Border: subtle WinUI fluent outline (#E2E2E5)
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(226, 226, 229));
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rcClient.left, rcClient.top, rcClient.right, rcClient.bottom, 16, 16);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(borderPen);

        HFONT hFontRegular = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        if (!hFontRegular) {
            hFontRegular = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }

        HFONT hFontBold = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");

        HGDIOBJ prevFont = SelectObject(hdc, hFontRegular);
        SetBkMode(hdc, TRANSPARENT);

        for (size_t i = 0; i < g_menuItems.size(); ++i) {
            const auto& item = g_menuItems[i];
            RECT itemRc = GetItemRect((int)i);

            if (item.isSeparator) {
                int midY = itemRc.top + (SEPARATOR_HEIGHT / 2);
                HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(232, 232, 235));
                HGDIOBJ p = SelectObject(hdc, sepPen);
                MoveToEx(hdc, itemRc.left + 8, midY, nullptr);
                LineTo(hdc, itemRc.right - 8, midY);
                SelectObject(hdc, p);
                DeleteObject(sepPen);
                continue;
            }

            if ((int)i == g_hoveredIndex && item.isEnabled && !item.isHeader) {
                HBRUSH hoverBrush = CreateSolidBrush(RGB(240, 240, 243));
                HPEN hoverPen = CreatePen(PS_SOLID, 1, RGB(235, 235, 239));
                HGDIOBJ op = SelectObject(hdc, hoverPen);
                HGDIOBJ ob = SelectObject(hdc, hoverBrush);
                RoundRect(hdc, itemRc.left + 2, itemRc.top + 2, itemRc.right - 2, itemRc.bottom - 2, 8, 8);
                SelectObject(hdc, op);
                SelectObject(hdc, ob);
                DeleteObject(hoverPen);
                DeleteObject(hoverBrush);
            }

            if (item.isHeader) {
                SelectObject(hdc, hFontBold);
                SetTextColor(hdc, RGB(20, 20, 20));
            }
            else if (!item.isEnabled) {
                SelectObject(hdc, hFontRegular);
                SetTextColor(hdc, RGB(130, 130, 136));
            }
            else {
                SelectObject(hdc, hFontRegular);
                SetTextColor(hdc, RGB(30, 30, 32));
            }

            RECT textRc = itemRc;
            textRc.left += 16;
            textRc.right -= 16;
            DrawTextW(hdc, item.text.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, prevFont);
        if (hFontRegular) DeleteObject(hFontRegular);
        if (hFontBold) DeleteObject(hFontBold);

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
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        // Back Button (←)
        g_hBtnBack = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            12, TAB_BAR_HEIGHT + 8, 28, 28,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_BACK, g_hInstance, nullptr);
        g_originalNavBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnBack, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        // Forward Button (→)
        g_hBtnForward = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            44, TAB_BAR_HEIGHT + 8, 28, 28,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_FORWARD, g_hInstance, nullptr);
        SetWindowLongPtr(g_hBtnForward, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        // Reload Button (↻)
        g_hBtnReload = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            76, TAB_BAR_HEIGHT + 8, 28, 28,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_RELOAD, g_hInstance, nullptr);
        SetWindowLongPtr(g_hBtnReload, GWLP_WNDPROC, (LONG_PTR)NavButtonSubclassProc);

        // Borderless Search/URL Edit
        g_hEdit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            124, TAB_BAR_HEIGHT + 13, 400, 22,
            hWnd, (HMENU)(INT_PTR)IDC_EDIT_URL, g_hInstance, nullptr);

        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(g_hEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search or enter address");
        g_originalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // Search Action Button (Magnifying Glass Pill Button)
        g_hBtnSearch = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            550, TAB_BAR_HEIGHT + 10, 28, 28,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_SEARCH, g_hInstance, nullptr);

        g_originalSearchBtnProc = (WNDPROC)SetWindowLongPtr(g_hBtnSearch, GWLP_WNDPROC, (LONG_PTR)SearchButtonSubclassProc);

        // System Tray Notification Icon setup
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hAppIcon;
        wcscpy_s(g_nid.szTip, L"HAZE Browser");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        CreateNewTab(L"https://www.google.com");
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

        // 1. Chrome / Tab Strip Background: Soft Muted Gray (#DEE1E6)
        RECT tabStripRc{ 0, 0, width, TAB_BAR_HEIGHT };
        HBRUSH stripBrush = CreateSolidBrush(RGB(222, 225, 230));
        FillRect(hdc, &tabStripRc, stripBrush);
        DeleteObject(stripBrush);

        // 2. Toolbar & Navigation Row Background: Pure Clean White (#FFFFFF)
        RECT navRowRc{ 0, TAB_BAR_HEIGHT, width, TOP_BAR_HEIGHT };
        HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(hdc, &navRowRc, whiteBrush);

        HFONT hTabFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hTabBoldFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hCloseFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        SetBkMode(hdc, TRANSPARENT);

        // 3. Render Chrome-style tabs seamlessly connecting down to the white toolbar
        for (size_t i = 0; i < g_tabs.size(); ++i) {
            RECT tabRc = GetTabRect((int)i, width);
            bool isActive = ((int)i == g_activeTabIndex);
            bool isHover = ((int)i == g_hoveredTabIndex);

            if (isActive) {
                // Active Tab: Pure White, seamlessly connects into toolbar with rounded top
                HBRUSH activeBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
                HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ ob = SelectObject(hdc, activeBrush);
                HGDIOBJ op = SelectObject(hdc, nullPen);

                // Rounded top rectangle
                RoundRect(hdc, tabRc.left, tabRc.top, tabRc.right, tabRc.bottom + 6, 14, 14);

                SelectObject(hdc, ob);
                SelectObject(hdc, op);
            }
            else if (isHover) {
                // Inactive hover tab
                HBRUSH hoverBrush = CreateSolidBrush(RGB(232, 235, 239));
                HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ ob = SelectObject(hdc, hoverBrush);
                HGDIOBJ op = SelectObject(hdc, nullPen);

                RoundRect(hdc, tabRc.left, tabRc.top, tabRc.right, tabRc.bottom, 12, 12);

                SelectObject(hdc, ob);
                SelectObject(hdc, op);
                DeleteObject(hoverBrush);
            }

            // Draw Favicon / Logo (small HAZE icon next to title)
            if (g_hAppIcon) {
                DrawIconEx(hdc, tabRc.left + 12, tabRc.top + 7, g_hAppIcon, 16, 16, 0, nullptr, DI_NORMAL);
            }

            // Draw Tab Title Text
            RECT titleRc = tabRc;
            titleRc.left += 34;
            titleRc.right -= 28;
            titleRc.bottom -= 2;

            HGDIOBJ oldFont = SelectObject(hdc, isActive ? hTabBoldFont : hTabFont);
            SetTextColor(hdc, isActive ? RGB(32, 33, 36) : RGB(95, 99, 104));
            DrawTextW(hdc, g_tabs[i]->title.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, oldFont);

            // Draw Tab Close Button "×"
            RECT closeRc = GetTabCloseRect(tabRc);
            bool isCloseHover = ((int)i == g_hoveredCloseTabIndex);
            if (isCloseHover) {
                HBRUSH closeBg = CreateSolidBrush(RGB(228, 231, 236));
                HPEN np = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ ob = SelectObject(hdc, closeBg);
                HGDIOBJ op = SelectObject(hdc, np);
                RoundRect(hdc, closeRc.left, closeRc.top, closeRc.right, closeRc.bottom, 4, 4);
                SelectObject(hdc, ob);
                SelectObject(hdc, op);
                DeleteObject(closeBg);
            }

            HGDIOBJ ocFont = SelectObject(hdc, hCloseFont);
            SetTextColor(hdc, isCloseHover ? RGB(217, 48, 37) : RGB(95, 99, 104));
            DrawTextW(hdc, L"×", -1, &closeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, ocFont);
        }

        // Draw "+" New Tab Button
        RECT newTabRc = GetNewTabBtnRect(width);
        if (g_isHoveringNewTab) {
            HBRUSH ntBrush = CreateSolidBrush(RGB(230, 233, 238));
            HPEN np = (HPEN)GetStockObject(NULL_PEN);
            HGDIOBJ ob = SelectObject(hdc, ntBrush);
            HGDIOBJ op = SelectObject(hdc, np);
            RoundRect(hdc, newTabRc.left, newTabRc.top, newTabRc.right, newTabRc.bottom, 6, 6);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(ntBrush);
        }
        HGDIOBJ onFont = SelectObject(hdc, hTabBoldFont);
        SetTextColor(hdc, RGB(60, 64, 67));
        DrawTextW(hdc, L"+", -1, &newTabRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, onFont);

        // 4. Draw Pill-Shaped Rounded Search Bar with Blue Accent Border (exact matching uploaded image)
        RECT searchRc = GetSearchBarOuterRect(width);
        COLORREF borderColor = g_isEditFocused ? RGB(26, 115, 232) : RGB(66, 133, 244); // Google vibrant blue
        HPEN borderPen = CreatePen(PS_SOLID, 2, borderColor);
        HBRUSH fillBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);

        HGDIOBJ osb = SelectObject(hdc, fillBrush);
        HGDIOBJ osp = SelectObject(hdc, borderPen);

        RoundRect(hdc, searchRc.left, searchRc.top, searchRc.right, searchRc.bottom, SEARCH_BAR_HEIGHT, SEARCH_BAR_HEIGHT);

        SelectObject(hdc, osb);
        SelectObject(hdc, osp);
        DeleteObject(borderPen);

        // 5. Divider Line separating top bar from WebView
        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(228, 231, 236));
        HGDIOBJ olp = SelectObject(hdc, linePen);
        MoveToEx(hdc, 0, TOP_BAR_HEIGHT - 1, nullptr);
        LineTo(hdc, width, TOP_BAR_HEIGHT - 1);
        SelectObject(hdc, olp);
        DeleteObject(linePen);

        DeleteObject(hTabFont);
        DeleteObject(hTabBoldFont);
        DeleteObject(hCloseFont);

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
                CreateNewTab(L"https://www.google.com");
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

    INITCOMMONCONTROLSEX icex{ sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_hAppIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_HAZE_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!g_hAppIcon) {
        g_hAppIcon = (HICON)LoadImageW(nullptr, L"app.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }

    SetupMenuItems();

    // Register WinUI3 Menu Window Class
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

    // Register Main Window Class
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
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
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

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return (int)msg.wParam;
}
