#pragma once
#include <klib/types.hpp>

// CRC-32 (IEEE 802.3, reflected, polynomial 0xedb88320) -- the one GPT uses.
// Bitwise rather than table-driven: it only ever checks a few kilobytes of
// partition table at boot.
inline u32 crc32(const void* data, usize length, u32 crc = 0) {
    const auto* bytes = static_cast<const u8*>(data);
    crc = ~crc;
    for (usize i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
