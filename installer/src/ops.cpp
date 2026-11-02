// Install and uninstall operations. Nothing in here shows UI, so the same code serves the
// window, the silent command line and the elevated worker process.
#include "setup.h"
#include "payload.h"
#include "resource.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shldisp.h>
#include <exdisp.h>
#include <shlguid.h>
#include <tlhelp32.h>
#include <compressapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;
using namespace setup;

namespace {

const wchar_t* const kExtensions[] = {L".md", L".markdown"};
const wchar_t kClasses[] = L"Software\\Classes\\";

// cabinet.dll is not a KnownDLL, so it is loaded from System32 explicitly rather than imported:
// an installer usually runs from the Downloads folder, where a planted DLL could otherwise load.
using FnCreateDecompressor = BOOL(WINAPI*)(DWORD, PCOMPRESS_ALLOCATION_ROUTINES, PDECOMPRESSOR_HANDLE);
using FnDecompress = BOOL(WINAPI*)(DECOMPRESSOR_HANDLE, LPCVOID, SIZE_T, PVOID, SIZE_T, PSIZE_T);
using FnCloseDecompressor = BOOL(WINAPI*)(DECOMPRESSOR_HANDLE);

class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() { if (SUCCEEDED(hr_)) CoUninitialize(); }
private:
    HRESULT hr_;
};

HKEY RootFor(Scope s) { return s == Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ------------------------------------------------------------------ registry
LSTATUS SetStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

LSTATUS SetDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(root, key.c_str(), name, REG_DWORD, &value, sizeof(value));
}

std::wstring GetStr(HKEY root, const std::wstring& key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return std::wstring();
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, &s[0], &bytes) != ERROR_SUCCESS) return std::wstring();
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

DWORD GetDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD def) {
    DWORD v = 0, bytes = sizeof(v);
    return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &bytes) == ERROR_SUCCESS ? v : def;
}

void DeleteValue(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return;
    RegDeleteValueW(h, name);
    RegCloseKey(h);
}

void DeleteKeyIfEmpty(HKEY root, const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return;
    DWORD subkeys = 0, values = 0;
    LSTATUS st = RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(h);
    if (st == ERROR_SUCCESS && !subkeys && !values) RegDeleteKeyW(root, key.c_str());
}

bool ReadInstall(Scope scope, ExistingInstall& e) {
    HKEY root = RootFor(scope);
    std::wstring dir = GetStr(root, kUninstallKey, L"InstallLocation");
    if (dir.empty()) return false;
    e.found = true;
    e.scope = scope;
    e.dir = NormalizeDir(dir);
    e.version = GetStr(root, kUninstallKey, L"DisplayVersion");
    e.desktopShortcut = GetDword(root, kUninstallKey, L"MarkLiteDesktopShortcut", 1) != 0;
    e.fileAssociations = GetDword(root, kUninstallKey, L"MarkLiteFileAssociations", 1) != 0;
    return true;
}

// ------------------------------------------------------------------ payload
DWORD LoadPayload(std::vector<unsigned char>& out) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return SETUP_ERR_PAYLOAD;
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL mem = LoadResource(nullptr, res);
    const unsigned char* p = mem ? (const unsigned char*)LockResource(mem) : nullptr;
    if (!p || size < sizeof(PayloadHeader)) return SETUP_ERR_PAYLOAD;
    PayloadHeader h;
    memcpy(&h, p, sizeof(h));
    if (memcmp(h.magic, kPayloadMagic, 4) != 0 || h.format != kPayloadFormatLzms || !h.rawSize || h.rawSize > (256u << 20))
        return SETUP_ERR_PAYLOAD;

    HMODULE cab = LoadLibraryExW(L"cabinet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = cab ? (FnCreateDecompressor)(void*)GetProcAddress(cab, "CreateDecompressor") : nullptr;
    auto decompress = cab ? (FnDecompress)(void*)GetProcAddress(cab, "Decompress") : nullptr;
    auto close = cab ? (FnCloseDecompressor)(void*)GetProcAddress(cab, "CloseDecompressor") : nullptr;
    if (!create || !decompress || !close) return ERROR_PROC_NOT_FOUND;

    DECOMPRESSOR_HANDLE d = nullptr;
    if (!create(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) return GetLastError();
    out.resize((size_t)h.rawSize);
    SIZE_T got = 0;
    BOOL ok = decompress(d, p + sizeof(h), size - sizeof(h), out.data(), out.size(), &got);
    close(d);
    if (!ok || got != h.rawSize || Crc32(out.data(), out.size()) != h.crc) return SETUP_ERR_PAYLOAD;
    return SETUP_OK;
}

// ------------------------------------------------------------------ files
// Writes next to the target first and then swaps it in, so a failure never leaves a half-written exe.
DWORD WriteFileAtomic(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0, err = 0;
    if (!WriteFile(f, data, (DWORD)n, &written, nullptr) || written != n || !FlushFileBuffers(f)) err = GetLastError();
    CloseHandle(f);
    if (!err && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) err = GetLastError();
    if (err) DeleteFileW(tmp.c_str());
    return err;
}

DWORD CopyFileAtomic(const std::wstring& from, const std::wstring& to) {
    std::wstring tmp = to + L".new";
    if (!CopyFileW(from.c_str(), tmp.c_str(), FALSE)) return GetLastError();
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        return err;
    }
    return 0;
}

