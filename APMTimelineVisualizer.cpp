#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <wincodec.h>
#include <gdiplus.h>

using namespace Gdiplus;

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cwctype>
#include <cmath>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

// ============================================================
// SETTINGS
// ============================================================

static const int VIDEO_WIDTH = 1920;
static const int VIDEO_HEIGHT = 1080;

static const int FPS = 60;
static const int DEFAULT_WIDTH = 1100;
static const int DEFAULT_HEIGHT = 850;

static const UINT TIMER_PREVIEW = 1;

// ============================================================
// EVENT
// ============================================================

struct TimelineEvent
{
    double time = 0.0;
    std::wstring name;
    bool down = true;
};

// ============================================================
// GLOBALS
// ============================================================

static HWND hwndMain = nullptr;
static HWND hwndTimeline = nullptr;
static HWND hwndSecondTimeline = nullptr;

static HWND hwndLoad = nullptr;
static HWND hwndPlay = nullptr;
static HWND hwndReset = nullptr;
static HWND hwndExportGIF = nullptr;
static ULONG_PTR gdiplusToken = 0;

static std::vector<TimelineEvent> events;
static std::vector<std::wstring> heldKeys;

static size_t processedEventIndex = 0;

static double currentTime = 0.0;
static double duration = 0.0;

static bool playing = false;

static LARGE_INTEGER performanceFrequency{};
static LARGE_INTEGER playbackStartPerformance{};
static double playbackStartTimelineTime = 0.0;

// ============================================================
// STRING HELPERS
// ============================================================

static std::wstring Trim(
    const std::wstring& input
)
{
    size_t first = 0;

    while (
        first < input.size() &&
        iswspace(input[first])
    )
    {
        ++first;
    }

    size_t last = input.size();

    while (
        last > first &&
        iswspace(input[last - 1])
    )
    {
        --last;
    }

    return input.substr(
        first,
        last - first
    );
}

static std::wstring ToUpper(
    std::wstring value
)
{
    for (wchar_t& c : value)
        c = towupper(c);

    return value;
}

static bool EndsWith(
    const std::wstring& value,
    const std::wstring& ending
)
{
    if (value.size() < ending.size())
        return false;

    return value.compare(
        value.size() - ending.size(),
        ending.size(),
        ending
    ) == 0;
}

// ============================================================
// TIMESTAMP PARSER
// ============================================================

static bool ParseTimestamp(
    const std::wstring& text,
    double& seconds
)
{
    std::wstring s = Trim(text);

    for (wchar_t& c : s)
    {
        if (c == ':')
            c = L' ';
    }

    std::wstringstream ss(s);

    int hour = 0;
    int minute = 0;
    double second = 0.0;
    std::wstring ampm;

    if (!(ss >> hour >> minute >> second >> ampm))
        return false;

    ampm = ToUpper(ampm);

    if (
        ampm != L"AM" &&
        ampm != L"PM"
    )
    {
        return false;
    }

    if (hour < 1 || hour > 12)
        return false;

    if (minute < 0 || minute > 59)
        return false;

    if (second < 0.0 || second >= 60.0)
        return false;

    if (ampm == L"AM")
    {
        if (hour == 12)
            hour = 0;
    }
    else
    {
        if (hour != 12)
            hour += 12;
    }

    seconds =
        static_cast<double>(hour) * 3600.0 +
        static_cast<double>(minute) * 60.0 +
        second;

    return true;
}

// ============================================================
// TIMELINE PARSER
// ============================================================

static bool ParseTimeline(
    const std::wstring& text
)
{
    std::vector<TimelineEvent> parsed;

    std::wstringstream stream(text);
    std::wstring line;

    // Compact format written by APMOverlay:
    //   2218 S+
    //   125 S-
    //   0 T+
    //   110 A+
    //
    // Number = delta milliseconds from the previous event.
    // First number = milliseconds from recording start.
    // NAME+ = press, NAME- = release.
    //
    // Legacy timestamp lines are also accepted.
    double compactTimeMs = 0.0;
    bool sawCompact = false;
    double previousClock = -1.0;
    double dayOffset = 0.0;
    bool sawLegacy = false;

    while (std::getline(stream, line))
    {
        line = Trim(line);

        if (line.empty() || line[0] == L'#' || line[0] == L'=')
            continue;

        // --------------------------------------------------------
        // COMPACT FORMAT
        // --------------------------------------------------------
        // Parse this BEFORE the legacy timestamp parser. Numeric
        // input names such as 1, 2, 3 are valid names.
        {
            std::wstringstream compact(line);
            long long deltaMs = 0;
            std::wstring nameWithState;
            std::wstring extra;

            if (
                (compact >> deltaMs) &&
                (compact >> nameWithState) &&
                !(compact >> extra) &&
                deltaMs >= 0 &&
                nameWithState.size() >= 2
            )
            {
                wchar_t state =
                    nameWithState[nameWithState.size() - 1];

                bool isDown = state == L'+';
                bool isUp = state == L'-';

                if (isDown || isUp)
                {
                    std::wstring name =
                        nameWithState.substr(
                            0,
                            nameWithState.size() - 1
                        );

                    if (!name.empty())
                    {
                        TimelineEvent event;

                        compactTimeMs +=
                            static_cast<double>(deltaMs);

                        event.time =
                            compactTimeMs / 1000.0;

                        event.name = name;
                        event.down = isDown;

                        parsed.push_back(event);
                        sawCompact = true;
                        continue;
                    }
                }
            }
        }

        // --------------------------------------------------------
        // LEGACY TIMESTAMP FORMAT
        // --------------------------------------------------------
        std::wstring timestampText;
        std::wstring inputName;

        size_t tab = line.find(L'\t');

        if (tab != std::wstring::npos)
        {
            timestampText = Trim(line.substr(0, tab));
            inputName = Trim(line.substr(tab + 1));
        }
        else
        {
            std::wstring upper = ToUpper(line);
            size_t amPos = upper.find(L" AM");
            size_t pmPos = upper.find(L" PM");
            size_t pos = std::wstring::npos;

            if (amPos != std::wstring::npos)
                pos = amPos;

            if (
                pmPos != std::wstring::npos &&
                (pos == std::wstring::npos || pmPos < pos)
            )
            {
                pos = pmPos;
            }

            if (pos == std::wstring::npos)
                continue;

            size_t timestampEnd = pos + 3;

            timestampText =
                Trim(line.substr(0, timestampEnd));

            inputName =
                Trim(line.substr(timestampEnd));
        }

        if (timestampText.empty() || inputName.empty())
            continue;

        double clockSeconds = 0.0;

        if (!ParseTimestamp(timestampText, clockSeconds))
            continue;

        if (
            previousClock >= 0.0 &&
            clockSeconds < previousClock
        )
        {
            dayOffset += 86400.0;
        }

        previousClock = clockSeconds;

        TimelineEvent event;
        event.time = clockSeconds + dayOffset;
        event.name = inputName;

        if (EndsWith(event.name, L"_UP"))
        {
            event.down = false;
            event.name.resize(event.name.size() - 3);
            event.name = Trim(event.name);
        }
        else
        {
            event.down = true;
        }

        parsed.push_back(event);
        sawLegacy = true;
    }

    if (parsed.empty())
        return false;

    // Compact events are already relative to recording start.
    // Legacy timestamps need to be normalized against their first event.
    if (!sawCompact && sawLegacy)
    {
        double firstTime = parsed.front().time;

        for (TimelineEvent& event : parsed)
        {
            event.time -= firstTime;

            if (event.time < 0.0)
                event.time = 0.0;
        }
    }

    std::stable_sort(
        parsed.begin(),
        parsed.end(),
        [](const TimelineEvent& a, const TimelineEvent& b)
        {
            return a.time < b.time;
        }
    );

    events = parsed;
    duration = events.back().time + 0.5;

    if (duration < 0.5)
        duration = 0.5;

    currentTime = 0.0;
    processedEventIndex = 0;
    heldKeys.clear();

    return true;
}

