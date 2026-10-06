#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfuuid.h>

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================
// CONSTANTS
// ============================================================

static const int VIDEO_WIDTH = 1920;
static const int VIDEO_HEIGHT = 1080;

static const int FPS = 60;

static const UINT TIMER_ID = 1001;

static const double TAIL_SECONDS = 0.5;

// ============================================================
// DATA
// ============================================================

struct InputEvent
{
    double time = 0.0;
    std::wstring name;
    bool down = true;
};

static std::vector<InputEvent> events;

static double timelineDuration = 0.0;

static double previewTime = 0.0;

static bool playing = false;

static DWORD lastTick = 0;

// ============================================================
// WINDOWS
// ============================================================

static HWND mainWindow = nullptr;
static HWND editBox = nullptr;

static HWND loadButton = nullptr;
static HWND playButton = nullptr;
static HWND resetButton = nullptr;
static HWND exportButton = nullptr;

static HFONT uiFont = nullptr;

// ============================================================
// HELPERS
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
        result.find_last_of(
            L"\\/"
        );

    if (slash != std::wstring::npos)
        result.resize(slash);

    return result;
}

static std::wstring Trim(
    const std::wstring& value
)
{
    size_t start = 0;

    while (start < value.size() &&
           iswspace(value[start]))
    {
        start++;
    }

    size_t end = value.size();

    while (end > start &&
           iswspace(value[end - 1]))
    {
        end--;
    }

    return value.substr(
        start,
        end - start
    );
}

static std::vector<std::wstring> SplitWhitespace(
    const std::wstring& line
)
{
    std::vector<std::wstring> result;

    std::wistringstream stream(line);

    std::wstring token;

    while (stream >> token)
        result.push_back(token);

    return result;
}

// ============================================================
// TIMESTAMP PARSING
// ============================================================

static bool ParseClock(
    const std::vector<std::wstring>& parts,
    size_t& index,
    double& seconds
)
{
    if (index >= parts.size())
        return false;

    int hour = 0;
    int minute = 0;
    double sec = 0.0;

    // Format:
    // 10:46:46.533
    if (parts[index].find(L':') != std::wstring::npos)
    {
        wchar_t dummy;

        std::wistringstream ss(
            parts[index]
        );

        if (!(ss >> hour))
            return false;

        if (!(ss >> dummy) ||
            dummy != L':')
            return false;

        if (!(ss >> minute))
            return false;

        if (!(ss >> dummy) ||
            dummy != L':')
            return false;

        if (!(ss >> sec))
            return false;

        index++;
    }
    else
    {
        // Format:
        // 10 46 46.533
        if (index + 2 >= parts.size())
            return false;

        try
        {
            hour =
                std::stoi(
                    parts[index]
                );

            minute =
                std::stoi(
                    parts[index + 1]
                );

            sec =
                std::stod(
                    parts[index + 2]
                );
        }
        catch (...)
        {
            return false;
        }

        index += 3;
    }

    if (index >= parts.size())
        return false;

    std::wstring ampm =
        parts[index];

    index++;

    for (auto& c : ampm)
        c = static_cast<wchar_t>(
            towupper(c)
        );

    if (ampm != L"AM" &&
        ampm != L"PM")
    {
        return false;
    }

    if (hour == 12)
        hour = 0;

    if (ampm == L"PM")
        hour += 12;

    seconds =
        hour * 3600.0 +
        minute * 60.0 +
        sec;

    return true;
}

// ============================================================
// LOAD TIMELINE FROM EDIT BOX
// ============================================================

static std::wstring GetEditText()
{
    int length =
        GetWindowTextLengthW(
            editBox
        );

    if (length <= 0)
        return L"";

    std::wstring text;

    text.resize(
        length + 1
    );

    GetWindowTextW(
        editBox,
        &text[0],
        length + 1
    );

    text.resize(
        length
    );

    return text;
}

