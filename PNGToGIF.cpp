#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <algorithm>
#include <vector>
#include <string>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ============================================================
// SETTINGS
// ============================================================

static const int WINDOW_W = 560;
static const int WINDOW_H = 310;

static const int MAX_WIDTH = 1280;
static const int MAX_HEIGHT = 720;

static const int DEFAULT_FPS = 30;

// ============================================================
// GLOBALS
// ============================================================

static HWND g_hWindow = NULL;
static HWND g_hFolderEdit = NULL;
static HWND g_hOutputEdit = NULL;
static HWND g_hFpsEdit = NULL;
static HWND g_hStatus = NULL;
static HWND g_hConvertButton = NULL;

static ULONG_PTR g_gdiplusToken = 0;

// ============================================================
// SMALL HELPERS
// ============================================================

static std::wstring ToLowerString(const std::wstring& input)
{
    std::wstring result = input;

    for (size_t i = 0; i < result.size(); ++i)
    {
        result[i] =
            static_cast<wchar_t>(
                towlower(result[i]));
    }

    return result;
}

static bool IsPNGFile(const std::wstring& filename)
{
    std::wstring lower =
        ToLowerString(filename);

    if (lower.length() < 4)
        return false;

    return
        lower.substr(
            lower.length() - 4) == L".png";
}

static std::wstring GetWindowString(HWND hwnd)
{
    int length =
        GetWindowTextLengthW(hwnd);

    if (length <= 0)
        return L"";

    std::vector<wchar_t> buffer(
        static_cast<size_t>(length) + 1);

    GetWindowTextW(
        hwnd,
        &buffer[0],
        length + 1);

    return std::wstring(
        &buffer[0]);
}

static void SetStatus(const std::wstring& text)
{
    if (g_hStatus)
    {
        SetWindowTextW(
            g_hStatus,
            text.c_str());

        UpdateWindow(g_hStatus);
    }
}

// ============================================================
// NATURAL SORT
//
// frame_1.png
// frame_2.png
// frame_10.png
//
// rather than:
//
// frame_1.png
// frame_10.png
// frame_2.png
// ============================================================

static bool NaturalLess(
    const std::wstring& a,
    const std::wstring& b)
{
    size_t i = 0;
    size_t j = 0;

    while (i < a.length() &&
           j < b.length())
    {
        wchar_t ca =
            static_cast<wchar_t>(
                towlower(a[i]));

        wchar_t cb =
            static_cast<wchar_t>(
                towlower(b[j]));

        if (iswdigit(ca) &&
            iswdigit(cb))
        {
            size_t startA = i;
            size_t startB = j;

            while (i < a.length() &&
                   iswdigit(a[i]))
            {
                ++i;
            }

            while (j < b.length() &&
                   iswdigit(b[j]))
            {
                ++j;
            }

            std::wstring numberA =
                a.substr(
                    startA,
                    i - startA);

            std::wstring numberB =
                b.substr(
                    startB,
                    j - startB);

            size_t nonZeroA =
                numberA.find_first_not_of(L'0');

            size_t nonZeroB =
                numberB.find_first_not_of(L'0');

            std::wstring valueA =
                (nonZeroA == std::wstring::npos)
                ? L"0"
                : numberA.substr(nonZeroA);

            std::wstring valueB =
                (nonZeroB == std::wstring::npos)
                ? L"0"
                : numberB.substr(nonZeroB);

            if (valueA.length() != valueB.length())
            {
                return valueA.length() <
                       valueB.length();
            }

            if (valueA != valueB)
            {
                return valueA < valueB;
            }

            // Same numeric value.
            // Fewer leading zeroes first.
            if (numberA.length() !=
                numberB.length())
            {
                return numberA.length() <
                       numberB.length();
            }

            continue;
        }

        if (ca != cb)
            return ca < cb;

        ++i;
        ++j;
    }

    return a.length() < b.length();
}

static bool IsAlreadySorted(
    const std::vector<std::wstring>& files)
{
    if (files.size() < 2)
        return true;

    for (size_t i = 1;
         i < files.size();
         ++i)
    {
        if (NaturalLess(
                files[i],
                files[i - 1]))
        {
            return false;
        }
    }

    return true;
}

// ============================================================
// FOLDER PICKER
// ============================================================