DWORD FileSizeKb(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    ULONGLONG size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return (DWORD)((size + 1023) / 1024);
}

DWORD DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    DWORD err = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = JoinPath(dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (DWORD e = DeleteTree(p)) err = e;
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? !RemoveDirectoryW(p.c_str()) : !DeleteFileW(p.c_str())) err = GetLastError();
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str()) && !err) err = GetLastError();
    return err;
}

// The uninstaller can't delete its own exe while it runs. A hidden cmd.exe waits for it to exit,
// deletes it and then removes the install folder if nothing else is left in it.
void ScheduleSelfDelete(const std::wstring& exe, const std::wstring& dir) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe";
    std::wstring line = L"\"" + cmdExe + L"\" /d /q /s /c \"for /l %n in (1,1,600) do (del /f /q \"" + exe +
                        L"\" >nul 2>&1 & if not exist \"" + exe + L"\" (rmdir \"" + dir +
                        L"\" >nul 2>&1 & exit) else ping -n 2 127.0.0.1 >nul)\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    if (CreateProcessW(cmdExe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ------------------------------------------------------------------ shortcuts
std::wstring StartMenuShortcut(Scope s) {
    return JoinPath(KnownFolder(s == Scope::Machine ? FOLDERID_CommonPrograms : FOLDERID_Programs), kShortcutName);
}
std::wstring DesktopShortcut(Scope s) {
    return JoinPath(KnownFolder(s == Scope::Machine ? FOLDERID_PublicDesktop : FOLDERID_Desktop), kShortcutName);
}

HRESULT CreateShortcut(const std::wstring& lnk, const std::wstring& target) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return hr;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(ParentDir(target).c_str());
    link->SetDescription(L"Markdown reader and editor");
    link->SetIconLocation(target.c_str(), 0);
    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    if (SUCCEEDED(hr)) hr = file->Save(lnk.c_str(), TRUE);
    return hr;
}

// ------------------------------------------------------------------ file types
std::wstring OpenCommand(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }
std::wstring AppKey() { return std::wstring(kClasses) + L"Applications\\" + kExeName; }

// Applications\MarkLite.exe is keyed by file name alone, so "Open with" may already know another
// MarkLite.exe (a portable copy, for example). Setup only writes or removes that entry when it is
// free or already points at this installation.
bool OwnsAppKey(HKEY root, const std::wstring& exe) {
    std::wstring current = GetStr(root, AppKey() + L"\\shell\\open\\command", nullptr);
    return current.empty() || current == OpenCommand(exe);
}

