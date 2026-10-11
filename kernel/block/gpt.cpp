#include <block/gpt.hpp>
#include <klib/crc32.hpp>

namespace gpt {
namespace {

struct [[gnu::packed]] Header {
    u8  signature[8];        // "EFI PART"
    u32 revision;
    u32 header_size;
    u32 header_crc;
    u32 reserved;
    u64 current_lba;
    u64 backup_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8  disk_guid[16];
    u64 entries_lba;
    u32 entry_count;
    u32 entry_size;
    u32 entries_crc;
};

struct [[gnu::packed]] Entry {
    u8  type_guid[16];
    u8  unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
};

static_assert(sizeof(Header) == 92, "GPT header is 92 bytes");
static_assert(sizeof(Entry) == 128, "GPT entries are 128 bytes");

// Large enough for the standard 128 entries of 128 bytes.
constexpr u32 kMaxEntryBytes = 128 * 128;

bool all_zero(const u8* bytes, usize length) {
    for (usize i = 0; i < length; i++) if (bytes[i]) return false;
    return true;
}

} // namespace

const char* error_name(Error error) {
    switch (error) {
    case Error::None:          return "ok";
    case Error::Io:            return "read error";
    case Error::NoTable:       return "no GPT signature";
    case Error::BadHeaderCrc:  return "header checksum mismatch";
    case Error::BadEntriesCrc: return "partition entry checksum mismatch";
    case Error::BadGeometry:   return "inconsistent geometry";
    }
    return "unknown error";
}

Error read(block::Device& disk, Table& out) {
    out.count = 0;

    alignas(8) u8 sector[block::kSectorSize];
    if (disk.read(1, 1, sector) < 0) return Error::Io;

    Header header;
    for (usize i = 0; i < sizeof(header); i++) reinterpret_cast<u8*>(&header)[i] = sector[i];

    const char kSignature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (int i = 0; i < 8; i++) if (header.signature[i] != static_cast<u8>(kSignature[i])) return Error::NoTable;

    if (header.header_size < sizeof(Header) || header.header_size > block::kSectorSize) return Error::BadGeometry;
    // The CRC covers header_size bytes with the CRC field itself zeroed.
    const u32 stored_crc = header.header_crc;
    sector[16] = sector[17] = sector[18] = sector[19] = 0;
    if (crc32(sector, header.header_size) != stored_crc) return Error::BadHeaderCrc;

    if (header.entry_size < sizeof(Entry) || header.entry_size % 8 != 0) return Error::BadGeometry;
    const u64 entry_bytes = static_cast<u64>(header.entry_count) * header.entry_size;
    if (entry_bytes == 0 || entry_bytes > kMaxEntryBytes) return Error::BadGeometry;
    if (header.first_usable_lba > header.last_usable_lba ||
        header.last_usable_lba >= disk.sector_count()) return Error::BadGeometry;

    alignas(8) u8 entries[kMaxEntryBytes];
    const u32 sectors = static_cast<u32>((entry_bytes + block::kSectorSize - 1) / block::kSectorSize);
    if (header.entries_lba + sectors > disk.sector_count()) return Error::BadGeometry;
    if (disk.read(header.entries_lba, sectors, entries) < 0) return Error::Io;
    if (crc32(entries, entry_bytes) != header.entries_crc) return Error::BadEntriesCrc;

    for (u32 i = 0; i < header.entry_count && out.count < kMaxPartitions; i++) {
        Entry entry;
        const u8* raw = entries + static_cast<u64>(i) * header.entry_size;
        for (usize b = 0; b < sizeof(entry); b++) reinterpret_cast<u8*>(&entry)[b] = raw[b];
        if (all_zero(entry.type_guid, sizeof(entry.type_guid))) continue;

        if (entry.first_lba > entry.last_lba || entry.first_lba < header.first_usable_lba ||
            entry.last_lba > header.last_usable_lba) return Error::BadGeometry;

        Partition& p = out.partitions[out.count++];
        p.number = i + 1;
        p.first_lba = entry.first_lba;
        p.last_lba = entry.last_lba;
        for (int g = 0; g < 16; g++) p.type_guid[g] = entry.type_guid[g];
        int n = 0;
        for (; n < 36 && entry.name[n]; n++) p.name[n] = entry.name[n] < 0x80 ? static_cast<char>(entry.name[n]) : '?';
        p.name[n] = '\0';
    }
    return Error::None;
}

} // namespace gpt