static bool ParseTimeline()
{
    events.clear();

    std::wstring text =
        GetEditText();

    std::wistringstream stream(
        text
    );

    std::wstring line;

    while (std::getline(
        stream,
        line
    ))
    {
        line = Trim(line);

        if (line.empty())
            continue;

        std::vector<std::wstring> parts =
            SplitWhitespace(line);

        if (parts.size() < 2)
            continue;

        size_t index = 0;

        double timestamp = 0.0;

        if (!ParseClock(
            parts,
            index,
            timestamp
        ))
        {
            continue;
        }

        if (index >= parts.size())
            continue;

        std::wstring name =
            parts[index];

        bool down = true;

        if (name.size() >= 3 &&
            name.substr(
                name.size() - 3
            ) == L"_UP")
        {
            down = false;

            name =
                name.substr(
                    0,
                    name.size() - 3
                );
        }

        InputEvent event;

        event.time = timestamp;
        event.name = name;
        event.down = down;

        events.push_back(event);
    }

    if (events.empty())
    {
        timelineDuration = 0.0;
        previewTime = 0.0;

        return false;
    }

    std::sort(
        events.begin(),
        events.end(),
        [](const InputEvent& a,
           const InputEvent& b)
        {
            return a.time < b.time;
        }
    );

    // Normalize first timestamp to zero.
    double first =
        events.front().time;

    // Handle midnight rollover.
    double previous = first;

    for (auto& event : events)
    {
        while (event.time < previous)
            event.time += 86400.0;

        previous = event.time;
    }

    for (auto& event : events)
        event.time -= first;

    timelineDuration =
        events.back().time +
        TAIL_SECONDS;

    previewTime = 0.0;

    return true;
}

// ============================================================
// APM
// ============================================================

static int CalculateAPM(
    double time
)
{
    int count = 0;

    for (const auto& event : events)
    {
        if (!event.down)
            continue;

        if (event.time <= time &&
            event.time > time - 60.0)
        {
            count++;
        }
    }

    return count;
}

// ============================================================
// PRESSED KEYS
// ============================================================

static bool IsPressed(
    const std::wstring& name,
    double time
)
{
    bool pressed = false;

    for (const auto& event : events)
    {
        if (event.time > time)
            break;

        if (event.name == name)
        {
            pressed = event.down;
        }
    }

    return pressed;
}

// ============================================================
// DRAWING HELPERS
// ============================================================

static void Fill(
    HDC dc,
    RECT rc,
    HBRUSH brush
)
{
    FillRect(
        dc,
        &rc,
        brush
    );
}

static void DrawCenteredText(
    HDC dc,
    const std::wstring& text,
    RECT rc,
    HFONT font,
    COLORREF color
)
{
    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                dc,
                font
            )
        );

    SetBkMode(
        dc,
        TRANSPARENT
    );

    SetTextColor(
        dc,
        color
    );

    DrawTextW(
        dc,
        text.c_str(),
        -1,
        &rc,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        dc,
        oldFont
    );
}

static void DrawKey(
    HDC dc,
    int x,
    int y,
    int width,
    int height,
    const std::wstring& label,
    bool pressed,
    HFONT font
)
{
    RECT rc =
    {
        x,
        y,
        x + width,
        y + height
    };

    HBRUSH brush =
        CreateSolidBrush(
            pressed
                ? RGB(220, 120, 30)
                : RGB(45, 45, 50)
        );

    Fill(
        dc,
        rc,
        brush
    );

    DeleteObject(
        brush
    );

    FrameRect(
        dc,
        &rc,
        static_cast<HBRUSH>(
            GetStockObject(
                WHITE_BRUSH
            )
        )
    );

    DrawCenteredText(
        dc,
        label,
        rc,
        font,
        RGB(255, 255, 255)
    );
}

// ============================================================
// KEYBOARD
// ============================================================