// ============================================================
// PLAYBACK STATE FORWARD DECLARATIONS
// ============================================================

static void ResetPlaybackState();
static void ProcessEventsTo(double targetTime);

// ============================================================
// SECOND-BASED TIMELINE DISPLAY
// ============================================================

static std::wstring BuildSecondTimelineText()
{
    if (events.empty())
        return L"";

    std::wstringstream output;

    int totalSeconds =
        static_cast<int>(std::ceil(duration));

    if (totalSeconds < 1)
        totalSeconds = 1;

    for (int second = 1; second <= totalSeconds; ++second)
    {
        double startTime =
            static_cast<double>(second - 1);

        double endTime =
            static_cast<double>(second);

        std::vector<std::wstring> active;

        auto addUnique =
            [&active](const std::wstring& name)
            {
                if (
                    std::find(
                        active.begin(),
                        active.end(),
                        name
                    ) == active.end()
                )
                {
                    active.push_back(name);
                }
            };

        // Keys already held when this second begins.
        ResetPlaybackState();
        ProcessEventsTo(startTime);

        for (const std::wstring& name : heldKeys)
            addUnique(name);

        // Also show inputs that occur at any point during this second.
        for (const TimelineEvent& event : events)
        {
            if (event.time < startTime)
                continue;

            if (event.time >= endTime)
                break;

            if (event.down)
                addUnique(event.name);
        }

        output << second << L"s  ";

        if (active.empty())
        {
            output << L"-";
        }
        else
        {
            for (size_t i = 0; i < active.size(); ++i)
            {
                if (i > 0)
                    output << L" + ";

                output << active[i];
            }
        }

        if (second < totalSeconds)
            output << L"\r\n";
    }

    return output.str();
}

static void UpdateSecondTimeline()
{
    if (!hwndSecondTimeline)
        return;

    std::wstring text =
        BuildSecondTimelineText();

    SetWindowTextW(
        hwndSecondTimeline,
        text.c_str()
    );

    // BuildSecondTimelineText temporarily walks the event state.
    // Restore the normal playback state afterward.
    ResetPlaybackState();
    ProcessEventsTo(currentTime);
}

// ============================================================
// EDIT CONTROL
// ============================================================

static std::wstring GetEditText(
    HWND edit
)
{
    int length =
        GetWindowTextLengthW(edit);

    if (length <= 0)
        return L"";

    std::wstring text(
        static_cast<size_t>(length) + 1,
        L'\0'
    );

    GetWindowTextW(
        edit,
        &text[0],
        length + 1
    );

    text.resize(
        static_cast<size_t>(length)
    );

    return text;
}

// ============================================================
// HELD STATE
// ============================================================

static bool IsHeld(
    const std::wstring& name
)
{
    return std::find(
        heldKeys.begin(),
        heldKeys.end(),
        name
    ) != heldKeys.end();
}

static void SetHeld(
    const std::wstring& name,
    bool held
)
{
    auto it =
        std::find(
            heldKeys.begin(),
            heldKeys.end(),
            name
        );

    if (held)
    {
        if (it == heldKeys.end())
            heldKeys.push_back(name);
    }
    else
    {
        if (it != heldKeys.end())
            heldKeys.erase(it);
    }
}

// ============================================================
// PROCESS EVENTS
// ============================================================

static void ResetPlaybackState()
{
    heldKeys.clear();
    processedEventIndex = 0;
}

static void ProcessEventsTo(
    double targetTime
)
{
    while (
        processedEventIndex < events.size() &&
        events[processedEventIndex].time <= targetTime
    )
    {
        const TimelineEvent& event =
            events[processedEventIndex];

        SetHeld(
            event.name,
            event.down
        );

        ++processedEventIndex;
    }
}

// ============================================================
// APM
// ============================================================

static int CalculateAPM(
    double time
)
{
    double start =
        time - 60.0;

    int count = 0;

    for (const TimelineEvent& event : events)
    {
        if (!event.down)
            continue;

        if (event.time > time)
            break;

        if (event.time >= start)
            ++count;
    }

    return count;
}

// ============================================================
// DRAW HELPERS
// ============================================================

static void FillRectColor(
    HDC hdc,
    int left,
    int top,
    int right,
    int bottom,
    COLORREF color
)
{
    RECT r{
        left,
        top,
        right,
        bottom
    };

    HBRUSH brush =
        CreateSolidBrush(color);

    FillRect(
        hdc,
        &r,
        brush
    );

    DeleteObject(brush);
}

