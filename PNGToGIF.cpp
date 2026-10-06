#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <commdlg.h>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

// ============================================================
// GLOBALS
// ============================================================

static HWND g_hWnd = nullptr;
static HWND g_hFolder = nullptr;
static HWND g_hFPS = nullptr;
static HWND g_hOutput = nullptr;
static HWND g_hConvert = nullptr;
static HWND g_hStatus = nullptr;

static std::wstring g_folder;
static std::wstring g_output;

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

    size_t pos = result.find_last_of(L"\\/");

    if (pos != std::wstring::npos)
        result.resize(pos);

    return result;
}

static std::wstring GetFileName(
    const std::wstring& path
)
{
    size_t pos = path.find_last_of(L"\\/");

    if (pos == std::wstring::npos)
        return path;

    return path.substr(pos + 1);
}

static std::wstring GetExtension(
    const std::wstring& path
)
{
    size_t pos = path.find_last_of(L'.');

    if (pos == std::wstring::npos)
        return L"";

    std::wstring ext = path.substr(pos);

    for (wchar_t& c : ext)
        c = towlower(c);

    return ext;
}

static bool IsPNG(
    const std::wstring& path
)
{
    return GetExtension(path) == L".png";
}

static void SetStatus(
    const std::wstring& text
)
{
    if (g_hStatus)
    {
        SetWindowTextW(
            g_hStatus,
            text.c_str()
        );
    }
}

// ============================================================
// FOLDER PICKER
// ============================================================

static bool ChooseFolder()
{
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = L"Select PNG Frames Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS |
                 BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl =
        SHBrowseForFolderW(&bi);

    if (!pidl)
        return false;

    wchar_t path[MAX_PATH]{};

    bool success =
        SHGetPathFromIDListW(
            pidl,
            path
        );

    CoTaskMemFree(pidl);

    if (!success)
        return false;

    g_folder = path;

    SetWindowTextW(
        g_hFolder,
        g_folder.c_str()
    );

    return true;
}

// ============================================================
// PNG FILE LIST
// ============================================================