static std::wstring BrowseForFolder(HWND owner)
{
    BROWSEINFOW info;
    ZeroMemory(
        &info,
        sizeof(info));

    info.hwndOwner = owner;
    info.ulFlags =
        BIF_RETURNONLYFSDIRS |
        BIF_NEWDIALOGSTYLE;

    info.lpszTitle =
        L"Select the folder containing PNG frames.";

    PIDLIST_ABSOLUTE pidl =
        SHBrowseForFolderW(&info);

    if (!pidl)
        return L"";

    wchar_t path[MAX_PATH];
    ZeroMemory(
        path,
        sizeof(path));

    std::wstring result;

    if (SHGetPathFromIDListW(
            pidl,
            path))
    {
        result = path;
    }

    CoTaskMemFree(pidl);

    return result;
}

// ============================================================
// ENUMERATE PNG FILES
//
// Uses Win32 FindFirstFile instead of std::filesystem.
// This works with the existing MSVC build command.
// ============================================================

static bool GetPNGFiles(
    const std::wstring& folder,
    std::vector<std::wstring>& files)
{
    files.clear();

    std::wstring searchPath = folder;

    if (!searchPath.empty())
    {
        wchar_t last =
            searchPath[searchPath.length() - 1];

        if (last != L'\\' &&
            last != L'/')
        {
            searchPath += L'\\';
        }
    }

    searchPath += L"*";

    WIN32_FIND_DATAW data;
    ZeroMemory(
        &data,
        sizeof(data));

    HANDLE hFind =
        FindFirstFileW(
            searchPath.c_str(),
            &data);

    if (hFind == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        if (data.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }

        std::wstring filename =
            data.cFileName;

        if (!IsPNGFile(filename))
            continue;

        std::wstring fullPath =
            folder;

        if (!fullPath.empty())
        {
            wchar_t last =
                fullPath[fullPath.length() - 1];

            if (last != L'\\' &&
                last != L'/')
            {
                fullPath += L'\\';
            }
        }

        fullPath += filename;

        files.push_back(fullPath);

    } while (FindNextFileW(
        hFind,
        &data));

    FindClose(hFind);

    return !files.empty();
}

// ============================================================
// GET FILENAME FROM PATH
// ============================================================

static std::wstring GetFilename(
    const std::wstring& path)
{
    size_t position =
        path.find_last_of(
            L"\\/");

    if (position == std::wstring::npos)
        return path;

    return path.substr(
        position + 1);
}

// ============================================================
// GET FILE EXTENSION
// ============================================================

static std::wstring GetExtension(
    const std::wstring& path)
{
    std::wstring filename =
        GetFilename(path);

    size_t position =
        filename.find_last_of(L'.');

    if (position == std::wstring::npos)
        return L"";

    return filename.substr(
        position);
}

// ============================================================
// GET FPS
// ============================================================

static int GetFPS()
{
    std::wstring value =
        GetWindowString(
            g_hFpsEdit);

    int fps =
        _wtoi(value.c_str());

    if (fps < 1)
        fps = 1;

    if (fps > 60)
        fps = 60;

    return fps;
}

// ============================================================
// GIF ENCODER CLSID
// ============================================================

static bool GetGIFEncoderCLSID(
    CLSID& clsid)
{
    UINT number = 0;
    UINT size = 0;

    if (GetImageEncodersSize(
            &number,
            &size) != Ok)
    {
        return false;
    }

    if (size == 0)
        return false;

    std::vector<BYTE> buffer(size);

    ImageCodecInfo* codecs =
        reinterpret_cast<ImageCodecInfo*>(
            &buffer[0]);

    if (GetImageEncoders(
            number,
            size,
            codecs) != Ok)
    {
        return false;
    }

    for (UINT i = 0;
         i < number;
         ++i)
    {
        if (wcscmp(
                codecs[i].MimeType,
                L"image/gif") == 0)
        {
            clsid =
                codecs[i].Clsid;

            return true;
        }
    }

    return false;
}

// ============================================================
// LOAD + RESIZE PNG
//
// Never upscales.
// Maximum output is 1280x720.
// Aspect ratio is preserved.
// ============================================================