static void DrawCenteredText(
    HDC hdc,
    const std::wstring& text,
    int left,
    int top,
    int right,
    int bottom,
    int fontSize,
    bool bold,
    COLORREF textColor = RGB(255, 255, 255)
)
{
    HFONT font =
        CreateFontW(
            -fontSize,
            0,
            0,
            0,
            bold ? FW_BOLD : FW_NORMAL,
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
        textColor
    );

    RECT r{
        left,
        top,
        right,
        bottom
    };

    DrawTextW(
        hdc,
        text.c_str(),
        -1,
        &r,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        old
    );

    DeleteObject(font);
}

// ============================================================
// HAND-DRAWN / MS-PAINT STYLE FINAL GIF VISUALIZER
// ============================================================

static unsigned int SketchSeed(const std::wstring& text, int x, int y)
{
    unsigned int seed = 2166136261u;
    for (wchar_t c : text)
    {
        seed ^= static_cast<unsigned int>(c);
        seed *= 16777619u;
    }
    seed ^= static_cast<unsigned int>(x * 31 + y * 17);
    seed *= 16777619u;
    return seed;
}

static int SketchJitter(unsigned int& seed, int amount)
{
    if (amount <= 0)
        return 0;
    seed = seed * 1664525u + 1013904223u;
    return static_cast<int>((seed >> 24) % (amount * 2 + 1)) - amount;
}

