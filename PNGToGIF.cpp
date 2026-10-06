#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ============================================================
// SETTINGS
// ============================================================

static const int WINDOW_W = 560;
static const int WINDOW_H = 300;

static const int MAX_WIDTH_720P  = 1280;
static const int MAX_HEIGHT_720P = 720;

static const int DEFAULT_FPS = 30;

// ============================================================
// GLOBALS
// ============================================================

HWND hWindow = nullptr;
HWND hFolderEdit = nullptr;
HWND hOutputEdit = nullptr;
HWND hFpsEdit = nullptr;
HWND hStatus = nullptr;
HWND hConvertButton = nullptr;

ULONG_PTR g_gdiplusToken = 0;

// ============================================================
// HELPERS
// ============================================================

static std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;

    for (wchar_t& c : r)
        c = static_cast<wchar_t>(towlower(c));

    return r;
}

static bool IsPNG(const fs::path& p)
{
    if (!p.has_extension())
        return false;

    return ToLower(p.extension().wstring()) == L".png";
}

// Natural comparison:
//
// frame_1.png
// frame_2.png
// frame_10.png
//
// instead of:
//
// frame_1.png
// frame_10.png
// frame_2.png
//
static bool NaturalLess(const std::wstring& a, const std::wstring& b)
{
    size_t i = 0;
    size_t j = 0;

    while (i < a.size() && j < b.size())
    {
        wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
        wchar_t cb = static_cast<wchar_t>(towlower(b[j]));

        if (iswdigit(ca) && iswdigit(cb))
        {
            size_t iStart = i;
            size_t jStart = j;

            while (i < a.size() && iswdigit(a[i]))
                ++i;

            while (j < b.size() && iswdigit(b[j]))
                ++j;

            std::wstring na = a.substr(iStart, i - iStart);
            std::wstring nb = b.substr(jStart, j - jStart);

            // Ignore leading zeroes.
            size_t za = na.find_first_not_of(L'0');
            size_t zb = nb.find_first_not_of(L'0');

            std::wstring va =
                (za == std::wstring::npos) ? L"0" : na.substr(za);

            std::wstring vb =
                (zb == std::wstring::npos) ? L"0" : nb.substr(zb);

            if (va.length() != vb.length())
                return va.length() < vb.length();

            if (va != vb)
                return va < vb;

            // Same numeric value.
            // Shorter zero-padding comes first.
            if (na.length() != nb.length())
                return na.length() < nb.length();

            continue;
        }

        if (ca != cb)
            return ca < cb;

        ++i;
        ++j;
    }

    return a.size() < b.size();
}

static bool IsAlreadySorted(const std::vector<fs::path>& files)
{
    for (size_t i = 1; i < files.size(); ++i)
    {
        if (NaturalLess(
                files[i].filename().wstring(),
                files[i - 1].filename().wstring()))
        {
            return false;
        }
    }

    return true;
}

static void SetStatus(const std::wstring& text)
{
    if (hStatus)
    {
        SetWindowTextW(hStatus, text.c_str());
        UpdateWindow(hStatus);
    }
}

static std::wstring BrowseForFolder(HWND owner)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"Select PNG frame folder";

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);

    if (!pidl)
        return L"";

    wchar_t path[MAX_PATH]{};

    std::wstring result;

    if (SHGetPathFromIDListW(pidl, path))
        result = path;

    CoTaskMemFree(pidl);

    return result;
}

static int GetFPS()
{
    wchar_t buffer[64]{};

    GetWindowTextW(hFpsEdit, buffer, 64);

    int fps = _wtoi(buffer);

    if (fps < 1)
        fps = 1;

    if (fps > 60)
        fps = 60;

    return fps;
}

static std::wstring GetText(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);

    if (len <= 0)
        return L"";

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');

    GetWindowTextW(hwnd, text.data(), len + 1);

    text.resize(static_cast<size_t>(len));

    return text;
}

// ============================================================
// GDI+ GIF ENCODER
// ============================================================

