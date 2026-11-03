// KeyOverlay - a tiny always-on-top, click-through overlay that echoes what you type.
//
//  * WH_KEYBOARD_LL global hook  -> captures keystrokes from any application
//  * WS_EX_LAYERED  + per-pixel alpha -> smooth edges, translucent panel
//  * WS_EX_TRANSPARENT -> the mouse goes straight through the overlay
//  * GDI+ -> "HD" (vector text) and "dot matrix" (sampled dot grid) rendering
//  * System tray menu + global hotkeys -> style / colour / size / opacity

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "config.h"
#include "gdiplus_prelude.h"
#include "input_hook.h"
#include "overlay.h"
#include "renderer.h"

using namespace Gdiplus;

namespace {

// ---------------------------------------------------------------- constants

enum : UINT {
    WM_TRAY = WM_APP + 2,
};

enum Cmd : UINT {
    CMD_STYLE_HD = 100,
    CMD_STYLE_DOT,
    CMD_MODE_SOLID,
    CMD_MODE_GRADIENT,
    CMD_GRAD_VERTICAL,

    CMD_SOLID_BASE   = 200,   // + index
    CMD_GRAD_BASE    = 220,   // + index

    CMD_SIZE_UP      = 400,
    CMD_SIZE_DOWN,
    CMD_SIZE_RESET,

    CMD_OPACITY_BASE = 420,   // + index
    CMD_PANEL        = 440,
    CMD_PAUSE        = 441,
    CMD_MARGIN_BASE  = 480,   // + index
    CMD_PLACE_RIGHT  = 460,
    CMD_PLACE_LEFT,
    CMD_PLACE_TOP,
    CMD_PLACE_BOTTOM,
    CMD_ABOUT        = 500,
    CMD_EXIT         = 501,
};

enum HotkeyId : int {
    HK_SIZE_UP = 1,
    HK_SIZE_DOWN,
    HK_STYLE,
    HK_COLOR,
    HK_PAUSE,
    HK_CLEAR,
    HK_EXIT,
};

constexpr int  kHoldMs      = 2400;   // text stays fully opaque this long after the last key
constexpr int  kFadeMs      = 720;
constexpr UINT kFrameTimer  = 1;
constexpr int  kFrameMs     = 33;

struct NamedColor    { const wchar_t* name; COLORREF color; };
struct NamedGradient { const wchar_t* name; COLORREF from, to; };

const NamedColor kSolidColors[] = {
    {L"冰蓝",   RGB(0, 234, 255)},
    {L"薄荷绿", RGB(0, 255, 170)},
    {L"暖黄",   RGB(255, 214, 64)},
    {L"橙红",   RGB(255, 112, 64)},
    {L"霓虹粉", RGB(255, 64, 176)},
    {L"紫罗兰", RGB(170, 118, 255)},
    {L"荧光绿", RGB(140, 255, 96)},
    {L"纯白",   RGB(255, 255, 255)},
};

const NamedGradient kGradients[] = {
    {L"霓虹青紫", RGB(0, 234, 255),   RGB(255, 64, 190)},
    {L"日落",     RGB(255, 196, 72),  RGB(255, 64, 96)},
    {L"极光",     RGB(72, 255, 190),  RGB(72, 132, 255)},
    {L"火焰",     RGB(255, 240, 128), RGB(255, 48, 32)},
    {L"冰川",     RGB(240, 252, 255), RGB(64, 148, 255)},
    {L"青柠汽水", RGB(160, 255, 96),  RGB(0, 208, 255)},
};

const int kOpacities[] = {100, 90, 80, 70, 60, 50};
const int kMarginList[] = {16, 48, 96, 160};

template <typename T, size_t N>
constexpr int CountOf(T (&)[N]) { return (int)N; }

// ---------------------------------------------------------------- helpers

// Readable name for keys that do not produce a printable character
// (modifiers, function keys, navigation keys, ...).
std::wstring KeyLabel(WPARAM vk, UINT scan, bool extended)
{
    switch (vk) {
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:       return L"Shift";
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return L"Ctrl";
        case VK_LMENU: case VK_RMENU: case VK_MENU:           return L"Alt";
        case VK_LWIN: case VK_RWIN:                          return L"Win";
        case VK_CAPITAL:  return L"Caps";
        case VK_NUMLOCK:  return L"Num";
        case VK_SCROLL:   return L"ScrLk";
        case VK_SPACE:    return L"Space";
        case VK_ESCAPE:   return L"Esc";
        case VK_TAB:      return L"Tab";
        case VK_RETURN:   return L"Enter";
        case VK_BACK:     return L"Backspace";
        case VK_DELETE:   return L"Del";
        case VK_INSERT:   return L"Ins";
        case VK_HOME:     return L"Home";
        case VK_END:      return L"End";
        case VK_PRIOR:    return L"PgUp";
        case VK_NEXT:     return L"PgDn";
        case VK_LEFT:     return L"←";
        case VK_RIGHT:    return L"→";
        case VK_UP:       return L"↑";
        case VK_DOWN:     return L"↓";
        case VK_APPS:     return L"Menu";
        case VK_SNAPSHOT: return L"PrtSc";
        case VK_PAUSE:    return L"Pause";
        default: break;
    }

    wchar_t buf[32] = {};
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(buf, L"F%u", (unsigned)(vk - VK_F1 + 1));
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        swprintf_s(buf, L"Num%u", (unsigned)(vk - VK_NUMPAD0));
        return buf;
    }

