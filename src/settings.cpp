// Settings persisted in a small UTF-8 INI file.
// Location: MarkLite.ini next to the exe if it exists there (portable mode),
// otherwise %APPDATA%\MarkLite\MarkLite.ini. The API key is DPAPI-encrypted.
#include "app.h"
#include <shlobj.h>
#include <wincrypt.h>
#include <map>
#include <cstdio>

Settings g_set;

namespace {
using Section = std::map<std::wstring, std::wstring>;
std::map<std::wstring, Section> g_ini;

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring portable = DirOf(exe) + L"\\MarkLite.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\MarkLite";
        CoTaskMemFree(appdata);
    } else {
        dir = DirOf(exe);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\MarkLite.ini";
}

void ReadIni(const std::wstring& path) {
    g_ini.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), rd = 0;
    std::string buf(size, '\0');
    if (size) ReadFile(f, &buf[0], size, &rd, nullptr);
    CloseHandle(f);
    std::wstring text = U8ToW(buf);
    std::wstring sec;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(L'\n', i);
        if (j == std::wstring::npos) j = text.size();
        std::wstring line = Trim(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[' && line.back() == L']') { sec = line.substr(1, line.size() - 2); continue; }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        g_ini[sec][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
}

void WriteIni(const std::wstring& path) {
    std::wstring out = L"; MarkLite settings\r\n";
    for (auto& s : g_ini) {
        out += L"\r\n[" + s.first + L"]\r\n";
        for (auto& kv : s.second) out += kv.first + L"=" + kv.second + L"\r\n";
    }
    std::string u = WToU8(out);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring Get(const wchar_t* sec, const wchar_t* key, const std::wstring& def = L"") {
    auto s = g_ini.find(sec);
    if (s == g_ini.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}
int GetInt(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring v = Get(sec, key);
    return v.empty() ? def : _wtoi(v.c_str());
}
bool GetBool(const wchar_t* sec, const wchar_t* key, bool def) { return GetInt(sec, key, def ? 1 : 0) != 0; }
void Set(const wchar_t* sec, const wchar_t* key, const std::wstring& v) { g_ini[sec][key] = v; }
void SetInt(const wchar_t* sec, const wchar_t* key, int v) { g_ini[sec][key] = std::to_wstring(v); }

using Ini = std::map<std::wstring, Section>;
void Put(Ini& ini, const wchar_t* sec, const wchar_t* key, const std::wstring& v) { ini[sec][key] = v; }
void PutInt(Ini& ini, const wchar_t* sec, const wchar_t* key, int v) { ini[sec][key] = std::to_wstring(v); }
void PutList(Ini& ini, const wchar_t* sec, const std::vector<std::wstring>& v) {
    Section& s = ini[sec];
    s.clear();
    s[L"count"] = std::to_wstring(v.size());
    for (size_t i = 0; i < v.size(); i++) s[L"item" + std::to_wstring(i)] = v[i];
}
bool IsListSection(const std::wstring& sec) { return sec == L"Instructions" || sec == L"Models" || sec == L"Session"; }

// The settings as last read from or written to the file, and this window's changes to the recent
// list since then (0 = add, 1 = remove, 2 = clear). See Settings_Save.
Settings g_saved;
std::vector<std::pair<int, std::wstring>> g_recentOps;

void ApplyRecent(std::vector<std::wstring>& r, int op, const std::wstring& path) {
    if (op == 2) { r.clear(); return; }
    for (size_t i = 0; i < r.size(); i++)
        if (_wcsicmp(r[i].c_str(), path.c_str()) == 0) { r.erase(r.begin() + i); break; }
    if (op == 0) {
        r.insert(r.begin(), path);
        if (r.size() > 15) r.resize(15);
    }
}

std::vector<std::wstring> GetList(const wchar_t* sec) {
    std::vector<std::wstring> v;
    int n = GetInt(sec, L"count", 0);
    for (int i = 0; i < n; i++) {
        std::wstring s = Get(sec, (L"item" + std::to_wstring(i)).c_str());
        if (!s.empty()) v.push_back(s);
    }
    return v;
}
void SetList(const wchar_t* sec, const std::vector<std::wstring>& v) {
    g_ini[sec].clear();
    SetInt(sec, L"count", (int)v.size());
    for (size_t i = 0; i < v.size(); i++) Set(sec, (L"item" + std::to_wstring(i)).c_str(), v[i]);
}

// Values are stored on one line; escape newlines / backslashes.
std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') {}
        else o += c;
    }
    return o;
}
std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[++i];
            o += c == L'n' ? L'\n' : c;
        } else o += s[i];
    }
    return o;
}