// Registers MarkLite as a handler for Markdown files: it appears under "Open with" and in
// Settings > Default apps. It becomes the default only for extensions nobody else has claimed;
// otherwise Windows leaves that choice to the user.
LSTATUS RegisterAssociations(HKEY root, const std::wstring& exe) {
    const std::wstring cls = kClasses;
    const std::wstring open = OpenCommand(exe);
    const std::wstring icon = L"\"" + exe + L"\",0";
    const std::wstring prog = cls + kProgId;
    LSTATUS st = SetStr(root, prog, nullptr, L"Markdown document");
    if (st != ERROR_SUCCESS) return st;
    SetStr(root, prog, L"FriendlyTypeName", L"Markdown document");
    SetStr(root, prog + L"\\DefaultIcon", nullptr, icon);
    SetStr(root, prog + L"\\shell\\open\\command", nullptr, open);

    const std::wstring app = AppKey();
    const bool ownApp = OwnsAppKey(root, exe);
    if (ownApp) {
        SetStr(root, app, L"FriendlyAppName", kAppName);
        SetStr(root, app + L"\\shell\\open\\command", nullptr, open);
    }

    for (const wchar_t* ext : kExtensions) {
        if (ownApp) SetStr(root, app + L"\\SupportedTypes", ext, L"");
        RegSetKeyValueW(root, (cls + ext + L"\\OpenWithProgids").c_str(), kProgId, REG_NONE, nullptr, 0);
        if (GetStr(HKEY_CLASSES_ROOT, ext, nullptr).empty()) SetStr(root, cls + ext, nullptr, kProgId);
        SetStr(root, std::wstring(kCapabilitiesKey) + L"\\FileAssociations", ext, kProgId);
    }
    SetStr(root, kCapabilitiesKey, L"ApplicationName", kAppName);
    SetStr(root, kCapabilitiesKey, L"ApplicationDescription", L"Markdown reader and editor");
    return SetStr(root, L"Software\\RegisteredApplications", kAppName, kCapabilitiesKey);
}

void UnregisterAssociations(HKEY root, const std::wstring& exe) {
    const std::wstring cls = kClasses;
    RegDeleteTreeW(root, (cls + kProgId).c_str());
    RegDeleteKeyW(root, (cls + kProgId).c_str());
    if (GetStr(root, AppKey() + L"\\shell\\open\\command", nullptr) == OpenCommand(exe)) {
        RegDeleteTreeW(root, AppKey().c_str());
        RegDeleteKeyW(root, AppKey().c_str());
    }
    for (const wchar_t* ext : kExtensions) {
        DeleteValue(root, cls + ext + L"\\OpenWithProgids", kProgId);
        if (GetStr(root, cls + ext, nullptr) == kProgId) DeleteValue(root, cls + ext, nullptr);
        DeleteKeyIfEmpty(root, cls + ext + L"\\OpenWithProgids");
        DeleteKeyIfEmpty(root, cls + ext);
    }
    RegDeleteTreeW(root, kCapabilitiesKey);
    RegDeleteKeyW(root, kCapabilitiesKey);
    DeleteKeyIfEmpty(root, L"Software\\MarkLite");
    DeleteValue(root, L"Software\\RegisteredApplications", kAppName);
}

// ------------------------------------------------------------------ Apps & features entry
LSTATUS WriteUninstallEntry(const InstallOptions& o, const std::wstring& exe, const std::wstring& uninstaller) {
    HKEY root = RootFor(o.scope);
    const std::wstring key = kUninstallKey;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t date[16];
    swprintf(date, 16, L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay);

    LSTATUS st = SetStr(root, key, L"DisplayName", kAppName);
    if (st != ERROR_SUCCESS) return st;
    SetStr(root, key, L"DisplayVersion", SETUP_VERSION);
    SetStr(root, key, L"Publisher", kPublisher);
    SetStr(root, key, L"DisplayIcon", exe + L",0");
    SetStr(root, key, L"InstallLocation", NormalizeDir(o.dir));
    SetStr(root, key, L"InstallDate", date);
    SetStr(root, key, L"UninstallString", QuoteArg(uninstaller));
    SetStr(root, key, L"QuietUninstallString", QuoteArg(uninstaller) + L" --silent");
    SetStr(root, key, L"URLInfoAbout", kWebsite);
    SetStr(root, key, L"HelpLink", kRepo);
    SetDword(root, key, L"NoModify", 1);
    SetDword(root, key, L"NoRepair", 1);
    SetDword(root, key, L"EstimatedSize", FileSizeKb(exe) + FileSizeKb(uninstaller));
    SetDword(root, key, L"MarkLiteDesktopShortcut", o.desktopShortcut ? 1 : 0);
    return SetDword(root, key, L"MarkLiteFileAssociations", o.fileAssociations ? 1 : 0);
}

