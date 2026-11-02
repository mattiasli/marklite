// MarkLite - main window: documents & tabs, menus, layout, status bar, file I/O.
#include "app.h"
#include "resource.h"
#include "tabs.h"
#include "findbar.h"
#include "aipanel.h"
#include "preview.h"
#include "spell.h"
#include "ai.h"
#include "toolbar.h"
#include "format.h"
#include "clipimage.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <windowsx.h>
#include <algorithm>
#include <memory>

#ifndef REPLACEFILE_IGNORE_ACL_ERRORS
#define REPLACEFILE_IGNORE_ACL_ERRORS 0x00000004
#endif

extern "C" int Scintilla_RegisterClasses(void* hInstance);
void Dlg_Settings(HWND owner);
int Dlg_GotoLine(HWND owner, int current, int maxLine);

namespace {

enum { ID_EDITOR = 100, ID_AUX = 101 };
enum { T_PREVIEW = 1, T_SPELL, T_WORDS, T_MSG, T_FIND, T_TOOLBAR };
enum Enc { ENC_UTF8, ENC_UTF8BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_ANSI };
const wchar_t* const kEncNames[] = {L"UTF-8", L"UTF-8 with BOM", L"UTF-16 LE", L"UTF-16 BE", L"ANSI"};
const wchar_t* const kMainClass = L"MarkLiteMain";
const ULONG_PTR kCopyDataOpen = 0x4D4C4F50;   // 'MLOP'

struct Doc {
    int id = 0;
    std::wstring path, name;          // name = display name of an untitled document
    sptr_t sci = 0;                   // Scintilla document (we own one reference)
    int enc = ENC_UTF8;
    int eol = SC_EOL_CRLF;
    FILETIME mtime{};
    bool modified = false, forceDirty = false, missing = false;
    bool eolConverted = false;        // line endings were converted: undo / redo can change them back
    sptr_t anchor = 0, caret = 0, topLine = 0;
};

HINSTANCE g_inst;
HWND g_main, g_tabs, g_toolbar, g_findBar, g_aiPanel, g_preview;
Sci g_ed, g_aux;
std::vector<std::unique_ptr<Doc>> g_docs;
int g_active = -1, g_nextId = 1;
bool g_primary = true, g_previewDirty = true, g_closing = false;
HACCEL g_accFull, g_accGlobal;
HMENU g_menuBar, g_mFile, g_mRecent, g_mEdit, g_mFormat, g_mView, g_mTheme, g_mSpell, g_mAi, g_mHelp, g_mEnc, g_mEol;
HFONT g_statusFont;
int g_dpi = 96;
std::wstring g_msg;
bool g_msgErr = false;
long long g_words = 0;
DWORD g_lastChar = 0;
HWND g_lastFocus = nullptr;
RECT g_splitRc{}, g_statusRc{};
bool g_splitDrag = false;
enum SegKind { SEG_NONE = -1, SEG_POS, SEG_WORDS, SEG_ZOOM, SEG_SPELL, SEG_ENC, SEG_EOL };
struct Seg { RECT rc; int kind; };
std::vector<Seg> g_segs;
int g_segHot = SEG_NONE;
bool g_checkingFiles = false;

// ------------------------------------------------------------------ small helpers
Doc& Cur() { return *g_docs[g_active]; }
bool Dirty(const Doc& d) { return d.modified || d.forceDirty || d.missing; }
int S(int v) { return Sc(v, g_dpi); }

std::wstring Title(const Doc& d) {
    if (!d.path.empty()) return FileNameOf(d.path);
    return d.name.empty() ? L"Untitled" : d.name;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

std::wstring ExtOf(const std::wstring& p) {
    size_t d = p.find_last_of(L'.'), s = p.find_last_of(L"\\/");
    if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return L"";
    return Lower(p.substr(d + 1));
}

bool IsMarkdownExt(const std::wstring& e) {
    return e == L"md" || e == L"markdown" || e == L"mdown" || e == L"mkd" || e == L"mkdn" || e == L"mdx" || e == L"txt" || e == L"mdtxt";
}

std::wstring ErrText(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? Trim(buf) : L"Error " + std::to_wstring(e);
    if (buf) LocalFree(buf);
    return s;
}

void MsgError(const std::wstring& s) { MessageBoxW(g_main, s.c_str(), APP_NAME, MB_ICONERROR); }

bool IsClass(HWND h, const wchar_t* cls) {
    wchar_t b[64] = {};
    GetClassNameW(h, b, 64);
    return !_wcsicmp(b, cls);
}

std::wstring NextUntitledName() {
    for (int n = 1;; n++) {
        std::wstring s = n == 1 ? L"Untitled" : L"Untitled " + std::to_wstring(n);
        bool used = false;
        for (auto& d : g_docs) if (d->path.empty() && d->name == s) used = true;
        if (!used) return s;
    }
}

void InvalidateStatus() { InvalidateRect(g_main, &g_statusRc, FALSE); }

// ------------------------------------------------------------------ encodings
bool ValidUtf8(const unsigned char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int k;
        unsigned cp;
        if ((c & 0xE0) == 0xC0) { if (c < 0xC2) return false; k = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { if (c > 0xF4) return false; k = 3; cp = c & 0x07; }
        else return false;
        if (i + k >= n) return false;
        for (int j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if (k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += k + 1;
    }
    return true;
}

// Code page for files that aren't Unicode. With the "Use Unicode UTF-8 for worldwide language support"
// setting, the ANSI code page is UTF-8 itself, and such files would decode with every accented byte
// replaced; Windows-1252 maps every byte to a character and back, so they survive a save unchanged.
UINT AnsiCodePage() {
    UINT cp = GetACP();
    return cp == CP_UTF8 ? 1252 : cp;
}

std::string Decode(std::string& raw, int& enc) {
    const unsigned char* b = (const unsigned char*)raw.data();
    size_t n = raw.size();
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { enc = ENC_UTF8BOM; return raw.substr(3); }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        enc = ENC_UTF16LE;
        std::wstring w((n - 2) / 2, L'\0');
        if (!w.empty()) memcpy(&w[0], b + 2, w.size() * 2);
        return WToU8(w);
    }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        enc = ENC_UTF16BE;
        std::wstring w((n - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++) w[i] = (wchar_t)((b[2 + 2 * i] << 8) | b[3 + 2 * i]);
        return WToU8(w);
    }
    if (ValidUtf8(b, n)) { enc = ENC_UTF8; return std::move(raw); }
    enc = ENC_ANSI;
    int len = MultiByteToWideChar(AnsiCodePage(), 0, raw.data(), (int)n, nullptr, 0);
    std::wstring w(len, L'\0');
    if (len) MultiByteToWideChar(AnsiCodePage(), 0, raw.data(), (int)n, &w[0], len);
    return WToU8(w);
}

// Returns false if the text can't be represented losslessly (ANSI only).
bool Encode(const char* p, size_t n, int enc, std::string& out) {
    switch (enc) {
    case ENC_UTF8BOM: out = "\xEF\xBB\xBF"; out.append(p, n); return true;
    case ENC_UTF16LE:
    case ENC_UTF16BE: {
        std::wstring w = U8ToW(p, n);
        out.resize(2 + w.size() * 2);
        bool be = enc == ENC_UTF16BE;
        out[0] = be ? '\xFE' : '\xFF';
        out[1] = be ? '\xFF' : '\xFE';
        for (size_t i = 0; i < w.size(); i++) {
            unsigned v = w[i];
            out[2 + 2 * i] = (char)(be ? v >> 8 : v & 0xFF);
            out[3 + 2 * i] = (char)(be ? v & 0xFF : v >> 8);
        }
        return true;
    }
    case ENC_ANSI: {
        std::wstring w = U8ToW(p, n);
        BOOL used = FALSE;
        // WC_NO_BEST_FIT_CHARS: without it Windows silently turns characters the code page lacks into
        // look-alikes (ł -> l, α -> a) and still reports the text as converted losslessly.
        int len = WideCharToMultiByte(AnsiCodePage(), WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, &used);
        out.assign(len, '\0');
        if (len) WideCharToMultiByte(AnsiCodePage(), WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(), &out[0], len, nullptr, &used);
        return !used;
    }
    default: out.assign(p, n); return true;
    }
}

int DetectEol(const std::string& t) {
    size_t crlf = 0, lf = 0, cr = 0, lim = std::min<size_t>(t.size(), 1 << 20);
    for (size_t i = 0; i < lim; i++) {
        if (t[i] == '\r') {
            if (i + 1 < t.size() && t[i + 1] == '\n') { crlf++; i++; }
            else cr++;
        } else if (t[i] == '\n') lf++;
    }
    if (!crlf && !lf && !cr) return SC_EOL_CRLF;
    if (lf > crlf && lf >= cr) return SC_EOL_LF;
    if (cr > crlf && cr > lf) return SC_EOL_CR;
    return SC_EOL_CRLF;
}

const wchar_t* EolName(int eol) { return eol == SC_EOL_LF ? L"LF" : eol == SC_EOL_CR ? L"CR" : L"CRLF"; }

// ------------------------------------------------------------------ file I/O
bool ReadFileAll(const std::wstring& path, std::string& out, FILETIME& mtime, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = ErrText(GetLastError()); return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > (1LL << 30)) { CloseHandle(h); err = L"The file is larger than 1 GB."; return false; }
    try { out.resize((size_t)sz.QuadPart); } catch (...) { CloseHandle(h); err = L"Not enough memory."; return false; }
    size_t total = 0;
    while (total < out.size()) {
        DWORD rd = 0;
        if (!ReadFile(h, &out[total], (DWORD)std::min<size_t>(out.size() - total, 64u << 20), &rd, nullptr) || !rd) break;
        total += rd;
    }
    out.resize(total);
    GetFileTime(h, nullptr, nullptr, &mtime);
    CloseHandle(h);
    return true;
}

bool WriteAll(HANDLE h, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        DWORD wr = 0;
        if (!WriteFile(h, d.data() + off, (DWORD)std::min<size_t>(d.size() - off, 16u << 20), &wr, nullptr) || !wr) return false;
        off += wr;
    }
    return true;
}

// Writes to a temporary file next to the target and swaps it in, so a failed
// save (disk full, crash) never leaves a half-written document behind.
bool WriteFileSafe(const std::wstring& path, const std::string& data, std::wstring& err) {
    DWORD attr = GetFileAttributesW(path.c_str());
    bool exists = attr != INVALID_FILE_ATTRIBUTES;
    if (exists && (attr & FILE_ATTRIBUTE_READONLY)) { err = L"The file is read-only."; return false; }
    std::wstring tmp = path + L".marklite-tmp";
    bool keepTmp = false;   // the temporary file is the only complete copy on disk
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        bool ok = WriteAll(h, data) && FlushFileBuffers(h);
        DWORD e = GetLastError();
        CloseHandle(h);
        if (ok) {
            if (!exists) {
                if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            } else if (ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
                return true;
            } else if (GetLastError() == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
                // The old file is already gone and the new one couldn't be renamed into its place.
                if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
                keepTmp = true;
            }
        }
        if (!keepTmp) DeleteFileW(tmp.c_str());
        if (!ok) { err = ErrText(e); return false; }
    }
    // Fallback: write in place (e.g. no permission to create files in that folder).
    h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = ErrText(GetLastError());
        if (keepTmp) err += L"\n\nThe document was saved as " + tmp;
        return false;
    }
    bool ok = WriteAll(h, data) && SetEndOfFile(h);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) {
        err = ErrText(e);
        if (keepTmp) err += L"\n\nThe document was saved as " + tmp;
        return false;
    }
    if (keepTmp) DeleteFileW(tmp.c_str());
    return true;
}

std::wstring FullPath(const std::wstring& p) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    std::wstring r = (n && n < buf.size()) ? std::wstring(buf.data(), n) : p;
    n = GetLongPathNameW(r.c_str(), buf.data(), (DWORD)buf.size());
    if (n && n < buf.size()) r.assign(buf.data(), n);
    return r;
}

// ------------------------------------------------------------------ Scintilla documents
// Documents are created and filled in a hidden helper view so the visible editor
// is untouched until the user switches to the tab.
sptr_t NewSciDoc(const char* text, size_t n, int eol) {
    sptr_t doc = g_aux(SCI_CREATEDOCUMENT, n + 1, SC_DOCUMENTOPTION_DEFAULT);
    if (!doc) return 0;
    g_aux(SCI_SETDOCPOINTER, 0, doc);
    g_aux(SCI_SETCODEPAGE, SC_CP_UTF8);
    g_aux(SCI_SETEOLMODE, eol);
    if (n) {
        g_aux(SCI_SETUNDOCOLLECTION, 0);
        g_aux(SCI_APPENDTEXT, n, (sptr_t)text);
        g_aux(SCI_SETUNDOCOLLECTION, 1);
    }
    g_aux(SCI_EMPTYUNDOBUFFER);
    g_aux(SCI_SETSAVEPOINT);
    g_aux(SCI_SETDOCPOINTER, 0, 0);
    return doc;
}

template <class F> void WithText(Doc& d, F fn) {
    if (g_active >= 0 && &d == &Cur()) { fn((const char*)g_ed(SCI_GETCHARACTERPOINTER), (size_t)g_ed.Len()); return; }
    g_aux(SCI_SETDOCPOINTER, 0, d.sci);
    fn((const char*)g_aux(SCI_GETCHARACTERPOINTER), (size_t)g_aux.Len());
    g_aux(SCI_SETDOCPOINTER, 0, 0);
}

// Runs fn(Sci&) on a document, whether or not it is the one shown in the editor.
template <class F> auto WithSci(Doc& d, F fn) -> decltype(fn(g_ed)) {
    if (g_active >= 0 && &d == &Cur()) return fn(g_ed);
    g_aux(SCI_SETDOCPOINTER, 0, d.sci);
    auto r = fn(g_aux);
    g_aux(SCI_SETDOCPOINTER, 0, 0);
    return r;
}

void SetSavePoint(Doc& d) {
    if (g_active >= 0 && &d == &Cur()) { g_ed(SCI_SETSAVEPOINT); return; }
    g_aux(SCI_SETDOCPOINTER, 0, d.sci);
    g_aux(SCI_SETSAVEPOINT);
    g_aux(SCI_SETDOCPOINTER, 0, 0);
}

// ------------------------------------------------------------------ UI updates
void UpdateTabs() {
    std::vector<TabItem> items;
    for (auto& d : g_docs) {
        TabItem t;
        t.title = Title(*d);
        t.tooltip = d->path.empty() ? Title(*d) + L" (not saved yet)" : d->path;
        t.modified = Dirty(*d);
        items.push_back(t);
    }
    Tabs_Set(g_tabs, items, g_active);
}

void UpdateTitle() {
    if (g_active < 0) return;
    Doc& d = Cur();
    std::wstring t = (Dirty(d) ? L"*" : L"") + Title(d) + L" - " + APP_NAME;
    SetWindowTextW(g_main, t.c_str());
}

void UpdateLineNumbers() {
    if (!g_ed.fn) return;   // WM_DPICHANGED can arrive while the window is placed, before the editor exists
    if (!g_set.lineNumbers) { g_ed(SCI_SETMARGINWIDTHN, 0, 0); return; }
    sptr_t lines = g_ed(SCI_GETLINECOUNT);
    int digits = 1;
    while (lines >= 10) { lines /= 10; digits++; }
    std::string s(std::max(3, digits) + 1, '9');
    int w = (int)g_ed(SCI_TEXTWIDTH, STYLE_LINENUMBER, (sptr_t)s.c_str()) + S(6);
    if (g_ed(SCI_GETMARGINWIDTHN, 0) != w) g_ed(SCI_SETMARGINWIDTHN, 0, w);
}

