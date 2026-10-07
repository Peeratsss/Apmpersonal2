#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cwctype>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ============================================================
// SETTINGS
// ============================================================

static const int GIF_WIDTH = 1200;
static const int GIF_HEIGHT = 300;
static const int FPS = 60;
static const int DEFAULT_WIDTH = 1100;
static const int DEFAULT_HEIGHT = 850;
static const UINT TIMER_PREVIEW = 1;

// The user's PNG is the artwork. It is stretched to exactly
// 1200x300 for the final GIF, as requested.
static const wchar_t* ARTWORK_FILE = L"image.png";

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

static std::vector<TimelineEvent> events;
static std::vector<std::wstring> heldKeys;
static std::vector<std::wstring> recentDowns;
static size_t processedEventIndex = 0;
static double currentTime = 0.0;
static double duration = 0.0;
static bool playing = false;
static LARGE_INTEGER performanceFrequency{};
static LARGE_INTEGER playbackStartPerformance{};
static double playbackStartTimelineTime = 0.0;

static ULONG_PTR gdiplusToken = 0;
static Bitmap* userImage = nullptr;
static UINT userImageWidth = 0;
static UINT userImageHeight = 0;

// ============================================================
// STRING HELPERS
// ============================================================

static std::wstring Trim(const std::wstring& input)
{
    size_t first = 0;
    while (first < input.size() && iswspace(input[first])) ++first;
    size_t last = input.size();
    while (last > first && iswspace(input[last - 1])) --last;
    return input.substr(first, last - first);
}

static std::wstring ToUpper(std::wstring value)
{
    for (wchar_t& c : value) c = towupper(c);
    return value;
}

static bool EndsWith(const std::wstring& value, const std::wstring& ending)
{
    if (value.size() < ending.size()) return false;
    return value.compare(value.size() - ending.size(), ending.size(), ending) == 0;
}

static std::wstring NormalizeInputName(std::wstring name)
{
    name = Trim(name);
    if (name.empty()) return name;
    name = ToUpper(name);
    if (EndsWith(name, L"_UP")) name.resize(name.size() - 3);
    name = Trim(name);
    return name;
}

// ============================================================
// PATH
// ============================================================

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path);
    size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) result.resize(slash);
    return result;
}

static std::wstring ArtworkPath()
{
    return GetExeDirectory() + L"\\" + ARTWORK_FILE;
}

// ============================================================
// TIMESTAMP PARSER
// ============================================================

static bool ParseTimestamp(const std::wstring& text, double& seconds)
{
    std::wstring s = Trim(text);
    for (wchar_t& c : s) if (c == L':') c = L' ';

    std::wstringstream ss(s);
    int hour = 0;
    int minute = 0;
    double second = 0.0;
    std::wstring ampm;

    if (!(ss >> hour >> minute >> second >> ampm)) return false;
    ampm = ToUpper(ampm);
    if (ampm != L"AM" && ampm != L"PM") return false;
    if (hour < 1 || hour > 12 || minute < 0 || minute > 59 || second < 0.0 || second >= 60.0) return false;

    if (ampm == L"AM") {
        if (hour == 12) hour = 0;
    } else if (hour != 12) {
        hour += 12;
    }

    seconds = hour * 3600.0 + minute * 60.0 + second;
    return true;
}

// Forward declarations used by the parser.
static void ResetPlaybackState();
static void ProcessEventsTo(double targetTime);

// ============================================================
// TIMELINE PARSER
// ============================================================

