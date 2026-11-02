// Formatting toolbar: a light, owner-drawn strip of icon buttons and dropdowns.
// Buttons post WM_COMMAND(cmd) to the parent; dropdowns show a menu supplied by the parent.
#pragma once
#include "app.h"

enum TbKind {
    TB_BUTTON,      // icon button
    TB_DROPDOWN,    // icon + chevron, opens a menu
    TB_LABELDROP,   // text label + chevron (paragraph style), opens a menu
    TB_TEXTBUTTON,  // icon + text label
    TB_SEP,         // vertical separator
    TB_SPRING,      // pushes the following items to the right edge
};
enum TbIcon {
    TI_GLYPH,       // `glyph` drawn with Segoe MDL2 Assets
    TI_SYMBOL,      // `glyph` drawn with Segoe UI Symbol
    TI_INLINECODE,  // custom "</>"
    TI_NUMLIST,     // custom numbered list
    TI_SPLIT,       // custom split-view rectangle
};

struct TbItem {
    int cmd = 0;
    TbKind kind = TB_BUTTON;
    std::wstring glyph;
    TbIcon icon = TI_GLYPH;
    std::wstring name;    // tooltip and overflow-menu text
    std::wstring key;     // keyboard shortcut shown with the name (may be empty)
    std::wstring label;   // visible text of TB_LABELDROP / TB_TEXTBUTTON items
};

// Builds the popup menu for a dropdown item (and for dropdowns listed in the overflow menu).
// The toolbar destroys the menu after showing it.
typedef HMENU (*TbMenuFn)(int cmd);

HWND Toolbar_Create(HWND parent, const std::vector<TbItem>& items, TbMenuFn menuFn);
int  Toolbar_Height(HWND h);
void Toolbar_SetChecked(HWND h, int cmd, bool on);
void Toolbar_SetEnabled(HWND h, int cmd, bool on);
void Toolbar_SetLabel(HWND h, int cmd, const std::wstring& label);
void Toolbar_ThemeChanged(HWND h);
