#pragma comment(lib, "ole32.lib")
#include <windows.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <objidl.h>
#include <memory>
#include <array>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")

#include "resource.h"

using namespace Gdiplus;

namespace Config {
    constexpr int WIN_W = 920;
    constexpr int WIN_H = 520;
    constexpr int TITLE_H = 28;

    constexpr ULONGLONG LOCK_DURATION_MS = 120000;
    constexpr int TIMER_INTERVAL_MS = 250;

    constexpr int BUTTON_SIZE = 22;
    constexpr int BUTTON_SPACING = 3;
    constexpr int BUTTON_MARGIN_RIGHT = 26;

    constexpr int FONT_TEXT_SIZE = 20;
    constexpr int FONT_TITLE_SIZE = 16;

    constexpr wchar_t TITLE_TEXT[] = L"Scheduled_Horse_Moment.Exe";
    constexpr wchar_t HELP_MESSAGE[] = L"Horse Moments are mandatory.";
    constexpr wchar_t HELP_TITLE[] = L"Help";
}

enum class ControlId : int {
    BtnHelp = 1001,
    BtnClose = 1002,
    TimerAutoClose = 1
};

class GdiObjectGuard {
    HGDIOBJ obj;
public:
    explicit GdiObjectGuard(HGDIOBJ o) : obj(o) {}
    ~GdiObjectGuard() { if (obj) DeleteObject(obj); }
    GdiObjectGuard(const GdiObjectGuard&) = delete;
    GdiObjectGuard& operator=(const GdiObjectGuard&) = delete;
    operator HGDIOBJ() const { return obj; }
    HGDIOBJ get() const { return obj; }
};

struct BitmapDeleter {
    void operator()(Bitmap* bmp) const { delete bmp; }
};
using BitmapPtr = std::unique_ptr<Bitmap, BitmapDeleter>;

static ULONGLONG g_unlockAt = 0;
static ULONG_PTR g_gdiplusToken = 0;

static HFONT g_fontText = nullptr;
static HFONT g_fontTitle = nullptr;
static HBRUSH g_brGray = nullptr;

static BitmapPtr g_horse;

static HWND g_hwndTitle = nullptr;
static HWND g_btnHelp = nullptr;
static HWND g_btnClose = nullptr;
static HWND g_lblHeader = nullptr;
static HWND g_lblMiddle = nullptr;
static HWND g_lblBottom = nullptr;
static HWND g_hwndHorseView = nullptr;

static HFONT CreateOldStyleFont(int heightPx, bool bold = false)
{
    static constexpr std::array<const wchar_t*, 4> candidates = {
        L"Terminal", L"Fixedsys", L"Lucida Console", L"Consolas"
    };

    for (const auto* name : candidates)
    {
        HFONT font = CreateFontW(
            -heightPx, 0, 0, 0,
            bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN,
            name
        );
        if (font) return font;
    }
    return static_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
}

static void ApplyFont(HWND hwnd, HFONT font)
{
    if (hwnd && font)
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

static BitmapPtr LoadPngFromResource(HINSTANCE hInst)
{
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_HORSE_PNG), MAKEINTRESOURCEW(RT_RCDATA));
    if (!hRes) return nullptr;

    DWORD size = SizeofResource(hInst, hRes);
    if (!size) return nullptr;

    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) return nullptr;

    void* pData = LockResource(hGlob);
    if (!pData) return nullptr;

    HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hCopy) return nullptr;

    void* pCopy = GlobalLock(hCopy);
    if (!pCopy)
    {
        GlobalFree(hCopy);
        return nullptr;
    }

    memcpy(pCopy, pData, size);
    GlobalUnlock(hCopy);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hCopy, TRUE, &stream);
    if (FAILED(hr) || !stream)
        return nullptr;

    BitmapPtr bmp(Bitmap::FromStream(stream));
    stream->Release();

    if (!bmp || bmp->GetLastStatus() != Ok)
        return nullptr;

    return bmp;
}

