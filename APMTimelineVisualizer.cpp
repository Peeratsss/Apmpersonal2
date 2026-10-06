#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================
// Media Foundation GUIDs
// Defined here so mfuuid.h is NOT required.
// ============================================================

static const GUID GUID_MF_MT_MAJOR_TYPE =
{
    0x48eba18e, 0xf8c9, 0x4687,
    { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f }
};

static const GUID GUID_MF_MT_SUBTYPE =
{
    0xf7e34c9a, 0x42e8, 0x4714,
    { 0xb7, 0x4b, 0xcb, 0x29, 0x29, 0x1c, 0xa1, 0x2b }
};

static const GUID GUID_MF_MT_AVG_BITRATE =
{
    0x20332624, 0xfb0d, 0x4d9e,
    { 0xbd, 0xd3, 0x77, 0x4f, 0x9a, 0x6f, 0x4f, 0x42 }
};

static const GUID GUID_MF_MT_INTERLACE_MODE =
{
    0xe2724bb8, 0xe676, 0x4806,
    { 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0x9b, 0x6f, 0x6f }
};

static const GUID GUID_MF_MT_FRAME_SIZE =
{
    0x1652c33d, 0xd6b2, 0x4012,
    { 0xb8, 0x34, 0x72, 0x0f, 0x2a, 0x3b, 0x7e, 0x5f }
};

static const GUID GUID_MF_MT_FRAME_RATE =
{
    0xc459a2e8, 0x3d2c, 0x4e44,
    { 0xb1, 0x32, 0xfe, 0x2f, 0x3a, 0x7f, 0x9f, 0x7d }
};

static const GUID GUID_MF_MT_PIXEL_ASPECT_RATIO =
{
    0xc6376a1e, 0x8d0a, 0x4027,
    { 0xbe, 0x45, 0x6d, 0x9a, 0x8f, 0x7e, 0x5a, 0x5c }
};