static std::vector<std::wstring> FindPNGFiles(
    const std::wstring& folder
)
{
    std::vector<std::wstring> files;

    std::wstring pattern =
        folder + L"\\*.png";

    WIN32_FIND_DATAW fd{};

    HANDLE hFind =
        FindFirstFileW(
            pattern.c_str(),
            &fd
        );

    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do
    {
        if (!(fd.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring name =
                fd.cFileName;

            if (IsPNG(name))
            {
                files.push_back(
                    folder + L"\\" + name
                );
            }
        }

    } while (
        FindNextFileW(
            hFind,
            &fd
        )
    );

    FindClose(hFind);

    std::sort(
        files.begin(),
        files.end()
    );

    return files;
}

// ============================================================
// WIC PNG LOADING
// ============================================================

static HRESULT LoadPNG(
    IWICImagingFactory* factory,
    const std::wstring& filename,
    IWICBitmap** output
)
{
    if (!factory ||
        !output)
    {
        return E_INVALIDARG;
    }

    *output = nullptr;

    IWICBitmapDecoder* decoder = nullptr;

    HRESULT hr =
        factory->CreateDecoderFromFilename(
            filename.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );

    if (FAILED(hr))
        return hr;

    IWICBitmapFrameDecode* frame = nullptr;

    hr =
        decoder->GetFrame(
            0,
            &frame
        );

    if (SUCCEEDED(hr))
    {
        IWICFormatConverter* converter =
            nullptr;

        hr =
            factory->CreateFormatConverter(
                &converter
            );

        if (SUCCEEDED(hr))
        {
            hr =
                converter->Initialize(
                    frame,
                    GUID_WICPixelFormat8bppIndexed,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeMedianCut
                );

            if (SUCCEEDED(hr))
            {
                hr =
                    factory->CreateBitmapFromSource(
                        converter,
                        WICBitmapCacheOnLoad,
                        output
                    );
            }

            converter->Release();
        }

        frame->Release();
    }

    decoder->Release();

    return hr;
}

// ============================================================
// GIF FRAME DELAY
// ============================================================

static HRESULT SetFrameDelay(
    IWICBitmapFrameEncode* frame,
    UINT delay
)
{
    if (!frame)
        return E_INVALIDARG;

    IWICMetadataQueryWriter* writer = nullptr;

    HRESULT hr =
        frame->GetMetadataQueryWriter(
            &writer
        );

    if (FAILED(hr))
        return hr;

    PROPVARIANT value;
    PropVariantInit(&value);

    value.vt = VT_UI4;
    value.ulVal = delay;

    hr =
        writer->SetMetadataByName(
            L"/grctlext/Delay",
            &value
        );

    PropVariantClear(&value);

    writer->Release();

    return hr;
}

// ============================================================
// GIF LOOP
// ============================================================

static HRESULT SetLoopCount(
    IWICMetadataQueryWriter* writer,
    UINT loopCount
)
{
    if (!writer)
        return E_INVALIDARG;

    IWICMetadataBlockWriter* blockWriter =
        nullptr;

    HRESULT hr =
        writer->QueryInterface(
            IID_PPV_ARGS(&blockWriter)
        );

    if (FAILED(hr))
        return hr;

    blockWriter->Release();

    PROPVARIANT value;
    PropVariantInit(&value);

    value.vt = VT_UI2;
    value.uiVal =
        static_cast<USHORT>(loopCount);

    hr =
        writer->SetMetadataByName(
            L"/appext/Data",
            &value
        );

    PropVariantClear(&value);

    return hr;
}

// ============================================================
// GIF CREATION
// ============================================================

static bool ConvertPNGFolderToGIF(
    const std::wstring& folder,
    const std::wstring& outputFile,
    int fps
)
{
    if (folder.empty())
    {
        SetStatus(
            L"Please select a PNG folder."
        );

        return false;
    }

    if (outputFile.empty())
    {
        SetStatus(
            L"Please select an output GIF."
        );

        return false;
    }

    if (fps < 1)
        fps = 1;

    if (fps > 100)
        fps = 100;

    std::vector<std::wstring> files =
        FindPNGFiles(folder);

    if (files.empty())
    {
        SetStatus(
            L"No PNG files found."
        );

        return false;
    }

    IWICImagingFactory* factory =
        nullptr;

    HRESULT hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)
        );

    if (FAILED(hr))
    {
        SetStatus(
            L"Could not initialize WIC."
        );

        return false;
    }

    IWICStream* stream = nullptr;

    hr =
        factory->CreateStream(
            &stream
        );

    if (FAILED(hr))
    {
        factory->Release();

        SetStatus(
            L"Could not create output stream."
        );

        return false;
    }

    hr =
        stream->InitializeFromFilename(
            outputFile.c_str(),
            GENERIC_WRITE
        );

    if (FAILED(hr))
    {
        stream->Release();
        factory->Release();

        SetStatus(
            L"Could not open output GIF."
        );

        return false;
    }

    IWICBitmapEncoder* encoder =
        nullptr;

    hr =
        factory->CreateEncoder(
            GUID_ContainerFormatGif,
            nullptr,
            &encoder
        );

    if (FAILED(hr))
    {
        stream->Release();
        factory->Release();

        SetStatus(
            L"Could not create GIF encoder."
        );

        return false;
    }

    hr =
        encoder->Initialize(
            stream,
            WICBitmapEncoderNoCache
        );

    if (FAILED(hr))
    {
        encoder->Release();
        stream->Release();
        factory->Release();

        SetStatus(
            L"Could not initialize GIF encoder."
        );

        return false;
    }

    const UINT delay =
        static_cast<UINT>(
            std::max(
                1,
                static_cast<int>(
                    100.0 / fps + 0.5
                )
            )
        );

    bool success = true;

    for (size_t i = 0; i < files.size(); ++i)
    {
        std::wstringstream status;

        status
            << L"Converting "
            << (i + 1)
            << L" / "
            << files.size();

        SetStatus(
            status.str()
        );

        IWICBitmap* bitmap = nullptr;

        hr =
            LoadPNG(
                factory,
                files[i],
                &bitmap
            );

        if (FAILED(hr))
        {
            success = false;
            break;
        }

        IWICBitmapFrameEncode* frame =
            nullptr;

        IPropertyBag2* props = nullptr;

        hr =
            encoder->CreateNewFrame(
                &frame,
                &props
            );

        if (props)
            props->Release();

        if (FAILED(hr))
        {
            bitmap->Release();
            success = false;
            break;
        }

        hr =
            frame->Initialize(
                nullptr
            );

        if (SUCCEEDED(hr))
        {
            UINT width = 0;
            UINT height = 0;

            bitmap->GetSize(
                &width,
                &height
            );

            hr =
                frame->SetSize(
                    width,
                    height
                );
        }

        if (SUCCEEDED(hr))
        {
            WICPixelFormatGUID format =
                GUID_WICPixelFormat8bppIndexed;

            hr =
                frame->SetPixelFormat(
                    &format
                );
        }

        if (SUCCEEDED(hr))
        {
            IWICPalette* palette = nullptr;

            hr =
                factory->CreatePalette(
                    &palette
                );

            if (SUCCEEDED(hr))
            {
                hr =
                    palette->InitializeFromBitmap(
                        bitmap,
                        256,
                        false
                    );

                if (SUCCEEDED(hr))
                {
                    hr =
                        frame->SetPalette(
                            palette
                        );
                }

                palette->Release();
            }
        }

        if (SUCCEEDED(hr))
        {
            hr =
                frame->WriteSource(
                    bitmap,
                    nullptr
                );
        }

        if (SUCCEEDED(hr))
        {
            hr =
                SetFrameDelay(
                    frame,
                    delay
                );
        }

        if (SUCCEEDED(hr))
        {
            hr =
                frame->Commit();
        }

        frame->Release();
        bitmap->Release();

        if (FAILED(hr))
        {
            success = false;
            break;
        }
    }

    if (success)
    {
        hr =
            encoder->Commit();

        if (FAILED(hr))
            success = false;
    }

    encoder->Release();
    stream->Release();
    factory->Release();

    if (success)
    {
        SetStatus(
            L"Done! GIF created successfully."
        );
    }
    else
    {
        SetStatus(
            L"GIF conversion failed."
        );
    }

    return success;
}