static int GetEncoderClsid(
    const WCHAR* format,
    CLSID* pClsid)
{
    UINT num = 0;
    UINT size = 0;

    GetImageEncodersSize(&num, &size);

    if (size == 0)
        return -1;

    std::vector<BYTE> buffer(size);

    ImageCodecInfo* codecs =
        reinterpret_cast<ImageCodecInfo*>(buffer.data());

    if (GetImageEncoders(num, size, codecs) != Ok)
        return -1;

    for (UINT i = 0; i < num; ++i)
    {
        if (wcscmp(codecs[i].MimeType, format) == 0)
        {
            *pClsid = codecs[i].Clsid;
            return static_cast<int>(i);
        }
    }

    return -1;
}

// ============================================================
// RESIZE FRAME
// ============================================================

static Bitmap* LoadAndResizeFrame(
    const fs::path& filename,
    int& outWidth,
    int& outHeight)
{
    Bitmap source(filename.wstring().c_str(), FALSE);

    if (source.GetLastStatus() != Ok)
        return nullptr;

    const UINT originalWidth = source.GetWidth();
    const UINT originalHeight = source.GetHeight();

    if (originalWidth == 0 || originalHeight == 0)
        return nullptr;

    double scale = 1.0;

    if (originalWidth > MAX_WIDTH_720P ||
        originalHeight > MAX_HEIGHT_720P)
    {
        double sx =
            static_cast<double>(MAX_WIDTH_720P) /
            static_cast<double>(originalWidth);

        double sy =
            static_cast<double>(MAX_HEIGHT_720P) /
            static_cast<double>(originalHeight);

        scale = std::min(sx, sy);
    }

    outWidth = std::max(
        1,
        static_cast<int>(
            originalWidth * scale + 0.5));

    outHeight = std::max(
        1,
        static_cast<int>(
            originalHeight * scale + 0.5));

    Bitmap* resized =
        new Bitmap(
            outWidth,
            outHeight,
            PixelFormat32bppARGB);

    if (resized->GetLastStatus() != Ok)
    {
        delete resized;
        return nullptr;
    }

    Graphics graphics(resized);

    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);

    graphics.Clear(Color(0, 0, 0, 0));

    Rect destination(
        0,
        0,
        outWidth,
        outHeight);

    Status status =
        graphics.DrawImage(
            &source,
            destination,
            0,
            0,
            static_cast<INT>(originalWidth),
            static_cast<INT>(originalHeight),
            UnitPixel);

    if (status != Ok)
    {
        delete resized;
        return nullptr;
    }

    return resized;
}

// ============================================================
// CREATE GIF
// ============================================================

