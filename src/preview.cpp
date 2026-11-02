#include "preview.h"
#include "md4c.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <shellapi.h>
#include <unordered_map>
#include <map>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// ------------------------------------------------------------------ data model
enum : uint16_t { F_BOLD = 1, F_ITALIC = 2, F_STRIKE = 4, F_CODE = 8, F_LINK = 16, F_UNDER = 32, F_ALT = 64, F_SMALL = 128 };
enum BlockKind { BK_TEXT, BK_CODE, BK_HR, BK_TABLE };

struct Run { uint32_t start, len; uint16_t flags; int link; uint8_t color; };
struct ImgRef { uint32_t pos; std::wstring src; float w = 0, h = 0; };
struct Inline {
    std::wstring text;
    std::vector<Run> runs;
    std::vector<ImgRef> imgs;
};

struct ImageEntry {
    ComPtr<IWICBitmapSource> wic;
    ComPtr<ID2D1Bitmap> bmp;
    int gen = -1;
    float w = 0, h = 0;
    bool ok = false;
    uint64_t stamp = 0;      // file time and size when decoded
    ULONGLONG checked = 0;   // tick of the last look at the file
};

// Last write time and size of a file in one number (0 = no such file).
uint64_t FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    uint64_t t = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    uint64_t sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return (t ^ (sz * 0x9E3779B97F4A7C15ull)) | 1;
}

class ImageInline;

struct Cell {
    Inline in;
    int align = 0;   // 0 left, 1 center, 2 right
    ComPtr<IDWriteTextLayout> lay;
    std::vector<ComPtr<ImageInline>> objs;
    float minW = 0, maxW = 0, h = 0;
};

struct QuoteRef { int id; float x; int kind; };

struct Block {
    BlockKind kind = BK_TEXT;
    int heading = 0;
    bool html = false;
    Inline in;
    std::wstring lang;
    float indent = 0;               // unzoomed DIPs
    std::vector<QuoteRef> quotes;
    std::wstring marker;
    int task = 0;                   // 1 open, 2 done
    bool tight = false;
    int outerList = 0;              // id of the outermost list containing the block
    size_t src = (size_t)-1;
    std::wstring anchor;
    std::vector<std::vector<Cell>> rows;
    int headRows = 0;
    // layout
    uint64_t hash = 0;
    ComPtr<IDWriteTextLayout> lay;
    std::vector<ComPtr<ImageInline>> objs;
    bool measured = false;
    float layW = -1;
    float y = 0, h = 0;
    std::vector<float> colW, rowH;
};

// ------------------------------------------------------------------ state
struct PreviewState {
    HWND wnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1HwndRenderTarget> rt;
    int rtGen = 0;
    struct {
        ComPtr<ID2D1SolidColorBrush> fg, dim, link, code, codeBg, quote, border, heading, sel, stripe, headBg, accent, white, admon[6];
    } br;
    std::unordered_map<int, ComPtr<IDWriteTextFormat>> formats;
    float fmtZoom = 0;
    std::wstring bodyFont, codeFont;

    std::vector<Block> blocks;
    std::vector<std::wstring> links;
    std::wstring baseDir;
    std::map<std::wstring, std::shared_ptr<ImageEntry>> images;
    std::map<std::wstring, ULONGLONG> missing;   // image path -> when it was last found missing

    float zoom = 1.0f;
    float scrollY = 0, totalH = 0, viewW = 0, viewH = 0, x0 = 0, contentW = 0;
    int dpi = 96;
    bool layoutDirty = true;

    // selection: anchor (selAB, selAP) and focus (selFB, selFP) as block index + position in the
    // block's linear text (for tables: all cells in row-major order, one separator slot after each)
    bool selecting = false, hasSel = false, dragged = false;
    int selAB = -1, selFB = -1;
    UINT32 selAP = 0, selFP = 0;
    int selUnit = 0;                 // 0 characters, 1 words (double-click), 2 paragraphs (triple-click)
    int origB = -1;                  // the word / paragraph that started a unit-wise selection
    UINT32 origS = 0, origE = 0;
    POINT downPt{}, dblPt{};
    LONG lastDbl = 0;
    int hoverLink = -1;
} V;


// ------------------------------------------------------------------ helpers
uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template <class T> uint64_t FnvV(uint64_t h, const T& v) { return Fnv(h, &v, sizeof v); }
uint64_t FnvS(uint64_t h, const std::wstring& s) { h = FnvV(h, s.size()); return Fnv(h, s.data(), s.size() * 2); }

D2D1_COLOR_F Col(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

struct Entity { const char* name; unsigned cp; };
const Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7}, {"times", 0xD7},
    {"divide", 0xF7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5},
    {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"larr", 0x2190},
    {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"rArr", 0x21D2}, {"lArr", 0x21D0},
    {"hearts", 0x2665}, {"check", 0x2713}, {"emsp", 0x2003}, {"ensp", 0x2002}, {"thinsp", 0x2009}, {"zwj", 0x200D},
    {"aring", 0xE5}, {"auml", 0xE4}, {"ouml", 0xF6}, {"Aring", 0xC5}, {"Auml", 0xC4}, {"Ouml", 0xD6},
    {"eacute", 0xE9}, {"uuml", 0xFC}, {"szlig", 0xDF}, {"infin", 0x221E}, {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265},
};

void AppendCp(std::wstring& s, unsigned cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        s += (wchar_t)(0xD800 + (cp >> 10));
        s += (wchar_t)(0xDC00 + (cp & 0x3FF));
    } else s += (wchar_t)cp;
}

std::wstring DecodeEntity(const char* t, size_t n) {
    std::string e(t, n);
    std::wstring out;
    if (e.size() > 2 && e[0] == '&' && e.back() == ';') {
        std::string body = e.substr(1, e.size() - 2);
        if (!body.empty() && body[0] == '#') {
            unsigned cp = (body.size() > 1 && (body[1] == 'x' || body[1] == 'X')) ? (unsigned)strtoul(body.c_str() + 2, nullptr, 16)
                                                                                  : (unsigned)strtoul(body.c_str() + 1, nullptr, 10);
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;   // as CommonMark says
            AppendCp(out, cp);
            return out;
        }
        for (auto& en : kEntities)
            if (body == en.name) { AppendCp(out, en.cp); return out; }
    }
    return U8ToW(e);
}

std::wstring DecodeEntitiesIn(const std::string& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        size_t a = s.find('&', i);
        if (a == std::string::npos) { out += U8ToW(s.substr(i)); break; }
        out += U8ToW(s.substr(i, a - i));
        size_t b = s.find(';', a);
        if (b != std::string::npos && b - a < 12) { out += DecodeEntity(s.data() + a, b - a + 1); i = b + 1; }
        else { out += L'&'; i = a + 1; }
    }
    return out;
}

// An md4c attribute (link href, image src, code language) with its entities decoded: md4c hands
// them over as separate sub-strings, e.g. "?a=1" "&amp;" "b=2".
std::wstring AttrStr(const MD_ATTRIBUTE& a) {
    if (!a.text) return std::wstring();
    if (!a.substr_offsets || !a.substr_types) return U8ToW(a.text, a.size);
    std::wstring out;
    for (int i = 0; a.substr_offsets[i] < a.size; i++) {
        MD_OFFSET b = a.substr_offsets[i], e = a.substr_offsets[i + 1];
        if (a.substr_types[i] == MD_TEXT_ENTITY) out += DecodeEntity(a.text + b, e - b);
        else if (a.substr_types[i] == MD_TEXT_NULLCHAR) out += L'\xFFFD';
        else out += U8ToW(a.text + b, e - b);
    }
    return out;
}

std::wstring UrlDecode(const std::wstring& s) {
    std::string bytes = WToU8(s), out;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] == '%' && i + 2 < bytes.size() && isxdigit((unsigned char)bytes[i + 1]) && isxdigit((unsigned char)bytes[i + 2])) {
            out += (char)strtol(bytes.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += bytes[i];
    }
    return U8ToW(out);
}

bool IsRemote(const std::wstring& s) {
    std::wstring l = Lower(s.substr(0, 8));
    return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0 || l.rfind(L"data:", 0) == 0 || l.rfind(L"//", 0) == 0;
}

std::wstring ResolveLocal(const std::wstring& src) {
    std::wstring p = src;
    size_t q = p.find_first_of(L"?#");
    if (q != std::wstring::npos) p = p.substr(0, q);
    p = UrlDecode(p);
    if (Lower(p.substr(0, 8)) == L"file:///") p = p.substr(8);
    else if (Lower(p.substr(0, 7)) == L"file://") p = L"\\\\" + p.substr(7);   // file://server/share/x.png
    for (auto& c : p) if (c == L'/') c = L'\\';
    bool abs = (p.size() > 2 && p[1] == L':') || p.rfind(L"\\\\", 0) == 0;
    if (!abs) {
        if (V.baseDir.empty()) return L"";
        p = V.baseDir + L"\\" + p;
    }
    wchar_t full[MAX_PATH * 2];
    if (GetFullPathNameW(p.c_str(), MAX_PATH * 2, full, nullptr)) p = full;
    return p;
}

// Server name of a network path (\\server\share\... or \\?\UNC\server\...), lower case; "" otherwise.
std::wstring ServerOf(const std::wstring& p) {
    std::wstring r;
    if (Lower(p.substr(0, 8)) == L"\\\\?\\unc\\") r = p.substr(8);
    else if (p.rfind(L"\\\\", 0) == 0 && p.rfind(L"\\\\?\\", 0) != 0 && p.rfind(L"\\\\.\\", 0) != 0) r = p.substr(2);
    else return L"";
    return Lower(r.substr(0, r.find(L'\\')));
}

// Images load by themselves as soon as a document is shown, so a path from a document (which may come
// from anyone) must not open a device (aux.png and com1.png are devices, as is \\.\pipe\...) or make
// Windows connect to another computer, which sends it the user's login credentials. Ordinary drive
// paths are fine, and so are paths on the same server the document itself was opened from.
bool AutoLoadable(const std::wstring& p) {
    if (p.size() >= 3 && p[1] == L':' && p[2] == L'\\') return true;
    if (p.size() >= 7 && p.rfind(L"\\\\?\\", 0) == 0 && p[5] == L':' && p[6] == L'\\') return true;
    std::wstring srv = ServerOf(p);
    return !srv.empty() && srv == ServerOf(V.baseDir);
}

