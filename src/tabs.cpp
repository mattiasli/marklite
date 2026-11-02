#include "tabs.h"
#include <windowsx.h>
#include <commctrl.h>

namespace {
struct TabsState {
    std::vector<TabItem> items;
    int active = 0;
    int hover = -1;
    bool hoverClose = false;
    bool hoverAdd = false;
    int scroll = 0;
    int dpi = 96;
    HFONT font = nullptr, icons = nullptr;
    std::vector<RECT> rects;
    RECT addRect{};
    int dragIndex = -1;
    int dragStartX = 0;
    bool dragging = false;
    bool tracking = false;
    HWND tip = nullptr;
};

TabsState* St(HWND h) { return (TabsState*)GetWindowLongPtrW(h, GWLP_USERDATA); }

void MakeFonts(HWND h, TabsState* s) {
    s->dpi = Dpi(h);
    if (s->font) DeleteObject(s->font);
    if (s->icons) DeleteObject(s->icons);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, s->dpi);
    ncm.lfMessageFont.lfHeight = -Sc(12, s->dpi);
    s->font = CreateFontIndirectW(&ncm.lfMessageFont);
    s->icons = CreateFontW(-Sc(10, s->dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
}

void Layout(HWND h, TabsState* s, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = Sc(12, s->dpi), closeW = Sc(20, s->dpi);
    int minW = Sc(90, s->dpi), maxW = Sc(220, s->dpi);
    s->rects.resize(s->items.size());
    HGDIOBJ old = SelectObject(dc, s->font);
    int x = Sc(4, s->dpi) - s->scroll;
    int top = Sc(4, s->dpi);
    for (size_t i = 0; i < s->items.size(); i++) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s->items[i].title.c_str(), (int)s->items[i].title.size(), &sz);
        int w = sz.cx + pad * 2 + closeW;
        w = std::max(minW, std::min(maxW, w));
        s->rects[i] = {x, top, x + w, rc.bottom};
        x += w + Sc(1, s->dpi);
    }
    int addW = Sc(32, s->dpi);
    s->addRect = {x + Sc(2, s->dpi), top, x + Sc(2, s->dpi) + addW, rc.bottom};
    SelectObject(dc, old);
}

void EnsureActiveVisible(HWND h, TabsState* s) {
    HDC dc = GetDC(h);
    Layout(h, s, dc);
    RECT rc;
    GetClientRect(h, &rc);
    int avail = rc.right - Sc(40, s->dpi);
    if (s->active >= 0 && s->active < (int)s->rects.size()) {
        RECT r = s->rects[s->active];
        if (r.right > avail) s->scroll += r.right - avail;
        if (r.left < 0) s->scroll += r.left - Sc(4, s->dpi);
    }
    if (s->scroll < 0) s->scroll = 0;
    // shrink scroll if everything fits
    int total = s->rects.empty() ? 0 : s->rects.back().right + s->scroll;
    if (total < avail) s->scroll = 0;
    Layout(h, s, dc);
    ReleaseDC(h, dc);
}

RECT CloseRect(TabsState* s, const RECT& r) {
    int sz = Sc(20, s->dpi);
    int cy = (r.top + r.bottom) / 2;
    return {r.right - sz - Sc(6, s->dpi), cy - sz / 2, r.right - Sc(6, s->dpi), cy + sz / 2};
}