// ============================================================
// OUTPUT FILE PICKER
// ============================================================

static bool ChooseOutputFile()
{
    OPENFILENAMEW ofn{};

    wchar_t filename[MAX_PATH]{};
    wcscpy_s(
        filename,
        L"animation.gif"
    );

    ofn.lStructSize =
        sizeof(ofn);

    ofn.hwndOwner =
        g_hWnd;

    ofn.lpstrFilter =
        L"GIF Files (*.gif)\0*.gif\0"
        L"All Files (*.*)\0*.*\0";

    ofn.lpstrFile =
        filename;

    ofn.nMaxFile =
        MAX_PATH;

    ofn.Flags =
        OFN_OVERWRITEPROMPT |
        OFN_PATHMUSTEXIST;

    ofn.lpstrDefExt =
        L"gif";

    if (!GetSaveFileNameW(&ofn))
        return false;

    g_output = filename;

    SetWindowTextW(
        g_hOutput,
        g_output.c_str()
    );

    return true;
}

// ============================================================
// WINDOW PROCEDURE
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
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case 1001:
                {
                    ChooseFolder();
                    return 0;
                }

                case 1002:
                {
                    ChooseOutputFile();
                    return 0;
                }

                case 1003:
                {
                    wchar_t fpsText[32]{};

                    GetWindowTextW(
                        g_hFPS,
                        fpsText,
                        32
                    );

                    int fps =
                        _wtoi(fpsText);

                    if (fps <= 0)
                        fps = 60;

                    if (g_folder.empty())
                    {
                        MessageBoxW(
                            hwnd,
                            L"Select the PNG folder first.",
                            L"PNG to GIF",
                            MB_OK |
                            MB_ICONWARNING
                        );

                        return 0;
                    }

                    if (g_output.empty())
                    {
                        MessageBoxW(
                            hwnd,
                            L"Select an output GIF first.",
                            L"PNG to GIF",
                            MB_OK |
                            MB_ICONWARNING
                        );

                        return 0;
                    }

                    EnableWindow(
                        g_hConvert,
                        FALSE
                    );

                    bool result =
                        ConvertPNGFolderToGIF(
                            g_folder,
                            g_output,
                            fps
                        );

                    EnableWindow(
                        g_hConvert,
                        TRUE
                    );

                    if (result)
                    {
                        MessageBoxW(
                            hwnd,
                            L"GIF conversion completed.",
                            L"PNG to GIF",
                            MB_OK |
                            MB_ICONINFORMATION
                        );
                    }

                    return 0;
                }
            }

            break;
        }

        case WM_CLOSE:
        {
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
        {
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
// WINMAIN
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow
)
{
    HRESULT hr =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED
        );

    if (FAILED(hr))
    {
        MessageBoxW(
            nullptr,
            L"COM initialization failed.",
            L"PNG to GIF",
            MB_OK |
            MB_ICONERROR
        );

        return 1;
    }

    const wchar_t CLASS_NAME[] =
        L"PNGToGIFConverterWindow";

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        hInstance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1
        );

    wc.lpszClassName =
        CLASS_NAME;

    RegisterClassW(&wc);

    g_hWnd =
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
            700,
            360,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!g_hWnd)
    {
        CoUninitialize();
        return 1;
    }

    HFONT font =
        static_cast<HFONT>(
            GetStockObject(
                DEFAULT_GUI_FONT
            )
        );

    CreateWindowW(
        L"STATIC",
        L"PNG Frames Folder:",
        WS_CHILD |
        WS_VISIBLE,
        25,
        30,
        160,
        25,
        g_hWnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_hFolder =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_AUTOHSCROLL,
            190,
            27,
            390,
            28,
            g_hWnd,
            nullptr,
            hInstance,
            nullptr
        );

    HWND browseFolder =
        CreateWindowW(
            L"BUTTON",
            L"Browse...",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            585,
            27,
            85,
            28,
            g_hWnd,
            reinterpret_cast<HMENU>(1001),
            hInstance,
            nullptr
        );

    CreateWindowW(
        L"STATIC",
        L"FPS:",
        WS_CHILD |
        WS_VISIBLE,
        25,
        80,
        160,
        25,
        g_hWnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_hFPS =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"60",
            WS_CHILD |
            WS_VISIBLE |
            ES_NUMBER |
            ES_AUTOHSCROLL,
            190,
            77,
            100,
            28,
            g_hWnd,
            nullptr,
            hInstance,
            nullptr
        );

    CreateWindowW(
        L"STATIC",
        L"Output GIF:",
        WS_CHILD |
        WS_VISIBLE,
        25,
        130,
        160,
        25,
        g_hWnd,
        nullptr,
        hInstance,
        nullptr
    );

    g_hOutput =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_AUTOHSCROLL,
            190,
            127,
            390,
            28,
            g_hWnd,
            nullptr,
            hInstance,
            nullptr
        );

    HWND browseOutput =
        CreateWindowW(
            L"BUTTON",
            L"Browse...",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            585,
            127,
            85,
            28,
            g_hWnd,
            reinterpret_cast<HMENU>(1002),
            hInstance,
            nullptr
        );

    g_hConvert =
        CreateWindowW(
            L"BUTTON",
            L"Convert PNG to GIF",
            WS_CHILD |
            WS_VISIBLE |
            BS_DEFPUSHBUTTON,
            190,
            190,
            250,
            42,
            g_hWnd,
            reinterpret_cast<HMENU>(1003),
            hInstance,
            nullptr
        );

    g_hStatus =
        CreateWindowW(
            L"STATIC",
            L"Select a PNG frames folder.",
            WS_CHILD |
            WS_VISIBLE,
            25,
            255,
            640,
            30,
            g_hWnd,
            nullptr,
            hInstance,
            nullptr
        );

    SendMessageW(
        g_hFolder,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        g_hFPS,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        g_hOutput,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        g_hConvert,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        browseFolder,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        browseOutput,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    SendMessageW(
        g_hStatus,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(font),
        TRUE
    );

    ShowWindow(
        g_hWnd,
        nCmdShow
    );

    UpdateWindow(
        g_hWnd
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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();

    return static_cast<int>(
        msg.wParam
    );
}