std::shared_ptr<ImageEntry> LoadImage(const std::wstring& path) {
    auto it = V.images.find(path);
    if (it != V.images.end()) {
        // look at the file again every few seconds: an edited or replaced image shows its new content
        auto& c = it->second;
        ULONGLONG t = GetTickCount64();
        if (t - c->checked < 2000) return c;
        c->checked = t;
        if (FileStamp(path) == c->stamp) return c;
        V.images.erase(it);
    }
    auto e = std::make_shared<ImageEntry>();
    if (path.empty() || !AutoLoadable(path)) return e;
    // A missing file isn't remembered for long, so the image shows up soon after the file appears.
    // Checking again at most every few seconds keeps a path on a slow or offline share from
    // stalling every re-render while typing.
    ULONGLONG now = GetTickCount64();
    auto miss = V.missing.find(path);
    if (miss != V.missing.end() && now - miss->second < 3000) return e;
    e->stamp = FileStamp(path);
    e->checked = now;
    if (!e->stamp) {
        V.missing[path] = now;
        return e;
    }
    V.missing.erase(path);
    V.images[path] = e;
    if (!V.wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&V.wic)))) return e;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(V.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return e;
    if (FAILED(dec->GetFrame(0, &frame))) return e;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return e;
    ComPtr<IWICBitmapSource> src = frame;
    const UINT maxDim = 2400;
    if (w > maxDim || h > maxDim) {
        double s = (double)maxDim / std::max(w, h);
        UINT nw = std::max(1u, (UINT)(w * s)), nh = std::max(1u, (UINT)(h * s));
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(V.wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(frame.Get(), nw, nh, WICBitmapInterpolationModeFant))) {
            src = sc;
            e->w = (float)w; e->h = (float)h;   // keep natural size for layout
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(V.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return e;
    // Decode once into memory so drawing doesn't touch the file again.
    ComPtr<IWICBitmap> mem;
    if (FAILED(V.wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &mem))) return e;
    e->wic = mem;
    if (e->w == 0) { e->w = (float)w; e->h = (float)h; }
    e->ok = true;
    return e;
}

// ------------------------------------------------------------------ md4c -> blocks
struct Ctr { int type; bool tight; unsigned num; int quoteId; int kind; bool pending; std::wstring marker; int task; float indentAt; std::wstring anchor; };
enum { C_QUOTE, C_UL, C_OL, C_LI };

struct Builder {
    const char* src = nullptr;
    size_t srcLen = 0;
    std::vector<Block>* out = nullptr;
    std::vector<Ctr> stack;
    int cur = -1;           // current text block
    int table = -1;
    bool inCell = false;
    std::vector<uint16_t> flags{0};
    std::vector<int> linkStack{-1};
    uint8_t color = 0;
    bool inImg = false;
    std::wstring imgSrc;
    int imgDepth = 0;
    int quoteCounter = 0;
    bool htmlComment = false;
    std::string htmlPending;   // start of a tag whose '>' comes in a later callback (attributes over lines)
    bool htmlSpace = false;    // whitespace seen in an HTML block, emitted before the next text
    size_t cellFlags = 1, cellLinks = 1;   // flags/linkStack depth when the current table cell began
    std::map<std::wstring, int> anchors;

    float IndentNow() const {
        float x = 0;
        for (auto& c : stack) {
            if (c.type == C_QUOTE) x += 22;
            else if (c.type == C_LI) x += 28;
        }
        return x;
    }

    Inline* Target() {
        if (table >= 0 && inCell) {
            auto& rows = (*out)[table].rows;
            if (!rows.empty() && !rows.back().empty()) return &rows.back().back().in;
        }
        if (cur < 0) {
            // text directly inside a tight list item: implicit paragraph
            NewBlock(BK_TEXT);
            (*out)[cur].tight = true;
        }
        return &(*out)[cur].in;
    }

    int NewBlock(BlockKind k) {
        Block b;
        b.kind = k;
        b.indent = IndentNow();
        float qx = 0;
        for (auto& c : stack) {
            if (c.type == C_QUOTE) { b.quotes.push_back({c.quoteId, qx, c.kind}); qx += 22; }
            else if (c.type == C_LI) qx += 28;
        }
        for (auto& c : stack)
            if (c.type == C_UL || c.type == C_OL) { b.outerList = c.quoteId; break; }
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (it->type == C_LI) {
                if (it->pending) { b.marker = it->marker; b.task = it->task; b.anchor = it->anchor; it->pending = false; }
                // tight list?
                for (auto jt = it + 1; jt != stack.rend(); ++jt)
                    if (jt->type == C_UL || jt->type == C_OL) { b.tight = jt->tight; break; }
                break;
            }
            if (it->type == C_QUOTE) break;
        }
        out->push_back(std::move(b));
        int idx = (int)out->size() - 1;
        if (k == BK_TEXT || k == BK_CODE) cur = idx;
        return idx;
    }

    void Close() {
        if (cur >= 0) {
            Block& b = (*out)[cur];
            if (b.kind == BK_CODE) {
                while (!b.in.text.empty() && (b.in.text.back() == L'\n' || b.in.text.back() == L'\r')) b.in.text.pop_back();
                for (auto& r : b.in.runs) if (r.start + r.len > b.in.text.size()) r.len = (uint32_t)b.in.text.size() - std::min<uint32_t>(r.start, (uint32_t)b.in.text.size());
            }
            if (b.html) {
                // trim and drop empty HTML blocks
                std::wstring& t = b.in.text;
                size_t e = t.size();
                while (e > 0 && (iswspace(t[e - 1]) || t[e - 1] == 0x2028)) e--;
                if (e < t.size()) {
                    t.resize(e);
                    for (auto& r : b.in.runs) { if (r.start > e) r.start = (uint32_t)e; if (r.start + r.len > e) r.len = (uint32_t)e - r.start; }
                }
                if (t.empty() && b.marker.empty()) { out->erase(out->begin() + cur); }
            }
        }
        cur = -1;
        // An unclosed inline tag (<b>, <a href>, or <a id="x"/>) ends with its block, as in a browser
        // paragraph; pure Markdown spans are always balanced, so this only drops HTML leftovers.
        if (!(table >= 0 && inCell)) {
            flags.resize(1);
            linkStack.resize(1);
        }
        htmlPending.clear();
        htmlSpace = false;
    }

    // A list item that starts with a nested list or quote: its marker gets a line of its own first,
    // instead of being drawn after the nested content.
    void MarkerFirst() {
        if (!stack.empty() && stack.back().type == C_LI && stack.back().pending) {
            NewBlock(BK_TEXT);
            cur = -1;
        }
    }

    // An image link nested in a link ([![badge](img.svg)](https://ci)) opens the outer link.
    int OuterLink() const { return linkStack.back(); }

    void SetSrc(const char* t) {
        if (t < src || t >= src + srcLen) return;
        size_t off = (size_t)(t - src);
        int idx = (table >= 0 && inCell) ? table : cur;
        if (idx >= 0 && (*out)[idx].src == (size_t)-1) (*out)[idx].src = off;
    }

    void Append(const std::wstring& s, uint16_t extra = 0) {
        if (s.empty()) return;
        Inline* in = Target();
        uint16_t f = flags.back() | extra;
        int link = linkStack.back();
        uint32_t start = (uint32_t)in->text.size();
        in->text += s;
        if (!in->runs.empty()) {
            Run& last = in->runs.back();
            if (last.flags == f && last.link == link && last.color == color && last.start + last.len == start) { last.len += (uint32_t)s.size(); return; }
        }
        in->runs.push_back({start, (uint32_t)s.size(), f, link, color});
    }

    void AddImage(const std::wstring& srcUrl, const std::wstring& alt, float w, float h) {
        if (IsRemote(srcUrl) || srcUrl.empty()) {
            std::wstring label = L"\U0001F5BC " + (alt.empty() ? std::wstring(L"image") : alt);
            if (OuterLink() >= 0) { Append(label, F_ALT | F_LINK); return; }   // badge: the enclosing link
            int link = (int)V.links.size();
            V.links.push_back(srcUrl);
            linkStack.push_back(link);
            Append(label, F_ALT | F_LINK);
            linkStack.pop_back();
            return;
        }
        std::wstring path = ResolveLocal(srcUrl);
        auto e = LoadImage(path);
        if (!e->ok) {
            Append(L"\U0001F5BC " + (alt.empty() ? srcUrl : alt), F_ALT);
            return;
        }
        Inline* in = Target();
        ImgRef r;
        r.pos = (uint32_t)in->text.size();
        r.src = path;
        r.w = w;
        r.h = h;
        Append(L"\xFFFC");
        in->imgs.push_back(r);
    }

    void Html(const char* t, size_t n) {
        std::string s = htmlPending;
        htmlPending.clear();
        s.append(t, n);
        size_t i = 0;
        bool block = cur >= 0 && (*out)[cur].html && !(table >= 0 && inCell);
        // whitespace collapsed in an HTML block: one space before the next visible text or inline tag
        auto flushSpace = [&] {
            if (!htmlSpace) return;
            htmlSpace = false;
            if (cur < 0) return;
            std::wstring& tx = (*out)[cur].in.text;
            if (!tx.empty() && tx.back() != L' ' && tx.back() != 0x2028) Append(L" ");
        };
        while (i < s.size()) {
            if (htmlComment) {
                size_t e = s.find("-->", i);
                if (e == std::string::npos) return;
                htmlComment = false;
                i = e + 3;
                continue;
            }
            // '<' starts a tag only before a letter, '/', '!' or '?' ("a < b" is text)
            bool tagStart = s[i] == '<' && (i + 1 >= s.size() || isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' ||
                                            s[i + 1] == '!' || s[i + 1] == '?');
            if (tagStart) {
                if (s.compare(i, 4, "<!--") == 0) { htmlComment = true; i += 4; continue; }
                size_t e = s.find('>', i);
                if (e == std::string::npos) {   // attributes continue on the next line: wait for the rest
                    if (s.size() - i < 8192) htmlPending = s.substr(i);
                    return;
                }
                std::string tag = s.substr(i + 1, e - i - 1);
                i = e + 1;
                bool closing = !tag.empty() && tag[0] == '/';
                size_t te = tag.find_last_not_of(" \t\r\n");
                bool selfClosing = !closing && te != std::string::npos && tag[te] == '/';
                size_t ns = closing ? 1 : 0, ne = ns;
                while (ne < tag.size() && (isalnum((unsigned char)tag[ne]))) ne++;
                std::string name = tag.substr(ns, ne - ns);
                for (auto& c : name) c = (char)tolower((unsigned char)c);
                auto attr = [&](const char* an) -> std::string {
                    std::string low = tag;
                    for (auto& c : low) c = (char)tolower((unsigned char)c);
                    size_t p = 0;
                    std::string key = std::string(an) + "=";
                    while ((p = low.find(key, p)) != std::string::npos) {
                        if (p == 0 || isspace((unsigned char)low[p - 1])) {
                            size_t v = p + key.size();
                            if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                                char q = tag[v];
                                size_t ve = tag.find(q, v + 1);
                                return tag.substr(v + 1, ve == std::string::npos ? std::string::npos : ve - v - 1);
                            }
                            size_t ve = v;
                            while (ve < tag.size() && !isspace((unsigned char)tag[ve]) && tag[ve] != '/') ve++;
                            return tag.substr(v, ve - v);
                        }
                        p++;
                    }
                    return std::string();
                };
                bool inlineTag = name == "img" || name == "a" || name == "b" || name == "strong" || name == "i" ||
                                 name == "em" || name == "code" || name == "kbd" || name == "samp" || name == "tt" ||
                                 name == "u" || name == "ins" || name == "s" || name == "del" || name == "strike";
                if (block && inlineTag && !closing) flushSpace();
                if (name == "br") { htmlSpace = false; Append(L"\x2028"); }
                else if (name == "img") {
                    float w = (float)atof(attr("width").c_str()), h = (float)atof(attr("height").c_str());
                    AddImage(U8ToW(attr("src")), DecodeEntitiesIn(attr("alt")), w, h);
                } else if (selfClosing) {
                    // <a id="top"/>, <b/>: nothing is opened
                } else if (name == "a") {
                    if (closing) {
                        if (linkStack.size() > 1) { linkStack.pop_back(); if (flags.size() > 1) flags.pop_back(); }
                    } else {
                        int link = (int)V.links.size();
                        V.links.push_back(DecodeEntitiesIn(attr("href")));
                        linkStack.push_back(link);
                        flags.push_back(flags.back() | F_LINK);
                    }
                } else if (name == "b" || name == "strong") {
                    if (closing) { if (flags.size() > 1) flags.pop_back(); } else flags.push_back(flags.back() | F_BOLD);
                } else if (name == "i" || name == "em") {
                    if (closing) { if (flags.size() > 1) flags.pop_back(); } else flags.push_back(flags.back() | F_ITALIC);
                } else if (name == "code" || name == "kbd" || name == "samp" || name == "tt") {
                    if (closing) { if (flags.size() > 1) flags.pop_back(); } else flags.push_back(flags.back() | F_CODE);
                } else if (name == "u" || name == "ins") {
                    if (closing) { if (flags.size() > 1) flags.pop_back(); } else flags.push_back(flags.back() | F_UNDER);
                } else if (name == "s" || name == "del" || name == "strike") {
                    if (closing) { if (flags.size() > 1) flags.pop_back(); } else flags.push_back(flags.back() | F_STRIKE);
                } else if (cur >= 0 && (*out)[cur].html &&
                           (name == "p" || name == "div" || name == "li" || name == "tr" || name == "h1" || name == "h2" || name == "h3" ||
                            name == "h4" || name == "h5" || name == "h6" || name == "details" || name == "summary" || name == "hr")) {
                    std::wstring& tx = (*out)[cur].in.text;
                    htmlSpace = false;
                    if (!tx.empty() && tx.back() != 0x2028) Append(L"\x2028");
                }
                continue;
            }
            // text up to the next tag (a '<' that doesn't start a tag is part of the text)
            size_t e = s.find('<', i + 1);
            while (e != std::string::npos && e + 1 < s.size() && !isalpha((unsigned char)s[e + 1]) && s[e + 1] != '/' &&
                   s[e + 1] != '!' && s[e + 1] != '?')
                e = s.find('<', e + 1);
            std::string txt = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? s.size() : e;
            if (block) {
                // collapse whitespace like a browser
                std::string c;
                for (char ch : txt) {
                    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') { htmlSpace = true; continue; }
                    if (htmlSpace) {
                        if (!c.empty()) c += ' ';
                        else {
                            std::wstring& tx = (*out)[cur].in.text;
                            if (!tx.empty() && tx.back() != L' ' && tx.back() != 0x2028) c += ' ';
                        }
                        htmlSpace = false;
                    }
                    c += ch;
                }
                txt = c;
            }
            if (!txt.empty()) Append(DecodeEntitiesIn(txt));
        }
    }
};