static Bitmap* LoadAndResizePNG(
    const std::wstring& filename,
    int& outputWidth,
    int& outputHeight)
{
    Bitmap source(
        filename.c_str(),
        FALSE);

    if (source.GetLastStatus() != Ok)
        return NULL;

    UINT sourceWidth =
        source.GetWidth();

    UINT sourceHeight =
        source.GetHeight();

    if (sourceWidth == 0 ||
        sourceHeight == 0)
    {
        return NULL;
    }

    double scale = 1.0;

    if (sourceWidth > MAX_WIDTH ||
        sourceHeight > MAX_HEIGHT)
    {
        double widthScale =
            static_cast<double>(MAX_WIDTH) /
            static_cast<double>(sourceWidth);

        double heightScale =
            static_cast<double>(MAX_HEIGHT) /
            static_cast<double>(sourceHeight);

        scale =
            (widthScale < heightScale)
            ? widthScale
            : heightScale;
    }

    outputWidth =
        static_cast<int>(
            sourceWidth * scale + 0.5);

    outputHeight =
        static_cast<int>(
            sourceHeight * scale + 0.5);

    if (outputWidth < 1)
        outputWidth = 1;

    if (outputHeight < 1)
        outputHeight = 1;

    Bitmap* result =
        new Bitmap(
            outputWidth,
            outputHeight,
            PixelFormat32bppARGB);

    if (!result ||
        result->GetLastStatus() != Ok)
    {
        delete result;
        return NULL;
    }

    Graphics graphics(result);

    graphics.SetCompositingMode(
        CompositingModeSourceCopy);

    graphics.SetCompositingQuality(
        CompositingQualityHighQuality);

    graphics.SetInterpolationMode(
        InterpolationModeHighQualityBicubic);

    graphics.SetPixelOffsetMode(
        PixelOffsetModeHighQuality);

    graphics.SetSmoothingMode(
        SmoothingModeHighQuality);

    graphics.Clear(
        Color(0, 0, 0, 0));

    Rect destination(
        0,
        0,
        outputWidth,
        outputHeight);

    Status drawStatus =
        graphics.DrawImage(
            &source,
            destination,
            0,
            0,
            static_cast<INT>(sourceWidth),
            static_cast<INT>(sourceHeight),
            UnitPixel);

    if (drawStatus != Ok)
    {
        delete result;
        return NULL;
    }

    return result;
}

// ============================================================
// SET GIF FRAME PROPERTIES
// ============================================================

static void SetGIFFrameProperties(
    Bitmap* bitmap,
    ULONG delay)
{
    if (!bitmap)
        return;

    // Frame delay is measured in 1/100 second.
    ULONG frameDelay = delay;

    PropertyItem delayItem;
    ZeroMemory(
        &delayItem,
        sizeof(delayItem));

    delayItem.id =
        PropertyTagFrameDelay;

    delayItem.length =
        sizeof(ULONG);

    delayItem.type =
        PropertyTagTypeLong;

    delayItem.value =
        &frameDelay;

    bitmap->SetPropertyItem(
        &delayItem);

    // 0 = infinite loop.
    USHORT loopCount = 0;

    PropertyItem loopItem;
    ZeroMemory(
        &loopItem,
        sizeof(loopItem));

    loopItem.id =
        PropertyTagLoopCount;

    loopItem.length =
        sizeof(USHORT);

    loopItem.type =
        PropertyTagTypeShort;

    loopItem.value =
        &loopCount;

    bitmap->SetPropertyItem(
        &loopItem);
}

// ============================================================
// CREATE ANIMATED GIF
// ============================================================

