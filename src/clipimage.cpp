#include "clipimage.h"
#include "format.h"
#include <wincodec.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cstring>

namespace {

UINT PngFormat() {
    static UINT f = RegisterClipboardFormatW(L"PNG");
    return f;
}

std::wstring SysError(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Error " + std::to_wstring(e);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

struct ClipboardLock {
    bool ok;
    explicit ClipboardLock(HWND owner) {
        ok = false;
        for (int i = 0; i < 5 && !ok; i++) {   // another program may hold it for a moment
            ok = OpenClipboard(owner) != 0;
            if (!ok) Sleep(20);
        }
    }
    ~ClipboardLock() { if (ok) CloseClipboard(); }
};

// Length of a PNG stream inside a (possibly over-allocated) clipboard block, or 0 if it isn't one.
size_t PngLength(const BYTE* p, size_t n) {
    static const BYTE sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || memcmp(p, sig, 8)) return 0;
    size_t at = 8;
    while (at + 12 <= n) {
        size_t len = ((size_t)p[at] << 24) | ((size_t)p[at + 1] << 16) | ((size_t)p[at + 2] << 8) | p[at + 3];
        size_t next = at + 12 + len;
        if (len > n || next > n) return 0;
        if (!memcmp(p + at + 4, "IEND", 4)) return next;
        at = next;
    }
    return 0;
}

std::wstring NewImagePath(const std::wstring& folder) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[40];
    swprintf(stamp, 40, L"image-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = folder + L"\\" + stamp, path = base + L".png";
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; i++) path = base + L"-" + std::to_wstring(i) + L".png";
    return path;
}

bool WriteBytes(const std::wstring& path, const BYTE* p, size_t n, std::wstring& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = SysError(GetLastError()); return false; }
    DWORD done = 0;
    bool ok = WriteFile(f, p, (DWORD)n, &done, nullptr) && done == n;
    if (!ok) error = SysError(GetLastError());
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Encodes a bitmap as a 24-bit PNG (screenshots have no transparency; bitmaps from the
// clipboard often carry an alpha channel of zeros, which would make the image invisible).
bool WriteBitmapPng(HBITMAP hb, const std::wstring& path, std::wstring& error) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    UINT w = 0, h = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &bmp);
    if (SUCCEEDED(hr)) hr = bmp->GetSize(&w, &h);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(bmp.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    bool created = SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(conv.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) {
        frame.Reset(); enc.Reset(); stream.Reset();
        if (created) DeleteFileW(path.c_str());
        error = SysError((DWORD)hr);
        return false;
    }
    return true;
}

}  // namespace

bool Clip_HasText() {
    return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
}

bool Clip_HasImage() {
    return IsClipboardFormatAvailable(PngFormat()) || IsClipboardFormatAvailable(CF_BITMAP) ||
           IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5);
}

std::vector<std::wstring> Clip_ImageFiles() {
    std::vector<std::wstring> files;
    if (!IsClipboardFormatAvailable(CF_HDROP)) return files;
    ClipboardLock lock(nullptr);
    if (!lock.ok) return files;
    if (HDROP drop = (HDROP)GetClipboardData(CF_HDROP)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH * 2];
            if (DragQueryFileW(drop, i, p, MAX_PATH * 2) && Fmt_IsImageFile(p)) files.push_back(p);
        }
    }
    return files;
}

bool Clip_HasPicture() { return Clip_HasImage() || !Clip_ImageFiles().empty(); }

std::wstring Clip_SaveImage(HWND owner, const std::wstring& folder, std::wstring& error) {
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Couldn't create the folder " + folder + L": " + SysError(GetLastError());
        return L"";
    }
    ClipboardLock lock(owner);
    if (!lock.ok) { error = L"Another program is using the clipboard. Try pasting again."; return L""; }
    std::wstring path = NewImagePath(folder), why;

    // A PNG stream (Snipping Tool, browsers) is saved as it is, keeping any transparency.
    if (HANDLE h = GetClipboardData(PngFormat())) {
        if (const BYTE* p = (const BYTE*)GlobalLock(h)) {
            size_t n = PngLength(p, GlobalSize(h));
            bool ok = n && WriteBytes(path, p, n, why);
            GlobalUnlock(h);
            if (ok) return path;
            if (n) { error = L"Couldn't save " + path + L": " + why; return L""; }
        }
    }
    // Otherwise a bitmap; Windows converts DIB / DIBV5 clipboard data to CF_BITMAP for us.
    HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!hb) { error = L"The clipboard doesn't contain a picture."; return L""; }
    if (!WriteBitmapPng(hb, path, why)) { error = L"Couldn't save " + path + L": " + why; return L""; }
    return path;
}
