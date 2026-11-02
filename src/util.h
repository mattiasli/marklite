// Small shared helpers: UTF conversion, DPI scaling, COM pointers.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

inline std::wstring U8ToW(const char* s, size_t n) {
    if (!n) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &w[0], len);
    return w;
}
inline std::wstring U8ToW(const std::string& s) { return U8ToW(s.data(), s.size()); }

inline std::string WToU8(const wchar_t* s, size_t n) {
    if (!n) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr);
    std::string u(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, &u[0], len, nullptr, nullptr);
    return u;
}
inline std::string WToU8(const std::wstring& s) { return WToU8(s.data(), s.size()); }

inline int Dpi(HWND h) { UINT d = h ? GetDpiForWindow(h) : 96; return d ? (int)d : 96; }
inline int Sc(int v, int dpi) { return MulDiv(v, dpi, 96); }

inline std::wstring GetWndText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

inline std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

inline std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}
inline std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

inline std::wstring FormatThousands(long long v) {
    std::wstring s = std::to_wstring(v), out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0 && s[i - 1] != L'-') out.insert(out.begin(), L',');
    }
    return out;
}
