// Shared declarations between the modules of MarkLite.
#pragma once
#include "util.h"
#include <stdint.h>
#include "Scintilla.h"

#define APP_NAME L"MarkLite"
#include "version.h"   // generated from VERSION by the Makefile
#define APP_VERSION MARKLITE_VERSION_W

// ---------------------------------------------------------------- messages
enum : UINT {
    WM_APP_AI = WM_APP + 1,   // wParam = job id, lParam = AiEvent*  (receiver deletes)
    WM_APP_MODELS,            // lParam = ModelList*                 (receiver deletes)
    WM_APP_PREVIEW_LINK,      // lParam = std::wstring*              (receiver deletes)
    WM_APP_PREVIEW_GOTO,      // wParam = source byte offset
    WM_APP_TAB,               // wParam = TabAction, lParam = index (or MAKELPARAM(from,to))
    WM_APP_FILECHECK,         // posted after activation to look for external file changes
    WM_APP_PANEL_CLOSED,      // AI panel / find bar closed -> relayout
    WM_APP_OPENFILES,         // lParam = std::wstring*, one path per line (receiver deletes)
};

enum TabAction { TAB_SELECT, TAB_CLOSE, TAB_NEW, TAB_MOVE, TAB_CONTEXT };

// Scintilla indicators (container range 8..31)
enum { IND_SPELL = 8, IND_AISEL = 9, IND_FIND = 10, IND_STRIKE = 11 };

// ---------------------------------------------------------------- Scintilla
struct Sci {
    HWND hwnd = nullptr;
    SciFnDirect fn = nullptr;
    sptr_t ptr = 0;
    void Attach(HWND h) {
        hwnd = h;
        fn = (SciFnDirect)SendMessageW(h, SCI_GETDIRECTFUNCTION, 0, 0);
        ptr = (sptr_t)SendMessageW(h, SCI_GETDIRECTPOINTER, 0, 0);
    }
    sptr_t operator()(unsigned m, uptr_t w = 0, sptr_t l = 0) const { return fn(ptr, m, w, l); }
    sptr_t Len() const { return (*this)(SCI_GETLENGTH); }
    std::string Range(sptr_t a, sptr_t b) const {
        if (b <= a) return std::string();
        std::string s((size_t)(b - a), '\0');
        Sci_TextRangeFull tr{{a, b}, &s[0]};
        (*this)(SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        return s;
    }
    std::string Text() const { return Range(0, Len()); }
    std::string SelText() const { return Range((*this)(SCI_GETSELECTIONSTART), (*this)(SCI_GETSELECTIONEND)); }
    std::string LineText(sptr_t line) const {
        return Range((*this)(SCI_POSITIONFROMLINE, line), (*this)(SCI_GETLINEENDPOSITION, line));
    }
};

// ---------------------------------------------------------------- theme
struct Palette {
    COLORREF bg, fg, dim, accent, link, heading, codeBg, codeFg, quote, html, mark, listMark;
    COLORREF chromeBg, chromeFg, chromeDim, tabActive, tabHover, border, statusBg, statusFg;
    COLORREF selBg, caretLine, spell, findHit, aiSel, inputBg, inputFg, btnBg, btnHover, btnPrimary, btnPrimaryFg, error, ok;
};
bool  Theme_IsDark();
const Palette& Pal();
void  Theme_Init();                        // resolve undocumented uxtheme entry points
void  Theme_Set(bool dark);                // app-wide (menus, palette)
bool  Theme_SystemDark();
void  Theme_TitleBar(HWND h);              // dark/light caption
void  Theme_Control(HWND h, const wchar_t* lightTheme = L"Explorer"); // scrollbars, buttons...
void  Theme_Edit(HWND h);                  // edit / combobox
bool  Theme_MenuBarProc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res);
HBRUSH Theme_Brush(COLORREF c);            // cached solid brushes
LRESULT Theme_CtlColor(HDC hdc, bool input);

// ---------------------------------------------------------------- settings
struct ModelInfo { std::wstring id, name; };
struct Settings {
    // AI
    std::wstring apiKey;                     // plaintext in memory, DPAPI-encrypted on disk
    std::wstring model = L"claude-opus-5-5";
    std::wstring effort = L"medium";
    bool aiContext = true;
    std::vector<std::wstring> instrHistory;
    std::vector<ModelInfo> models;           // cached model list
    int aiPanelH = 300;                      // DIPs
    // Spelling
    bool spell = true;
    bool spellIgnoreCaps = true;
    std::vector<std::wstring> spellLangs;    // empty = pick defaults on first run
    bool spellLangsInit = false;
    // View
    int theme = 0;                           // 0 system, 1 light, 2 dark
    int viewMode = 1;                        // 0 editor, 1 split, 2 preview
    int lastPreviewMode = 1;
    bool wrap = true, lineNumbers = false, limitWidth = true, toolbar = true;
    int zoom = 0;
    float previewZoom = 1.0f;
    std::wstring font = L"Segoe UI";
    int fontSize = 11;
    std::wstring codeFont = L"Consolas";
    float split = 0.5f;
    // Window / files
    RECT win{CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
    bool maximized = false;
    std::vector<std::wstring> recent;
    std::vector<std::wstring> session;
    int sessionActive = 0;
    bool restoreSession = true;
};
extern Settings g_set;
void Settings_Load();
void Settings_Save();
void Settings_AddRecent(const std::wstring& path);
void Settings_RemoveRecent(const std::wstring& path);
void Settings_ClearRecent();
std::vector<ModelInfo> Settings_DefaultModels();
std::wstring ModelDisplayName(const std::wstring& id);

// ---------------------------------------------------------------- editor styling (editor.cpp)
void Md_ConfigureView(Sci& s, bool mainEditor);   // styles, colours, indicators for current theme
void Md_StyleNeeded(Sci& s, sptr_t endPos);        // container lexer
bool Md_IsCodeStyle(int style);
bool Md_IsCodeBlockStyle(int style);              // fenced or indented code
int  Md_InlineFlags(int style);                    // 1 bold, 2 italic, 4 strike, 8 code; -1 for markup characters
bool Md_IsSpellSkipStyle(int style);

// ---------------------------------------------------------------- app services (main.cpp)
HWND App_Main();
Sci& App_Editor();
int  App_ActiveDocId();
void App_Relayout();
void App_Status(const std::wstring& msg, bool error = false);
bool App_EnsureApiKey();
void App_OpenSettings();
HINSTANCE App_Instance();
