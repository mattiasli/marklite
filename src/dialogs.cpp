// Settings and "Go to line" dialogs (resource templates in res/app.rc).
#include "app.h"
#include "ai.h"
#include "resource.h"
#include <commctrl.h>
#include <memory>

namespace {
bool g_statusOk = false, g_statusErr = false;

void ThemeDialog(HWND d) {
    Theme_TitleBar(d);
    for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        if (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"ComboBox")) Theme_Edit(c);
        else if (!_wcsicmp(cls, L"Button")) Theme_Control(c);
    }
}

INT_PTR DlgColor(UINT m, WPARAM w, LPARAM l, HWND d) {
    HDC dc = (HDC)w;
    HWND ctl = (HWND)l;
    const Palette& p = Pal();
    bool dark = Theme_IsDark();
    if (m == WM_CTLCOLORDLG) {
        if (!dark) return 0;
        return (INT_PTR)Theme_Brush(p.chromeBg);
    }
    if (m == WM_CTLCOLORSTATIC) {
        int id = GetDlgCtrlID(ctl);
        COLORREF fg = dark ? p.chromeFg : GetSysColor(COLOR_WINDOWTEXT);
        if (id == IDC_STATUS && (g_statusOk || g_statusErr)) fg = g_statusOk ? p.ok : p.error;
        if (id == IDC_KEYHINT) fg = dark ? p.chromeDim : GetSysColor(COLOR_GRAYTEXT);
        SetTextColor(dc, fg);
        if (!dark) { SetBkMode(dc, TRANSPARENT); return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE); }
        SetBkColor(dc, p.chromeBg);
        return (INT_PTR)Theme_Brush(p.chromeBg);
    }
    if (!dark) return 0;
    if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) return (INT_PTR)Theme_CtlColor(dc, true);
    if (m == WM_CTLCOLORBTN) { SetBkColor(dc, p.chromeBg); return (INT_PTR)Theme_Brush(p.chromeBg); }
    (void)d;
    return 0;
}

void SetStatus(HWND d, const std::wstring& s, bool ok, bool err) {
    g_statusOk = ok;
    g_statusErr = err;
    SetDlgItemTextW(d, IDC_STATUS, s.c_str());
    InvalidateRect(GetDlgItem(d, IDC_STATUS), nullptr, TRUE);
}

void FillModelCombo(HWND d, const std::wstring& cur) {
    HWND cb = GetDlgItem(d, IDC_MODEL);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (auto& m : g_set.models) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)m.id.c_str());
    SetWindowTextW(cb, cur.c_str());
}

