// Owner-drawn formatting toolbar (GDI, double-buffered). See toolbar.h.
#include "toolbar.h"
#include "resource.h"
#include <windowsx.h>
#include <commctrl.h>

namespace {
struct TbState {
    std::vector<TbItem> items;
    std::vector<RECT> rects;           // empty rect = hidden
    std::vector<bool> checked, enabled;
    TbMenuFn menuFn = nullptr;
    int hover = -1, pressed = -1, open = -1;
    int menuClosed = -2;
    DWORD menuClosedTick = 0;
    RECT overflow{};                   // "more items" button when the window is too narrow
    bool hasOverflow = false;
    bool tracking = false;
    int dpi = 96;
    HFONT glyph = nullptr, chevron = nullptr, label = nullptr, code = nullptr, digits = nullptr, symbol = nullptr;
    HWND tip = nullptr;
};
const int kOverflow = 100000;          // pseudo index of the overflow button

TbState* St(HWND h) { return (TbState*)GetWindowLongPtrW(h, GWLP_USERDATA); }

COLORREF Blend(COLORREF a, COLORREF b, float t) {   // t = weight of a
    auto mix = [&](int x, int y) { return (BYTE)(x * t + y * (1 - t) + 0.5f); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void FreeFonts(TbState* s) {
    for (HFONT f : {s->glyph, s->chevron, s->label, s->code, s->digits, s->symbol})
        if (f) DeleteObject(f);
}

void MakeFonts(HWND h, TbState* s) {
    FreeFonts(s);
    s->dpi = Dpi(h);
    int d = s->dpi;
    auto mk = [&](int px, int weight, const wchar_t* face) {
        return CreateFontW(-Sc(px, d), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    s->glyph = mk(16, FW_NORMAL, L"Segoe MDL2 Assets");
    s->chevron = mk(8, FW_NORMAL, L"Segoe MDL2 Assets");
    s->code = mk(13, FW_BOLD, L"Consolas");
    s->digits = mk(8, FW_SEMIBOLD, L"Segoe UI");
    s->symbol = mk(15, FW_NORMAL, L"Segoe UI Symbol");
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, d);
    ncm.lfMessageFont.lfHeight = -Sc(12, d);
    s->label = CreateFontIndirectW(&ncm.lfMessageFont);
}

int TextW(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

int ItemWidth(TbState* s, HDC dc, const TbItem& it) {
    int d = s->dpi;
    switch (it.kind) {
    case TB_BUTTON: return Sc(30, d);
    case TB_DROPDOWN: return Sc(40, d);
    case TB_LABELDROP: return Sc(122, d);
    case TB_TEXTBUTTON: return Sc(42, d) + TextW(dc, s->label, it.label);
    case TB_SEP: return Sc(11, d);
    case TB_SPRING: return 0;
    }
    return 0;
}

void UpdateTips(HWND h, TbState* s) {
    if (!s->tip) return;
    for (size_t i = 0; i < s->items.size(); i++) {
        TOOLINFOW ti{sizeof(ti)};
        ti.hwnd = h;
        ti.uId = i + 1;
        ti.rect = s->rects[i];
        SendMessageW(s->tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
    TOOLINFOW ti{sizeof(ti)};
    ti.hwnd = h;
    ti.uId = kOverflow;
    ti.rect = s->hasOverflow ? s->overflow : RECT{};
    SendMessageW(s->tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

void Layout(HWND h, TbState* s) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = GetDC(h);
    int d = s->dpi, pad = Sc(6, d), bh = Sc(28, d);
    int top = (rc.bottom - Sc(1, d) - bh) / 2, avail = rc.right - pad;
    size_t n = s->items.size(), spring = n;
    std::vector<int> w(n);
    for (size_t i = 0; i < n; i++) {
        w[i] = ItemWidth(s, dc, s->items[i]);
        if (s->items[i].kind == TB_SPRING && spring == n) spring = i;
    }
    ReleaseDC(h, dc);
    int leftW = 0, rightW = 0;
    for (size_t i = 0; i < n; i++) (i < spring ? leftW : rightW) += w[i];
    s->rects.assign(n, RECT{});
    s->hasOverflow = false;
    bool fits = pad + leftW + Sc(16, d) + rightW <= avail;
    int x = pad;
    int ovW = Sc(30, d);
    for (size_t i = 0; i < n; i++) {
        if (i == spring) {
            if (fits) x = avail - rightW;
            continue;
        }
        if (s->items[i].kind == TB_SPRING) continue;
        if (!fits && x + w[i] > avail - ovW) {
            // everything from here on goes into the overflow menu
            s->hasOverflow = true;
            break;
        }
        s->rects[i] = {x, top, x + w[i], top + bh};
        x += w[i];
    }
    // a separator never ends the visible row
    for (size_t i = n; i-- > 0;) {
        if (IsRectEmpty(&s->rects[i])) continue;
        if (s->items[i].kind == TB_SEP) s->rects[i] = RECT{};
        break;
    }
    if (s->hasOverflow) s->overflow = {avail - ovW, top, avail, top + bh};
    UpdateTips(h, s);
}

bool Clickable(const TbItem& it) { return it.kind != TB_SEP && it.kind != TB_SPRING; }

int HitTest(TbState* s, POINT pt) {
    if (s->hasOverflow && PtInRect(&s->overflow, pt)) return kOverflow;
    for (size_t i = 0; i < s->items.size(); i++)
        if (Clickable(s->items[i]) && PtInRect(&s->rects[i], pt)) return (int)i;
    return -1;
}

void DrawGlyph(HDC dc, HFONT f, const std::wstring& g, RECT r, COLORREF c) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, g.c_str(), (int)g.size(), &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, o);
}

void DrawIcon(TbState* s, HDC dc, const TbItem& it, RECT r, COLORREF c) {
    int d = s->dpi;
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int th = std::max(1, Sc(1, d) + (d >= 144 ? 1 : 0) - (d >= 192 ? 1 : 0));   // line thickness
    HBRUSH br = Theme_Brush(c);
    switch (it.icon) {
    case TI_GLYPH: DrawGlyph(dc, s->glyph, it.glyph, r, c); break;
    case TI_SYMBOL: DrawGlyph(dc, s->symbol, it.glyph, r, c); break;
    case TI_INLINECODE: DrawGlyph(dc, s->code, L"</>", r, c); break;
    case TI_NUMLIST: {
        for (int k = 0; k < 3; k++) {
            int y = cy + Sc(-5 + 5 * k, d);
            RECT ln{cx - Sc(2, d), y - th / 2, cx + Sc(8, d), y - th / 2 + th};
            FillRect(dc, &ln, br);
            RECT nr{cx - Sc(10, d), y - Sc(5, d), cx - Sc(3, d), y + Sc(5, d)};
            wchar_t dg[2] = {(wchar_t)(L'1' + k), 0};
            DrawGlyph(dc, s->digits, dg, nr, c);
        }
        break;
    }
    case TI_SPLIT: {
        RECT o{cx - Sc(8, d), cy - Sc(6, d), cx + Sc(8, d), cy + Sc(7, d)};
        RECT e[4] = {{o.left, o.top, o.right, o.top + th}, {o.left, o.bottom - th, o.right, o.bottom},
                     {o.left, o.top, o.left + th, o.bottom}, {o.right - th, o.top, o.right, o.bottom}};
        for (auto& x : e) FillRect(dc, &x, br);
        RECT mid{cx - th / 2, o.top, cx - th / 2 + th, o.bottom};
        FillRect(dc, &mid, br);
        break;
    }
    }
}

void Paint(HWND h, TbState* s) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const Palette& p = Pal();
    COLORREF bg = p.tabActive;
    FillRect(dc, &rc, Theme_Brush(bg));
    RECT bl{0, rc.bottom - Sc(1, s->dpi), rc.right, rc.bottom};
    FillRect(dc, &bl, Theme_Brush(p.border));
    SetBkMode(dc, TRANSPARENT);
    int d = s->dpi, rad = Sc(8, d);
    COLORREF hoverBg = p.tabHover, pressBg = Blend(p.chromeFg, bg, 0.16f), checkBg = Blend(p.accent, bg, 0.22f);
    COLORREF disabled = Blend(p.chromeFg, bg, 0.38f);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    auto fillRound = [&](const RECT& r, COLORREF c) {
        HGDIOBJ ob = SelectObject(dc, Theme_Brush(c));
        RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
        SelectObject(dc, ob);
    };
    for (size_t i = 0; i < s->items.size(); i++) {
        const TbItem& it = s->items[i];
        RECT r = s->rects[i];
        if (IsRectEmpty(&r)) continue;
        if (it.kind == TB_SEP) {
            int x = (r.left + r.right) / 2;
            RECT ln{x, r.top + Sc(5, d), x + std::max(1, Sc(1, d)), r.bottom - Sc(5, d)};
            FillRect(dc, &ln, Theme_Brush(p.border));
            continue;
        }
        bool en = s->enabled[i], chk = s->checked[i];
        bool down = (int)i == s->open || ((int)i == s->pressed && (int)i == s->hover);
        if (en && down) fillRound(r, pressBg);
        else if (chk) fillRound(r, (en && (int)i == s->hover) ? Blend(p.accent, bg, 0.32f) : checkBg);
        else if (en && (int)i == s->hover) fillRound(r, hoverBg);
        COLORREF fg = !en ? disabled : chk ? p.accent : p.chromeFg;
        switch (it.kind) {
        case TB_BUTTON: DrawIcon(s, dc, it, r, fg); break;
        case TB_DROPDOWN: {
            RECT ir = r, cr = r;
            ir.right -= Sc(10, d);
            cr.left = ir.right - Sc(3, d);
            cr.right -= Sc(3, d);
            DrawIcon(s, dc, it, ir, fg);
            DrawGlyph(dc, s->chevron, L"\xE70D", cr, fg);
            break;
        }
        case TB_LABELDROP: {
            RECT tr = r, cr = r;
            tr.left += Sc(10, d);
            tr.right -= Sc(22, d);
            cr.left = r.right - Sc(22, d);
            HGDIOBJ o = SelectObject(dc, s->label);
            SetTextColor(dc, fg);
            DrawTextW(dc, it.label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, o);
            DrawGlyph(dc, s->chevron, L"\xE70D", cr, fg);
            break;
        }
        case TB_TEXTBUTTON: {
            RECT ir = r, tr = r;
            ir.left += Sc(6, d);
            ir.right = ir.left + Sc(22, d);
            tr.left = ir.right + Sc(4, d);
            DrawIcon(s, dc, it, ir, fg);
            HGDIOBJ o = SelectObject(dc, s->label);
            SetTextColor(dc, fg);
            DrawTextW(dc, it.label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, o);
            break;
        }
        default: break;
        }
    }
    if (s->hasOverflow) {
        RECT r = s->overflow;
        if (s->open == kOverflow) fillRound(r, pressBg);
        else if (s->hover == kOverflow) fillRound(r, hoverBg);
        HGDIOBJ o = SelectObject(dc, s->label);
        SetTextColor(dc, p.chromeFg);
        DrawTextW(dc, L"\x00BB", 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(dc, o);
    }
    SelectObject(dc, oldPen);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

HMENU OverflowMenu(TbState* s) {
    HMENU m = CreatePopupMenu();
    bool needSep = false;
    for (size_t i = 0; i < s->items.size(); i++) {
        const TbItem& it = s->items[i];
        if (!IsRectEmpty(&s->rects[i])) continue;
        if (it.kind == TB_SEP || it.kind == TB_SPRING) { needSep = GetMenuItemCount(m) > 0; continue; }
        if (needSep) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); needSep = false; }
        UINT fl = s->enabled[i] ? 0 : MF_GRAYED;
        std::wstring name = it.name + (it.key.empty() ? std::wstring() : L"\t" + it.key);
        if (it.kind == TB_DROPDOWN || it.kind == TB_LABELDROP) {
            HMENU sub = s->menuFn ? s->menuFn(it.cmd) : nullptr;
            if (sub) AppendMenuW(m, MF_POPUP | fl, (UINT_PTR)sub, name.c_str());
        } else {
            AppendMenuW(m, MF_STRING | fl | (s->checked[i] ? MF_CHECKED : 0), (UINT_PTR)it.cmd, name.c_str());
        }
    }
    return m;
}

void ShowMenu(HWND h, TbState* s, int idx) {
    HMENU m = idx == kOverflow ? OverflowMenu(s) : (s->menuFn ? s->menuFn(s->items[idx].cmd) : nullptr);
    if (!m) return;
    RECT r = idx == kOverflow ? s->overflow : s->rects[idx];
    MapWindowPoints(h, nullptr, (POINT*)&r, 2);
    s->open = idx;
    InvalidateRect(h, nullptr, FALSE);
    UpdateWindow(h);
    TPMPARAMS tp{sizeof(tp), r};
    UINT align = idx == kOverflow ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_TOPALIGN | TPM_VERTICAL | align, idx == kOverflow ? r.right : r.left,
                                    r.bottom, GetParent(h), &tp);
    DestroyMenu(m);
    s->open = -1;
    s->menuClosed = idx;
    s->menuClosedTick = GetTickCount();
    InvalidateRect(h, nullptr, FALSE);
    if (cmd) PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

int Find(TbState* s, int cmd) {
    for (size_t i = 0; i < s->items.size(); i++)
        if (s->items[i].cmd == cmd && Clickable(s->items[i])) return (int)i;
    return -1;
}

void InvalidateItem(HWND h, TbState* s, int i) {
    if (i >= 0 && i < (int)s->rects.size() && !IsRectEmpty(&s->rects[i])) InvalidateRect(h, &s->rects[i], FALSE);
    else if (s->hasOverflow) InvalidateRect(h, &s->overflow, FALSE);
}

LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    TbState* s = St(h);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        break;
    case WM_DESTROY:
        if (s) {
            FreeFonts(s);
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        MakeFonts(h, s);
        Layout(h, s);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(h, s); return 0;
    case WM_SIZE: Layout(h, s); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: {
        if (!s->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            s->tracking = true;
        }
        int hv = HitTest(s, {GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (hv != s->hover) {
            InvalidateItem(h, s, s->hover);
            s->hover = hv;
            InvalidateItem(h, s, hv);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking = false;
        InvalidateItem(h, s, s->hover);
        s->hover = -1;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int i = HitTest(s, {GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (i < 0) return 0;
        // the click that dismissed this item's menu must not reopen it
        if (i == s->menuClosed && GetTickCount() - s->menuClosedTick < 250) { s->menuClosed = -2; return 0; }
        if (i == kOverflow) { ShowMenu(h, s, i); return 0; }
        if (!s->enabled[i]) return 0;
        TbKind k = s->items[i].kind;
        if (k == TB_DROPDOWN || k == TB_LABELDROP) { ShowMenu(h, s, i); return 0; }
        s->pressed = i;
        SetCapture(h);
        InvalidateItem(h, s, i);
        return 0;
    }
    case WM_LBUTTONUP: {
        int pr = s->pressed;
        if (pr < 0) return 0;
        s->pressed = -1;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateItem(h, s, pr);
        if (HitTest(s, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}) == pr)
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(s->items[pr].cmd, 0), 0);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (s->pressed >= 0 && (HWND)l != h) {
            InvalidateItem(h, s, s->pressed);
            s->pressed = -1;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}

HWND Toolbar_Create(HWND parent, const std::vector<TbItem>& items, TbMenuFn menuFn) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = ToolbarProc;
        wc.hInstance = App_Instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"MarkLiteToolbar";
        RegisterClassW(&wc);
        reg = true;
    }
    auto* s = new TbState();
    s->items = items;
    s->checked.assign(items.size(), false);
    s->enabled.assign(items.size(), true);
    s->rects.assign(items.size(), RECT{});
    s->menuFn = menuFn;
    HWND h = CreateWindowExW(0, L"MarkLiteToolbar", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr,
                             App_Instance(), s);
    if (!h) { delete s; return nullptr; }
    MakeFonts(h, s);
    s->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h, nullptr, App_Instance(), nullptr);
    if (s->tip) {
        Theme_Control(s->tip);
        for (size_t i = 0; i <= items.size(); i++) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = h;
            ti.uId = i < items.size() ? i + 1 : kOverflow;
            std::wstring t = L"More buttons";
            if (i < items.size()) t = items[i].name + (items[i].key.empty() ? std::wstring() : L" (" + items[i].key + L")");
            ti.lpszText = (LPWSTR)t.c_str();
            if (i < items.size() && !Clickable(items[i])) continue;
            SendMessageW(s->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        SendMessageW(s->tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    return h;
}

int Toolbar_Height(HWND h) { return Sc(38, Dpi(h)); }

void Toolbar_SetChecked(HWND h, int cmd, bool on) {
    TbState* s = St(h);
    int i = s ? Find(s, cmd) : -1;
    if (i < 0 || s->checked[i] == on) return;
    s->checked[i] = on;
    InvalidateItem(h, s, i);
}

void Toolbar_SetEnabled(HWND h, int cmd, bool on) {
    TbState* s = St(h);
    int i = s ? Find(s, cmd) : -1;
    if (i < 0 || s->enabled[i] == on) return;
    s->enabled[i] = on;
    InvalidateItem(h, s, i);
}

void Toolbar_SetLabel(HWND h, int cmd, const std::wstring& label) {
    TbState* s = St(h);
    int i = s ? Find(s, cmd) : -1;
    if (i < 0 || s->items[i].label == label) return;
    s->items[i].label = label;
    if (s->items[i].kind == TB_TEXTBUTTON) Layout(h, s);
    InvalidateItem(h, s, i);
}

void Toolbar_ThemeChanged(HWND h) {
    TbState* s = St(h);
    if (s && s->tip) Theme_Control(s->tip);
    InvalidateRect(h, nullptr, FALSE);
}