void Paint(HWND h, TabsState* s) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const Palette& p = Pal();
    FillRect(dc, &rc, Theme_Brush(p.chromeBg));
    Layout(h, s, dc);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < s->items.size(); i++) {
        RECT r = s->rects[i];
        if (r.right < 0 || r.left > rc.right) continue;
        bool act = (int)i == s->active, hov = (int)i == s->hover;
        if (act || hov) {
            HBRUSH b = Theme_Brush(act ? p.tabActive : p.tabHover);
            HPEN pen = CreatePen(PS_SOLID, 1, act ? p.border : p.tabHover);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, b);
            int rad = Sc(8, s->dpi);
            RoundRect(dc, r.left, r.top, r.right, r.bottom + rad, rad, rad);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            if (act) {
                RECT ac{r.left + rad / 2, r.top, r.right - rad / 2, r.top + Sc(2, s->dpi)};
                FillRect(dc, &ac, Theme_Brush(p.accent));
            }
        }
        RECT tr = r;
        tr.left += Sc(12, s->dpi);
        tr.right -= Sc(28, s->dpi);
        SelectObject(dc, s->font);
        SetTextColor(dc, act ? p.chromeFg : p.chromeDim);
        DrawTextW(dc, s->items[i].title.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT cr = CloseRect(s, r);
        bool showClose = act || hov;
        if (hov && s->hoverClose) {
            HBRUSH hb = Theme_Brush(Theme_IsDark() ? RGB(0x45, 0x45, 0x45) : RGB(0xD6, 0xD6, 0xD6));
            HGDIOBJ ob = SelectObject(dc, hb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, cr.left, cr.top, cr.right, cr.bottom, Sc(4, s->dpi), Sc(4, s->dpi));
            SelectObject(dc, ob);
            SelectObject(dc, op);
        }
        SelectObject(dc, s->icons);
        if (s->items[i].modified && !(hov && s->hoverClose)) {
            SetTextColor(dc, act ? p.chromeFg : p.chromeDim);
            DrawTextW(dc, L"\xE91F", 1, &cr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);   // filled dot
        } else if (showClose) {
            SetTextColor(dc, p.chromeDim);
            DrawTextW(dc, L"\xE8BB", 1, &cr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);   // close
        }
    }
    // "+" button
    if (s->hoverAdd) {
        HGDIOBJ ob = SelectObject(dc, Theme_Brush(p.tabHover));
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        RECT a = s->addRect;
        InflateRect(&a, -Sc(3, s->dpi), -Sc(3, s->dpi));
        RoundRect(dc, a.left, a.top, a.right, a.bottom, Sc(6, s->dpi), Sc(6, s->dpi));
        SelectObject(dc, ob);
        SelectObject(dc, op);
    }
    SelectObject(dc, s->icons);
    SetTextColor(dc, p.chromeDim);
    DrawTextW(dc, L"\xE710", 1, &s->addRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    // bottom border
    RECT bl{0, rc.bottom - 1, rc.right, rc.bottom};
    FillRect(dc, &bl, Theme_Brush(p.border));
    if (s->active >= 0 && s->active < (int)s->rects.size()) {
        RECT a = s->rects[s->active];
        RECT gap{a.left + 1, rc.bottom - 1, a.right - 1, rc.bottom};
        FillRect(dc, &gap, Theme_Brush(p.tabActive));
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

int HitTab(TabsState* s, POINT pt, bool* onClose) {
    for (size_t i = 0; i < s->rects.size(); i++) {
        if (PtInRect(&s->rects[i], pt)) {
            if (onClose) { RECT cr = CloseRect(s, s->rects[i]); *onClose = PtInRect(&cr, pt) != 0; }
            return (int)i;
        }
    }
    return -1;
}

void Notify(HWND h, int action, LPARAM l) { SendMessageW(GetParent(h), WM_APP_TAB, (WPARAM)action, l); }

LRESULT CALLBACK TabsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    TabsState* s = St(h);
    switch (m) {
    case WM_CREATE: {
        s = new TabsState();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        MakeFonts(h, s);
        return 0;
    }
    case WM_DESTROY:
        if (s) {
            DeleteObject(s->font);
            DeleteObject(s->icons);
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        MakeFonts(h, s);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(h, s); return 0;
    case WM_SIZE: EnsureActiveVisible(h, s); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!s->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            s->tracking = true;
        }
        if (s->dragIndex >= 0 && (w & MK_LBUTTON)) {
            if (!s->dragging && abs(pt.x - s->dragStartX) > Sc(6, s->dpi)) s->dragging = true;
            if (s->dragging) {
                int target = HitTab(s, pt, nullptr);
                if (target >= 0 && target != s->dragIndex) {
                    RECT tr = s->rects[target];
                    int mid = (tr.left + tr.right) / 2;
                    if ((target > s->dragIndex && pt.x > mid) || (target < s->dragIndex && pt.x < mid)) {
                        Notify(h, TAB_MOVE, MAKELPARAM(s->dragIndex, target));
                        s->dragIndex = target;
                    }
                }
            }
        }
        bool onClose = false;
        int hv = HitTab(s, pt, &onClose);
        bool add = PtInRect(&s->addRect, pt) != 0;
        if (hv != s->hover || onClose != s->hoverClose || add != s->hoverAdd) {
            s->hover = hv;
            s->hoverClose = onClose;
            s->hoverAdd = add;
            InvalidateRect(h, nullptr, FALSE);
            if (s->tip) {
                TOOLINFOW ti{sizeof(ti)};
                ti.hwnd = h;
                ti.uId = 1;
                std::wstring tipText = hv >= 0 ? s->items[hv].tooltip : std::wstring();
                ti.lpszText = (LPWSTR)tipText.c_str();
                SendMessageW(s->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            }
        }
        if (s->tip) {
            MSG msg{h, m, w, l};
            SendMessageW(s->tip, TTM_RELAYEVENT, 0, (LPARAM)&msg);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking = false;
        s->hover = -1;
        s->hoverClose = s->hoverAdd = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        bool onClose = false;
        int i = HitTab(s, pt, &onClose);
        if (i >= 0 && !onClose) {
            Notify(h, TAB_SELECT, i);
            s->dragIndex = i;
            s->dragStartX = pt.x;
            s->dragging = false;
            SetCapture(h);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (GetCapture() == h) ReleaseCapture();
        bool wasDrag = s->dragging;
        s->dragIndex = -1;
        s->dragging = false;
        if (wasDrag) return 0;
        bool onClose = false;
        int i = HitTab(s, pt, &onClose);
        if (i >= 0 && onClose) Notify(h, TAB_CLOSE, i);
        else if (PtInRect(&s->addRect, pt)) Notify(h, TAB_NEW, 0);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (HitTab(s, pt, nullptr) < 0 && !PtInRect(&s->addRect, pt)) Notify(h, TAB_NEW, 0);
        return 0;
    }
    case WM_MBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int i = HitTab(s, pt, nullptr);
        if (i >= 0) Notify(h, TAB_CLOSE, i);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int i = HitTab(s, pt, nullptr);
        if (i >= 0) {
            Notify(h, TAB_SELECT, i);
            Notify(h, TAB_CONTEXT, i);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        s->scroll -= d / 2;
        if (s->scroll < 0) s->scroll = 0;
        HDC dc = GetDC(h);
        Layout(h, s, dc);
        ReleaseDC(h, dc);
        RECT rc;
        GetClientRect(h, &rc);
        int total = s->rects.empty() ? 0 : s->addRect.right + s->scroll;
        int maxScroll = std::max(0, total - (int)rc.right + Sc(8, s->dpi));
        if (s->scroll > maxScroll) s->scroll = maxScroll;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
}

HWND Tabs_Create(HWND parent) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = TabsProc;
        wc.hInstance = App_Instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"MarkLiteTabs";
        RegisterClassW(&wc);
        reg = true;
    }
    HWND h = CreateWindowExW(0, L"MarkLiteTabs", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, App_Instance(), nullptr);
    TabsState* s = St(h);
    s->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h, nullptr, App_Instance(), nullptr);
    if (s->tip) {
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.uFlags = 0;
        ti.hwnd = h;
        ti.uId = 1;
        ti.rect = {0, 0, 10000, 10000};
        ti.lpszText = (LPWSTR)L"";
        SendMessageW(s->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(s->tip, TTM_SETMAXTIPWIDTH, 0, 800);
    }
    return h;
}

void Tabs_Set(HWND h, const std::vector<TabItem>& items, int active) {
    TabsState* s = St(h);
    s->items = items;
    s->active = active;
    if (s->hover >= (int)items.size()) s->hover = -1;
    EnsureActiveVisible(h, s);
    InvalidateRect(h, nullptr, FALSE);
}

int Tabs_Height(HWND h) { return Sc(36, Dpi(h)); }

void Tabs_ThemeChanged(HWND h) { InvalidateRect(h, nullptr, FALSE); }
