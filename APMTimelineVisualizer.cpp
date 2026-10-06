#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================
// Media Foundation GUIDs
// ============================================================

static const GUID GUID_MF_MT_MAJOR_TYPE =
{
    0x48eba18e, 0xf8c9, 0x4687,
    {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}
};

static const GUID GUID_MF_MT_SUBTYPE =
{
    0xf7e34c9a, 0x42e8, 0x4714,
    {0xb7, 0x4b, 0xcb, 0x29, 0x29, 0x1c, 0xa1, 0x2b}
};

static const GUID GUID_MF_MT_AVG_BITRATE =
{
    0x20332624, 0xfb0d, 0x4d9e,
    {0xbd, 0xd3, 0x77, 0x4f, 0x9a, 0x6f, 0x4f, 0x42}
};

static const GUID GUID_MF_MT_INTERLACE_MODE =
{
    0xe2724bb8, 0xe676, 0x4806,
    {0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0x9b, 0x6f, 0x6f}
};

static const GUID GUID_MF_MT_FRAME_SIZE =
{
    0x1652c33d, 0xd6b2, 0x4012,
    {0xb8, 0x34, 0x72, 0x0f, 0x2a, 0x3b, 0x7e, 0x5f}
};

static const GUID GUID_MF_MT_FRAME_RATE =
{
    0xc459a2e8, 0x3d2c, 0x4e44,
    {0xb1, 0x32, 0xfe, 0x2f, 0x3a, 0x7f, 0x9f, 0x7d}
};

static const GUID GUID_MF_MT_PIXEL_ASPECT_RATIO =
{
    0xc6376a1e, 0x8d0a, 0x4027,
    {0xbe, 0x45, 0x6d, 0x9a, 0x8f, 0x7e, 0x5a, 0x5c}
};