std::wstring Slug(const std::wstring& t) {
    std::wstring s;
    for (wchar_t c : t) {
        if (IsCharAlphaNumericW(c)) s += (wchar_t)towlower(c);
        else if (c == L' ' || c == L'-') s += L'-';
        else if (c == L'_') s += L'_';
    }
    return s;
}

int EnterBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    Builder& b = *(Builder*)ud;
    switch (type) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_QUOTE: b.Close(); b.MarkerFirst(); b.stack.push_back({C_QUOTE, false, 0, ++b.quoteCounter, 0, false, L"", 0, 0}); break;
    case MD_BLOCK_ADMONITION: {
        b.Close();
        b.MarkerFirst();
        auto* d = (MD_BLOCK_ADMONITION_DETAIL*)detail;
        std::wstring t = Lower(AttrStr(d->type));
        int kind = t == L"note" ? 1 : t == L"tip" ? 2 : t == L"important" ? 3 : t == L"warning" ? 4 : t == L"caution" ? 5 : 1;
        b.stack.push_back({C_QUOTE, false, 0, ++b.quoteCounter, kind, false, L"", 0, 0});
        b.NewBlock(BK_TEXT);
        static const wchar_t* titles[] = {L"", L"\x2139  Note", L"\U0001F4A1 Tip", L"\x2757 Important", L"\x26A0  Warning", L"\x26D4 Caution"};
        b.color = (uint8_t)kind;
        b.Append(titles[kind], F_BOLD);
        b.color = 0;
        b.Close();
        break;
    }
    case MD_BLOCK_UL: b.Close(); b.MarkerFirst(); b.stack.push_back({C_UL, ((MD_BLOCK_UL_DETAIL*)detail)->is_tight != 0, 0, ++b.quoteCounter, 0, false, L"", 0, 0}); break;
    case MD_BLOCK_OL: {
        b.Close();
        b.MarkerFirst();
        auto* d = (MD_BLOCK_OL_DETAIL*)detail;
        b.stack.push_back({C_OL, d->is_tight != 0, d->start, ++b.quoteCounter, 0, false, L"", 0, 0});
        break;
    }
    case MD_BLOCK_LI: {
        b.Close();
        auto* d = (MD_BLOCK_LI_DETAIL*)detail;
        Ctr li{C_LI, false, 0, 0, 0, true, L"", 0, 0};
        int depth = 0;
        for (auto& c : b.stack) if (c.type == C_UL || c.type == C_OL) depth++;
        if (!b.stack.empty() && b.stack.back().type == C_OL) {
            li.marker = std::to_wstring(b.stack.back().num++) + L".";
        } else {
            static const wchar_t* bullets[] = {L"\x2022", L"\x25E6", L"\x25AA"};
            li.marker = bullets[(depth - 1 + 3) % 3];
        }
        if (d && d->is_task) li.task = (d->task_mark == 'x' || d->task_mark == 'X') ? 2 : 1;
        b.stack.push_back(li);
        break;
    }
    case MD_BLOCK_FOOTNOTE_DEF_SECTION: b.Close(); b.NewBlock(BK_HR); break;
    case MD_BLOCK_FOOTNOTE_DEF: {
        b.Close();
        auto* d = (MD_BLOCK_FOOTNOTE_DEF_DETAIL*)detail;
        Ctr li{C_LI, false, 0, 0, 0, true, std::to_wstring(d->id) + L".", 0, 0, L"fn-" + std::to_wstring(d->id)};
        b.stack.push_back(li);
        break;
    }
    case MD_BLOCK_HR: b.Close(); b.NewBlock(BK_HR); break;
    case MD_BLOCK_H: {
        b.Close();
        int i = b.NewBlock(BK_TEXT);
        (*b.out)[i].heading = (int)((MD_BLOCK_H_DETAIL*)detail)->level;
        break;
    }
    case MD_BLOCK_CODE: {
        b.Close();
        int i = b.NewBlock(BK_CODE);
        auto* d = (MD_BLOCK_CODE_DETAIL*)detail;
        (*b.out)[i].lang = AttrStr(d->lang);
        break;
    }
    case MD_BLOCK_HTML: {
        b.Close();
        int i = b.NewBlock(BK_TEXT);
        (*b.out)[i].html = true;
        break;
    }
    case MD_BLOCK_P: b.Close(); b.NewBlock(BK_TEXT); break;
    case MD_BLOCK_TABLE: {
        b.Close();
        b.table = b.NewBlock(BK_TABLE);
        b.cur = -1;
        break;
    }
    case MD_BLOCK_THEAD: break;
    case MD_BLOCK_TBODY: break;
    case MD_BLOCK_TR:
        if (b.table >= 0) (*b.out)[b.table].rows.emplace_back();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (b.table >= 0 && !(*b.out)[b.table].rows.empty()) {
            Cell c;
            MD_ALIGN a = ((MD_BLOCK_TD_DETAIL*)detail)->align;
            c.align = a == MD_ALIGN_CENTER ? 1 : a == MD_ALIGN_RIGHT ? 2 : 0;
            (*b.out)[b.table].rows.back().push_back(std::move(c));
            b.inCell = true;
            b.cellFlags = b.flags.size();
            b.cellLinks = b.linkStack.size();
            if (type == MD_BLOCK_TH) b.flags.push_back(b.flags.back() | F_BOLD);
            else b.flags.push_back(b.flags.back());
        }
        break;
    default: break;
    }
    return 0;
}

int LeaveBlock(MD_BLOCKTYPE type, void* detail, void* ud) {
    Builder& b = *(Builder*)ud;
    (void)detail;
    switch (type) {
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_ADMONITION:
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        b.Close();
        if (!b.stack.empty()) b.stack.pop_back();
        break;
    case MD_BLOCK_LI:
    case MD_BLOCK_FOOTNOTE_DEF:
        b.Close();
        if (!b.stack.empty() && b.stack.back().pending) { b.NewBlock(BK_TEXT); b.cur = -1; }   // empty item keeps its marker
        if (!b.stack.empty()) b.stack.pop_back();
        break;
    case MD_BLOCK_H: {
        if (b.cur >= 0) {
            Block& blk = (*b.out)[b.cur];
            std::wstring s = Slug(blk.in.text);
            int n = b.anchors[s]++;
            blk.anchor = n ? s + L"-" + std::to_wstring(n) : s;
        }
        b.Close();
        break;
    }
    case MD_BLOCK_P:
    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
        b.Close();
        break;
    case MD_BLOCK_TABLE:
        if (b.table >= 0) (*b.out)[b.table].headRows = 1;
        b.table = -1;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (b.inCell) {
            b.inCell = false;
            if (b.flags.size() > b.cellFlags) b.flags.resize(std::max<size_t>(1, b.cellFlags));
            if (b.linkStack.size() > b.cellLinks) b.linkStack.resize(std::max<size_t>(1, b.cellLinks));
            b.htmlPending.clear();
        }
        break;
    default: break;
    }
    return 0;
}

int EnterSpan(MD_SPANTYPE type, void* detail, void* ud) {
    Builder& b = *(Builder*)ud;
    if (b.inImg) { if (type == MD_SPAN_IMG) b.imgDepth++; return 0; }
    uint16_t f = b.flags.back();
    switch (type) {
    case MD_SPAN_EM: f |= F_ITALIC; break;
    case MD_SPAN_STRONG: f |= F_BOLD; break;
    case MD_SPAN_DEL: f |= F_STRIKE; break;
    case MD_SPAN_CODE: f |= F_CODE; break;
    case MD_SPAN_U: f |= F_UNDER; break;
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY: f |= F_CODE; break;
    case MD_SPAN_A: {
        auto* d = (MD_SPAN_A_DETAIL*)detail;
        V.links.push_back(AttrStr(d->href));
        b.linkStack.push_back((int)V.links.size() - 1);
        f |= F_LINK;
        break;
    }
    case MD_SPAN_IMG: {
        auto* d = (MD_SPAN_IMG_DETAIL*)detail;
        b.inImg = true;
        b.imgDepth = 1;
        b.imgSrc = AttrStr(d->src);
        b.flags.push_back(f);
        b.Target();   // make sure a block exists
        V.links.push_back(L"");   // placeholder slot for alt text collection
        return 0;
    }
    case MD_SPAN_FOOTNOTE_REF: {
        auto* d = (MD_SPAN_FOOTNOTE_REF_DETAIL*)detail;
        V.links.push_back(L"#fn-" + std::to_wstring(d->id));
        b.linkStack.push_back((int)V.links.size() - 1);
        b.flags.push_back(f | F_LINK | F_SMALL);
        b.Append(L"[" + std::to_wstring(d->id) + L"]");
        return 0;
    }
    default: break;
    }
    b.flags.push_back(f);
    return 0;
}

int LeaveSpan(MD_SPANTYPE type, void* detail, void* ud) {
    Builder& b = *(Builder*)ud;
    (void)detail;
    if (b.inImg) {
        if (type == MD_SPAN_IMG && --b.imgDepth == 0) {
            b.inImg = false;
            std::wstring alt = V.links.back();
            V.links.pop_back();
            if (b.flags.size() > 1) b.flags.pop_back();
            b.AddImage(b.imgSrc, alt, 0, 0);
        }
        return 0;
    }
    if (type == MD_SPAN_A || type == MD_SPAN_FOOTNOTE_REF) { if (b.linkStack.size() > 1) b.linkStack.pop_back(); }
    if (b.flags.size() > 1) b.flags.pop_back();
    return 0;
}

int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    Builder& b = *(Builder*)ud;
    if (b.inImg) {   // collect alt text
        if (type == MD_TEXT_ENTITY) V.links.back() += DecodeEntity(text, size);
        else if (type != MD_TEXT_HTML) V.links.back() += U8ToW(text, size);
        return 0;
    }
    if (b.cur < 0 && !(b.table >= 0 && b.inCell) && type == MD_TEXT_HTML && b.htmlComment == false) {
        // inline html outside any block: ignore whitespace-only
    }
    switch (type) {
    case MD_TEXT_NORMAL: b.Target(); b.SetSrc(text); b.Append(U8ToW(text, size)); break;
    case MD_TEXT_CODE:
    case MD_TEXT_LATEXMATH: b.Target(); b.SetSrc(text); b.Append(U8ToW(text, size)); break;
    case MD_TEXT_NULLCHAR: b.Append(L"\xFFFD"); break;
    case MD_TEXT_BR: b.Append(L"\x2028"); break;
    case MD_TEXT_SOFTBR: b.Append(L" "); break;
    case MD_TEXT_ENTITY: b.Target(); b.SetSrc(text); b.Append(DecodeEntity(text, size)); break;
    case MD_TEXT_HTML: b.Target(); b.SetSrc(text); b.Html(text, size); break;
    default: b.Append(U8ToW(text, size)); break;
    }
    return 0;
}

// Is t[0..n) a line of 3+ `ch` (up to 3 spaces before, spaces after)?
static bool RunLine(const char* t, size_t n, char ch) {
    size_t i = 0, run = 0;
    while (i < n && i < 3 && t[i] == ' ') i++;
    while (i < n && t[i] == ch) { i++; run++; }
    while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) i++;
    return run >= 3 && i == n;
}

