//  cover.cpp - plain Win32 windows that cover a rectangle of the screen
//  in one solid colour.  Used by the acceptance test to check that a
//  normal application window really does cover the HUD.
//
//  Two windows are made:
//
//    JarvisCoverTestWindow  an ordinary window, NOT topmost.  A topmost
//                           window would cover the HUD even if the HUD
//                           were topmost too, which would prove nothing.
//                           This one is used to read the z order.
//
//    JarvisCoverTopWindow   a topmost copy of the same rectangle.  It is
//                           used for the measurement on the screen: an
//                           application that keeps re-raising itself (a
//                           browser that wants to stay in front) would
//                           otherwise sit on top of the ordinary cover,
//                           and the screen would say nothing about the
//                           HUD.  A topmost ordinary window is still
//                           composited before a layered one, so this
//                           window is exactly what is needed to catch a
//                           HUD that was accidentally made layered.
//
//    cover.exe [x y w h] [r g b]
//
#include <windows.h>
#include <shellapi.h>

static const wchar_t kClass[] = L"JarvisCoverTestWindow";
static const wchar_t kTopClass[] = L"JarvisCoverTopWindow";

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    //  Per-monitor DPI awareness, so the rectangle the test asks for is
    //  the rectangle in real device pixels.  Without this the window is
    //  scaled by the system and lands somewhere else.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int x = 0, y = 0, w = 1920, h = 1080, r = 255, g = 0, b = 255;
    if (argc >= 5) {
        x = _wtoi(argv[1]); y = _wtoi(argv[2]);
        w = _wtoi(argv[3]); h = _wtoi(argv[4]);
    }
    if (argc >= 8) {
        r = _wtoi(argv[5]); g = _wtoi(argv[6]); b = _wtoi(argv[7]);
    }
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(r, g, b));
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kClass, L"cover", WS_POPUP,
                                x, y, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    //  A normal window, raised to the top of the z-order but never
    //  topmost.  It is brought to the foreground on purpose: a game
    //  that holds the display in exclusive fullscreen has to give it
    //  up when another window takes the foreground, and until it does
    //  no window at all can be seen on the screen.
    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);

    //  The topmost copy of the same rectangle.  It is what the
    //  measurement on the screen is made against.
    WNDCLASSEXW wct = wc;
    wct.lpszClassName = kTopClass;
    if (RegisterClassExW(&wct)) {
        HWND top = CreateWindowExW(WS_EX_TOPMOST, kTopClass, L"cover-top", WS_POPUP,
                                   x, y, w, h, nullptr, nullptr, instance, nullptr);
        if (top) {
            ShowWindow(top, SW_SHOW);
            SetWindowPos(top, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
