#pragma once

#include <windows.h>

// Posted to the target window for every physical key press.
// wParam = virtual key code.
// lParam = state captured at event time (the target thread may process it late):
//   bit 0-15   scan code
//   bit 16     extended key
//   bit 17     Ctrl held
//   bit 18     Alt held
//   bit 19     Shift held
constexpr UINT WM_KEYOVERLAY_KEYSTROKE = WM_APP + 1;

constexpr LPARAM kKeyExt   = 1 << 16;
constexpr LPARAM kKeyCtrl  = 1 << 17;
constexpr LPARAM kKeyAlt   = 1 << 18;
constexpr LPARAM kKeyShift = 1 << 19;

class InputHook
{
public:
    bool Install(HWND target);
    void Uninstall();

    ~InputHook() { Uninstall(); }

private:
    HHOOK m_hook = nullptr;
    HWND  m_target = nullptr;

    static LRESULT CALLBACK Proc(int code, WPARAM wParam, LPARAM lParam);
    static InputHook* s_active;
};
