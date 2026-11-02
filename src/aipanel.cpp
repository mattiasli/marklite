#include "aipanel.h"
#include "ai.h"
#include <commctrl.h>
#include <windowsx.h>
#include <set>
#include <memory>

namespace {
enum { ID_INSTR = 200, ID_MODEL, ID_CTX, ID_GEN, ID_ACCEPT, ID_DISCARD, ID_ORIG, ID_NEW };
enum State { ST_IDLE, ST_RUNNING, ST_DONE, ST_ERROR };

struct Panel {
    HWND wnd = nullptr, instr = nullptr, instrEdit = nullptr, model = nullptr;
    HWND btnCtx = nullptr, btnGen = nullptr, btnAccept = nullptr, btnDiscard = nullptr;
    Sci orig, sug;
    HFONT font = nullptr, fontBold = nullptr;
    int dpi = 96;
    int hot = -1;
    bool visible = false;
    bool fetchedModels = false;
    bool dragging = false;
    int dragY = 0, dragH = 0;
    // session
    bool active = false;
    int docId = -1;
    std::string original, lead, trail;
    bool ctx = true;
    State state = ST_IDLE;
    int jobId = 0;
    std::string result;
    std::wstring modelUsed, status;
    bool statusErr = false;
    DWORD startTick = 0;
    std::set<int> staleDocs;
} P;

const int kTopGrip = 6;

void SetStatus(const std::wstring& s, bool err = false) {
    P.status = s;
    P.statusErr = err;
    InvalidateRect(P.wnd, nullptr, FALSE);
}

void SetSciText(Sci& s, const std::string& t, bool readOnly) {
    s(SCI_SETREADONLY, 0);
    s(SCI_SETTEXT, 0, (sptr_t)t.c_str());
    s(SCI_EMPTYUNDOBUFFER);
    s(SCI_SETREADONLY, readOnly);
    s(SCI_GOTOPOS, 0);
}

void UpdateButtons() {
    InvalidateRect(P.btnGen, nullptr, FALSE);
    InvalidateRect(P.btnAccept, nullptr, FALSE);
    InvalidateRect(P.btnCtx, nullptr, FALSE);
    EnableWindow(P.btnAccept, P.state == ST_DONE);
}

void FreeModelData();
void FillModels() {
    std::wstring cur = g_set.model;
    FreeModelData();
    SendMessageW(P.model, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    auto add = [&](const std::wstring& id, const std::wstring& name) {
        std::wstring label = name.empty() ? id : name;
        int i = (int)SendMessageW(P.model, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(P.model, CB_SETITEMDATA, i, (LPARAM) new std::wstring(id));
        if (id == cur) sel = i;
    };
    for (auto& m : g_set.models) add(m.id, m.name);
    if (sel < 0 && !cur.empty()) { add(cur, ModelDisplayName(cur)); }
    SendMessageW(P.model, CB_SETCURSEL, sel < 0 ? (int)SendMessageW(P.model, CB_GETCOUNT, 0, 0) - 1 : sel, 0);
}

std::wstring SelectedModel() {
    int i = (int)SendMessageW(P.model, CB_GETCURSEL, 0, 0);
    if (i < 0) return g_set.model;
    auto* id = (std::wstring*)SendMessageW(P.model, CB_GETITEMDATA, i, 0);
    return (id && id != (void*)CB_ERR) ? *id : g_set.model;
}

void FreeModelData() {
    int n = (int)SendMessageW(P.model, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        auto* id = (std::wstring*)SendMessageW(P.model, CB_GETITEMDATA, i, 0);
        if (id && id != (void*)CB_ERR) delete id;
    }
}

void FillHistory() {
    std::wstring cur = GetWndText(P.instr);
    SendMessageW(P.instr, CB_RESETCONTENT, 0, 0);
    for (auto& h : g_set.instrHistory) SendMessageW(P.instr, CB_ADDSTRING, 0, (LPARAM)h.c_str());
    SetWindowTextW(P.instr, cur.c_str());
}

void MakeFonts() {
    P.dpi = Dpi(P.wnd);
    if (P.font) DeleteObject(P.font);
    if (P.fontBold) DeleteObject(P.fontBold);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, P.dpi);
    ncm.lfMessageFont.lfHeight = -Sc(13, P.dpi);
    P.font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    P.fontBold = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {P.instr, P.model}) SendMessageW(h, WM_SETFONT, (WPARAM)P.font, TRUE);
}

void Layout() {
    RECT rc;
    GetClientRect(P.wnd, &rc);
    int d = P.dpi, W = rc.right, H = rc.bottom;
    int m = Sc(12, d), gap = Sc(8, d);
    int rowTop = Sc(kTopGrip + 8, d), bh = Sc(30, d);
    int genW = Sc(96, d), ctxW = Sc(212, d), modelW = Sc(200, d);
    int x = W - m - genW;
    MoveWindow(P.btnGen, x, rowTop, genW, bh, TRUE);
    x -= gap + ctxW;
    MoveWindow(P.btnCtx, x, rowTop, ctxW, bh, TRUE);
    x -= gap + modelW;
    RECT cr;
    GetWindowRect(P.model, &cr);
    int ch = cr.bottom - cr.top;
    MoveWindow(P.model, x, rowTop + (bh - ch) / 2, modelW, Sc(300, d), TRUE);
    int instrW = x - gap - m;
    GetWindowRect(P.instr, &cr);
    ch = cr.bottom - cr.top;
    MoveWindow(P.instr, m, rowTop + (bh - ch) / 2, std::max(Sc(120, d), instrW), Sc(260, d), TRUE);

    int capTop = rowTop + bh + Sc(10, d), capH = Sc(20, d);
    int viewTop = capTop + capH, bottomH = Sc(46, d);
    int viewH = std::max(Sc(40, d), H - viewTop - bottomH);
    int half = (W - 2 * m - gap) / 2;
    MoveWindow(P.orig.hwnd, m + 1, viewTop + 1, half - 2, viewH - 2, TRUE);
    MoveWindow(P.sug.hwnd, m + half + gap + 1, viewTop + 1, half - 2, viewH - 2, TRUE);
    int by = H - bottomH + Sc(8, d);
    int aw = Sc(96, d), dw = Sc(96, d);
    MoveWindow(P.btnDiscard, W - m - dw, by, dw, bh, TRUE);
    MoveWindow(P.btnAccept, W - m - dw - gap - aw, by, aw, bh, TRUE);
    InvalidateRect(P.wnd, nullptr, FALSE);
}

void FrameRect2(HDC dc, RECT r, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

void Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(P.wnd, &ps);
    RECT rc;
    GetClientRect(P.wnd, &rc);
    const Palette& p = Pal();
    int d = P.dpi;
    FillRect(dc, &rc, Theme_Brush(p.chromeBg));
    RECT top{0, 0, rc.right, 1};
    FillRect(dc, &top, Theme_Brush(p.border));
    // grip
    RECT grip{rc.right / 2 - Sc(18, d), Sc(2, d), rc.right / 2 + Sc(18, d), Sc(4, d)};
    FillRect(dc, &grip, Theme_Brush(p.border));

    SetBkMode(dc, TRANSPARENT);
    RECT ro, rn;
    GetWindowRect(P.orig.hwnd, &ro);
    MapWindowPoints(nullptr, P.wnd, (POINT*)&ro, 2);
    GetWindowRect(P.sug.hwnd, &rn);
    MapWindowPoints(nullptr, P.wnd, (POINT*)&rn, 2);
    InflateRect(&ro, 1, 1);
    InflateRect(&rn, 1, 1);
    FrameRect2(dc, ro, p.border);
    FrameRect2(dc, rn, P.state == ST_DONE ? p.aiSel : p.border);
    SelectObject(dc, P.fontBold);
    SetTextColor(dc, p.chromeDim);
    RECT c1{ro.left, ro.top - Sc(20, d), ro.right, ro.top - Sc(2, d)};
    DrawTextW(dc, L"Original", -1, &c1, DT_SINGLELINE | DT_BOTTOM | DT_LEFT);
    RECT c2{rn.left, rn.top - Sc(20, d), rn.right, rn.top - Sc(2, d)};
    std::wstring cap = L"Suggestion";
    if (!P.modelUsed.empty()) cap += L"  \x2022  " + ModelDisplayName(P.modelUsed);
    if (P.state == ST_DONE) cap += L"  (editable)";
    DrawTextW(dc, cap.c_str(), -1, &c2, DT_SINGLELINE | DT_BOTTOM | DT_LEFT | DT_END_ELLIPSIS);

    // status line
    RECT ra;
    GetWindowRect(P.btnAccept, &ra);
    MapWindowPoints(nullptr, P.wnd, (POINT*)&ra, 2);
    RECT st{Sc(12, d), ra.top, ra.left - Sc(12, d), ra.bottom};
    SelectObject(dc, P.font);
    SetTextColor(dc, P.statusErr ? p.error : p.chromeDim);
    std::wstring s = P.status;
    if (P.state == ST_RUNNING) {
        DWORD secs = (GetTickCount() - P.startTick) / 1000;
        s += L"  " + std::to_wstring(secs) + L"s";
    }
    DrawTextW(dc, s.c_str(), -1, &st, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    EndPaint(P.wnd, &ps);
}

void DrawButton(DRAWITEMSTRUCT* di) {
    const Palette& p = Pal();
    int id = (int)di->CtlID;
    RECT r = di->rcItem;
    FillRect(di->hDC, &r, Theme_Brush(p.chromeBg));
    bool disabled = di->itemState & ODS_DISABLED;
    bool hot = (P.hot == id || (di->itemState & ODS_SELECTED)) && !disabled;
    bool primary = (id == ID_GEN && P.state != ST_RUNNING && P.state != ST_DONE) || (id == ID_ACCEPT && P.state == ST_DONE);
    COLORREF bg = primary ? p.btnPrimary : (hot ? p.btnHover : p.btnBg);
    if (primary && hot) bg = RGB(std::min(255, GetRValue(bg) + 20), std::min(255, GetGValue(bg) + 20), std::min(255, GetBValue(bg) + 20));
    COLORREF fg = disabled ? p.chromeDim : primary ? p.btnPrimaryFg : p.chromeFg;
    if (id == ID_CTX) bg = hot ? p.btnHover : p.chromeBg;
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? bg : p.border);
    HGDIOBJ op = SelectObject(di->hDC, pen), ob = SelectObject(di->hDC, Theme_Brush(bg));
    if (id != ID_CTX || hot) RoundRect(di->hDC, r.left, r.top, r.right, r.bottom, Sc(6, P.dpi), Sc(6, P.dpi));
    SelectObject(di->hDC, op);
    SelectObject(di->hDC, ob);
    DeleteObject(pen);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, fg);
    SelectObject(di->hDC, primary ? P.fontBold : P.font);
    std::wstring t;
    switch (id) {
    case ID_GEN: t = P.state == ST_RUNNING ? L"Stop" : (P.state == ST_IDLE ? L"Generate" : L"Retry"); break;
    case ID_ACCEPT: t = L"Accept"; break;
    case ID_DISCARD: t = L"Discard"; break;
    case ID_CTX: {
        // checkbox glyph + label
        int bx = r.left + Sc(6, P.dpi), sz = Sc(14, P.dpi), cy = (r.top + r.bottom) / 2;
        RECT box{bx, cy - sz / 2, bx + sz, cy + sz / 2};
        HPEN bp = CreatePen(PS_SOLID, 1, P.ctx ? p.accent : p.chromeDim);
        HGDIOBJ op2 = SelectObject(di->hDC, bp), ob2 = SelectObject(di->hDC, Theme_Brush(P.ctx ? p.accent : p.inputBg));
        RoundRect(di->hDC, box.left, box.top, box.right, box.bottom, Sc(3, P.dpi), Sc(3, P.dpi));
        SelectObject(di->hDC, op2);
        SelectObject(di->hDC, ob2);
        DeleteObject(bp);
        if (P.ctx) {
            HPEN cp = CreatePen(PS_SOLID, Sc(2, P.dpi), RGB(255, 255, 255));
            HGDIOBJ o3 = SelectObject(di->hDC, cp);
            MoveToEx(di->hDC, box.left + sz * 22 / 100, box.top + sz * 52 / 100, nullptr);
            LineTo(di->hDC, box.left + sz * 42 / 100, box.top + sz * 72 / 100);
            LineTo(di->hDC, box.left + sz * 78 / 100, box.top + sz * 30 / 100);
            SelectObject(di->hDC, o3);
            DeleteObject(cp);
        }
        RECT tr = r;
        tr.left = box.right + Sc(8, P.dpi);
        SetTextColor(di->hDC, p.chromeFg);
        SelectObject(di->hDC, P.font);
        DrawTextW(di->hDC, L"Whole document as context", -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }
    }
    DrawTextW(di->hDC, t.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// Range of the AI selection indicator in the editor.
bool IndicatorRange(Sci& ed, sptr_t& a, sptr_t& b) {
    sptr_t len = ed.Len();
    if (len == 0) return false;
    a = ed(SCI_INDICATORVALUEAT, IND_AISEL, 0) ? 0 : ed(SCI_INDICATOREND, IND_AISEL, 0);
    if (a >= len || !ed(SCI_INDICATORVALUEAT, IND_AISEL, a)) return false;
    b = ed(SCI_INDICATOREND, IND_AISEL, a);
    return b > a;
}

void ClearIndicator() {
    Sci& ed = App_Editor();
    if (App_ActiveDocId() == P.docId) {
        ed(SCI_SETINDICATORCURRENT, IND_AISEL);
        ed(SCI_INDICATORCLEARRANGE, 0, ed.Len());
    } else if (P.docId >= 0) {
        P.staleDocs.insert(P.docId);
    }
}

void EndSession() {
    if (P.jobId) Ai_Cancel(P.jobId);
    P.jobId = 0;
    if (P.active) ClearIndicator();
    P.active = false;
    P.docId = -1;
    P.state = ST_IDLE;
    KillTimer(P.wnd, 1);
}

void Close() {
    EndSession();
    P.visible = false;
    ShowWindow(P.wnd, SW_HIDE);
    App_Relayout();
    SetFocus(App_Editor().hwnd);
}

std::string TrimWs(const std::string& s, std::string* lead, std::string* trail) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    if (lead) *lead = s.substr(0, a);
    if (trail) *trail = s.substr(b);
    return s.substr(a, b - a);
}

// Text with its line endings changed to those of the document being edited.
std::string ToDocEol(const std::string& t) {
    Sci& ed = App_Editor();
    int mode = (int)ed(SCI_GETEOLMODE);
    std::string eol = mode == SC_EOL_CRLF ? "\r\n" : mode == SC_EOL_CR ? "\r" : "\n";
    std::string out;
    out.reserve(t.size() + 64);
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\r') { if (i + 1 < t.size() && t[i + 1] == '\n') i++; out += eol; }
        else if (t[i] == '\n') out += eol;
        else out += t[i];
    }
    return out;
}

// A reply that is nothing but one fenced code block (```lang ... ```) is unwrapped, unless the
// original was a code block itself. Anything else - text around the block, or several blocks - is
// kept as it is, so no part of the reply is lost.
std::string CleanResult(const std::string& r) {
    std::string t = TrimWs(r, nullptr, nullptr);
    if (t.rfind("```", 0) == 0 && P.original.rfind("```", 0) != 0) {
        std::vector<size_t> fences;   // starts of lines that begin with ```
        for (size_t i = 0; i < t.size();) {
            size_t e = t.find('\n', i);
            if (e == std::string::npos) e = t.size();
            size_t a = i;
            while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
            if (t.compare(a, 3, "```") == 0) fences.push_back(i);
            i = e + 1;
        }
        size_t nl = t.find('\n');
        if (fences.size() == 2 && nl != std::string::npos && fences[1] > nl &&
            TrimWs(t.substr(fences[1]), nullptr, nullptr) == "```")
            t = TrimWs(t.substr(nl + 1, fences[1] - nl - 1), nullptr, nullptr);
    }
    return t;
}

void Generate() {
    if (P.state == ST_RUNNING) {   // Stop
        Ai_Cancel(P.jobId);
        P.jobId = 0;
        P.state = P.result.empty() ? ST_IDLE : ST_ERROR;
        SetStatus(L"Stopped.");
        P.sug(SCI_SETREADONLY, 0);
        KillTimer(P.wnd, 1);
        UpdateButtons();
        return;
    }
    if (!App_EnsureApiKey()) return;
    Sci& ed = App_Editor();
    sptr_t a, b;
    if (!IndicatorRange(ed, a, b)) { SetStatus(L"The selected text no longer exists in the document.", true); return; }

    std::wstring instr = Trim(GetWndText(P.instr));
    if (!instr.empty()) {
        auto& h = g_set.instrHistory;
        for (size_t i = 0; i < h.size(); i++) if (h[i] == instr) { h.erase(h.begin() + i); break; }
        h.insert(h.begin(), instr);
        if (h.size() > 25) h.resize(25);
        FillHistory();
    }
    std::wstring model = SelectedModel();
    g_set.model = model;

    AiRequest req;
    req.apiKey = g_set.apiKey;
    req.model = model;
    req.effort = g_set.effort;
    // The highlighted text may have been edited since the panel opened: what is sent now is what
    // Accept later compares against and what the Original pane shows.
    std::string nowText = ed.Range(a, b);
    std::string core = TrimWs(nowText, &P.lead, &P.trail);
    if (core != P.original) {
        P.original = core;
        SetSciText(P.orig, core, true);
    }
    req.selection = core;
    req.instruction = WToU8(instr);
    if (P.ctx) req.document = ed.Text();
    // short context snippets, snapped to UTF-8 boundaries
    auto snap = [&](sptr_t pos) {
        while (pos > 0 && pos < ed.Len() && (((unsigned char)ed(SCI_GETCHARAT, pos)) & 0xC0) == 0x80) pos--;
        return pos;
    };
    sptr_t ba = snap(std::max<sptr_t>(0, a - 600)), ab = snap(std::min<sptr_t>(ed.Len(), b + 600));
    req.before = ed.Range(ba, a);
    req.after = ed.Range(b, ab);
    if (req.selection.empty()) { SetStatus(L"Nothing to rewrite - the selection is empty.", true); return; }

    P.result.clear();
    P.modelUsed.clear();
    SetSciText(P.sug, "", true);
    P.state = ST_RUNNING;
    P.startTick = GetTickCount();
    SetStatus(L"Sending to " + ModelDisplayName(model) + L"...");
    P.jobId = Ai_Start(P.wnd, req);
    SetTimer(P.wnd, 1, 1000, nullptr);
    UpdateButtons();
}

void Accept() {
    if (P.state != ST_DONE) return;
    Sci& ed = App_Editor();
    sptr_t a, b;
    if (!IndicatorRange(ed, a, b)) {
        MessageBoxW(App_Main(), L"The original text was removed from the document, so the suggestion can't be inserted.\n\nYou can still copy it from the suggestion box.", APP_NAME, MB_ICONWARNING);
        return;
    }
    std::string now = ed.Range(a, b);
    if (now != P.lead + P.original + P.trail && TrimWs(now, nullptr, nullptr) != P.original) {
        if (MessageBoxW(App_Main(), L"The original text has been edited since the request was made.\n\nReplace it with the suggestion anyway?", APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES)
            return;
    }
    std::string lead, trail;
    TrimWs(now, &lead, &trail);
    std::string text = ToDocEol(P.sug.Text());
    std::string repl = lead + text + trail;
    ed(SCI_BEGINUNDOACTION);
    ed(SCI_SETTARGETRANGE, a, b);
    ed(SCI_REPLACETARGET, repl.size(), (sptr_t)repl.data());
    ed(SCI_ENDUNDOACTION);
    sptr_t ns = a + (sptr_t)lead.size(), ne = ns + (sptr_t)text.size();
    Close();
    ed(SCI_SETSEL, ns, ne);
    ed(SCI_SCROLLRANGE, ne, ns);
    App_Status(L"Suggestion inserted (Ctrl+Z to undo)");
}

void OnAi(int jobId, AiEvent* e) {
    std::unique_ptr<AiEvent> ev(e);
    if (jobId != P.jobId) return;
    switch (e->type) {
    case AI_STARTED:
        P.modelUsed = e->info;
        SetStatus(L"Claude is working...");
        break;
    case AI_THINKING: SetStatus(L"Thinking..."); break;
    case AI_DELTA:
        P.result += e->text;
        P.sug(SCI_SETREADONLY, 0);
        P.sug(SCI_APPENDTEXT, e->text.size(), (sptr_t)e->text.data());
        P.sug(SCI_SETREADONLY, 1);
        P.sug(SCI_DOCUMENTEND);
        if (P.status != L"Writing...") SetStatus(L"Writing...");
        break;
    case AI_FALLBACK:
        P.modelUsed = e->info;
        SetStatus(L"Continuing on fallback model " + ModelDisplayName(e->info) + L"...");
        break;
    case AI_DONE: {
        P.jobId = 0;
        KillTimer(P.wnd, 1);
        if (e->info == L"refusal") {
            P.state = ST_ERROR;
            SetStatus(L"Claude declined to rewrite this text (refusal). Try rephrasing the instruction.", true);
            break;
        }
        std::string clean = ToDocEol(CleanResult(P.result));
        P.sug(SCI_SETEOLMODE, App_Editor()(SCI_GETEOLMODE));   // Enter in the box: the document's line ends
        SetSciText(P.sug, clean, false);
        if (clean.empty()) { P.state = ST_ERROR; SetStatus(L"Claude returned an empty response.", true); break; }
        P.state = ST_DONE;
        double secs = (GetTickCount() - P.startTick) / 1000.0;
        wchar_t sb[32];
        swprintf(sb, 32, L"%.1fs", secs);
        std::wstring st = L"Done in " + std::wstring(sb) + L"  \x2022  " + FormatThousands(e->inTokens + e->cacheRead) + L" in / " +
                          FormatThousands(e->outTokens) + L" out tokens";
        if (e->cacheRead) st += L" (" + FormatThousands(e->cacheRead) + L" cached)";
        st += L"  \x2022  Ctrl+Enter to accept";
        if (e->info == L"max_tokens") { st = L"The response was cut off at the token limit. " + st; SetStatus(st, true); }
        else SetStatus(st);
        break;
    }
    case AI_ERROR:
        P.jobId = 0;
        KillTimer(P.wnd, 1);
        P.state = ST_ERROR;
        P.sug(SCI_SETREADONLY, 0);
        SetStatus(e->info, true);
        break;
    }
    UpdateButtons();
}

WNDPROC g_comboEditProc = nullptr;
LRESULT CALLBACK ComboEditSub(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == '\n')) return 0;
    return CallWindowProcW(g_comboEditProc, h, m, w, l);
}

LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        int id = GetDlgCtrlID(h);
        if (P.hot != id) {
            P.hot = id;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (m == WM_MOUSELEAVE) {
        P.hot = -1;
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, m, w, l);
}

void ConfigureViews() {
    for (Sci* s : {&P.orig, &P.sug}) {
        Md_ConfigureView(*s, false);
        (*s)(SCI_SETZOOM, -1);
        (*s)(SCI_SETMARGINLEFT, 0, Sc(6, P.dpi));
        (*s)(SCI_SETMARGINRIGHT, 0, Sc(6, P.dpi));
        (*s)(SCI_SETMARGINWIDTHN, 0, 0);
        (*s)(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    }
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: MakeFonts(); ConfigureViews(); Layout(); return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: return Theme_CtlColor((HDC)w, true);
    case WM_DRAWITEM: DrawButton((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_NOTIFY: {
        auto* n = (SCNotification*)l;
        if (n->nmhdr.code == SCN_STYLENEEDED) {
            if (n->nmhdr.hwndFrom == P.orig.hwnd) Md_StyleNeeded(P.orig, n->position);
            else if (n->nmhdr.hwndFrom == P.sug.hwnd) Md_StyleNeeded(P.sug, n->position);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_GEN: Generate(); break;
        case ID_ACCEPT: Accept(); break;
        case ID_DISCARD: Close(); break;
        case ID_CTX: P.ctx = !P.ctx; g_set.aiContext = P.ctx; InvalidateRect(P.btnCtx, nullptr, FALSE); break;
        case ID_MODEL:
            if (HIWORD(w) == CBN_SELCHANGE) g_set.model = SelectedModel();
            break;
        }
        return 0;
    case WM_APP_AI: OnAi((int)w, (AiEvent*)l); return 0;
    case WM_APP_MODELS: {
        std::unique_ptr<ModelList> ml((ModelList*)l);
        if (ml->ok && !ml->models.empty()) {
            g_set.models = ml->models;
            FillModels();
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        if (pt.y < Sc(kTopGrip, P.dpi)) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
        break;
    }
    case WM_LBUTTONDOWN:
        if (GET_Y_LPARAM(l) < Sc(kTopGrip, P.dpi)) {
            P.dragging = true;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            P.dragY = pt.y;
            RECT rc;
            GetClientRect(h, &rc);
            P.dragH = rc.bottom;
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (P.dragging) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            int nh = P.dragH + (P.dragY - pt.y);
            nh = std::max(Sc(200, P.dpi), nh);
            g_set.aiPanelH = MulDiv(nh, 96, P.dpi);
            App_Relayout();
        }
        return 0;
    case WM_CAPTURECHANGED:   // capture lost mid-drag (Alt+Tab, Win key): stop resizing
        if ((HWND)l != h) P.dragging = false;
        return 0;
    case WM_LBUTTONUP:
        if (P.dragging) { P.dragging = false; ReleaseCapture(); }
        return 0;
    case WM_DESTROY: FreeModelData(); SendMessageW(P.model, CB_RESETCONTENT, 0, 0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND MakeButton(int id) {
    HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, P.wnd, (HMENU)(INT_PTR)id, App_Instance(), nullptr);
    SetWindowSubclass(b, BtnSub, 1, 0);
    return b;
}
}

HWND AiPanel_Create(HWND parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = App_Instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MarkLiteAiPanel";
    RegisterClassW(&wc);
    P.wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"MarkLiteAiPanel", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr, App_Instance(), nullptr);
    P.instr = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 10, 200, P.wnd, (HMENU)ID_INSTR, App_Instance(), nullptr);
    P.model = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, 200, P.wnd, (HMENU)ID_MODEL, App_Instance(), nullptr);
    COMBOBOXINFO ci{sizeof(ci)};
    if (GetComboBoxInfo(P.instr, &ci)) {
        P.instrEdit = ci.hwndItem;
        g_comboEditProc = (WNDPROC)SetWindowLongPtrW(P.instrEdit, GWLP_WNDPROC, (LONG_PTR)ComboEditSub);
    }
    SendMessageW(P.instr, CB_SETCUEBANNER, 0, (LPARAM)L"Instruction (optional), e.g. \"make this shorter\" or \"more formal\"");
    P.btnCtx = MakeButton(ID_CTX);
    P.btnGen = MakeButton(ID_GEN);
    P.btnAccept = MakeButton(ID_ACCEPT);
    P.btnDiscard = MakeButton(ID_DISCARD);
    HWND o = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, P.wnd, (HMENU)ID_ORIG, App_Instance(), nullptr);
    HWND n = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, P.wnd, (HMENU)ID_NEW, App_Instance(), nullptr);
    P.orig.Attach(o);
    P.sug.Attach(n);
    MakeFonts();
    ConfigureViews();
    AiPanel_ThemeChanged();
    FillModels();
    FillHistory();
    return P.wnd;
}