void UpdateEditorMargins() {
    RECT rc;
    GetClientRect(g_ed.hwnd, &rc);
    int base = S(16), left = base, right = base;
    if (g_set.limitWidth) {
        float f = std::max(0.3f, (g_set.fontSize + (float)g_ed(SCI_GETZOOM)) / (float)std::max(1, g_set.fontSize));
        int maxText = (int)(S(760) * f * g_set.fontSize / 11.0f);
        int avail = rc.right - (int)g_ed(SCI_GETMARGINWIDTHN, 0);
        if (avail - maxText > 2 * base) left = right = (avail - maxText) / 2;
    }
    if (g_ed(SCI_GETMARGINLEFT) != left) g_ed(SCI_SETMARGINLEFT, 0, left);
    if (g_ed(SCI_GETMARGINRIGHT) != right) g_ed(SCI_SETMARGINRIGHT, 0, right);
}

long long CountWords(const char* p, size_t n) {
    long long words = 0;
    bool in = false;
    const unsigned char* s = (const unsigned char*)p;
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        bool w;
        if (c < 0x80) {
            w = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (in && (c == '\'' || c == '-' || c == '_'));
            i++;
        } else {
            unsigned cp = 0;
            int k = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
            cp = k == 1 ? c & 0x1F : k == 2 ? c & 0x0F : c & 0x07;
            for (int j = 1; j <= k && i + j < n; j++) cp = (cp << 6) | (s[i + j] & 0x3F);
            i += k + 1;
            w = cp >= 0x10000 || IsCharAlphaNumericW((wchar_t)cp) || (in && cp == 0x2019);
        }
        if (w && !in) words++;
        in = w;
    }
    return words;
}

// ------------------------------------------------------------------ preview
bool PreviewShown() { return g_set.viewMode != 0; }

void SyncPreview(bool force = false) {
    if (!PreviewShown() || g_previewDirty || g_active < 0) return;
    if (!force && g_set.viewMode != 1) return;
    sptr_t vis = g_ed(SCI_GETFIRSTVISIBLELINE);
    sptr_t total = g_ed(SCI_VISIBLEFROMDOCLINE, g_ed(SCI_GETLINECOUNT));
    if (vis > 0 && vis + g_ed(SCI_LINESONSCREEN) >= total) { Preview_SyncToSource(g_preview, (size_t)-1); return; }
    sptr_t pos = vis == 0 ? 0 : g_ed(SCI_POSITIONFROMLINE, g_ed(SCI_DOCLINEFROMVISIBLE, vis));
    Preview_SyncToSource(g_preview, (size_t)pos);
}

void RefreshPreview() {
    KillTimer(g_main, T_PREVIEW);
    if (!PreviewShown() || g_active < 0) { g_previewDirty = true; return; }
    g_previewDirty = false;
    Preview_SetMarkdown(g_preview, (const char*)g_ed(SCI_GETCHARACTERPOINTER), (size_t)g_ed.Len(), DirOf(Cur().path));
    SyncPreview();
}

void SchedulePreview() {
    g_previewDirty = true;
    if (!PreviewShown()) return;
    UINT delay = (UINT)std::min<sptr_t>(1000, 150 + g_ed.Len() / 40000);
    SetTimer(g_main, T_PREVIEW, delay, nullptr);
}

void ScheduleSpell(UINT ms) {
    if (g_set.spell) SetTimer(g_main, T_SPELL, ms, nullptr);
}

void ScheduleWords() {
    SetTimer(g_main, T_WORDS, (UINT)std::min<sptr_t>(1500, 250 + g_ed.Len() / 20000), nullptr);
}

void SetViewMode(int m);
void UpdateToolbar();
void ScheduleToolbar();

// ------------------------------------------------------------------ activation
void SaveViewState() {
    if (g_active < 0 || g_active >= (int)g_docs.size()) return;
    Doc& d = Cur();
    d.anchor = g_ed(SCI_GETANCHOR);
    d.caret = g_ed(SCI_GETCURRENTPOS);
    d.topLine = g_ed(SCI_DOCLINEFROMVISIBLE, g_ed(SCI_GETFIRSTVISIBLELINE));
}

void RestoreView(Doc& d) {
    sptr_t len = g_ed.Len();
    g_ed(SCI_SETSEL, std::min(d.anchor, len), std::min(d.caret, len));
    g_ed(SCI_SETFIRSTVISIBLELINE, g_ed(SCI_VISIBLEFROMDOCLINE, d.topLine));
}

void AttachActive() {
    Doc& d = Cur();
    g_ed(SCI_SETDOCPOINTER, 0, d.sci);
    g_ed(SCI_SETCODEPAGE, SC_CP_UTF8);
    g_ed(SCI_SETEOLMODE, d.eol);
    g_ed(SCI_SETTABWIDTH, 4);
    g_ed(SCI_SETUSETABS, 0);
    d.modified = g_ed(SCI_GETMODIFY) != 0;
}

void AfterDocChange() {
    UpdateTabs();
    UpdateTitle();
    UpdateLineNumbers();
    UpdateEditorMargins();
    g_previewDirty = true;
    RefreshPreview();
    SyncPreview(true);
    Find_Refresh();
    AiPanel_OnDocSwitch();
    if (g_set.spell) ScheduleSpell(30);
    else Spell::Clear(g_ed);
    g_words = CountWords((const char*)g_ed(SCI_GETCHARACTERPOINTER), (size_t)g_ed.Len());
    InvalidateStatus();
}

void Activate(int idx) {
    if (idx < 0 || idx >= (int)g_docs.size()) return;
    if (idx == g_active) return;
    SaveViewState();
    g_active = idx;
    AttachActive();
    RestoreView(Cur());
    AfterDocChange();
    ScheduleToolbar();
}

int AddDoc(sptr_t sci, const std::wstring& path, int enc, int eol, FILETIME mt) {
    auto d = std::make_unique<Doc>();
    d->id = g_nextId++;
    d->sci = sci;
    d->path = path;
    d->enc = enc;
    d->eol = eol;
    d->mtime = mt;
    if (path.empty()) d->name = NextUntitledName();
    g_docs.push_back(std::move(d));
    return (int)g_docs.size() - 1;
}

int NewUntitled(const std::string& text = std::string()) {
    return AddDoc(NewSciDoc(text.data(), text.size(), SC_EOL_CRLF), L"", ENC_UTF8, SC_EOL_CRLF, FILETIME{});
}

int FindDoc(const std::wstring& path) {
    for (size_t i = 0; i < g_docs.size(); i++)
        if (!g_docs[i]->path.empty() && !_wcsicmp(g_docs[i]->path.c_str(), path.c_str())) return (int)i;
    return -1;
}

bool LoadText(const std::wstring& path, sptr_t& sci, int& enc, int& eol, FILETIME& mt, std::wstring& err) {
    std::string raw;
    if (!ReadFileAll(path, raw, mt, err)) return false;
    std::string text = Decode(raw, enc);
    raw = std::string();
    eol = DetectEol(text);
    sci = NewSciDoc(text.data(), text.size(), eol);
    if (!sci) { err = L"Not enough memory."; return false; }
    return true;
}

