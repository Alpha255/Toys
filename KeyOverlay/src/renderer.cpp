#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "gdiplus_prelude.h"

using namespace Gdiplus;

namespace {

RECT WorkArea()
{
    RECT r{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) {
        r = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return r;
}

// Returns a font family name that is actually installed.
std::wstring ResolveFamily(const std::wstring& wanted)
{
    const std::wstring candidates[] = {
        wanted, L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", L"Arial"
    };
    for (const auto& name : candidates) {
        if (name.empty()) continue;
        FontFamily ff(name.c_str());
        if (ff.GetLastStatus() == Ok && ff.IsAvailable()) return name;
    }
    return L"Arial";
}

void MeasureText(const std::wstring& text, Font& font, StringFormat& sf, REAL& w, REAL& h)
{
    w = h = 0;
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    {
        Graphics g(screen);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        RectF box;
        g.MeasureString(text.c_str(), (INT)text.size(), &font, PointF(0, 0), &sf, &box);
        w = box.Width;
        h = box.Height;
    }
    ReleaseDC(nullptr, screen);
}

struct Layout
{
    int  pad     = 0;
    int  width   = 0;
    int  height  = 0;
    int  gridW   = 0;   // dot-matrix grid size
    int  gridH   = 0;
    REAL em      = 0;   // font em size in pixels
};

bool LayoutDot(const Config& c, const std::wstring& text, Layout& L)
{
    L.em = (REAL)std::max(6, c.dotRows - 3);
    Font font(ResolveFamily(c.fontFamily).c_str(), L.em, FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return false;

    StringFormat sf(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    REAL tw = 0, th = 0;
    MeasureText(text, font, sf, tw, th);

    L.gridW = (int)std::ceil(tw) + 2;
    L.gridH = c.dotRows;
    L.pad   = std::max(6, c.dotSize * 2);
    L.width  = L.gridW * c.dotSize + L.pad * 2;
    L.height = L.gridH * c.dotSize + L.pad * 2;
    return L.width > 0 && L.height > 0;
}

bool LayoutHD(const Config& c, const std::wstring& text, Layout& L)
{
    L.em = (REAL)c.hdFontSize;
    Font font(ResolveFamily(c.fontFamily).c_str(), L.em, FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return false;

    StringFormat sf(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    REAL tw = 0, th = 0;
    MeasureText(text, font, sf, tw, th);

    L.pad   = std::max(10, c.hdFontSize / 5);
    const int shadowPad = c.hdShadow ? std::max(4, c.hdFontSize / 14) : 2;
    L.width  = (int)std::ceil(tw) + L.pad * 2 + shadowPad;
    L.height = (int)std::ceil(th) + L.pad * 2 + shadowPad;
    return L.width > 0 && L.height > 0;
}

bool MakeLayout(const Config& c, const std::wstring& text, Layout& L)
{
    return c.style == RenderStyle::DotMatrix ? LayoutDot(c, text, L) : LayoutHD(c, text, L);
}

Color LerpColor(const Config& c, float t, BYTE a)
{
    t = std::clamp(t, 0.0f, 1.0f);
    if (c.colorMode == PaintMode::Solid) {
        return Color(a, GetRValue(c.solidColor), GetGValue(c.solidColor), GetBValue(c.solidColor));
    }
    auto mix = [t](BYTE x, BYTE y) {
        return (BYTE)std::lround(x + (y - (double)x) * t);
    };
    return Color(a,
                 mix(GetRValue(c.gradStart), GetRValue(c.gradEnd)),
                 mix(GetGValue(c.gradStart), GetGValue(c.gradEnd)),
                 mix(GetBValue(c.gradStart), GetBValue(c.gradEnd)));
}

std::unique_ptr<Brush> MakeBrush(const Config& c, const RectF& rect, BYTE a)
{
    if (c.colorMode == PaintMode::Solid) {
        return std::make_unique<SolidBrush>(
            Color(a, GetRValue(c.solidColor), GetGValue(c.solidColor), GetBValue(c.solidColor)));
    }
    const Color c1(a, GetRValue(c.gradStart), GetGValue(c.gradStart), GetBValue(c.gradStart));
    const Color c2(a, GetRValue(c.gradEnd), GetGValue(c.gradEnd), GetBValue(c.gradEnd));
    PointF p1(rect.X, rect.Y);
    PointF p2 = c.gradVertical ? PointF(rect.X, rect.Y + rect.Height)
                               : PointF(rect.X + rect.Width, rect.Y);
    return std::make_unique<LinearGradientBrush>(p1, p2, c1, c2);
}

bool CreateLayerBitmap(int w, int h, LayerBitmap& out)
{
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;    // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return false;
    }
    std::memset(bits, 0, (size_t)w * (size_t)h * 4u);

    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem) {
        DeleteObject(bmp);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!old || old == HGDI_ERROR) {
        DeleteDC(mem);
        DeleteObject(bmp);
        return false;
    }

    out.bmp    = bmp;
    out.dc     = mem;
    out.width  = w;
    out.height = h;
    return true;
}

void DrawPanel(Graphics& g, const Layout& L, BYTE a)
{
    const RectF r(1.f, 1.f, (REAL)L.width - 2.f, (REAL)L.height - 2.f);
    const REAL radius = std::min(20.f, std::min(r.Width, r.Height) * 0.35f);
    const REAL d = radius * 2.f;

    GraphicsPath path;
    path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();

    SolidBrush bg(Color((BYTE)(a * 0.42f), 6, 8, 14));
    g.FillPath(&bg, &path);

    Pen border(Color((BYTE)(a * 0.16f), 255, 255, 255), 1.f);
    g.DrawPath(&border, &path);
}

void DrawDots(Graphics& g, const Config& c, const std::wstring& text, const Layout& L, BYTE a)
{
    Font font(ResolveFamily(c.fontFamily).c_str(), L.em, FontStyleRegular, UnitPixel);
    StringFormat sf(StringFormatFlagsNoWrap | StringFormatFlagsNoClip | StringFormatFlagsMeasureTrailingSpaces);

    Bitmap mask(L.gridW, L.gridH, PixelFormat32bppARGB);
    if (mask.GetLastStatus() != Ok) return;
    {
        Graphics mg(&mask);
        mg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        mg.SetSmoothingMode(SmoothingModeNone);
        SolidBrush white(Color(255, 255, 255, 255));
        mg.DrawString(text.c_str(), (INT)text.size(), &font, PointF(1.f, 0.f), &sf, &white);
    }

    BitmapData bd{};
    Rect rc(0, 0, L.gridW, L.gridH);
    if (mask.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return;

    const int dot = c.dotSize;
    const float denomX = (float)std::max(1, L.gridW - 1);
    const float denomY = (float)std::max(1, L.gridH - 1);

    for (int y = 0; y < L.gridH; ++y) {
        const BYTE* row = reinterpret_cast<const BYTE*>(bd.Scan0) + (size_t)y * (size_t)bd.Stride;
        for (int x = 0; x < L.gridW; ++x) {
            const BYTE m = row[x * 4 + 3];
            if (m < 10) continue;

            const float t = c.gradVertical ? ((float)y / denomY) : ((float)x / denomX);
            SolidBrush brush(LerpColor(c, t, (BYTE)((m * a) / 255)));

            const REAL cx = (REAL)(L.pad + x * dot) + dot * 0.5f;
            const REAL cy = (REAL)(L.pad + y * dot) + dot * 0.5f;
            const REAL r  = dot * 0.46f;
            g.FillEllipse(&brush, cx - r, cy - r, r * 2.f, r * 2.f);
        }
    }
    mask.UnlockBits(&bd);
}

void DrawHD(Graphics& g, const Config& c, const std::wstring& text, const Layout& L, BYTE a)
{
    Font font(ResolveFamily(c.fontFamily).c_str(), L.em, FontStyleRegular, UnitPixel);
    StringFormat sf(StringFormatFlagsNoWrap | StringFormatFlagsNoClip | StringFormatFlagsMeasureTrailingSpaces);

    const RectF rect((REAL)L.pad, (REAL)L.pad,
                     (REAL)L.width - L.pad * 2.f, (REAL)L.height - L.pad * 2.f);

    FontFamily family;
    if (font.GetFamily(&family) != Ok) return;

    GraphicsPath path;
    if (path.AddString(text.c_str(), (INT)text.size(), &family,
                       FontStyleRegular, L.em, rect, &sf) != Ok) {
        return;
    }

    if (c.hdShadow) {
        GraphicsPath* shadow = path.Clone();
        if (shadow) {
            Matrix m;
            m.Translate(0.f, std::max(2.f, L.em * 0.05f));
            shadow->Transform(&m);
            SolidBrush sb(Color((BYTE)(a * 0.5f), 0, 0, 0));
            g.FillPath(&sb, shadow);
            delete shadow;
        }
    }

    auto brush = MakeBrush(c, rect, a);
    g.FillPath(brush.get(), &path);
}

} // namespace

void LayerBitmap::Release()
{
    if (dc) {
        DeleteDC(dc);
        dc = nullptr;
    }
    if (bmp) {
        DeleteObject(bmp);
        bmp = nullptr;
    }
    width = height = 0;
}

void Renderer::Measure(const Config& cfg, const std::wstring& text, int& width, int& height)
{
    width = height = 0;
    if (text.empty()) return;

    Layout L;
    if (!MakeLayout(cfg, text, L)) return;
    width  = L.width;
    height = L.height;
}

bool Renderer::Render(const Config& cfg, const std::wstring& text, float alpha, LayerBitmap& out)
{
    out.Release();
    if (text.empty() || alpha <= 0.004f) return false;

    Config c = cfg;
    c.Sanitize();

    const RECT wa = WorkArea();
    const int maxW = std::max(160, (int)(wa.right - wa.left) - 24);

    Layout L;
    for (int guard = 0; guard < 100; ++guard) {
        if (!MakeLayout(c, text, L)) return false;
        if (L.width <= maxW) break;
        if (c.style == RenderStyle::DotMatrix) {
            if (c.dotSize <= 2) break;
            --c.dotSize;
        } else {
            if (c.hdFontSize <= 12) break;
            c.hdFontSize = std::max(12, (int)(c.hdFontSize * 0.92f));
        }
    }

    if (!CreateLayerBitmap(L.width, L.height, out)) return false;

    {
        Graphics g(out.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        const BYTE a = (BYTE)std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f));

        if (c.showPanel) DrawPanel(g, L, a);
        if (c.style == RenderStyle::DotMatrix) DrawDots(g, c, text, L, a);
        else                                  DrawHD(g, c, text, L, a);

        g.Flush();
    }

    return true;
}
