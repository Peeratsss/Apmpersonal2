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
    bool bold
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
        RGB(255, 255, 255)
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
// KEY DRAWING
// ============================================================

static void DrawKey(
    HDC hdc,
    const std::wstring& name,
    int x,
    int y,
    int width,
    int height,
    int fontSize = 18
)
{
    bool pressed =
        IsHeld(name);

    COLORREF background =
        pressed
            ? RGB(255, 170, 40)
            : RGB(55, 55, 60);

    FillRectColor(
        hdc,
        x,
        y,
        x + width,
        y + height,
        background
    );

    HPEN pen =
        CreatePen(
            PS_SOLID,
            2,
            RGB(100, 100, 105)
        );

    HGDIOBJ oldPen =
        SelectObject(
            hdc,
            pen
        );

    HGDIOBJ oldBrush =
        SelectObject(
            hdc,
            GetStockObject(NULL_BRUSH)
        );

    Rectangle(
        hdc,
        x,
        y,
        x + width,
        y + height
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

    DrawCenteredText(
        hdc,
        name,
        x,
        y,
        x + width,
        y + height,
        fontSize,
        true
    );
}

// ============================================================
// HAND-DRAWN / MS-PAINT STYLE KEYBOARD
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
    seed = seed * 1664525u + 1013904223u;
    return static_cast<int>((seed >> 24) % (amount * 2 + 1)) - amount;
}

