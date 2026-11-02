#include "spell.h"
#include <initguid.h>
#include <spellcheck.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

DEFINE_GUID(CLSID_MlSpellCheckerFactory, 0x7AB36653, 0x1796, 0x484B, 0xBD, 0xFA, 0xE7, 0x4F, 0x1D, 0xB7, 0xC1, 0xDC);
DEFINE_GUID(IID_MlISpellCheckerFactory, 0x8E018A9D, 0x2415, 0x4677, 0xBF, 0x08, 0x79, 0x4E, 0xA6, 0x1F, 0x94, 0xBB);

namespace {
ComPtr<ISpellCheckerFactory> g_factory;
std::vector<SpellLang> g_supported;
std::vector<std::wstring> g_active;
std::vector<ComPtr<ISpellChecker>> g_checkers;
std::unordered_map<std::wstring, bool> g_cache;
std::unordered_set<std::wstring> g_ignored;

std::wstring DisplayName(const std::wstring& tag) {
    wchar_t buf[128];
    if (GetLocaleInfoEx(tag.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, buf, 128) > 0) return buf;
    return tag;
}

// Decode one UTF-8 code point; returns byte length.
int DecodeUtf8(const unsigned char* s, int n, unsigned& cp) {
    unsigned char c = s[0];
    if (c < 0x80) { cp = c; return 1; }
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (len > n || len == 1) { cp = 0xFFFD; return 1; }
    cp = c & (0xFF >> (len + 1));
    for (int i = 1; i < len; i++) cp = (cp << 6) | (s[i] & 0x3F);
    return len;
}

bool IsLetter(unsigned cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if (cp >= 0x300 && cp <= 0x36F) return true;  // combining diacritics
    if (cp < 0x10000) return IsCharAlphaW((wchar_t)cp) != 0;
    return false;
}
bool IsUpperCp(unsigned cp) { return cp < 0x10000 && IsCharUpperW((wchar_t)cp); }
}

namespace Spell {

bool Init() {
    if (g_factory) return true;
    if (FAILED(CoCreateInstance(CLSID_MlSpellCheckerFactory, nullptr, CLSCTX_INPROC_SERVER, IID_MlISpellCheckerFactory,
                                (void**)g_factory.GetAddressOf())))
        return false;
    ComPtr<IEnumString> e;
    if (SUCCEEDED(g_factory->get_SupportedLanguages(&e))) {
        LPOLESTR s;
        ULONG n;
        while (e->Next(1, &s, &n) == S_OK) {
            std::wstring tag = s;
            CoTaskMemFree(s);
            if (tag.find(L"-x-") != std::wstring::npos) continue;   // e.g. ja-Latn-JP-x-ext
            g_supported.push_back({tag, DisplayName(tag)});
        }
    }
    std::sort(g_supported.begin(), g_supported.end(), [](const SpellLang& a, const SpellLang& b) { return a.name < b.name; });
    return true;
}

const std::vector<SpellLang>& Supported() { return g_supported; }
const std::vector<std::wstring>& Active() { return g_active; }

std::wstring NameOf(const std::wstring& tag) {
    for (auto& l : g_supported) if (l.tag == tag) return l.name;
    return tag;
}

static bool IsSupportedTag(const std::wstring& tag) {
    for (auto& l : g_supported) if (_wcsicmp(l.tag.c_str(), tag.c_str()) == 0) return true;
    return false;
}

std::vector<std::wstring> DefaultLanguages() {
    // The user's Windows display languages that have a spell checker, at most two,
    // plus English (US) if nothing matched.
    std::vector<std::wstring> out;
    ULONG num = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, nullptr, &size) && size) {
        std::vector<wchar_t> buf(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, buf.data(), &size)) {
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1)
                if (IsSupportedTag(p) && out.size() < 2) out.push_back(p);
        }
    }
    // Also look at the input languages list (Settings > Language)
    HKEY k;
    if (out.size() < 2 && RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024];
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, L"Languages", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
            for (const wchar_t* p = buf; *p && out.size() < 2; p += wcslen(p) + 1) {
                bool dup = false;
                for (auto& o : out) if (_wcsicmp(o.c_str(), p) == 0) dup = true;
                if (!dup && IsSupportedTag(p)) out.push_back(p);
            }
        }
        RegCloseKey(k);
    }
    if (out.empty() && IsSupportedTag(L"en-US")) out.push_back(L"en-US");
    return out;
}