static void MakeTopMost(HWND hwnd)
{
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

static void TryClose(HWND hwnd)
{
    if (GetTickCount64() < g_unlockAt)
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    DestroyWindow(hwnd);
}

static void DrawClassicFrame(HDC hdc, RECT rc)
{
    if (!g_brGray) return;

    FillRect(hdc, &rc, g_brGray);
    DrawEdge(hdc, &rc, EDGE_RAISED, BF_RECT);

    RECT inner = rc;
    InflateRect(&inner, -1, -1);
    DrawEdge(hdc, &inner, EDGE_SUNKEN, BF_RECT);
}

static LRESULT CALLBACK HorseViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        if (g_brGray)
            FillRect(hdc, &rc, g_brGray);

        if (g_horse)
        {
            Graphics graphics(hdc);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

            int boxW = rc.right - rc.left;
            int boxH = rc.bottom - rc.top;

            UINT imgW = g_horse->GetWidth();
            UINT imgH = g_horse->GetHeight();

            double scaleX = static_cast<double>(boxW) / static_cast<double>(imgW);
            double scaleY = static_cast<double>(boxH) / static_cast<double>(imgH);
            double scale = (scaleX < scaleY) ? scaleX : scaleY;

            int drawW = static_cast<int>(imgW * scale);
            int drawH = static_cast<int>(imgH * scale);

            int x = (boxW - drawW) / 2;
            int y = (boxH - drawH) / 2;

            Rect destRect(x, y, drawW, drawH);
            graphics.DrawImage(g_horse.get(), destRect);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK TitleBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(GetParent(hwnd), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case static_cast<int>(ControlId::BtnHelp):
            MessageBoxW(GetParent(hwnd),
                Config::HELP_MESSAGE,
                Config::HELP_TITLE,
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            return 0;

        case static_cast<int>(ControlId::BtnClose):
            SendMessageW(GetParent(hwnd), WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        GdiObjectGuard blueBrush(CreateSolidBrush(RGB(0, 0, 128)));
        FillRect(hdc, &rc, static_cast<HBRUSH>(blueBrush.get()));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_fontTitle));

        RECT textRect = rc;
        textRect.left += 8;
        textRect.top += 6;

        DrawTextW(hdc, Config::TITLE_TEXT, -1, &textRect, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, oldFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool CreateUI(HWND hwnd, HINSTANCE hInst)
{
    g_hwndTitle = CreateWindowExW(
        0, L"SHM_TITLEBAR", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, Config::WIN_W, Config::TITLE_H,
        hwnd, nullptr, hInst, nullptr
    );
    if (!g_hwndTitle) return false;

    g_btnHelp = CreateWindowExW(
        0, L"BUTTON", L"?",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Config::WIN_W - (Config::BUTTON_MARGIN_RIGHT + Config::BUTTON_SIZE + Config::BUTTON_SPACING),
        Config::BUTTON_SPACING,
        Config::BUTTON_SIZE, Config::BUTTON_SIZE,
        g_hwndTitle, reinterpret_cast<HMENU>(static_cast<int>(ControlId::BtnHelp)),
        hInst, nullptr
    );
    if (!g_btnHelp) return false;

    g_btnClose = CreateWindowExW(
        0, L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Config::WIN_W - Config::BUTTON_MARGIN_RIGHT,
        Config::BUTTON_SPACING,
        Config::BUTTON_SIZE, Config::BUTTON_SIZE,
        g_hwndTitle, reinterpret_cast<HMENU>(static_cast<int>(ControlId::BtnClose)),
        hInst, nullptr
    );
    if (!g_btnClose) return false;

    SetWindowTheme(g_btnHelp, L"", L"");
    SetWindowTheme(g_btnClose, L"", L"");

    ApplyFont(g_btnHelp, g_fontTitle);
    ApplyFont(g_btnClose, g_fontTitle);

    g_lblHeader = CreateWindowW(L"STATIC",
        L"Please stand by for a scheduled Horse\r\nMoment.",
        WS_CHILD | WS_VISIBLE,
        18, Config::TITLE_H + 22, 520, 80,
        hwnd, nullptr, hInst, nullptr);
    if (!g_lblHeader) return false;

    g_lblMiddle = CreateWindowW(L"STATIC",
        L"You will now contemplate and reflect.\r\nThis is a time of seriousness.",
        WS_CHILD | WS_VISIBLE,
        18, Config::TITLE_H + 190, 520, 80,
        hwnd, nullptr, hInst, nullptr);
    if (!g_lblMiddle) return false;

    g_lblBottom = CreateWindowW(L"STATIC",
        L"You currently have scheduled Horse Moments turned on.\r\nYou can not change this in settings.",
        WS_CHILD | WS_VISIBLE,
        18, Config::TITLE_H + 410, 780, 80,
        hwnd, nullptr, hInst, nullptr);
    if (!g_lblBottom) return false;

    ApplyFont(g_lblHeader, g_fontText);
    ApplyFont(g_lblMiddle, g_fontText);
    ApplyFont(g_lblBottom, g_fontText);

    g_hwndHorseView = CreateWindowExW(
        0, L"SHM_HORSEVIEW", L"",
        WS_CHILD | WS_VISIBLE,
        560, Config::TITLE_H + 70, 330, 330,
        hwnd, nullptr, hInst, nullptr
    );
    if (!g_hwndHorseView) return false;

    return true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        MakeTopMost(hwnd);

        g_unlockAt = GetTickCount64() + Config::LOCK_DURATION_MS;

        SetTimer(hwnd, static_cast<UINT_PTR>(ControlId::TimerAutoClose),
            Config::TIMER_INTERVAL_MS, nullptr);

        HINSTANCE hInst = reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance;
        if (!CreateUI(hwnd, hInst))
        {
            MessageBoxW(hwnd, L"Failed to create UI controls", L"Error",
                MB_OK | MB_ICONERROR);
            return -1;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == static_cast<WPARAM>(ControlId::TimerAutoClose))
        {
            if (GetTickCount64() >= g_unlockAt)
            {
                KillTimer(hwnd, static_cast<UINT_PTR>(ControlId::TimerAutoClose));
                DestroyWindow(hwnd);
            }
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_CLOSE)
        {
            TryClose(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        TryClose(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        return reinterpret_cast<INT_PTR>(g_brGray);
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        DrawClassicFrame(hdc, rc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        g_horse.reset();

        if (g_fontText)
        {
            DeleteObject(g_fontText);
            g_fontText = nullptr;
        }
        if (g_fontTitle)
        {
            DeleteObject(g_fontTitle);
            g_fontTitle = nullptr;
        }
        if (g_brGray)
        {
            DeleteObject(g_brGray);
            g_brGray = nullptr;
        }

        if (g_gdiplusToken)
        {
            GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool RegisterWindowClasses(HINSTANCE hInst)
{
    WNDCLASSW wcMain{};
    wcMain.lpfnWndProc = WndProc;
    wcMain.hInstance = hInst;
    wcMain.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcMain.lpszClassName = L"SHM_MAIN";
    if (!RegisterClassW(&wcMain)) return false;

    WNDCLASSW wcTitle{};
    wcTitle.lpfnWndProc = TitleBarProc;
    wcTitle.hInstance = hInst;
    wcTitle.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcTitle.lpszClassName = L"SHM_TITLEBAR";
    if (!RegisterClassW(&wcTitle)) return false;

    WNDCLASSW wcHorse{};
    wcHorse.lpfnWndProc = HorseViewProc;
    wcHorse.hInstance = hInst;
    wcHorse.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcHorse.lpszClassName = L"SHM_HORSEVIEW";
    if (!RegisterClassW(&wcHorse)) return false;

    return true;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok)
    {
        MessageBoxW(nullptr, L"Failed to initialize GDI+", L"Error",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    g_brGray = CreateSolidBrush(RGB(192, 192, 192));
    g_fontText = CreateOldStyleFont(Config::FONT_TEXT_SIZE, false);
    g_fontTitle = CreateOldStyleFont(Config::FONT_TITLE_SIZE, true);

    g_horse = LoadPngFromResource(hInst);

    if (!RegisterWindowClasses(hInst))
    {
        MessageBoxW(nullptr, L"Failed to register window classes", L"Error",
            MB_OK | MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        L"SHM_MAIN",
        Config::TITLE_TEXT,
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        Config::WIN_W, Config::WIN_H,
        nullptr, nullptr, hInst, nullptr
    );

    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Failed to create main window", L"Error",
            MB_OK | MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