static void DrawKeyboard(
    HDC dc,
    HFONT font
)
{
    int startX = 130;
    int startY = 480;

    int keyW = 82;
    int keyH = 58;
    int gap = 7;

    // Number row.
    const wchar_t* row1[] =
    {
        L"1", L"2", L"3", L"4", L"5",
        L"6", L"7", L"8", L"9", L"0"
    };

    for (int i = 0; i < 10; i++)
    {
        DrawKey(
            dc,
            startX + i * (keyW + gap),
            startY,
            keyW,
            keyH,
            row1[i],
            IsPressed(
                row1[i],
                previewTime
            ),
            font
        );
    }

    // QWERTY.
    const wchar_t* row2[] =
    {
        L"Q", L"W", L"E", L"R", L"T",
        L"Y", L"U", L"I", L"O", L"P"
    };

    for (int i = 0; i < 10; i++)
    {
        DrawKey(
            dc,
            startX + 25 + i * (keyW + gap),
            startY + 70,
            keyW,
            keyH,
            row2[i],
            IsPressed(
                row2[i],
                previewTime
            ),
            font
        );
    }

    const wchar_t* row3[] =
    {
        L"A", L"S", L"D", L"F", L"G",
        L"H", L"J", L"K", L"L"
    };

    for (int i = 0; i < 9; i++)
    {
        DrawKey(
            dc,
            startX + 60 + i * (keyW + gap),
            startY + 140,
            keyW,
            keyH,
            row3[i],
            IsPressed(
                row3[i],
                previewTime
            ),
            font
        );
    }

    const wchar_t* row4[] =
    {
        L"Z", L"X", L"C", L"V", L"B",
        L"N", L"M"
    };

    for (int i = 0; i < 7; i++)
    {
        DrawKey(
            dc,
            startX + 105 + i * (keyW + gap),
            startY + 210,
            keyW,
            keyH,
            row4[i],
            IsPressed(
                row4[i],
                previewTime
            ),
            font
        );
    }

    // Space.
    DrawKey(
        dc,
        startX + 300,
        startY + 280,
        500,
        55,
        L"SPACE",
        IsPressed(
            L"Space",
            previewTime
        ),
        font
    );

    // Enter.
    DrawKey(
        dc,
        startX + 9 * (keyW + gap),
        startY + 140,
        120,
        keyH,
        L"ENTER",
        IsPressed(
            L"Enter",
            previewTime
        ),
        font
    );

    // CTRL / ALT / SHIFT.
    DrawKey(
        dc,
        startX,
        startY + 280,
        130,
        55,
        L"CTRL",
        IsPressed(
            L"Left Ctrl",
            previewTime
        ),
        font
    );

    DrawKey(
        dc,
        startX + 140,
        startY + 280,
        130,
        55,
        L"ALT",
        IsPressed(
            L"Left Alt",
            previewTime
        ),
        font
    );

    DrawKey(
        dc,
        startX + 820,
        startY + 280,
        130,
        55,
        L"SHIFT",
        IsPressed(
            L"Left Shift",
            previewTime
        ),
        font
    );
}

// ============================================================
// MOUSE
// ============================================================

static void DrawMouse(
    HDC dc,
    HFONT font
)
{
    int cx = 1650;
    int cy = 650;

    RECT body =
    {
        cx - 75,
        cy - 140,
        cx + 75,
        cy + 140
    };

    HBRUSH bodyBrush =
        CreateSolidBrush(
            RGB(45, 45, 50)
        );

    Fill(
        dc,
        body,
        bodyBrush
    );

    DeleteObject(
        bodyBrush
    );

    FrameRect(
        dc,
        &body,
        static_cast<HBRUSH>(
            GetStockObject(
                WHITE_BRUSH
            )
        )
    );

    // Left button.
    RECT left =
    {
        cx - 70,
        cy - 135,
        cx,
        cy - 30
    };

    HBRUSH leftBrush =
        CreateSolidBrush(
            IsPressed(
                L"LMB",
                previewTime
            )
            ? RGB(220, 120, 30)
            : RGB(60, 60, 65)
        );

    Fill(
        dc,
        left,
        leftBrush
    );

    DeleteObject(leftBrush);

    // Right button.
    RECT right =
    {
        cx,
        cy - 135,
        cx + 70,
        cy - 30
    };

    HBRUSH rightBrush =
        CreateSolidBrush(
            IsPressed(
                L"RMB",
                previewTime
            )
            ? RGB(220, 120, 30)
            : RGB(60, 60, 65)
        );

    Fill(
        dc,
        right,
        rightBrush
    );

    DeleteObject(rightBrush);

    RECT wheel =
    {
        cx - 12,
        cy - 20,
        cx + 12,
        cy + 35
    };

    HBRUSH wheelBrush =
        CreateSolidBrush(
            RGB(150, 150, 150)
        );

    Fill(
        dc,
        wheel,
        wheelBrush
    );

    DeleteObject(wheelBrush);

    DrawCenteredText(
        dc,
        L"LMB",
        left,
        font,
        RGB(255, 255, 255)
    );

    DrawCenteredText(
        dc,
        L"RMB",
        right,
        font,
        RGB(255, 255, 255)
    );
}

// ============================================================
// RENDER SCENE
// ============================================================

