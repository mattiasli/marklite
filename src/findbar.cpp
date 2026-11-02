#include "findbar.h"
#include <commctrl.h>
#include <windowsx.h>

namespace {
enum { ID_FIND = 100, ID_REPL, ID_CASE, ID_WORD, ID_REGEX, ID_PREV, ID_NEXT, ID_CLOSE, ID_TOGGLE, ID_DOREPL, ID_REPLALL };

struct FindState {
    HWND bar = nullptr, edFind = nullptr, edRepl = nullptr;
    HWND btn[12] = {};
    HWND tip = nullptr;
    Sci* sci = nullptr;
    bool matchCase = false, word = false, regex = false, replace = false, visible = false;
    int dpi = 96;
    HFONT font = nullptr, icons = nullptr;
    std::wstring count;
    bool countError = false;
    int hot = -1;
} F;

int Flags() {
    return (F.matchCase ? SCFIND_MATCHCASE : 0) | (F.word ? SCFIND_WHOLEWORD : 0) | (F.regex ? (SCFIND_REGEXP | SCFIND_POSIX) : 0);
}

std::string Needle() { return WToU8(GetWndText(F.edFind)); }

bool SearchRange(const std::string& n, sptr_t a, sptr_t b, sptr_t& fa, sptr_t& fb) {
    Sci& s = *F.sci;
    s(SCI_SETSEARCHFLAGS, Flags());
    s(SCI_SETTARGETRANGE, a, b);
    sptr_t r = s(SCI_SEARCHINTARGET, n.size(), (sptr_t)n.data());
    if (r < 0) return false;
    fa = s(SCI_GETTARGETSTART);
    fb = s(SCI_GETTARGETEND);
    return true;
}

void Highlight() {
    if (!F.sci) return;
    Sci& s = *F.sci;
    s(SCI_SETINDICATORCURRENT, IND_FIND);
    s(SCI_INDICATORCLEARRANGE, 0, s.Len());
    F.count.clear();
    F.countError = false;
    std::string n = Needle();
    if (!F.visible || n.empty()) { InvalidateRect(F.bar, nullptr, FALSE); return; }
    sptr_t len = s.Len(), pos = 0, a, b;
    sptr_t selStart = s(SCI_GETSELECTIONSTART);
    int total = 0, current = 0;
    DWORD t0 = GetTickCount();
    bool capped = false;
    while (pos <= len && SearchRange(n, pos, len, a, b)) {
        total++;
        if (a <= selStart) current = total;
        if (b > a) s(SCI_INDICATORFILLRANGE, a, b - a);
        pos = b > a ? b : a + 1;
        if (total >= 20000 || GetTickCount() - t0 > 150) { capped = true; break; }
    }
    if (total == 0) { F.count = L"No results"; F.countError = true; }
    else if (capped) F.count = FormatThousands(total) + L"+ matches";
    else F.count = std::to_wstring(current ? current : 1) + L" of " + FormatThousands(total);
    InvalidateRect(F.bar, nullptr, FALSE);
}

// skipEmptyHere: an empty regular-expression match (^, $, x*) right at `pos` is the one already
// found, so step past it - otherwise Find Next / Replace would stay on the same spot forever.
bool FindFrom(sptr_t pos, bool backwards, bool select = true, bool skipEmptyHere = false) {
    std::string n = Needle();
    if (n.empty() || !F.sci) return false;
    Sci& s = *F.sci;
    sptr_t len = s.Len(), a = 0, b = 0;
    auto search = [&](sptr_t from) {
        bool ok = backwards ? SearchRange(n, from, 0, a, b) : SearchRange(n, from, len, a, b);
        if (!ok) ok = backwards ? SearchRange(n, len, from, a, b) : SearchRange(n, 0, from, a, b);   // wrap
        return ok;
    };
    bool ok = search(pos);
    if (ok && skipEmptyHere && a == b && a == pos) {
        sptr_t next = backwards ? s(SCI_POSITIONBEFORE, pos) : s(SCI_POSITIONAFTER, pos);
        if (next == pos) next = backwards ? len : 0;   // at the very start/end: continue from the other end
        ok = search(next);
    }
    if (ok && select) {
        s(SCI_SETSEL, a, b);
        s(SCI_SCROLLRANGE, b, a);
    }
    return ok;
}

void Layout() {
    if (!F.bar) return;
    RECT rc;
    GetClientRect(F.bar, &rc);
    int d = F.dpi;
    int rowH = Sc(36, d), pad = Sc(6, d), h = Sc(26, d), bw = Sc(28, d);
    int x = pad, y = (rowH - h) / 2;
    MoveWindow(F.btn[ID_TOGGLE - ID_FIND], x, y, bw, h, TRUE);
    x += bw + pad;
    int editW = std::max(Sc(160, d), std::min(Sc(340, d), (int)rc.right - Sc(420, d)));
    int ex = x;
    MoveWindow(F.edFind, x + Sc(6, d), y + Sc(4, d), editW - Sc(12, d), h - Sc(8, d), TRUE);
    x += editW + pad;
    for (int id : {ID_CASE, ID_WORD, ID_REGEX}) { MoveWindow(F.btn[id - ID_FIND], x, y, bw, h, TRUE); x += bw + Sc(2, d); }
    x += Sc(100, d);   // count text
    for (int id : {ID_PREV, ID_NEXT}) { MoveWindow(F.btn[id - ID_FIND], x, y, bw, h, TRUE); x += bw + Sc(2, d); }
    MoveWindow(F.btn[ID_CLOSE - ID_FIND], rc.right - bw - pad, y, bw, h, TRUE);
    int y2 = rowH + (rowH - h) / 2 - Sc(4, d);
    ShowWindow(F.edRepl, F.replace ? SW_SHOW : SW_HIDE);
    ShowWindow(F.btn[ID_DOREPL - ID_FIND], F.replace ? SW_SHOW : SW_HIDE);
    ShowWindow(F.btn[ID_REPLALL - ID_FIND], F.replace ? SW_SHOW : SW_HIDE);
    MoveWindow(F.edRepl, ex + Sc(6, d), y2 + Sc(4, d), editW - Sc(12, d), h - Sc(8, d), TRUE);
    int rx = ex + editW + pad;
    MoveWindow(F.btn[ID_DOREPL - ID_FIND], rx, y2, Sc(72, d), h, TRUE);
    MoveWindow(F.btn[ID_REPLALL - ID_FIND], rx + Sc(76, d), y2, Sc(88, d), h, TRUE);
    InvalidateRect(F.bar, nullptr, TRUE);
}

void MakeFonts() {
    F.dpi = Dpi(F.bar);
    if (F.font) DeleteObject(F.font);
    if (F.icons) DeleteObject(F.icons);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, F.dpi);
    ncm.lfMessageFont.lfHeight = -Sc(13, F.dpi);
    F.font = CreateFontIndirectW(&ncm.lfMessageFont);
    F.icons = CreateFontW(-Sc(11, F.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    SendMessageW(F.edFind, WM_SETFONT, (WPARAM)F.font, TRUE);
    SendMessageW(F.edRepl, WM_SETFONT, (WPARAM)F.font, TRUE);
}

void DrawFrame(HDC dc, HWND ed) {
    RECT r;
    GetWindowRect(ed, &r);
    MapWindowPoints(nullptr, F.bar, (POINT*)&r, 2);
    InflateRect(&r, Sc(6, F.dpi), Sc(4, F.dpi));
    bool focus = GetFocus() == ed;
    HPEN pen = CreatePen(PS_SOLID, 1, focus ? Pal().accent : Pal().border);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, Theme_Brush(Pal().inputBg));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, Sc(6, F.dpi), Sc(6, F.dpi));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

void Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(F.bar, &ps);
    RECT rc;
    GetClientRect(F.bar, &rc);
    FillRect(dc, &rc, Theme_Brush(Pal().chromeBg));
    RECT bl{0, rc.bottom - 1, rc.right, rc.bottom};
    FillRect(dc, &bl, Theme_Brush(Pal().border));
    DrawFrame(dc, F.edFind);
    if (F.replace) DrawFrame(dc, F.edRepl);
    // count text
    RECT r;
    GetWindowRect(F.btn[ID_REGEX - ID_FIND], &r);
    MapWindowPoints(nullptr, F.bar, (POINT*)&r, 2);
    RECT tr{r.right + Sc(8, F.dpi), r.top, r.right + Sc(104, F.dpi), r.bottom};
    SelectObject(dc, F.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, F.countError ? Pal().error : Pal().chromeDim);
    DrawTextW(dc, F.count.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    EndPaint(F.bar, &ps);
}

void DrawButton(DRAWITEMSTRUCT* di) {
    int id = (int)di->CtlID;
    const Palette& p = Pal();
    bool on = (id == ID_CASE && F.matchCase) || (id == ID_WORD && F.word) || (id == ID_REGEX && F.regex);
    bool hot = F.hot == id || (di->itemState & ODS_SELECTED);
    RECT r = di->rcItem;
    FillRect(di->hDC, &r, Theme_Brush(p.chromeBg));
    COLORREF bg = on ? (Theme_IsDark() ? RGB(0x1F, 0x3A, 0x5F) : RGB(0xD4, 0xE6, 0xFC)) : hot ? p.btnHover : p.chromeBg;
    bool textBtn = id == ID_DOREPL || id == ID_REPLALL;
    if (textBtn && !hot) bg = p.btnBg;
    HPEN pen = CreatePen(PS_SOLID, 1, on ? p.accent : (textBtn ? p.border : bg));
    HGDIOBJ op = SelectObject(di->hDC, pen), ob = SelectObject(di->hDC, Theme_Brush(bg));
    RoundRect(di->hDC, r.left, r.top, r.right, r.bottom, Sc(6, F.dpi), Sc(6, F.dpi));
    SelectObject(di->hDC, op);
    SelectObject(di->hDC, ob);
    DeleteObject(pen);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, on ? p.accent : p.chromeFg);
    const wchar_t* txt = L"";
    bool icon = true;
    switch (id) {
    case ID_TOGGLE: txt = F.replace ? L"\xE70D" : L"\xE76C"; break;
    case ID_PREV: txt = L"\xE70E"; break;
    case ID_NEXT: txt = L"\xE70D"; break;
    case ID_CLOSE: txt = L"\xE8BB"; break;
    case ID_CASE: txt = L"Aa"; icon = false; break;
    case ID_WORD: txt = L"W"; icon = false; break;
    case ID_REGEX: txt = L".*"; icon = false; break;
    case ID_DOREPL: txt = L"Replace"; icon = false; break;
    case ID_REPLALL: txt = L"Replace all"; icon = false; break;
    }
    SelectObject(di->hDC, icon ? F.icons : F.font);
    DrawTextW(di->hDC, txt, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

void DoReplace(bool all) {
    if (!F.sci) return;
    Sci& s = *F.sci;
    std::string n = Needle(), rep = WToU8(GetWndText(F.edRepl));
    if (n.empty()) return;
    unsigned msg = F.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
    if (all) {
        int count = 0;
        sptr_t pos = 0, a, b;
        s(SCI_BEGINUNDOACTION);
        while (pos <= s.Len() && SearchRange(n, pos, s.Len(), a, b)) {
            sptr_t rl = s(msg, rep.size(), (sptr_t)rep.data());
            count++;
            pos = a + rl;
            if (b == a) pos++;
            if (count > 1000000) break;
        }
        s(SCI_ENDUNDOACTION);
        App_Status(L"Replaced " + FormatThousands(count) + L" occurrence" + (count == 1 ? L"" : L"s"));
    } else {
        sptr_t ss = s(SCI_GETSELECTIONSTART), se = s(SCI_GETSELECTIONEND), a, b;
        bool emptyReplaced = false;
        if (SearchRange(n, ss, se, a, b) && a == ss && b == se) {
            sptr_t rl = s(msg, rep.size(), (sptr_t)rep.data());
            s(SCI_SETSEL, a + rl, a + rl);
            emptyReplaced = a == b;   // like Replace All: don't replace at the same empty spot again
        }
        FindFrom(s(SCI_GETSELECTIONEND), false, true, emptyReplaced);
    }
    Highlight();
}

WNDPROC g_editProc = nullptr;
LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;   // no beep
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(F.bar, nullptr, FALSE);
    return CallWindowProcW(g_editProc, h, m, w, l);
}

LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        int id = GetDlgCtrlID(h);
        if (F.hot != id) {
            F.hot = id;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (m == WM_MOUSELEAVE) {
        F.hot = -1;
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: MakeFonts(); Layout(); return 0;
    case WM_CTLCOLOREDIT: return Theme_CtlColor((HDC)w, true);
    case WM_DRAWITEM: DrawButton((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_FIND && HIWORD(w) == EN_CHANGE) {
            if (F.sci) FindFrom((*F.sci)(SCI_GETSELECTIONSTART), false);
            Highlight();
            return 0;
        }
        switch (id) {
        case ID_CASE: F.matchCase = !F.matchCase; Highlight(); InvalidateRect(F.btn[id - ID_FIND], nullptr, FALSE); break;
        case ID_WORD: F.word = !F.word; Highlight(); InvalidateRect(F.btn[id - ID_FIND], nullptr, FALSE); break;
        case ID_REGEX: F.regex = !F.regex; Highlight(); InvalidateRect(F.btn[id - ID_FIND], nullptr, FALSE); break;
        case ID_PREV: Find_Next(true); break;
        case ID_NEXT: Find_Next(false); break;
        case ID_CLOSE: Find_Hide(); break;
        case ID_TOGGLE: F.replace = !F.replace; PostMessageW(App_Main(), WM_APP_PANEL_CLOSED, 0, 0); Layout(); break;
        case ID_DOREPL: DoReplace(false); break;
        case ID_REPLALL: DoReplace(true); break;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
}

HWND Find_Create(HWND parent, Sci* editor) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = BarProc;
    wc.hInstance = App_Instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MarkLiteFind";
    RegisterClassW(&wc);
    F.sci = editor;
    F.bar = CreateWindowExW(WS_EX_CONTROLPARENT, L"MarkLiteFind", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr, App_Instance(), nullptr);
    F.edFind = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, F.bar, (HMENU)ID_FIND, App_Instance(), nullptr);
    F.edRepl = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, F.bar, (HMENU)ID_REPL, App_Instance(), nullptr);
    SendMessageW(F.edFind, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find");
    SendMessageW(F.edRepl, EM_SETCUEBANNER, TRUE, (LPARAM)L"Replace");
    g_editProc = (WNDPROC)SetWindowLongPtrW(F.edFind, GWLP_WNDPROC, (LONG_PTR)EditSub);
    SetWindowLongPtrW(F.edRepl, GWLP_WNDPROC, (LONG_PTR)EditSub);
    F.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, F.bar, nullptr, App_Instance(), nullptr);
    struct { int id; const wchar_t* tip; } btns[] = {
        {ID_TOGGLE, L"Toggle replace (Ctrl+H)"}, {ID_CASE, L"Match case"}, {ID_WORD, L"Whole word"},
        {ID_REGEX, L"Regular expression"}, {ID_PREV, L"Previous match (Shift+F3)"}, {ID_NEXT, L"Next match (F3)"},
        {ID_CLOSE, L"Close (Esc)"}, {ID_DOREPL, L"Replace (Enter)"}, {ID_REPLALL, L"Replace all (Ctrl+Alt+Enter)"},
    };
    for (auto& b : btns) {
        HWND hb = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, F.bar, (HMENU)(INT_PTR)b.id, App_Instance(), nullptr);
        F.btn[b.id - ID_FIND] = hb;
        SetWindowSubclass(hb, BtnSub, 1, 0);
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = F.bar;
        ti.uId = (UINT_PTR)hb;
        ti.lpszText = (LPWSTR)b.tip;
        SendMessageW(F.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    MakeFonts();
    Find_ThemeChanged();
    return F.bar;
}

void Find_ThemeChanged() {
    Theme_Edit(F.edFind);
    Theme_Edit(F.edRepl);
    InvalidateRect(F.bar, nullptr, TRUE);
    RedrawWindow(F.bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool Find_Visible() { return F.visible; }
int Find_Height() { return F.visible ? Sc(F.replace ? 68 : 36, F.dpi) : 0; }

void Find_Show(bool replace) {
    if (!F.sci) return;
    Sci& s = *F.sci;
    std::string sel = s.SelText();
    if (!sel.empty() && sel.find('\n') == std::string::npos && sel.size() < 200) {
        SetWindowTextW(F.edFind, U8ToW(sel).c_str());
    }
    bool wasVisible = F.visible;
    F.visible = true;
    F.replace = replace || (wasVisible && F.replace);
    ShowWindow(F.bar, SW_SHOW);
    App_Relayout();
    Layout();
    SetFocus(F.edFind);
    SendMessageW(F.edFind, EM_SETSEL, 0, -1);
    Highlight();
}

void Find_Hide() {
    if (!F.visible) return;
    F.visible = false;
    ShowWindow(F.bar, SW_HIDE);
    Highlight();
    App_Relayout();
    if (F.sci) SetFocus(F.sci->hwnd);
}

void Find_Next(bool backwards) {
    if (!F.sci) return;
    if (GetWindowTextLengthW(F.edFind) == 0) { Find_Show(false); return; }
    Sci& s = *F.sci;
    sptr_t pos = backwards ? s(SCI_GETSELECTIONSTART) : s(SCI_GETSELECTIONEND);
    bool caretOnly = s(SCI_GETSELECTIONSTART) == s(SCI_GETSELECTIONEND);
    if (!FindFrom(pos, backwards, true, caretOnly)) {
        App_Status(L"No matches", true);
        MessageBeep(MB_OK);
    }
    Highlight();
}

void Find_Refresh() { if (F.visible) Highlight(); }

bool Find_PreTranslate(MSG* msg) {
    if (!F.visible || msg->message != WM_KEYDOWN) return false;
    HWND f = GetFocus();
    if (f != F.edFind && f != F.edRepl) return false;
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    switch (msg->wParam) {
    case VK_RETURN:
        if (f == F.edRepl) DoReplace(ctrl && alt);
        else Find_Next(shift);
        return true;
    case VK_ESCAPE: Find_Hide(); return true;
    case VK_TAB:
        if (F.replace) SetFocus(f == F.edFind ? F.edRepl : F.edFind);
        return true;
    case VK_F3: Find_Next(shift); return true;
    }
    return false;
}
