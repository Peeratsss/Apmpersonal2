#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

// ============================================================
// GLOBALS
// ============================================================

static HWND g_hwnd = nullptr;
static HWND g_folderEdit = nullptr;
static HWND g_outputEdit = nullptr;
static HWND g_fpsEdit = nullptr;
static HWND g_convertButton = nullptr;
static HWND g_status = nullptr;

static const int ID_FOLDER = 1001;
static const int ID_OUTPUT = 1002;
static const int ID_FPS = 1003;
static const int ID_BROWSE = 1004;
static const int ID_CONVERT = 1005;

// ============================================================
// HELPERS
// ============================================================

static std::wstring GetText(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);

    std::wstring text;
    text.resize(len);

    if (len > 0)
        GetWindowTextW(hwnd, text.data(), len + 1);

    return text;
}

static void SetStatus(const std::wstring& text)
{
    if (g_status)
        SetWindowTextW(g_status, text.c_str());

    UpdateWindow(g_hwnd);
}

static bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());

    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring GetExeDirectory()
{
    wchar_t buffer[MAX_PATH];

    DWORD len = GetModuleFileNameW(
        nullptr,
        buffer,
        MAX_PATH
    );

    if (len == 0)
        return L".";

    std::wstring path(buffer, len);

    size_t slash = path.find_last_of(L"\\/");

    if (slash == std::wstring::npos)
        return L".";

    return path.substr(0, slash);
}

static bool IsPNG(const std::wstring& path)
{
    if (path.size() < 4)
        return false;

    std::wstring ext =
        path.substr(path.size() - 4);

    for (wchar_t& c : ext)
        c = towlower(c);

    return ext == L".png";
}

static std::vector<std::wstring> GetPNGFiles(
    const std::wstring& folder)
{
    std::vector<std::wstring> files;

    std::wstring search =
        folder + L"\\*.png";

    WIN32_FIND_DATAW data;

    HANDLE hFind =
        FindFirstFileW(
            search.c_str(),
            &data
        );

    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do
    {
        if (!(data.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring file =
                folder + L"\\" + data.cFileName;

            if (IsPNG(file))
                files.push_back(file);
        }

    } while (FindNextFileW(hFind, &data));

    FindClose(hFind);

    std::sort(
        files.begin(),
        files.end(),
        [](const std::wstring& a,
           const std::wstring& b)
        {
            return a < b;
        }
    );

    return files;
}

// ============================================================
// WIC
// ============================================================

static IWICImagingFactory* g_factory = nullptr;

static bool InitWIC()
{
    if (g_factory)
        return true;

    HRESULT hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&g_factory)
        );

    return SUCCEEDED(hr);
}