void AiPanel_ThemeChanged() {
    Theme_Edit(P.instr);
    Theme_Edit(P.model);
    ConfigureViews();
    RedrawWindow(P.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void AiPanel_FontChanged() { ConfigureViews(); }

bool AiPanel_Visible() { return P.visible; }
int AiPanel_Height() { return P.visible ? Sc(g_set.aiPanelH, Dpi(P.wnd)) : 0; }

void AiPanel_Begin() {
    if (!App_EnsureApiKey()) return;
    Sci& ed = App_Editor();
    sptr_t a = ed(SCI_GETSELECTIONSTART), b = ed(SCI_GETSELECTIONEND);
    if (a == b) {
        // no selection: use the paragraph around the caret
        sptr_t line = ed(SCI_LINEFROMPOSITION, a), n = ed(SCI_GETLINECOUNT);
        auto blank = [&](sptr_t l) { std::string t = ed.LineText(l); return t.find_first_not_of(" \t") == std::string::npos; };
        if (blank(line)) { App_Status(L"Select the text you want Claude to rewrite (Ctrl+K)", true); return; }
        sptr_t l0 = line, l1 = line;
        while (l0 > 0 && !blank(l0 - 1)) l0--;
        while (l1 + 1 < n && !blank(l1 + 1)) l1++;
        a = ed(SCI_POSITIONFROMLINE, l0);
        b = ed(SCI_GETLINEENDPOSITION, l1);
        ed(SCI_SETSEL, a, b);
    }
    std::string text = ed.Range(a, b);
    std::string core = TrimWs(text, &P.lead, &P.trail);
    if (core.empty()) { App_Status(L"Select some text first", true); return; }
    if (core.size() > 400000) { App_Status(L"The selection is too large to rewrite in one go", true); return; }

    EndSession();
    P.active = true;
    P.docId = App_ActiveDocId();
    P.original = core;
    P.ctx = g_set.aiContext;
    P.state = ST_IDLE;
    P.result.clear();
    P.modelUsed.clear();
    ed(SCI_SETINDICATORCURRENT, IND_AISEL);
    ed(SCI_INDICATORCLEARRANGE, 0, ed.Len());
    ed(SCI_INDICATORFILLRANGE, a, b - a);

    SetSciText(P.orig, core, true);
    SetSciText(P.sug, "", true);
    FillModels();
    if (!P.fetchedModels && !g_set.apiKey.empty()) {
        P.fetchedModels = true;
        Ai_FetchModels(P.wnd, g_set.apiKey);
    }
    SetStatus(L"Type an instruction (or leave it empty) and press Enter.  Esc closes.");
    P.visible = true;
    ShowWindow(P.wnd, SW_SHOW);
    App_Relayout();
    UpdateButtons();
    SetFocus(P.instrEdit ? P.instrEdit : P.instr);
    SendMessageW(P.instrEdit, EM_SETSEL, 0, -1);
}

void AiPanel_OnDocSwitch() {
    int id = App_ActiveDocId();
    if (P.staleDocs.count(id) && id != P.docId) {
        Sci& ed = App_Editor();
        ed(SCI_SETINDICATORCURRENT, IND_AISEL);
        ed(SCI_INDICATORCLEARRANGE, 0, ed.Len());
        P.staleDocs.erase(id);
    }
    bool show = P.active && P.docId == id;
    if (show != P.visible) {
        P.visible = show;
        ShowWindow(P.wnd, show ? SW_SHOW : SW_HIDE);
        App_Relayout();
    }
}

void AiPanel_OnDocClosed(int docId) {
    P.staleDocs.erase(docId);
    if (P.active && P.docId == docId) {
        if (P.jobId) Ai_Cancel(P.jobId);
        P.jobId = 0;
        P.active = false;
        P.docId = -1;
        P.state = ST_IDLE;
        KillTimer(P.wnd, 1);
        if (P.visible) {
            P.visible = false;
            ShowWindow(P.wnd, SW_HIDE);
            App_Relayout();
        }
    }
}

bool AiPanel_PreTranslate(MSG* msg) {
    if (!P.visible || msg->message != WM_KEYDOWN) return false;
    HWND f = GetFocus();
    bool inPanel = f && (f == P.wnd || IsChild(P.wnd, f));
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (msg->wParam == VK_RETURN && ctrl && (inPanel || f == App_Editor().hwnd)) {
        if (P.state == ST_DONE) Accept();
        return true;
    }
    if (!inPanel) return false;
    if (msg->wParam == VK_ESCAPE) {
        if (SendMessageW(P.instr, CB_GETDROPPEDSTATE, 0, 0)) { SendMessageW(P.instr, CB_SHOWDROPDOWN, FALSE, 0); return true; }
        if (SendMessageW(P.model, CB_GETDROPPEDSTATE, 0, 0)) return false;
        Close();
        return true;
    }
    if (msg->wParam == VK_RETURN && (f == P.instrEdit || f == P.instr)) {
        if (SendMessageW(P.instr, CB_GETDROPPEDSTATE, 0, 0)) SendMessageW(P.instr, CB_SHOWDROPDOWN, FALSE, 0);
        if (P.state == ST_RUNNING) return true;
        Generate();
        return true;
    }
    return false;
}

void AiPanel_Shutdown() {
    if (P.jobId) Ai_Cancel(P.jobId);
}
