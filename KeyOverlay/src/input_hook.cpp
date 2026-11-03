#include "input_hook.h"

namespace {
bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}

InputHook* InputHook::s_active = nullptr;

bool InputHook::Install(HWND target)
{
    m_target = target;
    s_active = this;
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &InputHook::Proc, GetModuleHandleW(nullptr), 0);
    if (!m_hook) {
        s_active = nullptr;
        m_target = nullptr;
        return false;
    }
    return true;
}

void InputHook::Uninstall()
{
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (s_active == this) s_active = nullptr;
    m_target = nullptr;
}

LRESULT CALLBACK InputHook::Proc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_active && s_active->m_target) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            LPARAM payload = (LPARAM)((info->scanCode & 0xFFFF) |
                                      ((info->flags & LLKHF_EXTENDED) ? kKeyExt : 0));
            // Modifier state is sampled here, synchronously with the key event.
            if (KeyDown(VK_CONTROL)) payload |= kKeyCtrl;
            if (KeyDown(VK_MENU))    payload |= kKeyAlt;
            if (KeyDown(VK_SHIFT))   payload |= kKeyShift;
            PostMessageW(s_active->m_target, WM_KEYOVERLAY_KEYSTROKE, (WPARAM)info->vkCode, payload);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