    // Layout-provided name ("Page Down", "PrtSc", ...), localized by the keyboard driver.
    LONG keyParam = (LONG)(scan & 0xFF) << 16;
    if (extended) keyParam |= (1L << 24);
    if (GetKeyNameTextW(keyParam, buf, CountOf(buf) - 1) > 0) return buf;
    return L"";
}

// Turns a key press into the single token to display: either the typed character
// or the human readable key name.
// Modifier flags come from the hook payload (see input_hook.h).
std::wstring KeyToDisplay(WPARAM vk, UINT scan, bool extended, bool ctrl, bool shift)
{
    if (ctrl) {
        // Only one token is shown, so Ctrl+letter is displayed as the letter itself.
        if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
        if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
        return KeyLabel(vk, scan, extended);
    }

    if (vk != VK_SPACE) {
        BYTE keys[256] = {};
        GetKeyboardState(keys);
        keys[VK_SHIFT]   = shift ? 0x80 : 0;
        keys[VK_CONTROL] = 0;
        keys[VK_MENU]    = 0;
        keys[VK_CAPITAL] = (BYTE)(GetKeyState(VK_CAPITAL) & 1);

        const HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
        wchar_t buf[8] = {};
        const int n = ToUnicodeEx((UINT)vk, scan, keys, buf, (int)CountOf(buf) - 1, 0, hkl);
        if (n > 0) {
            const std::wstring text(buf, (size_t)n);
            if (!text.empty() && (unsigned)text[0] >= 0x20 && text[0] != 0x7F) return text;
        }
        // n == -1: dead key; fall through so the key name is still shown.
    }

    return KeyLabel(vk, scan, extended);
}

HICON MakeTrayIcon(int size)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);
    ZeroMemory(bits, (size_t)size * size * 4);

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, color);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        const RectF r(1.f, 1.f, (REAL)size - 2.f, (REAL)size - 2.f);
        const REAL d = r.Width * 0.5f;
        GraphicsPath p;
        p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
        p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
        p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
        p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
        p.CloseFigure();

        LinearGradientBrush bg(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y + r.Height),
                               Color(255, 0, 220, 255), Color(255, 240, 60, 200));
        g.FillPath(&bg, &p);

        FontFamily fam(L"Segoe UI");
        Font font(&fam, (REAL)size * 0.62f, FontStyleBold, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush white(Color(255, 255, 255, 255));
        g.DrawString(L"A", 1, &font, r, &sf, &white);
        g.Flush();
    }
    SelectObject(mem, old);
    DeleteDC(mem);

    std::vector<BYTE> zeros(((size_t)(size + 15) / 16) * 2 * (size_t)size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (mask && !zeros.empty()) SetBitmapBits(mask, (DWORD)zeros.size(), zeros.data());

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

} // namespace

