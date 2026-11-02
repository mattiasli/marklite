// Build tool: compresses MarkLite.exe into the payload that the installer embeds.
//   pack.exe <MarkLite.exe> <payload.bin>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <compressapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/payload.h"

static bool ReadFileAll(const wchar_t* path, std::vector<unsigned char>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD rd = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &rd, nullptr) && rd == out.size();
    }
    CloseHandle(f);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        fwprintf(stderr, L"usage: pack <input.exe> <payload.bin>\n");
        return 2;
    }
    std::vector<unsigned char> raw;
    if (!ReadFileAll(argv[1], raw)) {
        fwprintf(stderr, L"pack: cannot read %ls\n", argv[1]);
        return 1;
    }

    COMPRESSOR_HANDLE c = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c)) {
        fwprintf(stderr, L"pack: CreateCompressor failed (%lu)\n", GetLastError());
        return 1;
    }
    SIZE_T need = 0;
    Compress(c, raw.data(), raw.size(), nullptr, 0, &need);
    std::vector<unsigned char> packed(need);
    SIZE_T got = 0;
    if (!need || !Compress(c, raw.data(), raw.size(), packed.data(), packed.size(), &got)) {
        fwprintf(stderr, L"pack: Compress failed (%lu)\n", GetLastError());
        CloseCompressor(c);
        return 1;
    }
    CloseCompressor(c);
    packed.resize(got);

    PayloadHeader h{};
    memcpy(h.magic, kPayloadMagic, 4);
    h.format = kPayloadFormatLzms;
    h.rawSize = raw.size();
    h.crc = Crc32(raw.data(), raw.size());

    FILE* out = _wfopen(argv[2], L"wb");
    if (!out) {
        fwprintf(stderr, L"pack: cannot write %ls\n", argv[2]);
        return 1;
    }
    bool ok = fwrite(&h, sizeof(h), 1, out) == 1 && fwrite(packed.data(), 1, packed.size(), out) == packed.size();
    ok = (fclose(out) == 0) && ok;
    if (!ok) {
        fwprintf(stderr, L"pack: write failed\n");
        DeleteFileW(argv[2]);
        return 1;
    }
    wprintf(L"pack: %zu -> %zu bytes\n", raw.size(), packed.size() + sizeof(h));
    return 0;
}