static bool CreateAnimatedGIF(
    const std::vector<std::wstring>& files,
    const std::wstring& outputFile,
    int fps)
{
    if (files.empty())
    {
        SetStatus(
            L"No PNG frames found.");

        return false;
    }

    CLSID gifEncoder;

    if (!GetGIFEncoderCLSID(
            gifEncoder))
    {
        SetStatus(
            L"Windows GIF encoder was not found.");

        return false;
    }

    // GIF timing uses hundredths of a second.
    //
    // 30 FPS = approximately 3/100 sec
    // 60 FPS = approximately 2/100 sec
    //
    int delay =
        static_cast<int>(
            std::round(
                100.0 /
                static_cast<double>(fps)));

    if (delay < 1)
        delay = 1;

    if (delay > 65535)
        delay = 65535;

    // --------------------------------------------------------
    // Load first frame.
    // --------------------------------------------------------

    int width = 0;
    int height = 0;

    Bitmap* first =
        LoadAndResizePNG(
            files[0],
            width,
            height);

    if (!first)
    {
        SetStatus(
            L"Could not load the first PNG.");

        return false;
    }

    // GIF requires all frames to have the same canvas size.
    //
    // If the first frame is small, the GIF stays at that size.
    // If it is larger than 720p, it is reduced to 720p.
    //
    // Smaller frames are NOT upscaled.

    SetGIFFrameProperties(
        first,
        static_cast<ULONG>(delay));

    // --------------------------------------------------------
    // Encoder parameters.
    // --------------------------------------------------------

    EncoderParameters encoderParameters;
    ZeroMemory(
        &encoderParameters,
        sizeof(encoderParameters));

    encoderParameters.Count = 1;

    encoderParameters.Parameter[0].Guid =
        EncoderSaveFlag;

    encoderParameters.Parameter[0].Type =
        EncoderParameterValueTypeLong;

    encoderParameters.Parameter[0].NumberOfValues =
        1;

    ULONG saveFlag =
        EncoderValueMultiFrame;

    encoderParameters.Parameter[0].Value =
        &saveFlag;

    // Remove existing file.
    DeleteFileW(
        outputFile.c_str());

    SetStatus(
        L"Creating GIF: frame 1 / " +
        std::to_wstring(files.size()));

    Status status =
        first->Save(
            outputFile.c_str(),
            &gifEncoder,
            &encoderParameters);

    if (status != Ok)
    {
        delete first;

        SetStatus(
            L"Could not start GIF creation.");

        return false;
    }

    // --------------------------------------------------------
    // Add frames.
    // --------------------------------------------------------

    for (size_t i = 1;
         i < files.size();
         ++i)
    {
        int frameWidth = 0;
        int frameHeight = 0;

        Bitmap* frame =
            LoadAndResizePNG(
                files[i],
                frameWidth,
                frameHeight);

        if (!frame)
        {
            delete first;

            DeleteFileW(
                outputFile.c_str());

            SetStatus(
                L"Could not load frame " +
                std::to_wstring(i + 1));

            return false;
        }

        // ----------------------------------------------------
        // Every GIF frame needs the same canvas dimensions.
        //
        // If the PNG is a different size, fit it onto the
        // first frame's canvas without changing its aspect
        // ratio.
        // ----------------------------------------------------

        if (frameWidth != width ||
            frameHeight != height)
        {
            Bitmap* corrected =
                new Bitmap(
                    width,
                    height,
                    PixelFormat32bppARGB);

            if (!corrected ||
                corrected->GetLastStatus() != Ok)
            {
                delete frame;
                delete corrected;
                delete first;

                DeleteFileW(
                    outputFile.c_str());

                SetStatus(
                    L"Could not prepare GIF frame.");

                return false;
            }

            Graphics graphics(corrected);

            graphics.SetCompositingMode(
                CompositingModeSourceCopy);

            graphics.SetCompositingQuality(
                CompositingQualityHighQuality);

            graphics.SetInterpolationMode(
                InterpolationModeHighQualityBicubic);

            graphics.SetPixelOffsetMode(
                PixelOffsetModeHighQuality);

            graphics.Clear(
                Color(0, 0, 0, 0));

            // Preserve the frame's aspect ratio.
            double scaleX =
                static_cast<double>(width) /
                static_cast<double>(frameWidth);

            double scaleY =
                static_cast<double>(height) /
                static_cast<double>(frameHeight);

            double scale =
                (scaleX < scaleY)
                ? scaleX
                : scaleY;

            // Do not upscale.
            if (scale > 1.0)
                scale = 1.0;

            int drawWidth =
                static_cast<int>(
                    frameWidth * scale + 0.5);

            int drawHeight =
                static_cast<int>(
                    frameHeight * scale + 0.5);

            if (drawWidth < 1)
                drawWidth = 1;

            if (drawHeight < 1)
                drawHeight = 1;

            int x =
                (width - drawWidth) / 2;

            int y =
                (height - drawHeight) / 2;

            graphics.DrawImage(
                frame,
                Rect(
                    x,
                    y,
                    drawWidth,
                    drawHeight));

            delete frame;

            frame = corrected;
        }

        SetGIFFrameProperties(
            frame,
            static_cast<ULONG>(delay));

        SetStatus(
            L"Creating GIF: frame " +
            std::to_wstring(i + 1) +
            L" / " +
            std::to_wstring(files.size()));

        saveFlag =
            EncoderValueFrameDimensionTime;

        encoderParameters.Parameter[0].Value =
            &saveFlag;

        status =
            first->SaveAdd(
                frame,
                &encoderParameters);

        delete frame;

        if (status != Ok)
        {
            delete first;

            DeleteFileW(
                outputFile.c_str());

            SetStatus(
                L"GIF failed at frame " +
                std::to_wstring(i + 1));

            return false;
        }

        // Keep the window responsive.
        MSG message;

        while (PeekMessageW(
            &message,
            NULL,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // --------------------------------------------------------
    // Flush GIF encoder.
    // --------------------------------------------------------

    saveFlag =
        EncoderValueFlush;

    encoderParameters.Parameter[0].Value =
        &saveFlag;

    status =
        first->SaveAdd(
            &encoderParameters);

    delete first;

    if (status != Ok)
    {
        DeleteFileW(
            outputFile.c_str());

        SetStatus(
            L"Could not finalize GIF.");

        return false;
    }

    // --------------------------------------------------------
    // Verify output.
    // --------------------------------------------------------

    DWORD attributes =
        GetFileAttributesW(
            outputFile.c_str());

    if (attributes ==
        INVALID_FILE_ATTRIBUTES)
    {
        SetStatus(
            L"GIF file was not created.");

        return false;
    }

    return true;
}

// ============================================================
// CONVERT
// ============================================================

static bool ConvertFolder()
{
    std::wstring folder =
        GetWindowString(
            g_hFolderEdit);

    std::wstring output =
        GetWindowString(
            g_hOutputEdit);

    if (folder.empty())
    {
        SetStatus(
            L"Please select a PNG folder.");

        return false;
    }

    if (output.empty())
    {
        SetStatus(
            L"Please select an output GIF.");

        return false;
    }

    // Remove quotes if the user pasted a quoted path.
    if (folder.length() >= 2 &&
        folder[0] == L'"' &&
        folder[folder.length() - 1] == L'"')
    {
        folder =
            folder.substr(
                1,
                folder.length() - 2);
    }

    if (output.length() >= 2 &&
        output[0] == L'"' &&
        output[output.length() - 1] == L'"')
    {
        output =
            output.substr(
                1,
                output.length() - 2);
    }

    DWORD folderAttributes =
        GetFileAttributesW(
            folder.c_str());

    if (folderAttributes ==
        INVALID_FILE_ATTRIBUTES)
    {
        SetStatus(
            L"The selected folder does not exist.");

        return false;
    }

    if (!(folderAttributes &
          FILE_ATTRIBUTE_DIRECTORY))
    {
        SetStatus(
            L"The selected path is not a folder.");

        return false;
    }

    // --------------------------------------------------------
    // Find PNG files.
    // --------------------------------------------------------

    std::vector<std::wstring> files;

    if (!GetPNGFiles(
            folder,
            files))
    {
        SetStatus(
            L"No PNG files were found.");

        return false;
    }

    // --------------------------------------------------------
    // Check order.
    //
    // If already naturally sorted:
    //     leave it alone.
    //
    // If not:
    //     sort naturally.
    // --------------------------------------------------------

    bool alreadySorted =
        IsAlreadySorted(files);

    if (alreadySorted)
    {
        SetStatus(
            L"Frames already sorted. Keeping folder order...");
    }
    else
    {
        SetStatus(
            L"Frames not sorted. Sorting naturally...");

        std::stable_sort(
            files.begin(),
            files.end(),
            [](const std::wstring& a,
               const std::wstring& b)
            {
                return NaturalLess(
                    GetFilename(a),
                    GetFilename(b));
            });
    }

    // --------------------------------------------------------
    // Make sure output ends in .gif.
    // --------------------------------------------------------

    if (GetExtension(output).empty())
    {
        output += L".gif";
    }
    else
    {
        std::wstring extension =
            ToLowerString(
                GetExtension(output));

        if (extension != L".gif")
        {
            size_t dot =
                output.find_last_of(L'.');

            if (dot != std::wstring::npos)
            {
                output =
                    output.substr(0, dot);
            }

            output += L".gif";
        }
    }

    int fps =
        GetFPS();

    // --------------------------------------------------------
    // Create animation.
    // --------------------------------------------------------

    std::wstring startMessage =
        L"Playing " +
        std::to_wstring(files.size()) +
        L" frames at " +
        std::to_wstring(fps) +
        L" FPS...";

    SetStatus(
        startMessage);

    bool success =
        CreateAnimatedGIF(
            files,
            output,
            fps);

    if (success)
    {
        std::wstring filename =
            GetFilename(output);

        SetStatus(
            L"Done! Created " +
            filename +
            L" (" +
            std::to_wstring(files.size()) +
            L" frames)");
    }

    return success;
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
        {
            HFONT normalFont =
                CreateFontW(
                    18,
                    0,
                    0,
                    0,
                    FW_NORMAL,
                    FALSE,
                    FALSE,
                    FALSE,
                    DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE,
                    L"Arial");

            HFONT boldFont =
                CreateFontW(
                    18,
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
                    DEFAULT_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE,
                    L"Arial");

            HWND title =
                CreateWindowW(
                    L"STATIC",
                    L"PNG Flipbook -> GIF",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    15,
                    500,
                    30,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                title,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // ------------------------------------------------
            // Folder
            // ------------------------------------------------

            HWND folderLabel =
                CreateWindowW(
                    L"STATIC",
                    L"PNG Folder:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    60,
                    100,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                folderLabel,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            g_hFolderEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_AUTOHSCROLL,
                    120,
                    58,
                    330,
                    28,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                g_hFolderEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            HWND browseFolder =
                CreateWindowW(
                    L"BUTTON",
                    L"...",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    460,
                    58,
                    60,
                    28,
                    hwnd,
                    reinterpret_cast<HMENU>(1001),
                    NULL,
                    NULL);

            SendMessageW(
                browseFolder,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // ------------------------------------------------
            // Output
            // ------------------------------------------------

            HWND outputLabel =
                CreateWindowW(
                    L"STATIC",
                    L"Output GIF:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    100,
                    100,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                outputLabel,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            g_hOutputEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"output.gif",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_AUTOHSCROLL,
                    120,
                    98,
                    330,
                    28,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                g_hOutputEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            HWND browseOutput =
                CreateWindowW(
                    L"BUTTON",
                    L"...",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    460,
                    98,
                    60,
                    28,
                    hwnd,
                    reinterpret_cast<HMENU>(1002),
                    NULL,
                    NULL);

            SendMessageW(
                browseOutput,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // ------------------------------------------------
            // FPS
            // ------------------------------------------------

            HWND fpsLabel =
                CreateWindowW(
                    L"STATIC",
                    L"FPS:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    140,
                    100,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                fpsLabel,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            g_hFpsEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"30",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_NUMBER |
                    ES_AUTOHSCROLL,
                    120,
                    138,
                    100,
                    28,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                g_hFpsEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            HWND fpsInfo =
                CreateWindowW(
                    L"STATIC",
                    L"1-60 FPS | Max 1280x720",
                    WS_CHILD | WS_VISIBLE,
                    230,
                    140,
                    290,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                fpsInfo,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            // ------------------------------------------------
            // Convert
            // ------------------------------------------------

            g_hConvertButton =
                CreateWindowW(
                    L"BUTTON",
                    L"CREATE GIF",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_DEFPUSHBUTTON,
                    20,
                    180,
                    500,
                    42,
                    hwnd,
                    reinterpret_cast<HMENU>(1003),
                    NULL,
                    NULL);

            SendMessageW(
                g_hConvertButton,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // ------------------------------------------------
            // Status
            // ------------------------------------------------

            g_hStatus =
                CreateWindowW(
                    L"STATIC",
                    L"Select a folder containing PNG frames.",
                    WS_CHILD |
                    WS_VISIBLE,
                    20,
                    235,
                    500,
                    45,
                    hwnd,
                    NULL,
                    NULL,
                    NULL);

            SendMessageW(
                g_hStatus,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(normalFont),
                TRUE);

            break;
        }

        // ====================================================
        // BUTTONS
        // ====================================================

        case WM_COMMAND:
        {
            int id =
                LOWORD(wParam);

            // Folder picker
            if (id == 1001)
            {
                std::wstring folder =
                    BrowseForFolder(hwnd);

                if (!folder.empty())
                {
                    SetWindowTextW(
                        g_hFolderEdit,
                        folder.c_str());
                }

                return 0;
            }

            // Output picker
            if (id == 1002)
            {
                OPENFILENAMEW dialog;
                ZeroMemory(
                    &dialog,
                    sizeof(dialog));

                wchar_t filename[MAX_PATH] =
                    L"output.gif";

                dialog.lStructSize =
                    sizeof(dialog);

                dialog.hwndOwner =
                    hwnd;

                dialog.lpstrFilter =
                    L"GIF files (*.gif)\0*.gif\0"
                    L"All files (*.*)\0*.*\0";

                dialog.lpstrFile =
                    filename;

                dialog.nMaxFile =
                    MAX_PATH;

                dialog.Flags =
                    OFN_OVERWRITEPROMPT |
                    OFN_PATHMUSTEXIST;

                dialog.lpstrDefExt =
                    L"gif";

                if (GetSaveFileNameW(
                        &dialog))
                {
                    SetWindowTextW(
                        g_hOutputEdit,
                        filename);
                }

                return 0;
            }

            // Create GIF
            if (id == 1003)
            {
                EnableWindow(
                    g_hConvertButton,
                    FALSE);

                ConvertFolder();

                EnableWindow(
                    g_hConvertButton,
                    TRUE);

                return 0;
            }

            break;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}

// ============================================================
// ENTRY POINT
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow)
{
    // --------------------------------------------------------
    // COM
    // --------------------------------------------------------

    HRESULT comResult =
        CoInitializeEx(
            NULL,
            COINIT_APARTMENTTHREADED);

    // --------------------------------------------------------
    // GDI+
    // --------------------------------------------------------

    GdiplusStartupInput startupInput;
    ZeroMemory(
        &startupInput,
        sizeof(startupInput));

    startupInput.GdiplusVersion = 1;

    if (GdiplusStartup(
            &g_gdiplusToken,
            &startupInput,
            NULL) != Ok)
    {
        MessageBoxW(
            NULL,
            L"Could not initialize GDI+.",
            L"PNG to GIF",
            MB_ICONERROR);

        if (SUCCEEDED(comResult))
            CoUninitialize();

        return 1;
    }

    // --------------------------------------------------------
    // Window class
    // --------------------------------------------------------

    const wchar_t CLASS_NAME[] =
        L"PNGToGIFFlipbookWindow";

    WNDCLASSW windowClass;
    ZeroMemory(
        &windowClass,
        sizeof(windowClass));

    windowClass.lpfnWndProc =
        WindowProc;

    windowClass.hInstance =
        hInstance;

    windowClass.lpszClassName =
        CLASS_NAME;

    windowClass.hCursor =
        LoadCursorW(
            NULL,
            IDC_ARROW);

    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_BTNFACE + 1);

    if (!RegisterClassW(
            &windowClass))
    {
        GdiplusShutdown(
            g_gdiplusToken);

        if (SUCCEEDED(comResult))
            CoUninitialize();

        return 1;
    }

    // --------------------------------------------------------
    // Window
    // --------------------------------------------------------

    g_hWindow =
        CreateWindowExW(
            0,
            CLASS_NAME,
            L"PNG Flipbook to GIF",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            WINDOW_W,
            WINDOW_H,
            NULL,
            NULL,
            hInstance,
            NULL);

    if (!g_hWindow)
    {
        GdiplusShutdown(
            g_gdiplusToken);

        if (SUCCEEDED(comResult))
            CoUninitialize();

        return 1;
    }

    ShowWindow(
        g_hWindow,
        nCmdShow);

    UpdateWindow(
        g_hWindow);

    // --------------------------------------------------------
    // Message loop
    // --------------------------------------------------------

    MSG message;

    while (GetMessageW(
        &message,
        NULL,
        0,
        0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    GdiplusShutdown(
        g_gdiplusToken);

    if (SUCCEEDED(comResult))
        CoUninitialize();

    return static_cast<int>(
        message.wParam);
}