static void SketchLine(
    HDC hdc,
    int x1,
    int y1,
    int x2,
    int y2,
    int width = 3
)
{
    HPEN pen = CreatePen(
        PS_SOLID,
        width,
        RGB(25, 25, 25)
    );

    HGDIOBJ oldPen = SelectObject(hdc, pen);

    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void DrawSketchKey(
    HDC hdc,
    const std::wstring& name,
    int x,
    int y,
    int width,
    int height,
    int fontSize = 17
)
{
    const bool pressed = IsHeld(name);

    unsigned int seed = SketchSeed(name, x, y);

    const int j1 = SketchJitter(seed, 3);
    const int j2 = SketchJitter(seed, 3);
    const int j3 = SketchJitter(seed, 3);
    const int j4 = SketchJitter(seed, 3);

    const int x1 = x + j1;
    const int y1 = y + j2;
    const int x2 = x + width + j3;
    const int y2 = y + height + j4;

    // Cheap-looking white MS Paint fill.
    HBRUSH fill = CreateSolidBrush(
        pressed
            ? RGB(175, 215, 255)
            : RGB(245, 245, 238)
    );

    HPEN outline = CreatePen(
        PS_SOLID,
        3,
        RGB(25, 25, 25)
    );

    HGDIOBJ oldBrush = SelectObject(hdc, fill);
    HGDIOBJ oldPen = SelectObject(hdc, outline);

    POINT points[4];

    points[0] = { x1, y1 };
    points[1] = { x2, y1 + SketchJitter(seed, 2) };
    points[2] = { x2 + SketchJitter(seed, 2), y2 };
    points[3] = { x1 + SketchJitter(seed, 2), y2 + SketchJitter(seed, 2) };

    Polygon(hdc, points, 4);

    // Double scribbled outline, deliberately imperfect.
    SelectObject(hdc, GetStockObject(NULL_BRUSH));

    for (int pass = 0; pass < 2; ++pass)
    {
        int ox = pass == 0 ? 1 : -1;
        int oy = pass == 0 ? -1 : 2;

        MoveToEx(hdc, x1 + ox, y1 + oy, nullptr);
        LineTo(hdc, x2 + SketchJitter(seed, 2), y1 + SketchJitter(seed, 2));
        LineTo(hdc, x2 + SketchJitter(seed, 2), y2 + SketchJitter(seed, 2));
        LineTo(hdc, x1 + SketchJitter(seed, 2), y2 + SketchJitter(seed, 2));
        LineTo(hdc, x1 + ox, y1 + oy);
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);

    DeleteObject(outline);
    DeleteObject(fill);

    // Slightly messy handwritten-style label.
    HFONT font = CreateFontW(
        fontSize,
        0,
        SketchJitter(seed, 2),
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Comic Sans MS"
    );

    HGDIOBJ oldFont = SelectObject(hdc, font);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(20, 20, 20));

    RECT textRect{
        x1 + 3,
        y1 + 2,
        x2 - 2,
        y2 - 2
    };

    DrawTextW(
        hdc,
        name.c_str(),
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

static void DrawKeyboard(
    HDC hdc
)
{
    // Deliberately crude, MS-Paint-like keyboard.
    // It is only used in the final GIF, never in the visualizer UI.
    const int keyW = 72;
    const int keyH = 55;
    const int gap = 8;

    const int startX = 115;
    const int startY = 365;

    const wchar_t* numbers[] =
    {
        L"1", L"2", L"3", L"4", L"5",
        L"6", L"7", L"8", L"9", L"0"
    };

    for (int i = 0; i < 10; ++i)
    {
        DrawSketchKey(
            hdc,
            numbers[i],
            startX + i * (keyW + gap),
            startY,
            keyW,
            keyH,
            16
        );
    }

    const wchar_t* qrow[] =
    {
        L"Q", L"W", L"E", L"R", L"T",
        L"Y", L"U", L"I", L"O", L"P"
    };

    for (int i = 0; i < 10; ++i)
    {
        DrawSketchKey(
            hdc,
            qrow[i],
            startX + 30 + i * (keyW + gap),
            startY + keyH + gap,
            keyW,
            keyH,
            16
        );
    }

    const wchar_t* arow[] =
    {
        L"A", L"S", L"D", L"F", L"G",
        L"H", L"J", L"K", L"L"
    };

    for (int i = 0; i < 9; ++i)
    {
        DrawSketchKey(
            hdc,
            arow[i],
            startX + 65 + i * (keyW + gap),
            startY + 2 * (keyH + gap),
            keyW,
            keyH,
            16
        );
    }

    const wchar_t* zrow[] =
    {
        L"Z", L"X", L"C", L"V", L"B",
        L"N", L"M"
    };

    for (int i = 0; i < 7; ++i)
    {
        DrawSketchKey(
            hdc,
            zrow[i],
            startX + 105 + i * (keyW + gap),
            startY + 3 * (keyH + gap),
            keyW,
            keyH,
            16
        );
    }

    DrawSketchKey(
        hdc,
        L"Space",
        startX + 180,
        startY + 4 * (keyH + gap),
        500,
        keyH,
        17
    );

    DrawSketchKey(
        hdc,
        L"Ctrl",
        startX - 105,
        startY + 4 * (keyH + gap),
        100,
        keyH,
        14
    );

    DrawSketchKey(
        hdc,
        L"Shift",
        startX - 35,
        startY + 3 * (keyH + gap),
        125,
        keyH,
        14
    );

    DrawSketchKey(
        hdc,
        L"Alt",
        startX + 60,
        startY + 4 * (keyH + gap),
        100,
        keyH,
        14
    );

    DrawSketchKey(
        hdc,
        L"Enter",
        startX + 9 * (keyW + gap) - 10,
        startY + 2 * (keyH + gap),
        120,
        keyH,
        14
    );
}

// ============================================================
// HAND-DRAWN / MS-PAINT STYLE MOUSE
// ============================================================

static void PaintText(HDC hdc, const std::wstring& text, int x, int y, int w, int h, int size, bool bold = false);

static void DrawMouse(
    HDC hdc
)
{
    const int x = 1080;
    const int y = 430;

    const bool left = IsHeld(L"LMB");
    const bool right = IsHeld(L"RMB");

    // Mouse body: intentionally crude and slightly asymmetrical.
    POINT body[12] =
    {
        { x + 65,  y },
        { x + 185, y + 5 },
        { x + 220, y + 50 },
        { x + 225, y + 205 },
        { x + 205, y + 280 },
        { x + 160, y + 315 },
        { x + 90,  y + 318 },
        { x + 35,  y + 280 },
        { x + 15,  y + 205 },
        { x + 20,  y + 55 },
        { x + 38,  y + 18 },
        { x + 65,  y }
    };

    HBRUSH bodyBrush = CreateSolidBrush(RGB(245, 245, 238));
    HPEN bodyPen = CreatePen(PS_SOLID, 4, RGB(20, 20, 20));

    HGDIOBJ oldBrush = SelectObject(hdc, bodyBrush);
    HGDIOBJ oldPen = SelectObject(hdc, bodyPen);

    Polygon(hdc, body, 12);

    SelectObject(hdc, GetStockObject(NULL_BRUSH));

    // Scribbled second outline.
    MoveToEx(hdc, x + 62, y + 3, nullptr);
    LineTo(hdc, x + 183, y + 8);
    LineTo(hdc, x + 217, y + 52);
    LineTo(hdc, x + 220, y + 205);
    LineTo(hdc, x + 200, y + 278);
    LineTo(hdc, x + 155, y + 311);

    MoveToEx(hdc, x + 30, y + 62, nullptr);
    LineTo(hdc, x + 25, y + 205);
    LineTo(hdc, x + 42, y + 273);
    LineTo(hdc, x + 92, y + 311);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);

    DeleteObject(bodyPen);
    DeleteObject(bodyBrush);

    // Left/right click areas.
    HBRUSH leftBrush = CreateSolidBrush(
        left ? RGB(175, 215, 255) : RGB(235, 235, 228)
    );
    HBRUSH rightBrush = CreateSolidBrush(
        right ? RGB(175, 215, 255) : RGB(235, 235, 228)
    );

    RECT leftRect{ x + 35, y + 35, x + 115, y + 125 };
    RECT rightRect{ x + 120, y + 35, x + 205, y + 125 };

    FillRect(
        hdc,
        &leftRect,
        leftBrush
    );

    FillRect(
        hdc,
        &rightRect,
        rightBrush
    );

    DeleteObject(leftBrush);
    DeleteObject(rightBrush);

    SketchLine(hdc, x + 118, y + 35, x + 118, y + 130, 3);
    SketchLine(hdc, x + 35, y + 130, x + 205, y + 130, 3);

    // Wheel / center scribble.
    SketchLine(hdc, x + 120, y + 55, x + 120, y + 105, 5);
    SketchLine(hdc, x + 115, y + 75, x + 125, y + 75, 3);

    PaintText(
        hdc,
        L"LMB",
        x + 32,
        y + 55,
        85,
        55,
        18,
        true
    );

    PaintText(
        hdc,
        L"RMB",
        x + 120,
        y + 55,
        88,
        55,
        18,
        true
    );

    PaintText(
        hdc,
        L"mouse",
        x + 48,
        y + 205,
        125,
        45,
        20,
        true
    );

    // The deliberately bad side buttons.
    SketchLine(hdc, x + 15, y + 155, x - 18, y + 145, 4);
    SketchLine(hdc, x - 18, y + 145, x - 28, y + 165, 4);
    SketchLine(hdc, x - 27, y + 165, x + 12, y + 175, 4);

    PaintText(
        hdc,
        L"side btns",
        x - 90,
        y + 175,
        95,
        30,
        11,
        false
    );
}

// ============================================================
// PAINT-STYLE FINAL GIF PANEL
// ============================================================

static void PaintText(HDC hdc, const std::wstring& text, int x, int y, int w, int h, int size, bool bold = false)
{
    DrawCenteredText(hdc, text, x, y, x + w, y + h, size, bold);
}

static void DrawPaintWindow(HDC hdc)
{
    const int x = 1010;
    const int y = 105;
    const int w = 820;
    const int h = 830;

    // Outer window.
    FillRectColor(hdc, x, y, x + w, y + h, RGB(242, 242, 242));

    HPEN border = CreatePen(PS_SOLID, 2, RGB(125, 125, 130));
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    // Windows-style title bar.
    FillRectColor(hdc, x + 2, y + 2, x + w - 2, y + 42, RGB(250, 250, 250));
    PaintText(hdc, L"Untitled - Paint", x + 18, y + 7, 240, 28, 17, false);

    PaintText(hdc, L"—", x + w - 115, y + 4, 32, 30, 18, false);
    PaintText(hdc, L"□", x + w - 78, y + 4, 32, 30, 16, false);
    PaintText(hdc, L"×", x + w - 42, y + 3, 32, 30, 18, false);

    // Ribbon tabs.
    FillRectColor(hdc, x + 2, y + 42, x + w - 2, y + 78, RGB(248, 248, 248));
    FillRectColor(hdc, x + 12, y + 43, x + 74, y + 77, RGB(230, 230, 230));
    PaintText(hdc, L"Home", x + 12, y + 45, 62, 28, 15, true);

    // Ribbon body.
    FillRectColor(hdc, x + 2, y + 78, x + w - 2, y + 205, RGB(245, 245, 245));

    // Ribbon groups.
    const int gy = y + 87;
    const int gh = 92;

    // Clipboard.
    PaintText(hdc, L"Clipboard", x + 15, y + 174, 105, 18, 12, false);
    PaintText(hdc, L"Paste", x + 18, gy + 12, 58, 28, 13, true);
    PaintText(hdc, L"Cut", x + 78, gy + 12, 42, 28, 12, false);
    PaintText(hdc, L"Copy", x + 78, gy + 42, 48, 28, 12, false);

    // Image.
    PaintText(hdc, L"Image", x + 145, y + 174, 100, 18, 12, false);
    const wchar_t* imageTools[] = { L"Select", L"Crop", L"Resize", L"Rotate" };
    for (int i = 0; i < 4; ++i)
        PaintText(hdc, imageTools[i], x + 135 + i * 58, gy + 18, 55, 30, 11, false);

    // Tools.
    PaintText(hdc, L"Tools", x + 390, y + 174, 70, 18, 12, false);
    const wchar_t* tools[] = { L"✎", L"▣", L"A", L"⌫", L"◉", L"⌕", L"Brush" };
    for (int i = 0; i < 7; ++i)
        PaintText(hdc, tools[i], x + 365 + i * 55, gy + 15, 48, 35, i == 6 ? 10 : 17, false);

    // Shapes.
    PaintText(hdc, L"Shapes", x + 390, y + 174, 70, 18, 12, false);
    for (int i = 0; i < 5; ++i)
    {
        HPEN shapePen = CreatePen(PS_SOLID, 2, RGB(70, 70, 75));
        HGDIOBJ old = SelectObject(hdc, shapePen);
        int sx = x + 365 + i * 55;
        int sy = gy + 53;
        if (i == 0) LineTo(hdc, sx + 35, sy + 20);
        else if (i == 1) Rectangle(hdc, sx, sy, sx + 35, sy + 22);
        else if (i == 2) Ellipse(hdc, sx, sy, sx + 35, sy + 22);
        else if (i == 3) { MoveToEx(hdc, sx, sy + 22, nullptr); LineTo(hdc, sx + 18, sy); LineTo(hdc, sx + 36, sy + 22); }
        else { MoveToEx(hdc, sx, sy + 22, nullptr); LineTo(hdc, sx + 18, sy); LineTo(hdc, sx + 36, sy + 22); }
        SelectObject(hdc, old);
        DeleteObject(shapePen);
    }

    // Colors.
    PaintText(hdc, L"Colors", x + 650, y + 174, 80, 18, 12, false);
    PaintText(hdc, L"Color 1", x + 620, gy + 5, 65, 18, 10, false);
    PaintText(hdc, L"Color 2", x + 685, gy + 5, 65, 18, 10, false);
    FillRectColor(hdc, x + 625, gy + 25, x + 650, gy + 50, RGB(0, 0, 0));
    FillRectColor(hdc, x + 690, gy + 25, x + 715, gy + 50, RGB(255, 255, 255));
    const COLORREF palette[] = {
        RGB(0,0,0), RGB(128,128,128), RGB(128,0,0), RGB(255,0,0),
        RGB(128,128,0), RGB(255,255,0), RGB(0,128,0), RGB(0,255,0),
        RGB(0,128,128), RGB(0,255,255), RGB(0,0,128), RGB(0,0,255),
        RGB(128,0,128), RGB(255,0,255), RGB(128,64,0), RGB(255,255,255)
    };
    for (int i = 0; i < 16; ++i)
    {
        int px = x + 625 + (i % 8) * 20;
        int py = gy + 58 + (i / 8) * 20;
        FillRectColor(hdc, px, py, px + 18, py + 18, palette[i]);
    }

    // Canvas area.
    const int cx = x + 25;
    const int cy = y + 225;
    const int cw = w - 50;
    const int ch = h - 275;

    FillRectColor(hdc, cx, cy, cx + cw, cy + ch, RGB(255, 255, 255));

    HPEN canvasBorder = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
    HGDIOBJ oldCanvasPen = SelectObject(hdc, canvasBorder);
    HGDIOBJ oldCanvasBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, cx, cy, cx + cw, cy + ch);
    SelectObject(hdc, oldCanvasBrush);
    SelectObject(hdc, oldCanvasPen);
    DeleteObject(canvasBorder);

    // Handwritten-style wireframe sketches.
    HPEN ink = CreatePen(PS_SOLID, 3, RGB(25, 25, 25));
    HGDIOBJ oldInk = SelectObject(hdc, ink);
    HGDIOBJ oldInkBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    // F keys / Esc.
    Rectangle(hdc, cx + 35, cy + 35, cx + 350, cy + 105);
    Rectangle(hdc, cx + 55, cy + 50, cx + 120, cy + 92);
    PaintText(hdc, L"Esc", cx + 58, cy + 52, 60, 36, 13, false);
    PaintText(hdc, L"F keys", cx + 145, cy + 52, 110, 34, 15, false);

    // Keyboard.
    Rectangle(hdc, cx + 35, cy + 145, cx + 505, cy + 310);
    PaintText(hdc, L"Keyboard", cx + 145, cy + 205, 250, 48, 24, false);

    // Mouse sketch.
    Rectangle(hdc, cx + 570, cy + 35, cx + 750, cy + 205);
    MoveToEx(hdc, cx + 570, cy + 115, nullptr);
    LineTo(hdc, cx + 750, cy + 115);
    MoveToEx(hdc, cx + 660, cy + 115, nullptr);
    LineTo(hdc, cx + 660, cy + 205);
    PaintText(hdc, L"Mouse", cx + 595, cy + 65, 130, 38, 18, false);
    PaintText(hdc, L"2 side buttons", cx + 425, cy + 45, 135, 35, 13, false);
    PaintText(hdc, L"general layout", cx + 500, cy + 10, 160, 30, 13, false);

    // QWERTY / border sketch.
    Rectangle(hdc, cx + 35, cy + 360, cx + 505, cy + 470);
    PaintText(hdc, L"Q W E R T Y", cx + 55, cy + 390, 190, 38, 18, false);
    PaintText(hdc, L"U", cx + 285, cy + 390, 40, 38, 18, false);
    PaintText(hdc, L"no border", cx + 45, cy + 325, 120, 30, 13, false);
    PaintText(hdc, L"border", cx + 280, cy + 325, 90, 30, 13, false);
    PaintText(hdc, L"stylize border", cx + 380, cy + 480, 120, 30, 13, false);

    // Bottom-right sketch.
    Rectangle(hdc, cx + 570, cy + 330, cx + 735, cy + 485);
    Rectangle(hdc, cx + 545, cy + 370, cx + 580, cy + 420);
    MoveToEx(hdc, cx + 660, cy + 330, nullptr);
    LineTo(hdc, cx + 660, cy + 385);
    MoveToEx(hdc, cx + 660, cy + 385, nullptr);
    LineTo(hdc, cx + 720, cy + 385);
    // Scribble.
    for (int i = 0; i < 7; ++i)
    {
        MoveToEx(hdc, cx + 655 + i * 3, cy + 378, nullptr);
        LineTo(hdc, cx + 690 - i * 2, cy + 398);
    }

    SelectObject(hdc, oldInkBrush);
    SelectObject(hdc, oldInk);
    DeleteObject(ink);

    PaintText(hdc, L"646, 209px", cx + 5, cy + ch - 28, 110, 20, 10, false);
    PaintText(hdc, L"1750 x 2000px", cx + cw - 135, cy + ch - 28, 130, 20, 10, false);
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
        DrawKeyboard(hdc);
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

    // The keyboard is intentionally rendered only for the final GIF.
    // The interactive visualizer window itself stays lightweight.
    DrawPaintWindow(videoDC);

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