void Report(const ProgressFn& fn, int pct, const wchar_t* step) {
    if (fn) fn(pct, step);
}

}  // namespace

// ================================================================== helpers
std::wstring SelfPath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ParentDir(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : path.substr(0, k);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

std::wstring NormalizeDir(const std::wstring& path) {
    if (path.empty()) return path;
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring r = path;
    if (n) {
        std::wstring buf(n, L'\0');
        DWORD m = GetFullPathNameW(path.c_str(), n, &buf[0], nullptr);
        if (m && m < n) { buf.resize(m); r = buf; }
    }
    std::replace(r.begin(), r.end(), L'/', L'\\');
    while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    return r;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    std::wstring x = NormalizeDir(a), y = NormalizeDir(b);
    return CompareStringOrdinal(x.c_str(), (int)x.size(), y.c_str(), (int)y.size(), TRUE) == CSTR_EQUAL;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    const wchar_t *p = a.c_str(), *q = b.c_str();
    for (int i = 0; i < 4; i++) {
        long x = wcstol(p, const_cast<wchar_t**>(&p), 10), y = wcstol(q, const_cast<wchar_t**>(&q), 10);
        if (x != y) return x < y ? -1 : 1;
        if (*p == L'.') p++;
        if (*q == L'.') q++;
    }
    return 0;
}

std::wstring QuoteArg(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') r.append(slashes * 2 + 1, L'\\');
        else r.append(slashes, L'\\');
        slashes = 0;
        r += c;
    }
    r.append(slashes * 2, L'\\');
    r += L'"';
    return r;
}

std::wstring WorkerArgs(bool install, const InstallOptions& o) {
    std::wstring a = install ? L"--worker=install" : L"--worker=uninstall";
    a += o.scope == Scope::Machine ? L" --scope=machine " : L" --scope=user ";
    a += QuoteArg(L"--dir=" + NormalizeDir(o.dir));
    if (install) {
        a += o.desktopShortcut ? L" --desktop=1" : L" --desktop=0";
        a += o.fileAssociations ? L" --assoc=1" : L" --assoc=0";
    }
    return a;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return r;
}

DWORD RunElevated(HWND owner, const std::wstring& args) {
    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_UNICODE;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError();
    if (!sei.hProcess) return ERROR_GEN_FAILURE;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

// shellapi.h maps ShellExecute to ShellExecuteW, which would rename the IShellDispatch2 method below.
#undef ShellExecute

// When Setup itself runs elevated, MarkLite is started through Explorer so that it runs as the
// signed-in user. An elevated editor couldn't accept files dragged in from Explorer.
static bool ShellExecuteAsUser(const std::wstring& file) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
    VARIANT loc{}, empty{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) return false;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> viewDisp, appDisp;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDisp))) ||
        FAILED(viewDisp.As(&folderView)) || FAILED(folderView->get_Application(&appDisp)) || FAILED(appDisp.As(&shell)))
        return false;
    BSTR bfile = SysAllocString(file.c_str());
    VARIANT args{}, dir{}, op{}, show{};
    args.vt = VT_BSTR;
    args.bstrVal = SysAllocString(L"");
    dir.vt = VT_BSTR;
    dir.bstrVal = SysAllocString(ParentDir(file).c_str());
    op.vt = VT_BSTR;
    op.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    HRESULT hr = shell->ShellExecute(bfile, args, dir, op, show);
    SysFreeString(bfile);
    VariantClear(&args);
    VariantClear(&dir);
    VariantClear(&op);
    return SUCCEEDED(hr);
}

bool LaunchApp(const std::wstring& exe) {
    ComScope com;
    if (IsElevated() && ShellExecuteAsUser(exe)) return true;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, ParentDir(exe).c_str(), SW_SHOWNORMAL) > 32;
}

std::wstring ErrorMessage(DWORD code, const std::wstring& dir) {
    switch (code) {
    case SETUP_ERR_RUNNING: return L"MarkLite is open. Close it and try again.";
    case SETUP_ERR_PAYLOAD: return L"This installer is damaged. Download it again from marklite.vercel.app.";
    case SETUP_ERR_NOT_INSTALLED: return L"MarkLite isn't installed in " + dir + L".";
    case ERROR_CANCELLED: return L"Windows didn't get administrator permission.";
    case ERROR_ACCESS_DENIED: return L"Windows didn't allow changes in " + dir + L".";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return L"There isn't enough free disk space.";
    }
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (LPWSTR)&text, 0, nullptr);
    std::wstring msg = text ? text : L"Something went wrong.";
    LocalFree(text);
    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r' || msg.back() == L' ')) msg.pop_back();
    wchar_t num[32];
    swprintf(num, 32, L" (error %lu)", code);
    return msg + num;
}