static void SketchLine(HDC hdc, int x1, int y1, int x2, int y2, int width = 3)
{
    HPEN pen = CreatePen(PS_SOLID, width, RGB(20, 20, 20));
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static bool IsHeldAny(const wchar_t* a, const wchar_t* b = nullptr)
{
    if (IsHeld(a))
        return true;
    return b != nullptr && IsHeld(b);
}

static std::wstring DisplayInputName(const std::wstring& name)
{
    if (name == L"Left Shift" || name == L"Right Shift") return L"Shift";
    if (name == L"Left Ctrl" || name == L"Right Ctrl") return L"Ctrl";
    if (name == L"Left Alt" || name == L"Right Alt") return L"Alt";
    if (name == L"Page Up") return L"PgUp";
    if (name == L"Page Down") return L"PgDn";
    if (name == L"Caps Lock") return L"Caps";
    return name;
}

static void DrawSketchLabel(
    HDC hdc,
    const std::wstring& name,
    int x,
    int y,
    int w,
    int h,
    int fontSize = 20,
    bool bold = false,
    bool pressedOverride = false,
    bool useOverride = false
)
{
    const bool pressed = useOverride ? pressedOverride : IsHeld(name);

    unsigned int seed = SketchSeed(name, x, y);
    const int jx = SketchJitter(seed, 1);
    const int jy = SketchJitter(seed, 1);

    // The drawing has mostly unboxed labels. When a control is held,
    // invert only its little area: white text on black, then restore it.
    if (pressed)
    {
        HBRUSH black = CreateSolidBrush(RGB(15, 15, 15));
        RECT r{ x + 1, y + 2, x + w - 1, y + h - 2 };
        FillRect(hdc, &r, black);
        DeleteObject(black);
    }

    DrawCenteredText(
        hdc,
        name,
        x + jx,
        y + jy,
        x + w + jx,
        y + h + jy,
        fontSize,
        bold,
        pressed ? RGB(255, 255, 255) : RGB(20, 20, 20)
    );
}

static void DrawSketchKeyBox(
    HDC hdc,
    const std::wstring& name,
    int x,
    int y,
    int w,
    int h,
    int fontSize = 18
)
{
    const bool pressed = IsHeld(name);
    unsigned int seed = SketchSeed(name, x, y);

    POINT p[4] = {
        { x + SketchJitter(seed, 2), y + SketchJitter(seed, 2) },
        { x + w + SketchJitter(seed, 2), y + SketchJitter(seed, 2) },
        { x + w + SketchJitter(seed, 2), y + h + SketchJitter(seed, 2) },
        { x + SketchJitter(seed, 2), y + h + SketchJitter(seed, 2) }
    };

    HBRUSH fill = CreateSolidBrush(pressed ? RGB(15, 15, 15) : RGB(250, 250, 245));
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(20, 20, 20));
    HGDIOBJ oldBrush = SelectObject(hdc, fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    Polygon(hdc, p, 4);

    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (int pass = 0; pass < 2; ++pass)
    {
        MoveToEx(hdc, p[0].x + (pass ? -1 : 1), p[0].y + (pass ? 2 : -1), nullptr);
        LineTo(hdc, p[1].x + SketchJitter(seed, 1), p[1].y + SketchJitter(seed, 1));
        LineTo(hdc, p[2].x + SketchJitter(seed, 1), p[2].y + SketchJitter(seed, 1));
        LineTo(hdc, p[3].x + SketchJitter(seed, 1), p[3].y + SketchJitter(seed, 1));
        LineTo(hdc, p[0].x + (pass ? -1 : 1), p[0].y + (pass ? 2 : -1));
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(fill);

    DrawCenteredText(
        hdc,
        name,
        x,
        y,
        x + w,
        y + h,
        fontSize,
        true,
        pressed ? RGB(255, 255, 255) : RGB(20, 20, 20)
    );
}

static std::vector<std::wstring> GetLastTenInputs(double time)
{
    std::vector<std::wstring> result;

    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
        if (it->time > time || !it->down)
            continue;

        result.push_back(DisplayInputName(it->name));
        if (result.size() >= 10)
            break;
    }

    std::reverse(result.begin(), result.end());
    return result;
}

static void DrawKeyboardPanel(HDC hdc)
{
    // Based directly on the user's hand-drawn reference:
    // one long rough rectangle, rows of handwritten labels,
    // APM at the right, and the last ten inputs below it.
    const int x = 70;
    const int y = 250;
    const int w = 1210;
    const int h = 450;

    HBRUSH paper = CreateSolidBrush(RGB(250, 250, 245));
    HPEN ink = CreatePen(PS_SOLID, 3, RGB(20, 20, 20));
    HGDIOBJ oldBrush = SelectObject(hdc, paper);
    HGDIOBJ oldPen = SelectObject(hdc, ink);

    POINT border[5] = {
        { x, y + 6 },
        { x + 4, y },
        { x + w - 5, y + 3 },
        { x + w, y + h - 5 },
        { x + 2, y + h }
    };
    Polygon(hdc, border, 5);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    MoveToEx(hdc, x + 5, y + 7, nullptr);
    LineTo(hdc, x + w - 8, y + 2);
    LineTo(hdc, x + w - 2, y + h - 8);
    LineTo(hdc, x + 5, y + h - 2);
    LineTo(hdc, x + 5, y + 7);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(ink);
    DeleteObject(paper);

    const int rowH = 50;
    const int left = x + 18;
    const int top = y + 18;

    // F-row. The spacing is intentionally close to the user's sketch.
    DrawSketchLabel(hdc, L"Esc", left, top, 58, rowH, 18, true);
    const wchar_t* fkeys[] = {
        L"F1", L"F2", L"F3", L"F4", L"F5", L"F6",
        L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"
    };
    for (int i = 0; i < 12; ++i)
        DrawSketchLabel(hdc, fkeys[i], left + 65 + i * 58, top, 52, rowH, 17, true);

    // Main rows.
    const wchar_t* row1[] = { L"~", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"0", L"-", L"=" };
    for (int i = 0; i < 13; ++i)
        DrawSketchLabel(hdc, row1[i], left + i * 43, top + 52, 38, rowH, 20, true);

    const wchar_t* row2[] = { L"Tab", L"Q", L"W", L"E", L"R", L"T", L"Y", L"U", L"I", L"O", L"P", L"[", L"]", L"\\" };
    const int row2x[] = { 0, 58, 101, 144, 187, 230, 273, 316, 359, 402, 445, 488, 531, 574 };
    for (int i = 0; i < 14; ++i)
        DrawSketchLabel(hdc, row2[i], left + row2x[i], top + 104, i == 0 ? 52 : 38, rowH, i == 0 ? 16 : 20, true);

    const wchar_t* row3[] = { L"Caps", L"A", L"S", L"D", L"F", L"G", L"H", L"J", L"K", L"L", L";", L"'" };
    const int row3x[] = { 0, 68, 111, 154, 197, 240, 283, 326, 369, 412, 455, 498 };
    for (int i = 0; i < 12; ++i)
        DrawSketchLabel(hdc, row3[i], left + row3x[i], top + 156, i == 0 ? 60 : 38, rowH, i == 0 ? 15 : 20, true);

    const wchar_t* row4[] = { L"Shift", L"Z", L"X", L"C", L"V", L"B", L"N", L"M", L"<", L">", L"?" };
    const int row4x[] = { 0, 82, 125, 168, 211, 254, 297, 340, 383, 426, 469 };
    for (int i = 0; i < 11; ++i)
        DrawSketchLabel(hdc, row4[i], left + row4x[i], top + 208, i == 0 ? 72 : 38, rowH, i == 0 ? 15 : 20, true);

    DrawSketchLabel(hdc, L"Ctrl", left, top + 260, 62, rowH, 15, true, IsHeldAny(L"Left Ctrl", L"Right Ctrl"), true);
    DrawSketchLabel(hdc, L"Alt", left + 70, top + 260, 55, rowH, 16, true, IsHeldAny(L"Left Alt", L"Right Alt"), true);
    DrawSketchLabel(hdc, L"Space", left + 132, top + 260, 250, rowH, 18, true, IsHeld(L"Space"));
    DrawSketchLabel(hdc, L"Alt", left + 388, top + 260, 55, rowH, 16, true, IsHeldAny(L"Left Alt", L"Right Alt"), true);

    // Navigation column from the reference drawing.
    const int navX = x + 620;
    DrawSketchLabel(hdc, L"←", navX, top + 52, 48, rowH, 22, true, IsHeld(L"Left"));
    DrawSketchLabel(hdc, L"PgUp", navX + 55, top + 52, 70, rowH, 15, true, IsHeld(L"Page Up"));
    DrawSketchLabel(hdc, L"PgDn", navX + 55, top + 104, 70, rowH, 15, true, IsHeld(L"Page Down"));
    DrawSketchLabel(hdc, L"Home", navX + 55, top + 156, 70, rowH, 15, true, IsHeld(L"Home"));
    DrawSketchLabel(hdc, L"End", navX + 55, top + 208, 70, rowH, 15, true, IsHeld(L"End"));
    DrawSketchLabel(hdc, L"Enter", navX + 130, top + 104, 75, rowH, 15, true, IsHeld(L"Enter"));
    DrawSketchLabel(hdc, L"Shift", navX + 130, top + 156, 75, rowH, 15, true, IsHeldAny(L"Left Shift", L"Right Shift"), true);
    DrawSketchLabel(hdc, L"↑", navX + 130, top + 208, 42, rowH, 22, true, IsHeld(L"Up"));
    DrawSketchLabel(hdc, L"↓", navX + 175, top + 208, 42, rowH, 22, true, IsHeld(L"Down"));
    DrawSketchLabel(hdc, L"→", navX + 220, top + 208, 42, rowH, 22, true, IsHeld(L"Right"));

    // APM and last ten, positioned exactly where the sketch puts them.
    const int infoX = x + 900;
    DrawSketchLabel(hdc, L"APM: " + std::to_wstring(CalculateAPM(currentTime)), infoX, top + 42, 210, 58, 25, true);
    DrawSketchLabel(hdc, L"Last 10 btms", infoX, top + 105, 210, 45, 20, true);

    std::vector<std::wstring> lastTen = GetLastTenInputs(currentTime);
    for (int i = 0; i < 10; ++i)
    {
        std::wstring value = (i < static_cast<int>(lastTen.size())) ? lastTen[i] : L".";
        DrawSketchLabel(hdc, value, infoX + 15, top + 145 + i * 25, 180, 24, 16, false);
    }
}

static void DrawMouse(HDC hdc)
{
    // Simple mouse from the reference drawing, enlarged for the 1920x1080 GIF.
    const int x = 1450;
    const int y = 350;
    const int w = 270;
    const int h = 360;

    const bool left = IsHeld(L"LMB");
    const bool right = IsHeld(L"RMB");
    const bool middle = IsHeld(L"MMB");
    const bool x1 = IsHeld(L"X1");
    const bool x2 = IsHeld(L"X2");

    POINT body[10] = {
        { x + 82, y }, { x + 185, y + 3 }, { x + 230, y + 38 },
        { x + 242, y + 240 }, { x + 218, y + 300 }, { x + 168, y + 345 },
        { x + 92, y + 348 }, { x + 42, y + 305 }, { x + 22, y + 240 },
        { x + 28, y + 45 }
    };

    HBRUSH bodyBrush = CreateSolidBrush(RGB(250, 250, 245));
    HPEN bodyPen = CreatePen(PS_SOLID, 4, RGB(20, 20, 20));
    HGDIOBJ oldBrush = SelectObject(hdc, bodyBrush);
    HGDIOBJ oldPen = SelectObject(hdc, bodyPen);
    Polygon(hdc, body, 10);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    MoveToEx(hdc, x + 28, y + 45, nullptr);
    LineTo(hdc, x + 130, y + 48);
    LineTo(hdc, x + 230, y + 42);
    LineTo(hdc, x + 235, y + 238);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(bodyPen);
    DeleteObject(bodyBrush);

    auto mouseArea = [&](int l, int t, int r, int b, const wchar_t* label, bool pressed, int fs)
    {
        HBRUSH fill = CreateSolidBrush(pressed ? RGB(15, 15, 15) : RGB(250, 250, 245));
        RECT rr{ x + l, y + t, x + r, y + b };
        FillRect(hdc, &rr, fill);
        DeleteObject(fill);
        HPEN p = CreatePen(PS_SOLID, 3, RGB(20, 20, 20));
        HGDIOBJ op = SelectObject(hdc, p);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rr.left, rr.top, rr.right, rr.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(p);
        DrawCenteredText(hdc, label, rr.left, rr.top, rr.right, rr.bottom, fs, true, pressed ? RGB(255,255,255) : RGB(20,20,20));
    };

    mouseArea(38, 45, 125, 155, L"LMB", left, 17);
    mouseArea(132, 45, 218, 155, L"RMB", right, 17);
    mouseArea(112, 70, 145, 128, L"MMB", middle, 9);

    // Side buttons are deliberately outside the body like the sketch.
    mouseArea(-4, 170, 30, 218, L"X1", x1, 10);
    mouseArea(-4, 225, 30, 273, L"X2", x2, 10);

    DrawCenteredText(hdc, L"mouse", x + 55, y + 185, x + 205, y + 245, 23, true, RGB(20,20,20));
}

// Kept as a no-op for compatibility with older export code.
// The actual final GIF visualizer is now the keyboard + mouse above.
static void DrawPaintWindow(HDC)
{
}


// ============================================================
// SCENE
// ============================================================

static void RenderScene(
    HDC hdc,
    int width,
    int height,
    bool includeInputVisualizer
)
{
    FillRectColor(
        hdc,
        0,
        0,
        width,
        height,
        RGB(18, 18, 22)
    );

    int apm =
        CalculateAPM(currentTime);

    wchar_t apmText[64];

    swprintf_s(
        apmText,
        L"APM %d",
        apm
    );

    DrawCenteredText(
        hdc,
        apmText,
        0,
        50,
        width,
        160,
        70,
        true
    );

    wchar_t timeText[64];

    swprintf_s(
        timeText,
        L"%02d:%05.2f",
        static_cast<int>(currentTime / 60.0),
        fmod(currentTime, 60.0)
    );

    DrawCenteredText(
        hdc,
        timeText,
        0,
        165,
        width,
        215,
        26,
        false
    );

    if (includeInputVisualizer)
    {
        DrawKeyboardPanel(hdc);
        DrawMouse(hdc);
    }

    const int barX = 250;
    const int barY = 920;
    const int barW = 1420;
    const int barH = 18;

    FillRectColor(
        hdc,
        barX,
        barY,
        barX + barW,
        barY + barH,
        RGB(50, 50, 55)
    );

    double progress =
        duration > 0.0
            ? currentTime / duration
            : 0.0;

    progress =
        std::max(
            0.0,
            std::min(
                1.0,
                progress
            )
        );

    FillRectColor(
        hdc,
        barX,
        barY,
        barX +
            static_cast<int>(
                barW * progress
            ),
        barY + barH,
        RGB(255, 170, 40)
    );
}

// ============================================================
// LOAD
// ============================================================

static void LoadTimelineFromEditor()
{
    std::wstring text =
        GetEditText(hwndTimeline);

    if (!ParseTimeline(text))
    {
        MessageBoxW(
            hwndMain,
            L"No valid timeline events were found.\n\n"
            L"Example:\n"
            L"672 S+\n"
            L"125 S-\n"
            L"0 T+\n"
            L"110 A+\n"
            L"31 T-",
            L"Timeline Error",
            MB_OK |
            MB_ICONWARNING
        );

        return;
    }

    playing = false;

    UpdateSecondTimeline();

    SetWindowTextW(
        hwndPlay,
        L"Play"
    );

    InvalidateRect(
        hwndMain,
        nullptr,
        TRUE
    );
}

// ============================================================
// GET EXE DIRECTORY
// ============================================================

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH
    );

    std::wstring result(path);

    size_t slash =
        result.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        result.resize(slash);

    return result;
}

