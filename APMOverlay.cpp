#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ============================================================
// SETTINGS
// ============================================================

static const wchar_t* APP_NAME = L"APM Overlay";

static int overlayX = 20;
static int overlayY = 20;
static int overlayWidth = 150;
static int overlayHeight = 45;

static const UINT REFRESH_TIMER = 1001;
static const UINT REFRESH_MS = 1000;

static const int MAX_KEY_REPEATS = 5;

// ============================================================
// GLOBALS
// ============================================================

static HWND hwndOverlay = nullptr;

static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static bool clickableMode = false;

static NOTIFYICONDATAW nid{};

static CRITICAL_SECTION dataLock;

static std::vector<ULONGLONG> actions;

static std::map<std::wstring, int> buttonCounts;
static std::map<std::wstring, int> keyRepeatCounts;

static std::vector<std::wstring> timelineLines;

// ============================================================
// SETTINGS FILES
// ============================================================

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring result(path);

    size_t slash = result.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        result.resize(slash);

    return result;
}

static std::wstring GetSettingsPath()
{
    return GetExeDirectory() + L"\\APMOverlay.txt";
}

static std::wstring GetInputsPath()
{
    return GetExeDirectory() + L"\\Inputs.txt";
}

// ============================================================
// CLOCK
// ============================================================

static std::wstring GetClockTime()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    int hour = st.wHour;
    const wchar_t* suffix = L"AM";

    if (hour >= 12)
    {
        suffix = L"PM";

        if (hour > 12)
            hour -= 12;
    }

    if (hour == 0)
        hour = 12;

    wchar_t buffer[128];

    swprintf_s(
        buffer,
        L"%02d:%02d:%02d.%03d %s",
        hour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        suffix
    );

    return buffer;
}

// ============================================================
// SAVE / LOAD WINDOW SETTINGS
// ============================================================

static void SaveWindowSettings()
{
    if (!hwndOverlay)
        return;

    RECT rc{};

    if (!GetWindowRect(hwndOverlay, &rc))
        return;

    overlayX = rc.left;
    overlayY = rc.top;
    overlayWidth = rc.right - rc.left;
    overlayHeight = rc.bottom - rc.top;

    std::wofstream file(GetSettingsPath());

    if (!file)
        return;

    file << L"X=" << overlayX << L"\n";
    file << L"Y=" << overlayY << L"\n";
    file << L"Width=" << overlayWidth << L"\n";
    file << L"Height=" << overlayHeight << L"\n";
}

static void LoadWindowSettings()
{
    std::wifstream file(GetSettingsPath());

    if (!file)
        return;

    std::wstring line;

    while (std::getline(file, line))
    {
        size_t eq = line.find(L'=');

        if (eq == std::wstring::npos)
            continue;

        std::wstring key = line.substr(0, eq);
        std::wstring value = line.substr(eq + 1);

        int number = _wtoi(value.c_str());

        if (key == L"X")
            overlayX = number;
        else if (key == L"Y")
            overlayY = number;
        else if (key == L"Width")
            overlayWidth = number;
        else if (key == L"Height")
            overlayHeight = number;
    }

    if (overlayWidth < 80)
        overlayWidth = 80;

    if (overlayHeight < 30)
        overlayHeight = 30;
}

// ============================================================
// INPUT FILE
// ============================================================

static void WriteInputsFile()
{
    std::wofstream file(GetInputsPath());

    if (!file)
        return;

    file << L"=== BUTTON PRESS COUNTS ===\n\n";

    for (const auto& pair : buttonCounts)
    {
        file << pair.first << L" = " << pair.second << L"\n";
    }

    file << L"\n";
    file << L"=== INPUT TIMELINE ===\n\n";

    for (const auto& line : timelineLines)
    {
        file << line << L"\n";
    }
}

// ============================================================
// RESET
// ============================================================

static void ResetAPM()
{
    EnterCriticalSection(&dataLock);

    actions.clear();

    LeaveCriticalSection(&dataLock);

    InvalidateRect(hwndOverlay, nullptr, TRUE);
}

static void ResetInputTracker()
{
    EnterCriticalSection(&dataLock);

    buttonCounts.clear();
    keyRepeatCounts.clear();
    timelineLines.clear();

    LeaveCriticalSection(&dataLock);

    WriteInputsFile();

    InvalidateRect(hwndOverlay, nullptr, TRUE);
}

