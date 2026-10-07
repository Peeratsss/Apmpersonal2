
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

    return static_cast<int>(
        msg.wParam
    );
}