static const GUID GUID_MFVideoFormat_H264 =
{
    0x34363248, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

static const GUID GUID_MFVideoFormat_RGB32 =
{
    0x00000016, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

static const GUID GUID_MFMediaType_Video =
{
    0x73646976, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

static const GUID GUID_MFVideoInterlace_Progressive =
{
    0xcbf1e3f5, 0x70e4, 0x4b3f,
    { 0x98, 0x6d, 0x5e, 0x9f, 0x9a, 0x8f, 0x8e, 0x5a }
};

// ============================================================
// Constants
// ============================================================

static const int WINDOW_WIDTH = 1920;
static const int WINDOW_HEIGHT = 1080;

static const int FPS = 60;
static const double FRAME_TIME = 1.0 / 60.0;

static const int KEYBOARD_X = 250;
static const int KEYBOARD_Y = 360;

static const int KEY_W = 62;
static const int KEY_H = 62;
static const int KEY_GAP = 7;

static const int MOUSE_X = 250;
static const int MOUSE_Y = 680;

static const double FLASH_TIME = 0.12;

// ============================================================
// Structures
// ============================================================

struct InputEvent
{
    double time = 0.0;
    std::wstring key;
    bool down = false;
};

struct ActivePress
{
    double until = 0.0;
    bool held = false;
};

struct ButtonInfo
{
    std::wstring name;
    int x;
    int y;
    int w;
    int h;
};

// ============================================================
// Globals
// ============================================================

static HWND g_hwnd = nullptr;
static HWND g_edit = nullptr;

static std::vector<InputEvent> g_events;

static double g_duration = 0.0;
static double g_currentTime = 0.0;

static bool g_playing = false;

static LARGE_INTEGER g_lastCounter{};
static LARGE_INTEGER g_frequency{};

static std::wstring g_outputPath;

// ============================================================
// Utility
// ============================================================

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0;
    size_t b = s.size();

    while (a < b && iswspace(s[a]))
        ++a;

    while (b > a && iswspace(s[b - 1]))
        --b;

    return s.substr(a, b - a);
}

static std::wstring ToUpper(std::wstring s)
{
    for (wchar_t& c : s)
        c = (wchar_t)towupper(c);

    return s;
}

static bool ParseTimestamp(
    const std::wstring& text,
    double& seconds
)
{
    std::wstring s = Trim(text);

    if (s.empty())
        return false;

    // Normalize tabs and commas into spaces.
    for (wchar_t& c : s)
    {
        if (c == L'\t' || c == L',')
            c = L' ';
    }

    std::wstringstream ss(s);

    std::vector<std::wstring> parts;

    std::wstring p;

    while (ss >> p)
        parts.push_back(p);

    if (parts.size() < 3)
        return false;

    int hour = 0;
    int minute = 0;
    double sec = 0.0;

    wchar_t dummy;

    // Format:
    // 10:46:46.533
    if (parts[0].find(L':') != std::wstring::npos)
    {
        std::wstring timePart = parts[0];

        size_t p1 = timePart.find(L':');
        size_t p2 = timePart.find(L':', p1 + 1);

        if (p1 == std::wstring::npos ||
            p2 == std::wstring::npos)
            return false;

        try
        {
            hour = std::stoi(timePart.substr(0, p1));
            minute = std::stoi(
                timePart.substr(
                    p1 + 1,
                    p2 - p1 - 1
                )
            );

            sec = std::stod(
                timePart.substr(p2 + 1)
            );
        }
        catch (...)
        {
            return false;
        }
    }
    else
    {
        // Format:
        // 10 46 46.533 PM
        try
        {
            hour = std::stoi(parts[0]);
            minute = std::stoi(parts[1]);
            sec = std::stod(parts[2]);
        }
        catch (...)
        {
            return false;
        }
    }

    bool pm = false;
    bool am = false;

    for (const std::wstring& part : parts)
    {
        std::wstring u = ToUpper(part);

        if (u == L"PM")
            pm = true;

        if (u == L"AM")
            am = true;
    }

    if (hour < 0 || hour > 23)
        return false;

    if (minute < 0 || minute > 59)
        return false;

    if (sec < 0.0 || sec >= 60.0)
        return false;

    if (pm && hour < 12)
        hour += 12;

    if (am && hour == 12)
        hour = 0;

    seconds =
        hour * 3600.0 +
        minute * 60.0 +
        sec;

    return true;
}

static bool ParseTimeline(
    const std::wstring& text
)
{
    g_events.clear();
    g_duration = 0.0;
    g_currentTime = 0.0;

    std::wstringstream stream(text);
    std::wstring line;

    double firstTimestamp = -1.0;
    double lastTimestamp = 0.0;

    while (std::getline(stream, line))
    {
        line = Trim(line);

        if (line.empty())
            continue;

        double timestamp = 0.0;

        std::wstring input;

        // Find first whitespace separating timestamp/input.
        size_t separator = line.find_first_of(L" \t");

        if (separator == std::wstring::npos)
            continue;

        std::wstring timeText =
            Trim(line.substr(0, separator));

        input =
            Trim(line.substr(separator));

        // Handle "10 46 46.533 PM    1"
        if (timeText.find(L':') == std::wstring::npos)
        {
            std::wstringstream parts(line);

            std::wstring a, b, c, d, key;

            if (!(parts >> a >> b >> c))
                continue;

            std::wstring maybeAMPM;

            parts >> maybeAMPM;

            if (maybeAMPM == L"AM" ||
                maybeAMPM == L"PM" ||
                maybeAMPM == L"am" ||
                maybeAMPM == L"pm")
            {
                if (!(parts >> key))
                    continue;

                std::wstring timestampText =
                    a + L" " +
                    b + L" " +
                    c + L" " +
                    maybeAMPM;

                if (!ParseTimestamp(
                        timestampText,
                        timestamp))
                    continue;

                input = key;
            }
            else
            {
                // This was actually:
                // HH MM SS INPUT
                std::wstring timestampText =
                    a + L" " +
                    b + L" " +
                    c;

                if (!ParseTimestamp(
                        timestampText,
                        timestamp))
                    continue;

                input = maybeAMPM;

                if (input.empty())
                    continue;
            }
        }
        else
        {
            if (!ParseTimestamp(
                    timeText,
                    timestamp))
            {
                // Try the entire beginning of the line.
                std::wstringstream parts(line);

                std::wstring t;
                parts >> t;

                if (!ParseTimestamp(t, timestamp))
                    continue;
            }
        }

        if (input.empty())
            continue;

        if (firstTimestamp < 0.0)
            firstTimestamp = timestamp;

        double relative =
            timestamp - firstTimestamp;

        if (relative < 0.0)
            relative = 0.0;

        InputEvent event;

        event.time = relative;
        event.key = ToUpper(Trim(input));

        if (event.key.empty())
            continue;

        event.down =
            event.key.size() < 3 ||
            event.key.substr(
                event.key.size() - 3
            ) != L"_UP";

        g_events.push_back(event);

        lastTimestamp = relative;
    }

    if (g_events.empty())
        return false;

    std::sort(
        g_events.begin(),
        g_events.end(),
        [](const InputEvent& a, const InputEvent& b)
        {
            return a.time < b.time;
        }
    );

    g_duration =
        std::max(
            0.1,
            lastTimestamp + 0.5
        );

    return true;
}

// ============================================================
// Keyboard layout
// ============================================================

static std::vector<ButtonInfo> BuildKeyboard()
{
    std::vector<ButtonInfo> keys;

    const std::wstring rows[] =
    {
        L"1234567890",
        L"QWERTYUIOP",
        L"ASDFGHJKL",
        L"ZXCVBNM"
    };

    int y = KEYBOARD_Y;

    for (int r = 0; r < 4; ++r)
    {
        int x = KEYBOARD_X;

        if (r == 1)
            x += 28;

        if (r == 2)
            x += 56;

        if (r == 3)
            x += 84;

        for (wchar_t c : rows[r])
        {
            ButtonInfo b;

            b.name = std::wstring(1, c);
            b.x = x;
            b.y = y;
            b.w = KEY_W;
            b.h = KEY_H;

            keys.push_back(b);

            x += KEY_W + KEY_GAP;
        }

        y += KEY_H + KEY_GAP;
    }

    // Special keys.
    keys.push_back(
        { L"TAB", KEYBOARD_X - 65, KEYBOARD_Y + KEY_H + KEY_GAP,
          58, KEY_H }
    );

    keys.push_back(
        { L"SHIFT", KEYBOARD_X - 95,
          KEYBOARD_Y + 2 * (KEY_H + KEY_GAP),
          85, KEY_H }
    );

    keys.push_back(
        { L"CTRL", KEYBOARD_X - 95,
          KEYBOARD_Y + 3 * (KEY_H + KEY_GAP),
          85, KEY_H }
    );

    keys.push_back(
        { L"SPACE",
          KEYBOARD_X + 200,
          KEYBOARD_Y + 4 * (KEY_H + KEY_GAP),
          430, KEY_H }
    );

    return keys;
}

static const std::vector<ButtonInfo>& GetKeyboard()
{
    static std::vector<ButtonInfo> keyboard =
        BuildKeyboard();

    return keyboard;
}

// ============================================================
// Event matching
// ============================================================

static bool IsUpEvent(const std::wstring& key)
{
    if (key.size() < 3)
        return false;

    return key.substr(
        key.size() - 3
    ) == L"_UP";
}

static std::wstring BaseKey(
    const std::wstring& key
)
{
    if (IsUpEvent(key))
    {
        return key.substr(
            0,
            key.size() - 3
        );
    }

    return key;
}

static bool IsMouseKey(
    const std::wstring& key
)
{
    return
        key == L"LMB" ||
        key == L"RMB" ||
        key == L"MMB" ||
        key == L"X1" ||
        key == L"X2";
}

// ============================================================
// Drawing
// ============================================================

static void DrawTextCentered(
    HDC hdc,
    const std::wstring& text,
    RECT rc,
    int size,
    int weight = FW_BOLD
)
{
    HFONT font =
        CreateFontW(
            -size,
            0,
            0,
            0,
            weight,
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

    HFONT old =
        (HFONT)SelectObject(hdc, font);

    SetBkMode(hdc, TRANSPARENT);

    DrawTextW(
        hdc,
        text.c_str(),
        -1,
        &rc,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(hdc, old);
    DeleteObject(font);
}

static void FillRectSimple(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    HBRUSH brush
)
{
    RECT rc
    {
        x,
        y,
        x + w,
        y + h
    };

    FillRect(hdc, &rc, brush);
}

static bool IsPressedAtTime(
    const std::wstring& key,
    double time
)
{
    bool pressed = false;

    for (const InputEvent& event : g_events)
    {
        if (event.time > time)
            break;

        std::wstring base =
            BaseKey(event.key);

        if (base != key)
            continue;

        if (event.down)
            pressed = true;
        else
            pressed = false;
    }

    return pressed;
}

static bool IsFlashingAtTime(
    const std::wstring& key,
    double time
)
{
    for (auto it = g_events.rbegin();
         it != g_events.rend();
         ++it)
    {
        if (it->time > time)
            continue;

        if (time - it->time > FLASH_TIME)
            break;

        if (BaseKey(it->key) == key &&
            it->down)
        {
            return true;
        }
    }

    return false;
}

static int CalculateAPM(
    double time
)
{
    int count = 0;

    double start =
        std::max(
            0.0,
            time - 60.0
        );

    for (const InputEvent& event : g_events)
    {
        if (event.time > time)
            break;

        if (event.time >= start &&
            event.down)
        {
            ++count;
        }
    }

    return count;
}

static void DrawKeyboard(
    HDC hdc,
    double time
)
{
    for (const ButtonInfo& key :
         GetKeyboard())
    {
        bool held =
            IsPressedAtTime(
                key.name,
                time
            );

        bool flash =
            IsFlashingAtTime(
                key.name,
                time
            );

        HBRUSH brush =
            CreateSolidBrush(
                held || flash
                    ? RGB(230, 135, 30)
                    : RGB(42, 42, 48)
            );

        FillRectSimple(
            hdc,
            key.x,
            key.y,
            key.w,
            key.h,
            brush
        );

        DeleteObject(brush);

        HPEN pen =
            CreatePen(
                PS_SOLID,
                2,
                RGB(100, 100, 108)
            );

        HPEN oldPen =
            (HPEN)SelectObject(hdc, pen);

        HBRUSH oldBrush =
            (HBRUSH)SelectObject(
                hdc,
                GetStockObject(NULL_BRUSH)
            );

        Rectangle(
            hdc,
            key.x,
            key.y,
            key.x + key.w,
            key.y + key.h
        );

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);

        DeleteObject(pen);

        RECT textRc
        {
            key.x,
            key.y,
            key.x + key.w,
            key.y + key.h
        };

        SetTextColor(
            hdc,
            RGB(245, 245, 245)
        );

        DrawTextCentered(
            hdc,
            key.name,
            textRc,
            20
        );
    }
}

static void DrawMouse(
    HDC hdc,
    double time
)
{
    int x = MOUSE_X;
    int y = MOUSE_Y;

    const int mw = 105;
    const int mh = 155;

    bool lmb =
        IsPressedAtTime(
            L"LMB",
            time
        );

    bool rmb =
        IsPressedAtTime(
            L"RMB",
            time
        );

    HBRUSH body =
        CreateSolidBrush(
            RGB(48, 48, 55)
        );

    RECT mouseRc
    {
        x,
        y,
        x + mw,
        y + mh
    };

    FillRect(
        hdc,
        &mouseRc,
        body
    );

    DeleteObject(body);

    HPEN pen =
        CreatePen(
            PS_SOLID,
            3,
            RGB(130, 130, 140)
        );

    HPEN oldPen =
        (HPEN)SelectObject(
            hdc,
            pen
        );

    HBRUSH oldBrush =
        (HBRUSH)SelectObject(
            hdc,
            GetStockObject(NULL_BRUSH)
        );

    RoundRect(
        hdc,
        x,
        y,
        x + mw,
        y + mh,
        28,
        28
    );

    SelectObject(
        hdc,
        oldBrush
    );

    SelectObject(
        hdc,
        oldPen
    );

    DeleteObject(pen);

    // Left button.
    HBRUSH lb =
        CreateSolidBrush(
            lmb
                ? RGB(230, 135, 30)
                : RGB(65, 65, 72)
        );

    FillRectSimple(
        hdc,
        x + 5,
        y + 8,
        45,
        62,
        lb
    );

    DeleteObject(lb);

    // Right button.
    HBRUSH rb =
        CreateSolidBrush(
            rmb
                ? RGB(230, 135, 30)
                : RGB(65, 65, 72)
        );

    FillRectSimple(
        hdc,
        x + 55,
        y + 8,
        45,
        62,
        rb
    );

    DeleteObject(rb);

    RECT ltext
    {
        x + 5,
        y + 15,
        x + 50,
        y + 62
    };

    RECT rtext
    {
        x + 55,
        y + 15,
        x + 100,
        y + 62
    };

    SetTextColor(
        hdc,
        RGB(240, 240, 240)
    );

    DrawTextCentered(
        hdc,
        L"LMB",
        ltext,
        14
    );

    DrawTextCentered(
        hdc,
        L"RMB",
        rtext,
        14
    );

    RECT wheel
    {
        x + 43,
        y + 75,
        x + 62,
        y + 110
    };

    HBRUSH wb =
        CreateSolidBrush(
            RGB(100, 100, 110)
        );

    FillRect(
        hdc,
        &wheel,
        wb
    );

    DeleteObject(wb);
}

static void DrawProgress(
    HDC hdc,
    double time
)
{
    int x = 180;
    int y = 910;
    int w = 1560;
    int h = 12;

    HBRUSH bg =
        CreateSolidBrush(
            RGB(45, 45, 50)
        );

    FillRectSimple(
        hdc,
        x,
        y,
        w,
        h,
        bg
    );

    DeleteObject(bg);

    double progress = 0.0;

    if (g_duration > 0.0)
        progress =
            time / g_duration;

    progress =
        std::max(
            0.0,
            std::min(
                1.0,
                progress
            )
        );

    HBRUSH fg =
        CreateSolidBrush(
            RGB(230, 135, 30)
        );

    FillRectSimple(
        hdc,
        x,
        y,
        (int)(w * progress),
        h,
        fg
    );

    DeleteObject(fg);
}

static void PaintScene(
    HDC hdc
)
{
    RECT client;

    GetClientRect(
        g_hwnd,
        &client
    );

    HBRUSH bg =
        CreateSolidBrush(
            RGB(18, 18, 22)
        );

    FillRect(
        hdc,
        &client,
        bg
    );

    DeleteObject(bg);

    // APM.
    int apm =
        CalculateAPM(
            g_currentTime
        );

    RECT apmRc
    {
        0,
        45,
        WINDOW_WIDTH,
        160
    };

    SetTextColor(
        hdc,
        RGB(245, 245, 245)
    );

    DrawTextCentered(
        hdc,
        L"APM " +
            std::to_wstring(apm),
        apmRc,
        72
    );

    // Elapsed time.
    int totalMs =
        (int)std::round(
            g_currentTime * 1000.0
        );

    int minutes =
        totalMs / 60000;

    int seconds =
        (totalMs / 1000) % 60;

    int millis =
        totalMs % 1000;

    wchar_t elapsed[64];

    swprintf_s(
        elapsed,
        L"%02d:%02d.%03d",
        minutes,
        seconds,
        millis
    );

    RECT timeRc
    {
        0,
        180,
        WINDOW_WIDTH,
        235
    };

    SetTextColor(
        hdc,
        RGB(175, 175, 185)
    );

    DrawTextCentered(
        hdc,
        elapsed,
        timeRc,
        28,
        FW_NORMAL
    );

    DrawKeyboard(
        hdc,
        g_currentTime
    );

    DrawMouse(
        hdc,
        g_currentTime
    );

    DrawProgress(
        hdc,
        g_currentTime
    );
}

// ============================================================
// Timeline edit control
// ============================================================

static void CreateTimelineEdit()
{
    g_edit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_MULTILINE |
            ES_AUTOVSCROLL |
            ES_AUTOHSCROLL |
            WS_VSCROLL |
            WS_HSCROLL,
            10,
            10,
            600,
            180,
            g_hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );

    SendMessageW(
        g_edit,
        WM_SETFONT,
        (WPARAM)GetStockObject(
            DEFAULT_GUI_FONT
        ),
        TRUE
    );

    SetWindowTextW(
        g_edit,
        L"Paste your input timeline here.\r\n\r\n"
        L"Example:\r\n"
        L"10:46:46.533 PM    W\r\n"
        L"10:46:46.638 PM    W_UP\r\n"
        L"10:46:46.654 PM    2\r\n"
        L"10:46:46.734 PM    2_UP"
    );
}

// ============================================================
// Buttons
// ============================================================

#define ID_LOAD       1001
#define ID_PLAY       1002
#define ID_RESET      1003
#define ID_EXPORT     1004

static void CreateButtons()
{
    CreateWindowW(
        L"BUTTON",
        L"Load Timeline",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        625,
        10,
        145,
        35,
        g_hwnd,
        (HMENU)ID_LOAD,
        GetModuleHandleW(nullptr),
        nullptr
    );

    CreateWindowW(
        L"BUTTON",
        L"Play",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        780,
        10,
        100,
        35,
        g_hwnd,
        (HMENU)ID_PLAY,
        GetModuleHandleW(nullptr),
        nullptr
    );

    CreateWindowW(
        L"BUTTON",
        L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        890,
        10,
        100,
        35,
        g_hwnd,
        (HMENU)ID_RESET,
        GetModuleHandleW(nullptr),
        nullptr
    );

    CreateWindowW(
        L"BUTTON",
        L"Export MP4",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        1000,
        10,
        120,
        35,
        g_hwnd,
        (HMENU)ID_EXPORT,
        GetModuleHandleW(nullptr),
        nullptr
    );
}

// ============================================================
// MP4 Export
// ============================================================

static HRESULT SetAttributeSize(
    IMFMediaType* type,
    REFGUID guid,
    UINT32 width,
    UINT32 height
)
{
    ULONGLONG value =
        ((ULONGLONG)width << 32) |
        height;

    return type->SetUINT64(
        guid,
        value
    );
}

static HRESULT SetAttributeRatio(
    IMFMediaType* type,
    REFGUID guid,
    UINT32 numerator,
    UINT32 denominator
)
{
    ULONGLONG value =
        ((ULONGLONG)numerator << 32) |
        denominator;

    return type->SetUINT64(
        guid,
        value
    );
}

static bool ExportMP4()
{
    if (g_events.empty())
    {
        MessageBoxW(
            g_hwnd,
            L"Load a timeline first.",
            L"Export MP4",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    WCHAR exePath[MAX_PATH];

    GetModuleFileNameW(
        nullptr,
        exePath,
        MAX_PATH
    );

    std::wstring path =
        exePath;

    size_t slash =
        path.find_last_of(
            L"\\/"
        );

    if (slash != std::wstring::npos)
        path =
            path.substr(
                0,
                slash + 1
            );
    else
        path.clear();

    path +=
        L"APM_Replay.mp4";

    IMFAttributes* attributes =
        nullptr;

    IMFMediaType* outputType =
        nullptr;

    IMFMediaType* inputType =
        nullptr;

    IMFSinkWriter* writer =
        nullptr;

    DWORD streamIndex = 0;

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
        return false;

    do
    {
        hr =
            MFCreateAttributes(
                &attributes,
                1
            );

        if (FAILED(hr))
            break;

        hr =
            attributes->SetUINT32(
                MF_SINK_WRITER_DISABLE_THROTTLING,
                TRUE
            );

        if (FAILED(hr))
            break;

        hr =
            MFCreateMediaType(
                &outputType
            );

        if (FAILED(hr))
            break;

        outputType->SetGUID(
            GUID_MF_MT_MAJOR_TYPE,
            GUID_MFMediaType_Video
        );

        outputType->SetGUID(
            GUID_MF_MT_SUBTYPE,
            GUID_MFVideoFormat_H264
        );

        outputType->SetUINT32(
            GUID_MF_MT_AVG_BITRATE,
            12000000
        );

        outputType->SetUINT32(
            GUID_MF_MT_INTERLACE_MODE,
            2
        );

        SetAttributeSize(
            outputType,
            GUID_MF_MT_FRAME_SIZE,
            WINDOW_WIDTH,
            WINDOW_HEIGHT
        );

        SetAttributeRatio(
            outputType,
            GUID_MF_MT_FRAME_RATE,
            FPS,
            1
        );

        SetAttributeRatio(
            outputType,
            GUID_MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );

        hr =
            MFCreateSinkWriterFromURL(
                path.c_str(),
                nullptr,
                attributes,
                &writer
            );

        if (FAILED(hr))
            break;

        hr =
            writer->AddStream(
                outputType,
                &streamIndex
            );

        if (FAILED(hr))
            break;

        hr =
            MFCreateMediaType(
                &inputType
            );

        if (FAILED(hr))
            break;

        inputType->SetGUID(
            GUID_MF_MT_MAJOR_TYPE,
            GUID_MFMediaType_Video
        );

        inputType->SetGUID(
            GUID_MF_MT_SUBTYPE,
            GUID_MFVideoFormat_RGB32
        );

        inputType->SetUINT32(
            GUID_MF_MT_INTERLACE_MODE,
            2
        );

        inputType->SetUINT32(
            GUID_MF_MT_DEFAULT_STRIDE,
            WINDOW_WIDTH * 4
        );

        SetAttributeSize(
            inputType,
            GUID_MF_MT_FRAME_SIZE,
            WINDOW_WIDTH,
            WINDOW_HEIGHT
        );

        SetAttributeRatio(
            inputType,
            GUID_MF_MT_FRAME_RATE,
            FPS,
            1
        );

        SetAttributeRatio(
            inputType,
            GUID_MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );

        hr =
            writer->SetInputMediaType(
                streamIndex,
                inputType,
                nullptr
            );

        if (FAILED(hr))
            break;

        hr =
            writer->BeginWriting();

        if (FAILED(hr))
            break;

        HDC screenDC =
            GetDC(nullptr);

        HDC memDC =
            CreateCompatibleDC(
                screenDC
            );

        BITMAPINFO bmi{};

        bmi.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);

        bmi.bmiHeader.biWidth =
            WINDOW_WIDTH;

        bmi.bmiHeader.biHeight =
            -WINDOW_HEIGHT;

        bmi.bmiHeader.biPlanes =
            1;

        bmi.bmiHeader.biBitCount =
            32;

        bmi.bmiHeader.biCompression =
            BI_RGB;

        void* pixels =
            nullptr;

        HBITMAP bitmap =
            CreateDIBSection(
                screenDC,
                &bmi,
                DIB_RGB_COLORS,
                &pixels,
                nullptr,
                0
            );

        HBITMAP oldBitmap =
            (HBITMAP)SelectObject(
                memDC,
                bitmap
            );

        const LONG bufferSize =
            WINDOW_WIDTH *
            WINDOW_HEIGHT *
            4;

        std::vector<BYTE> frameData(
            bufferSize
        );

        int totalFrames =
            (int)std::ceil(
                g_duration * FPS
            );

        for (int frame = 0;
             frame < totalFrames;
             ++frame)
        {
            double time =
                frame *
                FRAME_TIME;

            g_currentTime =
                std::min(
                    time,
                    g_duration
                );

            PaintScene(
                memDC
            );

            memcpy(
                frameData.data(),
                pixels,
                bufferSize
            );

            IMFSample* sample =
                nullptr;

            IMFMediaBuffer* buffer =
                nullptr;

            hr =
                MFCreateMemoryBuffer(
                    bufferSize,
                    &buffer
                );

            if (FAILED(hr))
                break;

            BYTE* dst =
                nullptr;

            DWORD maxLength = 0;
            DWORD currentLength = 0;

            hr =
                buffer->Lock(
                    &dst,
                    &maxLength,
                    &currentLength
                );

            if (SUCCEEDED(hr))
            {
                memcpy(
                    dst,
                    frameData.data(),
                    bufferSize
                );

                buffer->Unlock();

                buffer->SetCurrentLength(
                    bufferSize
                );

                hr =
                    MFCreateSample(
                        &sample
                    );

                if (SUCCEEDED(hr))
                {
                    hr =
                        sample->AddBuffer(
                            buffer
                        );
                }

                if (SUCCEEDED(hr))
                {
                    LONGLONG sampleTime =
                        (LONGLONG)
                        frame *
                        10000000LL /
                        FPS;

                    LONGLONG sampleDuration =
                        10000000LL /
                        FPS;

                    sample->SetSampleTime(
                        sampleTime
                    );

                    sample->SetSampleDuration(
                        sampleDuration
                    );
                }

                if (SUCCEEDED(hr))
                {
                    hr =
                        writer->WriteSample(
                            streamIndex,
                            sample
                        );
                }
            }

            if (sample)
                sample->Release();

            if (buffer)
                buffer->Release();

            if (FAILED(hr))
                break;

            if ((frame % 60) == 0)
            {
                MSG msg;

                while (
                    PeekMessageW(
                        &msg,
                        nullptr,
                        0,
                        0,
                        PM_REMOVE
                    )
                )
                {
                    TranslateMessage(
                        &msg
                    );

                    DispatchMessageW(
                        &msg
                    );
                }
            }
        }

        SelectObject(
            memDC,
            oldBitmap
        );

        DeleteObject(
            bitmap
        );

        DeleteDC(
            memDC
        );

        ReleaseDC(
            nullptr,
            screenDC
        );

        if (SUCCEEDED(hr))
            hr =
                writer->Finalize();

    }
    while (false);

    if (writer)
        writer->Release();

    if (inputType)
        inputType->Release();

    if (outputType)
        outputType->Release();

    if (attributes)
        attributes->Release();

    MFShutdown();

    g_currentTime = 0.0;

    InvalidateRect(
        g_hwnd,
        nullptr,
        FALSE
    );

    if (FAILED(hr))
    {
        DeleteFileW(
            path.c_str()
        );

        wchar_t message[512];

        swprintf_s(
            message,
            L"MP4 export failed.\n\n"
            L"HRESULT: 0x%08X",
            (unsigned int)hr
        );

        MessageBoxW(
            g_hwnd,
            message,
            L"Export MP4",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    MessageBoxW(
        g_hwnd,
        (L"MP4 exported successfully:\n\n" +
         path).c_str(),
        L"Export MP4",
        MB_OK | MB_ICONINFORMATION
    );

    return true;
}

// ============================================================
// Timeline loading
// ============================================================

static void LoadTimelineFromEditor()
{
    int length =
        GetWindowTextLengthW(
            g_edit
        );

    if (length <= 0)
        return;

    std::wstring text(
        length,
        L'\0'
    );

    GetWindowTextW(
        g_edit,
        &text[0],
        length + 1
    );

    if (!ParseTimeline(text))
    {
        MessageBoxW(
            g_hwnd,
            L"No valid timeline entries were found.",
            L"Load Timeline",
            MB_OK | MB_ICONWARNING
        );

        return;
    }

    g_currentTime = 0.0;
    g_playing = false;

    SetWindowTextW(
        GetDlgItem(
            g_hwnd,
            ID_PLAY
        ),
        L"Play"
    );

    InvalidateRect(
        g_hwnd,
        nullptr,
        FALSE
    );
}

// ============================================================
// Window procedure
// ============================================================

static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        QueryPerformanceFrequency(
            &g_frequency
        );

        QueryPerformanceCounter(
            &g_lastCounter
        );

        CreateTimelineEdit();
        CreateButtons();

        SetTimer(
            hwnd,
            1,
            16,
            nullptr
        );

        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == 1)
        {
            LARGE_INTEGER now;

            QueryPerformanceCounter(
                &now
            );

            double delta =
                (double)(
                    now.QuadPart -
                    g_lastCounter.QuadPart
                ) /
                (double)g_frequency.QuadPart;

            g_lastCounter = now;

            if (g_playing)
            {
                g_currentTime += delta;

                if (g_currentTime >= g_duration)
                {
                    g_currentTime =
                        g_duration;

                    g_playing = false;

                    SetWindowTextW(
                        GetDlgItem(
                            hwnd,
                            ID_PLAY
                        ),
                        L"Play"
                    );
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE
                );
            }
        }

        return 0;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case ID_LOAD:
            LoadTimelineFromEditor();
            return 0;

        case ID_PLAY:
            if (g_events.empty())
                LoadTimelineFromEditor();

            if (!g_events.empty())
            {
                g_playing =
                    !g_playing;

                SetWindowTextW(
                    GetDlgItem(
                        hwnd,
                        ID_PLAY
                    ),
                    g_playing
                        ? L"Pause"
                        : L"Play"
                );

                QueryPerformanceCounter(
                    &g_lastCounter
                );
            }

            return 0;

        case ID_RESET:
            g_currentTime = 0.0;
            g_playing = false;

            SetWindowTextW(
                GetDlgItem(
                    hwnd,
                    ID_PLAY
                ),
                L"Play"
            );

            QueryPerformanceCounter(
                &g_lastCounter
            );

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;

        case ID_EXPORT:
            if (g_events.empty())
                LoadTimelineFromEditor();

            if (!g_events.empty())
                ExportMP4();

            return 0;
        }

        break;
    }

    case WM_SIZE:
    {
        if (g_edit)
        {
            // Keep the editor as a convenient
            // input area at the top.
            MoveWindow(
                g_edit,
                10,
                10,
                600,
                180,
                TRUE
            );
        }

        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        HDC hdc =
            BeginPaint(
                hwnd,
                &ps
            );

        // Scene starts below the editor.
        int saved =
            SaveDC(hdc);

        SetViewportOrgEx(
            hdc,
            0,
            0,
            nullptr
        );

        PaintScene(
            hdc
        );

        RestoreDC(
            hdc,
            saved
        );

        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        KillTimer(
            hwnd,
            1
        );

        PostQuitMessage(
            0
        );

        return 0;
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// ============================================================
// Entry point
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow
)
{
    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"APMTimelineVisualizerWindow";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        (HBRUSH)(
            COLOR_WINDOW + 1
        );

    RegisterClassW(
        &wc
    );

    g_hwnd =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1280,
            900,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!g_hwnd)
        return 1;

    ShowWindow(
        g_hwnd,
        nCmdShow
    );

    UpdateWindow(
        g_hwnd
    );

    MSG msg{};

    while (
        GetMessageW(
            &msg,
            nullptr,
            0,
            0
        ) > 0
    )
    {
        TranslateMessage(
            &msg
        );

        DispatchMessageW(
            &msg
        );
    }

    return (int)msg.wParam;
}