INT_PTR CALLBACK SettingsProc(HWND d, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        g_statusOk = g_statusErr = false;
        SetDlgItemTextW(d, IDC_APIKEY, g_set.apiKey.c_str());
        SendDlgItemMessageW(d, IDC_APIKEY, EM_SETCUEBANNER, TRUE, (LPARAM)L"sk-ant-...");
        FillModelCombo(d, g_set.model);
        HWND ef = GetDlgItem(d, IDC_EFFORT);
        const wchar_t* efforts[] = {L"low", L"medium", L"high", L"xhigh", L"max"};
        for (auto e : efforts) SendMessageW(ef, CB_ADDSTRING, 0, (LPARAM)e);
        int sel = (int)SendMessageW(ef, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)g_set.effort.c_str());
        SendMessageW(ef, CB_SETCURSEL, sel < 0 ? 1 : sel, 0);
        CheckDlgButton(d, IDC_CONTEXT, g_set.aiContext ? BST_CHECKED : BST_UNCHECKED);
        ThemeDialog(d);
        if (g_set.apiKey.empty()) {
            SetStatus(d, L"Paste your Anthropic API key to enable \"Rewrite with Claude\" (Ctrl+K).", false, false);
            SetFocus(GetDlgItem(d, IDC_APIKEY));
            return FALSE;
        }
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return DlgColor(m, w, l, d);
    case WM_APP_MODELS: {
        std::unique_ptr<ModelList> ml((ModelList*)l);
        EnableWindow(GetDlgItem(d, IDC_VERIFY), TRUE);
        EnableWindow(GetDlgItem(d, IDC_REFRESH), TRUE);
        if (ml->ok) {
            if (!ml->models.empty()) g_set.models = ml->models;
            wchar_t cur[256];
            GetDlgItemTextW(d, IDC_MODEL, cur, 256);
            FillModelCombo(d, cur);
            SetStatus(d, L"\x2714 The key works. " + std::to_wstring(ml->models.size()) + L" models available.", true, false);
        } else {
            SetStatus(d, L"\x2716 " + ml->error, false, true);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_SHOWKEY: {
            HWND e = GetDlgItem(d, IDC_APIKEY);
            bool hidden = SendMessageW(e, EM_GETPASSWORDCHAR, 0, 0) != 0;
            SendMessageW(e, EM_SETPASSWORDCHAR, hidden ? 0 : 0x25CF, 0);
            SetDlgItemTextW(d, IDC_SHOWKEY, hidden ? L"Hide" : L"Show");
            InvalidateRect(e, nullptr, TRUE);
            return TRUE;
        }
        case IDC_VERIFY:
        case IDC_REFRESH: {
            wchar_t key[512];
            GetDlgItemTextW(d, IDC_APIKEY, key, 512);
            std::wstring k = Trim(key);
            if (k.empty()) { SetStatus(d, L"Enter an API key first.", false, true); return TRUE; }
            EnableWindow(GetDlgItem(d, IDC_VERIFY), FALSE);
            EnableWindow(GetDlgItem(d, IDC_REFRESH), FALSE);
            SetStatus(d, L"Contacting api.anthropic.com...", false, false);
            Ai_FetchModels(d, k);
            return TRUE;
        }
        case IDC_CONTEXT_LBL:
            if (HIWORD(w) == STN_CLICKED)
                CheckDlgButton(d, IDC_CONTEXT, IsDlgButtonChecked(d, IDC_CONTEXT) ? BST_UNCHECKED : BST_CHECKED);
            return TRUE;
        case IDOK: {
            wchar_t buf[512];
            GetDlgItemTextW(d, IDC_APIKEY, buf, 512);
            g_set.apiKey = Trim(buf);
            GetDlgItemTextW(d, IDC_MODEL, buf, 512);
            std::wstring model = Trim(buf);
            if (!model.empty()) g_set.model = model;
            int ei = (int)SendDlgItemMessageW(d, IDC_EFFORT, CB_GETCURSEL, 0, 0);
            if (ei >= 0) {
                SendDlgItemMessageW(d, IDC_EFFORT, CB_GETLBTEXT, ei, (LPARAM)buf);
                g_set.effort = buf;
            }
            g_set.aiContext = IsDlgButtonChecked(d, IDC_CONTEXT) == BST_CHECKED;
            Settings_Save();
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(d, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR CALLBACK GotoProc(HWND d, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        auto* info = (std::pair<int, int>*)l;
        SetWindowLongPtrW(d, GWLP_USERDATA, (LONG_PTR)info);
        SetDlgItemInt(d, IDC_LINE, info->first, FALSE);
        std::wstring lbl = L"Line number (1 - " + FormatThousands(info->second) + L"):";
        SetDlgItemTextW(d, IDC_LBL1, lbl.c_str());
        ThemeDialog(d);
        SendDlgItemMessageW(d, IDC_LINE, EM_SETSEL, 0, -1);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        return DlgColor(m, w, l, d);
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            BOOL ok = FALSE;
            int v = (int)GetDlgItemInt(d, IDC_LINE, &ok, FALSE);
            EndDialog(d, ok ? v : 0);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        break;
    }
    return FALSE;
}
}

void Dlg_Settings(HWND owner) {
    DialogBoxParamW(App_Instance(), MAKEINTRESOURCEW(IDD_SETTINGS), owner, SettingsProc, 0);
}

int Dlg_GotoLine(HWND owner, int current, int maxLine) {
    std::pair<int, int> info{current, maxLine};
    return (int)DialogBoxParamW(App_Instance(), MAKEINTRESOURCEW(IDD_GOTO), owner, GotoProc, (LPARAM)&info);
}
