#pragma once

#include <windows.h>
#include <string>

#include "config.h"

// A 32-bit premultiplied ARGB DIB section that can be handed to UpdateLayeredWindow.
struct LayerBitmap
{
    HBITMAP bmp    = nullptr;
    HDC     dc     = nullptr;
    int     width  = 0;
    int     height = 0;

    void Release();
};

class Renderer
{
public:
    // Draws `text` using `cfg`. `alpha` is the global fade factor (0..1).
    // Returns false when there is nothing to draw.
    static bool Render(const Config& cfg, const std::wstring& text, float alpha, LayerBitmap& out);

    // Bounding box the text would occupy (used for layout / clamping).
    static void Measure(const Config& cfg, const std::wstring& text, int& width, int& height);
};
