// Format of the compressed MarkLite.exe that is embedded in the installer (RCDATA resource).
// Shared by the installer and by tools/pack.cpp, which produces it at build time.
#pragma once
#include <stdint.h>
#include <stddef.h>

#pragma pack(push, 1)
struct PayloadHeader {
    char magic[4];       // "MLPK"
    uint32_t format;     // 1 = LZMS (Windows Compression API, buffer mode)
    uint64_t rawSize;    // size of MarkLite.exe
    uint32_t crc;        // CRC-32 of MarkLite.exe
    uint32_t reserved;
};
#pragma pack(pop)

static const char kPayloadMagic[4] = {'M', 'L', 'P', 'K'};
static const uint32_t kPayloadFormatLzms = 1;

inline uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    const unsigned char* p = (const unsigned char*)data;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