static bool ParseTimeline(const std::wstring& text)
{
    std::vector<TimelineEvent> parsed;
    std::wstringstream stream(text);
    std::wstring line;
    double previousClock = -1.0;
    double dayOffset = 0.0;

    while (std::getline(stream, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == L'#') continue;

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
            if (amPos != std::wstring::npos) pos = amPos;
            if (pmPos != std::wstring::npos && (pos == std::wstring::npos || pmPos < pos)) pos = pmPos;
            if (pos == std::wstring::npos) continue;

            size_t timestampEnd = pos + 3;
            timestampText = Trim(line.substr(0, timestampEnd));
            inputName = Trim(line.substr(timestampEnd));
        }

        if (timestampText.empty() || inputName.empty()) continue;

        double clockSeconds = 0.0;
        if (!ParseTimestamp(timestampText, clockSeconds)) continue;

        if (previousClock >= 0.0 && clockSeconds < previousClock) dayOffset += 86400.0;
        previousClock = clockSeconds;

        TimelineEvent event;
        event.time = clockSeconds + dayOffset;
        event.down = !EndsWith(ToUpper(inputName), L"_UP");
        event.name = NormalizeInputName(inputName);
        if (!event.name.empty()) parsed.push_back(event);
    }

    if (parsed.empty()) return false;

    double firstTime = parsed.front().time;
    for (TimelineEvent& event : parsed) {
        event.time -= firstTime;
        if (event.time < 0.0) event.time = 0.0;
    }

    std::stable_sort(parsed.begin(), parsed.end(), [](const TimelineEvent& a, const TimelineEvent& b) {
        return a.time < b.time;
    });

    events = std::move(parsed);
    duration = std::max(0.5, events.back().time + 0.5);
    currentTime = 0.0;
    ResetPlaybackState();
    return true;
}

// ============================================================
// HELD STATE
// ============================================================

static bool IsHeld(const std::wstring& name)
{
    return std::find(heldKeys.begin(), heldKeys.end(), ToUpper(name)) != heldKeys.end();
}

static void SetHeld(const std::wstring& name, bool held)
{
    std::wstring n = ToUpper(name);
    auto it = std::find(heldKeys.begin(), heldKeys.end(), n);
    if (held) {
        if (it == heldKeys.end()) heldKeys.push_back(n);
    } else if (it != heldKeys.end()) {
        heldKeys.erase(it);
    }
}

static void ResetPlaybackState()
{
    heldKeys.clear();
    processedEventIndex = 0;
}

static void ProcessEventsTo(double targetTime)
{
    while (processedEventIndex < events.size() && events[processedEventIndex].time <= targetTime + 1e-9)
    {
        const TimelineEvent& event = events[processedEventIndex];
        SetHeld(event.name, event.down);
        ++processedEventIndex;
    }
}

// ============================================================
// APM / RECENT INPUTS
// ============================================================

static int CalculateAPM(double time)
{
    double start = time - 60.0;
    int count = 0;
    for (const TimelineEvent& event : events) {
        if (!event.down) continue;
        if (event.time > time) break;
        if (event.time >= start) ++count;
    }
    return count;
}

static std::vector<std::wstring> GetLastTenInputs(double time)
{
    std::vector<std::wstring> result;
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->time > time || !it->down) continue;
        result.push_back(it->name);
        if (result.size() == 10) break;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

// ============================================================
// SECOND TIMELINE
// ============================================================

static std::wstring BuildSecondTimelineText()
{
    if (events.empty()) return L"";
    std::wstringstream output;
    int totalSeconds = std::max(1, static_cast<int>(std::ceil(duration)));

    for (int second = 1; second <= totalSeconds; ++second)
    {
        double startTime = static_cast<double>(second - 1);
        double endTime = static_cast<double>(second);
        std::vector<std::wstring> active;

        ResetPlaybackState();
        ProcessEventsTo(startTime);
        for (const auto& key : heldKeys) active.push_back(key);

        for (const TimelineEvent& event : events) {
            if (event.time < startTime) continue;
            if (event.time >= endTime) break;
            if (event.down && std::find(active.begin(), active.end(), event.name) == active.end()) active.push_back(event.name);
        }

        output << second << L"s  ";
        if (active.empty()) output << L"-";
        else {
            for (size_t i = 0; i < active.size(); ++i) {
                if (i) output << L" + ";
                output << active[i];
            }
        }
        if (second < totalSeconds) output << L"\r\n";
    }
    return output.str();
}

