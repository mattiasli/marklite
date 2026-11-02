// Light / dark palettes and the Win32 dark-mode plumbing (title bar, menus, scrollbars).
#include "app.h"
#include <uxtheme.h>
#include <dwmapi.h>
#include <unordered_map>

namespace {
bool g_dark = false;

const Palette kLight = {
    /*bg*/ RGB(0xFF, 0xFF, 0xFF), /*fg*/ RGB(0x1F, 0x23, 0x28), /*dim*/ RGB(0x8C, 0x95, 0x9F),
    /*accent*/ RGB(0x09, 0x69, 0xDA), /*link*/ RGB(0x09, 0x69, 0xDA), /*heading*/ RGB(0x10, 0x18, 0x28),
    /*codeBg*/ RGB(0xF3, 0xF4, 0xF6), /*codeFg*/ RGB(0x24, 0x29, 0x2F), /*quote*/ RGB(0x57, 0x60, 0x6A),
    /*html*/ RGB(0x82, 0x50, 0xDF), /*mark*/ RGB(0xA0, 0xA8, 0xB2), /*listMark*/ RGB(0x09, 0x69, 0xDA),
    /*chromeBg*/ RGB(0xF3, 0xF3, 0xF3), /*chromeFg*/ RGB(0x1F, 0x1F, 0x1F), /*chromeDim*/ RGB(0x6E, 0x6E, 0x6E),
    /*tabActive*/ RGB(0xFF, 0xFF, 0xFF), /*tabHover*/ RGB(0xE6, 0xE6, 0xE6), /*border*/ RGB(0xDA, 0xDC, 0xE0),
    /*statusBg*/ RGB(0xF3, 0xF3, 0xF3), /*statusFg*/ RGB(0x50, 0x55, 0x5C),
    /*selBg*/ RGB(0xB4, 0xD5, 0xFE), /*caretLine*/ RGB(0xF6, 0xF8, 0xFA), /*spell*/ RGB(0xE5, 0x14, 0x00),
    /*findHit*/ RGB(0xFF, 0xC8, 0x3D), /*aiSel*/ RGB(0x8E, 0x6C, 0xEF),
    /*inputBg*/ RGB(0xFF, 0xFF, 0xFF), /*inputFg*/ RGB(0x1F, 0x23, 0x28),
    /*btnBg*/ RGB(0xFB, 0xFB, 0xFB), /*btnHover*/ RGB(0xEC, 0xEC, 0xEC),
    /*btnPrimary*/ RGB(0x09, 0x69, 0xDA), /*btnPrimaryFg*/ RGB(0xFF, 0xFF, 0xFF),
    /*error*/ RGB(0xC4, 0x2B, 0x1C), /*ok*/ RGB(0x1A, 0x7F, 0x37),
};

const Palette kDark = {
    /*bg*/ RGB(0x1E, 0x1E, 0x1E), /*fg*/ RGB(0xD8, 0xDB, 0xDF), /*dim*/ RGB(0x7D, 0x85, 0x90),
    /*accent*/ RGB(0x4C, 0x9A, 0xFF), /*link*/ RGB(0x58, 0xA6, 0xFF), /*heading*/ RGB(0xEE, 0xF1, 0xF5),
    /*codeBg*/ RGB(0x2A, 0x2B, 0x2E), /*codeFg*/ RGB(0xE6, 0xE8, 0xEB), /*quote*/ RGB(0x9A, 0xA3, 0xAD),
    /*html*/ RGB(0xC5, 0x9C, 0xFF), /*mark*/ RGB(0x5E, 0x66, 0x70), /*listMark*/ RGB(0x58, 0xA6, 0xFF),
    /*chromeBg*/ RGB(0x18, 0x18, 0x18), /*chromeFg*/ RGB(0xE0, 0xE0, 0xE0), /*chromeDim*/ RGB(0x9A, 0x9A, 0x9A),
    /*tabActive*/ RGB(0x1E, 0x1E, 0x1E), /*tabHover*/ RGB(0x2A, 0x2A, 0x2A), /*border*/ RGB(0x33, 0x33, 0x36),
    /*statusBg*/ RGB(0x18, 0x18, 0x18), /*statusFg*/ RGB(0xA8, 0xAE, 0xB6),
    /*selBg*/ RGB(0x26, 0x4F, 0x78), /*caretLine*/ RGB(0x25, 0x26, 0x28), /*spell*/ RGB(0xFF, 0x5F, 0x52),
    /*findHit*/ RGB(0x9E, 0x6A, 0x03), /*aiSel*/ RGB(0x8E, 0x6C, 0xEF),
    /*inputBg*/ RGB(0x2B, 0x2B, 0x2B), /*inputFg*/ RGB(0xE6, 0xE6, 0xE6),
    /*btnBg*/ RGB(0x2D, 0x2D, 0x2D), /*btnHover*/ RGB(0x3A, 0x3A, 0x3A),
    /*btnPrimary*/ RGB(0x2F, 0x7B, 0xE6), /*btnPrimaryFg*/ RGB(0xFF, 0xFF, 0xFF),
    /*error*/ RGB(0xFF, 0x6B, 0x5E), /*ok*/ RGB(0x56, 0xD3, 0x64),
};

// Undocumented uxtheme exports used by Explorer/Notepad++ for dark menus & scrollbars (Win10 1903+).
enum PreferredAppMode { PAM_Default, PAM_AllowDark, PAM_ForceDark, PAM_ForceLight };
using FnSetPreferredAppMode = int(WINAPI*)(int);
using FnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);
using FnFlushMenuThemes = void(WINAPI*)();
using FnRefreshPolicy = void(WINAPI*)();
FnSetPreferredAppMode pSetPreferredAppMode = nullptr;
FnAllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
FnFlushMenuThemes pFlushMenuThemes = nullptr;
FnRefreshPolicy pRefreshPolicy = nullptr;

