// MarkLite Setup window: one small window for installing and uninstalling.
//
// Everything is drawn with Direct2D/DirectWrite. The interactive parts are ordinary Win32 button,
// check box and radio controls painted through NM_CUSTOMDRAW, so keyboard navigation, focus and
// screen readers work as they do in any Windows dialog. Follows the Windows light/dark setting
// and high contrast mode.
#include "setup.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;
using namespace setup;

namespace {

// ------------------------------------------------------------------ layout (DIPs)
constexpr float kW = 560;
constexpr float kHInstall = 448, kHUninstall = 372;
constexpr float kM = 36;                 // side margin
constexpr float kFooter = 72;            // footer band height
constexpr float kBodyRight = kW - kM;    // 524

enum : int { ID_USER = 101, ID_MACHINE, ID_CHANGE, ID_DESKTOP, ID_ASSOC, ID_REMOVE };   // + IDOK / IDCANCEL
enum : UINT { WM_APP_PROGRESS = WM_APP + 1, WM_APP_DONE, WM_APP_CLOSED };
constexpr UINT_PTR kTimerProgress = 1;
constexpr wchar_t kClass[] = L"MarkLiteSetup";

enum class Page { Options, Confirm, Progress, Done, Running, Error };

// ------------------------------------------------------------------ state
struct State {
    UiMode mode = UiMode::Install;
    ExistingInstall existing;
    InstallOptions opt;
    bool dirCustomized = false;
    bool removeSettings = false;
    Page page = Page::Options;
    std::wstring hintError;              // shown under the scope switch (e.g. UAC declined)
    // progress
    bool working = false, done = false, elevating = false, closing = false, stillOpen = false;
    double shown = 0, target = 0;
    std::vector<std::pair<double, const wchar_t*>> steps;   // (percent where it starts, text)
    // result
    DWORD errorCode = 0;
    bool errorFatal = false;
    int exitCode = ERROR_CANCELLED;
} S;

HINSTANCE g_inst;
HWND g_wnd, hUser, hMachine, hChange, hDesktop, hAssoc, hRemove, hOk, hCancel;
UINT g_dpi = 96;
float g_scale = 1.f;

int Px(float v) { return (int)lroundf(v * g_scale); }
float ClientH() { return S.mode == UiMode::Install ? kHInstall : kHUninstall; }
float FooterTop() { return ClientH() - kFooter; }

// ------------------------------------------------------------------ colours
struct Palette {
    D2D1_COLOR_F bg, footer, text, dim, faint, border, borderStrong, field, hover, pressed;
    D2D1_COLOR_F segBg, segSel, segHover, track, accentA, accentB, onAccent, focus, ok, err, warn;
    COLORREF caption;
    bool dark, hc;
} C;

D2D1_COLOR_F Rgb(UINT32 v) { return D2D1::ColorF(v); }
D2D1_COLOR_F Sys(int idx) {
    COLORREF c = GetSysColor(idx);
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
}

bool SystemDark() {
    DWORD v = 1, n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;
}

bool HighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

void LoadPalette() {
    C.hc = HighContrast();
    C.dark = !C.hc && SystemDark();
    if (C.hc) {
        D2D1_COLOR_F win = Sys(COLOR_WINDOW), txt = Sys(COLOR_WINDOWTEXT), hi = Sys(COLOR_HIGHLIGHT), hiTxt = Sys(COLOR_HIGHLIGHTTEXT);
        C.bg = C.footer = C.field = C.segBg = C.segSel = C.track = win;
        C.text = C.dim = C.border = C.borderStrong = txt;
        C.faint = Sys(COLOR_GRAYTEXT);
        C.hover = C.pressed = C.segHover = win;
        C.accentA = C.accentB = C.focus = C.ok = C.err = C.warn = hi;
        C.onAccent = hiTxt;
        C.caption = GetSysColor(COLOR_WINDOW);
    } else if (C.dark) {
        C.bg = Rgb(0x1F2023); C.footer = Rgb(0x191A1D); C.text = Rgb(0xEDEEF0); C.dim = Rgb(0xA8AEB7); C.faint = Rgb(0x6F757E);
        C.border = Rgb(0x313338); C.borderStrong = Rgb(0x50545B); C.field = Rgb(0x2A2C30); C.hover = Rgb(0x2C2E33);
        C.pressed = Rgb(0x34373C); C.segBg = Rgb(0x2A2C30); C.segSel = Rgb(0x3B3E44); C.segHover = Rgb(0x313338);
        C.track = Rgb(0x34363B); C.accentA = Rgb(0x5A7DF5); C.accentB = Rgb(0x8358EA); C.onAccent = Rgb(0xFFFFFF);
        C.focus = Rgb(0x8EA6FA); C.ok = Rgb(0x3FBF77); C.err = Rgb(0xF06A6A); C.warn = Rgb(0xE9A23B);
        C.caption = RGB(0x1F, 0x20, 0x23);
    } else {
        C.bg = Rgb(0xFFFFFF); C.footer = Rgb(0xF7F8FA); C.text = Rgb(0x1A1D23); C.dim = Rgb(0x5B6270); C.faint = Rgb(0x9AA0A9);
        C.border = Rgb(0xE4E7EC); C.borderStrong = Rgb(0xC3C8D0); C.field = Rgb(0xF5F6F8); C.hover = Rgb(0xF1F3F6);
        C.pressed = Rgb(0xE6E9EE); C.segBg = Rgb(0xF0F2F5); C.segSel = Rgb(0xFFFFFF); C.segHover = Rgb(0xE8EBF0);
        C.track = Rgb(0xE9ECF1); C.accentA = Rgb(0x4269EE); C.accentB = Rgb(0x7244E1); C.onAccent = Rgb(0xFFFFFF);
        C.focus = Rgb(0x4269EE); C.ok = Rgb(0x1F9D55); C.err = Rgb(0xD93636); C.warn = Rgb(0xD98E04);
        C.caption = RGB(0xFF, 0xFF, 0xFF);
    }
}

// ------------------------------------------------------------------ graphics
// d2d1, dwrite and dwmapi aren't KnownDLLs; load them from System32 explicitly (see main.cpp).
template <class T> T SysProc(const wchar_t* dll, const char* name) {
    HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? (T)(void*)GetProcAddress(m, name) : nullptr;
}

struct Gfx {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1DCRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> accent;
    ComPtr<ID2D1StrokeStyle> round;
    ComPtr<ID2D1Bitmap> icon, shield;
    ComPtr<IDWriteTextFormat> title, subtitle, heading, body, label, small, button, buttonBold, path;
} G;

bool HasFamily(const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(G.dw->GetSystemFontCollection(&fonts, FALSE)) && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
}

ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, bool wrap) {
    ComPtr<IDWriteTextFormat> f;
    G.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

bool InitGraphics() {
    using FnD2D = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using FnDW = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    auto createD2D = SysProc<FnD2D>(L"d2d1.dll", "D2D1CreateFactory");
    auto createDW = SysProc<FnDW>(L"dwrite.dll", "DWriteCreateFactory");
    if (!createD2D || !createDW) return false;
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(createD2D(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)G.d2d.GetAddressOf()))) return false;
    if (FAILED(createDW(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)G.dw.GetAddressOf()))) return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&G.wic));
    G.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                         D2D1_LINE_JOIN_ROUND), nullptr, 0, &G.round);

    const wchar_t* text = HasFamily(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = HasFamily(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    G.title = MakeFormat(display, 26.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    G.subtitle = MakeFormat(text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.heading = MakeFormat(display, 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    G.body = MakeFormat(text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, true);
    G.label = MakeFormat(text, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    G.small = MakeFormat(text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    G.button = MakeFormat(text, 14.f, DWRITE_FONT_WEIGHT_NORMAL, false);
    G.buttonBold = MakeFormat(text, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    G.path = MakeFormat(text, 13.f, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!G.title || !G.subtitle || !G.heading || !G.body || !G.label || !G.small || !G.button || !G.buttonBold || !G.path) return false;
    // Long paths lose their middle: "C:\Users\…\Programs\MarkLite".
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, L'\\', 2};
    ComPtr<IDWriteInlineObject> ellipsis;
    G.dw->CreateEllipsisTrimmingSign(G.path.Get(), &ellipsis);
    G.path->SetTrimming(&trim, ellipsis.Get());
    return true;
}

void DiscardTarget() {
    G.rt.Reset();
    G.brush.Reset();
    G.accent.Reset();
    G.icon.Reset();
    G.shield.Reset();
}

bool EnsureTarget() {
    if (G.rt) return true;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(G.d2d->CreateDCRenderTarget(&props, &G.rt))) return false;
    G.rt->SetDpi((float)g_dpi, (float)g_dpi);
    G.rt->CreateSolidColorBrush(C.text, &G.brush);
    D2D1_GRADIENT_STOP stops[2] = {{0.f, C.accentA}, {1.f, C.accentB}};
    ComPtr<ID2D1GradientStopCollection> sc;
    G.rt->CreateGradientStopCollection(stops, 2, &sc);
    if (sc) G.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)), sc.Get(), &G.accent);
    if (!G.brush || !G.accent) {
        DiscardTarget();
        return false;
    }
    return true;
}

ComPtr<ID2D1Bitmap> BitmapFromIcon(HICON icon) {
    ComPtr<ID2D1Bitmap> bmp;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    if (icon && G.wic && SUCCEEDED(G.wic->CreateBitmapFromHICON(icon, &wb)) && SUCCEEDED(G.wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        G.rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

void EnsureIcons() {
    HICON h = nullptr;
    if (!G.icon) {
        int px = Px(56);
        if (SUCCEEDED(LoadIconWithScaleDown(g_inst, MAKEINTRESOURCEW(IDI_APP), px, px, &h))) {
            G.icon = BitmapFromIcon(h);
            DestroyIcon(h);
        }
    }
    if (!G.shield) {
        int px = Px(16);
        if (SUCCEEDED(LoadIconWithScaleDown(nullptr, IDI_SHIELD, px, px, &h))) {
            G.shield = BitmapFromIcon(h);
            DestroyIcon(h);
        }
    }
}

// ------------------------------------------------------------------ drawing helpers
D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return R(r.left + d, r.top + d, r.right - d, r.bottom - d); }

void Fill(D2D1_RECT_F r, D2D1_COLOR_F c) {
    G.brush->SetColor(c);
    G.rt->FillRectangle(r, G.brush.Get());
}
void FillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    G.brush->SetColor(c);
    G.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), G.brush.Get());
}
void StrokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float width) {
    G.brush->SetColor(c);
    G.rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), G.brush.Get(), width);
}
// The gradient runs across `span` so a growing progress bar keeps the same colours.
void FillAccent(D2D1_RECT_F r, float rad, D2D1_RECT_F span) {
    G.accent->SetStartPoint(D2D1::Point2F(span.left, span.top));
    G.accent->SetEndPoint(D2D1::Point2F(span.right, span.bottom));
    G.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), G.accent.Get());
}
void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) {
    G.brush->SetColor(c);
    G.rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), G.brush.Get(), w, G.round.Get());
}

