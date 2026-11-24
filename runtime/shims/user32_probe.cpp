// user32_probe: window-creation diagnostics for a mapped title.
//
// The title's font/text loader registers a window class, creates a window and then makes its own D3D12Xbox device.
// When either window call fails the loader returns 0, asserts and later dereferences a null arena, which is the
// boot15/boot16 crash. Host user32 only says "invalid parameter"; this table sits in front of the four window calls
// so the class name, the styles and the exact failing call and GetLastError are in the log. It does not reimplement
// anything: every call goes to the host's user32 and the last error is put back exactly as the failure left it.
// Logging is off unless LEONIDA_TRACE_WINDOWS is set (any value).
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "shims.h"

namespace {

bool enabled() {
    static const bool on = [] {
        char value[8]{};
        return GetEnvironmentVariableA("LEONIDA_TRACE_WINDOWS", value, sizeof(value)) > 0;
    }();
    return on;
}

void report(const char* call, const char* detail, const void* result, DWORD error) {
    if (!enabled()) return;
    if (error)
        std::fprintf(stderr, "[rt] user32: %s %s -> 0 (GetLastError %lu)\n", call, detail, error);
    else
        std::fprintf(stderr, "[rt] user32: %s %s -> %p\n", call, detail, result);
}

ATOM WINAPI trace_register_class_ex_a(const WNDCLASSEXA* wc) {
    SetLastError(0);
    const ATOM r = ::RegisterClassExA(wc);
    const DWORD err = r ? 0 : GetLastError();
    char detail[512]{};
    if (wc) {
        std::snprintf(detail, sizeof(detail),
                      "{class='%s' style=0x%lx wndproc=%p hinstance=%p hbrBackground=%p}",
                      wc->lpszClassName ? wc->lpszClassName : "(null)", static_cast<unsigned long>(wc->style),
                      reinterpret_cast<void*>(wc->lpfnWndProc), wc->hInstance, wc->hbrBackground);
    } else {
        std::snprintf(detail, sizeof(detail), "(null)");
    }
    report("RegisterClassExA", detail, reinterpret_cast<const void*>(static_cast<uintptr_t>(r)), err);
    if (!r) SetLastError(err);
    return r;
}

ATOM WINAPI trace_register_class_ex_w(const WNDCLASSEXW* wc) {
    SetLastError(0);
    const ATOM r = ::RegisterClassExW(wc);
    const DWORD err = r ? 0 : GetLastError();
    char class_name[256]{};
    if (wc && wc->lpszClassName) {
        WideCharToMultiByte(CP_UTF8, 0, wc->lpszClassName, -1, class_name, sizeof(class_name), nullptr, nullptr);
    }
    char detail[512]{};
    std::snprintf(detail, sizeof(detail), "{class='%s' style=0x%lx wndproc=%p hinstance=%p}",
                  wc ? class_name : "(null)", wc ? static_cast<unsigned long>(wc->style) : 0ul,
                  wc ? reinterpret_cast<void*>(wc->lpfnWndProc) : nullptr, wc ? wc->hInstance : nullptr);
    report("RegisterClassExW", detail, reinterpret_cast<const void*>(static_cast<uintptr_t>(r)), err);
    if (!r) SetLastError(err);
    return r;
}

HWND WINAPI trace_create_window_ex_a(DWORD ex_style, LPCSTR class_name, LPCSTR title, DWORD style, int x, int y,
                                     int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void* param) {
    SetLastError(0);
    const HWND hwnd = ::CreateWindowExA(ex_style, class_name, title, style, x, y, width, height, parent, menu, instance,
                                        param);
    const DWORD err = hwnd ? 0 : GetLastError();
    char detail[768]{};
    std::snprintf(detail, sizeof(detail),
                  "{class='%s' title='%s' exstyle=0x%lx style=0x%lx rect=%d,%d %dx%d parent=%p hinstance=%p}",
                  class_name ? class_name : "(null)", title ? title : "(null)",
                  static_cast<unsigned long>(ex_style), static_cast<unsigned long>(style), x, y, width, height, parent,
                  instance);
    report("CreateWindowExA", detail, hwnd, err);
    if (!hwnd) SetLastError(err);
    return hwnd;
}

HWND WINAPI trace_create_window_ex_w(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y,
                                     int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void* param) {
    SetLastError(0);
    const HWND hwnd = ::CreateWindowExW(ex_style, class_name, title, style, x, y, width, height, parent, menu, instance,
                                        param);
    const DWORD err = hwnd ? 0 : GetLastError();
    char detail[768]{};
    std::snprintf(detail, sizeof(detail), "{class=%p title=%p exstyle=0x%lx style=0x%lx rect=%d,%d %dx%d parent=%p}",
                  class_name, title, static_cast<unsigned long>(ex_style), static_cast<unsigned long>(style), x, y, width,
                  height, parent);
    report("CreateWindowExW", detail, hwnd, err);
    if (!hwnd) SetLastError(err);
    return hwnd;
}

// The title imports these through the API sets, not "user32.dll" (iatq.py at 0x6793c58/0x6793c48):
// ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0 for the A calls and ext-ms-win-rtcore-ntuser-window-l1-1-0
// for the W calls. owns_dll() keeps every ext-ms-* name outside the registry, so only these four entries divert.
SHIM_TABLE(user32_probe,
           {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "RegisterClassExA",
            reinterpret_cast<void*>(&trace_register_class_ex_a)},
           {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA",
            reinterpret_cast<void*>(&trace_create_window_ex_a)},
           {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "RegisterClassExW",
            reinterpret_cast<void*>(&trace_register_class_ex_w)},
           {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "CreateWindowExW",
            reinterpret_cast<void*>(&trace_create_window_ex_w)});

}  // namespace