std::unordered_map<COLORREF, HBRUSH> g_brushes;

// UAH menu bar drawing structures (undocumented, stable since Win10).
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#define WM_UAHMEASUREMENUITEM 0x0094
typedef union {
    struct { DWORD cx, cy; } rgsizeBar[2];
    struct { DWORD cx, cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

void PaintMenuBottomLine(HWND h) {
    MENUBARINFO mbi{sizeof(mbi)};
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return;
    RECT rc, rw;
    GetClientRect(h, &rc);
    MapWindowPoints(h, nullptr, (POINT*)&rc, 2);
    GetWindowRect(h, &rw);
    OffsetRect(&rc, -rw.left, -rw.top);
    RECT line = rc;
    line.bottom = line.top;
    line.top--;
    HDC dc = GetWindowDC(h);
    FillRect(dc, &line, Theme_Brush(Pal().chromeBg));
    ReleaseDC(h, dc);
}
}

bool Theme_IsDark() { return g_dark; }
const Palette& Pal() { return g_dark ? kDark : kLight; }

HBRUSH Theme_Brush(COLORREF c) {
    auto it = g_brushes.find(c);
    if (it != g_brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    g_brushes[c] = b;
    return b;
}

void Theme_Init() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    // Only safe on Windows 10 1903 (build 18362) and later.
    using RtlGetNtVersionNumbersFn = void(WINAPI*)(LPDWORD, LPDWORD, LPDWORD);
    auto rtl = (RtlGetNtVersionNumbersFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
    DWORD major = 0, minor = 0, build = 0;
    if (rtl) { rtl(&major, &minor, &build); build &= ~0xF0000000; }
    if (major < 10 || build < 18362) return;
    pSetPreferredAppMode = (FnSetPreferredAppMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    pAllowDarkModeForWindow = (FnAllowDarkModeForWindow)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    pFlushMenuThemes = (FnFlushMenuThemes)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    pRefreshPolicy = (FnRefreshPolicy)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(104));
}

bool Theme_SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

void Theme_Set(bool dark) {
    g_dark = dark;
    if (pSetPreferredAppMode) pSetPreferredAppMode(dark ? PAM_ForceDark : PAM_ForceLight);
    if (pRefreshPolicy) pRefreshPolicy();
    if (pFlushMenuThemes) pFlushMenuThemes();
}

void Theme_TitleBar(HWND h) {
    BOOL d = g_dark;
    if (FAILED(DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d)))
        DwmSetWindowAttribute(h, 19, &d, sizeof d);
}

void Theme_Control(HWND h, const wchar_t* lightTheme) {
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, g_dark);
    SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : lightTheme, nullptr);
}

void Theme_Edit(HWND h) {
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, g_dark);
    SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : L"CFD", nullptr);
}

LRESULT Theme_CtlColor(HDC hdc, bool input) {
    const Palette& p = Pal();
    COLORREF bg = input ? p.inputBg : p.chromeBg;
    SetTextColor(hdc, input ? p.inputFg : p.chromeFg);
    SetBkColor(hdc, bg);
    return (LRESULT)Theme_Brush(bg);
}

bool Theme_MenuBarProc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res) {
    if (!g_dark) return false;
    const Palette& p = Pal();
    switch (m) {
    case WM_UAHDRAWMENU: {
        auto* udm = (UAHMENU*)l;
        MENUBARINFO mbi{sizeof(mbi)};
        GetMenuBarInfo(h, OBJID_MENU, 0, &mbi);
        RECT rw;
        GetWindowRect(h, &rw);
        RECT rc = mbi.rcBar;
        OffsetRect(&rc, -rw.left, -rw.top);
        rc.top -= 1;
        FillRect(udm->hdc, &rc, Theme_Brush(p.chromeBg));
        *res = TRUE;
        return true;
    }
    case WM_UAHDRAWMENUITEM: {
        auto* di = (UAHDRAWMENUITEM*)l;
        wchar_t text[256] = {};
        MENUITEMINFOW mii{sizeof(mii), MIIM_STRING};
        mii.dwTypeData = text;
        mii.cch = 255;
        GetMenuItemInfoW(di->um.hmenu, di->umi.iPosition, TRUE, &mii);
        UINT st = di->dis.itemState;
        COLORREF bg = p.chromeBg;
        if (st & (ODS_HOTLIGHT | ODS_SELECTED)) bg = p.tabHover;
        FillRect(di->um.hdc, &di->dis.rcItem, Theme_Brush(bg));
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (st & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
        SetBkMode(di->um.hdc, TRANSPARENT);
        SetTextColor(di->um.hdc, (st & (ODS_INACTIVE | ODS_GRAYED)) ? p.chromeDim : p.chromeFg);
        DrawTextW(di->um.hdc, text, -1, &di->dis.rcItem, flags);
        *res = TRUE;
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE:
        *res = DefWindowProcW(h, m, w, l);
        PaintMenuBottomLine(h);
        return true;
    }
    return false;
}