static void RenderScene(
    HDC dc
)
{
    RECT full =
    {
        0,
        0,
        VIDEO_WIDTH,
        VIDEO_HEIGHT
    };

    HBRUSH bg =
        CreateSolidBrush(
            RGB(18, 18, 22)
        );

    Fill(
        dc,
        full,
        bg
    );

    DeleteObject(bg);

    HFONT titleFont =
        CreateFontW(
            -70,
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

    HFONT normalFont =
        CreateFontW(
            -25,
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

    wchar_t apmText[64];

    swprintf_s(
        apmText,
        L"APM %d",
        CalculateAPM(
            previewTime
        )
    );

    RECT title =
    {
        0,
        35,
        VIDEO_WIDTH,
        120
    };

    DrawCenteredText(
        dc,
        apmText,
        title,
        titleFont,
        RGB(255, 255, 255)
    );

    wchar_t timeText[128];

    swprintf_s(
        timeText,
        L"TIME  %.3f",
        previewTime
    );

    RECT timeRect =
    {
        0,
        125,
        VIDEO_WIDTH,
        170
    };

    DrawCenteredText(
        dc,
        timeText,
        timeRect,
        normalFont,
        RGB(190, 190, 190)
    );

    DrawKeyboard(
        dc,
        normalFont
    );

    DrawMouse(
        dc,
        normalFont
    );

    // Progress bar.
    int barX = 150;
    int barY = 970;
    int barW = 1620;
    int barH = 14;

    RECT bar =
    {
        barX,
        barY,
        barX + barW,
        barY + barH
    };

    HBRUSH barBg =
        CreateSolidBrush(
            RGB(55, 55, 60)
        );

    Fill(
        dc,
        bar,
        barBg
    );

    DeleteObject(barBg);

    double ratio = 0.0;

    if (timelineDuration > 0.0)
    {
        ratio =
            previewTime /
            timelineDuration;

        if (ratio < 0.0)
            ratio = 0.0;

        if (ratio > 1.0)
            ratio = 1.0;
    }

    RECT progress = bar;

    progress.right =
        progress.left +
        static_cast<int>(
            barW * ratio
        );

    HBRUSH progressBrush =
        CreateSolidBrush(
            RGB(220, 120, 30)
        );

    Fill(
        dc,
        progress,
        progressBrush
    );

    DeleteObject(progressBrush);

    DeleteObject(titleFont);
    DeleteObject(normalFont);
}

// ============================================================
// CREATE VIDEO FRAME
// ============================================================

static bool CreateVideoFrame(
    std::vector<BYTE>& pixels
)
{
    pixels.resize(
        VIDEO_WIDTH *
        VIDEO_HEIGHT *
        4
    );

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

    HDC dc =
        CreateCompatibleDC(
            nullptr
        );

    if (!dc)
        return false;

    HBITMAP bitmap =
        CreateDIBSection(
            dc,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    if (!bitmap)
    {
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ old =
        SelectObject(
            dc,
            bitmap
        );

    RenderScene(dc);

    memcpy(
        pixels.data(),
        bits,
        pixels.size()
    );

    SelectObject(
        dc,
        old
    );

    DeleteObject(bitmap);
    DeleteDC(dc);

    return true;
}

// ============================================================
// MP4 EXPORT
// ============================================================

static bool ExportMP4()
{
    if (events.empty())
    {
        MessageBoxW(
            mainWindow,
            L"Load a timeline first.",
            L"APM Timeline Visualizer",
            MB_ICONWARNING
        );

        return false;
    }

    playing = false;

    EnableWindow(
        exportButton,
        FALSE
    );

    std::wstring outputPath =
        GetExeDirectory() +
        L"\\APM_Replay.mp4";

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
    {
        EnableWindow(
            exportButton,
            TRUE
        );

        return false;
    }

    IMFAttributes* attributes = nullptr;

    hr =
        MFCreateAttributes(
            &attributes,
            1
        );

    if (SUCCEEDED(hr))
    {
        hr =
            attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                TRUE
            );
    }

    IMFMediaType* outputType = nullptr;
    IMFMediaType* inputType = nullptr;
    IMFSinkWriter* writer = nullptr;

    if (SUCCEEDED(hr))
    {
        hr =
            MFCreateMediaType(
                &outputType
            );
    }

    if (SUCCEEDED(hr))
    {
        outputType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video
        );

        outputType->SetGUID(
            MF_MT_SUBTYPE,
            MFVideoFormat_H264
        );

        outputType->SetUINT32(
            MF_MT_AVG_BITRATE,
            8000000
        );

        MFSetAttributeSize(
            outputType,
            MF_MT_FRAME_SIZE,
            VIDEO_WIDTH,
            VIDEO_HEIGHT
        );

        MFSetAttributeRatio(
            outputType,
            MF_MT_FRAME_RATE,
            FPS,
            1
        );

        MFSetAttributeRatio(
            outputType,
            MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );
    }

    if (SUCCEEDED(hr))
    {
        hr =
            MFCreateMediaType(
                &inputType
            );
    }

    if (SUCCEEDED(hr))
    {
        inputType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video
        );

        inputType->SetGUID(
            MF_MT_SUBTYPE,
            MFVideoFormat_RGB32
        );

        MFSetAttributeSize(
            inputType,
            MF_MT_FRAME_SIZE,
            VIDEO_WIDTH,
            VIDEO_HEIGHT
        );

        MFSetAttributeRatio(
            inputType,
            MF_MT_FRAME_RATE,
            FPS,
            1
        );

        MFSetAttributeRatio(
            inputType,
            MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );
    }

    if (SUCCEEDED(hr))
    {
        hr =
            MFCreateSinkWriterFromURL(
                outputPath.c_str(),
                nullptr,
                attributes,
                &writer
            );
    }

    DWORD streamIndex = 0;

    if (SUCCEEDED(hr))
    {
        hr =
            writer->AddStream(
                outputType,
                &streamIndex
            );
    }

    if (SUCCEEDED(hr))
    {
        hr =
            writer->SetInputMediaType(
                streamIndex,
                inputType,
                nullptr
            );
    }

    if (SUCCEEDED(hr))
    {
        hr =
            writer->BeginWriting();
    }

    std::vector<BYTE> pixels;

    int totalFrames =
        static_cast<int>(
            std::ceil(
                timelineDuration *
                FPS
            )
        );

    previewTime = 0.0;

    for (int frame = 0;
         frame < totalFrames &&
         SUCCEEDED(hr);
         frame++)
    {
        previewTime =
            static_cast<double>(frame) /
            FPS;

        if (!CreateVideoFrame(
            pixels
        ))
        {
            hr =
                E_FAIL;

            break;
        }

        IMFMediaBuffer* buffer = nullptr;

        hr =
            MFCreateMemoryBuffer(
                static_cast<DWORD>(
                    pixels.size()
                ),
                &buffer
            );

        if (FAILED(hr))
            break;

        BYTE* destination = nullptr;
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
                pixels.data(),
                pixels.size()
            );

            buffer->Unlock();

            hr =
                buffer->SetCurrentLength(
                    static_cast<DWORD>(
                        pixels.size()
                    )
                );
        }

        IMFSample* sample = nullptr;

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
                static_cast<LONGLONG>(
                    frame *
                    10000000LL /
                    FPS
                );

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

        buffer->Release();

        if ((frame % FPS) == 0)
        {
            MSG msg;

            while (PeekMessageW(
                &msg,
                nullptr,
                0,
                0,
                PM_REMOVE
            ))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        hr =
            writer->Finalize();
    }

    if (writer)
        writer->Release();

    if (inputType)
        inputType->Release();

    if (outputType)
        outputType->Release();

    if (attributes)
        attributes->Release();

    MFShutdown();

    previewTime = 0.0;

    EnableWindow(
        exportButton,
        TRUE
    );

    if (SUCCEEDED(hr))
    {
        MessageBoxW(
            mainWindow,
            L"MP4 exported successfully.\n\nAPM_Replay.mp4",
            L"Export Complete",
            MB_OK |
            MB_ICONINFORMATION
        );

        return true;
    }

    MessageBoxW(
        mainWindow,
        L"MP4 export failed.",
        L"Export Error",
        MB_OK |
        MB_ICONERROR
    );

    return false;
}