// ---------------------------------------------------------------- application

class App
{
public:
    bool Init(HINSTANCE hInst);
    int  Run();

private:
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(HWND, UINT, WPARAM, LPARAM);

    void Refresh();
    void OnKeystroke(WPARAM vk, LPARAM payload);
    void OnHotkey(int id);
    void ShowKey(const std::wstring& token);
    void AdjustSize(int direction);
    void CycleColor();
    void TogglePause();
    void ShowMenu();
    void Execute(UINT cmd);
    void SaveConfig();
    void UpdateTrayTip(const wchar_t* tip);

    HINSTANCE       m_hInst  = nullptr;
    HWND            m_host   = nullptr;
    Overlay         m_overlay;
    InputHook       m_hook;
    Config          m_cfg;
    std::wstring    m_text;
    ULONGLONG       m_lastInput = 0;
    bool            m_paused    = false;
    int             m_colorIndex = 0;
    HICON           m_icon      = nullptr;
    NOTIFYICONDATAW m_nid{};
};

bool App::Init(HINSTANCE hInst)
{
    m_hInst = hInst;

    const std::wstring cfgPath = ConfigFilePath();
    const bool firstRun = GetFileAttributesW(cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    m_cfg.Load(cfgPath);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &App::HostProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"KeyOverlay.Host";
    if (!RegisterClassExW(&wc)) return false;

    m_host = CreateWindowExW(WS_EX_TOOLWINDOW, L"KeyOverlay.Host", L"KeyOverlay",
                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, this);
    if (!m_host) return false;

    if (!m_overlay.Create(hInst)) return false;
    m_hook.Install(m_host);

    m_icon = MakeTrayIcon(GetSystemMetrics(SM_CXSMICON));

    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_host;
    m_nid.uID              = 1;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAY;
    m_nid.hIcon            = m_icon;
    wcscpy_s(m_nid.szTip, L"KeyOverlay · 桌面输入显示");
    Shell_NotifyIconW(NIM_ADD, &m_nid);

    RegisterHotKey(m_host, HK_SIZE_UP,   MOD_CONTROL | MOD_ALT, VK_UP);
    RegisterHotKey(m_host, HK_SIZE_DOWN, MOD_CONTROL | MOD_ALT, VK_DOWN);
    RegisterHotKey(m_host, HK_STYLE,     MOD_CONTROL | MOD_ALT, 'S');
    RegisterHotKey(m_host, HK_COLOR,     MOD_CONTROL | MOD_ALT, 'C');
    RegisterHotKey(m_host, HK_PAUSE,     MOD_CONTROL | MOD_ALT, 'H');
    RegisterHotKey(m_host, HK_CLEAR,     MOD_CONTROL | MOD_ALT, 'X');
    RegisterHotKey(m_host, HK_EXIT,      MOD_CONTROL | MOD_ALT, 'Q');

    SetTimer(m_host, kFrameTimer, kFrameMs, nullptr);

    if (firstRun) {
        SaveConfig();
        m_nid.uFlags |= NIF_INFO;
        wcscpy_s(m_nid.szInfoTitle, L"KeyOverlay 已启动");
        wcscpy_s(m_nid.szInfo,
                 L"在任意程序中打字即可看到效果。\n"
                 L"Ctrl+Alt+↑/↓ 调整大小，右键托盘图标可切换样式与配色。");
        m_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
    return true;
}

int App::Run()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK App::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_host = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->OnMessage(hwnd, msg, wp, lp);
}

LRESULT App::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_KEYOVERLAY_KEYSTROKE:
            OnKeystroke(wp, lp);
            return 0;

        case WM_TIMER:
            if (wp == kFrameTimer) Refresh();
            return 0;

        case WM_HOTKEY:
            OnHotkey((int)wp);
            return 0;

        case WM_TRAY: {
            const UINT ev = LOWORD(lp);
            if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU || ev == WM_LBUTTONUP) ShowMenu();
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, kFrameTimer);
            m_hook.Uninstall();
            for (int id = HK_SIZE_UP; id <= HK_EXIT; ++id) UnregisterHotKey(hwnd, id);
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            if (m_icon) DestroyIcon(m_icon);
            m_overlay.Destroy();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::Refresh()
{
    if (m_text.empty() || m_paused) {
        m_overlay.Hide();
        return;
    }

    const ULONGLONG idle = GetTickCount64() - m_lastInput;
    float alpha = 1.f;
    if (idle > (ULONGLONG)kHoldMs) {
        const float k = (float)(idle - (ULONGLONG)kHoldMs) / (float)kFadeMs;
        if (k >= 1.f) {
            m_text.clear();   // start fresh on the next burst of typing
            m_overlay.Hide();
            return;
        }
        alpha = 1.f - k;
    }
    m_overlay.Render(m_cfg, m_text, alpha);
}

