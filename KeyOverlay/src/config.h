#pragma once

#include <windows.h>
#include <string>

enum class RenderStyle { HD = 0, DotMatrix = 1 };
enum class PaintMode { Solid = 0, Gradient = 1 };
// Where the overlay sits inside the monitor work area.
enum class Placement { TopCenter = 0, RightCenter = 1, BottomCenter = 2, LeftCenter = 3 };

struct Config
{
    // ---- look ----
    RenderStyle style      = RenderStyle::HD;
    PaintMode    colorMode  = PaintMode::Gradient;
    COLORREF    solidColor = RGB(0, 234, 255);
    COLORREF    gradStart  = RGB(0, 234, 255);
    COLORREF    gradEnd    = RGB(255, 60, 190);
    bool        gradVertical = false;

    // ---- size ----
    int dotSize    = 4;    // dot matrix: pixels per dot
    int dotRows    = 16;   // dot matrix: dot rows per line
    int hdFontSize = 64;   // HD: font height in pixels

    // ---- behaviour ----
    Placement placement = Placement::RightCenter;
    int  margin     = 48;     // distance from the screen edge, in pixels
    int  opacityPct = 100;
    bool showPanel  = false;  // translucent rounded backdrop
    bool hdShadow   = true;
    std::wstring fontFamily = L"Microsoft YaHei UI";

    // Clamp every value into a sane range (protects against a hand-edited ini).
    void Sanitize();

    void Load(const std::wstring& path);
    void Save(const std::wstring& path) const;
};

// <exe folder>\KeyOverlay.ini
std::wstring ConfigFilePath();