// ============================================================
// INPUT NAME
// ============================================================

static std::wstring GetKeyName(DWORD vk)
{
    switch (vk)
    {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK: return L"Backspace";
    case VK_SHIFT: return L"Shift";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RSHIFT: return L"Right Shift";
    case VK_CONTROL: return L"Ctrl";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_MENU: return L"Alt";
    case VK_LMENU: return L"Left Alt";
    case VK_RMENU: return L"Right Alt";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LWIN: return L"Left Win";
    case VK_RWIN: return L"Right Win";
    }

    if (vk >= VK_F1 && vk <= VK_F24)
    {
        wchar_t buffer[32];

        swprintf_s(
            buffer,
            L"F%d",
            vk - VK_F1 + 1
        );

        return buffer;
    }

    if (vk >= 'A' && vk <= 'Z')
    {
        wchar_t buffer[2] =
        {
            static_cast<wchar_t>(vk),
            L'\0'
        };

        return buffer;
    }

    if (vk >= '0' && vk <= '9')
    {
        wchar_t buffer[2] =
        {
            static_cast<wchar_t>(vk),
            L'\0'
        };

        return buffer;
    }

    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);

    if (scanCode != 0)
    {
        wchar_t name[128]{};

        LONG lParam =
            static_cast<LONG>(scanCode << 16);

        if (GetKeyNameTextW(
            lParam,
            name,
            128
        ))
        {
            return name;
        }
    }

    wchar_t buffer[32];

    swprintf_s(
        buffer,
        L"VK_%u",
        vk
    );

    return buffer;
}

// ============================================================
// RECORD INPUT
// ============================================================

static void RecordInput(
    const std::wstring& name,
    bool isDown
)
{
    ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&dataLock);

    if (isDown)
    {
        actions.push_back(now);

        buttonCounts[name]++;

        timelineLines.push_back(
            GetClockTime() + L"\t" + name
        );
    }
    else
    {
        timelineLines.push_back(
            GetClockTime() + L"\t" + name + L"_UP"
        );
    }

    LeaveCriticalSection(&dataLock);

    WriteInputsFile();

    InvalidateRect(hwndOverlay, nullptr, FALSE);
}

// ============================================================
// KEYBOARD HOOK
// ============================================================

static LRESULT CALLBACK KeyboardProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* data =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (data)
        {
            DWORD vk = data->vkCode;

            if (vk != VK_F8 &&
                vk != VK_F9)
            {
                if (wParam == WM_KEYDOWN ||
                    wParam == WM_SYSKEYDOWN)
                {
                    int& repeatCount =
                        keyRepeatCounts[vk];

                    if (repeatCount < MAX_KEY_REPEATS)
                    {
                        repeatCount++;

                        RecordInput(
                            GetKeyName(vk),
                            true
                        );
                    }
                }
                else if (
                    wParam == WM_KEYUP ||
                    wParam == WM_SYSKEYUP
                )
                {
                    keyRepeatCounts[vk] = 0;

                    RecordInput(
                        GetKeyName(vk),
                        false
                    );
                }
            }
        }
    }

    return CallNextHookEx(
        keyboardHook,
        nCode,
        wParam,
        lParam
    );
}

// ============================================================
// MOUSE HOOK
// ============================================================

static std::wstring MouseName(UINT message)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return L"LMB";

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return L"RMB";

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return L"MMB";

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return L"XMB";
    }

    return L"Mouse";
}

static LRESULT CALLBACK MouseProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        MSLLHOOKSTRUCT* data =
            reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        if (data)
        {
            bool down = false;
            bool up = false;

            switch (wParam)
            {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN:
                down = true;
                break;

            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONUP:
                up = true;
                break;
            }

            if (down || up)
            {
                RecordInput(
                    MouseName(
                        static_cast<UINT>(wParam)
                    ),
                    down
                );
            }
        }
    }

    return CallNextHookEx(
        mouseHook,
        nCode,
        wParam,
        lParam
    );
}

// ============================================================
// APM
// ============================================================

static int GetCurrentAPM()
{
    ULONGLONG now = GetTickCount64();

    int count = 0;

    EnterCriticalSection(&dataLock);

    for (auto it = actions.rbegin();
         it != actions.rend();
         ++it)
    {
        ULONGLONG age = now - *it;

        if (age <= 60000ULL)
            count++;
        else
            break;
    }

    LeaveCriticalSection(&dataLock);

    return count;
}

