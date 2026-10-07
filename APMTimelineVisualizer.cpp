#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
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