// ============================================================
// GIF EXPORT
// ============================================================

static bool GetEncoderClsid(
    const WCHAR* mimeType,
    CLSID* clsid
)
{
    UINT num = 0;
    UINT size = 0;

    if (
        GetImageEncodersSize(
            &num,
            &size
        ) != Ok ||
        size == 0
    )
    {
        return false;
    }

    std::vector<BYTE> buffer(size);

    ImageCodecInfo* codecs =
        reinterpret_cast<ImageCodecInfo*>(
            buffer.data()
        );

    if (
        GetImageEncoders(
            num,
            size,
            codecs
        ) != Ok
    )
    {
        return false;
    }

    for (UINT i = 0; i < num; ++i)
    {
        if (
            wcscmp(
                codecs[i].MimeType,
                mimeType
            ) == 0
        )
        {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }

    return false;
}

static bool SetGIFProperty(
    Bitmap* bitmap,
    PROPID id,
    ULONG type,
    ULONG valueSize,
    void* value
)
{
    UINT itemSize =
        sizeof(PropertyItem);

    PropertyItem item{};

    item.id = id;
    item.length = valueSize;
    item.type = type;
    item.value = value;

    return bitmap->SetPropertyItem(&item) == Ok;
}

static HBITMAP CreateVideoBitmap(
    HDC* outDC
)
{
    HDC screen =
        GetDC(nullptr);

    HDC memDC =
        CreateCompatibleDC(screen);

    ReleaseDC(
        nullptr,
        screen
    );

    if (!memDC)
        return nullptr;

    BITMAPINFO bmi{};

    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth =
        VIDEO_WIDTH;

    bmi.bmiHeader.biHeight =
        -VIDEO_HEIGHT;

    bmi.bmiHeader.biPlanes =
        1;

    bmi.bmiHeader.biBitCount =
        32;

    bmi.bmiHeader.biCompression =
        BI_RGB;

    void* bits = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            memDC,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    if (!bitmap)
    {
        DeleteDC(memDC);
        return nullptr;
    }

    *outDC = memDC;

    return bitmap;
}

static Bitmap* RenderGIFFrame(
    HDC videoDC,
    HBITMAP videoBitmap,
    int outputWidth,
    int outputHeight
)
{
    RenderScene(
        videoDC,
        VIDEO_WIDTH,
        VIDEO_HEIGHT,
        true
    );


    Bitmap source(
        videoBitmap,
        nullptr
    );

    Bitmap* frame =
        new Bitmap(
            outputWidth,
            outputHeight,
            PixelFormat32bppARGB
        );

    if (
        frame->GetLastStatus() != Ok
    )
    {
        delete frame;
        return nullptr;
    }

    Graphics graphics(frame);

    graphics.SetInterpolationMode(
        InterpolationModeHighQualityBicubic
    );

    graphics.SetPixelOffsetMode(
        PixelOffsetModeHighQuality
    );

    Rect destination(
        0,
        0,
        outputWidth,
        outputHeight
    );

    if (
        graphics.DrawImage(
            &source,
            destination
        ) != Ok
    )
    {
        delete frame;
        return nullptr;
    }

    // GDI+ is much more reliable for animated GIFs when every
    // frame is an 8-bit indexed bitmap.  A 32-bit ARGB bitmap can
    // save successfully as a GIF but may silently produce a
    // single-frame GIF when used with SaveAdd.
    Bitmap* indexedFrame =
        frame->Clone(
            0,
            0,
            outputWidth,
            outputHeight,
            PixelFormat8bppIndexed
        );

    if (!indexedFrame || indexedFrame->GetLastStatus() != Ok)
    {
        delete indexedFrame;
        delete frame;
        return nullptr;
    }

    delete frame;
    return indexedFrame;
}

static bool ExportGIF()
{
    if (events.empty())
    {
        MessageBoxW(
            hwndMain,
            L"Load a timeline first.",
            L"Export GIF",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    if (gdiplusToken == 0)
        return false;

    const int outputWidth =
        std::min(
            VIDEO_WIDTH,
            1280
        );

    const int outputHeight =
        std::min(
            VIDEO_HEIGHT,
            720
        );

    std::wstring outputPath =
        GetExeDirectory() +
        L"\\APM_Replay.gif";

    HDC videoDC = nullptr;

    HBITMAP videoBitmap =
        CreateVideoBitmap(
            &videoDC
        );

    if (!videoBitmap)
    {
        MessageBoxW(
            hwndMain,
            L"Could not create the rendering surface.",
            L"GIF Export Failed",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    HBITMAP oldBitmap =
        static_cast<HBITMAP>(
            SelectObject(
                videoDC,
                videoBitmap
            )
        );

    CLSID gifClsid{};

    if (
        !GetEncoderClsid(
            L"image/gif",
            &gifClsid
        )
    )
    {
        SelectObject(
            videoDC,
            oldBitmap
        );

        DeleteObject(videoBitmap);
        DeleteDC(videoDC);

        MessageBoxW(
            hwndMain,
            L"Could not find the Windows GIF encoder.",
            L"GIF Export Failed",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    const double savedTime =
        currentTime;

    const bool savedPlaying =
        playing;

    playing = false;

    // --------------------------------------------------------
    // Create ONE canonical neutral frame.
    //
    // Whenever no keyboard/mouse input is held during export,
    // this exact same image is reused.  We do not redraw the
    // neutral scene for every idle interval.  The GIF delay is
    // what represents the amount of time spent idle.
    // --------------------------------------------------------
    ResetPlaybackState();
    currentTime = 0.0;

    Bitmap* neutralFrame =
        RenderGIFFrame(
            videoDC,
            videoBitmap,
            outputWidth,
            outputHeight
        );

    if (!neutralFrame)
    {
        SelectObject(
            videoDC,
            oldBitmap
        );

        DeleteObject(videoBitmap);
        DeleteDC(videoDC);

        currentTime = savedTime;
        playing = savedPlaying;

        MessageBoxW(
            hwndMain,
            L"Could not create the neutral GIF frame.",
            L"GIF Export Failed",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    std::vector<double> frameTimes;

    // The GIF only gets a new frame when the input state changes.
    // Time between changes is represented by the GIF frame delay.
    frameTimes.push_back(0.0);

    for (const TimelineEvent& event : events)
    {
        if (event.time <= 0.0)
            continue;

        if (
            frameTimes.empty() ||
            event.time >
                frameTimes.back() + 0.0000001
        )
        {
            frameTimes.push_back(
                event.time
            );
        }
    }

    bool success = true;

    DeleteFileW(
        outputPath.c_str()
    );

    Bitmap* firstFrame = nullptr;
    size_t neutralFrameUses = 0;

    EncoderParameters saveParameters{};

    saveParameters.Count = 1;

    saveParameters.Parameter[0].Guid =
        EncoderSaveFlag;

    saveParameters.Parameter[0].Type =
        EncoderParameterValueTypeLong;

    saveParameters.Parameter[0].NumberOfValues =
        1;

    ULONG saveFlag =
        EncoderValueMultiFrame;

    saveParameters.Parameter[0].Value =
        &saveFlag;

    for (
        size_t i = 0;
        i < frameTimes.size();
        ++i
    )
    {
        currentTime =
            frameTimes[i];

        ResetPlaybackState();

        ProcessEventsTo(
            currentTime
        );

        // If nothing is currently held, reuse the one canonical
        // neutral image instead of rendering another identical
        // bitmap.  This makes idle periods act as a buffer between
        // active input states.
        const bool isNeutral =
            heldKeys.empty();

        Bitmap* frame = nullptr;

        if (isNeutral)
        {
            frame = neutralFrame;
            ++neutralFrameUses;
        }
        else
        {
            frame =
                RenderGIFFrame(
                    videoDC,
                    videoBitmap,
                    outputWidth,
                    outputHeight
                );
        }

        if (!frame)
        {
            success = false;
            break;
        }

        double nextTime =
            (i + 1 < frameTimes.size())
                ? frameTimes[i + 1]
                : duration;

        double delaySeconds =
            nextTime - frameTimes[i];

        if (delaySeconds <= 0.0)
            delaySeconds = 0.01;

        ULONG delay =
            static_cast<ULONG>(
                std::max(
                    1.0,
                    std::round(
                        delaySeconds * 100.0
                    )
                )
            );

        SetGIFProperty(
            frame,
            PropertyTagFrameDelay,
            PropertyTagTypeLong,
            sizeof(ULONG),
            &delay
        );

        // 0 means infinite looping.
        USHORT loopCount = 0;

        SetGIFProperty(
            frame,
            PropertyTagLoopCount,
            PropertyTagTypeShort,
            sizeof(USHORT),
            &loopCount
        );

        if (i == 0)
        {
            firstFrame = frame;

            if (
                firstFrame->Save(
                    outputPath.c_str(),
                    &gifClsid,
                    &saveParameters
                ) != Ok
            )
            {
                success = false;
                if (firstFrame != neutralFrame)
                    delete firstFrame;
                firstFrame = nullptr;
                break;
            }
        }
        else
        {
            EncoderParameters addParameters{};

            addParameters.Count = 1;

            addParameters.Parameter[0].Guid =
                EncoderSaveFlag;

            addParameters.Parameter[0].Type =
                EncoderParameterValueTypeLong;

            addParameters.Parameter[0].NumberOfValues =
                1;

            ULONG addFlag =
                EncoderValueFrameDimensionTime;

            addParameters.Parameter[0].Value =
                &addFlag;

            if (
                firstFrame->SaveAdd(
                    frame,
                    &addParameters
                ) != Ok
            )
            {
                success = false;
                if (!isNeutral)
                    delete frame;
                break;
            }

            if (!isNeutral)
                delete frame;
        }
    }

    if (
        success &&
        firstFrame
    )
    {
        saveFlag =
            EncoderValueFlush;

        EncoderParameters flushParameters{};

        flushParameters.Count = 1;

        flushParameters.Parameter[0].Guid =
            EncoderSaveFlag;

        flushParameters.Parameter[0].Type =
            EncoderParameterValueTypeLong;

        flushParameters.Parameter[0].NumberOfValues =
            1;

        flushParameters.Parameter[0].Value =
            &saveFlag;

        if (
            firstFrame->SaveAdd(
                &flushParameters
            ) != Ok
        )
        {
            success = false;
        }

        if (firstFrame != neutralFrame)
            delete firstFrame;
        firstFrame = nullptr;
    }

    if (neutralFrame)
    {
        delete neutralFrame;
        neutralFrame = nullptr;
    }

    SelectObject(
        videoDC,
        oldBitmap
    );

    DeleteObject(videoBitmap);
    DeleteDC(videoDC);

    currentTime =
        savedTime;

    playing =
        savedPlaying;

    ResetPlaybackState();

    ProcessEventsTo(
        currentTime
    );

    InvalidateRect(
        hwndMain,
        nullptr,
        TRUE
    );

    if (success)
    {
        std::wstring message =
            L"GIF exported successfully:\\n\\n" +
            outputPath +
            L"\\n\\n" +
            std::to_wstring(
                frameTimes.size()
            ) +
            L" logical frames.\\n" +
            std::to_wstring(outputWidth) +
            L"x" +
            std::to_wstring(outputHeight) +
            L"\\n\\n"
            L"Frames are created only when the input state changes.\n"
            L"Idle periods reuse one canonical neutral frame.";

        MessageBoxW(
            hwndMain,
            message.c_str(),
            L"Export Complete",
            MB_OK | MB_ICONINFORMATION
        );
    }
    else
    {
        MessageBoxW(
            hwndMain,
            L"GIF export failed.",
            L"Export Failed",
            MB_OK | MB_ICONERROR
        );
    }

    return success;
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

static LRESULT CALLBACK WindowProc(
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
            CreateWindowW(
                L"STATIC",
                L"Raw Timeline",
                WS_CHILD |
                WS_VISIBLE,
                20,
                2,
                520,
                18,
                hwnd,
                nullptr,
                nullptr,
                nullptr
            );

            CreateWindowW(
                L"STATIC",
                L"Per-Second Timeline",
                WS_CHILD |
                WS_VISIBLE,
                560,
                2,
                520,
                18,
                hwnd,
                nullptr,
                nullptr,
                nullptr
            );

            hwndTimeline =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_WANTRETURN |
                    WS_VSCROLL |
                    WS_HSCROLL,
                    20,
                    20,
                    520,
                    220,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr
                );

            SendMessageW(
                hwndTimeline,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(
                    GetStockObject(
                        DEFAULT_GUI_FONT
                    )
                ),
                TRUE
            );

            hwndSecondTimeline =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"Load a timeline to generate the per-second view.",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_READONLY |
                    WS_VSCROLL |
                    WS_HSCROLL,
                    560,
                    20,
                    520,
                    220,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr
                );

            SendMessageW(
                hwndSecondTimeline,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(
                    GetStockObject(
                        DEFAULT_GUI_FONT
                    )
                ),
                TRUE
            );

            hwndLoad =
                CreateWindowW(
                    L"BUTTON",
                    L"Load Timeline",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    20,
                    255,
                    150,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1001),
                    nullptr,
                    nullptr
                );

            hwndPlay =
                CreateWindowW(
                    L"BUTTON",
                    L"Play",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    180,
                    255,
                    100,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1002),
                    nullptr,
                    nullptr
                );

            hwndReset =
                CreateWindowW(
                    L"BUTTON",
                    L"Reset",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    290,
                    255,
                    100,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1003),
                    nullptr,
                    nullptr
                );

            hwndExportGIF =
                CreateWindowW(
                    L"BUTTON",
                    L"Export GIF",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    400,
                    255,
                    130,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1005),
                    nullptr,
                    nullptr
                );

            QueryPerformanceFrequency(
                &performanceFrequency
            );

            QueryPerformanceCounter(
                &playbackStartPerformance
            );

            playbackStartTimelineTime = 0.0;

            SetTimer(
                hwnd,
                TIMER_PREVIEW,
                16,
                nullptr
            );

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case 1001:
                    LoadTimelineFromEditor();
                    return 0;

                case 1002:
                {
                    if (events.empty())
                        return 0;

                    if (playing)
                    {
                        // Pause at the exact current timeline position.
                        playing = false;
                    }
                    else
                    {
                        // If playback was already at the end, start again from 0.
                        if (currentTime >= duration)
                        {
                            currentTime = 0.0;
                            ResetPlaybackState();
                        }

                        // Anchor the real-time clock to the current timeline position.
                        QueryPerformanceCounter(
                            &playbackStartPerformance
                        );

                        playbackStartTimelineTime =
                            currentTime;

                        playing = true;
                    }

                    SetWindowTextW(
                        hwndPlay,
                        playing
                            ? L"Pause"
                            : L"Play"
                    );

                    return 0;
                }

                case 1003:
                {
                    playing = false;
                    currentTime = 0.0;
                    playbackStartTimelineTime = 0.0;

                    QueryPerformanceCounter(
                        &playbackStartPerformance
                    );

                    ResetPlaybackState();

                    SetWindowTextW(
                        hwndPlay,
                        L"Play"
                    );

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        TRUE
                    );

                    return 0;
                }

                case 1005:
                    ExportGIF();
                    return 0;
            }

            break;
        }

        case WM_TIMER:
        {
            if (
                wParam == TIMER_PREVIEW &&
                playing
            )
            {
                LARGE_INTEGER now{};

                QueryPerformanceCounter(
                    &now
                );

                double elapsed =
                    static_cast<double>(
                        now.QuadPart -
                        playbackStartPerformance.QuadPart
                    ) /
                    static_cast<double>(
                        performanceFrequency.QuadPart
                    );

                if (elapsed < 0.0)
                    elapsed = 0.0;

                // Use absolute elapsed time instead of repeatedly adding timer deltas.
                // This prevents drift and keeps playback locked to real time even if
                // Windows delivers WM_TIMER at uneven intervals.
                currentTime =
                    playbackStartTimelineTime +
                    elapsed;

                if (currentTime >= duration)
                {
                    currentTime = duration;
                    playing = false;

                    SetWindowTextW(
                        hwndPlay,
                        L"Play"
                    );

                    playbackStartTimelineTime =
                        duration;

                    ResetPlaybackState();
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    TRUE
                );
            }

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC hdc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            RECT client{};

            GetClientRect(
                hwnd,
                &client
            );

            const int sceneTop = 315;

            FillRectColor(
                hdc,
                0,
                sceneTop,
                client.right,
                client.bottom,
                RGB(18, 18, 22)
            );

            int previewWidth =
                client.right;

            int previewHeight =
                client.bottom - sceneTop;

            if (
                previewWidth > 0 &&
                previewHeight > 0
            )
            {
                HDC memDC =
                    CreateCompatibleDC(hdc);

                BITMAPINFO bmi{};

                bmi.bmiHeader.biSize =
                    sizeof(BITMAPINFOHEADER);

                bmi.bmiHeader.biWidth =
                    previewWidth;

                bmi.bmiHeader.biHeight =
                    -previewHeight;

                bmi.bmiHeader.biPlanes =
                    1;

                bmi.bmiHeader.biBitCount =
                    32;

                bmi.bmiHeader.biCompression =
                    BI_RGB;

                void* bits = nullptr;

                HBITMAP bitmap =
                    CreateDIBSection(
                        hdc,
                        &bmi,
                        DIB_RGB_COLORS,
                        &bits,
                        nullptr,
                        0
                    );

                if (bitmap)
                {
                    HBITMAP oldBitmap =
                        static_cast<HBITMAP>(
                            SelectObject(
                                memDC,
                                bitmap
                            )
                        );

                    ResetPlaybackState();

                    ProcessEventsTo(
                        currentTime
                    );

                    // Preview intentionally contains no keyboard/mouse visualizer.
                    // Those controls are rendered only into the exported GIF.
                    RenderScene(
                        memDC,
                        previewWidth,
                        previewHeight,
                        false
                    );

                    BitBlt(
                        hdc,
                        0,
                        sceneTop,
                        previewWidth,
                        previewHeight,
                        memDC,
                        0,
                        0,
                        SRCCOPY
                    );

                    SelectObject(
                        memDC,
                        oldBitmap
                    );

                    DeleteObject(bitmap);
                }

                DeleteDC(memDC);
            }

            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }

        case WM_SIZE:
        {
            int width =
                LOWORD(lParam);

            int paneGap = 20;
            int paneWidth =
                std::max(100, (width - 60) / 2);

            if (hwndTimeline)
            {
                MoveWindow(
                    hwndTimeline,
                    20,
                    20,
                    paneWidth,
                    220,
                    TRUE
                );
            }

            if (hwndSecondTimeline)
            {
                MoveWindow(
                    hwndSecondTimeline,
                    40 + paneWidth,
                    20,
                    paneWidth,
                    220,
                    TRUE
                );
            }

            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(
                hwnd,
                TIMER_PREVIEW
            );

            PostQuitMessage(0);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
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
    int nCmdShow
)
{
    GdiplusStartupInput gdiplusStartupInput{};

    if (
        GdiplusStartup(
            &gdiplusToken,
            &gdiplusStartupInput,
            nullptr
        ) != Ok
    )
    {
        MessageBoxW(
            nullptr,
            L"Could not initialize GDI+.",
            L"APM Timeline Visualizer",
            MB_OK | MB_ICONERROR
        );

        return 1;
    }

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"APMTimelineVisualizer";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(
                WHITE_BRUSH
            )
        );

    if (!RegisterClassW(&wc))
        return 1;

    hwndMain =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            DEFAULT_WIDTH,
            DEFAULT_HEIGHT,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwndMain)
        return 1;

    ShowWindow(
        hwndMain,
        nCmdShow
    );

    UpdateWindow(hwndMain);

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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (gdiplusToken != 0)
    {
        GdiplusShutdown(gdiplusToken);
        gdiplusToken = 0;
    }

    return static_cast<int>(
        msg.wParam
    );
}