// ============================================================
// WINDOW CREATION
// ============================================================

static void CreateControls(
    HWND hwnd,
    HINSTANCE instance
)
{
    uiFont =
        CreateFontW(
            -20,
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
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Arial"
        );

    editBox =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            WS_VSCROLL |
            WS_HSCROLL |
            ES_MULTILINE |
            ES_AUTOVSCROLL |
            ES_AUTOHSCROLL |
            ES_WANTRETURN,
            20,
            20,
            850,
            300,
            hwnd,
            nullptr,
            instance,
            nullptr
        );

    SendMessageW(
        editBox,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            uiFont
        ),
        TRUE
    );

    loadButton =
        CreateWindowW(
            L"BUTTON",
            L"Load Timeline",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            20,
            335,
            180,
            45,
            hwnd,
            reinterpret_cast<HMENU>(1001),
            instance,
            nullptr
        );

    playButton =
        CreateWindowW(
            L"BUTTON",
            L"Play",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            215,
            335,
            130,
            45,
            hwnd,
            reinterpret_cast<HMENU>(1002),
            instance,
            nullptr
        );

    resetButton =
        CreateWindowW(
            L"BUTTON",
            L"Reset",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            360,
            335,
            130,
            45,
            hwnd,
            reinterpret_cast<HMENU>(1003),
            instance,
            nullptr
        );

    exportButton =
        CreateWindowW(
            L"BUTTON",
            L"Export MP4",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            505,
            335,
            160,
            45,
            hwnd,
            reinterpret_cast<HMENU>(1004),
            instance,
            nullptr
        );

    HWND info =
        CreateWindowW(
            L"STATIC",
            L"Paste your APMOverlay timeline above, then click Load Timeline.",
            WS_CHILD |
            WS_VISIBLE,
            20,
            395,
            800,
            30,
            hwnd,
            nullptr,
            instance,
            nullptr
        );

    SendMessageW(
        info,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            uiFont
        ),
        TRUE
    );

    HWND controls[] =
    {
        loadButton,
        playButton,
        resetButton,
        exportButton
    };

    for (HWND control : controls)
    {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(
                uiFont
            ),
            TRUE
        );
    }
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
        CreateControls(
            hwnd,
            reinterpret_cast<LPCREATESTRUCTW>(
                lParam
            )->hInstance
        );

        SetTimer(
            hwnd,
            TIMER_ID,
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
        {
            if (ParseTimeline())
            {
                MessageBoxW(
                    hwnd,
                    L"Timeline loaded successfully.",
                    L"APM Timeline Visualizer",
                    MB_OK |
                    MB_ICONINFORMATION
                );

                InvalidateRect(
                    hwnd,
                    nullptr,
                    TRUE
                );
            }
            else
            {
                MessageBoxW(
                    hwnd,
                    L"No valid timeline events were found.",
                    L"Timeline Error",
                    MB_OK |
                    MB_ICONWARNING
                );
            }

            return 0;
        }

        case 1002:
        {
            if (events.empty())
            {
                if (!ParseTimeline())
                {
                    MessageBoxW(
                        hwnd,
                        L"Load a timeline first.",
                        L"APM Timeline Visualizer",
                        MB_OK |
                        MB_ICONWARNING
                    );

                    return 0;
                }
            }

            playing = !playing;

            SetWindowTextW(
                playButton,
                playing
                    ? L"Pause"
                    : L"Play"
            );

            lastTick =
                GetTickCount();

            return 0;
        }

        case 1003:
        {
            playing = false;

            previewTime = 0.0;

            SetWindowTextW(
                playButton,
                L"Play"
            );

            InvalidateRect(
                hwnd,
                nullptr,
                TRUE
            );

            return 0;
        }

        case 1004:
        {
            if (events.empty())
            {
                if (!ParseTimeline())
                {
                    MessageBoxW(
                        hwnd,
                        L"Load a timeline first.",
                        L"APM Timeline Visualizer",
                        MB_OK |
                        MB_ICONWARNING
                    );

                    return 0;
                }
            }

            ExportMP4();

            return 0;
        }
        }

        break;
    }

    case WM_TIMER:
    {
        if (wParam == TIMER_ID &&
            playing)
        {
            DWORD now =
                GetTickCount();

            double delta =
                static_cast<double>(
                    now - lastTick
                ) / 1000.0;

            lastTick = now;

            previewTime += delta;

            if (previewTime >=
                timelineDuration)
            {
                previewTime =
                    timelineDuration;

                playing = false;

                SetWindowTextW(
                    playButton,
                    L"Play"
                );
            }

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );
        }

        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};

        HDC dc =
            BeginPaint(
                hwnd,
                &ps
            );

        RECT client{};

        GetClientRect(
            hwnd,
            &client
        );

        HBRUSH brush =
            CreateSolidBrush(
                RGB(30, 30, 34)
            );

        FillRect(
            dc,
            &client,
            brush
        );

        DeleteObject(brush);

        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(
            hwnd,
            TIMER_ID
        );

        if (uiFont)
        {
            DeleteObject(
                uiFont
            );

            uiFont = nullptr;
        }

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
    int nCmdShow
)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(WNDCLASSEXW);

    wc.hInstance =
        hInstance;

    wc.lpfnWndProc =
        WindowProc;

    wc.lpszClassName =
        L"APMTimelineVisualizer";

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(
                BLACK_BRUSH
            )
        );

    if (!RegisterClassExW(&wc))
        return 1;

    mainWindow =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            920,
            480,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!mainWindow)
        return 1;

    ShowWindow(
        mainWindow,
        nCmdShow
    );

    UpdateWindow(
        mainWindow
    );

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0
    ) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