// YAML front matter: "---" on the first line, closed by "---" or "..." on a later one. Returns the
// length up to and including the closing line (0 = none) and the text between the two lines.
size_t FrontMatter(const char* text, size_t len, std::string& inner) {
    const char* nl = (const char*)memchr(text, '\n', len);
    if (!nl || !RunLine(text, (size_t)(nl - text), '-')) return 0;
    size_t bodyStart = (size_t)(nl - text) + 1, pos = bodyStart;
    while (pos < len) {
        const char* e = (const char*)memchr(text + pos, '\n', len - pos);
        size_t lineEnd = e ? (size_t)(e - text) : len;
        if (RunLine(text + pos, lineEnd - pos, '-') || RunLine(text + pos, lineEnd - pos, '.')) {
            inner.assign(text + bodyStart, pos - bodyStart);
            while (!inner.empty() && (inner.back() == '\n' || inner.back() == '\r')) inner.pop_back();
            return e ? lineEnd + 1 : len;
        }
        pos = lineEnd + 1;
    }
    return 0;   // never closed: an ordinary rule
}

void BuildBlocks(const char* text, size_t len, std::vector<Block>& out) {
    V.links.clear();
    Builder b;
    b.src = text;
    b.srcLen = len;
    b.out = &out;
    std::string fm;
    size_t skip = FrontMatter(text, len, fm);
    if (skip) {
        Block f;
        f.kind = BK_CODE;
        f.lang = L"YAML";
        f.src = 0;
        f.in.text = U8ToW(fm);
        f.in.text.erase(std::remove(f.in.text.begin(), f.in.text.end(), L'\r'), f.in.text.end());
        if (!f.in.text.empty()) f.in.runs.push_back({0, (uint32_t)f.in.text.size(), 0, -1, 0});
        out.push_back(std::move(f));
    }
    MD_PARSER p{};
    p.abi_version = 0;
    p.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    p.enter_block = EnterBlock;
    p.leave_block = LeaveBlock;
    p.enter_span = EnterSpan;
    p.leave_span = LeaveSpan;
    p.text = Text;
    md_parse(text + skip, (MD_SIZE)(len - skip), &p, &b);   // offsets stay relative to `text`
    b.Close();
    // make source offsets monotonic
    size_t last = 0;
    for (auto& blk : out) {
        if (blk.src == (size_t)-1 || blk.src < last) blk.src = last;
        last = blk.src;
    }
    // hashes
    for (auto& blk : out) {
        uint64_t h = 1469598103934665603ull;
        h = FnvV(h, blk.kind); h = FnvV(h, blk.heading); h = FnvV(h, blk.indent); h = FnvV(h, blk.task); h = FnvV(h, blk.tight);
        h = FnvS(h, blk.in.text); h = FnvS(h, blk.marker); h = FnvS(h, blk.lang);
        for (auto& r : blk.in.runs) { h = FnvV(h, r.start); h = FnvV(h, r.len); h = FnvV(h, r.flags); h = FnvV(h, r.color); }
        auto stampOf = [](const std::wstring& src) -> uint64_t {
            auto it = V.images.find(src);
            return it == V.images.end() ? 0 : it->second->stamp;
        };
        for (auto& im : blk.in.imgs) { h = FnvS(h, im.src); h = FnvV(h, im.w); h = FnvV(h, im.h); h = FnvV(h, im.pos); h = FnvV(h, stampOf(im.src)); }
        for (auto& row : blk.rows) {
            h = FnvV(h, row.size());
            for (auto& c : row) {
                h = FnvS(h, c.in.text); h = FnvV(h, c.align);
                for (auto& r : c.in.runs) { h = FnvV(h, r.start); h = FnvV(h, r.len); h = FnvV(h, r.flags); }
                for (auto& im : c.in.imgs) { h = FnvS(h, im.src); h = FnvV(h, stampOf(im.src)); }
            }
        }
        blk.hash = h;
    }
}

// ------------------------------------------------------------------ inline image object
class ImageInline : public IDWriteInlineObject {
    LONG ref = 1;
public:
    std::shared_ptr<ImageEntry> e;
    float w, h;
    ImageInline(std::shared_ptr<ImageEntry> en, float ww, float hh) : e(std::move(en)), w(ww), h(hh) {}
    virtual ~ImageInline() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown*) override {
        if (!V.rt || !e->ok) return S_OK;
        if (e->gen != V.rtGen || !e->bmp) {
            e->bmp.Reset();
            V.rt->CreateBitmapFromWicBitmap(e->wic.Get(), nullptr, &e->bmp);
            e->gen = V.rtGen;
        }
        if (e->bmp) V.rt->DrawBitmap(e->bmp.Get(), D2D1::RectF(x, y, x + w, y + h), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        m->width = w;
        m->height = h;
        m->baseline = h;
        m->supportsSideways = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override {
        o->left = o->top = o->right = o->bottom = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* b, DWRITE_BREAK_CONDITION* a) override {
        *b = *a = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }
};

// ------------------------------------------------------------------ formats & brushes
float Z(float v) { return v * V.zoom; }
const float kBase = 15.0f;

bool FontExists(const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fc;
    if (FAILED(V.dw->GetSystemFontCollection(&fc))) return false;
    UINT32 idx;
    BOOL exists = FALSE;
    fc->FindFamilyName(name, &idx, &exists);
    return exists != FALSE;
}

enum { FMT_BODY = 0, FMT_H1 = 1, FMT_CODE = 10, FMT_MARK = 11, FMT_LANG = 12 };

IDWriteTextFormat* Fmt(int which) {
    if (V.fmtZoom != V.zoom) { V.formats.clear(); V.fmtZoom = V.zoom; }
    auto it = V.formats.find(which);
    if (it != V.formats.end()) return it->second.Get();
    if (V.bodyFont.empty()) {
        V.bodyFont = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    }
    V.codeFont = FontExists(g_set.codeFont.c_str()) ? g_set.codeFont : L"Consolas";
    static const float hs[] = {0, 2.0f, 1.5f, 1.25f, 1.0f, 0.9f, 0.85f};
    ComPtr<IDWriteTextFormat> f;
    float size = Z(kBase);
    const wchar_t* face = V.bodyFont.c_str();
    DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL;
    if (which >= FMT_H1 && which <= 6) { size = Z(kBase * hs[which]); wt = which <= 2 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD; }
    if (which == FMT_CODE) { size = Z(kBase * 0.87f); face = V.codeFont.c_str(); }
    if (which == FMT_LANG) { size = Z(kBase * 0.75f); }
    V.dw->CreateTextFormat(face, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return nullptr;
    if (which == FMT_MARK) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    V.formats[which] = f;
    return f.Get();
}

void UpdateBrushColors() {
    if (!V.rt) return;
    const Palette& p = Pal();
    bool dark = Theme_IsDark();
    auto set = [&](ComPtr<ID2D1SolidColorBrush>& b, D2D1_COLOR_F c) {
        if (!b) V.rt->CreateSolidColorBrush(c, &b);
        else b->SetColor(c);
    };
    set(V.br.fg, Col(p.fg));
    set(V.br.dim, Col(p.dim));
    set(V.br.link, Col(p.link));
    set(V.br.code, Col(p.codeFg));
    set(V.br.codeBg, Col(dark ? RGB(0x26, 0x27, 0x2A) : RGB(0xF4, 0xF5, 0xF7)));
    set(V.br.quote, Col(dark ? RGB(0x3D, 0x44, 0x4D) : RGB(0xD0, 0xD7, 0xDE)));
    set(V.br.border, Col(dark ? RGB(0x3D, 0x44, 0x4D) : RGB(0xD8, 0xDE, 0xE4)));
    set(V.br.heading, Col(p.heading));
    set(V.br.sel, Col(p.selBg, dark ? 0.75f : 0.9f));
    set(V.br.stripe, Col(dark ? RGB(0x23, 0x24, 0x26) : RGB(0xF6, 0xF8, 0xFA)));
    set(V.br.headBg, Col(dark ? RGB(0x26, 0x28, 0x2B) : RGB(0xF0, 0xF2, 0xF4)));
    set(V.br.accent, Col(p.accent));
    set(V.br.white, Col(RGB(255, 255, 255)));
    COLORREF ad[6] = {p.quote, RGB(0x0969DA & 0xFF, 0x69, 0xDA), RGB(0x1A, 0x7F, 0x37), RGB(0x82, 0x50, 0xDF), RGB(0x9A, 0x67, 0x00), RGB(0xCF, 0x22, 0x2E)};
    if (dark) { ad[1] = RGB(0x4C, 0x9A, 0xFF); ad[2] = RGB(0x3F, 0xB9, 0x50); ad[3] = RGB(0xA3, 0x71, 0xF7); ad[4] = RGB(0xD2, 0x99, 0x22); ad[5] = RGB(0xF8, 0x51, 0x49); }
    else ad[1] = RGB(0x09, 0x69, 0xDA);
    for (int i = 0; i < 6; i++) set(V.br.admon[i], Col(ad[i]));
}

void DiscardLayouts() {
    for (auto& b : V.blocks) {
        b.lay.Reset();
        b.objs.clear();
        for (auto& r : b.rows) for (auto& c : r) { c.lay.Reset(); c.objs.clear(); }
    }
}

bool CreateRT() {
    if (V.rt) return true;
    RECT rc;
    GetClientRect(V.wnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), (float)V.dpi, (float)V.dpi);
    if (FAILED(V.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(V.wnd, D2D1::SizeU(rc.right, rc.bottom)), &V.rt)))
        return false;
    V.rtGen++;
    V.br = {};
    DiscardLayouts();   // drawing effects reference brushes of the old target
    UpdateBrushColors();
    V.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

// ------------------------------------------------------------------ layout
ComPtr<IDWriteTextLayout> MakeLayout(const Inline& in, IDWriteTextFormat* fmt, float width, float baseSize, bool code,
                                     std::vector<ComPtr<ImageInline>>& objs, bool headingDim = false) {
    ComPtr<IDWriteTextLayout> lay;
    const std::wstring& t = in.text;
    V.dw->CreateTextLayout(t.empty() ? L"" : t.c_str(), (UINT32)t.size(), fmt, std::max(1.0f, width), 1e7f, &lay);
    if (!lay) return lay;
    objs.clear();
    for (auto& r : in.runs) {
        DWRITE_TEXT_RANGE tr{r.start, r.len};
        if (r.flags & F_BOLD) lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, tr);
        if (r.flags & (F_ITALIC | F_ALT)) lay->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, tr);
        if (r.flags & F_STRIKE) lay->SetStrikethrough(TRUE, tr);
        if (r.flags & F_UNDER) lay->SetUnderline(TRUE, tr);
        if (r.flags & F_CODE && !code) {
            lay->SetFontFamilyName(V.codeFont.c_str(), tr);
            lay->SetFontSize(baseSize * 0.88f, tr);
            lay->SetDrawingEffect(V.br.code.Get(), tr);
        }
        if (r.flags & F_SMALL) lay->SetFontSize(baseSize * 0.75f, tr);
        if (r.flags & F_ALT) lay->SetDrawingEffect(V.br.dim.Get(), tr);
        if (r.flags & F_LINK) lay->SetDrawingEffect(V.br.link.Get(), tr);
        if (r.color) lay->SetDrawingEffect(V.br.admon[r.color].Get(), tr);
    }
    if (headingDim) lay->SetDrawingEffect(V.br.dim.Get(), {0, (UINT32)t.size()});
    bool hasImg = false;
    for (auto& im : in.imgs) {
        auto it = V.images.find(im.src);
        if (it == V.images.end() || !it->second->ok) continue;
        auto& e = it->second;
        if (e->w <= 0 || e->h <= 0) continue;
        float w, h;
        if (im.w > 0 && im.h > 0) { w = im.w; h = im.h; }
        else if (im.w > 0) { w = im.w; h = e->h * im.w / e->w; }
        else if (im.h > 0) { h = im.h; w = e->w * im.h / e->h; }
        else { w = e->w; h = e->h; }
        w = Z(w);
        h = Z(h);
        float maxW = std::max(16.0f, width - 2);
        if (w > maxW) { h = h * maxW / w; w = maxW; }
        ComPtr<ImageInline> obj;
        obj.Attach(new ImageInline(e, w, h));
        lay->SetInlineObject(obj.Get(), {im.pos, 1});
        objs.push_back(obj);
        hasImg = true;
    }
    if (!hasImg) {
        float ls = code ? 1.45f : 1.55f;
        lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, baseSize * ls, baseSize * ls * 0.78f);
    }
    return lay;
}