int OpenFile(const std::wstring& p, bool activate = true, bool quiet = false) {
    std::wstring path = FullPath(p);
    int ex = FindDoc(path);
    if (ex >= 0) { if (activate) Activate(ex); return ex; }
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { if (!quiet) MsgError(L"Couldn't find the file\n\n" + path); return -1; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { if (!quiet) App_Status(L"That is a folder, not a file", true); return -1; }
    sptr_t sci;
    int enc, eol;
    FILETIME mt;
    std::wstring err;
    if (!LoadText(path, sci, enc, eol, mt, err)) { if (!quiet) MsgError(L"Couldn't open\n" + path + L"\n\n" + err); return -1; }
    Settings_AddRecent(path);
    // Reuse an empty, untouched "Untitled" tab.
    if (g_active >= 0 && Cur().path.empty() && !Dirty(Cur()) && g_ed.Len() == 0) {
        Doc& d = Cur();
        sptr_t old = d.sci;
        d.sci = sci;
        d.path = path;
        d.name.clear();
        d.enc = enc;
        d.eol = eol;
        d.mtime = mt;
        d.anchor = d.caret = d.topLine = 0;
        AttachActive();
        g_ed(SCI_RELEASEDOCUMENT, 0, old);
        RestoreView(d);
        AfterDocChange();
        return g_active;
    }
    int idx = AddDoc(sci, path, enc, eol, mt);
    if (activate) Activate(idx);
    else UpdateTabs();
    return idx;
}

bool ReloadDoc(int idx) {
    Doc& d = *g_docs[idx];
    sptr_t sci;
    int enc, eol;
    FILETIME mt;
    std::wstring err;
    if (!LoadText(d.path, sci, enc, eol, mt, err)) { MsgError(L"Couldn't reload\n" + d.path + L"\n\n" + err); return false; }
    AiPanel_OnDocClosed(d.id);
    bool active = idx == g_active;
    if (active) SaveViewState();
    sptr_t old = d.sci;
    d.sci = sci;
    d.enc = enc;
    d.eol = eol;
    d.mtime = mt;
    d.forceDirty = d.missing = d.modified = d.eolConverted = false;
    if (active) {
        AttachActive();
        g_ed(SCI_RELEASEDOCUMENT, 0, old);
        RestoreView(d);
        AfterDocChange();
    } else {
        g_ed(SCI_RELEASEDOCUMENT, 0, old);
        UpdateTabs();
    }
    return true;
}

std::wstring SuggestName(Doc& d) {
    if (!d.path.empty()) return FileNameOf(d.path);
    std::string first;
    WithText(d, [&](const char* p, size_t n) {
        size_t i = 0;
        while (i < n && i < 8192) {
            size_t e = i;
            while (e < n && p[e] != '\n' && p[e] != '\r') e++;
            std::string line(p + i, e - i);
            size_t a = line.find_first_not_of(" \t#>*-+");
            if (a != std::string::npos) { first = line.substr(a); break; }
            i = e + 1;
        }
    });
    std::wstring w = U8ToW(first), out;
    for (wchar_t c : w) {
        if (wcschr(L"<>:\"/\\|?*`[]()", c) || c < 32) continue;
        out += c;
        if (out.size() >= 60) break;
    }
    out = Trim(out);
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    if (out.empty()) out = Title(d);
    return out + L".md";
}

std::wstring AskSavePath(Doc& d, const wchar_t* title = nullptr) {
    std::vector<wchar_t> buf(32768, 0);
    std::wstring name = SuggestName(d);
    wcsncpy(buf.data(), name.c_str(), buf.size() - 1);
    std::wstring dir = !d.path.empty() ? DirOf(d.path) : L"";
    if (dir.empty())
        for (auto& o : g_docs) if (!o->path.empty()) { dir = DirOf(o->path); break; }
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    of.lpstrFilter = L"Markdown (*.md)\0*.md;*.markdown\0Text (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.lpstrDefExt = L"md";
    of.lpstrTitle = title;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING | OFN_NOREADONLYRETURN;
    if (!GetSaveFileNameW(&of)) return L"";
    return buf.data();
}

bool SaveDoc(int idx, bool saveAs, const wchar_t* dialogTitle = nullptr) {
    Doc& d = *g_docs[idx];
    std::wstring path = d.path;
    if (saveAs || path.empty()) {
        path = AskSavePath(d, dialogTitle);
        if (path.empty()) return false;
    }
    // Saving over a file that another tab has open: that tab would show (and could later save back)
    // the old content, so it is closed - unless it holds unsaved changes of its own.
    int otherId = -1;
    int other = FindDoc(path);
    if (other >= 0 && other != idx) {
        if (Dirty(*g_docs[other])) {
            MsgError(path + L"\n\nis open in another tab with unsaved changes. Save or close that tab first.");
            return false;
        }
        otherId = g_docs[other]->id;
    }
    std::string data;
    bool lossless = true;
    WithText(d, [&](const char* p, size_t n) { lossless = Encode(p, n, d.enc, data); });
    if (!lossless) {
        if (MessageBoxW(g_main, L"Some characters in this document can't be stored in the ANSI code page.\n\nSave the file as UTF-8 instead?",
                        APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES)
            return false;
        d.enc = ENC_UTF8;
        WithText(d, [&](const char* p, size_t n) { Encode(p, n, d.enc, data); });
    }
    // Saved into another folder: take the document's images along (pasted ones live in images\).
    ImageMove moved;
    if (!d.path.empty() && path != d.path) {
        moved = WithSci(d, [&](Sci& s) { return Fmt_RelocateImages(s, d.path, path); });
        if (moved.rewritten) {
            bool fits = true;
            WithText(d, [&](const char* p, size_t n) { fits = Encode(p, n, d.enc, data); });
            if (!fits) {   // a rewritten link has characters the ANSI code page lacks
                if (MessageBoxW(g_main, L"The updated image links contain characters that can't be stored in the ANSI code page.\n\nSave the file as UTF-8 instead?",
                                APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) {
                    WithSci(d, [](Sci& s) { return s(SCI_UNDO); });
                    return false;
                }
                d.enc = ENC_UTF8;
                WithText(d, [&](const char* p, size_t n) { Encode(p, n, d.enc, data); });
            }
        }
    }
    std::wstring err;
    if (!WriteFileSafe(path, data, err)) {
        if (moved.rewritten) WithSci(d, [](Sci& s) { return s(SCI_UNDO); });   // links back as they were
        MsgError(L"Couldn't save\n" + path + L"\n\n" + err);
        return false;
    }
    bool pathChanged = path != d.path;
    if (otherId >= 0) {   // close the tab that had the file we just replaced
        for (size_t i = 0; i < g_docs.size(); i++)
            if (g_docs[i]->id == otherId) {
                if ((int)i == g_active) Activate(idx);   // (Save all can save a tab that isn't shown)
                Doc* o = g_docs[i].get();
                AiPanel_OnDocClosed(o->id);
                sptr_t sci = o->sci;
                g_docs.erase(g_docs.begin() + i);
                if (g_active > (int)i) g_active--;
                if (idx > (int)i) idx--;
                g_ed(SCI_RELEASEDOCUMENT, 0, sci);
                break;
            }
    }
    d.path = path;
    d.name.clear();
    d.forceDirty = d.missing = false;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) d.mtime = fa.ftLastWriteTime;
    SetSavePoint(d);
    d.modified = false;
    Settings_AddRecent(path);
    UpdateTabs();
    UpdateTitle();
    if (pathChanged && idx == g_active) RefreshPreview();
    std::wstring note;
    if (moved.copied) note = L" (copied " + std::to_wstring(moved.copied) + (moved.copied == 1 ? L" image" : L" images") + L" to the new folder)";
    App_Status(L"Saved " + path + note);
    if (!moved.error.empty())
        MsgError(L"The document was saved, but an image it uses couldn't be copied to the new folder:\n\n" + moved.error);
    return true;
}

bool ConfirmSave(int idx) {
    Doc& d = *g_docs[idx];
    if (!Dirty(d)) return true;
    if (idx != g_active) Activate(idx);
    std::wstring msg = L"Save changes to " + Title(d) + L"?";
    TASKDIALOG_BUTTON buttons[] = {{IDYES, L"&Save"}, {IDNO, L"Do&n't save"}};
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = msg.c_str();
    tc.pszContent = d.path.empty() ? L"This document has never been saved." : d.path.c_str();
    tc.pButtons = buttons;
    tc.cButtons = 2;
    tc.nDefaultButton = IDYES;
    int btn = 0;
    if (FAILED(TaskDialogIndirect(&tc, &btn, nullptr, nullptr)))
        btn = MessageBoxW(g_main, msg.c_str(), APP_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (btn == IDYES) return SaveDoc(idx, false);
    return btn == IDNO;
}

bool CloseDoc(int idx) {
    if (idx < 0 || idx >= (int)g_docs.size()) return false;
    if (!ConfirmSave(idx)) return false;
    Doc* d = g_docs[idx].get();
    AiPanel_OnDocClosed(d->id);
    if (g_docs.size() == 1) {
        if (d->path.empty() && !Dirty(*d) && g_ed.Len() == 0) return true;
        Activate(NewUntitled());
    } else if (idx == g_active) {
        Activate(idx + 1 < (int)g_docs.size() ? idx + 1 : idx - 1);
    }
    sptr_t sci = d->sci;
    g_docs.erase(g_docs.begin() + idx);
    if (g_active > idx) g_active--;
    g_ed(SCI_RELEASEDOCUMENT, 0, sci);
    // a lone "Untitled 3" becomes "Untitled" again (but the Markdown guide keeps its name)
    if (g_docs.size() == 1 && g_docs[0]->path.empty() && !Dirty(*g_docs[0]) && g_docs[0]->name.rfind(L"Untitled", 0) == 0) {
        g_docs[0]->name.clear();
        g_docs[0]->name = NextUntitledName();
    }
    UpdateTabs();
    UpdateTitle();
    return true;
}

void CheckFiles() {
    if (g_checkingFiles || g_closing) return;
    g_checkingFiles = true;
    for (size_t i = 0; i < g_docs.size(); i++) {
        Doc& d = *g_docs[i];
        if (d.path.empty()) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(d.path.c_str(), GetFileExInfoStandard, &fa)) {
            if (!d.missing) {
                d.missing = true;
                UpdateTabs();
                UpdateTitle();
                App_Status(Title(d) + L" was deleted or moved outside " APP_NAME, true);
            }
            continue;
        }
        if (d.missing) {   // back again (a sync tool or a rename and back): no longer unsaved because of it
            d.missing = false;
            UpdateTabs();
            UpdateTitle();
        }
        if (CompareFileTime(&fa.ftLastWriteTime, &d.mtime) == 0) continue;
        d.mtime = fa.ftLastWriteTime;
        if (!Dirty(d)) {
            if (ReloadDoc((int)i)) App_Status(L"Reloaded " + Title(d) + L" (changed by another program)");
        } else {
            Activate((int)i);
            std::wstring msg = Title(d) + L" was changed by another program.\n\nReload it and lose your unsaved changes?";
            if (MessageBoxW(g_main, msg.c_str(), APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) ReloadDoc((int)i);
        }
    }
    g_checkingFiles = false;
}

void OpenDialog() {
    std::vector<wchar_t> buf(65536, 0);
    std::wstring dir = (g_active >= 0 && !Cur().path.empty()) ? DirOf(Cur().path) : L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    of.lpstrFilter = L"Markdown and text\0*.md;*.markdown;*.mdown;*.mkd;*.mkdn;*.mdx;*.txt\0All files (*.*)\0*.*\0";
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_ENABLESIZING;
    if (!GetOpenFileNameW(&of)) return;
    const wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (!*p) { OpenFile(first); return; }
    while (*p) {
        std::wstring name = p;
        OpenFile(first + L"\\" + name);
        p += name.size() + 1;
    }
}

void RevealInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

// ------------------------------------------------------------------ Markdown editing helpers
void InsertAtCaret(const std::string& s) { g_ed(SCI_ADDTEXT, s.size(), (sptr_t)s.data()); }

bool IsHrLine(const std::string& s) {
    int marks = 0;
    char ch = 0;
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (ch && c != ch) return false;
        ch = c;
        marks++;
    }
    return marks >= 3;
}

// Continue lists / block quotes when Enter is pressed.
void OnNewLine() {
    if (g_ed(SCI_GETSELECTIONS) != 1) return;
    sptr_t pos = g_ed(SCI_GETCURRENTPOS);
    sptr_t line = g_ed(SCI_LINEFROMPOSITION, pos);
    if (line == 0 || pos != g_ed(SCI_POSITIONFROMLINE, line)) return;
    sptr_t prevStart = g_ed(SCI_POSITIONFROMLINE, line - 1);
    std::string prev = g_ed.LineText(line - 1);
    if (prev.size() > 4000) return;
    size_t i = 0;
    while (i < prev.size() && (prev[i] == ' ' || prev[i] == '\t')) i++;
    size_t indentEnd = i;
    g_ed(SCI_COLOURISE, prevStart, pos);
    if (Md_IsCodeStyle((int)g_ed(SCI_GETSTYLEAT, prevStart + (sptr_t)indentEnd))) {
        if (indentEnd) InsertAtCaret(prev.substr(0, indentEnd));
        return;
    }
    while (i < prev.size() && prev[i] == '>') {
        i++;
        if (i < prev.size() && prev[i] == ' ') i++;
    }
    size_t quoteEnd = i, markerEnd = i;
    std::string marker;
    if (!IsHrLine(prev.substr(i))) {
        if (i + 1 < prev.size() && (prev[i] == '-' || prev[i] == '*' || prev[i] == '+') && prev[i + 1] == ' ') {
            marker = prev.substr(i, 2);
            markerEnd = i + 2;
        } else {
            size_t j = i;
            while (j < prev.size() && j - i < 9 && prev[j] >= '0' && prev[j] <= '9') j++;
            if (j > i && j + 1 < prev.size() && (prev[j] == '.' || prev[j] == ')') && prev[j + 1] == ' ') {
                long n = atol(prev.substr(i, j - i).c_str());
                marker = std::to_string(n + 1) + prev[j] + " ";
                markerEnd = j + 2;
            }
        }
    }
    bool task = false;
    if (!marker.empty() && markerEnd + 3 <= prev.size() && prev[markerEnd] == '[' && prev[markerEnd + 2] == ']' &&
        (prev[markerEnd + 1] == ' ' || prev[markerEnd + 1] == 'x' || prev[markerEnd + 1] == 'X')) {
        task = true;
        markerEnd += 3;
        if (markerEnd < prev.size() && prev[markerEnd] == ' ') markerEnd++;
    }
    bool restBlank = prev.find_first_not_of(" \t", markerEnd) == std::string::npos;
    if (marker.empty() && quoteEnd == indentEnd) {          // plain line: keep indentation
        if (indentEnd && !restBlank) InsertAtCaret(prev.substr(0, indentEnd));
        return;
    }
    // Enter right after the marker of an item with text ("- |foo") moved the text down: that is not an
    // empty item, so the list goes on ("- " above "- foo") instead of ending.
    std::string moved = g_ed.LineText(line);
    if (restBlank && moved.find_first_not_of(" \t") == std::string::npos) {   // empty item: end the list / quote
        g_ed(SCI_SETTARGETRANGE, prevStart, pos);
        g_ed(SCI_REPLACETARGET, 0, (sptr_t)"");
        return;
    }
    InsertAtCaret(prev.substr(0, quoteEnd) + marker + (task ? "[ ] " : ""));
}

// ------------------------------------------------------------------ preview links
std::wstring PercentDecode(const std::wstring& s) {
    std::string b = WToU8(s), out;
    for (size_t i = 0; i < b.size(); i++) {
        if (b[i] == '%' && i + 2 < b.size() && isxdigit((unsigned char)b[i + 1]) && isxdigit((unsigned char)b[i + 2])) {
            out += (char)strtol(b.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += b[i];
    }
    return U8ToW(out);
}

void OnPreviewLink(const std::wstring& url) {
    std::wstring u = Trim(url), low = Lower(u);
    if (u.empty()) return;
    if (low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0 || low.rfind(L"mailto:", 0) == 0) {
        ShellExecuteW(g_main, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    size_t colon = low.find(L':');
    bool drive = colon == 1;
    if (colon != std::wstring::npos && !drive && low.rfind(L"file:", 0) != 0) {
        App_Status(L"Unsupported link: " + u, true);
        return;
    }
    std::wstring p = u;
    size_t q = p.find_first_of(L"?#");
    if (q != std::wstring::npos) p = p.substr(0, q);
    p = PercentDecode(p);
    if (Lower(p).rfind(L"file:///", 0) == 0) p = p.substr(8);
    else if (Lower(p).rfind(L"file:", 0) == 0) p = p.substr(5);
    for (auto& c : p) if (c == L'/') c = L'\\';
    bool abs = (p.size() > 2 && p[1] == L':') || p.rfind(L"\\\\", 0) == 0;
    if (!abs) {
        std::wstring base = g_active >= 0 ? DirOf(Cur().path) : L"";
        if (base.empty()) { App_Status(L"Save the document first to follow relative links", true); return; }
        p = base + L"\\" + p;
    }
    p = FullPath(p);
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { App_Status(L"Not found: " + p, true); return; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { ShellExecuteW(g_main, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    std::wstring ext = ExtOf(p);
    if (IsMarkdownExt(ext)) { OpenFile(p); return; }
    static const wchar_t* risky[] = {L"exe", L"com", L"bat", L"cmd", L"ps1", L"vbs", L"vbe", L"js", L"jse", L"wsf", L"wsh", L"msi", L"msp",
                                     L"scr", L"lnk", L"hta", L"cpl", L"reg", L"pif", L"jar", L"appref-ms", L"url", L"inf", L"dll", L"psm1", L"application"};
    for (auto r : risky)
        if (ext == r) {
            RevealInExplorer(p);
            App_Status(L"Programs aren't started from links - showing the file in Explorer instead");
            return;
        }
    std::wstring msg = L"Open this file with its default program?\n\n" + p;
    if (MessageBoxW(g_main, msg.c_str(), APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
        ShellExecuteW(g_main, L"open", p.c_str(), nullptr, DirOf(p).c_str(), SW_SHOWNORMAL);
}

// ------------------------------------------------------------------ theme / fonts
bool WantDark() { return g_set.theme == 2 || (g_set.theme == 0 && Theme_SystemDark()); }

void ConfigureEditor() {
    Md_ConfigureView(g_ed, true);
    UpdateLineNumbers();
    UpdateEditorMargins();
}

void ApplyTheme() {
    Theme_Set(WantDark());
    Theme_TitleBar(g_main);
    // Make DWM repaint the caption in the new colours.
    bool active = GetActiveWindow() == g_main;
    SendMessageW(g_main, WM_NCACTIVATE, !active, 0);
    SendMessageW(g_main, WM_NCACTIVATE, active, 0);
    SetWindowPos(g_main, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ConfigureEditor();
    Tabs_ThemeChanged(g_tabs);
    if (g_toolbar) Toolbar_ThemeChanged(g_toolbar);
    Find_ThemeChanged();
    AiPanel_ThemeChanged();
    Preview_ThemeChanged(g_preview);
    DrawMenuBar(g_main);
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void MakeStatusFont() {
    if (g_statusFont) DeleteObject(g_statusFont);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    ncm.lfStatusFont.lfHeight = -S(12);
    g_statusFont = CreateFontIndirectW(&ncm.lfStatusFont);
}

// ------------------------------------------------------------------ layout
int StatusH() { return S(26); }
int SplitW() { return S(6); }

void Layout() {
    if (!g_main || !g_tabs || !g_preview) return;
    RECT rc;
    GetClientRect(g_main, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    int y = Tabs_Height(g_tabs);
    HDWP dw = BeginDeferWindowPos(7);
    const UINT fl = SWP_NOZORDER | SWP_NOACTIVATE;
    dw = DeferWindowPos(dw, g_tabs, nullptr, 0, 0, W, y, fl);
    if (g_toolbar && g_set.toolbar) {
        int th = Toolbar_Height(g_toolbar);
        dw = DeferWindowPos(dw, g_toolbar, nullptr, 0, y, W, th, fl);
        y += th;
    }
    int fh = Find_Height();
    if (fh) { dw = DeferWindowPos(dw, g_findBar, nullptr, 0, y, W, fh, fl); y += fh; }
    int bottom = std::max(y, H - StatusH());
    g_statusRc = {0, bottom, W, H};
    int ah = AiPanel_Height();
    if (ah) {
        ah = std::min(ah, std::max(S(170), bottom - y - S(90)));
        dw = DeferWindowPos(dw, g_aiPanel, nullptr, 0, bottom - ah, W, ah, fl);
        bottom -= ah;
    }
    int ch = std::max(0, bottom - y);
    int mode = g_set.viewMode;
    g_splitRc = {0, 0, 0, 0};
    if (mode == 0) dw = DeferWindowPos(dw, g_ed.hwnd, nullptr, 0, y, W, ch, fl);
    else if (mode == 2) dw = DeferWindowPos(dw, g_preview, nullptr, 0, y, W, ch, fl);
    else {
        int sw = SplitW(), avail = std::max(0, W - sw);
        int ew = (int)(avail * g_set.split);
        if (avail > S(400)) ew = std::max(S(200), std::min(avail - S(200), ew));
        dw = DeferWindowPos(dw, g_ed.hwnd, nullptr, 0, y, ew, ch, fl);
        g_splitRc = {ew, y, ew + sw, y + ch};
        dw = DeferWindowPos(dw, g_preview, nullptr, ew + sw, y, W - ew - sw, ch, fl);
    }
    EndDeferWindowPos(dw);
    ShowWindow(g_ed.hwnd, mode != 2 ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_preview, mode != 0 ? SW_SHOWNA : SW_HIDE);
    UpdateEditorMargins();
    InvalidateRect(g_main, nullptr, FALSE);
}

void SetViewMode(int m) {
    m = std::max(0, std::min(2, m));
    HWND f = GetFocus();
    g_set.viewMode = m;
    if (m) g_set.lastPreviewMode = m;
    Layout();
    if (m != 0) {
        if (g_previewDirty) RefreshPreview();
        SyncPreview(true);
    }
    if (m == 2 && (f == g_ed.hwnd || !f)) SetFocus(g_preview);
    else if (m == 0 && f == g_preview) SetFocus(g_ed.hwnd);
    if (m != 2 && g_set.spell) ScheduleSpell(30);
    UpdateToolbar();
}

// ------------------------------------------------------------------ status bar
struct StatusItem { int kind; std::wstring text; };

std::vector<StatusItem> StatusItems() {
    std::vector<StatusItem> v;
    if (g_active < 0) return v;
    Doc& d = Cur();
    sptr_t caret = g_ed(SCI_GETCURRENTPOS);
    std::wstring pos = L"Ln " + FormatThousands(g_ed(SCI_LINEFROMPOSITION, caret) + 1) + L", Col " + FormatThousands(g_ed(SCI_GETCOLUMN, caret) + 1);
    sptr_t a = g_ed(SCI_GETSELECTIONSTART), b = g_ed(SCI_GETSELECTIONEND);
    if (b > a) pos += L"  (" + FormatThousands(g_ed(SCI_COUNTCHARACTERS, a, b)) + L" selected)";
    v.push_back({SEG_POS, pos});
    std::wstring words = FormatThousands(g_words) + (g_words == 1 ? L" word" : L" words");
    if (b > a && b - a < (4 << 20)) {
        std::string sel = g_ed.Range(a, b);
        words = FormatThousands(CountWords(sel.data(), sel.size())) + L" of " + words;
    }
    v.push_back({SEG_WORDS, words});
    if (g_set.zoom != 0) {
        int pct = (int)((g_set.fontSize + g_set.zoom) * 100.0 / std::max(1, g_set.fontSize) + 0.5);
        v.push_back({SEG_ZOOM, std::to_wstring(pct) + L"%"});
    }
    std::wstring sp;
    if (!g_set.spell) sp = L"Spelling off";
    else if (Spell::Active().empty()) sp = L"No spelling language";
    else {
        sp = L"Spelling: ";
        for (size_t i = 0; i < Spell::Active().size(); i++) sp += (i ? L" + " : L"") + Spell::Active()[i];
    }
    v.push_back({SEG_SPELL, sp});
    v.push_back({SEG_ENC, kEncNames[d.enc]});
    v.push_back({SEG_EOL, EolName(d.eol)});
    return v;
}

void PaintStatus(HDC dc) {
    RECT r = g_statusRc;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ ob = SelectObject(mem, bmp);
    const Palette& p = Pal();
    RECT lr{0, 0, w, h};
    FillRect(mem, &lr, Theme_Brush(p.statusBg));
    RECT line{0, 0, w, 1};
    FillRect(mem, &line, Theme_Brush(p.border));
    HGDIOBJ of = SelectObject(mem, g_statusFont);
    SetBkMode(mem, TRANSPARENT);
    g_segs.clear();
    auto items = StatusItems();
    int x = w - S(6);
    for (int i = (int)items.size() - 1; i >= 0; --i) {
        SIZE sz{};
        GetTextExtentPoint32W(mem, items[i].text.c_str(), (int)items[i].text.size(), &sz);
        RECT sr{x - sz.cx - S(20), 1, x, h};
        if (sr.left < S(100)) break;
        bool clickable = items[i].kind != SEG_WORDS;
        if (clickable && g_segHot == items[i].kind) FillRect(mem, &sr, Theme_Brush(p.tabHover));
        SetTextColor(mem, p.statusFg);
        DrawTextW(mem, items[i].text.c_str(), -1, &sr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        g_segs.push_back({{sr.left, r.top + sr.top, sr.right, r.top + sr.bottom}, items[i].kind});
        x = sr.left;
    }
    if (!g_msg.empty()) {
        RECT mr{S(12), 1, x - S(8), h};
        SetTextColor(mem, g_msgErr ? p.error : p.statusFg);
        DrawTextW(mem, g_msg.c_str(), -1, &mr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    BitBlt(dc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, of);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int SegAt(POINT pt) {
    for (auto& s : g_segs) if (PtInRect(&s.rc, pt)) return s.kind;
    return SEG_NONE;
}

void PaintMain() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_main, &ps);
    const Palette& p = Pal();
    RECT rc;
    GetClientRect(g_main, &rc);
    RECT top = rc;
    top.bottom = g_statusRc.top;
    FillRect(dc, &top, Theme_Brush(p.bg));
    if (g_splitRc.right > g_splitRc.left) {
        RECT l = g_splitRc;
        l.left = (g_splitRc.left + g_splitRc.right) / 2;
        l.right = l.left + std::max(1, S(1));
        FillRect(dc, &l, Theme_Brush(g_splitDrag ? p.accent : p.border));
    }
    PaintStatus(dc);
    EndPaint(g_main, &ps);
}

// ------------------------------------------------------------------ menus
void Add(HMENU m, UINT id, const wchar_t* text, UINT flags = 0) { AppendMenuW(m, MF_STRING | flags, id, text); }
void Sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }
void ClearMenu(HMENU m) { while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION); }

// ------------------------------------------------------------------ formatting: menus and toolbar
// The same builders fill the toolbar dropdowns, the Format menu and the editor's context menu.
bool IsFormatCmd(int id) {
    if (id == CMD_BOLD || id == CMD_ITALIC || id == CMD_CODE || id == CMD_LINK) return true;
    return id > CMD_FMT_HEADING && id < CMD_FMT_MORE && id != CMD_FMT_CALLOUT && id != CMD_FMT_TABLE;
}

UINT Chk(bool on) { return on ? MF_CHECKED : 0; }

void FillHeadingMenu(HMENU m, const FmtState& st, UINT gray = 0) {
    Add(m, CMD_FMT_PARA, L"&Normal text\tCtrl+Shift+0", gray);
    for (int i = 1; i <= 6; i++) {
        std::wstring t = L"Heading &" + std::to_wstring(i) + L"\tCtrl+Shift+" + std::to_wstring(i);
        Add(m, CMD_FMT_H1 + i - 1, t.c_str(), gray);
    }
    if (!st.inCode) CheckMenuRadioItem(m, CMD_FMT_PARA, CMD_FMT_H1 + 5, st.heading ? CMD_FMT_H1 + st.heading - 1 : CMD_FMT_PARA, MF_BYCOMMAND);
}

void FillCalloutMenu(HMENU m, UINT gray = 0) {
    static const wchar_t* names[] = {L"&Note", L"&Tip", L"&Important", L"&Warning", L"&Caution"};
    for (int i = 0; i < 5; i++) Add(m, CMD_FMT_CALLOUT0 + i, names[i], gray);
}

void FillTableSizes(HMENU m, UINT gray = 0) {
    auto& sizes = Fmt_TableSizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        std::wstring t = std::to_wstring(sizes[i].cols) + L" columns \x00D7 " + std::to_wstring(sizes[i].rows) + L" rows";
        Add(m, CMD_FMT_TABLE0 + (UINT)i, t.c_str(), gray);
    }
}

// Outside a table: the sizes to insert. Inside one: the editing commands, with the sizes in a submenu.
void FillTableMenu(HMENU m, const FmtState& st, UINT gray = 0) {
    if (!st.inTable) {
        Add(m, 0, L"Insert a table (plus a header row):", MF_GRAYED);
        FillTableSizes(m, gray);
        Sep(m);
        Add(m, 0, L"Put the cursor in a table to add or remove rows and columns", MF_GRAYED);
        return;
    }
    HMENU sizes = CreatePopupMenu();
    FillTableSizes(sizes, gray);
    UINT g = gray;
    Add(m, CMD_FMT_ROWABOVE, L"Insert row &above", g);
    Add(m, CMD_FMT_ROWBELOW, L"Insert row &below", g);
    Add(m, CMD_FMT_COLLEFT, L"Insert column &left", g);
    Add(m, CMD_FMT_COLRIGHT, L"Insert column &right", g);
    Sep(m);
    Add(m, CMD_FMT_DELROW, L"&Delete row", g);
    Add(m, CMD_FMT_DELCOL, L"Delete colu&mn", g);
    Sep(m);
    HMENU al = CreatePopupMenu();
    Add(al, CMD_FMT_ALIGNLEFT, L"&Left");
    Add(al, CMD_FMT_ALIGNCENTER, L"&Center");
    Add(al, CMD_FMT_ALIGNRIGHT, L"&Right");
    Add(al, CMD_FMT_ALIGNNONE, L"&Default");
    AppendMenuW(m, MF_POPUP | g, (UINT_PTR)al, L"Align &column");
    Add(m, CMD_FMT_TABLETIDY, L"&Tidy up columns", g);
    Sep(m);
    AppendMenuW(m, MF_POPUP | gray, (UINT_PTR)sizes, L"&Insert new table");
}

void FillMoreMenu(HMENU m, const FmtState& st, UINT gray = 0) {
    Add(m, CMD_FMT_UNDERLINE, L"&Underline\tCtrl+U", gray);
    Add(m, CMD_FMT_KBD, L"&Keyboard key", gray);
    Add(m, CMD_FMT_MATH, L"Inline &math", gray);
    Sep(m);
    Add(m, CMD_FMT_MATHBLOCK, L"Math &block", gray);
    Add(m, CMD_FMT_FOOTNOTE, L"&Footnote", gray);
    Add(m, CMD_FMT_LINEBREAK, L"Line b&reak", gray);
    Add(m, CMD_FMT_IMAGEURL, L"Image from &web address", gray);
    Sep(m);
    Add(m, CMD_FMT_TASKDONE, st.taskDone ? L"Mark task as &not done" : L"Mark task as &done", gray);
    Sep(m);
    Add(m, CMD_FMT_CLEAR, L"&Clear formatting\tCtrl+Space", gray);
}

// Format menu (menu bar) and the Format submenu of the editor's context menu.
void FillFormatMenu(HMENU m, bool withToolbarToggle) {
    FmtState st = g_active >= 0 ? Fmt_State(g_ed) : FmtState();
    UINT gray = (g_active < 0 || g_set.viewMode == 2) ? MF_GRAYED : 0;
    HMENU h = CreatePopupMenu();
    FillHeadingMenu(h, st, gray);
    AppendMenuW(m, MF_POPUP | gray, (UINT_PTR)h, L"&Paragraph style");
    Sep(m);
    int f = st.inlineFlags;
    Add(m, CMD_BOLD, L"&Bold\tCtrl+B", gray | Chk(f & 1));
    Add(m, CMD_ITALIC, L"&Italic\tCtrl+I", gray | Chk(f & 2));
    Add(m, CMD_FMT_STRIKE, L"&Strikethrough\tCtrl+Shift+X", gray | Chk(f & 4));
    Add(m, CMD_FMT_UNDERLINE, L"&Underline\tCtrl+U", gray);
    Add(m, CMD_CODE, L"Inline c&ode\tCtrl+Shift+C", gray | Chk(f & 8));
    Add(m, CMD_FMT_KBD, L"Keyboard ke&y", gray);
    Add(m, CMD_FMT_MATH, L"Inline &math", gray);
    Add(m, CMD_FMT_CLEAR, L"Clear &formatting\tCtrl+Space", gray);
    Sep(m);
    Add(m, CMD_FMT_BULLET, L"Bulleted &list\tCtrl+Shift+8", gray | Chk(st.list == 1 && !st.task));
    Add(m, CMD_FMT_NUMBER, L"&Numbered list\tCtrl+Shift+7", gray | Chk(st.list == 2 && !st.task));
    Add(m, CMD_FMT_TASK, L"&Task list\tCtrl+Shift+9", gray | Chk(st.task));
    Add(m, CMD_FMT_TASKDONE, st.taskDone ? L"Mark task as not &done" : L"Mark task as &done", gray);
    Add(m, CMD_FMT_INDENT, L"Increase indent\tTab", gray);
    Add(m, CMD_FMT_OUTDENT, L"Decrease indent\tShift+Tab", gray);
    Sep(m);
    Add(m, CMD_FMT_QUOTE, L"&Quote\tCtrl+Shift+Q", gray | Chk(st.quote));
    HMENU co = CreatePopupMenu();
    FillCalloutMenu(co, gray);
    AppendMenuW(m, MF_POPUP | gray, (UINT_PTR)co, L"C&allout");
    Add(m, CMD_FMT_CODEBLOCK, L"&Code block\tCtrl+Shift+K", gray | Chk(st.inCode));
    Add(m, CMD_FMT_MATHBLOCK, L"Math block", gray);
    Sep(m);
    Add(m, CMD_LINK, L"Lin&k\tCtrl+L", gray);
    Add(m, CMD_FMT_IMAGE, L"Ima&ge from file...\tCtrl+Shift+I", gray);
    Add(m, CMD_FMT_IMAGEURL, L"Image from &web address", gray);
    HMENU tb = CreatePopupMenu();
    FillTableMenu(tb, st, gray);
    AppendMenuW(m, MF_POPUP | gray, (UINT_PTR)tb, L"Tabl&e");
    Add(m, CMD_FMT_HR, L"&Horizontal rule", gray);
    Add(m, CMD_FMT_FOOTNOTE, L"Footnote", gray);
    Add(m, CMD_FMT_LINEBREAK, L"Line b&reak", gray);
    if (withToolbarToggle) {
        Sep(m);
        Add(m, CMD_TOOLBAR, L"Show formatting toolbar", Chk(g_set.toolbar));
    }
}

HMENU ToolbarMenu(int cmd) {
    FmtState st = g_active >= 0 ? Fmt_State(g_ed) : FmtState();
    HMENU m = CreatePopupMenu();
    switch (cmd) {
    case CMD_FMT_HEADING: FillHeadingMenu(m, st); break;
    case CMD_FMT_CALLOUT: FillCalloutMenu(m); break;
    case CMD_FMT_TABLE: FillTableMenu(m, st); break;
    case CMD_FMT_MORE: FillMoreMenu(m, st); break;
    default: DestroyMenu(m); return nullptr;
    }
    return m;
}

std::vector<TbItem> ToolbarItems() {
    return {
        {CMD_FMT_HEADING, TB_LABELDROP, L"", TI_GLYPH, L"Paragraph style", L"", L"Normal text"},
        {0, TB_SEP},
        {CMD_BOLD, TB_BUTTON, L"\xE8DD", TI_GLYPH, L"Bold", L"Ctrl+B"},
        {CMD_ITALIC, TB_BUTTON, L"\xE8DB", TI_GLYPH, L"Italic", L"Ctrl+I"},
        {CMD_FMT_STRIKE, TB_BUTTON, L"\xEDE0", TI_GLYPH, L"Strikethrough", L"Ctrl+Shift+X"},
        {CMD_CODE, TB_BUTTON, L"", TI_INLINECODE, L"Inline code", L"Ctrl+Shift+C"},
        {0, TB_SEP},
        {CMD_FMT_BULLET, TB_BUTTON, L"\xE8FD", TI_GLYPH, L"Bulleted list", L"Ctrl+Shift+8"},
        {CMD_FMT_NUMBER, TB_BUTTON, L"", TI_NUMLIST, L"Numbered list", L"Ctrl+Shift+7"},
        {CMD_FMT_TASK, TB_BUTTON, L"\xE9D5", TI_GLYPH, L"Task list", L"Ctrl+Shift+9"},
        {CMD_FMT_OUTDENT, TB_BUTTON, L"\xE290", TI_GLYPH, L"Decrease indent", L"Shift+Tab"},
        {CMD_FMT_INDENT, TB_BUTTON, L"\xE291", TI_GLYPH, L"Increase indent", L"Tab"},
        {0, TB_SEP},
        {CMD_FMT_QUOTE, TB_BUTTON, L"\xE9B2", TI_GLYPH, L"Quote", L"Ctrl+Shift+Q"},
        {CMD_FMT_CALLOUT, TB_DROPDOWN, L"\xE946", TI_GLYPH, L"Callout (note, tip, warning...)"},
        {CMD_FMT_CODEBLOCK, TB_BUTTON, L"\xE943", TI_GLYPH, L"Code block", L"Ctrl+Shift+K"},
        {0, TB_SEP},
        {CMD_LINK, TB_BUTTON, L"\xE71B", TI_GLYPH, L"Link", L"Ctrl+L"},
        {CMD_FMT_IMAGE, TB_BUTTON, L"\xEB9F", TI_GLYPH, L"Image from file", L"Ctrl+Shift+I"},
        {CMD_FMT_TABLE, TB_DROPDOWN, L"\xE8A9", TI_GLYPH, L"Table"},
        {CMD_FMT_HR, TB_BUTTON, L"\xE949", TI_GLYPH, L"Horizontal rule"},
        {0, TB_SEP},
        {CMD_FMT_MORE, TB_DROPDOWN, L"\xE712", TI_GLYPH, L"More formatting"},
        {0, TB_SPRING},
        {CMD_AI_REWRITE, TB_TEXTBUTTON, L"\x2728", TI_SYMBOL, L"Rewrite the selection with Claude", L"Ctrl+K", L"Rewrite"},
        {0, TB_SEP},
        {CMD_VIEW_EDITOR, TB_BUTTON, L"\xE70F", TI_GLYPH, L"Editor only", L"Ctrl+1"},
        {CMD_VIEW_SPLIT, TB_BUTTON, L"", TI_SPLIT, L"Editor and preview", L"Ctrl+2"},
        {CMD_VIEW_PREVIEW, TB_BUTTON, L"\xE7B3", TI_GLYPH, L"Preview only", L"Ctrl+3"},
    };
}

void UpdateToolbar() {
    KillTimer(g_main, T_TOOLBAR);
    if (!g_toolbar || !g_set.toolbar) return;
    bool en = g_active >= 0 && g_set.viewMode != 2;
    FmtState st = en ? Fmt_State(g_ed) : FmtState();
    static const int fmt[] = {CMD_FMT_HEADING, CMD_BOLD, CMD_ITALIC, CMD_FMT_STRIKE, CMD_CODE, CMD_FMT_BULLET, CMD_FMT_NUMBER,
                              CMD_FMT_TASK, CMD_FMT_OUTDENT, CMD_FMT_INDENT, CMD_FMT_QUOTE, CMD_FMT_CALLOUT, CMD_FMT_CODEBLOCK,
                              CMD_LINK, CMD_FMT_IMAGE, CMD_FMT_TABLE, CMD_FMT_HR, CMD_FMT_MORE};
    for (int c : fmt) Toolbar_SetEnabled(g_toolbar, c, en);
    Toolbar_SetEnabled(g_toolbar, CMD_AI_REWRITE, g_active >= 0);
    int f = st.inlineFlags;
    Toolbar_SetChecked(g_toolbar, CMD_BOLD, f & 1);
    Toolbar_SetChecked(g_toolbar, CMD_ITALIC, f & 2);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_STRIKE, f & 4);
    Toolbar_SetChecked(g_toolbar, CMD_CODE, f & 8);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_BULLET, st.list == 1 && !st.task);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_NUMBER, st.list == 2 && !st.task);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_TASK, st.task);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_QUOTE, st.quote);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_CODEBLOCK, st.inCode);
    Toolbar_SetChecked(g_toolbar, CMD_FMT_TABLE, st.inTable);
    Toolbar_SetLabel(g_toolbar, CMD_FMT_HEADING,
                     st.inCode ? L"Code block" : st.heading ? L"Heading " + std::to_wstring(st.heading) : L"Normal text");
    for (int i = 0; i < 3; i++) Toolbar_SetChecked(g_toolbar, CMD_VIEW_EDITOR + i, g_set.viewMode == i);
}

void ScheduleToolbar() {
    if (g_toolbar && g_set.toolbar) SetTimer(g_main, T_TOOLBAR, 40, nullptr);
}

void RunFormat(int id) {
    if (g_active < 0) return;
    if (g_set.viewMode == 2) SetViewMode(1);
    if (GetFocus() != g_ed.hwnd) SetFocus(g_ed.hwnd);
    Fmt_Command(g_ed, id, g_main, Cur().path);
    ScheduleToolbar();
}

HMENU BuildMenus() {
    HMENU bar = CreateMenu();
    g_mFile = CreatePopupMenu();
    g_mRecent = CreatePopupMenu();
    g_mEnc = CreatePopupMenu();
    g_mEol = CreatePopupMenu();
    Add(g_mFile, CMD_NEW, L"&New tab\tCtrl+N");
    Add(g_mFile, CMD_NEWWINDOW, L"New &window\tCtrl+Shift+N");
    Add(g_mFile, CMD_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(g_mFile, MF_POPUP, (UINT_PTR)g_mRecent, L"Open &recent");
    Sep(g_mFile);
    Add(g_mFile, CMD_SAVE, L"&Save\tCtrl+S");
    Add(g_mFile, CMD_SAVEAS, L"Save &as...\tCtrl+Shift+S");
    Add(g_mFile, CMD_SAVEALL, L"Save a&ll\tCtrl+Alt+S");
    Add(g_mFile, CMD_RELOAD, L"Re&load from disk");
    Sep(g_mFile);
    AppendMenuW(g_mFile, MF_POPUP, (UINT_PTR)g_mEnc, L"&Encoding");
    AppendMenuW(g_mFile, MF_POPUP, (UINT_PTR)g_mEol, L"Line en&dings");
    Add(g_mFile, CMD_OPENFOLDER, L"Show in &folder");
    Sep(g_mFile);
    Add(g_mFile, CMD_CLOSETAB, L"&Close tab\tCtrl+W");
    Add(g_mFile, CMD_EXIT, L"E&xit\tAlt+F4");

    g_mEdit = CreatePopupMenu();
    Add(g_mEdit, CMD_UNDO, L"&Undo\tCtrl+Z");
    Add(g_mEdit, CMD_REDO, L"&Redo\tCtrl+Y");
    Sep(g_mEdit);
    Add(g_mEdit, CMD_CUT, L"Cu&t\tCtrl+X");
    Add(g_mEdit, CMD_COPY, L"&Copy\tCtrl+C");
    Add(g_mEdit, CMD_PASTE, L"&Paste\tCtrl+V");
    Add(g_mEdit, CMD_PASTEIMAGE, L"Paste &image\tAlt+V");
    Add(g_mEdit, CMD_DELETE, L"&Delete\tDel");
    Add(g_mEdit, CMD_SELECTALL, L"Select &all\tCtrl+A");
    Sep(g_mEdit);
    Add(g_mEdit, CMD_FIND, L"&Find...\tCtrl+F");
    Add(g_mEdit, CMD_FINDNEXT, L"Find &next\tF3");
    Add(g_mEdit, CMD_FINDPREV, L"Find pre&vious\tShift+F3");
    Add(g_mEdit, CMD_REPLACE, L"R&eplace...\tCtrl+H");
    Add(g_mEdit, CMD_GOTO, L"&Go to line...\tCtrl+G");

    g_mFormat = CreatePopupMenu();   // filled when opened
    Add(g_mFormat, CMD_BOLD, L"&Bold\tCtrl+B");

    g_mView = CreatePopupMenu();
    g_mTheme = CreatePopupMenu();
    Add(g_mView, CMD_VIEW_EDITOR, L"&Editor only\tCtrl+1");
    Add(g_mView, CMD_VIEW_SPLIT, L"&Split: editor + preview\tCtrl+2");
    Add(g_mView, CMD_VIEW_PREVIEW, L"&Preview only\tCtrl+3");
    Add(g_mView, CMD_VIEW_TOGGLE, L"&Toggle preview\tCtrl+E");
    Sep(g_mView);
    Add(g_mView, CMD_TOOLBAR, L"&Formatting toolbar");
    Add(g_mView, CMD_WRAP, L"&Word wrap\tAlt+Z");
    Add(g_mView, CMD_LINENUMBERS, L"&Line numbers");
    Add(g_mView, CMD_LIMITWIDTH, L"&Comfortable line width");
    Sep(g_mView);
    Add(g_mView, CMD_ZOOMIN, L"Zoom &in\tCtrl++");
    Add(g_mView, CMD_ZOOMOUT, L"Zoom &out\tCtrl+-");
    Add(g_mView, CMD_ZOOMRESET, L"&Reset zoom\tCtrl+0");
    Sep(g_mView);
    Add(g_mTheme, CMD_THEME_SYSTEM, L"&Same as Windows");
    Add(g_mTheme, CMD_THEME_LIGHT, L"&Light");
    Add(g_mTheme, CMD_THEME_DARK, L"&Dark");
    AppendMenuW(g_mView, MF_POPUP, (UINT_PTR)g_mTheme, L"T&heme");
    Add(g_mView, CMD_FONT, L"Editor &font...");
    Sep(g_mView);
    Add(g_mView, CMD_NEXTTAB, L"Ne&xt tab\tCtrl+Tab");
    Add(g_mView, CMD_PREVTAB, L"Previous t&ab\tCtrl+Shift+Tab");
    Sep(g_mView);
    Add(g_mView, CMD_RESTORESESSION, L"Reopen &files on startup");

    g_mSpell = CreatePopupMenu();
    Add(g_mSpell, CMD_SPELL_TOGGLE, L"&Check spelling\tF7");   // rebuilt on open

    g_mAi = CreatePopupMenu();
    Add(g_mAi, CMD_AI_REWRITE, L"&Rewrite selection with Claude...\tCtrl+K");
    Sep(g_mAi);
    Add(g_mAi, CMD_AI_SETTINGS, L"Claude &settings (API key, model)...");

    g_mHelp = CreatePopupMenu();
    Add(g_mHelp, CMD_MDHELP, L"&Markdown guide");
    Add(g_mHelp, CMD_ABOUT, L"&About " APP_NAME);

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mFile, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mEdit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mFormat, L"F&ormat");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mView, L"V&iew");   // Alt+V pastes an image
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mSpell, L"&Spelling");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mAi, L"&Claude");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_mHelp, L"&Help");
    return bar;
}

void BuildSpellMenu(HMENU m) {
    ClearMenu(m);
    Add(m, CMD_SPELL_TOGGLE, L"&Check spelling\tF7", g_set.spell ? MF_CHECKED : 0);
    Add(m, CMD_SPELL_CAPS, L"Ignore words in &UPPERCASE", g_set.spellIgnoreCaps ? MF_CHECKED : 0);
    Sep(m);
    auto& sup = Spell::Supported();
    if (sup.empty()) Add(m, 0, L"No spell checkers are installed in Windows", MF_GRAYED);
    else Add(m, 0, L"Languages (a word is accepted if any checked language knows it)", MF_GRAYED);
    for (size_t i = 0; i < sup.size() && i < 190; i++) {
        bool on = std::find(g_set.spellLangs.begin(), g_set.spellLangs.end(), sup[i].tag) != g_set.spellLangs.end();
        std::wstring t = sup[i].name + L"\t" + sup[i].tag;
        Add(m, CMD_SPELL_LANG0 + (UINT)i, t.c_str(), on ? MF_CHECKED : 0);
    }
    Sep(m);
    Add(m, CMD_SPELL_WINLANG, L"Add more languages in Windows Settings...");
}

void BuildRecentMenu() {
    ClearMenu(g_mRecent);
    int n = 0;
    for (auto& r : g_set.recent) {
        if (n >= 20) break;
        std::wstring path;   // "&" in a path (R&D) must show as "&", not underline the next letter
        for (wchar_t c : r) { path += c; if (c == L'&') path += L'&'; }
        std::wstring t = (n < 9 ? L"&" + std::to_wstring(n + 1) + L"  " : L"    ") + path;
        Add(g_mRecent, CMD_RECENT0 + n, t.c_str());
        n++;
    }
    if (!n) Add(g_mRecent, 0, L"(empty)", MF_GRAYED);
    else {
        Sep(g_mRecent);
        Add(g_mRecent, CMD_CLEARRECENT, L"&Clear list");
    }
}

void BuildEncMenu() {
    ClearMenu(g_mEnc);
    int enc = g_active >= 0 ? Cur().enc : ENC_UTF8;
    if (enc == ENC_ANSI || enc == ENC_UTF16BE) {
        std::wstring t = std::wstring(kEncNames[enc]) + L" (current)";
        Add(g_mEnc, 0, t.c_str(), MF_CHECKED | MF_GRAYED);
        Sep(g_mEnc);
    }
    Add(g_mEnc, CMD_ENC_UTF8, L"UTF-8", enc == ENC_UTF8 ? MF_CHECKED : 0);
    Add(g_mEnc, CMD_ENC_UTF8BOM, L"UTF-8 with BOM", enc == ENC_UTF8BOM ? MF_CHECKED : 0);
    Add(g_mEnc, CMD_ENC_UTF16LE, L"UTF-16 LE", enc == ENC_UTF16LE ? MF_CHECKED : 0);
}

void BuildEolMenu() {
    ClearMenu(g_mEol);
    int eol = g_active >= 0 ? Cur().eol : SC_EOL_CRLF;
    Add(g_mEol, CMD_EOL_CRLF, L"Windows (CRLF)", eol == SC_EOL_CRLF ? MF_CHECKED : 0);
    Add(g_mEol, CMD_EOL_LF, L"Unix / macOS (LF)", eol == SC_EOL_LF ? MF_CHECKED : 0);
}

void OnInitMenuPopup(HMENU m) {
    HWND f = GetFocus();
    bool inEditor = f == g_ed.hwnd;
    if (m == g_mRecent) BuildRecentMenu();
    else if (m == g_mSpell) BuildSpellMenu(m);
    else if (m == g_mEnc) BuildEncMenu();
    else if (m == g_mEol) BuildEolMenu();
    else if (m == g_mFormat) { ClearMenu(m); FillFormatMenu(m, true); }
    else if (m == g_mFile) {
        bool hasPath = g_active >= 0 && !Cur().path.empty();
        EnableMenuItem(m, CMD_RELOAD, hasPath ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, CMD_OPENFOLDER, hasPath ? MF_ENABLED : MF_GRAYED);
    } else if (m == g_mEdit) {
        bool sel = g_ed(SCI_GETSELECTIONEMPTY) == 0;
        auto en = [&](UINT id, bool on) { EnableMenuItem(m, id, (on || !inEditor) ? MF_ENABLED : MF_GRAYED); };
        en(CMD_UNDO, g_ed(SCI_CANUNDO) != 0);
        en(CMD_REDO, g_ed(SCI_CANREDO) != 0);
        en(CMD_CUT, sel);
        en(CMD_COPY, sel);
        en(CMD_DELETE, sel);
        bool picture = !g_ed(SCI_GETREADONLY) && Clip_HasPicture();
        en(CMD_PASTE, g_ed(SCI_CANPASTE) != 0 || picture);
        EnableMenuItem(m, CMD_PASTEIMAGE, picture && g_active >= 0 ? MF_ENABLED : MF_GRAYED);
    } else if (m == g_mView) {
        CheckMenuRadioItem(m, CMD_VIEW_EDITOR, CMD_VIEW_PREVIEW, CMD_VIEW_EDITOR + g_set.viewMode, MF_BYCOMMAND);
        CheckMenuItem(m, CMD_WRAP, g_set.wrap ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(m, CMD_LINENUMBERS, g_set.lineNumbers ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(m, CMD_LIMITWIDTH, g_set.limitWidth ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(m, CMD_TOOLBAR, g_set.toolbar ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(m, CMD_RESTORESESSION, g_set.restoreSession ? MF_CHECKED : MF_UNCHECKED);
    } else if (m == g_mTheme) {
        CheckMenuRadioItem(m, CMD_THEME_SYSTEM, CMD_THEME_DARK, CMD_THEME_SYSTEM + g_set.theme, MF_BYCOMMAND);
    }
}

void PopupAt(HMENU m, const RECT& anchorClient) {
    POINT pt{anchorClient.left, anchorClient.top};
    ClientToScreen(g_main, &pt);
    SendMessageW(g_main, WM_INITMENUPOPUP, (WPARAM)m, 0);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
}

void BuildAccelerators() {
    const BYTE C = FCONTROL | FVIRTKEY, CS = FCONTROL | FSHIFT | FVIRTKEY, V = FVIRTKEY;
    std::vector<ACCEL> g = {
        {C, 'N', CMD_NEW}, {CS, 'N', CMD_NEWWINDOW}, {C, 'O', CMD_OPEN}, {C, 'S', CMD_SAVE}, {CS, 'S', CMD_SAVEAS},
        {(BYTE)(FCONTROL | FALT | FVIRTKEY), 'S', CMD_SAVEALL}, {C, 'W', CMD_CLOSETAB}, {C, VK_F4, CMD_CLOSETAB},
        {C, 'F', CMD_FIND}, {V, VK_F3, CMD_FINDNEXT}, {(BYTE)(FSHIFT | FVIRTKEY), VK_F3, CMD_FINDPREV}, {C, 'H', CMD_REPLACE},
        {C, 'G', CMD_GOTO}, {C, '1', CMD_VIEW_EDITOR}, {C, '2', CMD_VIEW_SPLIT}, {C, '3', CMD_VIEW_PREVIEW},
        {C, 'E', CMD_VIEW_TOGGLE}, {(BYTE)(FALT | FVIRTKEY), 'Z', CMD_WRAP}, {C, VK_OEM_PLUS, CMD_ZOOMIN}, {C, VK_ADD, CMD_ZOOMIN},
        {CS, VK_OEM_PLUS, CMD_ZOOMIN}, {C, VK_OEM_MINUS, CMD_ZOOMOUT}, {C, VK_SUBTRACT, CMD_ZOOMOUT}, {C, '0', CMD_ZOOMRESET},
        {C, VK_NUMPAD0, CMD_ZOOMRESET}, {C, VK_TAB, CMD_NEXTTAB}, {CS, VK_TAB, CMD_PREVTAB}, {C, VK_NEXT, CMD_NEXTTAB},
        {C, VK_PRIOR, CMD_PREVTAB}, {V, VK_F7, CMD_SPELL_TOGGLE}, {C, 'K', CMD_AI_REWRITE},
    };
    g_accGlobal = CreateAcceleratorTableW(g.data(), (int)g.size());
    g.push_back({C, 'B', CMD_BOLD});
    g.push_back({C, 'I', CMD_ITALIC});
    g.push_back({CS, 'C', CMD_CODE});
    g.push_back({C, 'L', CMD_LINK});
    g.push_back({C, 'U', CMD_FMT_UNDERLINE});
    g.push_back({CS, 'X', CMD_FMT_STRIKE});
    g.push_back({C, VK_SPACE, CMD_FMT_CLEAR});
    g.push_back({CS, '0', CMD_FMT_PARA});
    for (int i = 1; i <= 6; i++) g.push_back({CS, (WORD)('0' + i), (WORD)(CMD_FMT_H1 + i - 1)});
    g.push_back({CS, '7', CMD_FMT_NUMBER});
    g.push_back({CS, '8', CMD_FMT_BULLET});
    g.push_back({CS, '9', CMD_FMT_TASK});
    g.push_back({CS, 'Q', CMD_FMT_QUOTE});
    g.push_back({CS, 'K', CMD_FMT_CODEBLOCK});
    g.push_back({CS, 'I', CMD_FMT_IMAGE});
    g.push_back({V, VK_TAB, CMD_FMT_INDENT});
    g.push_back({(BYTE)(FSHIFT | FVIRTKEY), VK_TAB, CMD_FMT_OUTDENT});
    g.push_back({C, 'V', CMD_PASTE});   // through PasteIntoEditor, so screenshots paste too
    g.push_back({(BYTE)(FSHIFT | FVIRTKEY), VK_INSERT, CMD_PASTE});
    g.push_back({(BYTE)(FALT | FVIRTKEY), 'V', CMD_PASTEIMAGE});
    g_accFull = CreateAcceleratorTableW(g.data(), (int)g.size());
}

// ------------------------------------------------------------------ context menus
void EditorContextMenu(LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    sptr_t pos;
    if (pt.x == -1 && pt.y == -1) {
        pos = g_ed(SCI_GETCURRENTPOS);
        pt.x = (LONG)g_ed(SCI_POINTXFROMPOSITION, 0, pos);
        pt.y = (LONG)(g_ed(SCI_POINTYFROMPOSITION, 0, pos) + g_ed(SCI_TEXTHEIGHT, 0));
        ClientToScreen(g_ed.hwnd, &pt);
    } else {
        POINT c = pt;
        ScreenToClient(g_ed.hwnd, &c);
        pos = g_ed(SCI_POSITIONFROMPOINTCLOSE, c.x, c.y);
        sptr_t a = g_ed(SCI_GETSELECTIONSTART), b = g_ed(SCI_GETSELECTIONEND);
        if (pos >= 0 && (pos < a || pos > b)) g_ed(SCI_SETEMPTYSELECTION, pos);
    }
    HMENU m = CreatePopupMenu();
    HMENU addMenu = nullptr;
    std::vector<std::wstring> sugg;
    sptr_t wa = 0, wb = 0;
    std::wstring word;
    if (g_set.spell && pos >= 0 && Spell::MisspelledAt(g_ed, pos, wa, wb)) {
        word = U8ToW(g_ed.Range(wa, wb));
        sugg = Spell::Suggest(word, 7);
        if (sugg.empty()) Add(m, 0, L"(no suggestions)", MF_GRAYED);
        for (size_t i = 0; i < sugg.size(); i++) Add(m, CMD_SPELL_SUGG0 + (UINT)i, sugg[i].c_str());
        if (!sugg.empty()) SetMenuDefaultItem(m, CMD_SPELL_SUGG0, FALSE);
        Sep(m);
        auto& act = Spell::Active();
        if (act.size() == 1) Add(m, CMD_SPELL_ADD0, (L"Add to dictionary (" + act[0] + L")").c_str());
        else if (act.size() > 1) {
            addMenu = CreatePopupMenu();
            for (size_t i = 0; i < act.size() && i < 40; i++) Add(addMenu, CMD_SPELL_ADD0 + (UINT)i, Spell::NameOf(act[i]).c_str());
            AppendMenuW(m, MF_POPUP, (UINT_PTR)addMenu, L"Add to dictionary");
        }
        Add(m, CMD_SPELL_IGNORE, L"Ignore all");
        Sep(m);
    }
    Add(m, CMD_AI_REWRITE, L"Rewrite with Claude...\tCtrl+K");
    Sep(m);
    bool sel = g_ed(SCI_GETSELECTIONEMPTY) == 0;
    Add(m, CMD_UNDO, L"Undo\tCtrl+Z", g_ed(SCI_CANUNDO) ? 0 : MF_GRAYED);
    Add(m, CMD_REDO, L"Redo\tCtrl+Y", g_ed(SCI_CANREDO) ? 0 : MF_GRAYED);
    Sep(m);
    Add(m, CMD_CUT, L"Cut\tCtrl+X", sel ? 0 : MF_GRAYED);
    Add(m, CMD_COPY, L"Copy\tCtrl+C", sel ? 0 : MF_GRAYED);
    bool picture = !g_ed(SCI_GETREADONLY) && Clip_HasPicture();
    Add(m, CMD_PASTE, L"Paste\tCtrl+V", g_ed(SCI_CANPASTE) || picture ? 0 : MF_GRAYED);
    if (picture) Add(m, CMD_PASTEIMAGE, L"Paste image\tAlt+V");
    Sep(m);
    Add(m, CMD_BOLD, L"Bold\tCtrl+B");
    Add(m, CMD_ITALIC, L"Italic\tCtrl+I");
    Add(m, CMD_LINK, L"Link\tCtrl+L");
    HMENU fmt = CreatePopupMenu();
    FillFormatMenu(fmt, false);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fmt, L"Format");
    FmtState st = Fmt_State(g_ed);
    if (st.inTable) {
        HMENU tb = CreatePopupMenu();
        FillTableMenu(tb, st);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)tb, L"Table");
    }
    Sep(m);
    Add(m, CMD_SELECTALL, L"Select all\tCtrl+A");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd >= CMD_SPELL_SUGG0 && cmd < CMD_SPELL_SUGG0 + (int)sugg.size()) {
        std::string r = WToU8(sugg[cmd - CMD_SPELL_SUGG0]);
        g_ed(SCI_SETTARGETRANGE, wa, wb);
        g_ed(SCI_REPLACETARGET, r.size(), (sptr_t)r.data());
        g_ed(SCI_GOTOPOS, wa + (sptr_t)r.size());
        ScheduleSpell(10);
    } else if (cmd >= CMD_SPELL_ADD0 && cmd < CMD_SPELL_ADD0 + 40 && !word.empty()) {
        Spell::Add(word, (size_t)(cmd - CMD_SPELL_ADD0));
        ScheduleSpell(10);
    } else if (cmd == CMD_SPELL_IGNORE && !word.empty()) {
        Spell::Ignore(word);
        ScheduleSpell(10);
    } else {
        SetFocus(g_ed.hwnd);
        SendMessageW(g_main, WM_COMMAND, (WPARAM)cmd, 0);
    }
}

void TabContextMenu(int idx) {
    if (idx < 0 || idx >= (int)g_docs.size()) return;
    bool hasPath = !g_docs[idx]->path.empty();
    HMENU m = CreatePopupMenu();
    Add(m, 1, L"Close\tCtrl+W");
    Add(m, 2, L"Close other tabs", g_docs.size() > 1 ? 0 : MF_GRAYED);
    Add(m, 3, L"Close tabs to the right", idx + 1 < (int)g_docs.size() ? 0 : MF_GRAYED);
    Sep(m);
    Add(m, 4, L"Copy file path", hasPath ? 0 : MF_GRAYED);
    Add(m, 5, L"Show in folder", hasPath ? 0 : MF_GRAYED);
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    int id = g_docs[idx]->id;
    auto indexOf = [&](int docId) {
        for (size_t i = 0; i < g_docs.size(); i++) if (g_docs[i]->id == docId) return (int)i;
        return -1;
    };
    switch (cmd) {
    case 1: CloseDoc(idx); break;
    case 2:
        for (int i = (int)g_docs.size() - 1; i >= 0; i--)
            if (g_docs[i]->id != id && !CloseDoc(i)) break;
        break;
    case 3:
        for (int i = (int)g_docs.size() - 1; i > indexOf(id); i--)
            if (!CloseDoc(i)) break;
        break;
    case 4:
        if (OpenClipboard(g_main)) {
            const std::wstring& p = g_docs[idx]->path;
            EmptyClipboard();
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (p.size() + 1) * sizeof(wchar_t));
            if (g) {
                memcpy(GlobalLock(g), p.c_str(), (p.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
            }
            CloseClipboard();
        }
        break;
    case 5: RevealInExplorer(g_docs[idx]->path); break;
    }
}

// ------------------------------------------------------------------ paste (text, screenshots, image files)
// Ctrl+V pastes text when the clipboard has any, otherwise a picture. Alt+V (preferImage) takes
// the picture first, for programs such as Excel that copy both. A picture is saved as a PNG in an
// "images" folder next to the document and a link to it is inserted; image files copied in
// File Explorer are linked where they are.
void PasteIntoEditor(bool preferImage) {
    if (g_active < 0 || g_set.viewMode == 2) return;
    bool wantPicture = preferImage || !Clip_HasText();
    std::vector<std::wstring> files = wantPicture ? Clip_ImageFiles() : std::vector<std::wstring>();
    bool image = wantPicture && files.empty() && Clip_HasImage();
    if (files.empty() && !image) { g_ed(SCI_PASTE); return; }
    if (g_ed(SCI_GETREADONLY)) return;
    if (!files.empty()) { Fmt_PasteImages(g_ed, files, Cur().path); return; }
    if (Cur().path.empty() &&
        !SaveDoc(g_active, true, L"Save the document first - the pasted image is stored in an images folder next to it")) {
        App_Status(L"Image not pasted: the document has to be saved first.", false);
        return;
    }
    std::wstring err, file = Clip_SaveImage(g_main, DirOf(Cur().path) + L"\\images", err);
    if (file.empty()) { MsgError(L"Couldn't paste the image.\n\n" + err); return; }
    Fmt_PasteImages(g_ed, {file}, Cur().path, "image");
    App_Status(L"Image saved as images\\" + file.substr(file.find_last_of(L'\\') + 1), false);
}

// ------------------------------------------------------------------ commands
void EditCommand(int cmd) {
    HWND f = GetFocus();
    if (f && f != g_ed.hwnd && IsClass(f, L"Edit")) {
        switch (cmd) {
        case CMD_UNDO: SendMessageW(f, WM_UNDO, 0, 0); break;
        case CMD_CUT: SendMessageW(f, WM_CUT, 0, 0); break;
        case CMD_COPY: SendMessageW(f, WM_COPY, 0, 0); break;
        case CMD_PASTE: SendMessageW(f, WM_PASTE, 0, 0); break;
        case CMD_DELETE: SendMessageW(f, WM_CLEAR, 0, 0); break;
        case CMD_SELECTALL: SendMessageW(f, EM_SETSEL, 0, -1); break;
        }
        return;
    }
    if (f == g_preview) {
        if (cmd == CMD_COPY) Preview_Copy(g_preview);
        else if (cmd == CMD_SELECTALL) Preview_SelectAll(g_preview);
        return;
    }
    HWND target = (f && f != g_ed.hwnd && IsClass(f, L"Scintilla")) ? f : g_ed.hwnd;
    if (cmd == CMD_PASTE && target == g_ed.hwnd) { PasteIntoEditor(false); return; }
    UINT m = 0;
    switch (cmd) {
    case CMD_UNDO: m = SCI_UNDO; break;
    case CMD_REDO: m = SCI_REDO; break;
    case CMD_CUT: m = SCI_CUT; break;
    case CMD_COPY: m = SCI_COPY; break;
    case CMD_PASTE: m = SCI_PASTE; break;
    case CMD_DELETE: m = SCI_CLEAR; break;
    case CMD_SELECTALL: m = SCI_SELECTALL; break;
    }
    if (m) SendMessageW(target, m, 0, 0);
}

void Zoom(int dir) {
    HWND f = GetFocus();
    if (g_set.viewMode == 2 || f == g_preview) {
        float z = Preview_GetZoom(g_preview);
        Preview_SetZoom(g_preview, dir == 0 ? 1.0f : dir > 0 ? z * 1.1f : z / 1.1f);
        return;
    }
    if (dir > 0) g_ed(SCI_ZOOMIN);
    else if (dir < 0) g_ed(SCI_ZOOMOUT);
    else g_ed(SCI_SETZOOM, 0);
}

void ChooseEditorFont() {
    LOGFONTW lf{};
    wcsncpy(lf.lfFaceName, g_set.font.c_str(), LF_FACESIZE - 1);
    HDC dc = GetDC(g_main);
    lf.lfHeight = -MulDiv(g_set.fontSize, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(g_main, dc);
    lf.lfCharSet = DEFAULT_CHARSET;
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_main;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_NOVERTFONTS | CF_LIMITSIZE;
    cf.nSizeMin = 6;
    cf.nSizeMax = 48;
    if (!ChooseFontW(&cf)) return;
    g_set.font = lf.lfFaceName;
    g_set.fontSize = std::max(6, cf.iPointSize / 10);
    ConfigureEditor();
    AiPanel_FontChanged();
}

void ToggleSpellLang(size_t i) {
    auto& sup = Spell::Supported();
    if (i >= sup.size()) return;
    auto& v = g_set.spellLangs;
    auto it = std::find(v.begin(), v.end(), sup[i].tag);
    if (it != v.end()) v.erase(it);
    else v.push_back(sup[i].tag);
    g_set.spellLangsInit = true;
    Spell::SetLanguages(v);
    if (!v.empty()) g_set.spell = true;
    Spell::Clear(g_ed);
    ScheduleSpell(0);
    InvalidateStatus();
}

void SetEncoding(int enc) {
    if (g_active < 0 || Cur().enc == enc) return;
    Cur().enc = enc;
    Cur().forceDirty = true;
    UpdateTabs();
    UpdateTitle();
    InvalidateStatus();
    App_Status(L"Encoding set to " + std::wstring(kEncNames[enc]) + L" - save to apply");
}

void SetEol(int eol) {
    if (g_active < 0) return;
    Cur().eol = eol;
    Cur().eolConverted = true;
    g_ed(SCI_SETEOLMODE, eol);
    g_ed(SCI_CONVERTEOLS, eol);
    InvalidateStatus();
}

const char* kGuide =
    "# Markdown guide\n\n"
    "MarkLite shows the formatted result in the preview on the right (**Ctrl+2**). "
    "Press **Ctrl+1** for the editor only and **Ctrl+3** for the preview only.\n\n"
    "Everything below can also be done with the formatting toolbar, the **Format** menu or the "
    "right-click menu: select some text (or just put the cursor in a word or line) and click.\n\n"
    "## Text\n\n"
    "*Italic* (Ctrl+I), **bold** (Ctrl+B), ***both***, ~~strikethrough~~ (Ctrl+Shift+X), "
    "<u>underline</u> (Ctrl+U), `inline code` (Ctrl+Shift+C) and <kbd>keys</kbd>. "
    "Ctrl+Space removes formatting.\n\n"
    "## Headings\n\n"
    "Start a line with `#` for a level-1 heading, `##` for level 2, and so on up to `######` "
    "(Ctrl+Shift+1 to 6; Ctrl+Shift+0 for normal text).\n\n"
    "## Lists\n\n"
    "- Bullet item (press Enter to continue the list, Enter twice to end it)\n"
    "- Another item\n"
    "  - Nested item (Tab nests an item, Shift+Tab moves it back out)\n\n"
    "1. Numbered\n"
    "2. Items\n\n"
    "- [x] Done task\n"
    "- [ ] Open task\n\n"
    "## Links and images\n\n"
    "[A link](https://www.markdownguide.org) (Ctrl+L) and an image: `![alt text](picture.png)` - local images are shown in the preview. "
    "Ctrl+Shift+I picks an image file, and you can also drop image files onto the editor. "
    "A screenshot (Win+Shift+S) or a copied picture can be pasted with Ctrl+V or Alt+V: it is saved "
    "as a PNG in an `images` folder next to the document.\n\n"
    "## Quotes and callouts\n\n"
    "> A block quote.\n\n"
    "> [!TIP]\n"
    "> GitHub-style callouts: NOTE, TIP, IMPORTANT, WARNING and CAUTION.\n\n"
    "## Code\n\n"
    "```python\n"
    "def hello(name):\n"
    "    print(f\"Hello {name}\")\n"
    "```\n\n"
    "## Tables\n\n"
    "| Shortcut | Action |\n"
    "|:---------|:-------|\n"
    "| Ctrl+K | Rewrite the selection with Claude |\n"
    "| F7 | Spell checking on/off |\n"
    "| Ctrl+F / Ctrl+H | Find / replace |\n"
    "| Ctrl+E | Toggle the preview |\n\n"
    "With the cursor in a table, the toolbar's table button adds or removes rows and columns, "
    "aligns a column and lines the columns up.\n\n"
    "---\n\n"
    "Footnotes work too.[^1]\n\n"
    "[^1]: Like this one.\n";

void ShowGuide() {
    std::string text = kGuide;
    std::string crlf;
    for (char c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
    int idx = NewUntitled(crlf);
    g_docs[idx]->name = L"Markdown guide";
    Activate(idx);
    if (g_set.viewMode == 0) SetViewMode(1);
}

void LaunchNewWindow() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    ShellExecuteW(nullptr, L"open", exe, L"--new", nullptr, SW_SHOWNORMAL);
}

void OnCommand(int id) {
    if (id >= CMD_RECENT0 && id < CMD_RECENT0 + 20) {
        size_t i = (size_t)(id - CMD_RECENT0);
        if (i < g_set.recent.size()) {
            std::wstring p = g_set.recent[i];
            if (OpenFile(p, true, true) < 0) {
                Settings_RemoveRecent(p);
                MsgError(L"Couldn't open\n" + p + L"\n\nIt has been removed from the recent list.");
            }
        }
        return;
    }
    if (id >= CMD_SPELL_LANG0 && id < CMD_SPELL_LANG0 + 190) { ToggleSpellLang((size_t)(id - CMD_SPELL_LANG0)); return; }
    if (IsFormatCmd(id)) { RunFormat(id); return; }
    switch (id) {
    case CMD_NEW: Activate(NewUntitled()); SetFocus(g_ed.hwnd); break;
    case CMD_NEWWINDOW: LaunchNewWindow(); break;
    case CMD_OPEN: OpenDialog(); break;
    case CMD_SAVE: if (g_active >= 0) SaveDoc(g_active, false); break;
    case CMD_SAVEAS: if (g_active >= 0) SaveDoc(g_active, true); break;
    case CMD_SAVEALL:
        for (size_t i = 0; i < g_docs.size(); i++)
            if (Dirty(*g_docs[i]) && !SaveDoc((int)i, false)) break;
        break;
    case CMD_RELOAD:
        if (g_active >= 0 && !Cur().path.empty()) {
            if (Dirty(Cur()) && MessageBoxW(g_main, L"Discard your changes and reload the file from disk?", APP_NAME, MB_OKCANCEL | MB_ICONWARNING) != IDOK) break;
            if (ReloadDoc(g_active)) App_Status(L"Reloaded from disk");
        }
        break;
    case CMD_OPENFOLDER: if (g_active >= 0 && !Cur().path.empty()) RevealInExplorer(Cur().path); break;
    case CMD_CLOSETAB: CloseDoc(g_active); break;
    case CMD_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case CMD_CLEARRECENT: Settings_ClearRecent(); break;

    case CMD_UNDO: case CMD_REDO: case CMD_CUT: case CMD_COPY: case CMD_PASTE: case CMD_DELETE: case CMD_SELECTALL:
        EditCommand(id);
        break;
    case CMD_PASTEIMAGE: PasteIntoEditor(true); break;
    case CMD_FIND: Find_Show(false); break;
    case CMD_REPLACE: Find_Show(true); break;
    case CMD_FINDNEXT: Find_Next(false); break;
    case CMD_FINDPREV: Find_Next(true); break;
    case CMD_GOTO: {
        if (g_set.viewMode == 2) SetViewMode(1);
        sptr_t cur = g_ed(SCI_LINEFROMPOSITION, g_ed(SCI_GETCURRENTPOS)) + 1, n = g_ed(SCI_GETLINECOUNT);
        int line = Dlg_GotoLine(g_main, (int)cur, (int)n);
        if (line > 0) {
            sptr_t l = std::min<sptr_t>(line, n) - 1;
            g_ed(SCI_GOTOLINE, l);
            g_ed(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, g_ed(SCI_VISIBLEFROMDOCLINE, l) - g_ed(SCI_LINESONSCREEN) / 3));
        }
        SetFocus(g_ed.hwnd);
        break;
    }
    case CMD_TOOLBAR:
        g_set.toolbar = !g_set.toolbar;
        ShowWindow(g_toolbar, g_set.toolbar ? SW_SHOWNA : SW_HIDE);
        Layout();
        UpdateToolbar();
        break;

    case CMD_VIEW_EDITOR: SetViewMode(0); break;
    case CMD_VIEW_SPLIT: SetViewMode(1); break;
    case CMD_VIEW_PREVIEW: SetViewMode(2); break;
    case CMD_VIEW_TOGGLE: SetViewMode(g_set.viewMode == 0 ? (g_set.lastPreviewMode ? g_set.lastPreviewMode : 1) : 0); break;
    case CMD_WRAP:
        g_set.wrap = !g_set.wrap;
        g_ed(SCI_SETWRAPMODE, g_set.wrap ? SC_WRAP_WORD : SC_WRAP_NONE);
        g_ed(SCI_SETHSCROLLBAR, !g_set.wrap);
        break;
    case CMD_LINENUMBERS: g_set.lineNumbers = !g_set.lineNumbers; UpdateLineNumbers(); UpdateEditorMargins(); break;
    case CMD_LIMITWIDTH: g_set.limitWidth = !g_set.limitWidth; UpdateEditorMargins(); break;
    case CMD_ZOOMIN: Zoom(1); break;
    case CMD_ZOOMOUT: Zoom(-1); break;
    case CMD_ZOOMRESET: Zoom(0); break;
    case CMD_THEME_SYSTEM: case CMD_THEME_LIGHT: case CMD_THEME_DARK:
        g_set.theme = id - CMD_THEME_SYSTEM;
        ApplyTheme();
        break;
    case CMD_FONT: ChooseEditorFont(); break;
    case CMD_NEXTTAB: if (g_docs.size() > 1) Activate((g_active + 1) % (int)g_docs.size()); break;
    case CMD_PREVTAB: if (g_docs.size() > 1) Activate((g_active + (int)g_docs.size() - 1) % (int)g_docs.size()); break;
    case CMD_RESTORESESSION: g_set.restoreSession = !g_set.restoreSession; break;

    case CMD_SPELL_TOGGLE:
        g_set.spell = !g_set.spell;
        if (g_set.spell) ScheduleSpell(0);
        else Spell::Clear(g_ed);
        App_Status(g_set.spell ? L"Spell checking on" : L"Spell checking off");
        InvalidateStatus();
        break;
    case CMD_SPELL_CAPS: g_set.spellIgnoreCaps = !g_set.spellIgnoreCaps; ScheduleSpell(0); break;
    case CMD_SPELL_WINLANG: ShellExecuteW(g_main, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL); break;

    case CMD_AI_REWRITE:
        if (g_set.viewMode == 2) SetViewMode(1);
        AiPanel_Begin();
        break;
    case CMD_AI_SETTINGS: App_OpenSettings(); break;

    case CMD_EOL_CRLF: SetEol(SC_EOL_CRLF); break;
    case CMD_EOL_LF: SetEol(SC_EOL_LF); break;
    case CMD_ENC_UTF8: SetEncoding(ENC_UTF8); break;
    case CMD_ENC_UTF8BOM: SetEncoding(ENC_UTF8BOM); break;
    case CMD_ENC_UTF16LE: SetEncoding(ENC_UTF16LE); break;

    case CMD_MDHELP: ShowGuide(); break;
    case CMD_ABOUT:
        MessageBoxW(g_main,
                    APP_NAME L" " APP_VERSION L"\nA lightweight Markdown editor for Windows.\n\n"
                    L"Editor: Scintilla 5.6.9\nMarkdown parser: md4c 0.6.0\nSpelling: Windows spell checker\n"
                    L"AI rewriting: Claude (Anthropic API)",
                    L"About " APP_NAME, MB_ICONINFORMATION);
        break;
    }
}

// ------------------------------------------------------------------ editor notifications
void OnEditorNotify(SCNotification* n) {
    switch (n->nmhdr.code) {
    case SCN_STYLENEEDED: Md_StyleNeeded(g_ed, n->position); break;
    case SCN_MODIFIED:
        if (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
            SchedulePreview();
            ScheduleSpell(n->length > 1000 ? 50 : 350);
            ScheduleWords();
            if (n->linesAdded) UpdateLineNumbers();
            if (Find_Visible()) SetTimer(g_main, T_FIND, 300, nullptr);
            // Undoing a line-ending conversion brings the old line endings back: follow them, so new
            // lines and the status bar match the text again.
            if ((n->modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) && (n->modificationType & SC_LASTSTEPINUNDOREDO) &&
                g_active >= 0 && Cur().eolConverted) {
                int eol = DetectEol(g_ed.Range(0, std::min<sptr_t>(g_ed.Len(), 1 << 16)));
                if (g_ed(SCI_GETLINECOUNT) > 1 && eol != Cur().eol) {
                    Cur().eol = eol;
                    g_ed(SCI_SETEOLMODE, eol);
                    InvalidateStatus();
                }
            }
        }
        break;
    case SCN_SAVEPOINTREACHED:
    case SCN_SAVEPOINTLEFT:
        if (g_active >= 0) {
            Cur().modified = n->nmhdr.code == SCN_SAVEPOINTLEFT;
            UpdateTabs();
            UpdateTitle();
        }
        break;
    case SCN_UPDATEUI:
        if (n->updated & SC_UPDATE_V_SCROLL) {
            SyncPreview();
            ScheduleSpell(150);
        }
        if (n->updated & (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT)) {
            InvalidateStatus();
            ScheduleToolbar();
        }
        break;
    case SCN_CHARADDED:
        g_lastChar = GetTickCount();
        if (n->ch == '\n' || (n->ch == '\r' && g_ed(SCI_GETEOLMODE) == SC_EOL_CR)) OnNewLine();
        break;
    case SCN_ZOOM:
        g_set.zoom = (int)g_ed(SCI_GETZOOM);
        UpdateLineNumbers();
        UpdateEditorMargins();
        InvalidateStatus();
        break;
    case SCN_FOCUSIN: g_lastFocus = g_ed.hwnd; break;
    }
}

// ------------------------------------------------------------------ file drops on the editor
// Scintilla registers its own OLE drop target (for text). Wrap it so that
// dropped files are opened while text drag & drop keeps working.
struct DropTarget : IDropTarget {
    IDropTarget* inner = nullptr;
    bool files = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *pp = this; return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    static bool HasFiles(IDataObject* o) {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return o && o->QueryGetData(&fe) == S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* o, DWORD k, POINTL pt, DWORD* eff) override {
        files = HasFiles(o);
        if (files) { *eff = DROPEFFECT_COPY; return S_OK; }
        if (inner) return inner->DragEnter(o, k, pt, eff);
        *eff = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD k, POINTL pt, DWORD* eff) override {
        if (files) { *eff = DROPEFFECT_COPY; return S_OK; }
        if (inner) return inner->DragOver(k, pt, eff);
        *eff = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (!files && inner) return inner->DragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* o, DWORD k, POINTL pt, DWORD* eff) override {
        if (files) {
            FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM st{};
            if (SUCCEEDED(o->GetData(&fe, &st))) {
                HDROP hd = (HDROP)GlobalLock(st.hGlobal);
                std::vector<std::wstring> paths;
                if (hd) {
                    UINT cnt = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < cnt; i++) {
                        UINT len = DragQueryFileW(hd, i, nullptr, 0);
                        std::wstring p(len, L'\0');
                        DragQueryFileW(hd, i, &p[0], len + 1);
                        paths.push_back(p);
                    }
                    GlobalUnlock(st.hGlobal);
                }
                ReleaseStgMedium(&st);
                std::vector<std::wstring> images, others;
                for (auto& p : paths) (Fmt_IsImageFile(p) && g_active >= 0 ? images : others).push_back(p);
                // images go into the document they were dropped on, before other files open in new tabs
                if (!images.empty()) {
                    POINT c{pt.x, pt.y};
                    ScreenToClient(g_ed.hwnd, &c);
                    sptr_t pos = g_ed(SCI_POSITIONFROMPOINT, c.x, c.y);
                    Fmt_InsertImages(g_ed, images, Cur().path, pos < 0 ? g_ed(SCI_GETCURRENTPOS) : pos);
                    SetFocus(g_ed.hwnd);
                }
                for (auto& p : others) OpenFile(p);
            }
            *eff = DROPEFFECT_COPY;
            files = false;
            return S_OK;
        }
        if (inner) return inner->Drop(o, k, pt, eff);
        *eff = DROPEFFECT_NONE;
        return S_OK;
    }
} g_drop;

void InstallDropTarget(HWND ed) {
    g_drop.inner = (IDropTarget*)GetPropW(ed, L"OleDropTargetInterface");
    if (g_drop.inner) g_drop.inner->AddRef();
    RevokeDragDrop(ed);
    RegisterDragDrop(ed, &g_drop);
}

// ------------------------------------------------------------------ window state / session
void SaveWindowState() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(g_main, &wp)) {
        g_set.win = wp.rcNormalPosition;
        g_set.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    }
    if (g_primary) {
        g_set.session.clear();
        g_set.sessionActive = 0;
        for (size_t i = 0; i < g_docs.size(); i++) {
            if (g_docs[i]->path.empty()) continue;
            if ((int)i == g_active) g_set.sessionActive = (int)g_set.session.size();
            g_set.session.push_back(g_docs[i]->path);
        }
    }
}

bool CloseAll() {
    for (size_t i = 0; i < g_docs.size(); i++)
        if (Dirty(*g_docs[i]) && !ConfirmSave((int)i)) return false;
    return true;
}

// Files forwarded by another MarkLite process. They are opened after WM_COPYDATA has returned: an
// error box shown while opening would otherwise keep the sender waiting until it gives up and
// opens a window of its own.
void OpenFromCopyData(const COPYDATASTRUCT* cds) {
    if (!cds || cds->dwData != kCopyDataOpen || !cds->lpData) return;
    auto* all = new std::wstring((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
    if (!PostMessageW(g_main, WM_APP_OPENFILES, 0, (LPARAM)all)) delete all;
}

void OpenForwardedFiles(const std::wstring& all) {
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(L'\n', i);
        if (j == std::wstring::npos) j = all.size();
        std::wstring p = all.substr(i, j - i);
        if (!p.empty() && p[0]) OpenFile(p);
        i = j + 1;
    }
    if (IsIconic(g_main)) ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
}

// ------------------------------------------------------------------ window procedure
LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT res;
    if (Theme_MenuBarProc(h, m, w, l, &res)) return res;
    switch (m) {
    case WM_CREATE: return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintMain(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = {S(520), S(360)};
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        MakeStatusFont();
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        UpdateLineNumbers();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet") && g_set.theme == 0 && WantDark() != Theme_IsDark()) ApplyTheme();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(h, f)) g_lastFocus = f;
        } else if (!HIWORD(w)) {   // not while minimized: keystrokes would go into the hidden document
            HWND f = (g_lastFocus && IsWindow(g_lastFocus) && IsWindowVisible(g_lastFocus)) ? g_lastFocus : (g_set.viewMode == 2 ? g_preview : g_ed.hwnd);
            SetFocus(f);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (w) PostMessageW(h, WM_APP_FILECHECK, 0, 0);
        return 0;
    case WM_SETFOCUS: SetFocus(g_set.viewMode == 2 ? g_preview : g_ed.hwnd); return 0;
    case WM_APP_FILECHECK: CheckFiles(); return 0;
    case WM_INITMENUPOPUP: OnInitMenuPopup((HMENU)w); return 0;
    case WM_COMMAND: OnCommand(LOWORD(w)); return 0;
    case WM_CONTEXTMENU:
        if ((HWND)w == g_ed.hwnd) { EditorContextMenu(l); return 0; }
        break;
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)l;
        if (nh->hwndFrom == g_ed.hwnd) OnEditorNotify((SCNotification*)l);
        return 0;
    }
    case WM_TIMER:
        switch (w) {
        case T_PREVIEW: RefreshPreview(); break;
        case T_SPELL: {
            KillTimer(h, T_SPELL);
            if (!g_set.spell || g_set.viewMode == 2) break;
            bool typing = GetTickCount() - g_lastChar < 1500;
            Spell::CheckVisible(g_ed, typing);
            if (typing) SetTimer(h, T_SPELL, 1600, nullptr);
            break;
        }
        case T_WORDS:
            KillTimer(h, T_WORDS);
            g_words = CountWords((const char*)g_ed(SCI_GETCHARACTERPOINTER), (size_t)g_ed.Len());
            InvalidateStatus();
            break;
        case T_MSG: KillTimer(h, T_MSG); g_msg.clear(); InvalidateStatus(); break;
        case T_FIND: KillTimer(h, T_FIND); Find_Refresh(); break;
        case T_TOOLBAR: UpdateToolbar(); break;
        }
        return 0;
    case WM_APP_TAB:
        switch ((int)w) {
        case TAB_SELECT: Activate((int)l); break;
        case TAB_CLOSE: CloseDoc((int)l); break;
        case TAB_NEW: OnCommand(CMD_NEW); break;
        case TAB_CONTEXT: TabContextMenu((int)l); break;
        case TAB_MOVE: {
            int from = LOWORD(l), to = HIWORD(l);
            if (from < 0 || to < 0 || from >= (int)g_docs.size() || to >= (int)g_docs.size() || from == to) break;
            int activeId = Cur().id;
            auto d = std::move(g_docs[from]);
            g_docs.erase(g_docs.begin() + from);
            g_docs.insert(g_docs.begin() + to, std::move(d));
            for (size_t i = 0; i < g_docs.size(); i++) if (g_docs[i]->id == activeId) g_active = (int)i;
            UpdateTabs();
            break;
        }
        }
        return 0;
    case WM_APP_PANEL_CLOSED: Layout(); return 0;
    case WM_APP_PREVIEW_LINK: {
        std::unique_ptr<std::wstring> url((std::wstring*)l);
        OnPreviewLink(*url);
        return 0;
    }
    case WM_APP_PREVIEW_GOTO: {
        if (g_set.viewMode == 2) SetViewMode(1);
        sptr_t pos = std::min<sptr_t>((sptr_t)w, g_ed.Len());
        g_ed(SCI_GOTOPOS, pos);
        sptr_t line = g_ed(SCI_LINEFROMPOSITION, pos);
        g_ed(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, g_ed(SCI_VISIBLEFROMDOCLINE, line) - 2));
        SetFocus(g_ed.hwnd);
        return 0;
    }
    case WM_COPYDATA: OpenFromCopyData((const COPYDATASTRUCT*)l); return TRUE;
    case WM_APP_OPENFILES: {
        std::unique_ptr<std::wstring> all((std::wstring*)l);
        OpenForwardedFiles(*all);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        UINT cnt = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < cnt; i++) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(hd, i, &p[0], len + 1);
            OpenFile(p);
        }
        DragFinish(hd);
        return 0;
    }
    case WM_SETCURSOR:
        if ((HWND)w == h && LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (PtInRect(&g_splitRc, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
            int k = SegAt(pt);
            if (k != SEG_NONE && k != SEG_WORDS) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&g_splitRc, pt)) { g_splitDrag = true; SetCapture(h); InvalidateRect(h, &g_splitRc, FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&g_splitRc, pt)) { g_set.split = 0.5f; Layout(); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (g_splitDrag) {
            RECT rc;
            GetClientRect(h, &rc);
            float f = (pt.x - SplitW() / 2.0f) / (float)std::max(1, (int)rc.right - SplitW());
            g_set.split = std::max(0.15f, std::min(0.85f, f));
            Layout();
            return 0;
        }
        int k = PtInRect(&g_statusRc, pt) ? SegAt(pt) : SEG_NONE;
        if (k == SEG_WORDS) k = SEG_NONE;
        if (k != g_segHot) {
            g_segHot = k;
            InvalidateStatus();
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_segHot != SEG_NONE) { g_segHot = SEG_NONE; InvalidateStatus(); }
        return 0;
    case WM_CAPTURECHANGED:   // capture lost mid-drag (Alt+Tab, a menu): stop dragging the splitter
        if (g_splitDrag && (HWND)l != h) {
            g_splitDrag = false;
            InvalidateRect(h, &g_splitRc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (g_splitDrag) {
            g_splitDrag = false;
            ReleaseCapture();
            InvalidateRect(h, &g_splitRc, FALSE);
            return 0;
        }
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        for (auto& s : g_segs) {
            if (!PtInRect(&s.rc, pt)) continue;
            switch (s.kind) {
            case SEG_POS: OnCommand(CMD_GOTO); break;
            case SEG_ZOOM: g_ed(SCI_SETZOOM, 0); break;   // the segment shows the editor's zoom
            case SEG_SPELL: PopupAt(g_mSpell, s.rc); break;
            case SEG_ENC: PopupAt(g_mEnc, s.rc); break;
            case SEG_EOL: PopupAt(g_mEol, s.rc); break;
            }
            break;
        }
        return 0;
    }
    case WM_QUERYENDSESSION:
        if (!CloseAll()) return FALSE;
        g_closing = true;
        SaveWindowState();
        Settings_Save();
        return TRUE;
    case WM_ENDSESSION:
        if (!w) g_closing = false;   // the shutdown was cancelled: keep working normally
        return 0;
    case WM_CLOSE:
        if (!CloseAll()) return 0;
        g_closing = true;
        Ai_CancelAll();
        AiPanel_Shutdown();
        SaveWindowState();
        Settings_Save();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        RevokeDragDrop(g_ed.hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::vector<std::wstring> ParseArgs(bool& newWindow) {
    std::vector<std::wstring> files;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--new" || a == L"-n" || a == L"/new") newWindow = true;
        else if (!a.empty()) files.push_back(FullPath(a));
    }
    if (argv) LocalFree(argv);
    return files;
}

bool ForwardToRunningInstance(const std::vector<std::wstring>& files) {
    // When several files are opened from Explorer at once, MarkLite starts once per file: the first
    // process may not have created its window yet, so give it a moment.
    HWND other = nullptr;
    for (int i = 0; i < 60 && !(other = FindWindowW(kMainClass, nullptr)); i++) Sleep(50);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);
    std::wstring all;
    for (auto& f : files) all += f + L"\n";
    COPYDATASTRUCT cds{kCopyDataOpen, (DWORD)((all.size() + 1) * sizeof(wchar_t)), (void*)all.c_str()};
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &r) != 0;
}
}  // namespace