static void UpdateSecondTimeline()
{
    if (!hwndSecondTimeline) return;
    std::wstring text = BuildSecondTimelineText();
    SetWindowTextW(hwndSecondTimeline, text.c_str());
    ResetPlaybackState();
    ProcessEventsTo(currentTime);
}

// ============================================================
// EDIT CONTROL
// ============================================================

static std::wstring GetEditText(HWND edit)
{
    int length = GetWindowTextLengthW(edit);
    if (length <= 0) return L"";
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// ============================================================
// DRAW HELPERS
// ============================================================

static void FillRectColor(HDC hdc, int left, int top, int right, int bottom, COLORREF color)
{
    RECT r{left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &r, brush);
    DeleteObject(brush);
}

static void DrawCenteredText(HDC hdc, const std::wstring& text, int left, int top, int right, int bottom, int fontSize, bool bold, COLORREF color)
{
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT r{left, top, right, bottom};
    DrawTextW(hdc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

// ============================================================
// USER PNG
// ============================================================

static bool LoadUserImage()
{
    if (userImage) return true;
    std::wstring path = ArtworkPath();
    userImage = new Bitmap(path.c_str(), FALSE);
    if (!userImage || userImage->GetLastStatus() != Ok) {
        delete userImage;
        userImage = nullptr;
        return false;
    }
    userImageWidth = userImage->GetWidth();
    userImageHeight = userImage->GetHeight();
    return userImageWidth > 0 && userImageHeight > 0;
}

static bool IsGreenPixel(BYTE r, BYTE g, BYTE b)
{
    return g > 100 && g > static_cast<BYTE>(r * 1.25) && g > static_cast<BYTE>(b * 1.25);
}

struct ImageRegion
{
    const wchar_t* name;
    float left;
    float top;
    float right;
    float bottom;
};

// These are the 10 button regions used for inversion.
// They are based on the user's 1000x300 drawing and are normalized
// so the artwork can be rendered at exactly 1200x300.
static const ImageRegion IMAGE_REGIONS[] =
{
    {L"1", 0.095f, 0.420f, 0.120f, 0.570f},
    {L"2", 0.120f, 0.420f, 0.145f, 0.570f},
    {L"3", 0.145f, 0.420f, 0.170f, 0.570f},
    {L"4", 0.170f, 0.420f, 0.195f, 0.570f},
    {L"5", 0.195f, 0.420f, 0.220f, 0.570f},
    {L"Q", 0.105f, 0.555f, 0.140f, 0.690f},
    {L"W", 0.135f, 0.555f, 0.170f, 0.690f},
    {L"E", 0.165f, 0.555f, 0.200f, 0.690f},
    {L"R", 0.195f, 0.555f, 0.230f, 0.690f},
    {L"T", 0.225f, 0.555f, 0.260f, 0.690f}
};

static const ImageRegion* FindImageRegion(const std::wstring& name)
{
    std::wstring n = ToUpper(name);
    for (const auto& region : IMAGE_REGIONS) {
        if (n == region.name) return &region;
    }
    return nullptr;
}

// ============================================================
// SCENE BITMAP RENDERING
// ============================================================

static void DrawArtwork(HDC hdc, int width, int height)
{
    if (!LoadUserImage()) {
        FillRectColor(hdc, 0, 0, width, height, RGB(0, 255, 0));
        DrawCenteredText(hdc, L"image.png NOT FOUND", 0, 0, width, height, 34, true, RGB(0, 0, 0));
        return;
    }

    Graphics graphics(hdc);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.DrawImage(userImage, Rect(0, 0, width, height), 0, 0,
        static_cast<INT>(userImageWidth), static_cast<INT>(userImageHeight), UnitPixel);
}

static void InvertPressedRegions(HDC hdc, int width, int height)
{
    for (const auto& region : IMAGE_REGIONS)
    {
        if (!IsHeld(region.name)) continue;

        int left = static_cast<int>(region.left * width);
        int top = static_cast<int>(region.top * height);
        int right = static_cast<int>(region.right * width);
        int bottom = static_cast<int>(region.bottom * height);

        for (int y = top; y < bottom; ++y)
        {
            for (int x = left; x < right; ++x)
            {
                COLORREF c = GetPixel(hdc, x, y);
                if (c == CLR_INVALID) continue;
                BYTE r = GetRValue(c);
                BYTE g = GetGValue(c);
                BYTE b = GetBValue(c);
                if (IsGreenPixel(r, g, b)) continue;
                SetPixelV(hdc, x, y, RGB(255 - r, 255 - g, 255 - b));
            }
        }
    }
}

static void DrawDynamicInfo(HDC hdc, int width, int height)
{
    // The user's drawing has the APM and Last 10 areas already drawn.
    // These text overlays only replace the changing values.
    int apm = CalculateAPM(currentTime);
    wchar_t apmText[32]{};
    swprintf_s(apmText, L"%d", apm);

    DrawCenteredText(hdc, apmText,
        static_cast<int>(width * 0.485), static_cast<int>(height * 0.405),
        static_cast<int>(width * 0.560), static_cast<int>(height * 0.545),
        24, true, RGB(0, 0, 0));

    std::vector<std::wstring> last = GetLastTenInputs(currentTime);
    int x1 = static_cast<int>(width * 0.495);
    int x2 = static_cast<int>(width * 0.675);
    int y = static_cast<int>(height * 0.565);
    int rowH = 20;

    for (size_t i = 0; i < last.size(); ++i)
    {
        DrawCenteredText(hdc, last[i], x1, y + static_cast<int>(i) * rowH,
            x2, y + static_cast<int>(i + 1) * rowH, 13, true, RGB(0, 0, 0));
    }
}

static void RenderFinalArtwork(HDC hdc, int width, int height)
{
    DrawArtwork(hdc, width, height);
    InvertPressedRegions(hdc, width, height);
    DrawDynamicInfo(hdc, width, height);
}

static void RenderPreview(HDC hdc, int width, int height)
{
    FillRectColor(hdc, 0, 0, width, height, RGB(18, 18, 22));

    int apm = CalculateAPM(currentTime);
    wchar_t apmText[64]{};
    swprintf_s(apmText, L"APM %d", apm);
    DrawCenteredText(hdc, apmText, 0, 40, width, 130, 58, true, RGB(255, 255, 255));

    wchar_t timeText[64]{};
    swprintf_s(timeText, L"%02d:%05.2f", static_cast<int>(currentTime / 60.0), fmod(currentTime, 60.0));
    DrawCenteredText(hdc, timeText, 0, 135, width, 180, 24, false, RGB(255, 255, 255));

    // Normal preview intentionally does NOT draw the PNG keyboard/mouse artwork.
    const int barX = 60;
    const int barY = height - 40;
    const int barW = std::max(100, width - 120);
    const int barH = 16;
    FillRectColor(hdc, barX, barY, barX + barW, barY + barH, RGB(50, 50, 55));

    double progress = duration > 0.0 ? currentTime / duration : 0.0;
    progress = std::max(0.0, std::min(1.0, progress));
    FillRectColor(hdc, barX, barY, barX + static_cast<int>(barW * progress), barY + barH, RGB(255, 170, 40));
}

// ============================================================
// BITMAP SURFACE
// ============================================================

static HBITMAP Create32BitBitmap(HDC referenceDC, int width, int height, void** bits, HDC* memoryDC)
{
    *bits = nullptr;
    *memoryDC = CreateCompatibleDC(referenceDC);
    if (!*memoryDC) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HBITMAP bitmap = CreateDIBSection(*memoryDC, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(*memoryDC);
        *memoryDC = nullptr;
    }
    return bitmap;
}

// ============================================================
// GIF ENCODER HELPERS
// ============================================================

static int GetEncoderClsid(const WCHAR* mimeType, CLSID* clsid)
{
    UINT num = 0;
    UINT size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buffer(size);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mimeType) == 0) {
            *clsid = codecs[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void SetGifProperty(Bitmap* bitmap, PROPID id, WORD type, DWORD length, void* value)
{
    PropertyItem item{};
    item.id = id;
    item.length = length;
    item.type = type;
    item.value = value;
    bitmap->SetPropertyItem(&item);
}

static Bitmap* RenderGifFrame(double time)
{
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;

    void* bits = nullptr;
    HDC dc = nullptr;
    HBITMAP dib = Create32BitBitmap(screen, GIF_WIDTH, GIF_HEIGHT, &bits, &dc);
    ReleaseDC(nullptr, screen);
    if (!dib) return nullptr;

    HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, dib));

    currentTime = time;
    ResetPlaybackState();
    ProcessEventsTo(currentTime);
    RenderFinalArtwork(dc, GIF_WIDTH, GIF_HEIGHT);

    SelectObject(dc, old);

    Bitmap* result = new Bitmap(dib, nullptr);
    DeleteObject(dib);
    DeleteDC(dc);

    if (!result || result->GetLastStatus() != Ok) {
        delete result;
        return nullptr;
    }
    return result;
}

// ============================================================
// EXPORT GIF
// ============================================================

static bool ExportGIF()
{
    if (events.empty())
    {
        MessageBoxW(hwndMain, L"Load a timeline first.", L"Export GIF", MB_OK | MB_ICONWARNING);
        return false;
    }

    if (!LoadUserImage())
    {
        std::wstring msg = L"image.png was not found beside the EXE.\n\nExpected:\n" + ArtworkPath();
        MessageBoxW(hwndMain, msg.c_str(), L"Export GIF", MB_OK | MB_ICONERROR);
        return false;
    }

    CLSID gifClsid{};
    if (GetEncoderClsid(L"image/gif", &gifClsid) < 0)
    {
        MessageBoxW(hwndMain, L"Windows GIF encoder was not found.", L"Export GIF", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring outputPath = GetExeDirectory() + L"\\APM_Replay.gif";
    DeleteFileW(outputPath.c_str());

    const long long totalFrames = std::max<long long>(1,
        static_cast<long long>(std::ceil(duration * static_cast<double>(FPS))));

    // GIF stores delays in 1/100 second units. 60 FPS is represented by 2/100.
    const ULONG frameDelay = 2;
    const WORD loopCount = 0; // infinite loop

    EncoderParameters startParams{};
    startParams.Count = 1;
    startParams.Parameter[0].Guid = EncoderSaveFlag;
    startParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    startParams.Parameter[0].NumberOfValues = 1;
    ULONG multiFrame = EncoderValueMultiFrame;
    startParams.Parameter[0].Value = &multiFrame;

    Bitmap* firstFrame = RenderGifFrame(0.0);
    if (!firstFrame)
    {
        MessageBoxW(hwndMain, L"Could not render the first GIF frame.", L"Export GIF", MB_OK | MB_ICONERROR);
        return false;
    }

    SetGifProperty(firstFrame, PropertyTagFrameDelay, PropertyTagTypeLong,
        sizeof(ULONG), const_cast<ULONG*>(&frameDelay));
    SetGifProperty(firstFrame, PropertyTagLoopCount, PropertyTagTypeShort,
        sizeof(WORD), const_cast<WORD*>(&loopCount));

    Status status = firstFrame->Save(outputPath.c_str(), &gifClsid, &startParams);
    if (status != Ok)
    {
        delete firstFrame;
        DeleteFileW(outputPath.c_str());
        MessageBoxW(hwndMain, L"Could not start GIF encoding.", L"Export GIF", MB_OK | MB_ICONERROR);
        return false;
    }

    EncoderParameters addParams{};
    addParams.Count = 1;
    addParams.Parameter[0].Guid = EncoderSaveFlag;
    addParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    addParams.Parameter[0].NumberOfValues = 1;
    ULONG nextFrame = EncoderValueFrameDimensionTime;
    addParams.Parameter[0].Value = &nextFrame;

    bool success = true;

    for (long long frame = 1; frame < totalFrames; ++frame)
    {
        const double t = std::min(
            duration,
            static_cast<double>(frame) / static_cast<double>(FPS));

        Bitmap* image = RenderGifFrame(t);
        if (!image)
        {
            success = false;
            break;
        }

        SetGifProperty(image, PropertyTagFrameDelay, PropertyTagTypeLong,
            sizeof(ULONG), const_cast<ULONG*>(&frameDelay));

        status = firstFrame->SaveAdd(image, &addParams);
        delete image;

        if (status != Ok)
        {
            success = false;
            break;
        }
    }

    if (success)
    {
        EncoderParameters flushParams{};
        flushParams.Count = 1;
        flushParams.Parameter[0].Guid = EncoderSaveFlag;
        flushParams.Parameter[0].Type = EncoderParameterValueTypeLong;
        flushParams.Parameter[0].NumberOfValues = 1;
        ULONG flush = EncoderValueFlush;
        flushParams.Parameter[0].Value = &flush;
        status = firstFrame->SaveAdd(&flushParams);
        if (status != Ok) success = false;
    }

    delete firstFrame;

    currentTime = 0.0;
    ResetPlaybackState();
    InvalidateRect(hwndMain, nullptr, TRUE);

    if (success)
    {
        wchar_t message[512]{};
        swprintf_s(message,
            L"GIF exported successfully.\n\n%ls\n\nSize: %dx%d\nFrames: %lld\nFPS: %d",
            outputPath.c_str(), GIF_WIDTH, GIF_HEIGHT, totalFrames, FPS);
        MessageBoxW(hwndMain, message, L"Export Complete", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        DeleteFileW(outputPath.c_str());
        MessageBoxW(hwndMain, L"GIF export failed.", L"Export GIF", MB_OK | MB_ICONERROR);
    }

    return success;
}

// ============================================================
// LOAD TIMELINE
// ============================================================

static void LoadTimelineFromEditor()
{
    std::wstring text = GetEditText(hwndTimeline);
    if (!ParseTimeline(text)) {
        MessageBoxW(hwndMain,
            L"No valid timeline events were found.\n\nExample:\n"
            L"10:46:46.533 PM\t1\n"
            L"10:46:46.638 PM\t2\n"
            L"10:46:46.654 PM\t1_UP",
            L"Timeline Error", MB_OK | MB_ICONWARNING);
        return;
    }

    playing = false;
    UpdateSecondTimeline();
    SetWindowTextW(hwndPlay, L"Play");
    InvalidateRect(hwndMain, nullptr, TRUE);
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            CreateWindowW(L"STATIC", L"Raw Timeline", WS_CHILD | WS_VISIBLE,
                20, 2, 520, 18, hwnd, nullptr, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Per-Second Timeline", WS_CHILD | WS_VISIBLE,
                560, 2, 520, 18, hwnd, nullptr, nullptr, nullptr);

            hwndTimeline = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL,
                20, 20, 520, 220, hwnd, nullptr, nullptr, nullptr);

            SendMessageW(hwndTimeline, WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

            hwndSecondTimeline = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                L"Load a timeline to generate the per-second view.",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_READONLY | WS_VSCROLL | WS_HSCROLL,
                560, 20, 520, 220, hwnd, nullptr, nullptr, nullptr);

            SendMessageW(hwndSecondTimeline, WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

            hwndLoad = CreateWindowW(L"BUTTON", L"Load Timeline",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 255, 150, 40, hwnd, reinterpret_cast<HMENU>(1001), nullptr, nullptr);

            hwndPlay = CreateWindowW(L"BUTTON", L"Play",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                180, 255, 100, 40, hwnd, reinterpret_cast<HMENU>(1002), nullptr, nullptr);

            hwndReset = CreateWindowW(L"BUTTON", L"Reset",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                290, 255, 100, 40, hwnd, reinterpret_cast<HMENU>(1003), nullptr, nullptr);

            hwndExportGIF = CreateWindowW(L"BUTTON", L"Export GIF",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                400, 255, 130, 40, hwnd, reinterpret_cast<HMENU>(1004), nullptr, nullptr);

            QueryPerformanceFrequency(&performanceFrequency);
            QueryPerformanceCounter(&playbackStartPerformance);
            playbackStartTimelineTime = 0.0;

            SetTimer(hwnd, TIMER_PREVIEW, 16, nullptr);
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
                    if (events.empty()) return 0;
                    if (playing) {
                        playing = false;
                    } else {
                        if (currentTime >= duration) {
                            currentTime = 0.0;
                            ResetPlaybackState();
                        }
                        QueryPerformanceCounter(&playbackStartPerformance);
                        playbackStartTimelineTime = currentTime;
                        playing = true;
                    }
                    SetWindowTextW(hwndPlay, playing ? L"Pause" : L"Play");
                    return 0;

                case 1003:
                    playing = false;
                    currentTime = 0.0;
                    playbackStartTimelineTime = 0.0;
                    QueryPerformanceCounter(&playbackStartPerformance);
                    ResetPlaybackState();
                    SetWindowTextW(hwndPlay, L"Play");
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;

                case 1004:
                    ExportGIF();
                    return 0;
            }
            break;
        }

        case WM_TIMER:
            if (wParam == TIMER_PREVIEW && playing)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                double elapsed = static_cast<double>(now.QuadPart - playbackStartPerformance.QuadPart) /
                    static_cast<double>(performanceFrequency.QuadPart);
                if (elapsed < 0.0) elapsed = 0.0;
                currentTime = playbackStartTimelineTime + elapsed;

                if (currentTime >= duration) {
                    currentTime = duration;
                    playing = false;
                    SetWindowTextW(hwndPlay, L"Play");
                    playbackStartTimelineTime = duration;
                    ResetPlaybackState();
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);

            const int sceneTop = 315;
            FillRectColor(hdc, 0, sceneTop, client.right, client.bottom, RGB(18, 18, 22));

            int previewWidth = client.right;
            int previewHeight = client.bottom - sceneTop;
            if (previewWidth > 0 && previewHeight > 0)
            {
                HDC memDC = CreateCompatibleDC(hdc);
                void* bits = nullptr;
                HBITMAP bitmap = Create32BitBitmap(hdc, previewWidth, previewHeight, &bits, &memDC);
                if (bitmap)
                {
                    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, bitmap));
                    ResetPlaybackState();
                    ProcessEventsTo(currentTime);
                    RenderPreview(memDC, previewWidth, previewHeight);
                    BitBlt(hdc, 0, sceneTop, previewWidth, previewHeight, memDC, 0, 0, SRCCOPY);
                    SelectObject(memDC, oldBitmap);
                    DeleteObject(bitmap);
                }
                DeleteDC(memDC);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
        {
            int width = LOWORD(lParam);
            int paneWidth = std::max(100, (width - 60) / 2);
            if (hwndTimeline) MoveWindow(hwndTimeline, 20, 20, paneWidth, 220, TRUE);
            if (hwndSecondTimeline) MoveWindow(hwndSecondTimeline, 40 + paneWidth, 20, paneWidth, 220, TRUE);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_PREVIEW);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// ENTRY POINT
// ============================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    GdiplusStartupInput gdiplusStartupInput{};
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok)
        return 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"APMTimelineVisualizer";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));

    if (!RegisterClassW(&wc)) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    hwndMain = CreateWindowExW(0, wc.lpszClassName, L"APM Timeline Visualizer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        DEFAULT_WIDTH, DEFAULT_HEIGHT, nullptr, nullptr, hInstance, nullptr);

    if (!hwndMain) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    ShowWindow(hwndMain, nCmdShow);
    UpdateWindow(hwndMain);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete userImage;
    userImage = nullptr;
    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}
