#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::wstring Trim(const std::wstring& s)
{
    const wchar_t* ws = L" \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return L"";
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool ParseColor(const std::wstring& v, COLORREF& out)
{
    unsigned int r = 0, g = 0, b = 0;
    if (swscanf_s(v.c_str(), L"%u,%u,%u", &r, &g, &b) == 3) {
        out = RGB(r & 0xFF, g & 0xFF, b & 0xFF);
        return true;
    }
    return false;
}

std::string ToUtf8(COLORREF c)
{
    char buf[64];
    sprintf_s(buf, "%u,%u,%u", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

void Config::Sanitize()
{
    dotSize    = Clamp(dotSize, 2, 24);
    dotRows    = Clamp(dotRows, 8, 32);
    hdFontSize = Clamp(hdFontSize, 12, 240);
    margin     = Clamp(margin, 0, 2000);
    opacityPct = Clamp(opacityPct, 20, 100);
    if (fontFamily.empty()) fontFamily = L"Microsoft YaHei UI";
}

void Config::Load(const std::wstring& path)
{
    std::ifstream in(path.c_str());
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::wstring key = Trim(FromUtf8(line.substr(0, eq)));
        std::wstring val = Trim(FromUtf8(line.substr(eq + 1)));
        if (key.empty()) continue;

        if (key == L"style") style = (val == L"dot") ? RenderStyle::DotMatrix : RenderStyle::HD;
        else if (key == L"colorMode") colorMode = (val == L"solid") ? PaintMode::Solid : PaintMode::Gradient;
        else if (key == L"solidColor") ParseColor(val, solidColor);
        else if (key == L"gradStart") ParseColor(val, gradStart);
        else if (key == L"gradEnd") ParseColor(val, gradEnd);
        else if (key == L"gradVertical") gradVertical = (val == L"1" || val == L"true");
        else if (key == L"dotSize") dotSize = _wtoi(val.c_str());
        else if (key == L"dotRows") dotRows = _wtoi(val.c_str());
        else if (key == L"hdFontSize") hdFontSize = _wtoi(val.c_str());
        else if (key == L"placement") {
            if (val == L"top")         placement = Placement::TopCenter;
            else if (val == L"bottom") placement = Placement::BottomCenter;
            else if (val == L"left")   placement = Placement::LeftCenter;
            else                       placement = Placement::RightCenter;
        }
        // "topMargin" is the pre-0.4 key name, still accepted for the edge gap.
        else if (key == L"margin" || key == L"topMargin") margin = _wtoi(val.c_str());
        else if (key == L"opacityPct") opacityPct = _wtoi(val.c_str());
        else if (key == L"showPanel") showPanel = (val == L"1" || val == L"true");
        else if (key == L"hdShadow") hdShadow = (val == L"1" || val == L"true");
        else if (key == L"fontFamily") fontFamily = val;
    }
    Sanitize();
}

void Config::Save(const std::wstring& path) const
{
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out) return;

    out << "; KeyOverlay settings (edit while the app is closed)\n";
    out << "style=" << (style == RenderStyle::DotMatrix ? "dot" : "hd") << "\n";
    out << "colorMode=" << (colorMode == PaintMode::Solid ? "solid" : "gradient") << "\n";
    out << "solidColor=" << ToUtf8(solidColor) << "\n";
    out << "gradStart=" << ToUtf8(gradStart) << "\n";
    out << "gradEnd=" << ToUtf8(gradEnd) << "\n";
    out << "gradVertical=" << (gradVertical ? 1 : 0) << "\n";
    out << "dotSize=" << dotSize << "\n";
    out << "dotRows=" << dotRows << "\n";
    out << "hdFontSize=" << hdFontSize << "\n";
    out << "placement=" << (placement == Placement::TopCenter    ? "top"
                            : placement == Placement::BottomCenter ? "bottom"
                            : placement == Placement::LeftCenter   ? "left"
                                                                   : "right") << "\n";
    out << "margin=" << margin << "\n";
    out << "opacityPct=" << opacityPct << "\n";
    out << "showPanel=" << (showPanel ? 1 : 0) << "\n";
    out << "hdShadow=" << (hdShadow ? 1 : 0) << "\n";
    out << "fontFamily=" << ToUtf8(fontFamily) << "\n";
}

std::wstring ConfigFilePath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    return p + L"KeyOverlay.ini";
}