// ------------------------------------------------------------------ app services
HWND App_Main() { return g_main; }
Sci& App_Editor() { return g_ed; }
int App_ActiveDocId() { return g_active >= 0 ? Cur().id : -1; }
void App_Relayout() { Layout(); }
HINSTANCE App_Instance() { return g_inst; }

void App_Status(const std::wstring& msg, bool error) {
    g_msg = msg;
    g_msgErr = error;
    InvalidateStatus();
    if (g_main) SetTimer(g_main, T_MSG, error ? 7000 : 4000, nullptr);
}

void App_OpenSettings() {
    Dlg_Settings(g_main);
    InvalidateStatus();
}

bool App_EnsureApiKey() {
    if (!g_set.apiKey.empty()) return true;
    App_OpenSettings();
    return !g_set.apiKey.empty();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    bool newWindow = false;
    std::vector<std::wstring> files = ParseArgs(newWindow);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\MarkLite.SingleInstance");
    bool another = GetLastError() == ERROR_ALREADY_EXISTS;
    if (another && !newWindow && ForwardToRunningInstance(files)) return 0;
    g_primary = !another;

    if (FAILED(OleInitialize(nullptr))) return 1;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    Scintilla_RegisterClasses(inst);
    Theme_Init();
    Settings_Load();
    Theme_Set(WantDark());

    Spell::Init();
    if (!g_set.spellLangsInit) {
        g_set.spellLangs = Spell::DefaultLanguages();
        g_set.spellLangsInit = true;
    }
    Spell::SetLanguages(g_set.spellLangs);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    g_menuBar = BuildMenus();
    BuildAccelerators();
    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, kMainClass, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, nullptr, g_menuBar, inst, nullptr);
    if (!g_main) return 1;
    g_dpi = Dpi(g_main);
    Theme_TitleBar(g_main);
    MakeStatusFont();

    // Restore the window placement (or pick a comfortable default size).
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(g_main, &wp);
    RECT r = g_set.win;
    bool valid = r.left != CW_USEDEFAULT && r.right - r.left > 200 && r.bottom - r.top > 150 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!valid) {
        HMONITOR mon = MonitorFromWindow(g_main, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        int ww = std::min<int>(S(1280), (mi.rcWork.right - mi.rcWork.left) * 9 / 10);
        int wh = std::min<int>(S(820), (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10);
        r.left = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
        r.top = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
        r.right = r.left + ww;
        r.bottom = r.top + wh;
    }
    if (another) OffsetRect(&r, S(30), S(30));
    wp.rcNormalPosition = r;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(g_main, &wp);
    g_dpi = Dpi(g_main);
    MakeStatusFont();

    g_tabs = Tabs_Create(g_main);
    g_toolbar = Toolbar_Create(g_main, ToolbarItems(), ToolbarMenu);
    if (g_toolbar && !g_set.toolbar) ShowWindow(g_toolbar, SW_HIDE);
    HWND ed = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0, 10, 10, g_main,
                              (HMENU)(INT_PTR)ID_EDITOR, inst, nullptr);
    HWND aux = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD, 0, 0, 1, 1, g_main, (HMENU)(INT_PTR)ID_AUX, inst, nullptr);
    if (!ed || !aux) return 1;
    g_ed.Attach(ed);
    g_aux.Attach(aux);
    g_aux(SCI_SETMODEVENTMASK, 0);
    g_aux(SCI_SETUNDOCOLLECTION, 0);
    g_ed(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
    g_ed(SCI_SETCOMMANDEVENTS, 0);
    Md_ConfigureView(g_ed, true);
    InstallDropTarget(ed);
    g_preview = Preview_Create(g_main);
    g_findBar = Find_Create(g_main, &g_ed);
    g_aiPanel = AiPanel_Create(g_main);

    // Documents: previous session, then files from the command line.
    // The tab to show: the last file named on the command line (even when the session already had it
    // open), else the session's active document - looked up by position in the saved session, since
    // session files that no longer exist are skipped.
    int first = -1;
    if (g_primary && g_set.restoreSession) {
        for (size_t k = 0; k < g_set.session.size(); k++) {
            const std::wstring& p = g_set.session[k];
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            int idx = OpenFile(p, false, true);
            if (idx >= 0 && ((int)k <= g_set.sessionActive || first < 0)) first = idx;
        }
    }
    for (auto& f : files) {
        int idx = OpenFile(f, false);
        if (idx >= 0) first = idx;
    }
    if (g_docs.empty()) NewUntitled();
    Activate(first >= 0 && first < (int)g_docs.size() ? first : 0);
    Layout();

    ShowWindow(g_main, g_set.maximized ? SW_SHOWMAXIMIZED : (show == SW_SHOWMINIMIZED || show == SW_MINIMIZE ? show : SW_SHOWNORMAL));
    UpdateWindow(g_main);
    if (!IsIconic(g_main)) SetFocus(g_set.viewMode == 2 ? g_preview : g_ed.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (Find_PreTranslate(&msg) || AiPanel_PreTranslate(&msg)) continue;
        HWND f = GetFocus();
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && f && IsChild(g_aiPanel, f) && GetKeyState(VK_CONTROL) >= 0) {
            if (IsDialogMessageW(g_aiPanel, &msg)) continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && f == g_ed.hwnd && Find_Visible() && g_ed(SCI_GETSELECTIONS) == 1) {
            Find_Hide();
            continue;
        }
        HACCEL acc = (f == g_ed.hwnd || f == g_main) ? g_accFull : g_accGlobal;
        if (g_main && (f == g_main || IsChild(g_main, f)) && TranslateAcceleratorW(g_main, acc, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Settings and documents are saved by now. Exit directly: running the global
    // destructors would release COM objects (spell checkers, WIC) after COM shutdown.
    if (mutex) CloseHandle(mutex);
    ExitProcess((UINT)msg.wParam);
}
