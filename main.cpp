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
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;

// Constants
constexpr UINT WM_TRAYICON = WM_USER + 100;
constexpr UINT_PTR TRAY_SUBCLASS_ID = 2001;

constexpr int TOOLBAR_HEIGHT = 44;
constexpr int PADDING = 8;
constexpr int BUTTON_WIDTH = 64;

constexpr int IDC_EDIT_URL = 101;
constexpr int IDC_BTN_GO = 102;

constexpr const wchar_t* APP_VERSION = L"v1.0.0";
constexpr const wchar_t* APP_NAME = L"HAZE";

// Globals
HINSTANCE g_hInstance = nullptr;
HWND g_hWnd = nullptr;
HWND g_hEdit = nullptr;
HWND g_hBtnGo = nullptr;
HWND g_hMenuWnd = nullptr;
WNDPROC g_originalEditProc = nullptr;
HICON g_hAppIcon = nullptr;
NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };

wil::com_ptr<ICoreWebView2Controller> g_controller;
wil::com_ptr<ICoreWebView2> g_webview;

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

void NavigateToInput(const std::wstring& input) {
    if (!g_webview) return;

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

    g_webview->Navigate(url.c_str());
}

void ResizeControls(int width, int height) {
    int editWidth = width - (PADDING * 3) - BUTTON_WIDTH;
    if (editWidth < 50) editWidth = 50;

    if (g_hEdit) {
        SetWindowPos(g_hEdit, nullptr, PADDING, PADDING, editWidth, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hBtnGo) {
        SetWindowPos(g_hBtnGo, nullptr, PADDING * 2 + editWidth, PADDING, BUTTON_WIDTH, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_controller) {
        RECT bounds{ 0, TOOLBAR_HEIGHT, width, height };
        g_controller->put_Bounds(bounds);
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

// Subclass edit control to handle Enter key press
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        wchar_t buffer[2048] = { 0 };
        GetWindowTextW(hWnd, buffer, ARRAYSIZE(buffer));
        NavigateToInput(buffer);
        return 0;
    }
    return CallWindowProc(g_originalEditProc, hWnd, uMsg, wParam, lParam);
}

void InitWebView(HWND hWnd) {
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;

                env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            wil::com_ptr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                            }

                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(g_controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                COREWEBVIEW2_COLOR whiteColor{ 255, 255, 255, 255 };
                                controller2->put_DefaultBackgroundColor(whiteColor);
                            }

                            g_webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [](ICoreWebView2* webview, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(webview->get_Source(&uri)) && uri) {
                                            SetWindowTextW(g_hEdit, uri);
                                            CoTaskMemFree(uri);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            ResizeControls(bounds.right - bounds.left, bounds.bottom - bounds.top);
                            g_controller->put_IsVisible(TRUE);

                            g_webview->Navigate(L"https://www.google.com");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
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
        if (g_webview) {
            g_webview->Navigate(L"https://www.google.com/preferences");
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

        // WinUI3 Light Theme background (#FCFCFC / almost white)
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

            // Hover indicator
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

    // Position above tray or adjust within work area
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
        HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        g_hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"https://www.google.com",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            PADDING, PADDING, 500, 28,
            hWnd, (HMENU)(INT_PTR)IDC_EDIT_URL, g_hInstance, nullptr);

        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        g_originalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        g_hBtnGo = CreateWindowExW(
            0, L"BUTTON", L"Search",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            510, PADDING, BUTTON_WIDTH, 28,
            hWnd, (HMENU)(INT_PTR)IDC_BTN_GO, g_hInstance, nullptr);

        SendMessage(g_hBtnGo, WM_SETFONT, (WPARAM)hFont, TRUE);

        // System Tray Notification Icon setup
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hAppIcon;
        wcscpy_s(g_nid.szTip, L"HAZE Browser");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        InitWebView(hWnd);
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDC_BTN_GO) {
            wchar_t buffer[2048] = { 0 };
            GetWindowTextW(g_hEdit, buffer, ARRAYSIZE(buffer));
            NavigateToInput(buffer);
        }
        break;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        ResizeControls(width, height);
        break;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hWnd, &rect);
        HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(hdc, &rect, whiteBrush);
        return 1;
    }
    case WM_CLOSE: {
        // Closing keeps it in the system tray instead of terminating
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

    // Load App Icon from resource or fallback to file
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

    // Create popup menu window
    g_hMenuWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HazeWinUI3MenuClass", L"",
        WS_POPUP,
        0, 0, MENU_WIDTH, CalculateMenuHeight(),
        nullptr, nullptr, hInstance, nullptr);

    // Apply rounded corners (WinUI 3 / Windows 11 style) and shadow
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

    // Create Main Window: Title bar strictly "HAZE"
    g_hWnd = CreateWindowExW(
        0, L"HazeDesktopBrowserClass", APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        return 0;
    }

    // Set icons explicitly for alt-tab, taskbar and titlebar
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