// ============================================================
// TRAY
// ============================================================

#define ID_TRAY 5000

#define ID_TRAY_CLICKABLE 5001
#define ID_TRAY_RESET_APM 5002
#define ID_TRAY_RESET_INPUT 5003
#define ID_TRAY_EXIT 5004

static void UpdateWindowStyle()
{
    LONG_PTR style =
        GetWindowLongPtrW(
            hwndOverlay,
            GWL_STYLE
        );

    LONG_PTR exStyle =
        GetWindowLongPtrW(
            hwndOverlay,
            GWL_EXSTYLE
        );

    if (clickableMode)
    {
        style |= WS_THICKFRAME;

        exStyle &=
            ~WS_EX_TRANSPARENT;
    }
    else
    {
        style &= ~WS_THICKFRAME;

        exStyle |=
            WS_EX_TRANSPARENT;
    }

    SetWindowLongPtrW(
        hwndOverlay,
        GWL_STYLE,
        style
    );

    SetWindowLongPtrW(
        hwndOverlay,
        GWL_EXSTYLE,
        exStyle
    );

    SetWindowPos(
        hwndOverlay,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );

    InvalidateRect(
        hwndOverlay,
        nullptr,
        TRUE
    );
}

static void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (clickableMode ? MF_CHECKED : 0),
        ID_TRAY_CLICKABLE,
        clickableMode
            ? L"Pass-through Mode"
            : L"Clickable Mode"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_RESET_APM,
        L"Reset APM"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_RESET_INPUT,
        L"Reset Input Tracker"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_EXIT,
        L"Exit"
    );

    POINT point{};

    GetCursorPos(&point);

    SetForegroundWindow(hwndOverlay);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        point.x,
        point.y,
        0,
        hwndOverlay,
        nullptr
    );

    PostMessageW(
        hwndOverlay,
        WM_NULL,
        0,
        0
    );

    DestroyMenu(menu);
}

// ============================================================
// PAINT
// ============================================================

static void PaintOverlay(HDC hdc)
{
    RECT rc{};

    GetClientRect(
        hwndOverlay,
        &rc
    );

    // Transparent black background using color key.
    HBRUSH background =
        CreateSolidBrush(
            RGB(0, 0, 0)
        );

    FillRect(
        hdc,
        &rc,
        background
    );

    DeleteObject(background);

    // Clickable mode resize bar.
    if (clickableMode)
    {
        RECT bar = rc;

        bar.bottom = 6;

        HBRUSH white =
            CreateSolidBrush(
                RGB(255, 255, 255)
            );

        FillRect(
            hdc,
            &bar,
            white
        );

        DeleteObject(white);
    }

    int apm = GetCurrentAPM();

    wchar_t text[64];

    swprintf_s(
        text,
        L"APM %d",
        apm
    );

    HFONT font =
        CreateFontW(
            -28,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Arial"
        );

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                hdc,
                font
            )
        );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    SetTextColor(
        hdc,
        RGB(255, 255, 255)
    );

    RECT textRect = rc;

    if (clickableMode)
        textRect.top += 6;

    DrawTextW(
        hdc,
        text,
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        oldFont
    );

    DeleteObject(font);
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (message)
    {
    case WM_CREATE:
    {
        SetTimer(
            hwnd,
            REFRESH_TIMER,
            REFRESH_MS,
            nullptr
        );

        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == REFRESH_TIMER)
        {
            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );
        }

        return 0;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case ID_TRAY_CLICKABLE:
        {
            clickableMode =
                !clickableMode;

            UpdateWindowStyle();

            return 0;
        }

        case ID_TRAY_RESET_APM:
        {
            ResetAPM();
            return 0;
        }

        case ID_TRAY_RESET_INPUT:
        {
            ResetInputTracker();
            return 0;
        }

        case ID_TRAY_EXIT:
        {
            DestroyWindow(hwnd);
            return 0;
        }
        }

        break;
    }

    case WM_NCHITTEST:
    {
        if (!clickableMode)
            return HTTRANSPARENT;

        POINT pt =
        {
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam)
        };

        ScreenToClient(
            hwnd,
            &pt
        );

        RECT rc{};

        GetClientRect(
            hwnd,
            &rc
        );

        const int grip = 8;

        bool left =
            pt.x < grip;

        bool right =
            pt.x >= rc.right - grip;

        bool top =
            pt.y < grip;

        bool bottom =
            pt.y >= rc.bottom - grip;

        if (top && left)
            return HTTOPLEFT;

        if (top && right)
            return HTTOPRIGHT;

        if (bottom && left)
            return HTBOTTOMLEFT;

        if (bottom && right)
            return HTBOTTOMRIGHT;

        if (top)
            return HTTOP;

        if (bottom)
            return HTBOTTOM;

        if (left)
            return HTLEFT;

        if (right)
            return HTRIGHT;

        // Drag anywhere else.
        return HTCAPTION;
    }

    case WM_SIZE:
    case WM_MOVE:
    {
        SaveWindowSettings();
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};

        HDC hdc =
            BeginPaint(
                hwnd,
                &ps
            );

        PaintOverlay(
            hdc
        );

        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_RBUTTONUP:
    {
        ShowTrayMenu();
        return 0;
    }

    case WM_CLOSE:
    {
        SaveWindowSettings();

        DestroyWindow(hwnd);

        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(
            hwnd,
            REFRESH_TIMER
        );

        if (keyboardHook)
        {
            UnhookWindowsHookEx(
                keyboardHook
            );

            keyboardHook = nullptr;
        }

        if (mouseHook)
        {
            UnhookWindowsHookEx(
                mouseHook
            );

            mouseHook = nullptr;
        }

        Shell_NotifyIconW(
            NIM_DELETE,
            &nid
        );

        PostQuitMessage(0);

        return 0;
    }
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}