static bool LoadPNG(
    const std::wstring& filename,
    IWICBitmap** output)
{
    if (!output)
        return false;

    *output = nullptr;

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr =
        g_factory->CreateDecoderFromFilename(
            filename.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        decoder->GetFrame(
            0,
            &frame
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        g_factory->CreateFormatConverter(
            &converter
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        g_factory->CreateBitmapFromSource(
            converter,
            WICBitmapCacheOnLoad,
            output
        );

cleanup:

    if (converter)
        converter->Release();

    if (frame)
        frame->Release();

    if (decoder)
        decoder->Release();

    return SUCCEEDED(hr);
}

// ============================================================
// GIF ENCODER
// ============================================================

static bool SetGifMetadata(
    IWICMetadataQueryWriter* writer,
    UINT delayCentiseconds,
    bool firstFrame)
{
    if (!writer)
        return false;

    PROPVARIANT value;
    PropVariantInit(&value);

    bool ok = true;

    // Frame delay.
    value.vt = VT_UI2;
    value.uiVal =
        static_cast<USHORT>(
            max(1u, min(65535u, delayCentiseconds))
        );

    HRESULT hr =
        writer->SetMetadataByName(
            L"/grctlext/Delay",
            &value
        );

    if (FAILED(hr))
        ok = false;

    PropVariantClear(&value);

    // Disposal method: restore to background.
    PropVariantInit(&value);

    value.vt = VT_UI1;
    value.bVal = 1;

    hr =
        writer->SetMetadataByName(
            L"/grctlext/Disposal",
            &value
        );

    if (FAILED(hr))
        ok = false;

    PropVariantClear(&value);

    if (firstFrame)
    {
        // Netscape loop extension.
        BYTE loopData[3] = { 1, 0, 0 };

        PropVariantInit(&value);

        value.vt = VT_UNKNOWN;

        IWICMetadataQueryWriter* extensionWriter = nullptr;

        IWICComponentFactory* componentFactory = nullptr;

        hr =
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&componentFactory)
            );

        if (SUCCEEDED(hr))
        {
            IWICMetadataQueryWriter* writer2 = nullptr;

            hr =
                componentFactory->CreateQueryWriter(
                    GUID_MetadataQueryWriter,
                    nullptr,
                    &writer2
                );

            if (SUCCEEDED(hr) && writer2)
                writer2->Release();

            componentFactory->Release();
        }

        PropVariantClear(&value);

        (void)extensionWriter;
        (void)loopData;
    }

    return ok;
}

// ============================================================
// CONVERSION
// ============================================================

static bool ConvertPNGToGIF()
{
    std::wstring inputFolder =
        GetText(g_folderEdit);

    std::wstring outputFile =
        GetText(g_outputEdit);

    std::wstring fpsText =
        GetText(g_fpsEdit);

    if (inputFolder.empty())
    {
        MessageBoxW(
            g_hwnd,
            L"Select a PNG folder first.",
            L"PNG to GIF",
            MB_ICONWARNING
        );

        return false;
    }

    if (outputFile.empty())
    {
        outputFile = GetExeDirectory() +
                     L"\\APM_Replay.gif";
    }

    if (outputFile.find(L".gif") ==
        std::wstring::npos)
    {
        outputFile += L".gif";
    }

    int fps = 60;

    if (!fpsText.empty())
    {
        int value = _wtoi(fpsText.c_str());

        if (value > 0 && value <= 1000)
            fps = value;
    }

    std::vector<std::wstring> files =
        GetPNGFiles(inputFolder);

    if (files.empty())
    {
        MessageBoxW(
            g_hwnd,
            L"No PNG files were found in the selected folder.",
            L"PNG to GIF",
            MB_ICONWARNING
        );

        return false;
    }

    SetStatus(
        L"Loading " +
        std::to_wstring(files.size()) +
        L" PNG frames..."
    );

    IWICBitmap* firstBitmap = nullptr;

    if (!LoadPNG(files[0], &firstBitmap))
    {
        MessageBoxW(
            g_hwnd,
            L"Could not load the first PNG.",
            L"PNG to GIF",
            MB_ICONERROR
        );

        return false;
    }

    UINT width = 0;
    UINT height = 0;

    firstBitmap->GetSize(
        &width,
        &height
    );

    if (width == 0 || height == 0)
    {
        firstBitmap->Release();

        MessageBoxW(
            g_hwnd,
            L"Invalid PNG dimensions.",
            L"PNG to GIF",
            MB_ICONERROR
        );

        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    HRESULT hr =
        g_factory->CreateEncoder(
            GUID_ContainerFormatGif,
            nullptr,
            &encoder
        );

    if (FAILED(hr))
    {
        firstBitmap->Release();
        return false;
    }

    hr =
        encoder->Initialize(
            nullptr,
            WICBitmapEncoderNoCache
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        encoder->CreateNewFrame(
            &frame,
            &props
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        frame->Initialize(props);

    if (FAILED(hr))
        goto cleanup;

    hr =
        frame->SetSize(
            width,
            height
        );

    if (FAILED(hr))
        goto cleanup;

    {
        GUID format =
            GUID_WICPixelFormat8bppIndexed;

        hr =
            frame->SetPixelFormat(
                &format
            );

        if (FAILED(hr))
            goto cleanup;
    }

    hr =
        frame->WriteSource(
            firstBitmap,
            nullptr
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        frame->Commit();

    if (FAILED(hr))
        goto cleanup;

    firstBitmap->Release();
    firstBitmap = nullptr;

    frame->Release();
    frame = nullptr;

    if (props)
    {
        props->Release();
        props = nullptr;
    }

    // --------------------------------------------------------
    // Remaining frames
    // --------------------------------------------------------

    for (size_t i = 1; i < files.size(); ++i)
    {
        IWICBitmap* bitmap = nullptr;

        if (!LoadPNG(files[i], &bitmap))
        {
            SetStatus(
                L"Failed loading frame " +
                std::to_wstring(i)
            );

            goto cleanup;
        }

        UINT frameWidth = 0;
        UINT frameHeight = 0;

        bitmap->GetSize(
            &frameWidth,
            &frameHeight
        );

        if (frameWidth != width ||
            frameHeight != height)
        {
            bitmap->Release();

            SetStatus(
                L"All PNGs must have the same resolution."
            );

            goto cleanup;
        }

        IWICFormatConverter* converter = nullptr;

        hr =
            g_factory->CreateFormatConverter(
                &converter
            );

        if (FAILED(hr))
        {
            bitmap->Release();
            goto cleanup;
        }

        hr =
            converter->Initialize(
                bitmap,
                GUID_WICPixelFormat8bppIndexed,
                WICBitmapDitherTypeFloydSteinberg,
                nullptr,
                0.0,
                WICBitmapPaletteTypeMedianCut
            );

        if (FAILED(hr))
        {
            converter->Release();
            bitmap->Release();
            goto cleanup;
        }

        IWICBitmapFrameEncode* gifFrame = nullptr;
        IPropertyBag2* gifProps = nullptr;

        hr =
            encoder->CreateNewFrame(
                &gifFrame,
                &gifProps
            );

        if (FAILED(hr))
        {
            converter->Release();
            bitmap->Release();
            goto cleanup;
        }

        hr =
            gifFrame->Initialize(
                gifProps
            );

        if (SUCCEEDED(hr))
        {
            hr =
                gifFrame->SetSize(
                    width,
                    height
                );
        }

        if (SUCCEEDED(hr))
        {
            GUID format =
                GUID_WICPixelFormat8bppIndexed;

            hr =
                gifFrame->SetPixelFormat(
                    &format
                );
        }

        if (SUCCEEDED(hr))
        {
            hr =
                gifFrame->WriteSource(
                    converter,
                    nullptr
                );
        }

        if (SUCCEEDED(hr))
        {
            hr =
                gifFrame->Commit();
        }

        if (gifProps)
            gifProps->Release();

        if (gifFrame)
            gifFrame->Release();

        converter->Release();
        bitmap->Release();

        if (FAILED(hr))
        {
            SetStatus(
                L"Failed encoding frame " +
                std::to_wstring(i)
            );

            goto cleanup;
        }

        int percent =
            static_cast<int>(
                ((i + 1) * 100) /
                files.size()
            );

        SetStatus(
            L"Converting... " +
            std::to_wstring(percent) +
            L"%"
        );
    }

    hr =
        encoder->Commit();

    if (FAILED(hr))
        goto cleanup;

    SetStatus(
        L"Done: " +
        outputFile
    );

    if (encoder)
        encoder->Release();

    return true;

cleanup:

    if (firstBitmap)
        firstBitmap->Release();

    if (frame)
        frame->Release();

    if (props)
        props->Release();

    if (encoder)
        encoder->Release();

    MessageBoxW(
        g_hwnd,
        L"GIF conversion failed.\n\n"
        L"Make sure all PNGs have the same resolution.",
        L"PNG to GIF",
        MB_ICONERROR
    );

    return false;
}

// ============================================================
// BROWSE
// ============================================================

static void BrowseFolder()
{
    BROWSEINFOW bi = {};

    bi.hwndOwner = g_hwnd;
    bi.lpszTitle =
        L"Select the folder containing PNG frames";
    bi.ulFlags = BIF_RETURNONLYFSDIRS |
                 BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl =
        SHBrowseForFolderW(&bi);

    if (!pidl)
        return;

    wchar_t path[MAX_PATH];

    if (SHGetPathFromIDListW(
            pidl,
            path))
    {
        SetWindowTextW(
            g_folderEdit,
            path
        );

        if (GetText(g_outputEdit).empty())
        {
            std::wstring output =
                std::wstring(path) +
                L"\\APM_Replay.gif";

            SetWindowTextW(
                g_outputEdit,
                output.c_str()
            );
        }
    }

    CoTaskMemFree(pidl);
}

// ============================================================
// WINDOW
// ============================================================

static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:

        switch (LOWORD(wParam))
        {
        case ID_BROWSE:
            BrowseFolder();
            return 0;

        case ID_CONVERT:
            ConvertPNGToGIF();
            return 0;
        }

        break;

    case WM_DESTROY:

        if (g_factory)
        {
            g_factory->Release();
            g_factory = nullptr;
        }

        PostQuitMessage(0);
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
// MAIN
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow)
{
    HRESULT hr =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED
        );

    if (FAILED(hr))
        return 1;

    if (!InitWIC())
    {
        MessageBoxW(
            nullptr,
            L"Could not initialize Windows Imaging Component.",
            L"PNG to GIF",
            MB_ICONERROR
        );

        CoUninitialize();
        return 1;
    }

    const wchar_t CLASS_NAME[] =
        L"PNGToGIFConverterWindow";

    WNDCLASSW wc = {};

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    RegisterClassW(&wc);

    g_hwnd =
        CreateWindowExW(
            0,
            CLASS_NAME,
            L"PNG to GIF Converter",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            650,
            330,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!g_hwnd)
    {
        CoUninitialize();
        return 1;
    }

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
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Arial"
        );

    CreateWindowW(
        L"STATIC",
        L"PNG Frames Folder:",
        WS_CHILD | WS_VISIBLE,
        25,
        25,
        180,
        25,
        g_hwnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_folderEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_AUTOHSCROLL,
            25,
            55,
            470,
            32,
            g_hwnd,
            (HMENU)ID_FOLDER,
            hInstance,
            nullptr
        );

    CreateWindowW(
        L"BUTTON",
        L"Browse",
        WS_CHILD |
        WS_VISIBLE |
        BS_PUSHBUTTON,
        505,
        55,
        100,
        32,
        g_hwnd,
        (HMENU)ID_BROWSE,
        hInstance,
        nullptr
    );

    CreateWindowW(
        L"STATIC",
        L"Output GIF:",
        WS_CHILD | WS_VISIBLE,
        25,
        105,
        180,
        25,
        g_hwnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_outputEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"APM_Replay.gif",
            WS_CHILD |
            WS_VISIBLE |
            ES_AUTOHSCROLL,
            25,
            135,
            580,
            32,
            g_hwnd,
            (HMENU)ID_OUTPUT,
            hInstance,
            nullptr
        );

    CreateWindowW(
        L"STATIC",
        L"FPS:",
        WS_CHILD | WS_VISIBLE,
        25,
        185,
        60,
        25,
        g_hwnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_fpsEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"60",
            WS_CHILD |
            WS_VISIBLE |
            ES_NUMBER |
            ES_AUTOHSCROLL,
            85,
            180,
            80,
            32,
            g_hwnd,
            (HMENU)ID_FPS,
            hInstance,
            nullptr
        );

    g_convertButton =
        CreateWindowW(
            L"BUTTON",
            L"Convert PNGs to GIF",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            25,
            230,
            580,
            40,
            g_hwnd,
            (HMENU)ID_CONVERT,
            hInstance,
            nullptr
        );

    g_status =
        CreateWindowW(
            L"STATIC",
            L"Ready.",
            WS_CHILD |
            WS_VISIBLE,
            25,
            280,
            580,
            25,
            g_hwnd,
            nullptr,
            hInstance,
            nullptr
        );

    HWND children[] =
    {
        g_folderEdit,
        g_outputEdit,
        g_fpsEdit,
        g_convertButton
    };

    for (HWND child : children)
    {
        SendMessageW(
            child,
            WM_SETFONT,
            (WPARAM)font,
            TRUE
        );
    }

    ShowWindow(
        g_hwnd,
        nCmdShow
    );

    UpdateWindow(g_hwnd);

    MSG msg;

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(font);

    CoUninitialize();

    return static_cast<int>(
        msg.wParam
    );
}