static const GUID GUID_MFVideoFormat_H264 =
{
    0x34363248, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

static const GUID GUID_MFVideoFormat_RGB32 =
{
    0x00000016, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

static const GUID GUID_MFMediaType_Video =
{
    0x73646976, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

// ============================================================
// Constants
// ============================================================

static const int WINDOW_WIDTH = 1920;
static const int WINDOW_HEIGHT = 1080;

static const int FPS = 60;

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
    double time;
    std::wstring key;
    bool down;
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

// ============================================================
// String helpers
// ============================================================

static std::wstring Trim(
    const std::wstring& value
)
{
    size_t start = 0;
    size_t end = value.size();

    while (
        start < end &&
        iswspace(value[start])
    )
    {
        ++start;
    }

    while (
        end > start &&
        iswspace(value[end - 1])
    )
    {
        --end;
    }

    return value.substr(
        start,
        end - start
    );
}

static std::wstring Upper(
    std::wstring value
)
{
    for (wchar_t& c : value)
        c = (wchar_t)towupper(c);

    return value;
}

// ============================================================
// Timestamp parsing
// ============================================================

static bool ParseTimestamp(
    const std::wstring& timestampText,
    double& seconds
)
{
    std::wstring s =
        Trim(timestampText);

    if (s.empty())
        return false;

    for (wchar_t& c : s)
    {
        if (c == L'\t')
            c = L' ';
    }

    std::wstringstream ss(s);

    std::vector<std::wstring> parts;

    std::wstring part;

    while (ss >> part)
        parts.push_back(part);

    if (parts.size() < 1)
        return false;

    int hour = 0;
    int minute = 0;
    double second = 0.0;

    bool hasAM = false;
    bool hasPM = false;

    for (const std::wstring& p : parts)
    {
        std::wstring u = Upper(p);

        if (u == L"AM")
            hasAM = true;

        if (u == L"PM")
            hasPM = true;
    }

    try
    {
        // HH:MM:SS.mmm
        if (
            parts[0].find(L':') !=
            std::wstring::npos
        )
        {
            std::wstring t =
                parts[0];

            size_t first =
                t.find(L':');

            size_t secondColon =
                t.find(
                    L':',
                    first + 1
                );

            if (
                first == std::wstring::npos ||
                secondColon == std::wstring::npos
            )
            {
                return false;
            }

            hour =
                std::stoi(
                    t.substr(
                        0,
                        first
                    )
                );

            minute =
                std::stoi(
                    t.substr(
                        first + 1,
                        secondColon - first - 1
                    )
                );

            second =
                std::stod(
                    t.substr(
                        secondColon + 1
                    )
                );
        }
        else
        {
            // HH MM SS.mmm
            if (parts.size() < 3)
                return false;

            hour =
                std::stoi(parts[0]);

            minute =
                std::stoi(parts[1]);

            second =
                std::stod(parts[2]);
        }
    }
    catch (...)
    {
        return false;
    }

    if (
        hour < 0 ||
        hour > 23
    )
    {
        return false;
    }

    if (
        minute < 0 ||
        minute > 59
    )
    {
        return false;
    }

    if (
        second < 0.0 ||
        second >= 60.0
    )
    {
        return false;
    }

    if (hasPM && hour < 12)
        hour += 12;

    if (hasAM && hour == 12)
        hour = 0;

    seconds =
        hour * 3600.0 +
        minute * 60.0 +
        second;

    return true;
}

// ============================================================
// Timeline parsing
// ============================================================

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

        // Ignore section headings.
        if (line[0] == L'=')
            continue;

        std::wstringstream ss(line);

        std::vector<std::wstring> tokens;

        std::wstring token;

        while (ss >> token)
            tokens.push_back(token);

        if (tokens.size() < 2)
            continue;

        double timestamp = 0.0;
        std::wstring input;

        // ----------------------------------------------------
        // Format:
        // 10:46:46.533 PM    1
        // ----------------------------------------------------

        if (
            tokens[0].find(L':') !=
            std::wstring::npos
        )
        {
            std::wstring timePart =
                tokens[0];

            std::wstring timestampText =
                timePart;

            size_t index = 1;

            if (
                index < tokens.size() &&
                (
                    Upper(tokens[index]) == L"AM" ||
                    Upper(tokens[index]) == L"PM"
                )
            )
            {
                timestampText +=
                    L" " +
                    tokens[index];

                ++index;
            }

            if (index >= tokens.size())
                continue;

            input =
                tokens[index];

            if (
                !ParseTimestamp(
                    timestampText,
                    timestamp
                )
            )
            {
                continue;
            }
        }

        // ----------------------------------------------------
        // Format:
        // 10 46 46.533 PM    1
        // ----------------------------------------------------

        else
        {
            if (tokens.size() < 4)
                continue;

            std::wstring timestampText =
                tokens[0] +
                L" " +
                tokens[1] +
                L" " +
                tokens[2];

            size_t index = 3;

            if (
                index < tokens.size() &&
                (
                    Upper(tokens[index]) == L"AM" ||
                    Upper(tokens[index]) == L"PM"
                )
            )
            {
                timestampText +=
                    L" " +
                    tokens[index];

                ++index;
            }

            if (index >= tokens.size())
                continue;

            input =
                tokens[index];

            if (
                !ParseTimestamp(
                    timestampText,
                    timestamp
                )
            )
            {
                continue;
            }
        }

        input =
            Upper(
                Trim(input)
            );

        if (input.empty())
            continue;

        if (firstTimestamp < 0.0)
            firstTimestamp = timestamp;

        double relative =
            timestamp -
            firstTimestamp;

        if (relative < 0.0)
            relative = 0.0;

        InputEvent event{};

        event.time = relative;
        event.key = input;
        event.down = true;

        if (
            input.size() >= 3 &&
            input.substr(
                input.size() - 3
            ) == L"_UP"
        )
        {
            event.down = false;
        }

        g_events.push_back(event);

        lastTimestamp = relative;
    }

    if (g_events.empty())
        return false;

    std::sort(
        g_events.begin(),
        g_events.end(),
        [](const InputEvent& a,
           const InputEvent& b)
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
// Keyboard
// ============================================================

static std::vector<ButtonInfo> BuildKeyboard()
{
    std::vector<ButtonInfo> result;

    const std::wstring rows[] =
    {
        L"1234567890",
        L"QWERTYUIOP",
        L"ASDFGHJKL",
        L"ZXCVBNM"
    };

    int y = KEYBOARD_Y;

    for (int row = 0; row < 4; ++row)
    {
        int x =
            KEYBOARD_X +
            row * 28;

        for (wchar_t c : rows[row])
        {
            ButtonInfo button{};

            button.name =
                std::wstring(1, c);

            button.x = x;
            button.y = y;
            button.w = KEY_W;
            button.h = KEY_H;

            result.push_back(button);

            x +=
                KEY_W +
                KEY_GAP;
        }

        y +=
            KEY_H +
            KEY_GAP;
    }

    result.push_back(
        {
            L"TAB",
            KEYBOARD_X - 65,
            KEYBOARD_Y + KEY_H + KEY_GAP,
            58,
            KEY_H
        }
    );

    result.push_back(
        {
            L"SHIFT",
            KEYBOARD_X - 95,
            KEYBOARD_Y + 2 * (KEY_H + KEY_GAP),
            85,
            KEY_H
        }
    );

    result.push_back(
        {
            L"CTRL",
            KEYBOARD_X - 95,
            KEYBOARD_Y + 3 * (KEY_H + KEY_GAP),
            85,
            KEY_H
        }
    );

    result.push_back(
        {
            L"SPACE",
            KEYBOARD_X + 200,
            KEYBOARD_Y + 4 * (KEY_H + KEY_GAP),
            430,
            KEY_H
        }
    );

    return result;
}

static const std::vector<ButtonInfo>&
GetKeyboard()
{
    static std::vector<ButtonInfo> keyboard =
        BuildKeyboard();

    return keyboard;
}

// ============================================================
// Event helpers
// ============================================================

static bool IsUpEvent(
    const std::wstring& key
)
{
    return
        key.size() >= 3 &&
        key.substr(
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

        if (
            BaseKey(event.key) ==
            key
        )
        {
            pressed =
                event.down;
        }
    }

    return pressed;
}

static bool WasRecentlyPressed(
    const std::wstring& key,
    double time
)
{
    for (
        auto it = g_events.rbegin();
        it != g_events.rend();
        ++it
    )
    {
        if (it->time > time)
            continue;

        if (
            time - it->time >
            FLASH_TIME
        )
        {
            break;
        }

        if (
            BaseKey(it->key) ==
                key &&
            it->down
        )
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

        if (
            event.time >= start &&
            event.down
        )
        {
            ++count;
        }
    }

    return count;
}

// ============================================================
// Drawing
// ============================================================

static void DrawCenteredText(
    HDC hdc,
    const std::wstring& text,
    const RECT& rect,
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

    HFONT oldFont =
        (HFONT)SelectObject(
            hdc,
            font
        );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    DrawTextW(
        hdc,
        text.c_str(),
        -1,
        const_cast<RECT*>(&rect),
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

static void DrawKeyboard(
    HDC hdc,
    double time
)
{
    for (
        const ButtonInfo& button :
        GetKeyboard()
    )
    {
        bool pressed =
            IsPressedAtTime(
                button.name,
                time
            );

        bool flash =
            WasRecentlyPressed(
                button.name,
                time
            );

        HBRUSH brush =
            CreateSolidBrush(
                pressed || flash
                    ? RGB(230, 135, 30)
                    : RGB(42, 42, 48)
            );

        RECT rect
        {
            button.x,
            button.y,
            button.x + button.w,
            button.y + button.h
        };

        FillRect(
            hdc,
            &rect,
            brush
        );

        DeleteObject(brush);

        HPEN pen =
            CreatePen(
                PS_SOLID,
                2,
                RGB(100, 100, 110)
            );

        HPEN oldPen =
            (HPEN)SelectObject(
                hdc,
                pen
            );

        HBRUSH oldBrush =
            (HBRUSH)SelectObject(
                hdc,
                GetStockObject(
                    NULL_BRUSH
                )
            );

        Rectangle(
            hdc,
            button.x,
            button.y,
            button.x + button.w,
            button.y + button.h
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

        SetTextColor(
            hdc,
            RGB(245, 245, 245)
        );

        DrawCenteredText(
            hdc,
            button.name,
            rect,
            20
        );
    }
}

static void DrawMouse(
    HDC hdc,
    double time
)
{
    const int x = MOUSE_X;
    const int y = MOUSE_Y;

    const int width = 105;
    const int height = 155;

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

    RECT bodyRect
    {
        x,
        y,
        x + width,
        y + height
    };

    FillRect(
        hdc,
        &bodyRect,
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
            GetStockObject(
                NULL_BRUSH
            )
        );

    RoundRect(
        hdc,
        x,
        y,
        x + width,
        y + height,
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

    HBRUSH leftBrush =
        CreateSolidBrush(
            lmb
                ? RGB(230, 135, 30)
                : RGB(65, 65, 72)
        );

    FillRectSimple:
    {
        RECT r
        {
            x + 5,
            y + 8,
            x + 50,
            y + 70
        };

        FillRect(
            hdc,
            &r,
            leftBrush
        );
    }

    DeleteObject(leftBrush);

    HBRUSH rightBrush =
        CreateSolidBrush(
            rmb
                ? RGB(230, 135, 30)
                : RGB(65, 65, 72)
        );

    {
        RECT r
        {
            x + 55,
            y + 8,
            x + 100,
            y + 70
        };

        FillRect(
            hdc,
            &r,
            rightBrush
        );
    }

    DeleteObject(rightBrush);

    RECT leftText
    {
        x + 5,
        y + 10,
        x + 50,
        y + 68
    };

    RECT rightText
    {
        x + 55,
        y + 10,
        x + 100,
        y + 68
    };

    SetTextColor(
        hdc,
        RGB(240, 240, 240)
    );

    DrawCenteredText(
        hdc,
        L"LMB",
        leftText,
        14
    );

    DrawCenteredText(
        hdc,
        L"RMB",
        rightText,
        14
    );

    RECT wheel
    {
        x + 43,
        y + 78,
        x + 62,
        y + 112
    };

    HBRUSH wheelBrush =
        CreateSolidBrush(
            RGB(100, 100, 110)
        );

    FillRect(
        hdc,
        &wheel,
        wheelBrush
    );

    DeleteObject(wheelBrush);
}

static void DrawProgress(
    HDC hdc,
    double time
)
{
    const int x = 180;
    const int y = 910;
    const int width = 1560;
    const int height = 12;

    HBRUSH background =
        CreateSolidBrush(
            RGB(45, 45, 50)
        );

    RECT backgroundRect
    {
        x,
        y,
        x + width,
        y + height
    };

    FillRect(
        hdc,
        &backgroundRect,
        background
    );

    DeleteObject(background);

    double progress = 0.0;

    if (g_duration > 0.0)
    {
        progress =
            time / g_duration;
    }

    progress =
        std::max(
            0.0,
            std::min(
                1.0,
                progress
            )
        );

    int filled =
        (int)(
            width *
            progress
        );

    if (filled > 0)
    {
        HBRUSH foreground =
            CreateSolidBrush(
                RGB(230, 135, 30)
            );

        RECT foregroundRect
        {
            x,
            y,
            x + filled,
            y + height
        };

        FillRect(
            hdc,
            &foregroundRect,
            foreground
        );

        DeleteObject(foreground);
    }
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

    HBRUSH background =
        CreateSolidBrush(
            RGB(18, 18, 22)
        );

    FillRect(
        hdc,
        &client,
        background
    );

    DeleteObject(background);

    // --------------------------------------------------------
    // APM
    // --------------------------------------------------------

    int apm =
        CalculateAPM(
            g_currentTime
        );

    RECT apmRect
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

    DrawCenteredText(
        hdc,
        L"APM " +
            std::to_wstring(apm),
        apmRect,
        72
    );

    // --------------------------------------------------------
    // Time
    // --------------------------------------------------------

    int totalMilliseconds =
        (int)std::round(
            g_currentTime * 1000.0
        );

    int minutes =
        totalMilliseconds / 60000;

    int seconds =
        (totalMilliseconds / 1000) %
        60;

    int milliseconds =
        totalMilliseconds %
        1000;

    wchar_t timeText[64];

    swprintf_s(
        timeText,
        L"%02d:%02d.%03d",
        minutes,
        seconds,
        milliseconds
    );

    RECT timeRect
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

    DrawCenteredText(
        hdc,
        timeText,
        timeRect,
        28,
        FW_NORMAL
    );

    // --------------------------------------------------------
    // Keyboard
    // --------------------------------------------------------

    DrawKeyboard(
        hdc,
        g_currentTime
    );

    // --------------------------------------------------------
    // Mouse
    // --------------------------------------------------------

    DrawMouse(
        hdc,
        g_currentTime
    );

    // --------------------------------------------------------
    // Progress bar
    // --------------------------------------------------------

    DrawProgress(
        hdc,
        g_currentTime
    );
}

// ============================================================
// Helper for mouse drawing
// ============================================================

static void FillRectSimple(
    HDC hdc,
    int x,
    int y,
    int width,
    int height,
    HBRUSH brush
)
{
    RECT r
    {
        x,
        y,
        x + width,
        y + height
    };

    FillRect(
        hdc,
        &r,
        brush
    );
}

// ============================================================
// Buttons
// ============================================================

#define ID_LOAD       1001
#define ID_PLAY       1002
#define ID_RESET      1003
#define ID_EXPORT     1004

static void CreateTimelineControls()
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

    HFONT font =
        (HFONT)GetStockObject(
            DEFAULT_GUI_FONT
        );

    SendMessageW(
        g_edit,
        WM_SETFONT,
        (WPARAM)font,
        TRUE
    );

    SetWindowTextW(
        g_edit,
        L"Paste your input timeline here.\r\n"
        L"\r\n"
        L"Example:\r\n"
        L"10:46:46.533 PM    1\r\n"
        L"10:46:46.638 PM    2\r\n"
        L"10:46:46.654 PM    1_UP\r\n"
        L"10:46:46.734 PM    2_UP"
    );

    CreateWindowW(
        L"BUTTON",
        L"Load Timeline",
        WS_CHILD |
        WS_VISIBLE |
        BS_PUSHBUTTON,
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
        WS_CHILD |
        WS_VISIBLE |
        BS_PUSHBUTTON,
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
        WS_CHILD |
        WS_VISIBLE |
        BS_PUSHBUTTON,
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
        WS_CHILD |
        WS_VISIBLE |
        BS_PUSHBUTTON,
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
// Load timeline
// ============================================================

static void LoadTimeline()
{
    int length =
        GetWindowTextLengthW(
            g_edit
        );

    if (length <= 0)
        return;

    std::wstring text(
        length + 1,
        L'\0'
    );

    GetWindowTextW(
        g_edit,
        &text[0],
        length + 1
    );

    text.resize(
        wcslen(
            text.c_str()
        )
    );

    if (!ParseTimeline(text))
    {
        MessageBoxW(
            g_hwnd,
            L"No valid timeline entries were found.",
            L"Load Timeline",
            MB_OK |
            MB_ICONWARNING
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
// MP4 export
// ============================================================

static HRESULT SetSizeAttribute(
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

static HRESULT SetRatioAttribute(
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
            MB_OK |
            MB_ICONWARNING
        );

        return false;
    }

    WCHAR modulePath[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        modulePath,
        MAX_PATH
    );

    std::wstring outputPath =
        modulePath;

    size_t slash =
        outputPath.find_last_of(
            L"\\/"
        );

    if (
        slash !=
        std::wstring::npos
    )
    {
        outputPath =
            outputPath.substr(
                0,
                slash + 1
            );
    }
    else
    {
        outputPath.clear();
    }

    outputPath +=
        L"APM_Replay.mp4";

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
        return false;

    IMFAttributes* attributes =
        nullptr;

    IMFMediaType* outputType =
        nullptr;

    IMFMediaType* inputType =
        nullptr;

    IMFSinkWriter* writer =
        nullptr;

    DWORD streamIndex = 0;

    HDC screenDC = nullptr;
    HDC memoryDC = nullptr;

    HBITMAP bitmap = nullptr;
    HBITMAP oldBitmap = nullptr;

    void* pixels = nullptr;

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

        // ----------------------------------------------------
        // H.264 output type
        // ----------------------------------------------------

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

        SetSizeAttribute(
            outputType,
            GUID_MF_MT_FRAME_SIZE,
            WINDOW_WIDTH,
            WINDOW_HEIGHT
        );

        SetRatioAttribute(
            outputType,
            GUID_MF_MT_FRAME_RATE,
            FPS,
            1
        );

        SetRatioAttribute(
            outputType,
            GUID_MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );

        // ----------------------------------------------------
        // Create writer
        // ----------------------------------------------------

        hr =
            MFCreateSinkWriterFromURL(
                outputPath.c_str(),
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

        // ----------------------------------------------------
        // RGB32 input type
        // ----------------------------------------------------

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

        SetSizeAttribute(
            inputType,
            GUID_MF_MT_FRAME_SIZE,
            WINDOW_WIDTH,
            WINDOW_HEIGHT
        );

        SetRatioAttribute(
            inputType,
            GUID_MF_MT_FRAME_RATE,
            FPS,
            1
        );

        SetRatioAttribute(
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

        // ----------------------------------------------------
        // Frame bitmap
        // ----------------------------------------------------

        screenDC =
            GetDC(nullptr);

        memoryDC =
            CreateCompatibleDC(
                screenDC
            );

        BITMAPINFO bitmapInfo{};

        bitmapInfo.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);

        bitmapInfo.bmiHeader.biWidth =
            WINDOW_WIDTH;

        bitmapInfo.bmiHeader.biHeight =
            -WINDOW_HEIGHT;

        bitmapInfo.bmiHeader.biPlanes =
            1;

        bitmapInfo.bmiHeader.biBitCount =
            32;

        bitmapInfo.bmiHeader.biCompression =
            BI_RGB;

        bitmap =
            CreateDIBSection(
                screenDC,
                &bitmapInfo,
                DIB_RGB_COLORS,
                &pixels,
                nullptr,
                0
            );

        if (!bitmap)
        {
            hr =
                E_OUTOFMEMORY;

            break;
        }

        oldBitmap =
            (HBITMAP)SelectObject(
                memoryDC,
                bitmap
            );

        const DWORD bufferSize =
            WINDOW_WIDTH *
            WINDOW_HEIGHT *
            4;

        int totalFrames =
            (int)std::ceil(
                g_duration *
                FPS
            );

        // ----------------------------------------------------
        // Render every frame
        // ----------------------------------------------------

        for (
            int frame = 0;
            frame < totalFrames;
            ++frame
        )
        {
            double time =
                (double)frame /
                (double)FPS;

            g_currentTime =
                std::min(
                    time,
                    g_duration
                );

            PaintScene(
                memoryDC
            );

            IMFMediaBuffer* buffer =
                nullptr;

            IMFSample* sample =
                nullptr;

            hr =
                MFCreateMemoryBuffer(
                    bufferSize,
                    &buffer
                );

            if (FAILED(hr))
                break;

            BYTE* destination =
                nullptr;

            DWORD maxLength = 0;
            DWORD currentLength = 0;

            hr =
                buffer->Lock(
                    &destination,
                    &maxLength,
                    &currentLength
                );

            if (SUCCEEDED(hr))
            {
                memcpy(
                    destination,
                    pixels,
                    bufferSize
                );

                buffer->Unlock();

                hr =
                    buffer->SetCurrentLength(
                        bufferSize
                    );
            }

            if (SUCCEEDED(hr))
            {
                hr =
                    MFCreateSample(
                        &sample
                    );
            }

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
                    (LONGLONG)frame *
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

                hr =
                    writer->WriteSample(
                        streamIndex,
                        sample
                    );
            }

            if (sample)
                sample->Release();

            if (buffer)
                buffer->Release();

            if (FAILED(hr))
                break;
        }

        if (SUCCEEDED(hr))
        {
            hr =
                writer->Finalize();
        }

    }
    while (false);

    if (oldBitmap &&
        memoryDC)
    {
        SelectObject(
            memoryDC,
            oldBitmap
        );
    }

    if (bitmap)
        DeleteObject(bitmap);

    if (memoryDC)
        DeleteDC(memoryDC);

    if (screenDC)
        ReleaseDC(
            nullptr,
            screenDC
        );

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
            outputPath.c_str()
        );

        wchar_t message[256];

        swprintf_s(
            message,
            L"MP4 export failed.\r\n\r\n"
            L"HRESULT: 0x%08X",
            (unsigned int)hr
        );

        MessageBoxW(
            g_hwnd,
            message,
            L"Export MP4",
            MB_OK |
            MB_ICONERROR
        );

        return false;
    }

    MessageBoxW(
        g_hwnd,
        (L"MP4 exported successfully:\r\n\r\n" +
         outputPath).c_str(),
        L"Export MP4",
        MB_OK |
        MB_ICONINFORMATION
    );

    return true;
}

// ============================================================
// Window procedure
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
        QueryPerformanceFrequency(
            &g_frequency
        );

        QueryPerformanceCounter(
            &g_lastCounter
        );

        CreateTimelineControls();

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
                (double)
                    g_frequency.QuadPart;

            g_lastCounter =
                now;

            if (g_playing)
            {
                g_currentTime +=
                    delta;

                if (
                    g_currentTime >=
                    g_duration
                )
                {
                    g_currentTime =
                        g_duration;

                    g_playing =
                        false;

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
            LoadTimeline();
            return 0;

        case ID_PLAY:
        {
            if (g_events.empty())
            {
                LoadTimeline();
            }

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
        }

        case ID_RESET:
        {
            g_currentTime =
                0.0;

            g_playing =
                false;

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
        }

        case ID_EXPORT:
        {
            if (g_events.empty())
            {
                LoadTimeline();
            }

            if (!g_events.empty())
            {
                ExportMP4();
            }

            return 0;
        }
        }

        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        HDC hdc =
            BeginPaint(
                hwnd,
                &ps
            );

        PaintScene(
            hdc
        );

        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }

    case WM_SIZE:
    {
        if (g_edit)
        {
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

    case WM_DESTROY:
    {
        KillTimer(
            hwnd,
            1
        );

        PostQuitMessage(
            0
        );

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
// Entry point
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow
)
{
    WNDCLASSW windowClass{};

    windowClass.lpfnWndProc =
        WindowProc;

    windowClass.hInstance =
        hInstance;

    windowClass.lpszClassName =
        L"APMTimelineVisualizerWindow";

    windowClass.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    windowClass.hbrBackground =
        (HBRUSH)(
            COLOR_WINDOW + 1
        );

    if (!RegisterClassW(
            &windowClass
        ))
    {
        return 1;
    }

    g_hwnd =
        CreateWindowExW(
            0,
            windowClass.lpszClassName,
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

    MSG message{};

    while (
        GetMessageW(
            &message,
            nullptr,
            0,
            0
        ) > 0
    )
    {
        TranslateMessage(
            &message
        );

        DispatchMessageW(
            &message
        );
    }

    return (int)message.wParam;
}