void App::OnKeystroke(WPARAM vk, LPARAM payload)
{
    if (m_paused) return;

    const UINT scan     = (UINT)(payload & 0xFFFF);
    const bool extended = (payload & kKeyExt) != 0;
    const bool ctrl     = (payload & kKeyCtrl) != 0;
    const bool alt      = (payload & kKeyAlt) != 0;
    const bool shift    = (payload & kKeyShift) != 0;

    if (ctrl && alt) {
        // Presses that drive one of our own hotkeys never reach the display.
        switch (vk) {
            case VK_UP: case VK_DOWN:
            case 'S': case 'C': case 'H': case 'X': case 'Q':
                return;
            default: break;
        }
    }

    const std::wstring token = KeyToDisplay(vk, scan, extended, ctrl, shift);
    if (token.empty()) return;

    ShowKey(token);
    m_lastInput = GetTickCount64();
    Refresh();
}

// Only one key is ever shown: every key press replaces the previous one.
// `token` is either a single typed character or a key name like "Ctrl" / "F5".
void App::ShowKey(const std::wstring& token)
{
    m_text = token;
    if (m_text.size() > 24) m_text.resize(24);   // keep the overlay sane on odd layouts
}

void App::AdjustSize(int direction)
{
    if (m_cfg.style == RenderStyle::DotMatrix) {
        m_cfg.dotSize = std::clamp(m_cfg.dotSize + direction, 2, 24);
    } else {
        const int cur = m_cfg.hdFontSize;
        const int next = direction > 0 ? (int)std::lround(cur * 1.12) + 1
                                       : (int)std::lround(cur * 0.88) - 1;
        m_cfg.hdFontSize = std::clamp(next, 12, 240);
    }
    SaveConfig();
    Refresh();
}

void App::CycleColor()
{
    if (m_cfg.colorMode == PaintMode::Solid) {
        m_colorIndex = (m_colorIndex + 1) % CountOf(kSolidColors);
        m_cfg.solidColor = kSolidColors[m_colorIndex].color;
    } else {
        m_colorIndex = (m_colorIndex + 1) % CountOf(kGradients);
        m_cfg.gradStart = kGradients[m_colorIndex].from;
        m_cfg.gradEnd   = kGradients[m_colorIndex].to;
    }
    SaveConfig();
    Refresh();
}

void App::TogglePause()
{
    m_paused = !m_paused;
    UpdateTrayTip(m_paused ? L"KeyOverlay · 已暂停捕获" : L"KeyOverlay · 桌面输入显示");
    Refresh();
}

