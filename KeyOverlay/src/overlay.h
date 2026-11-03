#pragma once

#include <windows.h>
#include <string>

#include "config.h"

// Topmost, click-through, per-pixel-alpha window that shows the text buffer.
class Overlay
{
public:
    bool Create(HINSTANCE hInst);
    void Destroy();

    // Renders `text` and moves/resizes the window to match the content.
    void Render(const Config& cfg, const std::wstring& text, float alpha);
    void Hide();

    HWND Handle() const { return m_hwnd; }

private:
    HWND m_hwnd = nullptr;
};