void SetLanguages(const std::vector<std::wstring>& tags) {
    g_active.clear();
    g_checkers.clear();
    g_cache.clear();
    if (!g_factory) return;
    for (auto& t : tags) {
        BOOL ok = FALSE;
        if (FAILED(g_factory->IsSupported(t.c_str(), &ok)) || !ok) continue;
        ComPtr<ISpellChecker> c;
        if (SUCCEEDED(g_factory->CreateSpellChecker(t.c_str(), &c))) {
            for (auto& w : g_ignored) c->Ignore(w.c_str());
            g_active.push_back(t);
            g_checkers.push_back(c);
        }
    }
}

bool IsCorrect(const std::wstring& word) {
    if (g_checkers.empty()) return true;
    auto it = g_cache.find(word);
    if (it != g_cache.end()) return it->second;
    bool ok = false;
    for (auto& c : g_checkers) {
        ComPtr<IEnumSpellingError> errs;
        if (FAILED(c->Check(word.c_str(), &errs))) { ok = true; break; }
        ComPtr<ISpellingError> e;
        if (errs->Next(&e) != S_OK) { ok = true; break; }
    }
    if (g_cache.size() > 50000) g_cache.clear();
    g_cache.emplace(word, ok);
    return ok;
}

std::vector<std::wstring> Suggest(const std::wstring& word, size_t max) {
    std::vector<std::wstring> out;
    std::vector<std::vector<std::wstring>> per;
    for (auto& c : g_checkers) {
        std::vector<std::wstring> v;
        ComPtr<IEnumString> e;
        if (SUCCEEDED(c->Suggest(word.c_str(), &e)) && e) {
            LPOLESTR s;
            ULONG n;
            while (v.size() < max && e->Next(1, &s, &n) == S_OK) { v.push_back(s); CoTaskMemFree(s); }
        }
        per.push_back(std::move(v));
    }
    // interleave suggestions from the languages
    for (size_t i = 0; out.size() < max; i++) {
        bool any = false;
        for (auto& v : per) {
            if (i < v.size()) {
                any = true;
                if (std::find(out.begin(), out.end(), v[i]) == out.end() && out.size() < max) out.push_back(v[i]);
            }
        }
        if (!any) break;
    }
    return out;
}

void Add(const std::wstring& word, size_t langIndex) {
    if (langIndex < g_checkers.size()) g_checkers[langIndex]->Add(word.c_str());
    g_cache[word] = true;
}

void Ignore(const std::wstring& word) {
    g_ignored.insert(word);
    for (auto& c : g_checkers) c->Ignore(word.c_str());
    g_cache[word] = true;
}

void Clear(Sci& s) {
    s(SCI_SETINDICATORCURRENT, IND_SPELL);
    s(SCI_INDICATORCLEARRANGE, 0, s.Len());
}

bool MisspelledAt(Sci& s, sptr_t pos, sptr_t& a, sptr_t& b) {
    for (sptr_t p : {pos, pos - 1}) {
        if (p < 0) continue;
        if (s(SCI_INDICATORVALUEAT, IND_SPELL, p)) {
            a = s(SCI_INDICATORSTART, IND_SPELL, p);
            b = s(SCI_INDICATOREND, IND_SPELL, p);
            return b > a;
        }
    }
    return false;
}