void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT ta = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    f->SetTextAlignment(ta);
    f->SetParagraphAlignment(pa);
    G.brush->SetColor(c);
    G.rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, G.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS m{};
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(G.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000.f, 200.f, &layout)) || FAILED(layout->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

// Round status mark left of a page heading: a check, or an exclamation mark.
void Badge(float cx, float cy, bool check, D2D1_COLOR_F color) {
    D2D1_RECT_F r = R(cx - 20, cy - 20, cx + 20, cy + 20);
    if (check && !C.hc) FillAccent(r, 20, r);
    else FillRound(r, 20, color);
    D2D1_COLOR_F mark = C.hc ? C.onAccent : D2D1::ColorF(D2D1::ColorF::White);
    if (check) {
        Line(cx - 8, cy + 0.5f, cx - 2.5f, cy + 6, mark, 3);
        Line(cx - 2.5f, cy + 6, cx + 8.5f, cy - 6, mark, 3);
    } else {
        Line(cx, cy - 9, cx, cy + 2.5f, mark, 3);
        Line(cx, cy + 8.5f, cx, cy + 8.6f, mark, 3.4f);
    }
}

// ------------------------------------------------------------------ text content
std::wstring ScopeName(Scope s) { return s == Scope::Machine ? L"all users" : L"you"; }

int VersionCompare() { return S.existing.found ? CompareVersions(SETUP_VERSION, S.existing.version) : 1; }

std::wstring PrimaryInstallLabel() {
    if (!S.existing.found) return L"Install";
    int c = VersionCompare();
    return c > 0 ? L"Update" : c == 0 ? L"Reinstall" : L"Install";
}

std::wstring Subtitle() {
    if (S.mode == UiMode::Uninstall) {
        std::wstring v = S.existing.version.empty() ? std::wstring(SETUP_VERSION) : S.existing.version;
        return L"Version " + v + L"  \u00B7  Installed for " + ScopeName(S.existing.scope);
    }
    return std::wstring(L"Markdown reader and editor  \u00B7  Version ") + SETUP_VERSION;
}

std::wstring ScopeHint() {
    if (!S.hintError.empty()) return S.hintError;
    if (S.existing.found) {
        std::wstring s = L"MarkLite " + S.existing.version + L" is installed for " + ScopeName(S.existing.scope) + L". ";
        int c = VersionCompare();
        if (c > 0) return s + L"Setup will update it.";
        if (c == 0) return s + L"Setup will reinstall it.";
        return s + L"Setup will replace it with version " SETUP_VERSION L".";
    }
    return S.opt.scope == Scope::User ? L"Installs in your user folder. No administrator rights needed."
                                      : L"Installs for everyone on this PC. Windows will ask for administrator rights.";
}

// ------------------------------------------------------------------ main window painting
void DrawHeader() {
    if (G.icon) G.rt->DrawBitmap(G.icon.Get(), R(kM, 32, kM + 56, 88), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    Text(L"MarkLite", G.title.Get(), R(108, 29, kBodyRight, 66), C.text);
    Text(Subtitle(), G.subtitle.Get(), R(108, 66, kBodyRight, 88), C.dim);
}

void DrawPathField(float top, float right) {
    D2D1_RECT_F box = R(kM, top, right, top + 34);
    FillRound(box, 8, C.field);
    StrokeRound(Inset(box, 0.5f), 7.5f, C.border, 1);
    Text(S.opt.dir, G.path.Get(), R(kM + 12, top, right - 12, top + 34), C.text, DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void DrawOptions() {
    Text(L"Install for", G.label.Get(), R(kM, 118, kBodyRight, 138), C.text);
    FillRound(R(kM, 144, kBodyRight, 184), 10, C.segBg);
    Text(ScopeHint(), G.small.Get(), R(kM, 191, kBodyRight, 212), S.hintError.empty() ? C.dim : C.err);
    Text(L"Location", G.label.Get(), R(kM, 226, kBodyRight, 246), C.text);
    DrawPathField(250, 416);
}

void DrawConfirm() {
    Text(L"Remove MarkLite from this PC?", G.heading.Get(), R(kM, 116, kBodyRight, 142), C.text);
    Text(L"This removes MarkLite, its shortcuts and its file type registrations. It is installed here:", G.body.Get(),
         R(kM, 148, kBodyRight, 190), C.dim);
    DrawPathField(194, kBodyRight);
    Text(L"Your theme, recent files and saved Claude API key are kept unless you tick this.", G.small.Get(),
         R(kM + 28, 268, kBodyRight, 288), C.dim);
}

D2D1_RECT_F ProgressBarRect() { return R(kM, 164, kBodyRight, 170); }

void DrawProgress() {
    std::wstring heading;
    if (S.mode == UiMode::Uninstall) heading = L"Removing MarkLite";
    else heading = S.existing.found ? L"Updating MarkLite" : L"Installing MarkLite";
    Text(heading, G.heading.Get(), R(kM, 120, kBodyRight, 148), C.text);
    D2D1_RECT_F track = ProgressBarRect();
    FillRound(track, 3, C.track);
    float w = (float)(track.right - track.left) * (float)std::clamp(S.shown, 0.0, 100.0) / 100.f;
    if (w > 0.5f) {
        D2D1_RECT_F fill = track;
        fill.right = track.left + std::max(w, 6.f);
        if (C.hc) FillRound(fill, 3, C.accentA);
        else FillAccent(fill, 3, track);
    }
    // The step text follows the animated bar rather than jumping ahead to the newest step.
    const wchar_t* step = L"";
    for (auto& s : S.steps)
        if (s.first <= S.shown + 0.5) step = s.second;
    Text(step, G.small.Get(), R(kM, 180, kBodyRight, 200), C.dim);
}

void DrawMessage(bool check, D2D1_COLOR_F badge, const std::wstring& heading, const std::wstring& body) {
    Badge(kM + 20, 146, check, badge);
    Text(heading, G.heading.Get(), R(kM + 56, 122, kBodyRight, 148), C.text);
    Text(body, G.body.Get(), R(kM + 56, 152, kBodyRight, FooterTop() - 12), C.dim);
}

std::wstring DoneBody() {
    if (S.mode == UiMode::Uninstall)
        return S.removeSettings ? L"Your settings were removed as well." : L"Your settings were kept in case you install MarkLite again.";
    std::wstring s = S.opt.desktopShortcut ? L"Open it from the Start menu or the shortcut on your desktop."
                                           : L"Open it from the Start menu.";
    if (S.opt.fileAssociations) s += L" Markdown files can now be opened with MarkLite.";
    return s;
}

std::wstring ErrorBody() {
    std::wstring s = ErrorMessage(S.errorCode, S.opt.dir);
    if (S.errorCode == ERROR_ACCESS_DENIED && S.mode == UiMode::Install && S.opt.scope == Scope::User && !S.existing.found)
        s += L" Choose another folder, or install for all users.";
    return s;
}

void DrawPage() {
    switch (S.page) {
    case Page::Options: DrawOptions(); break;
    case Page::Confirm: DrawConfirm(); break;
    case Page::Progress: DrawProgress(); break;
    case Page::Done:
        DrawMessage(true, C.ok,
                    S.mode == UiMode::Uninstall ? L"MarkLite has been removed"
                    : S.existing.found          ? L"MarkLite has been updated"
                                                : L"MarkLite is installed",
                    DoneBody());
        break;
    case Page::Running:
        DrawMessage(false, C.warn, S.stillOpen ? L"MarkLite is still open" : L"MarkLite is open",
                    L"Close MarkLite to continue. If a document has unsaved changes, MarkLite asks what to do with them first.");
        break;
    case Page::Error:
        DrawMessage(false, C.err, S.mode == UiMode::Install ? L"MarkLite couldn't be installed" : L"MarkLite couldn't be removed",
                    ErrorBody());
        break;
    }
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    if (!EnsureTarget() || FAILED(G.rt->BindDC(hdc, &rc))) return;
    EnsureIcons();
    G.rt->BeginDraw();
    G.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    G.rt->Clear(C.bg);
    if (IsWindowVisible(hOk) || IsWindowVisible(hCancel)) {
        Fill(R(0, FooterTop(), kW, ClientH() + 1), C.footer);
        Fill(R(0, FooterTop(), kW, FooterTop() + 1), C.border);
    }
    DrawHeader();
    DrawPage();
    if (G.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        DiscardTarget();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

// ------------------------------------------------------------------ controls (NM_CUSTOMDRAW)
struct CtlState {
    bool hot, pressed, focus, enabled, checked;
};

void DrawSegment(float w, float h, const std::wstring& text, bool shield, const CtlState& st) {
    G.rt->Clear(C.segBg);
    D2D1_RECT_F r = Inset(R(0, 0, w, h), 0.5f);
    if (st.checked) {
        FillRound(r, 7, C.segSel);
        StrokeRound(r, 7, C.hc ? C.text : C.border, 1);
    } else if (st.hot && st.enabled) {
        FillRound(r, 7, C.segHover);
    }
    IDWriteTextFormat* f = st.checked ? G.buttonBold.Get() : G.button.Get();
    D2D1_COLOR_F tc = st.checked ? (st.enabled ? C.text : C.dim) : (st.enabled ? C.dim : C.faint);
    float tw = TextWidth(text, f);
    float total = tw + (shield && G.shield ? 22.f : 0.f);
    float x = (w - total) / 2;
    if (shield && G.shield) {
        G.rt->DrawBitmap(G.shield.Get(), R(x, (h - 16) / 2, x + 16, (h + 16) / 2), st.enabled ? 1.f : 0.5f);
        x += 22;
    }
    Text(text, f, R(x, 0, x + tw + 2, h), tc, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (st.focus) StrokeRound(Inset(R(0, 0, w, h), 1.5f), 6, C.focus, 2);
}

void DrawCheckBox(float w, float h, const std::wstring& text, const CtlState& st) {
    G.rt->Clear(C.bg);
    const float s = 18, x0 = 4, y0 = (h - s) / 2;
    D2D1_RECT_F box = R(x0, y0, x0 + s, y0 + s);
    if (st.checked) {
        if (C.hc) FillRound(box, 5, C.accentA);
        else FillAccent(box, 5, box);
        D2D1_COLOR_F mark = C.hc ? C.onAccent : D2D1::ColorF(D2D1::ColorF::White);
        Line(x0 + 4.6f, y0 + 9.3f, x0 + 7.6f, y0 + 12.3f, mark, 2);
        Line(x0 + 7.6f, y0 + 12.3f, x0 + 13.4f, y0 + 6.0f, mark, 2);
    } else {
        FillRound(box, 5, st.hot ? C.hover : C.bg);
        StrokeRound(Inset(box, 0.5f), 4.5f, st.hot ? C.dim : C.borderStrong, 1);
    }
    Text(text, G.body.Get(), R(x0 + s + 10, 0, w, h), st.enabled ? C.text : C.faint, DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (st.focus) StrokeRound(Inset(box, -3), 7, C.focus, 2);
}

void DrawButton(float w, float h, const std::wstring& text, bool primary, D2D1_COLOR_F behind, const CtlState& st) {
    G.rt->Clear(behind);
    D2D1_RECT_F r = Inset(R(0, 0, w, h), 0.5f);
    if (primary && st.enabled) {
        if (C.hc) FillRound(r, 8, C.accentA);
        else FillAccent(r, 8, r);
        if (!C.hc && st.hot) FillRound(r, 8, D2D1::ColorF(1, 1, 1, 0.10f));
        if (!C.hc && st.pressed) FillRound(r, 8, D2D1::ColorF(0, 0, 0, 0.12f));
        Text(text, G.buttonBold.Get(), R(0, 0, w, h), C.onAccent, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (st.focus) StrokeRound(Inset(r, 2.5f), 5.5f, C.hc ? C.onAccent : D2D1::ColorF(1, 1, 1, 0.9f), 1.5f);
        return;
    }
    FillRound(r, 8, !st.enabled ? C.field : st.pressed ? C.pressed : st.hot ? C.hover : C.bg);
    StrokeRound(r, 8, st.enabled ? C.borderStrong : C.border, 1);
    Text(text, primary ? G.buttonBold.Get() : G.button.Get(), R(0, 0, w, h), st.enabled ? C.text : C.faint,
         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (st.focus) StrokeRound(Inset(R(0, 0, w, h), 1.5f), 7, C.focus, 2);
}

LRESULT CustomDraw(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND h = cd->hdr.hwndFrom;
    RECT rc;
    GetClientRect(h, &rc);
    if (!EnsureTarget() || FAILED(G.rt->BindDC(cd->hdc, &rc))) return CDRF_DODEFAULT;
    EnsureIcons();
    bool showFocus = !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    CtlState st;
    st.hot = (cd->uItemState & CDIS_HOT) != 0;
    st.pressed = (cd->uItemState & CDIS_SELECTED) != 0 || (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED);
    st.focus = (GetFocus() == h) && showFocus;
    st.enabled = IsWindowEnabled(h) != FALSE;
    st.checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring text(GetWindowTextLengthW(h), L'\0');
    if (!text.empty()) GetWindowTextW(h, &text[0], (int)text.size() + 1);
    float w = (rc.right - rc.left) / g_scale, hh = (rc.bottom - rc.top) / g_scale;

    G.rt->BeginDraw();
    G.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    switch (GetDlgCtrlID(h)) {
    case ID_USER: DrawSegment(w, hh, text, false, st); break;
    case ID_MACHINE: DrawSegment(w, hh, text, !IsElevated(), st); break;
    case ID_DESKTOP:
    case ID_ASSOC:
    case ID_REMOVE: DrawCheckBox(w, hh, text, st); break;
    case IDOK: DrawButton(w, hh, text, true, C.footer, st); break;
    case IDCANCEL: DrawButton(w, hh, text, false, C.footer, st); break;
    default: DrawButton(w, hh, text, false, C.bg, st); break;
    }
    if (G.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) DiscardTarget();
    return CDRF_SKIPDEFAULT;
}

HWND MakeControl(int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | style, 0, 0, 0, 0, g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void Place(HWND h, float x, float y, float w, float hh) {
    SetWindowPos(h, nullptr, Px(x), Px(y), Px(x + w) - Px(x), Px(y + hh) - Px(y), SWP_NOZORDER | SWP_NOACTIVATE);
}

float CheckWidth(HWND h) {
    std::wstring text(GetWindowTextLengthW(h), L'\0');
    if (!text.empty()) GetWindowTextW(h, &text[0], (int)text.size() + 1);
    return std::min(kBodyRight - kM + 4, TextWidth(text, G.body.Get()) + 40);
}

float ButtonWidth(HWND h, bool primary) {
    std::wstring text(GetWindowTextLengthW(h), L'\0');
    if (!text.empty()) GetWindowTextW(h, &text[0], (int)text.size() + 1);
    return std::max(primary ? 112.f : 92.f, std::ceil(TextWidth(text, primary ? G.buttonBold.Get() : G.button.Get()) + 44));
}

void Layout() {
    Place(hUser, kM + 4, 148, 240, 32);
    Place(hMachine, kM + 4 + 240, 148, 240, 32);
    Place(hChange, 424, 250, 100, 34);
    Place(hDesktop, kM - 4, 300, CheckWidth(hDesktop), 30);
    Place(hAssoc, kM - 4, 332, CheckWidth(hAssoc), 30);
    Place(hRemove, kM - 4, 238, CheckWidth(hRemove), 30);
    float y = ClientH() - 18 - 36;
    float okW = ButtonWidth(hOk, true), cancelW = ButtonWidth(hCancel, false);
    Place(hOk, kBodyRight - okW, y, okW, 36);
    Place(hCancel, kBodyRight - okW - 8 - cancelW, y, cancelW, 36);
}

bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetChecked(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
void Show(HWND h, bool on) { ShowWindow(h, on ? SW_SHOWNA : SW_HIDE); }

void SetPage(Page p) {
    S.page = p;
    Show(hUser, p == Page::Options);
    Show(hMachine, p == Page::Options);
    Show(hChange, p == Page::Options);
    Show(hDesktop, p == Page::Options);
    Show(hAssoc, p == Page::Options);
    Show(hRemove, p == Page::Confirm);

    const wchar_t *ok = L"", *cancel = L"Cancel";
    bool showOk = true, showCancel = true;
    std::wstring installLabel = PrimaryInstallLabel();
    switch (p) {
    case Page::Options: ok = installLabel.c_str(); break;
    case Page::Confirm: ok = L"Uninstall"; break;
    case Page::Progress: showOk = showCancel = false; break;
    case Page::Done:
        if (S.mode == UiMode::Install) { ok = L"Open MarkLite"; cancel = L"Close"; }
        else { ok = L"Close"; showCancel = false; }
        break;
    case Page::Running: ok = S.closing ? L"Closing\u2026" : L"Close MarkLite"; break;
    case Page::Error:
        if (S.errorFatal) { ok = L"Close"; showCancel = false; }
        else { ok = L"Try again"; cancel = L"Back"; }
        break;
    }
    SetWindowTextW(hOk, ok);
    SetWindowTextW(hCancel, cancel);
    EnableWindow(hOk, !(p == Page::Running && S.closing));
    Show(hOk, showOk);
    Show(hCancel, showCancel);
    EnableMenuItem(GetSystemMenu(g_wnd, FALSE), SC_CLOSE, MF_BYCOMMAND | (p == Page::Progress ? MF_GRAYED : MF_ENABLED));
    Layout();
    if (showOk && IsWindowEnabled(hOk)) SetFocus(hOk);
    else SetFocus(g_wnd);
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ------------------------------------------------------------------ actions
struct Job {
    bool install;
    InstallOptions opt;
    bool removeSettings;
    bool elevate;
};

DWORD WINAPI JobThread(LPVOID param) {
    Job* job = (Job*)param;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    auto progress = [](int pct, const wchar_t* step) { PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)pct, (LPARAM)step); };
    DWORD code;
    if (job->elevate) code = RunElevated(g_wnd, WorkerArgs(job->install, job->opt));
    else if (job->install) code = DoInstall(job->opt, progress);
    else code = DoUninstall(job->opt.scope, job->opt.dir, progress);
    // Settings live in the user's own profile, so they are removed here and never by the elevated worker.
    if (!job->install && code == SETUP_OK && job->removeSettings) RemoveUserSettings();
    PostMessageW(g_wnd, WM_APP_DONE, code, 0);
    delete job;
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

DWORD WINAPI CloseAppThread(LPVOID param) {
    std::wstring* dir = (std::wstring*)param;
    bool closed = CloseRunningInstances(*dir, 30000);
    delete dir;
    PostMessageW(g_wnd, WM_APP_CLOSED, closed, 0);
    return 0;
}

void ShowError(DWORD code) {
    S.errorCode = code;
    S.errorFatal = code == SETUP_ERR_PAYLOAD || code == SETUP_ERR_NOT_INSTALLED;
    S.exitCode = (int)code;
    SetPage(Page::Error);
}

void StartJob() {
    if (!RunningInstances(S.opt.dir).empty()) {
        S.stillOpen = false;
        SetPage(Page::Running);
        return;
    }
    S.hintError.clear();
    bool elevate = S.opt.scope == Scope::Machine && !IsElevated();
    Job* job = new Job{S.mode == UiMode::Install, S.opt, S.removeSettings, elevate};
    S.working = true;
    S.done = false;
    S.elevating = elevate;
    S.shown = 0;
    S.target = elevate ? 6 : 2;
    S.steps.assign(1, {0.0, elevate ? (S.mode == UiMode::Install ? L"Installing for all users" : L"Removing MarkLite for all users")
                                       : L"Starting"});
    SetPage(Page::Progress);
    SetTimer(g_wnd, kTimerProgress, 15, nullptr);
    HANDLE t = CreateThread(nullptr, 0, JobThread, job, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        DWORD e = GetLastError();
        delete job;
        S.working = false;
        KillTimer(g_wnd, kTimerProgress);
        ShowError(e);
    }
}

void OnJobDone(DWORD code) {
    S.working = false;
    if (code == SETUP_OK) {
        S.done = true;
        S.target = 100;
        return;   // the timer finishes the progress animation, then shows the result
    }
    KillTimer(g_wnd, kTimerProgress);
    if (code == SETUP_ERR_RUNNING) {
        S.stillOpen = false;
        SetPage(Page::Running);
    } else if (code == ERROR_CANCELLED && S.mode == UiMode::Install) {
        S.hintError = L"Installing for all users needs administrator permission. You can install just for you instead.";
        SetPage(Page::Options);
    } else {
        ShowError(code);
    }
}

void OnTick() {
    if (S.elevating && !S.done) S.target = std::min(88.0, S.target + (88.0 - S.target) * 0.015);
    double d = S.target - S.shown;
    S.shown = std::min(S.target, S.shown + d * 0.14 + (d > 0 ? 0.3 : 0));
    D2D1_RECT_F b = ProgressBarRect();
    RECT rc{Px(b.left) - 2, Px(b.top - 50), Px(b.right) + 2, Px(b.bottom + 34)};
    InvalidateRect(g_wnd, &rc, FALSE);
    if (S.done && S.shown >= 99.999) {
        KillTimer(g_wnd, kTimerProgress);
        S.exitCode = 0;
        SetPage(Page::Done);
    }
}

void OnScope(Scope s) {
    if (S.opt.scope == s) return;
    S.opt.scope = s;
    if (!S.dirCustomized) S.opt.dir = DefaultInstallDir(s);
    S.hintError.clear();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void OnChangeFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to install MarkLite");
    dlg->SetOkButtonLabel(L"Select folder");
    ComPtr<IShellItem> start;
    std::wstring parent = ParentDir(S.opt.dir);
    if (!parent.empty() && SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&start)))) dlg->SetFolder(start.Get());
    if (FAILED(dlg->Show(g_wnd))) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
    std::wstring dir = NormalizeDir(path);
    CoTaskMemFree(path);
    // Picking "D:\Apps" installs into "D:\Apps\MarkLite", like most installers do.
    size_t k = dir.find_last_of(L'\\');
    std::wstring last = k == std::wstring::npos ? dir : dir.substr(k + 1);
    if (_wcsicmp(last.c_str(), kAppName) != 0) dir = JoinPath(dir, kAppName);
    S.opt.dir = dir;
    S.dirCustomized = !SamePath(dir, DefaultInstallDir(S.opt.scope));
    S.hintError.clear();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void CloseApp() {
    S.closing = true;
    SetPage(Page::Running);
    HANDLE t = CreateThread(nullptr, 0, CloseAppThread, new std::wstring(S.opt.dir), 0, nullptr);
    if (t) CloseHandle(t);
    else PostMessageW(g_wnd, WM_APP_CLOSED, FALSE, 0);
}

void OnPrimary() {
    switch (S.page) {
    case Page::Options:
    case Page::Confirm: StartJob(); break;
    case Page::Running: CloseApp(); break;
    case Page::Done:
        if (S.mode == UiMode::Install) LaunchApp(JoinPath(S.opt.dir, kExeName));
        DestroyWindow(g_wnd);
        break;
    case Page::Error:
        if (S.errorFatal) DestroyWindow(g_wnd);
        else StartJob();
        break;
    case Page::Progress: break;
    }
}

void OnSecondary() {
    switch (S.page) {
    case Page::Options:
    case Page::Confirm:
    case Page::Done: DestroyWindow(g_wnd); break;
    case Page::Running:
    case Page::Error:
        if (S.errorFatal && S.page == Page::Error) DestroyWindow(g_wnd);
        else SetPage(S.mode == UiMode::Install ? Page::Options : Page::Confirm);
        break;
    case Page::Progress: break;
    }
}

// ------------------------------------------------------------------ window
void StyleTitleBar() {
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static Fn set = SysProc<Fn>(L"dwmapi.dll", "DwmSetWindowAttribute");
    if (!set) return;
    BOOL dark = C.dark;
    if (FAILED(set(g_wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark)))) set(g_wnd, 19, &dark, sizeof(dark));
    COLORREF caption = C.hc ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/ : C.caption;
    set(g_wnd, 35 /*DWMWA_CAPTION_COLOR, Windows 11*/, &caption, sizeof(caption));
}

void ApplyTheme() {
    LoadPalette();
    DiscardTarget();
    StyleTitleBar();
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

void SizeWindow(const RECT* suggested) {
    RECT r{0, 0, Px(kW), Px(ClientH())};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, g_dpi);
    int w = r.right - r.left, h = r.bottom - r.top, x, y;
    if (suggested) {
        x = suggested->left;
        y = suggested->top;
    } else {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        y = mi.rcWork.top + std::max(0, (int)(mi.rcWork.bottom - mi.rcWork.top - h) * 2 / 5);
    }
    SetWindowPos(g_wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NOTIFY:
        if (((NMHDR*)l)->code == NM_CUSTOMDRAW) return CustomDraw((NMCUSTOMDRAW*)l);
        break;
    case DM_GETDEFID:
        return (IsWindowVisible(hOk) && IsWindowEnabled(hOk)) ? MAKELRESULT(IDOK, DC_HASDEFID) : 0;
    case WM_COMMAND:
        if (HIWORD(w) != BN_CLICKED) break;
        switch (LOWORD(w)) {
        case IDOK:
            if (IsWindowVisible(hOk) && IsWindowEnabled(hOk)) OnPrimary();
            break;
        case IDCANCEL:
            if (IsWindowVisible(hCancel)) OnSecondary();
            else if (S.page == Page::Done || S.page == Page::Error) DestroyWindow(h);
            break;
        case ID_USER: OnScope(Scope::User); break;
        case ID_MACHINE: OnScope(Scope::Machine); break;
        case ID_CHANGE: OnChangeFolder(); break;
        case ID_DESKTOP: S.opt.desktopShortcut = Checked(hDesktop); break;
        case ID_ASSOC: S.opt.fileAssociations = Checked(hAssoc); break;
        case ID_REMOVE: S.removeSettings = Checked(hRemove); break;
        }
        return 0;
    case WM_TIMER:
        if (w == kTimerProgress) OnTick();
        return 0;
    case WM_APP_PROGRESS:
        S.target = std::max(S.target, (double)w);
        if (l && w < 100) S.steps.emplace_back((double)w, (const wchar_t*)l);
        return 0;
    case WM_APP_DONE: OnJobDone((DWORD)w); return 0;
    case WM_APP_CLOSED:
        S.closing = false;
        if (w) StartJob();
        else {
            S.stillOpen = true;
            SetPage(Page::Running);
        }
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(w);
        g_scale = g_dpi / 96.f;
        if (G.rt) G.rt->SetDpi((float)g_dpi, (float)g_dpi);
        G.icon.Reset();
        G.shield.Reset();
        SizeWindow((RECT*)l);
        Layout();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) ApplyTheme();
        break;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED: ApplyTheme(); break;
    case WM_CLOSE:
        if (S.working || S.closing || S.page == Page::Progress) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, kTimerProgress);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int RunUi(HINSTANCE inst, UiMode mode, const ExistingInstall& existing) {
    g_inst = inst;
    S.mode = mode;
    S.existing = existing;
    if (mode == UiMode::Install) {
        S.opt.scope = existing.found ? existing.scope : Scope::User;
        S.opt.dir = existing.found ? existing.dir : DefaultInstallDir(S.opt.scope);
        S.opt.desktopShortcut = existing.found ? existing.desktopShortcut : true;
        S.opt.fileAssociations = existing.found ? existing.fileAssociations : true;
    } else {
        S.opt.scope = existing.scope;
        S.opt.dir = existing.dir;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (!InitGraphics()) {
        MessageBoxW(nullptr, L"Setup couldn't start because Direct2D isn't available on this PC.", L"MarkLite Setup", MB_ICONERROR);
        return ERROR_NOT_SUPPORTED;
    }
    LoadPalette();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // Open on the monitor with the mouse pointer, sized for that monitor's DPI.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    const wchar_t* title = mode == UiMode::Install ? L"MarkLite Setup" : L"Uninstall MarkLite";
    g_wnd = CreateWindowExW(0, kClass, title, kStyle, mi.rcWork.left + 40, mi.rcWork.top + 40, 200, 200, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return (int)GetLastError();
    g_dpi = GetDpiForWindow(g_wnd);
    g_scale = g_dpi / 96.f;
    SizeWindow(nullptr);

    hUser = MakeControl(ID_USER, L"Just me", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP);
    hMachine = MakeControl(ID_MACHINE, L"All users", BS_AUTORADIOBUTTON);
    hChange = MakeControl(ID_CHANGE, L"Change\u2026", BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP);
    hDesktop = MakeControl(ID_DESKTOP, L"Create a desktop shortcut", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP);
    hAssoc = MakeControl(ID_ASSOC, L"Open .md files with MarkLite", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP);
    hRemove = MakeControl(ID_REMOVE, L"Also remove my settings", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP);
    hCancel = MakeControl(IDCANCEL, L"Cancel", BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP);
    hOk = MakeControl(IDOK, L"Install", BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP);

    SetChecked(S.opt.scope == Scope::User ? hUser : hMachine, true);
    SetChecked(hDesktop, S.opt.desktopShortcut);
    SetChecked(hAssoc, S.opt.fileAssociations);
    if (existing.found && mode == UiMode::Install) {
        // An existing installation is updated where it is.
        EnableWindow(hUser, FALSE);
        EnableWindow(hMachine, FALSE);
        EnableWindow(hChange, FALSE);
    }
    SendMessageW(g_wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
    StyleTitleBar();

    if (mode == UiMode::Uninstall && !existing.found) {
        S.errorCode = SETUP_ERR_NOT_INSTALLED;
        S.errorFatal = true;
        S.exitCode = (int)SETUP_ERR_NOT_INSTALLED;
        SetPage(Page::Error);
    } else {
        SetPage(mode == UiMode::Install ? Page::Options : Page::Confirm);
    }
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
    UpdateWindow(g_wnd);
    SetFocus(hOk);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return S.exitCode;
}