float SpaceBefore(const Block& b) {
    if (b.kind == BK_TEXT && b.heading) return Z(b.heading <= 2 ? 26.f : 20.f);
    if (b.kind == BK_HR) return Z(14.f);
    return 0;
}
float SpaceAfter(const Block& b) {
    if (b.kind == BK_TEXT && b.heading) return Z(b.heading <= 2 ? 14.f : 8.f);
    if (b.kind == BK_TEXT && b.tight) return Z(3.f);
    if (b.kind == BK_HR) return Z(14.f);
    return Z(13.f);
}

void EnsureLayout(Block& b, float width) {
    float pad = Z(14.f);
    if (b.kind == BK_HR) { b.h = Z(2.f); b.measured = true; b.layW = width; return; }
    if (b.kind == BK_TEXT || b.kind == BK_CODE) {
        bool code = b.kind == BK_CODE;
        float w = code ? width - 2 * pad : width;
        if (b.lay && b.layW != width && b.in.imgs.empty()) {
            b.lay->SetMaxWidth(std::max(1.0f, w));
        } else if (!b.lay || b.layW != width) {
            int fmt = code ? FMT_CODE : (b.heading ? FMT_H1 + b.heading - 1 : FMT_BODY);
            static const float hs[] = {1, 2.0f, 1.5f, 1.25f, 1.0f, 0.9f, 0.85f};
            float size = code ? Z(kBase * 0.87f) : Z(kBase * (b.heading ? hs[b.heading] : 1.0f));
            b.lay = MakeLayout(b.in, Fmt(fmt), w, size, code, b.objs, b.heading == 6);
            if (b.lay && b.heading) {
                b.lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.3f, size * 1.0f);
            }
            if (b.lay && code) b.lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        b.layW = width;
        if (!b.lay) { b.h = 0; return; }
        DWRITE_TEXT_METRICS m{};
        b.lay->GetMetrics(&m);
        b.h = code ? m.height + 2 * pad : m.height;
        if (b.heading && b.heading <= 2) b.h += Z(8.f);
        b.measured = true;
        return;
    }
    // table
    size_t cols = 0;
    for (auto& r : b.rows) cols = std::max(cols, r.size());
    if (!cols) { b.h = 0; b.measured = true; return; }
    float cp = Z(10.f), rp = Z(6.f);
    float size = Z(kBase * 0.95f);
    std::vector<float> mn(cols, Z(30.f)), mx(cols, Z(30.f));
    for (auto& r : b.rows)
        for (size_t c = 0; c < r.size(); c++) {
            Cell& cell = r[c];
            if (!cell.lay || b.layW != width) {
                cell.lay = MakeLayout(cell.in, Fmt(FMT_BODY), 1e5f, size, false, cell.objs);
                if (!cell.lay) continue;
                cell.lay->SetFontSize(size, {0, (UINT32)cell.in.text.size()});
                for (auto& rr : cell.in.runs)
                    if (rr.flags & F_CODE) cell.lay->SetFontSize(size * 0.88f, {rr.start, rr.len});
                if (cell.in.imgs.empty()) cell.lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.4f, size * 1.08f);
                else cell.lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
                cell.lay->SetMaxWidth(1e5f);
                DWRITE_TEXT_METRICS m{};
                cell.lay->GetMetrics(&m);
                cell.maxW = m.widthIncludingTrailingWhitespace;
                FLOAT mw = 0;
                cell.lay->DetermineMinWidth(&mw);
                cell.minW = mw;
                cell.lay->SetTextAlignment(cell.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : cell.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            mn[c] = std::max(mn[c], cell.minW + 2 * cp);
            mx[c] = std::max(mx[c], cell.maxW + 2 * cp);
        }
    float sumMin = 0, sumMax = 0;
    for (size_t c = 0; c < cols; c++) { sumMin += mn[c]; sumMax += mx[c]; }
    b.colW.assign(cols, 0);
    for (size_t c = 0; c < cols; c++) {
        if (sumMax <= width) b.colW[c] = mx[c];
        else if (sumMin >= width) b.colW[c] = mn[c];
        else b.colW[c] = mn[c] + (mx[c] - mn[c]) * (width - sumMin) / (sumMax - sumMin);
    }
    b.rowH.assign(b.rows.size(), 0);
    float total = 0;
    for (size_t r = 0; r < b.rows.size(); r++) {
        float rh = Z(20.f);
        for (size_t c = 0; c < b.rows[r].size(); c++) {
            Cell& cell = b.rows[r][c];
            if (!cell.lay) continue;
            cell.lay->SetMaxWidth(std::max(1.0f, b.colW[c] - 2 * cp));
            DWRITE_TEXT_METRICS m{};
            cell.lay->GetMetrics(&m);
            cell.h = m.height;
            rh = std::max(rh, m.height);
        }
        b.rowH[r] = rh + 2 * rp;
        total += b.rowH[r];
    }
    b.h = total + 1;
    b.layW = width;
    b.measured = true;
}

void LayoutAll() {
    RECT rc;
    GetClientRect(V.wnd, &rc);
    V.viewW = rc.right * 96.0f / V.dpi;
    V.viewH = rc.bottom * 96.0f / V.dpi;
    float pad = Z(28.f);
    V.contentW = std::min(Z(860.f), std::max(Z(120.f), V.viewW - 2 * pad - 14));
    V.x0 = std::max(pad, (V.viewW - V.contentW) / 2);
    float y = Z(22.f);
    float prevAfter = 0;
    for (size_t i = 0; i < V.blocks.size(); i++) {
        Block& b = V.blocks[i];
        if (i > 0) {
            float gap = std::max(prevAfter, SpaceBefore(b));
            const Block& pb = V.blocks[i - 1];
            if (pb.tight && pb.outerList != b.outerList) gap = std::max(gap, Z(13.f));   // end of a tight list
            y += gap;
        }
        b.y = y;
        float w = V.contentW - Z(b.indent);
        if (!b.measured || b.layW != w) EnsureLayout(b, w);
        y += b.h;
        prevAfter = SpaceAfter(b);
    }
    V.totalH = y + Z(40.f);
    V.layoutDirty = false;
}

void ClampScroll() {
    float maxS = std::max(0.0f, V.totalH - V.viewH);
    if (V.scrollY > maxS) V.scrollY = maxS;
    if (V.scrollY < 0) V.scrollY = 0;
}

void UpdateScrollbar() {
    SCROLLINFO si{sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL};
    float k = V.dpi / 96.0f;
    si.nMin = 0;
    si.nMax = (int)(V.totalH * k);
    si.nPage = (UINT)(V.viewH * k);
    si.nPos = (int)(V.scrollY * k);
    SetScrollInfo(V.wnd, SB_VERT, &si, TRUE);
}

// Release layouts far from the viewport for big documents (keeps measured heights).
void TrimLayouts() {
    if (V.blocks.size() < 250) return;
    float lo = V.scrollY - 3 * V.viewH, hi = V.scrollY + 4 * V.viewH;
    for (auto& b : V.blocks) {
        if (b.y + b.h < lo || b.y > hi) {
            if (b.lay) { b.lay.Reset(); b.objs.clear(); }
            for (auto& r : b.rows) for (auto& c : r) { c.lay.Reset(); c.objs.clear(); }
        }
    }
}

int BlockAtY(float y) {
    int lo = 0, hi = (int)V.blocks.size() - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (V.blocks[mid].y <= y) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

void EnsureLive(Block& b) {
    float w = V.contentW - Z(b.indent);
    bool missing = (b.kind == BK_TEXT || b.kind == BK_CODE) ? !b.lay : false;
    if (b.kind == BK_TABLE) for (auto& r : b.rows) for (auto& c : r) if (!c.lay && !c.in.text.empty()) missing = true;
    if (missing) { b.layW = -1; EnsureLayout(b, w); }
}

// ------------------------------------------------------------------ selection text model
// Length of a block's linear selection space. Tables concatenate their cells row by row,
// each cell followed by one separator slot, so a selection can start and end inside cells.
UINT32 BlockLen(const Block& b) {
    if (b.kind == BK_TABLE) {
        UINT32 n = 0;
        for (auto& r : b.rows) for (auto& c : r) n += (UINT32)c.in.text.size() + 1;
        return n;
    }
    if (b.kind == BK_HR) return 0;
    return (UINT32)b.in.text.size();
}

// The text holding linear position pos (a table cell, or the block's own text) and where it starts.
const std::wstring* TextAt(const Block& b, UINT32 pos, UINT32& base) {
    base = 0;
    if (b.kind != BK_TABLE) return &b.in.text;
    const std::wstring* last = nullptr;
    UINT32 n = 0;
    for (auto& r : b.rows)
        for (auto& c : r) {
            UINT32 len = (UINT32)c.in.text.size();
            if (pos <= n + len) { base = n; return &c.in.text; }
            last = &c.in.text;
            base = n;
            n += len + 1;
        }
    return last;
}

bool IsWordChar(wchar_t c) { return IsCharAlphaNumericW(c) || c == L'_'; }
bool IsHiSur(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLoSur(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
// Combining marks, variation selectors: part of the character before them.
bool IsMark(wchar_t c) {
    if (c >= 0xFE00 && c <= 0xFE0F) return true;
    WORD t = 0;
    return GetStringTypeW(CT_CTYPE3, &c, 1, &t) && (t & C3_NONSPACING) && !(c >= 0xD800 && c <= 0xDFFF);
}

// Word around character index ch (linear). Whitespace runs and single symbols count as words.
void WordRange(const Block& b, UINT32 ch, UINT32& s, UINT32& e) {
    UINT32 base = 0;
    const std::wstring* tp = TextAt(b, ch, base);
    s = e = ch;
    if (!tp || tp->empty()) return;
    const std::wstring& t = *tp;
    size_t n = t.size(), i = std::min<size_t>(ch - base, n - 1);
    // start on the character itself, not on the second half of a pair or on a mark after it
    for (;;) {
        if (i > 0 && IsLoSur(t[i]) && IsHiSur(t[i - 1])) { i--; continue; }
        if (i > 0 && (IsMark(t[i]) || t[i] == 0x200D)) { i--; continue; }
        if (i >= 2 && t[i] == 0xD83C && i + 1 < n && t[i + 1] >= 0xDFFB && t[i + 1] <= 0xDFFF && IsLoSur(t[i - 1])) { i -= 2; continue; }   // skin tone
        if (i >= 2 && t[i - 1] == 0x200D) { i -= 2; continue; }   // joined to the emoji before
        break;
    }
    auto cpLen = [&](size_t k) -> size_t { return (k + 1 < n && IsHiSur(t[k]) && IsLoSur(t[k + 1])) ? 2 : 1; };
    size_t a = i, z = i + cpLen(i);
    auto apos = [&](size_t k) {   // apostrophe inside a word: don't, it's
        return (t[k] == L'\'' || t[k] == 0x2019) && k > 0 && k + 1 < n && IsWordChar(t[k - 1]) && IsWordChar(t[k + 1]);
    };
    if (IsWordChar(t[i]) || apos(i)) {
        while (a > 0 && (IsWordChar(t[a - 1]) || apos(a - 1) || IsMark(t[a - 1]))) a--;
        while (z < n && (IsWordChar(t[z]) || apos(z) || IsMark(t[z]))) z++;
    } else if (t[i] == L' ' || t[i] == L'\t' || t[i] == 0xA0) {
        auto sp = [&](wchar_t c) { return c == L' ' || c == L'\t' || c == 0xA0; };
        while (a > 0 && sp(t[a - 1])) a--;
        while (z < n && sp(t[z])) z++;
    } else {
        // one symbol or emoji, with its marks, skin tone and zero-width-joined parts
        while (z < n) {
            if (t[z] == 0x200D && z + 1 < n) { z++; z += cpLen(z); continue; }
            if (IsMark(t[z])) { z++; continue; }
            if (z + 1 < n && t[z] == 0xD83C && t[z + 1] >= 0xDFFB && t[z + 1] <= 0xDFFF) { z += 2; continue; }
            break;
        }
    }
    if (z > n) z = n;
    s = base + (UINT32)a;
    e = base + (UINT32)z;
}

// Paragraph around ch: the whole block, one line of a code block, or one table cell.
void ParaRange(const Block& b, UINT32 ch, UINT32& s, UINT32& e) {
    UINT32 base = 0;
    const std::wstring* tp = TextAt(b, ch, base);
    s = e = ch;
    if (!tp) return;
    const std::wstring& t = *tp;
    size_t a = 0, z = t.size();
    if (b.kind == BK_CODE && !t.empty()) {
        size_t i = std::min<size_t>(ch - base, t.size() - 1);
        a = i == 0 ? std::wstring::npos : t.rfind(L'\n', i - 1);
        a = a == std::wstring::npos ? 0 : a + 1;
        z = t.find(L'\n', i);
        if (z == std::wstring::npos) z = t.size();
    }
    s = base + (UINT32)a;
    e = base + (UINT32)z;
}

// ------------------------------------------------------------------ painting
UINT32 SelStart(int bi, UINT32 len) {
    int ab = V.selAB, fb = V.selFB;
    UINT32 ap = V.selAP, fp = V.selFP;
    if (ab > fb || (ab == fb && ap > fp)) { std::swap(ab, fb); std::swap(ap, fp); }
    if (bi < ab || bi > fb) return UINT32_MAX;
    UINT32 s = bi == ab ? ap : 0;
    return std::min(s, len);
}
UINT32 SelEnd(int bi, UINT32 len) {
    int ab = V.selAB, fb = V.selFB;
    UINT32 ap = V.selAP, fp = V.selFP;
    if (ab > fb || (ab == fb && ap > fp)) { std::swap(ab, fb); std::swap(ap, fp); }
    UINT32 e = bi == fb ? fp : len;
    return std::min(e, len);
}

void DrawRanges(IDWriteTextLayout* lay, float ox, float oy, UINT32 s, UINT32 len, ID2D1Brush* brush, float rad, float padX) {
    if (!len) return;
    UINT32 n = 0;
    lay->HitTestTextRange(s, len, ox, oy, nullptr, 0, &n);
    if (!n) return;
    std::vector<DWRITE_HIT_TEST_METRICS> hm(n);
    lay->HitTestTextRange(s, len, ox, oy, hm.data(), n, &n);
    for (auto& m : hm) {
        D2D1_RECT_F r = D2D1::RectF(m.left - padX, m.top, m.left + m.width + padX, m.top + m.height);
        if (rad > 0) V.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush);
        else V.rt->FillRectangle(r, brush);
    }
}

// Metrics of a layout's first line. GetLineMetrics fails unless the buffer holds every line, so a
// 1-entry buffer only works for single-line layouts.
bool FirstLineMetrics(IDWriteTextLayout* lay, DWRITE_LINE_METRICS& out) {
    if (!lay) return false;
    UINT32 n = 0;
    DWRITE_LINE_METRICS one{};
    HRESULT hr = lay->GetLineMetrics(&one, 1, &n);
    if (SUCCEEDED(hr)) { if (!n) return false; out = one; return true; }
    if (hr != E_NOT_SUFFICIENT_BUFFER || !n) return false;
    std::vector<DWRITE_LINE_METRICS> all(n);
    if (FAILED(lay->GetLineMetrics(all.data(), n, &n)) || !n) return false;
    out = all[0];
    return true;
}

void DrawMarker(Block& b, float x, float y) {
    if (b.task) {
        float sz = Z(14.f);
        float lineTop = y;
        DWRITE_LINE_METRICS lm{};
        if (FirstLineMetrics(b.lay.Get(), lm)) lineTop = y + (lm.height - sz) / 2;
        D2D1_RECT_F r = D2D1::RectF(x - Z(22.f), lineTop, x - Z(22.f) + sz, lineTop + sz);
        auto rr = D2D1::RoundedRect(r, Z(3.f), Z(3.f));
        if (b.task == 2) {
            V.rt->FillRoundedRectangle(rr, V.br.accent.Get());
            ID2D1PathGeometry* g = nullptr;
            V.d2d->CreatePathGeometry(&g);
            if (g) {
                ID2D1GeometrySink* sink = nullptr;
                g->Open(&sink);
                sink->BeginFigure(D2D1::Point2F(r.left + sz * 0.22f, r.top + sz * 0.52f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(r.left + sz * 0.42f, r.top + sz * 0.72f));
                sink->AddLine(D2D1::Point2F(r.left + sz * 0.78f, r.top + sz * 0.3f));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                sink->Release();
                V.rt->DrawGeometry(g, V.br.white.Get(), Z(1.8f));
                g->Release();
            }
        } else {
            V.rt->DrawRoundedRectangle(rr, V.br.dim.Get(), Z(1.2f));
        }
        if (b.marker == L"\x2022" || b.marker == L"\x25E6" || b.marker == L"\x25AA") return;
    }
    if (b.marker.empty()) return;
    IDWriteTextFormat* f = Fmt(FMT_MARK);
    ComPtr<IDWriteTextLayout> ml;
    float mw = Z(40.f);
    V.dw->CreateTextLayout(b.marker.c_str(), (UINT32)b.marker.size(), f, mw, 100, &ml);
    if (!ml) return;
    bool bullet = b.marker.size() == 1;
    if (bullet) ml->SetFontSize(Z(kBase * 1.0f), {0, 1});
    float baseBlock = 0, baseMark = 0;
    DWRITE_LINE_METRICS lm{};
    if (FirstLineMetrics(ml.Get(), lm)) baseMark = lm.baseline;
    if (b.kind == BK_TEXT || b.kind == BK_CODE) { if (FirstLineMetrics(b.lay.Get(), lm)) baseBlock = lm.baseline; }
    else baseBlock = baseMark;   // table / rule: marker at the top
    float tx = x - mw - Z(b.task ? 28.f : 8.f);
    V.rt->DrawTextLayout(D2D1::Point2F(tx, y + baseBlock - baseMark), ml.Get(), V.br.dim.Get());
}

void DrawBlock(int bi, float prevBottom) {
    Block& b = V.blocks[bi];
    EnsureLive(b);
    float x = V.x0 + Z(b.indent), y = b.y;
    float w = V.contentW - Z(b.indent);
    // quote bars
    for (size_t q = 0; q < b.quotes.size(); q++) {
        float qx = V.x0 + Z(b.quotes[q].x);
        float top = y;
        if (bi > 0) {
            const Block& p = V.blocks[bi - 1];
            if (q < p.quotes.size() && p.quotes[q].id == b.quotes[q].id) top = prevBottom;
            else top = y - Z(4.f);
        }
        ID2D1Brush* br = b.quotes[q].kind ? (ID2D1Brush*)V.br.admon[b.quotes[q].kind].Get() : (ID2D1Brush*)V.br.quote.Get();
        V.rt->FillRectangle(D2D1::RectF(qx + Z(2.f), top, qx + Z(5.5f), y + b.h + Z(4.f)), br);
    }
    bool selActive = V.hasSel;
    switch (b.kind) {
    case BK_HR:
        DrawMarker(b, x, y - Z(8.f));
        V.rt->FillRectangle(D2D1::RectF(x, y, x + w, y + Z(2.f)), V.br.border.Get());
        break;
    case BK_CODE: {
        float pad = Z(14.f);
        DrawMarker(b, x, y + pad);
        V.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + b.h), Z(6.f), Z(6.f)), V.br.codeBg.Get());
        if (!b.lang.empty()) {
            ComPtr<IDWriteTextLayout> ll;
            V.dw->CreateTextLayout(b.lang.c_str(), (UINT32)b.lang.size(), Fmt(FMT_LANG), w - Z(16.f), 50, &ll);
            if (ll) {
                ll->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                V.rt->DrawTextLayout(D2D1::Point2F(x + Z(8.f), y + Z(5.f)), ll.Get(), V.br.dim.Get());
            }
        }
        if (b.lay) {
            if (selActive) {
                UINT32 len = (UINT32)b.in.text.size(), s = SelStart(bi, len);
                if (s != UINT32_MAX) DrawRanges(b.lay.Get(), x + pad, y + pad, s, SelEnd(bi, len) - s, V.br.sel.Get(), 0, 0);
            }
            V.rt->DrawTextLayout(D2D1::Point2F(x + pad, y + pad), b.lay.Get(), V.br.code.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
        break;
    }
    case BK_TEXT: {
        if (!b.lay) break;
        DrawMarker(b, x, y);
        for (auto& r : b.in.runs)
            if ((r.flags & F_CODE)) DrawRanges(b.lay.Get(), x, y, r.start, r.len, V.br.codeBg.Get(), Z(4.f), Z(3.f));
        if (selActive) {
            UINT32 len = (UINT32)b.in.text.size(), s = SelStart(bi, len);
            if (s != UINT32_MAX) DrawRanges(b.lay.Get(), x, y, s, SelEnd(bi, len) - s, V.br.sel.Get(), 0, 0);
        }
        ID2D1Brush* fg = b.heading ? (ID2D1Brush*)V.br.heading.Get() : (ID2D1Brush*)V.br.fg.Get();
        if (!b.quotes.empty() && !b.heading && b.quotes.back().kind == 0) fg = V.br.dim.Get();
        V.rt->DrawTextLayout(D2D1::Point2F(x, y), b.lay.Get(), fg, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (b.heading && b.heading <= 2)
            V.rt->FillRectangle(D2D1::RectF(x, y + b.h - Z(1.f), x + w, y + b.h), V.br.border.Get());
        break;
    }
    case BK_TABLE: {
        float cp = Z(10.f), rp = Z(6.f);
        float ty = y;
        DrawMarker(b, x, y + rp);
        float tw = 0;
        for (float c : b.colW) tw += c;
        // selected span in the table's linear text (cells row-major, one separator slot each)
        UINT32 ss = UINT32_MAX, se = 0;
        if (selActive) {
            UINT32 tl = BlockLen(b);
            ss = SelStart(bi, tl);
            if (ss != UINT32_MAX) se = SelEnd(bi, tl);
            if (se <= ss) ss = UINT32_MAX;
        }
        UINT32 off = 0;
        for (size_t r = 0; r < b.rows.size() && r < b.rowH.size(); r++) {
            float rh = b.rowH[r];
            if ((int)r < b.headRows) V.rt->FillRectangle(D2D1::RectF(x, ty, x + tw, ty + rh), V.br.headBg.Get());
            else if (r % 2 == 0) V.rt->FillRectangle(D2D1::RectF(x, ty, x + tw, ty + rh), V.br.stripe.Get());
            float cx = x;
            for (size_t c = 0; c < b.colW.size() && c < b.rows[r].size(); c++) {
                Cell& cell = b.rows[r][c];
                UINT32 len = (UINT32)cell.in.text.size();
                if (cell.lay)
                    for (auto& run : cell.in.runs)
                        if (run.flags & F_CODE) DrawRanges(cell.lay.Get(), cx + cp, ty + rp, run.start, run.len, V.br.codeBg.Get(), Z(4.f), Z(3.f));
                if (ss != UINT32_MAX) {
                    if (ss <= off && se > off + len) {   // selection runs through the whole cell
                        V.rt->FillRectangle(D2D1::RectF(cx, ty, cx + b.colW[c], ty + rh), V.br.sel.Get());
                    } else if (cell.lay) {
                        UINT32 a = std::max(ss, off), z = std::min(se, off + len);
                        if (z > a) DrawRanges(cell.lay.Get(), cx + cp, ty + rp, a - off, z - a, V.br.sel.Get(), 0, 0);
                    }
                }
                if (cell.lay)
                    V.rt->DrawTextLayout(D2D1::Point2F(cx + cp, ty + rp), cell.lay.Get(), V.br.fg.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                off += len + 1;
                cx += b.colW[c];
            }
            for (size_t c = b.colW.size(); c < b.rows[r].size(); c++) off += (UINT32)b.rows[r][c].in.text.size() + 1;
            ty += rh;
        }
        // grid
        float gx = x;
        ID2D1Brush* g = V.br.border.Get();
        for (size_t c = 0; c <= b.colW.size(); c++) {
            V.rt->FillRectangle(D2D1::RectF(gx, y, gx + 1, y + b.h - 1), g);
            if (c < b.colW.size()) gx += b.colW[c];
        }
        float gy = y;
        for (size_t r = 0; r <= b.rows.size(); r++) {
            V.rt->FillRectangle(D2D1::RectF(x, gy, x + tw + 1, gy + 1), g);
            if (r < b.rows.size()) gy += b.rowH[r];
        }
        break;
    }
    }
}

void Paint() {
    PAINTSTRUCT ps;
    BeginPaint(V.wnd, &ps);
    EndPaint(V.wnd, &ps);
    if (!CreateRT()) return;
    if (V.layoutDirty) { LayoutAll(); ClampScroll(); UpdateScrollbar(); }
    V.rt->BeginDraw();
    V.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    V.rt->Clear(Col(Pal().bg));
    V.rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -V.scrollY));
    if (!V.blocks.empty()) {
        int first = BlockAtY(V.scrollY - Z(40.f));
        if (first > 0) first--;
        float bottom = V.scrollY + V.viewH + Z(40.f);
        for (int i = first; i < (int)V.blocks.size() && V.blocks[i].y <= bottom; i++) {
            float prevBottom = i > 0 ? V.blocks[i - 1].y + V.blocks[i - 1].h : 0;
            DrawBlock(i, prevBottom);
        }
    } else {
        ComPtr<IDWriteTextLayout> l;
        const wchar_t* msg = L"Nothing to preview yet.";
        V.dw->CreateTextLayout(msg, (UINT32)wcslen(msg), Fmt(FMT_BODY), V.viewW, 100, &l);
        if (l) {
            l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            V.rt->DrawTextLayout(D2D1::Point2F(0, V.scrollY + Z(40.f)), l.Get(), V.br.dim.Get());
        }
    }
    HRESULT hr = V.rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        V.rt.Reset();
        InvalidateRect(V.wnd, nullptr, FALSE);
    }
    TrimLayouts();
}

// ------------------------------------------------------------------ hit testing
// block: block index; pos: caret position (between characters) in the block's linear text;
// ch: the character under the point (for word / paragraph selection); link: link under the point.
struct Hit { int block = -1; UINT32 pos = 0, ch = 0; int link = -1; };

Hit HitTest(float px, float py) {
    Hit h;
    if (V.blocks.empty()) return h;
    float y = py + V.scrollY;
    int bi = BlockAtY(y);
    // prefer the block whose vertical span contains y; otherwise nearest
    if (bi + 1 < (int)V.blocks.size() && y > V.blocks[bi].y + V.blocks[bi].h) {
        float gapMid = (V.blocks[bi].y + V.blocks[bi].h + V.blocks[bi + 1].y) / 2;
        if (y > gapMid) bi++;
    }
    Block& b = V.blocks[bi];
    EnsureLive(b);
    h.block = bi;
    UINT32 len = BlockLen(b);
    // above or below the block (in a gap, or past the first / last block): snap to its start / end
    if (y < b.y) return h;
    if (y > b.y + b.h) { h.pos = len; h.ch = len ? len - 1 : 0; return h; }
    float x = V.x0 + Z(b.indent);
    if ((b.kind == BK_TEXT || b.kind == BK_CODE) && b.lay) {
        float ox = b.kind == BK_CODE ? Z(14.f) : 0, oy = ox;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        b.lay->HitTestPoint(px - x - ox, y - b.y - oy, &trailing, &inside, &m);
        h.pos = std::min(len, m.textPosition + (trailing ? m.length : 0));
        h.ch = m.textPosition;
        if (inside)
            for (auto& r : b.in.runs)
                if (r.link >= 0 && m.textPosition >= r.start && m.textPosition < r.start + r.len) { h.link = r.link; break; }
    } else if (b.kind == BK_TABLE && !b.rows.empty()) {
        // row under y (clamped), then the column under px (clamped to the row's cells)
        size_t r = 0;
        float cy = b.y;
        while (r + 1 < b.rows.size() && r < b.rowH.size() && y >= cy + b.rowH[r]) cy += b.rowH[r++];
        UINT32 off = 0;
        for (size_t i = 0; i < r; i++) for (auto& c : b.rows[i]) off += (UINT32)c.in.text.size() + 1;
        const auto& row = b.rows[r];
        if (row.empty()) { h.pos = h.ch = off; return h; }
        size_t c = 0;
        float cx = x;
        while (c + 1 < row.size() && c < b.colW.size() && px >= cx + b.colW[c]) {
            off += (UINT32)row[c].in.text.size() + 1;
            cx += b.colW[c++];
        }
        const Cell& cell = row[c];
        UINT32 clen = (UINT32)cell.in.text.size();
        if (cell.lay) {
            BOOL tr = FALSE, in = FALSE;
            DWRITE_HIT_TEST_METRICS m{};
            cell.lay->HitTestPoint(px - cx - Z(10.f), y - cy - Z(6.f), &tr, &in, &m);
            h.pos = off + std::min(clen, m.textPosition + (tr ? m.length : 0));
            h.ch = off + m.textPosition;
            if (in)
                for (auto& run : cell.in.runs)
                    if (run.link >= 0 && m.textPosition >= run.start && m.textPosition < run.start + run.len) { h.link = run.link; break; }
        } else {
            h.pos = h.ch = off;
        }
    }
    return h;
}

// Plain text of t[s, e): image placeholders dropped, line breaks as CRLF.
void AppendPlain(std::wstring& out, const std::wstring& t, size_t s, size_t e) {
    e = std::min(e, t.size());
    for (size_t i = s; i < e; i++) {
        wchar_t c = t[i];
        if (c == 0xFFFC) continue;
        if (c == 0x2028 || c == L'\n') out += L"\r\n";
        else out += c;
    }
}

std::wstring BlockPlainText(const Block& b, UINT32 s, UINT32 e) {
    std::wstring out;
    if (b.kind == BK_TABLE) {
        // selected cells: tab between cells of a row, newline between rows
        UINT32 off = 0;
        size_t lastRow = SIZE_MAX;
        for (size_t r = 0; r < b.rows.size(); r++)
            for (auto& c : b.rows[r]) {
                UINT32 len = (UINT32)c.in.text.size(), a = std::max(s, off), z = std::min(e, off + len);
                if (z > a || (s <= off && e > off + len)) {
                    if (lastRow != SIZE_MAX) out += r == lastRow ? L"\t" : L"\r\n";
                    if (z > a) AppendPlain(out, c.in.text, a - off, z - off);
                    lastRow = r;
                }
                off += len + 1;
            }
        return out;
    }
    if (b.kind == BK_HR) return out;
    AppendPlain(out, b.in.text, s, e);
    return out;
}

void CopySelection() {
    if (!V.hasSel) return;
    int ab = V.selAB, fb = V.selFB;
    UINT32 ap = V.selAP, fp = V.selFP;
    if (ab > fb || (ab == fb && ap > fp)) { std::swap(ab, fb); std::swap(ap, fp); }
    if (ab < 0) return;
    std::wstring out;
    bool blankAfter = false;
    for (int i = ab; i <= fb && i < (int)V.blocks.size(); i++) {
        const Block& b = V.blocks[i];
        UINT32 len = BlockLen(b);
        UINT32 s = i == ab ? std::min(ap, len) : 0, e = i == fb ? std::min(fp, len) : len;
        if (s >= e) continue;   // nothing of this block is selected
        std::wstring piece = BlockPlainText(b, s, e);
        if (piece.empty()) continue;   // e.g. a paragraph holding only an image
        if (!out.empty()) out += blankAfter ? L"\r\n\r\n" : L"\r\n";
        if (!b.marker.empty() && s == 0 && b.kind == BK_TEXT) out += (b.marker.size() == 1 ? L"- " : b.marker + L" ");
        out += piece;
        blankAfter = b.kind == BK_TEXT && !b.tight;
    }
    if (out.empty() || !OpenClipboard(V.wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (out.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), out.c_str(), (out.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

void UnitRange(const Hit& h, int unit, UINT32& s, UINT32& e) {
    const Block& b = V.blocks[h.block];
    if (unit == 1) WordRange(b, h.ch, s, e);
    else ParaRange(b, h.ch, s, e);
}

// Double-click (unit 1) / triple-click (unit 2): select the word or paragraph under the point.
void StartUnitSelection(const Hit& h, int unit) {
    UINT32 s, e;
    UnitRange(h, unit, s, e);
    V.selUnit = unit;
    V.origB = V.selAB = V.selFB = h.block;
    V.origS = V.selAP = s;
    V.origE = V.selFP = e;
    V.hasSel = e > s;
}

// Move the selection focus to the point while dragging; unit-wise selections grow by whole units
// and always keep the word / paragraph they started from.
void ExtendSelection(const Hit& h) {
    if (h.block < 0) return;
    if (V.selUnit == 0 || V.origB < 0) {
        V.selFB = h.block;
        V.selFP = h.pos;
    } else {
        UINT32 s, e;
        UnitRange(h, V.selUnit, s, e);
        V.selAB = V.origB;
        V.selFB = h.block;
        if (h.block < V.origB || (h.block == V.origB && s < V.origS)) {
            V.selAP = V.origE;
            V.selFP = s;
        } else {
            V.selAP = V.origS;
            V.selFP = std::max(e, h.block == V.origB ? V.origE : 0);
        }
    }
    V.hasSel = !(V.selAB == V.selFB && V.selAP == V.selFP);
}

void SelectAll() {
    if (V.blocks.empty()) return;
    V.hasSel = true;
    V.selUnit = 0;
    V.selAB = 0;
    V.selAP = 0;
    V.selFB = (int)V.blocks.size() - 1;
    V.selFP = std::max<UINT32>(1, BlockLen(V.blocks.back()));
    InvalidateRect(V.wnd, nullptr, FALSE);
}

void ClearSelection() {
    V.hasSel = false;
    V.selAB = V.selFB = V.origB = -1;
    V.selUnit = 0;
}

void EndSelecting() {
    if (!V.selecting) return;
    V.selecting = false;
    KillTimer(V.wnd, 2);
    if (GetCapture() == V.wnd) ReleaseCapture();
}

void ScrollTo(float y) {
    float old = V.scrollY;
    V.scrollY = y;
    ClampScroll();
    if (old != V.scrollY) {
        UpdateScrollbar();
        InvalidateRect(V.wnd, nullptr, FALSE);
    }
}

void OpenLink(int link) {
    if (link < 0 || link >= (int)V.links.size()) return;
    std::wstring url = V.links[link];
    if (!url.empty() && url[0] == L'#') {
        std::wstring a = Lower(UrlDecode(url.substr(1)));
        for (auto& b : V.blocks)
            if (b.heading && b.anchor == a) { ScrollTo(b.y - Z(10.f)); return; }
        if (a.rfind(L"fn-", 0) == 0) {   // footnote: scroll to its definition
            for (auto& b : V.blocks) if (!b.heading && b.anchor == a) { ScrollTo(b.y - Z(10.f)); return; }
        }
        return;
    }
    PostMessageW(GetParent(V.wnd), WM_APP_PREVIEW_LINK, 0, (LPARAM) new std::wstring(url));
}

void Rebuild(const char* text, size_t len) {
    std::vector<Block> nb;
    BuildBlocks(text, len, nb);
    // reuse layouts from the previous generation when blocks are unchanged
    std::unordered_multimap<uint64_t, size_t> old;
    for (size_t i = 0; i < V.blocks.size(); i++) old.emplace(V.blocks[i].hash, i);
    std::vector<int> moved(V.blocks.size(), -1);   // old block index -> new index (unchanged blocks)
    for (size_t ni = 0; ni < nb.size(); ni++) {
        Block& b = nb[ni];
        auto it = old.find(b.hash);
        if (it == old.end()) continue;
        moved[it->second] = (int)ni;
        Block& o = V.blocks[it->second];
        b.lay = std::move(o.lay);
        b.objs = std::move(o.objs);
        b.measured = o.measured;
        b.layW = o.layW;
        b.h = o.h;
        b.colW = o.colW;
        b.rowH = o.rowH;
        if (b.rows.size() == o.rows.size()) {
            for (size_t r = 0; r < b.rows.size(); r++)
                for (size_t c = 0; c < b.rows[r].size() && c < o.rows[r].size(); c++) {
                    Cell& nc = b.rows[r][c];
                    Cell& oc = o.rows[r][c];
                    nc.lay = std::move(oc.lay);
                    nc.objs = std::move(oc.objs);
                    nc.minW = oc.minW;
                    nc.maxW = oc.maxW;
                    nc.h = oc.h;
                }
        }
        old.erase(it);
    }
    V.blocks.swap(nb);
    // keep the selection (even mid-drag) as long as the blocks it starts and ends in are unchanged
    auto map = [&](int i) { return i >= 0 && i < (int)moved.size() ? moved[i] : -1; };
    int a = map(V.selAB), f = map(V.selFB);
    if (a >= 0 && f >= 0) {
        V.selAB = a;
        V.selFB = f;
        V.origB = map(V.origB);
    } else {
        EndSelecting();
        ClearSelection();
    }
    V.layoutDirty = true;
}

}
void Preview_SetZoom(HWND h, float z);
namespace {
LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        V.wnd = h;
        V.dpi = Dpi(h);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_SIZE: {
        if (V.rt) V.rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        float oldW = V.viewW;
        RECT rc;
        GetClientRect(h, &rc);
        float nw = rc.right * 96.0f / V.dpi;
        if (std::fabs(nw - oldW) > 0.5f) {
            // width changed: re-layout shortly (coalesces resize drags)
            SetTimer(h, 1, V.blocks.size() > 400 ? 120 : 15, nullptr);
        } else {
            V.viewH = rc.bottom * 96.0f / V.dpi;
            ClampScroll();
            UpdateScrollbar();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (w == 1) {
            KillTimer(h, 1);
            V.layoutDirty = true;
            InvalidateRect(h, nullptr, FALSE);
        } else if (w == 2) {   // auto-scroll while the pointer is dragged above / below the view
            if (!V.selecting || GetCapture() != h) { KillTimer(h, 2); return 0; }
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            RECT rc;
            GetClientRect(h, &rc);
            float dist = pt.y < 0 ? (float)pt.y : pt.y >= rc.bottom ? (float)(pt.y - rc.bottom + 1) : 0.f;
            if (dist != 0) {
                dist *= 96.0f / V.dpi;   // faster the further the pointer is outside
                float step = std::min(600.f, 8.f + std::fabs(dist) * 0.9f);
                ScrollTo(V.scrollY + (dist < 0 ? -step : step));
                SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
            }
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        V.dpi = Dpi(h);
        if (V.rt) V.rt->SetDpi((float)V.dpi, (float)V.dpi);
        V.layoutDirty = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(h, SB_VERT, &si);
        float k = V.dpi / 96.0f, line = Z(40.f);
        switch (LOWORD(w)) {
        case SB_LINEUP: ScrollTo(V.scrollY - line); break;
        case SB_LINEDOWN: ScrollTo(V.scrollY + line); break;
        case SB_PAGEUP: ScrollTo(V.scrollY - V.viewH * 0.9f); break;
        case SB_PAGEDOWN: ScrollTo(V.scrollY + V.viewH * 0.9f); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: ScrollTo(si.nTrackPos / k); break;
        case SB_TOP: ScrollTo(0); break;
        case SB_BOTTOM: ScrollTo(V.totalH); break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
            Preview_SetZoom(h, V.zoom * (d > 0 ? 1.1f : 1 / 1.1f));
            return 0;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) ScrollTo(V.scrollY - d / 120.0f * std::max(Z(22.f), V.viewH - Z(44.f)));
        else ScrollTo(V.scrollY - d / 120.0f * Z(22.f) * (float)lines);
        if (V.selecting) {   // keep extending the selection to the text now under the pointer
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (w) {
        case VK_UP: ScrollTo(V.scrollY - Z(40.f)); return 0;
        case VK_DOWN: ScrollTo(V.scrollY + Z(40.f)); return 0;
        case VK_PRIOR: ScrollTo(V.scrollY - V.viewH * 0.9f); return 0;
        case VK_NEXT:
        case VK_SPACE: ScrollTo(V.scrollY + V.viewH * 0.9f); return 0;
        case VK_HOME: ScrollTo(0); return 0;
        case VK_END: ScrollTo(V.totalH); return 0;
        case 'C': if (ctrl) CopySelection(); return 0;
        case 'A': if (ctrl) SelectAll(); return 0;
        case VK_ESCAPE:
            if (V.hasSel) { ClearSelection(); InvalidateRect(h, nullptr, FALSE); return 0; }
            break;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Hit ht = HitTest(pt.x * 96.0f / V.dpi, pt.y * 96.0f / V.dpi);
        if (ht.block < 0) return 0;
        V.downPt = pt;
        V.dragged = false;
        // a third click soon after a double-click, near the same spot, selects the paragraph
        bool triple = V.lastDbl && (DWORD)(GetMessageTime() - V.lastDbl) <= GetDoubleClickTime() &&
                      abs(pt.x - V.dblPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                      abs(pt.y - V.dblPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
        V.lastDbl = 0;
        bool anchorOk = V.selAB >= 0 && V.selAB < (int)V.blocks.size();
        if (triple) {
            StartUnitSelection(ht, 2);
            V.dragged = true;
        } else if (GetKeyState(VK_SHIFT) < 0 && anchorOk) {
            // Shift+click extends from the existing anchor (or the previous click)
            V.selUnit = 0;
            ExtendSelection(ht);
            V.dragged = true;
        } else {
            V.selUnit = 0;
            V.selAB = V.selFB = ht.block;
            V.selAP = V.selFP = ht.pos;
            V.hasSel = false;
        }
        V.selecting = true;
        SetCapture(h);
        SetTimer(h, 2, 30, nullptr);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Hit ht = HitTest(pt.x * 96.0f / V.dpi, pt.y * 96.0f / V.dpi);
        if (ht.block < 0) return 0;
        StartUnitSelection(ht, 1);   // word; dragging on extends word by word
        V.lastDbl = GetMessageTime();
        V.dblPt = pt;
        V.dragged = true;
        V.selecting = true;
        SetCapture(h);
        SetTimer(h, 2, 30, nullptr);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Hit ht = HitTest(pt.x * 96.0f / V.dpi, pt.y * 96.0f / V.dpi);
        if (V.selecting) {
            if (!V.dragged && (abs(pt.x - V.downPt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                               abs(pt.y - V.downPt.y) >= GetSystemMetrics(SM_CYDRAG)))
                V.dragged = true;
            if (V.dragged) {
                ExtendSelection(ht);
                InvalidateRect(h, nullptr, FALSE);
            }
            V.hoverLink = -1;
        } else {
            V.hoverLink = ht.link;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        bool click = V.selecting && !V.dragged;
        EndSelecting();
        if (click && !V.hasSel) {   // a plain click (no drag) on a link follows it
            Hit ht = HitTest(GET_X_LPARAM(l) * 96.0f / V.dpi, GET_Y_LPARAM(l) * 96.0f / V.dpi);
            if (ht.link >= 0) OpenLink(ht.link);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:   // capture lost (alt-tab, a menu, ...): stop selecting
        if ((HWND)l != h && V.selecting) {
            V.selecting = false;
            KillTimer(h, 2);
        }
        return 0;
    case WM_CANCELMODE:
        EndSelecting();
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, V.hoverLink >= 0 ? IDC_HAND : IDC_IBEAM));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        POINT cpt = pt;
        if (pt.x == -1) { cpt = {10, 10}; pt = cpt; ClientToScreen(h, &pt); }
        else ScreenToClient(h, &cpt);
        Hit ht = HitTest(cpt.x * 96.0f / V.dpi, cpt.y * 96.0f / V.dpi);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (V.hasSel ? 0 : MF_GRAYED), 1, L"Copy\tCtrl+C");
        AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
        if (ht.link >= 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 3, L"Open link");
            AppendMenuW(menu, MF_STRING, 4, L"Copy link address");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 5, L"Show in editor");
        // the preview can be rebuilt while the menu is open (typing continues to arrive): remember
        // what was under the pointer now
        std::wstring linkUrl = ht.link >= 0 && ht.link < (int)V.links.size() ? V.links[ht.link] : std::wstring();
        size_t blockSrc = ht.block >= 0 && ht.block < (int)V.blocks.size() ? V.blocks[ht.block].src : (size_t)-1;
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) CopySelection();
        else if (cmd == 2) { SelectAll(); }
        else if (cmd == 3) {
            if (ht.link < (int)V.links.size() && V.links[ht.link] == linkUrl) OpenLink(ht.link);
            else if (!linkUrl.empty() && linkUrl[0] != L'#') PostMessageW(GetParent(h), WM_APP_PREVIEW_LINK, 0, (LPARAM) new std::wstring(linkUrl));
        } else if (cmd == 4 && OpenClipboard(h)) {
            const std::wstring& u = linkUrl;
            EmptyClipboard();
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (u.size() + 1) * 2);
            if (g) { memcpy(GlobalLock(g), u.c_str(), (u.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
            CloseClipboard();
        } else if (cmd == 5 && blockSrc != (size_t)-1) {
            PostMessageW(GetParent(h), WM_APP_PREVIEW_GOTO, (WPARAM)blockSrc, 0);
        }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_DESTROY:
        V.blocks.clear();
        V.rt.Reset();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}

HWND Preview_Create(HWND parent) {
    if (!V.d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, V.d2d.GetAddressOf());
    if (!V.dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)V.dw.GetAddressOf());
    V.zoom = g_set.previewZoom;
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = App_Instance();
    wc.lpszClassName = L"MarkLitePreview";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"MarkLitePreview", L"", WS_CHILD | WS_VSCROLL | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, App_Instance(), nullptr);
    Theme_Control(h);
    return h;
}

void Preview_SetMarkdown(HWND h, const char* text, size_t len, const std::wstring& baseDir) {
    if (baseDir != V.baseDir) { V.baseDir = baseDir; V.images.clear(); V.missing.clear(); V.blocks.clear(); }
    Rebuild(text, len);
    InvalidateRect(h, nullptr, FALSE);
}

void Preview_Clear(HWND h) {
    EndSelecting();
    ClearSelection();
    V.blocks.clear();
    V.images.clear();
    V.missing.clear();
    V.scrollY = 0;
    V.layoutDirty = true;
    InvalidateRect(h, nullptr, FALSE);
}

void Preview_SyncToSource(HWND h, size_t off) {
    if (V.blocks.empty()) return;
    if (V.layoutDirty) {
        if (!CreateRT()) return;
        LayoutAll();
        ClampScroll();
        UpdateScrollbar();
    }
    if (off == (size_t)-1) { ScrollTo(V.totalH); return; }
    int lo = 0, hi = (int)V.blocks.size() - 1, k = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (V.blocks[mid].src <= off) { k = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    const Block& b = V.blocks[k];
    float y = b.y;
    if (k + 1 < (int)V.blocks.size()) {
        const Block& n = V.blocks[k + 1];
        if (n.src > b.src && off >= b.src) {
            float f = std::min(1.0f, (float)(off - b.src) / (float)(n.src - b.src));
            y = b.y + f * (n.y - b.y);
        }
    }
    if (off == 0) y = 0;
    ScrollTo(y - Z(16.f));
    (void)h;
}

size_t Preview_TopSource(HWND) {
    if (V.blocks.empty()) return 0;
    return V.blocks[BlockAtY(V.scrollY)].src;
}

void Preview_ThemeChanged(HWND h) {
    Theme_Control(h);
    UpdateBrushColors();
    InvalidateRect(h, nullptr, FALSE);
}

void Preview_SetZoom(HWND h, float z) {
    z = std::max(0.5f, std::min(3.0f, z));
    float rel = V.totalH > 0 ? V.scrollY / V.totalH : 0;
    V.zoom = z;
    g_set.previewZoom = z;
    for (auto& b : V.blocks) {
        b.lay.Reset();
        b.objs.clear();
        b.measured = false;
        for (auto& r : b.rows) for (auto& c : r) { c.lay.Reset(); c.objs.clear(); }
    }
    if (CreateRT()) {
        LayoutAll();
        V.scrollY = rel * V.totalH;
        ClampScroll();
        UpdateScrollbar();
    }
    InvalidateRect(h, nullptr, FALSE);
}

float Preview_GetZoom(HWND) { return V.zoom; }
void Preview_Copy(HWND) { CopySelection(); }
void Preview_SelectAll(HWND) { SelectAll(); }