// ============================================================
// ENTRY POINT
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int
)
{
    InitializeCriticalSection(
        &dataLock
    );

    LoadWindowSettings();

    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(WNDCLASSEXW);

    wc.hInstance =
        hInstance;

    wc.lpfnWndProc =
        WindowProc;

    wc.lpszClassName =
        L"APMOverlayWindow";

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(BLACK_BRUSH)
        );

    if (!RegisterClassExW(&wc))
    {
        DeleteCriticalSection(
            &dataLock
        );

        return 1;
    }

    DWORD exStyle =
        WS_EX_LAYERED |
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW |
        WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE;

    DWORD style =
        WS_POPUP;

    hwndOverlay =
        CreateWindowExW(
            exStyle,
            wc.lpszClassName,
            APP_NAME,
            style,
            overlayX,
            overlayY,
            overlayWidth,
            overlayHeight,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwndOverlay)
    {
        DeleteCriticalSection(
            &dataLock
        );

        return 1;
    }

    // Black becomes transparent.
    SetLayeredWindowAttributes(
        hwndOverlay,
        RGB(0, 0, 0),
        0,
        LWA_COLORKEY
    );

    // ========================================================
    // TRAY ICON
    // ========================================================

    ZeroMemory(
        &nid,
        sizeof(nid)
    );

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwndOverlay;

    nid.uID =
        ID_TRAY;

    nid.uFlags =
        NIF_ICON |
        NIF_MESSAGE |
        NIF_TIP;

    nid.uCallbackMessage =
        WM_USER + 1;

    nid.hIcon =
        LoadIcon(
            nullptr,
            IDI_APPLICATION
        );

    wcscpy_s(
        nid.szTip,
        L"APM Overlay"
    );

    Shell_NotifyIconW(
        NIM_ADD,
        &nid
    );

    // ========================================================
    // HOOKS
    // ========================================================

    keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardProc,
            nullptr,
            0
        );

    mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseProc,
            nullptr,
            0
        );

    if (!keyboardHook ||
        !mouseHook)
    {
        if (keyboardHook)
            UnhookWindowsHookEx(
                keyboardHook
            );

        if (mouseHook)
            UnhookWindowsHookEx(
                mouseHook
            );

        Shell_NotifyIconW(
            NIM_DELETE,
            &nid
        );

        DestroyWindow(
            hwndOverlay
        );

        DeleteCriticalSection(
            &dataLock
        );

        return 1;
    }

    ShowWindow(
        hwndOverlay,
        SW_SHOW
    );

    UpdateWindow(
        hwndOverlay
    );

    // ========================================================
    // MESSAGE LOOP
    // ========================================================

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0
    ) > 0)
    {
        // Tray callback.
        if (msg.message == WM_USER + 1 &&
            msg.hwnd == hwndOverlay)
        {
            if (msg.lParam == WM_RBUTTONUP)
            {
                ShowTrayMenu();
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(
        &dataLock
    );

    return 0;
}