std::wstring Protect(const std::wstring& plain) {
    if (plain.empty()) return L"";
    DATA_BLOB in{(DWORD)(plain.size() * sizeof(wchar_t)), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"MarkLite API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    DWORD len = 0;
    CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::wstring b64(len, L'\0');
    CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &len);
    b64.resize(len);
    LocalFree(out.pbData);
    return b64;
}
std::wstring Unprotect(const std::wstring& b64) {
    if (b64.empty()) return L"";
    DWORD len = 0;
    if (!CryptStringToBinaryW(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return L"";
    std::vector<BYTE> bin(len);
    CryptStringToBinaryW(b64.c_str(), 0, CRYPT_STRING_BASE64, bin.data(), &len, nullptr, nullptr);
    DATA_BLOB in{len, bin.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    std::wstring s((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

std::wstring FloatStr(float f) {
    wchar_t b[32];
    swprintf(b, 32, L"%.3f", f);
    return b;
}

// Every setting except the API key and the recent files, in INI form.
void Store(const Settings& s, Ini& ini) {
    Put(ini, L"AI", L"model", s.model);
    Put(ini, L"AI", L"effort", s.effort);
    PutInt(ini, L"AI", L"context", s.aiContext);
    PutInt(ini, L"AI", L"panelHeight", s.aiPanelH);
    std::vector<std::wstring> h;
    for (auto& x : s.instrHistory) h.push_back(Esc(x));
    PutList(ini, L"Instructions", h);
    std::vector<std::wstring> m;
    for (auto& x : s.models) m.push_back(x.id + L"|" + x.name);
    PutList(ini, L"Models", m);

    PutInt(ini, L"Spell", L"enabled", s.spell);
    PutInt(ini, L"Spell", L"ignoreCaps", s.spellIgnoreCaps);
    PutInt(ini, L"Spell", L"initialized", s.spellLangsInit);
    std::wstring langs;
    for (auto& l : s.spellLangs) { if (!langs.empty()) langs += L","; langs += l; }
    Put(ini, L"Spell", L"languages", langs);

    PutInt(ini, L"View", L"theme", s.theme);
    PutInt(ini, L"View", L"mode", s.viewMode);
    PutInt(ini, L"View", L"lastPreviewMode", s.lastPreviewMode);
    PutInt(ini, L"View", L"wrap", s.wrap);
    PutInt(ini, L"View", L"lineNumbers", s.lineNumbers);
    PutInt(ini, L"View", L"limitWidth", s.limitWidth);
    PutInt(ini, L"View", L"toolbar", s.toolbar);
    PutInt(ini, L"View", L"zoom", s.zoom);
    Put(ini, L"View", L"previewZoom", FloatStr(s.previewZoom));
    Put(ini, L"View", L"font", s.font);
    PutInt(ini, L"View", L"fontSize", s.fontSize);
    Put(ini, L"View", L"codeFont", s.codeFont);
    Put(ini, L"View", L"split", FloatStr(s.split));

    PutInt(ini, L"Window", L"x", s.win.left);
    PutInt(ini, L"Window", L"y", s.win.top);
    PutInt(ini, L"Window", L"w", s.win.right - s.win.left);
    PutInt(ini, L"Window", L"h", s.win.bottom - s.win.top);
    PutInt(ini, L"Window", L"maximized", s.maximized);
    PutInt(ini, L"Window", L"restoreSession", s.restoreSession);
    PutInt(ini, L"Window", L"sessionActive", s.sessionActive);
    PutList(ini, L"Session", s.session);
}
}

std::vector<ModelInfo> Settings_DefaultModels() {
    return {
        {L"claude-opus-5-5", L"Claude Opus 5.5"},
        {L"claude-sonnet-5-5", L"Claude Sonnet 5.5"},
        {L"claude-haiku-5-5", L"Claude Haiku 5.5"},
        {L"claude-fable-5-1", L"Claude Fable 5.1"},
    };
}

std::wstring ModelDisplayName(const std::wstring& id) {
    for (auto& m : g_set.models) if (m.id == id && !m.name.empty()) return m.name;
    for (auto& m : Settings_DefaultModels()) if (m.id == id) return m.name;
    return id;
}

void Settings_Load() {
    ReadIni(IniPath());
    Settings& s = g_set;
    s.apiKey = Unprotect(Get(L"AI", L"key"));
    s.model = Get(L"AI", L"model", s.model);
    s.effort = Get(L"AI", L"effort", s.effort);
    s.aiContext = GetBool(L"AI", L"context", s.aiContext);
    s.aiPanelH = GetInt(L"AI", L"panelHeight", s.aiPanelH);
    s.instrHistory.clear();
    for (auto& h : GetList(L"Instructions")) s.instrHistory.push_back(Unesc(h));
    s.models.clear();
    for (auto& m : GetList(L"Models")) {
        size_t bar = m.find(L'|');
        if (bar == std::wstring::npos) s.models.push_back({m, L""});
        else s.models.push_back({m.substr(0, bar), m.substr(bar + 1)});
    }
    if (s.models.empty()) s.models = Settings_DefaultModels();

    s.spell = GetBool(L"Spell", L"enabled", s.spell);
    s.spellIgnoreCaps = GetBool(L"Spell", L"ignoreCaps", s.spellIgnoreCaps);
    s.spellLangsInit = GetBool(L"Spell", L"initialized", false);
    s.spellLangs.clear();
    std::wstring langs = Get(L"Spell", L"languages");
    for (size_t i = 0; i < langs.size();) {
        size_t j = langs.find(L',', i);
        if (j == std::wstring::npos) j = langs.size();
        std::wstring t = Trim(langs.substr(i, j - i));
        if (!t.empty()) s.spellLangs.push_back(t);
        i = j + 1;
    }

    s.theme = GetInt(L"View", L"theme", s.theme);
    s.viewMode = GetInt(L"View", L"mode", s.viewMode);
    s.lastPreviewMode = GetInt(L"View", L"lastPreviewMode", s.lastPreviewMode);
    s.wrap = GetBool(L"View", L"wrap", s.wrap);
    s.lineNumbers = GetBool(L"View", L"lineNumbers", s.lineNumbers);
    s.limitWidth = GetBool(L"View", L"limitWidth", s.limitWidth);
    s.toolbar = GetBool(L"View", L"toolbar", s.toolbar);
    s.zoom = GetInt(L"View", L"zoom", s.zoom);
    s.previewZoom = (float)_wtof(Get(L"View", L"previewZoom", L"1.0").c_str());
    if (s.previewZoom < 0.5f || s.previewZoom > 3.0f) s.previewZoom = 1.0f;
    s.font = Get(L"View", L"font", s.font);
    s.fontSize = GetInt(L"View", L"fontSize", s.fontSize);
    s.codeFont = Get(L"View", L"codeFont", s.codeFont);
    s.split = (float)_wtof(Get(L"View", L"split", L"0.5").c_str());
    if (s.split < 0.15f || s.split > 0.85f) s.split = 0.5f;

    s.win.left = GetInt(L"Window", L"x", CW_USEDEFAULT);
    s.win.top = GetInt(L"Window", L"y", CW_USEDEFAULT);
    s.win.right = s.win.left + GetInt(L"Window", L"w", 0);
    s.win.bottom = s.win.top + GetInt(L"Window", L"h", 0);
    s.maximized = GetBool(L"Window", L"maximized", false);
    s.restoreSession = GetBool(L"Window", L"restoreSession", true);
    s.recent = GetList(L"Recent");
    s.session = GetList(L"Session");
    s.sessionActive = GetInt(L"Window", L"sessionActive", 0);
    g_saved = s;
    g_recentOps.clear();
}

void Settings_Save() {
    // Another MarkLite window may have saved since this one read the file. Start from what is in the
    // file now and write only what this window changed, so windows don't undo each other's changes
    // (otherwise the last window to close would bring back its own old session, recent files and key).
    Ini mine, before;
    Store(g_set, mine);
    Store(g_saved, before);
    ReadIni(IniPath());
    for (auto& sec : mine) {
        Section& disk = g_ini[sec.first];
        Section& old = before[sec.first];
        if (IsListSection(sec.first)) {
            if (sec.second != old || disk.empty()) disk = sec.second;
            continue;
        }
        for (auto& kv : sec.second) {
            auto o = old.find(kv.first);
            if (o == old.end() || o->second != kv.second || !disk.count(kv.first)) disk[kv.first] = kv.second;
        }
    }
    // recent files: this window's additions and removals, replayed on the list in the file
    std::vector<std::wstring> recent = g_ini.count(L"Recent") ? GetList(L"Recent") : g_saved.recent;
    for (auto& op : g_recentOps) ApplyRecent(recent, op.first, op.second);
    g_recentOps.clear();
    SetList(L"Recent", recent);
    g_set.recent = recent;
    // The key is rewritten only when it was changed here: a key this window couldn't decrypt (an INI
    // copied from another PC or user) stays stored, and so does the old one if encrypting fails.
    if (g_set.apiKey != g_saved.apiKey) {
        std::wstring enc = Protect(g_set.apiKey);
        if (enc.empty() == g_set.apiKey.empty()) Set(L"AI", L"key", enc);
    }
    WriteIni(IniPath());
    g_saved = g_set;
}

void Settings_AddRecent(const std::wstring& path) {
    ApplyRecent(g_set.recent, 0, path);
    g_recentOps.push_back({0, path});
}

void Settings_RemoveRecent(const std::wstring& path) {
    ApplyRecent(g_set.recent, 1, path);
    g_recentOps.push_back({1, path});
}

void Settings_ClearRecent() {
    g_set.recent.clear();
    g_recentOps.clear();
    g_recentOps.push_back({2, L""});
}
