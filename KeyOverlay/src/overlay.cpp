#include "overlay.h"

#include <algorithm>

#include "renderer.h"

namespace {
constexpr const wchar_t* kClassName = L"KeyOverlay.Layer";
}

bool Overlay::Create(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;   // never receives input
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // WS_EX_TRANSPARENT  -> mouse events pass straight through
    // WS_EX_NOACTIVATE   -> never takes focus from the app you are typing in
    // WS_EX_TOOLWINDOW   -> stays out of Alt+Tab / task bar
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kClassName, L"KeyOverlay", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);

    return m_hwnd != nullptr;
}

void Overlay::Destroy()
{
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Overlay::Hide()
{
    if (m_hwnd && IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
}

void Overlay::Render(const Config& cfg, const std::wstring& text, float alpha)
{
    if (!m_hwnd) return;

    LayerBitmap lb;
    if (!Renderer::Render(cfg, text, alpha, lb)) {
        Hide();
        return;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT wa = mi.rcWork;
    const int waW = (int)(wa.right - wa.left);
    const int waH = (int)(wa.bottom - wa.top);

    int x = 0;
    int y = 0;
    switch (cfg.placement) {
        case Placement::TopCenter:
            x = wa.left + (waW - lb.width) / 2;
            y = wa.top + cfg.margin;
            break;
        case Placement::BottomCenter:
            x = wa.left + (waW - lb.width) / 2;
            y = wa.bottom - lb.height - cfg.margin;
            break;
        case Placement::LeftCenter:
            x = wa.left + cfg.margin;
            y = wa.top + (waH - lb.height) / 2;
            break;
        case Placement::RightCenter:
        default:
            x = wa.right - lb.width - cfg.margin;
            y = wa.top + (waH - lb.height) / 2;
            break;
    }

    // Keep the window fully inside the work area even for large glyphs.
    x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - lb.width));
    y = std::clamp(y, (int)wa.top,  std::max((int)wa.top,  (int)wa.bottom - lb.height));

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, lb.width, lb.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    HDC screen = GetDC(nullptr);
    POINT dst{x, y};
    POINT src{0, 0};
    SIZE  size{lb.width, lb.height};

    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)std::clamp(cfg.opacityPct * 255 / 100, 0, 255);
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_hwnd, screen, &dst, &size, lb.dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);

    lb.Release();
}