static bool CreateAnimatedGIF(
    const std::vector<fs::path>& files,
    const std::wstring& outputFile,
    int fps)
{
    if (files.empty())
    {
        SetStatus(L"No PNG files found.");
        return false;
    }

    CLSID gifClsid{};

    if (GetEncoderClsid(L"image/gif", &gifClsid) < 0)
    {
        SetStatus(L"Could not find GIF encoder.");
        return false;
    }

    // GIF frame delays are measured in 1/100 second.
    //
    // Example:
    // 30 FPS -> 3 = 30ms
    // 60 FPS -> 2 = 20ms
    //
    int delay = static_cast<int>(
        std::round(100.0 / static_cast<double>(fps)));

    if (delay < 1)
        delay = 1;

    if (delay > 65535)
        delay = 65535;

    // Load first frame.
    int width = 0;
    int height = 0;

    Bitmap* first =
        LoadAndResizeFrame(
            files[0],
            width,
            height);

    if (!first)
    {
        SetStatus(L"Failed to load first PNG.");
        return false;
    }

    const size_t frameCount = files.size();

    // --------------------------------------------------------
    // Frame delay metadata
    // --------------------------------------------------------

    std::vector<ULONG> delays(frameCount);

    for (size_t i = 0; i < frameCount; ++i)
        delays[i] = static_cast<ULONG>(delay);

    PropertyItem frameDelayItem{};

    frameDelayItem.id = PropertyTagFrameDelay;
    frameDelayItem.length =
        static_cast<ULONG>(
            delays.size() * sizeof(ULONG));
    frameDelayItem.type = PropertyTagTypeLong;
    frameDelayItem.value = delays.data();

    first->SetPropertyItem(&frameDelayItem);

    // --------------------------------------------------------
    // Loop forever
    // --------------------------------------------------------

    USHORT loopCount = 0;

    PropertyItem loopItem{};

    loopItem.id = PropertyTagLoopCount;
    loopItem.length = sizeof(USHORT);
    loopItem.type = PropertyTagTypeShort;
    loopItem.value = &loopCount;

    first->SetPropertyItem(&loopItem);

    // --------------------------------------------------------
    // Start multi-frame GIF
    // --------------------------------------------------------

    EncoderParameters encoderParams{};

    encoderParams.Count = 1;

    encoderParams.Parameter[0].Guid =
        EncoderSaveFlag;

    encoderParams.Parameter[0].Type =
        EncoderParameterValueTypeLong;

    encoderParams.Parameter[0].NumberOfValues = 1;

    ULONG saveFlag =
        EncoderValueMultiFrame;

    encoderParams.Parameter[0].Value =
        &saveFlag;

    DeleteFileW(outputFile.c_str());

    SetStatus(
        L"Creating GIF: frame 1 / " +
        std::to_wstring(frameCount));

    Status status =
        first->Save(
            outputFile.c_str(),
            &gifClsid,
            &encoderParams);

    if (status != Ok)
    {
        delete first;

        SetStatus(
            L"GIF creation failed at first frame.");

        return false;
    }

    // --------------------------------------------------------
    // Add remaining frames
    // --------------------------------------------------------

    for (size_t i = 1; i < frameCount; ++i)
    {
        int frameWidth = 0;
        int frameHeight = 0;

        Bitmap* frame =
            LoadAndResizeFrame(
                files[i],
                frameWidth,
                frameHeight);

        if (!frame)
        {
            delete first;

            SetStatus(
                L"Failed to load frame " +
                std::to_wstring(i + 1));

            DeleteFileW(outputFile.c_str());

            return false;
        }

        // Every GIF frame must have the same dimensions.
        //
        // Normally this will already be true because the
        // source sequence should have identical dimensions.
        //
        // If one frame differs, resize it again to match.
        if (frameWidth != width ||
            frameHeight != height)
        {
            Bitmap* corrected =
                new Bitmap(
                    width,
                    height,
                    PixelFormat32bppARGB);

            if (corrected->GetLastStatus() != Ok)
            {
                delete frame;
                delete first;

                DeleteFileW(outputFile.c_str());

                SetStatus(
                    L"Could not resize frame.");

                return false;
            }

            Graphics g(corrected);

            g.SetCompositingMode(
                CompositingModeSourceCopy);

            g.SetCompositingQuality(
                CompositingQualityHighQuality);

            g.SetInterpolationMode(
                InterpolationModeHighQualityBicubic);

            g.SetPixelOffsetMode(
                PixelOffsetModeHighQuality);

            g.Clear(
                Color(0, 0, 0, 0));

            g.DrawImage(
                frame,
                Rect(
                    0,
                    0,
                    width,
                    height));

            delete frame;

            frame = corrected;
        }

        SetStatus(
            L"Creating GIF: frame " +
            std::to_wstring(i + 1) +
            L" / " +
            std::to_wstring(frameCount));

        saveFlag =
            EncoderValueFrameDimensionTime;

        encoderParams.Parameter[0].Value =
            &saveFlag;

        status =
            first->SaveAdd(
                frame,
                &encoderParams);

        delete frame;

        if (status != Ok)
        {
            delete first;

            DeleteFileW(outputFile.c_str());

            SetStatus(
                L"GIF creation failed at frame " +
                std::to_wstring(i + 1));

            return false;
        }

        // Give Windows a chance to process window messages.
        MSG msg{};

        while (PeekMessageW(
            &msg,
            nullptr,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // --------------------------------------------------------
    // Finish GIF
    // --------------------------------------------------------

    saveFlag =
        EncoderValueFlush;

    encoderParams.Parameter[0].Value =
        &saveFlag;

    status =
        first->SaveAdd(
            &encoderParams);

    delete first;

    if (status != Ok)
    {
        DeleteFileW(outputFile.c_str());

        SetStatus(
            L"GIF finalization failed.");

        return false;
    }

    // Verify the output actually exists.
    DWORD attributes =
        GetFileAttributesW(
            outputFile.c_str());

    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        SetStatus(
            L"GIF was not created.");

        return false;
    }

    return true;
}

// ============================================================
// CONVERSION
// ============================================================

static bool ConvertFolder()
{
    std::wstring folder =
        GetText(hFolderEdit);

    std::wstring output =
        GetText(hOutputEdit);

    if (folder.empty())
    {
        SetStatus(L"Select a PNG folder first.");
        return false;
    }

    if (output.empty())
    {
        SetStatus(L"Select an output GIF.");
        return false;
    }

    fs::path folderPath(folder);

    if (!fs::exists(folderPath) ||
        !fs::is_directory(folderPath))
    {
        SetStatus(L"Selected folder does not exist.");
        return false;
    }

    std::vector<fs::path> files;

    try
    {
        for (const auto& entry :
             fs::directory_iterator(folderPath))
        {
            if (!entry.is_regular_file())
                continue;

            if (IsPNG(entry.path()))
                files.push_back(entry.path());
        }
    }
    catch (...)
    {
        SetStatus(L"Could not read the folder.");
        return false;
    }

    if (files.empty())
    {
        SetStatus(L"No PNG files found in folder.");
        return false;
    }

    // --------------------------------------------------------
    // Check whether the folder is already naturally sorted.
    //
    // If it is already sorted:
    //     keep the exact discovered order.
    //
    // If it is not sorted:
    //     naturally sort it.
    // --------------------------------------------------------

    bool alreadySorted =
        IsAlreadySorted(files);

    if (!alreadySorted)
    {
        SetStatus(
            L"Frames are not sorted. Sorting...");

        std::stable_sort(
            files.begin(),
            files.end(),
            [](const fs::path& a,
               const fs::path& b)
            {
                return NaturalLess(
                    a.filename().wstring(),
                    b.filename().wstring());
            });
    }
    else
    {
        SetStatus(
            L"Frames already sorted. Keeping order...");
    }

    int fps = GetFPS();

    // Make sure the output has .gif.
    fs::path outputPath(output);

    if (ToLower(outputPath.extension().wstring()) != L".gif")
        outputPath += L".gif";

    // --------------------------------------------------------
    // Show what is going to happen.
    // --------------------------------------------------------

    std::wstring status =
        L"Found " +
        std::to_wstring(files.size()) +
        L" PNG frames. Creating flipbook...";

    SetStatus(status);

    bool success =
        CreateAnimatedGIF(
            files,
            outputPath.wstring(),
            fps);

    if (success)
    {
        std::wstring done =
            L"Done! " +
            std::to_wstring(files.size()) +
            L" frames -> " +
            outputPath.filename().wstring();

        SetStatus(done);
    }

    return success;
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT font =
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

            CreateWindowW(
                L"STATIC",
                L"PNG Flipbook → GIF",
                WS_CHILD | WS_VISIBLE,
                20,
                15,
                500,
                30,
                hwnd,
                nullptr,
                nullptr,
                nullptr);

            HWND title =
                GetDlgItem(hwnd, 0);

            // Folder label
            HWND label1 =
                CreateWindowW(
                    L"STATIC",
                    L"PNG Folder:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    60,
                    100,
                    25,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                label1,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            hFolderEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD | WS_VISIBLE |
                    ES_AUTOHSCROLL,
                    120,
                    58,
                    330,
                    28,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                hFolderEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            HWND browseFolder =
                CreateWindowW(
                    L"BUTTON",
                    L"...",
                    WS_CHILD | WS_VISIBLE |
                    BS_PUSHBUTTON,
                    460,
                    58,
                    60,
                    28,
                    hwnd,
                    reinterpret_cast<HMENU>(1001),
                    nullptr,
                    nullptr);

            SendMessageW(
                browseFolder,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // Output label
            HWND label2 =
                CreateWindowW(
                    L"STATIC",
                    L"Output GIF:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    100,
                    100,
                    25,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                label2,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            hOutputEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"output.gif",
                    WS_CHILD | WS_VISIBLE |
                    ES_AUTOHSCROLL,
                    120,
                    98,
                    330,
                    28,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                hOutputEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            HWND browseOutput =
                CreateWindowW(
                    L"BUTTON",
                    L"...",
                    WS_CHILD | WS_VISIBLE |
                    BS_PUSHBUTTON,
                    460,
                    98,
                    60,
                    28,
                    hwnd,
                    reinterpret_cast<HMENU>(1002),
                    nullptr,
                    nullptr);

            SendMessageW(
                browseOutput,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // FPS
            HWND label3 =
                CreateWindowW(
                    L"STATIC",
                    L"FPS:",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    140,
                    100,
                    25,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                label3,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            hFpsEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"30",
                    WS_CHILD | WS_VISIBLE |
                    ES_NUMBER | ES_AUTOHSCROLL,
                    120,
                    138,
                    100,
                    28,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                hFpsEdit,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            HWND fpsInfo =
                CreateWindowW(
                    L"STATIC",
                    L"1–60 FPS   |   Maximum output: 1280×720",
                    WS_CHILD | WS_VISIBLE,
                    230,
                    140,
                    290,
                    25,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                fpsInfo,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            // Convert button
            hConvertButton =
                CreateWindowW(
                    L"BUTTON",
                    L"CREATE GIF",
                    WS_CHILD | WS_VISIBLE |
                    BS_DEFPUSHBUTTON,
                    20,
                    180,
                    500,
                    42,
                    hwnd,
                    reinterpret_cast<HMENU>(1003),
                    nullptr,
                    nullptr);

            SendMessageW(
                hConvertButton,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(boldFont),
                TRUE);

            // Status
            hStatus =
                CreateWindowW(
                    L"STATIC",
                    L"Select a folder containing PNG frames.",
                    WS_CHILD | WS_VISIBLE,
                    20,
                    235,
                    500,
                    40,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr);

            SendMessageW(
                hStatus,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font),
                TRUE);

            break;
        }

        case WM_COMMAND:
        {
            const int id =
                LOWORD(wParam);

            if (id == 1001)
            {
                std::wstring folder =
                    BrowseForFolder(hwnd);

                if (!folder.empty())
                    SetWindowTextW(
                        hFolderEdit,
                        folder.c_str());
            }
            else if (id == 1002)
            {
                OPENFILENAMEW ofn{};

                wchar_t filename[MAX_PATH] =
                    L"output.gif";

                ofn.lStructSize =
                    sizeof(ofn);

                ofn.hwndOwner =
                    hwnd;

                ofn.lpstrFilter =
                    L"GIF files (*.gif)\0*.gif\0All files (*.*)\0*.*\0";

                ofn.lpstrFile =
                    filename;

                ofn.nMaxFile =
                    MAX_PATH;

                ofn.Flags =
                    OFN_OVERWRITEPROMPT |
                    OFN_PATHMUSTEXIST;

                ofn.lpstrDefExt =
                    L"gif";

                if (GetSaveFileNameW(&ofn))
                {
                    SetWindowTextW(
                        hOutputEdit,
                        filename);
                }
            }
            else if (id == 1003)
            {
                EnableWindow(
                    hConvertButton,
                    FALSE);

                ConvertFolder();

                EnableWindow(
                    hConvertButton,
                    TRUE);
            }

            break;
        }

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            break;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
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
    // COM is needed by the folder dialog.
    CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED);

    // Start GDI+.
    GdiplusStartupInput gdiplusStartupInput{};

    gdiplusStartupInput.GdiplusVersion = 1;

    if (GdiplusStartup(
            &g_gdiplusToken,
            &gdiplusStartupInput,
            nullptr) != Ok)
    {
        MessageBoxW(
            nullptr,
            L"Could not start GDI+.",
            L"PNG to GIF",
            MB_ICONERROR);

        CoUninitialize();

        return 1;
    }

    const wchar_t CLASS_NAME[] =
        L"PNGToGIFFlipbookWindow";

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        CLASS_NAME;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    hWindow =
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
            nullptr,
            nullptr,
            hInstance,
            nullptr);

    if (!hWindow)
    {
        GdiplusShutdown(
            g_gdiplusToken);

        CoUninitialize();

        return 1;
    }

    ShowWindow(
        hWindow,
        nCmdShow);

    UpdateWindow(hWindow);

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(
        g_gdiplusToken);

    CoUninitialize();

    return static_cast<int>(
        msg.wParam);
}