void App::UpdateTrayTip(const wchar_t* tip)
{
    wcscpy_s(m_nid.szTip, tip);
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void App::SaveConfig()
{
    m_cfg.Sanitize();
    m_cfg.Save(ConfigFilePath());
}

void App::OnHotkey(int id)
{
    switch (id) {
        case HK_SIZE_UP:   AdjustSize(+1); break;
        case HK_SIZE_DOWN: AdjustSize(-1); break;
        case HK_STYLE:
            m_cfg.style = (m_cfg.style == RenderStyle::HD) ? RenderStyle::DotMatrix : RenderStyle::HD;
            SaveConfig();
            Refresh();
            break;
        case HK_COLOR: CycleColor(); break;
        case HK_PAUSE: TogglePause(); break;
        case HK_CLEAR: m_text.clear(); Refresh(); break;
        case HK_EXIT:  DestroyWindow(m_host); break;
        default: break;
    }
}

void App::ShowMenu()
{
    HMENU root = CreatePopupMenu();
    if (!root) return;

    const auto flag = [](bool on) -> UINT { return on ? MF_CHECKED : 0u; };

    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING | flag(m_cfg.style == RenderStyle::HD), CMD_STYLE_HD, L"高清 · 平滑字形");
    AppendMenuW(style, MF_STRING | flag(m_cfg.style == RenderStyle::DotMatrix), CMD_STYLE_DOT, L"点阵 · 像素灯珠");
    AppendMenuW(style, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(style, MF_STRING | flag(m_cfg.colorMode == PaintMode::Solid), CMD_MODE_SOLID, L"纯色填充");
    AppendMenuW(style, MF_STRING | flag(m_cfg.colorMode == PaintMode::Gradient), CMD_MODE_GRADIENT, L"渐变填充");
    AppendMenuW(style, MF_STRING | flag(m_cfg.gradVertical), CMD_GRAD_VERTICAL, L"渐变方向：垂直");

    HMENU solid = CreatePopupMenu();
    for (int i = 0; i < CountOf(kSolidColors); ++i) {
        const bool on = m_cfg.colorMode == PaintMode::Solid && m_cfg.solidColor == kSolidColors[i].color;
        AppendMenuW(solid, MF_STRING | flag(on), CMD_SOLID_BASE + i, kSolidColors[i].name);
    }

    HMENU gradient = CreatePopupMenu();
    for (int i = 0; i < CountOf(kGradients); ++i) {
        const bool on = m_cfg.colorMode == PaintMode::Gradient &&
                        m_cfg.gradStart == kGradients[i].from && m_cfg.gradEnd == kGradients[i].to;
        AppendMenuW(gradient, MF_STRING | flag(on), CMD_GRAD_BASE + i, kGradients[i].name);
    }

    HMENU colors = CreatePopupMenu();
    AppendMenuW(colors, MF_POPUP, (UINT_PTR)solid, L"纯色");
    AppendMenuW(colors, MF_POPUP, (UINT_PTR)gradient, L"渐变");

    HMENU size = CreatePopupMenu();
    AppendMenuW(size, MF_STRING, CMD_SIZE_UP, L"放大\tCtrl+Alt+↑");
    AppendMenuW(size, MF_STRING, CMD_SIZE_DOWN, L"缩小\tCtrl+Alt+↓");
    AppendMenuW(size, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(size, MF_STRING, CMD_SIZE_RESET, L"恢复默认大小");

    HMENU opacity = CreatePopupMenu();
    for (int i = 0; i < CountOf(kOpacities); ++i) {
        wchar_t label[16];
        swprintf_s(label, L"%d%%", kOpacities[i]);
        AppendMenuW(opacity, MF_STRING | flag(m_cfg.opacityPct == kOpacities[i]),
                    CMD_OPACITY_BASE + i, label);
    }

    HMENU margin = CreatePopupMenu();
    for (int i = 0; i < CountOf(kMarginList); ++i) {
        wchar_t label[32];
        swprintf_s(label, L"距屏幕边缘 %d px", kMarginList[i]);
        AppendMenuW(margin, MF_STRING | flag(m_cfg.margin == kMarginList[i]),
                    CMD_MARGIN_BASE + i, label);
    }

    HMENU place = CreatePopupMenu();
    AppendMenuW(place, MF_STRING | flag(m_cfg.placement == Placement::RightCenter),
                CMD_PLACE_RIGHT, L"右侧居中");
    AppendMenuW(place, MF_STRING | flag(m_cfg.placement == Placement::LeftCenter),
                CMD_PLACE_LEFT, L"左侧居中");
    AppendMenuW(place, MF_STRING | flag(m_cfg.placement == Placement::TopCenter),
                CMD_PLACE_TOP, L"顶部居中");
    AppendMenuW(place, MF_STRING | flag(m_cfg.placement == Placement::BottomCenter),
                CMD_PLACE_BOTTOM, L"底部居中");

    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING | flag(m_cfg.showPanel), CMD_PANEL, L"半透明背景板");
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(display, MF_POPUP, (UINT_PTR)place, L"屏幕位置");
    AppendMenuW(display, MF_POPUP, (UINT_PTR)margin, L"边距");

    AppendMenuW(root, MF_POPUP, (UINT_PTR)style, L"样式与填充");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)colors, L"颜色");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)size, L"大小");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)opacity, L"透明度");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)display, L"显示");
    AppendMenuW(root, MF_STRING | flag(m_paused), CMD_PAUSE, L"暂停捕获\tCtrl+Alt+H");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_STRING, CMD_ABOUT, L"快捷键 / 关于");
    AppendMenuW(root, MF_STRING, CMD_EXIT, L"退出");

    SetMenuDefaultItem(root, CMD_ABOUT, FALSE);

    POINT pt{};
    GetCursorPos(&pt);
    HWND prevForeground = GetForegroundWindow();

    // TrackPopupMenu only dismisses itself properly when its owner is foreground.
    const DWORD fgThread = prevForeground ? GetWindowThreadProcessId(prevForeground, nullptr) : 0;
    const DWORD myThread = GetCurrentThreadId();
    const bool attached = (fgThread != 0 && fgThread != myThread);
    if (attached) AttachThreadInput(fgThread, myThread, TRUE);
    SetForegroundWindow(m_host);

    const UINT cmd = TrackPopupMenu(root, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, m_host, nullptr);

    if (attached) AttachThreadInput(fgThread, myThread, FALSE);
    if (prevForeground && IsWindow(prevForeground)) SetForegroundWindow(prevForeground);
    PostMessageW(m_host, WM_NULL, 0, 0);
    DestroyMenu(root);

    if (cmd) Execute(cmd);
}