// ================================================================== queries
std::wstring DefaultInstallDir(Scope scope) {
    std::wstring base = KnownFolder(scope == Scope::Machine ? FOLDERID_ProgramFiles : FOLDERID_UserProgramFiles);
    if (base.empty() && scope == Scope::User) base = JoinPath(KnownFolder(FOLDERID_LocalAppData), L"Programs");
    return JoinPath(base, kAppName);
}

ExistingInstall FindExistingInstall() {
    ExistingInstall e;
    if (!ReadInstall(Scope::User, e)) ReadInstall(Scope::Machine, e);
    return e;
}

ExistingInstall FindInstallAt(const std::wstring& dir) {
    ExistingInstall e;
    for (Scope s : {Scope::User, Scope::Machine}) {
        ExistingInstall x;
        if (ReadInstall(s, x) && SamePath(x.dir, dir)) return x;
    }
    e.dir = NormalizeDir(dir);
    std::wstring pf = KnownFolder(FOLDERID_ProgramFiles);
    e.scope = (!pf.empty() && e.dir.size() > pf.size() && SamePath(e.dir.substr(0, pf.size()), pf)) ? Scope::Machine : Scope::User;
    e.found = Exists(JoinPath(e.dir, kExeName));
    return e;
}

std::vector<DWORD> RunningInstances(const std::wstring& dir) {
    std::vector<DWORD> pids;
    std::wstring exe = JoinPath(NormalizeDir(dir), kExeName);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, kExeName) != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        std::wstring image(32768, L'\0');
        DWORD n = (DWORD)image.size();
        if (QueryFullProcessImageNameW(p, 0, &image[0], &n)) {
            image.resize(n);
            if (SamePath(image, exe)) pids.push_back(pe.th32ProcessID);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return pids;
}

namespace {
struct CloseCtx { const std::vector<DWORD>* pids; };
BOOL CALLBACK CloseWindowsProc(HWND h, LPARAM l) {
    auto* ctx = (CloseCtx*)l;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, kAppWindowClass) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (std::find(ctx->pids->begin(), ctx->pids->end(), pid) != ctx->pids->end()) {
        AllowSetForegroundWindow(pid);
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
}  // namespace

bool CloseRunningInstances(const std::wstring& dir, DWORD timeoutMs) {
    std::vector<DWORD> pids = RunningInstances(dir);
    if (pids.empty()) return true;
    CloseCtx ctx{&pids};
    EnumWindows(CloseWindowsProc, (LPARAM)&ctx);
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (DWORD pid : pids) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!p) continue;
        ULONGLONG now = GetTickCount64();
        WaitForSingleObject(p, now < deadline ? (DWORD)(deadline - now) : 0);
        CloseHandle(p);
    }
    return RunningInstances(dir).empty();
}