void CheckVisible(Sci& s, bool typing) {
    if (g_checkers.empty()) return;
    sptr_t lineCount = s(SCI_GETLINECOUNT);
    sptr_t firstVis = s(SCI_GETFIRSTVISIBLELINE);
    sptr_t first = s(SCI_DOCLINEFROMVISIBLE, firstVis);
    sptr_t last = s(SCI_DOCLINEFROMVISIBLE, firstVis + s(SCI_LINESONSCREEN) + 1);
    first = first > 8 ? first - 8 : 0;
    last = std::min<sptr_t>(lineCount - 1, last + 8);
    sptr_t start = s(SCI_POSITIONFROMLINE, first);
    sptr_t end = s(SCI_GETLINEENDPOSITION, last);
    if (end <= start) { s(SCI_SETINDICATORCURRENT, IND_SPELL); s(SCI_INDICATORCLEARRANGE, start, end - start); return; }

    s(SCI_COLOURISE, start, end);
    std::string buf((size_t)(end - start) * 2 + 2, '\0');
    Sci_TextRangeFull tr{{start, end}, &buf[0]};
    s(SCI_GETSTYLEDTEXTFULL, 0, (sptr_t)&tr);
    int n = (int)(end - start);
    std::string text(n, '\0');
    std::string styles(n, '\0');
    for (int i = 0; i < n; i++) { text[i] = buf[2 * i]; styles[i] = buf[2 * i + 1]; }

    sptr_t caret = s(SCI_GETCURRENTPOS);
    std::vector<std::pair<int, int>> bad;
    const unsigned char* t = (const unsigned char*)text.data();
    int i = 0;
    while (i < n) {
        unsigned cp;
        int k = DecodeUtf8(t + i, n - i, cp);
        if (!IsLetter(cp)) { i += k; continue; }
        int ws = i, we = i;
        int letters = 0, uppers = 0;
        bool upperInside = false, skip = false;
        while (we < n) {
            unsigned c2;
            int k2 = DecodeUtf8(t + we, n - we, c2);
            if (IsLetter(c2)) {
                if (IsUpperCp(c2)) { uppers++; if (letters > 0) upperInside = true; }
                letters++;
                we += k2;
                continue;
            }
            // apostrophe inside a word (don't, it’s)
            if ((c2 == '\'' || c2 == 0x2019) && we + k2 < n) {
                unsigned c3;
                DecodeUtf8(t + we + k2, n - we - k2, c3);
                if (IsLetter(c3)) { we += k2; continue; }
            }
            break;
        }
        i = we;
        if (letters < 2) continue;
        for (int x = ws; x < we; x++) if (Md_IsSpellSkipStyle(styles[x])) { skip = true; break; }
        if (skip) continue;
        if (uppers == letters && g_set.spellIgnoreCaps) continue;   // ALLCAPS
        if (upperInside && uppers != letters) continue;             // camelCase, iPhone
        unsigned char before = ws > 0 ? t[ws - 1] : ' ';
        unsigned char after = we < n ? t[we] : ' ';
        if ((before >= '0' && before <= '9') || before == '_' || before == '@' || before == '/' || before == '\\' || before == '$') continue;
        if ((after >= '0' && after <= '9') || after == '_' || after == '/' || after == '\\' || after == '@') continue;
        if (before == '.' && ws > 1 && IsLetter(t[ws - 2])) continue;              // file.ext, a.b.c
        if (after == '.' && we + 1 < n && IsLetter(t[we + 1])) continue;
        if (typing && caret >= start + ws && caret <= start + we) continue;
        std::wstring w = U8ToW((const char*)t + ws, (size_t)(we - ws));
        if (g_ignored.count(w)) continue;
        if (!IsCorrect(w)) bad.push_back({ws, we});
    }

    s(SCI_SETINDICATORCURRENT, IND_SPELL);
    s(SCI_INDICATORCLEARRANGE, start, end - start);
    for (auto& b : bad) s(SCI_INDICATORFILLRANGE, start + b.first, b.second - b.first);
}

}