void App::Execute(UINT cmd)
{
    bool changed = true;

    if (cmd >= (UINT)CMD_SOLID_BASE && cmd < (UINT)CMD_SOLID_BASE + (UINT)CountOf(kSolidColors)) {
        m_colorIndex = (int)(cmd - CMD_SOLID_BASE);
        m_cfg.solidColor = kSolidColors[m_colorIndex].color;
        m_cfg.colorMode = PaintMode::Solid;
    } else if (cmd >= (UINT)CMD_GRAD_BASE && cmd < (UINT)CMD_GRAD_BASE + (UINT)CountOf(kGradients)) {
        m_colorIndex = (int)(cmd - CMD_GRAD_BASE);
        m_cfg.gradStart = kGradients[m_colorIndex].from;
        m_cfg.gradEnd   = kGradients[m_colorIndex].to;
        m_cfg.colorMode = PaintMode::Gradient;
    } else if (cmd >= (UINT)CMD_OPACITY_BASE && cmd < (UINT)CMD_OPACITY_BASE + (UINT)CountOf(kOpacities)) {
        m_cfg.opacityPct = kOpacities[cmd - CMD_OPACITY_BASE];
    } else if (cmd >= (UINT)CMD_MARGIN_BASE && cmd < (UINT)CMD_MARGIN_BASE + (UINT)CountOf(kMarginList)) {
        m_cfg.margin = kMarginList[cmd - CMD_MARGIN_BASE];
    } else {
        switch (cmd) {
            case CMD_PLACE_RIGHT:  m_cfg.placement = Placement::RightCenter; break;
            case CMD_PLACE_LEFT:   m_cfg.placement = Placement::LeftCenter; break;
            case CMD_PLACE_TOP:    m_cfg.placement = Placement::TopCenter; break;
            case CMD_PLACE_BOTTOM: m_cfg.placement = Placement::BottomCenter; break;
            case CMD_STYLE_HD:      m_cfg.style = RenderStyle::HD; break;
            case CMD_STYLE_DOT:     m_cfg.style = RenderStyle::DotMatrix; break;
            case CMD_MODE_SOLID:    m_cfg.colorMode = PaintMode::Solid; break;
            case CMD_MODE_GRADIENT: m_cfg.colorMode = PaintMode::Gradient; break;
            case CMD_GRAD_VERTICAL: m_cfg.gradVertical = !m_cfg.gradVertical; break;
            case CMD_SIZE_UP:       AdjustSize(+1); changed = false; break;
            case CMD_SIZE_DOWN:     AdjustSize(-1); changed = false; break;
            case CMD_SIZE_RESET:
                m_cfg.dotSize = 4;
                m_cfg.dotRows = 16;
                m_cfg.hdFontSize = 64;
                break;
            case CMD_PANEL:  m_cfg.showPanel = !m_cfg.showPanel; break;
            case CMD_PAUSE:  TogglePause(); changed = false; break;
            case CMD_ABOUT:
                MessageBoxW(nullptr,
                    L"KeyOverlay · 在桌面上显示你当前按下的按键\n\n"
                    L"字符键显示字符；Space / Esc / Tab / Shift / Ctrl / Alt /\n"
                    L"Caps / Enter / Backspace / F1~F24 等显示按键名称，\n"
                    L"方向键显示 ← → ↑ ↓。\n\n"
                    L"快捷键（全局）\n"
                    L"  Ctrl+Alt+↑ / ↓    放大 / 缩小\n"
                    L"  Ctrl+Alt+S        切换 高清 / 点阵\n"
                    L"  Ctrl+Alt+C        切换下一套配色\n"
                    L"  Ctrl+Alt+H        暂停 / 恢复捕获\n"
                    L"  Ctrl+Alt+X        清空当前文字\n"
                    L"  Ctrl+Alt+Q        退出\n\n"
                    L"托盘图标右键可以调整颜色、透明度与位置。\n"
                    L"窗口始终置顶且鼠标穿透，不会抢走输入焦点。",
                    L"KeyOverlay", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
                changed = false;
                break;
            case CMD_EXIT:  DestroyWindow(m_host); changed = false; break;
            default:        changed = false; break;
        }
    }

    if (changed) {
        SaveConfig();
        Refresh();
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // Single instance: a stale process would keep drawing with its old settings.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\KeyOverlay.SingleInstance");
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instanceMutex) CloseHandle(instanceMutex);
        MessageBoxW(nullptr, L"KeyOverlay 已经在运行了（请查看托盘图标）。",
                    L"KeyOverlay", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        return 0;
    }

    // Per-monitor DPI awareness keeps text crisp on scaled displays (Win10 1703+).
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = reinterpret_cast<SetDpiCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Ok) {
        CloseHandle(instanceMutex);
        return 1;
    }

    int rc = 0;
    {
        App app;
        if (app.Init(hInst)) rc = app.Run();
    }

    GdiplusShutdown(gdiplusToken);
    CloseHandle(instanceMutex);
    return rc;
}