// ================================================================== install
DWORD DoInstall(const InstallOptions& opt, const ProgressFn& progress) {
    ComScope com;
    const std::wstring dir = NormalizeDir(opt.dir);
    if (dir.size() < 3) return ERROR_INVALID_PARAMETER;
    const HKEY root = RootFor(opt.scope);
    const std::wstring exe = JoinPath(dir, kExeName), uninstaller = JoinPath(dir, kUninstallerName);
    const bool upgrade = Exists(exe);
    const bool dirExisted = Exists(dir);
    InstallOptions o = opt;
    o.dir = dir;

    Report(progress, 5, L"Checking for a running MarkLite");
    if (!RunningInstances(dir).empty()) return SETUP_ERR_RUNNING;

    Report(progress, 15, L"Unpacking MarkLite");
    std::vector<unsigned char> payload;
    if (DWORD e = LoadPayload(payload)) return e;

    // Undo a fresh install that fails part of the way through. An upgrade keeps what was there.
    auto fail = [&](DWORD e) -> DWORD {
        if (!upgrade) {
            DeleteFileW(StartMenuShortcut(o.scope).c_str());
            if (o.desktopShortcut) DeleteFileW(DesktopShortcut(o.scope).c_str());
            if (o.fileAssociations) UnregisterAssociations(root, exe);
            RegDeleteTreeW(root, kUninstallKey);
            RegDeleteKeyW(root, kUninstallKey);
            DeleteFileW(exe.c_str());
            if (!SamePath(SelfPath(), uninstaller)) DeleteFileW(uninstaller.c_str());
            if (!dirExisted) RemoveDirectoryW(dir.c_str());
        }
        return e;
    };

    Report(progress, 35, L"Copying files");
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) return fail((DWORD)rc);
    if (DWORD e = WriteFileAtomic(exe, payload.data(), payload.size())) {
        if ((e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) && !RunningInstances(dir).empty()) e = SETUP_ERR_RUNNING;
        return fail(e);
    }
    if (!SamePath(SelfPath(), uninstaller)) {
        if (DWORD e = CopyFileAtomic(SelfPath(), uninstaller)) return fail(e);
    }

    Report(progress, 60, L"Adding MarkLite to Apps & features");
    if (LSTATUS st = WriteUninstallEntry(o, exe, uninstaller)) return fail((DWORD)st);

    Report(progress, 75, L"Creating shortcuts");
    if (HRESULT hr = CreateShortcut(StartMenuShortcut(o.scope), exe); FAILED(hr))
        return fail(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
    if (o.desktopShortcut) CreateShortcut(DesktopShortcut(o.scope), exe);
    else DeleteFileW(DesktopShortcut(o.scope).c_str());

    Report(progress, 90, L"Registering Markdown files");
    if (o.fileAssociations) {
        if (LSTATUS st = RegisterAssociations(root, exe)) return fail((DWORD)st);
    } else {
        UnregisterAssociations(root, exe);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    Report(progress, 100, L"Done");
    return SETUP_OK;
}

// ================================================================== uninstall
DWORD DoUninstall(Scope scope, const std::wstring& dirIn, const ProgressFn& progress) {
    ComScope com;
    const std::wstring dir = NormalizeDir(dirIn);
    if (dir.size() < 3) return ERROR_INVALID_PARAMETER;
    const HKEY root = RootFor(scope);
    const std::wstring exe = JoinPath(dir, kExeName), uninstaller = JoinPath(dir, kUninstallerName);

    Report(progress, 5, L"Checking for a running MarkLite");
    if (!RunningInstances(dir).empty()) return SETUP_ERR_RUNNING;

    Report(progress, 25, L"Removing shortcuts");
    DeleteFileW(StartMenuShortcut(scope).c_str());
    DeleteFileW(DesktopShortcut(scope).c_str());

    Report(progress, 45, L"Removing file types");
    UnregisterAssociations(root, exe);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    Report(progress, 65, L"Removing files");
    DeleteFileW((exe + L".new").c_str());
    DeleteFileW((uninstaller + L".new").c_str());
    if (Exists(exe) && !DeleteFileW(exe.c_str())) {
        DWORD e = GetLastError();
        return (e == ERROR_ACCESS_DENIED && !RunningInstances(dir).empty()) ? SETUP_ERR_RUNNING : e;
    }

    Report(progress, 85, L"Removing MarkLite from Apps & features");
    std::wstring registered = GetStr(root, kUninstallKey, L"InstallLocation");
    if (!registered.empty() && SamePath(registered, dir)) {
        RegDeleteTreeW(root, kUninstallKey);
        RegDeleteKeyW(root, kUninstallKey);
    }

    if (SamePath(SelfPath(), uninstaller)) {
        ScheduleSelfDelete(uninstaller, dir);
    } else {
        DeleteFileW(uninstaller.c_str());
        RemoveDirectoryW(dir.c_str());
    }
    Report(progress, 100, L"Done");
    return SETUP_OK;
}

DWORD RemoveUserSettings() {
    std::wstring appData = KnownFolder(FOLDERID_RoamingAppData);
    if (appData.empty()) return ERROR_PATH_NOT_FOUND;
    std::wstring dir = JoinPath(appData, kAppName);
    return Exists(dir) ? DeleteTree(dir) : 0;
}
