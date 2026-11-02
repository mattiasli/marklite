// MarkLite Setup entry point.
//
// The same executable is the installer (MarkLite-Setup.exe) and, copied into the install folder,
// the uninstaller (uninstall.exe). Command line:
//
//   MarkLite-Setup.exe [--silent] [--just-me | --all-users] [--dir=PATH]
//                      [--no-desktop-shortcut] [--no-file-associations]
//   uninstall.exe      [--silent] [--remove-settings]
//
// Exit code 0 means success. Internally, "--worker=install|uninstall" runs one operation with no UI;
// Setup starts itself that way, elevated, to install for all users.
#include "setup.h"
#include <shellapi.h>
#include <objbase.h>
#include <cwchar>

namespace {

struct Args {
    bool silent = false, uninstall = false, removeSettings = false, help = false, bad = false;
    std::wstring worker;   // "install" / "uninstall" (internal)
    bool hasScope = false;
    Scope scope = Scope::User;
    std::wstring dir;
    int desktop = -1, assoc = -1;   // -1 = keep the existing choice (or the default)
};

bool Starts(const std::wstring& s, const wchar_t* prefix, std::wstring* rest = nullptr) {
    size_t n = wcslen(prefix);
    if (s.size() < n || _wcsnicmp(s.c_str(), prefix, n) != 0) return false;
    if (rest) *rest = s.substr(n);
    return true;
}

Args ParseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        std::wstring s = argv[i], v;
        if (s == L"--silent" || s == L"/S" || s == L"/s" || s == L"/silent" || s == L"/quiet") a.silent = true;
        else if (s == L"--uninstall" || s == L"/uninstall") a.uninstall = true;
        else if (s == L"--remove-settings") a.removeSettings = true;
        else if (s == L"--all-users") { a.hasScope = true; a.scope = Scope::Machine; }
        else if (s == L"--just-me") { a.hasScope = true; a.scope = Scope::User; }
        else if (s == L"--desktop-shortcut") a.desktop = 1;
        else if (s == L"--no-desktop-shortcut") a.desktop = 0;
        else if (s == L"--file-associations") a.assoc = 1;
        else if (s == L"--no-file-associations") a.assoc = 0;
        else if (Starts(s, L"--dir=", &v) || Starts(s, L"/D=", &v)) a.dir = v;
        else if (Starts(s, L"--worker=", &v)) a.worker = v;
        else if (Starts(s, L"--scope=", &v)) { a.hasScope = true; a.scope = _wcsicmp(v.c_str(), L"machine") ? Scope::User : Scope::Machine; }
        else if (Starts(s, L"--desktop=", &v)) a.desktop = v == L"1";
        else if (Starts(s, L"--assoc=", &v)) a.assoc = v == L"1";
        else if (s == L"--help" || s == L"/?" || s == L"-h") a.help = true;
        else a.bad = true;
    }
    if (argv) LocalFree(argv);
    return a;
}

void ShowHelp() {
    std::wstring text =
        L"MarkLite " SETUP_VERSION L" Setup\n\n"
        L"MarkLite-Setup.exe [options]\n"
        L"  --silent\t\t\tInstall without showing a window\n"
        L"  --just-me\t\tInstall for the current user (default)\n"
        L"  --all-users\t\tInstall for everyone (needs administrator rights)\n"
        L"  --dir=PATH\t\tInstall folder\n"
        L"  --no-desktop-shortcut\tDon't create a desktop shortcut\n"
        L"  --no-file-associations\tDon't register for .md files\n\n"
        L"uninstall.exe [--silent] [--remove-settings]";
    MessageBoxW(nullptr, text.c_str(), L"MarkLite Setup", MB_OK | MB_ICONINFORMATION);
}

InstallOptions OptionsFrom(const Args& a, const ExistingInstall& ex) {
    InstallOptions o;
    o.scope = a.hasScope ? a.scope : (ex.found ? ex.scope : Scope::User);
    if (!a.dir.empty()) o.dir = a.dir;
    else if (ex.found && ex.scope == o.scope) o.dir = ex.dir;
    else o.dir = DefaultInstallDir(o.scope);
    o.dir = NormalizeDir(o.dir);
    o.desktopShortcut = a.desktop >= 0 ? a.desktop == 1 : (ex.found ? ex.desktopShortcut : true);
    o.fileAssociations = a.assoc >= 0 ? a.assoc == 1 : (ex.found ? ex.fileAssociations : true);
    return o;
}

bool IsUninstallerExe() {
    std::wstring self = SelfPath();
    size_t k = self.find_last_of(L"\\/");
    std::wstring name = k == std::wstring::npos ? self : self.substr(k + 1);
    return _wcsicmp(name.c_str(), setup::kUninstallerName) == 0;
}

// The installed uninstaller works on its own folder. "MarkLite-Setup.exe --uninstall" works on
// whatever installation the registry points to.
ExistingInstall UninstallTarget(const Args& a) {
    if (!a.dir.empty()) return FindInstallAt(a.dir);
    if (IsUninstallerExe()) return FindInstallAt(ParentDir(SelfPath()));
    ExistingInstall e = FindExistingInstall();
    return e.found ? FindInstallAt(e.dir) : e;
}

int RunSilentUninstall(const Args& a) {
    ExistingInstall t = UninstallTarget(a);
    if (!t.found) return (int)SETUP_ERR_NOT_INSTALLED;
    InstallOptions o;
    o.scope = t.scope;
    o.dir = t.dir;
    DWORD code = (t.scope == Scope::Machine && !IsElevated()) ? RunElevated(nullptr, WorkerArgs(false, o))
                                                               : DoUninstall(t.scope, t.dir, nullptr);
    if (code == SETUP_OK && a.removeSettings) RemoveUserSettings();
    return (int)code;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Only load system DLLs from System32, never from the folder Setup was downloaded to.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Don't keep the install folder busy as the current directory.
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) SetCurrentDirectoryW(sys);

    Args a = ParseArgs();
    if (a.help) {
        ShowHelp();
        return 0;
    }
    if (a.bad) {
        if (!a.silent) ShowHelp();
        return (int)SETUP_ERR_USAGE;
    }

    // Elevated worker started by the UI or by a silent all-users run.
    if (!a.worker.empty()) {
        InstallOptions o = OptionsFrom(a, ExistingInstall{});
        if (a.worker == L"install") return (int)DoInstall(o, nullptr);
        if (a.worker == L"uninstall") return (int)DoUninstall(o.scope, o.dir, nullptr);
        return (int)SETUP_ERR_USAGE;
    }

    const bool uninstall = a.uninstall || IsUninstallerExe();
    if (a.silent) {
        if (uninstall) return RunSilentUninstall(a);
        InstallOptions o = OptionsFrom(a, FindExistingInstall());
        if (o.scope == Scope::Machine && !IsElevated()) return (int)RunElevated(nullptr, WorkerArgs(true, o));
        return (int)DoInstall(o, nullptr);
    }

    // One Setup window at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MarkLite.Setup");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"MarkLiteSetup", nullptr);
        if (other) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code;
    if (uninstall) {
        code = RunUi(inst, UiMode::Uninstall, UninstallTarget(a));
    } else {
        code = RunUi(inst, UiMode::Install, FindExistingInstall());
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
