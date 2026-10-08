#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <deque>
#include <sstream>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")

using namespace Gdiplus;

struct KeyRect {
    std::string id;
    std::wstring label;
    int x, y, w, h;
    bool isPressed = false;
};

class VisualizerApp {
public:
    std::vector<KeyRect> keys;
    std::deque<std::string> last10;
    std::deque<int64_t> actionTimestamps;
    int64_t currentTimestampMs = 0;

    VisualizerApp() {
        initLayout();
    }

    void initLayout() {
        keys.clear();
        // Keyboard (Left)
        keys.push_back({"ESC", L"ESC", 10, 15, 40, 25});
        keys.push_back({"F1",  L"F1",  60, 15, 35, 25});
        keys.push_back({"F3",  L"F3", 100, 15, 35, 25});
        keys.push_back({"F5",  L"F5", 140, 15, 35, 25});
        keys.push_back({"F6",  L"F6", 180, 15, 35, 25});

        keys.push_back({"`",   L"`",   10, 50, 35, 35});
        keys.push_back({"1",   L"1",   50, 50, 35, 35});
        keys.push_back({"2",   L"2",   90, 50, 35, 35});
        keys.push_back({"3",   L"3",  130, 50, 35, 35});

        keys.push_back({"TAB", L"TAB", 10, 95, 45, 35});
        keys.push_back({"Q",   L"Q",   60, 95, 35, 35});
        keys.push_back({"W",   L"W",  100, 95, 35, 35});
        keys.push_back({"E",   L"E",  140, 95, 35, 35});
        keys.push_back({"D",   L"D",  180, 95, 35, 35});
        keys.push_back({"F",   L"F",  220, 95, 35, 35});
        keys.push_back({"R",   L"R",  260, 95, 35, 35});

        keys.push_back({"A",   L"A",   60, 140, 35, 35});
        keys.push_back({"S",   L"S",  100, 140, 35, 35});
        keys.push_back({"H",   L"H",  140, 140, 35, 35});
        keys.push_back({"M",   L"M",  180, 140, 35, 35});
        keys.push_back({"T",   L"T",  220, 140, 35, 35});

        keys.push_back({"Z",   L"Z",   60, 185, 35, 35});
        keys.push_back({"X",   L"X",  100, 185, 35, 35});
        keys.push_back({"C",   L"C",  140, 185, 35, 35});
        keys.push_back({"V",   L"V",  180, 185, 35, 35});

        // Mouse (Right)
        keys.push_back({"LMB", L"LMB", 890, 50, 60, 90});
        keys.push_back({"RMB", L"RMB", 1020, 50, 60, 90});
        keys.push_back({"MMB", L"MMB", 960, 50, 50, 45});
    }

    void draw(Graphics& g) {
        SolidBrush bgBrush(Color(255, 0, 0, 0));
        g.FillRectangle(&bgBrush, 0, 0, 1200, 300);

        Pen borderPen(Color(255, 64, 169, 255), 1.0f);
        SolidBrush defaultKeyBrush(Color(255, 30, 30, 30));
        SolidBrush pressedKeyBrush(Color(255, 162, 73, 245));
        SolidBrush textBrush(Color(255, 255, 255, 255));
        Font font(L"Arial", 10, FontStyleBold, UnitPixel);

        for (const auto& k : keys) {
            Brush* fillBrush = k.isPressed ? (Brush*)&pressedKeyBrush : (Brush*)&defaultKeyBrush;
            g.FillRectangle(fillBrush, k.x, k.y, k.w, k.h);
            g.DrawRectangle(&borderPen, k.x, k.y, k.w, k.h);

            PointF pt(static_cast<REAL>(k.x + 5), static_cast<REAL>(k.y + 8));
            g.DrawString(k.label.c_str(), -1, &font, pt, &textBrush);
        }
    }
};

VisualizerApp app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Graphics graphics(hdc);
        app.draw(graphics);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int main() {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"APMVisualizerClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0, L"APMVisualizerClass", L"APM Visualizer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1216, 339, NULL, NULL, hInstance, NULL
    );

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}
