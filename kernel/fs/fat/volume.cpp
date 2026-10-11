#include <fs/fat/volume.hpp>
#include <mm/heap.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace fat {
namespace {

constexpr u32 kSectorSize      = block::kSectorSize;
constexpr u32 kEntrySize       = 32;
constexpr u32 kEntriesPerSector = kSectorSize / kEntrySize;
constexpr u32 kFatMask         = 0x0fffffff;
constexpr u32 kEndOfChain      = 0x0ffffff8;   // and above
constexpr u32 kEndMark         = 0x0fffffff;
constexpr u32 kUnknown         = 0xffffffff;
constexpr u32 kMaxDirectoryEntries = 65536;    // the format's own limit

constexpr u8 kAttrReadOnly  = 0x01;
constexpr u8 kAttrVolumeId  = 0x08;
constexpr u8 kAttrDirectory = 0x10;
constexpr u8 kAttrArchive   = 0x20;
constexpr u8 kAttrLongName  = 0x0f;
constexpr u8 kAttrLongMask  = 0x3f;

constexpr u8 kCaseLowerBase = 0x08;
constexpr u8 kCaseLowerExt  = 0x10;
constexpr u8 kDeleted       = 0xe5;
constexpr u8 kLastLongEntry = 0x40;
constexpr u32 kCharsPerLongEntry = 13;

// No real-time clock driver yet: every timestamp is 2026-01-01 00:00.
constexpr u16 kFixedDate = ((2026 - 1980) << 9) | (1 << 5) | 1;

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
void put16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void put32(u8* p, u32 v) { for (int i = 0; i < 4; i++) p[i] = static_cast<u8>(v >> (8 * i)); }

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool names_equal(const char* a, const char* b) {
    while (*a && ascii_lower(*a) == ascii_lower(*b)) { a++; b++; }
    return *a == *b;
}

// Characters allowed in a short name besides letters and digits.
bool short_name_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    for (const char* p = "!#$%&'()-@^_`{}~"; *p; p++) if (*p == c) return true;
    return false;
}

u32 entry_cluster(const u8* entry) {
    return (static_cast<u32>(le16(entry + 20)) << 16) | le16(entry + 26);
}

void set_entry_cluster(u8* entry, u32 cluster) {
    put16(entry + 20, static_cast<u16>(cluster >> 16));
    put16(entry + 26, static_cast<u16>(cluster));
}

// UTF-16 (from a long name) to UTF-8, stopping at a NUL or 0xffff padding.
void utf16_to_utf8(const u16* in, u32 units, char* out, usize size) {
    usize length = 0;
    auto put = [&](u32 byte) { if (length + 1 < size) out[length++] = static_cast<char>(byte); };
    for (u32 i = 0; i < units && in[i] != 0 && in[i] != 0xffff; i++) {
        u32 c = in[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < units && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00);
        } else if (c >= 0xd800 && c < 0xe000) {
            c = '?';   // an unpaired surrogate
        }
        if (c < 0x80) { put(c); }
        else if (c < 0x800) { put(0xc0 | (c >> 6)); put(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { put(0xe0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3f)); put(0x80 | (c & 0x3f)); }
        else { put(0xf0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3f)); put(0x80 | ((c >> 6) & 0x3f)); put(0x80 | (c & 0x3f)); }
    }
    out[length] = '\0';
}

// UTF-8 to UTF-16. Returns the number of units, or -1 for malformed input or
// more than `capacity` units.
int utf8_to_utf16(const char* in, u16* out, u32 capacity) {
    u32 units = 0;
    const auto* s = reinterpret_cast<const u8*>(in);
    while (*s) {
        u32 c, extra;
        if (*s < 0x80)              { c = *s; extra = 0; }
        else if ((*s & 0xe0) == 0xc0) { c = *s & 0x1f; extra = 1; }
        else if ((*s & 0xf0) == 0xe0) { c = *s & 0x0f; extra = 2; }
        else if ((*s & 0xf8) == 0xf0) { c = *s & 0x07; extra = 3; }
        else return -1;
        s++;
        for (u32 i = 0; i < extra; i++, s++) {
            if ((*s & 0xc0) != 0x80) return -1;
            c = (c << 6) | (*s & 0x3f);
        }
        // Overlong encodings and surrogates are not valid UTF-8.
        if ((extra == 1 && c < 0x80) || (extra == 2 && c < 0x800) || (extra == 3 && c < 0x10000)) return -1;
        if ((c >= 0xd800 && c < 0xe000) || c > 0x10ffff) return -1;

        if (c >= 0x10000) {
            if (units + 2 > capacity) return -1;
            c -= 0x10000;
            out[units++] = static_cast<u16>(0xd800 + (c >> 10));
            out[units++] = static_cast<u16>(0xdc00 + (c & 0x3ff));
        } else {
            if (units + 1 > capacity) return -1;
            out[units++] = static_cast<u16>(c);
        }
    }
    return static_cast<int>(units);
}

// A directory entry with its name assembled from any long-name entries
// before it.
struct Named {
    char name[path::kNameMax + 1];
    u8   raw[kEntrySize];   // the short entry
    u32  index;             // of the short entry
    u32  first_index;       // of the first long-name entry, or == index
};

} // namespace

// --- name handling ---------------------------------------------------------------

u8 short_name_checksum(const u8 name[11]) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

bool valid_long_name(const char* name) {
    usize length = strlen(name);
    if (length == 0 || length > path::kNameMax) return false;
    if (name[length - 1] == '.' || name[length - 1] == ' ') return false;
    for (const char* p = name; *p; p++) {
        const auto c = static_cast<u8>(*p);
        if (c < 0x20) return false;
        for (const char* bad = "\\/:*?\"<>|"; *bad; bad++) if (*p == *bad) return false;
    }
    return true;
}

bool exact_short_name(const char* name, u8 out[11], u8& case_flags) {
    case_flags = 0;
    const usize length = strlen(name);
    if (length == 0 || length > 12) return false;

    const char* dot = nullptr;
    for (const char* p = name; *p; p++) {
        if (*p == '.') { if (dot) return false; dot = p; }
        else if (!short_name_char(*p)) return false;
    }
    const usize base_length = dot ? static_cast<usize>(dot - name) : length;
    const usize ext_length = dot ? length - base_length - 1 : 0;
    if (base_length == 0 || base_length > 8 || ext_length > 3 || (dot && ext_length == 0)) return false;

    // Each part may be all lower case or all upper case, not mixed: the NT
    // flags only record one case per part.
    auto part_case = [](const char* p, usize n, bool& lower, bool& upper) {
        lower = upper = false;
        for (usize i = 0; i < n; i++) {
            if (p[i] >= 'a' && p[i] <= 'z') lower = true;
            if (p[i] >= 'A' && p[i] <= 'Z') upper = true;
        }
    };
    bool lower, upper;
    part_case(name, base_length, lower, upper);
    if (lower && upper) return false;
    if (lower) case_flags |= kCaseLowerBase;
    if (dot) {
        part_case(dot + 1, ext_length, lower, upper);
        if (lower && upper) return false;
        if (lower) case_flags |= kCaseLowerExt;
    }

    for (int i = 0; i < 11; i++) out[i] = ' ';
    for (usize i = 0; i < base_length; i++) out[i] = static_cast<u8>(ascii_upper(name[i]));
    for (usize i = 0; i < ext_length; i++) out[8 + i] = static_cast<u8>(ascii_upper(dot[1 + i]));
    // 0xe5 as a first byte means "deleted"; a real one is stored as 0x05.
    if (out[0] == kDeleted) out[0] = 0x05;
    return true;
}

void generated_short_name(const char* name, u32 n, u8 out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    // The extension comes from after the last dot, unless that dot leads.
    const char* dot = nullptr;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    if (dot == name) dot = nullptr;

    auto convert = [](char c) -> u8 {
        if (static_cast<u8>(c) >= 0x80) return '_';
        c = ascii_upper(c);
        return short_name_char(c) ? static_cast<u8>(c) : '_';
    };

    u8 base[8];
    u32 base_length = 0;
    for (const char* p = name; *p && p != dot && base_length < 8; p++) {
        if (*p == ' ' || *p == '.') continue;
        base[base_length++] = convert(*p);
    }
    if (base_length == 0) base[base_length++] = '_';

    u32 ext_length = 0;
    if (dot) {
        for (const char* p = dot + 1; *p && ext_length < 3; p++) {
            if (*p == ' ' || *p == '.') continue;
            out[8 + ext_length++] = convert(*p);
        }
    }

    char tail[12];
    u32 tail_length = 0;
    char digits[11];
    u32 digit_count = 0;
    do { digits[digit_count++] = static_cast<char>('0' + n % 10); n /= 10; } while (n);
    tail[tail_length++] = '~';
    while (digit_count) tail[tail_length++] = digits[--digit_count];

    const u32 keep = base_length + tail_length > 8 ? 8 - tail_length : base_length;
    for (u32 i = 0; i < keep; i++) out[i] = base[i];
    for (u32 i = 0; i < tail_length; i++) out[keep + i] = static_cast<u8>(tail[i]);
    if (out[0] == kDeleted) out[0] = 0x05;
}

void format_short_name(const u8 raw[11], u8 case_flags, char* out) {
    usize length = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = static_cast<char>((i == 0 && raw[0] == 0x05) ? kDeleted : raw[i]);
        out[length++] = (case_flags & kCaseLowerBase) ? ascii_lower(c) : c;
    }
    if (raw[8] != ' ') {
        out[length++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            const char c = static_cast<char>(raw[i]);
            out[length++] = (case_flags & kCaseLowerExt) ? ascii_lower(c) : c;
        }
    }
    out[length] = '\0';
}

// --- directory scanning ---------------------------------------------------------

struct Volume::Location {
    u64 sector;
    u32 offset;   // byte offset of the entry in that sector
};

// Walks a directory's raw 32-byte entries in order, following its cluster
// chain one sector at a time.
class Volume::DirectoryScan {
public:
    DirectoryScan(Volume& volume, u32 first_cluster, u32 start_index)
        : m_volume(volume), m_cluster(first_cluster), m_index(start_index) {}

    // 1 with the next entry, 0 past the end of the chain, or -errno.
    int next(u8 entry[kEntrySize], u32& index) {
        const u32 per_cluster = m_volume.m_cluster_bytes / kEntrySize;
        if (m_index >= kMaxDirectoryEntries || m_cluster == 0) return 0;

        const u32 target = m_index / per_cluster;
        while (m_cluster_index < target) {
            u32 next_cluster;
            const int status = m_volume.fat_get(m_cluster, next_cluster);
            if (status < 0) return status;
            if (next_cluster >= kEndOfChain) return 0;
            if (!m_volume.valid_cluster(next_cluster)) return -SIRIUS_EIO;
            m_cluster = next_cluster;
            m_cluster_index++;
        }

        const u32 within = m_index % per_cluster;
        const u64 sector = m_volume.cluster_sector(m_cluster) + within / kEntriesPerSector;
        if (sector != m_loaded) {
            const int status = m_volume.read_sectors(sector, 1, m_sector);
            if (status < 0) return status;
            m_loaded = sector;
        }
        memcpy(entry, m_sector + (within % kEntriesPerSector) * kEntrySize, kEntrySize);
        index = m_index++;
        return 1;
    }

    // Read the next live entry, with its long name assembled. 1, 0 at the end
    // of the directory, or -errno. Skips deleted entries and the volume label.
    int next_named(Named& out) {
        u16 long_name[20 * kCharsPerLongEntry + 1];
        bool long_valid = false;
        u8 long_checksum = 0;
        u32 expected = 0, long_first = 0;

        u8 entry[kEntrySize];
        u32 index;
        for (;;) {
            const int status = next(entry, index);
            if (status <= 0) return status;
            if (entry[0] == 0x00) return 0;                        // end of directory
            if (entry[0] == kDeleted) { long_valid = false; continue; }

            if ((entry[11] & kAttrLongMask) == kAttrLongName) {
                const u32 sequence = entry[0] & 0x1f;
                if (entry[0] & kLastLongEntry) {
                    long_valid = sequence >= 1 && sequence <= 20;
                    long_checksum = entry[13];
                    long_first = index;
                    for (auto& c : long_name) c = 0;
                } else if (!long_valid || sequence != expected - 1 || entry[13] != long_checksum) {
                    long_valid = false;
                }
                expected = sequence;
                if (long_valid) {
                    u16* chars = &long_name[(sequence - 1) * kCharsPerLongEntry];
                    const u8 offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (u32 i = 0; i < 13; i++) chars[i] = le16(entry + offsets[i]);
                }
                continue;
            }

            if (entry[11] & kAttrVolumeId) { long_valid = false; continue; }

            memcpy(out.raw, entry, kEntrySize);
            out.index = index;
            if (long_valid && expected == 1 && long_checksum == short_name_checksum(entry)) {
                utf16_to_utf8(long_name, 20 * kCharsPerLongEntry, out.name, sizeof(out.name));
                out.first_index = long_first;
            } else {
                format_short_name(entry, entry[12], out.name);
                out.first_index = index;
            }
            return 1;
        }
    }

    u32 position() const { return m_index; }

private:
    Volume& m_volume;
    u32 m_cluster;
    u32 m_cluster_index = 0;
    u32 m_index;
    u64 m_loaded = ~0ULL;
    u8  m_sector[kSectorSize];
};

// --- sectors, clusters and the FAT -------------------------------------------------

int Volume::read_sectors(u64 sector, u32 count, void* buffer) { return m_device->read(sector, count, buffer); }
int Volume::write_sectors(u64 sector, u32 count, const void* buffer) { return m_device->write(sector, count, buffer); }

u64 Volume::cluster_sector(u32 cluster) const {
    return m_data_start + static_cast<u64>(cluster - 2) * m_sectors_per_cluster;
}

bool Volume::valid_cluster(u32 cluster) const { return cluster >= 2 && cluster < m_cluster_count + 2; }

int Volume::fat_get(u32 cluster, u32& value) {
    if (!valid_cluster(cluster)) return -SIRIUS_EIO;
    u8 sector[kSectorSize];
    const u64 offset = static_cast<u64>(cluster) * 4;
    const int status = read_sectors(m_reserved_sectors + offset / kSectorSize, 1, sector);
    if (status < 0) return status;
    value = le32(sector + offset % kSectorSize) & kFatMask;
    return 0;
}

int Volume::fat_set(u32 cluster, u32 value) {
    if (!valid_cluster(cluster)) return -SIRIUS_EIO;
    const u64 offset = static_cast<u64>(cluster) * 4;
    u8 sector[kSectorSize];
    // Every copy of the FAT, kept identical. The top four bits are reserved
    // and must be preserved.
    for (u32 copy = 0; copy < m_fat_count; copy++) {
        const u64 lba = m_reserved_sectors + static_cast<u64>(copy) * m_fat_sectors + offset / kSectorSize;
        int status = read_sectors(lba, 1, sector);
        if (status < 0) return status;
        u8* entry = sector + offset % kSectorSize;
        put32(entry, (le32(entry) & ~kFatMask) | (value & kFatMask));
        status = write_sectors(lba, 1, sector);
        if (status < 0) return status;
    }
    return 0;
}

int Volume::zero_cluster(u32 cluster) {
    u8 zeros[kSectorSize] = {};
    const u64 first = cluster_sector(cluster);
    for (u32 i = 0; i < m_sectors_per_cluster; i++) {
        const int status = write_sectors(first + i, 1, zeros);
        if (status < 0) return status;
    }
    return 0;
}

int Volume::allocate_cluster(u32 previous, bool zero, u32& out) {
    if (m_free_count == 0) return -SIRIUS_ENOSPC;

    // Scan the FAT a sector at a time from the hint, wrapping around once.
    u8 sector[kSectorSize];
    u64 loaded = ~0ULL;
    u32 found = 0;
    for (u32 i = 0; i < m_cluster_count && !found; i++) {
        const u32 cluster = 2 + (m_next_free - 2 + i) % m_cluster_count;
        const u64 offset = static_cast<u64>(cluster) * 4;
        const u64 lba = m_reserved_sectors + offset / kSectorSize;
        if (lba != loaded) {
            const int status = read_sectors(lba, 1, sector);
            if (status < 0) return status;
            loaded = lba;
        }
        if ((le32(sector + offset % kSectorSize) & kFatMask) == 0) found = cluster;
    }
    if (!found) { m_free_count = 0; return -SIRIUS_ENOSPC; }

    // Contents first, then the FAT entry, then the link: a crash part-way
    // leaves at worst a lost cluster, never a chain into garbage.
    int status;
    if (zero && (status = zero_cluster(found)) < 0) return status;
    if ((status = fat_set(found, kEndMark)) < 0) return status;
    if (previous && (status = fat_set(previous, found)) < 0) return status;

    m_next_free = found + 1 < m_cluster_count + 2 ? found + 1 : 2;
    if (m_free_count != kUnknown) m_free_count--;
    m_fsinfo_dirty = true;
    out = found;
    return 0;
}

int Volume::free_chain(u32 first) {
    u32 cluster = first;
    for (u32 steps = 0; valid_cluster(cluster); steps++) {
        if (steps > m_cluster_count) return -SIRIUS_EIO;   // a loop in the chain
        u32 next;
        int status = fat_get(cluster, next);
        if (status < 0) return status;
        if ((status = fat_set(cluster, 0)) < 0) return status;
        if (m_free_count != kUnknown) m_free_count++;
        if (cluster < m_next_free) m_next_free = cluster;
        m_fsinfo_dirty = true;
        if (next >= kEndOfChain) break;
        cluster = next;
    }
    return 0;
}

int Volume::cluster_at(Node& node, u32 index, bool extend, u32& out) {
    if (node.first_cluster == 0) {
        if (!extend) return -SIRIUS_EIO;
        u32 first;
        const int status = allocate_cluster(0, node.directory, first);
        if (status < 0) return status;
        node.first_cluster = first;
        node.cursor_index = 0;
        node.cursor_cluster = first;
    }

    u32 position = 0, cluster = node.first_cluster;
    if (node.cursor_cluster && node.cursor_index <= index) {
        position = node.cursor_index;
        cluster = node.cursor_cluster;
    }

    while (position < index) {
        u32 next;
        int status = fat_get(cluster, next);
        if (status < 0) return status;
        if (next >= kEndOfChain) {
            if (!extend) return -SIRIUS_EIO;
            if ((status = allocate_cluster(cluster, node.directory, next)) < 0) return status;
        } else if (!valid_cluster(next)) {
            return -SIRIUS_EIO;
        }
        cluster = next;
        position++;
    }

    node.cursor_index = index;
    node.cursor_cluster = cluster;
    out = cluster;
    return 0;
}

int Volume::locate(u32 directory_cluster, u32 index, bool extend, Location& out) {
    if (index >= kMaxDirectoryEntries) return -SIRIUS_ENOSPC;
    Node directory{};
    directory.first_cluster = directory_cluster;
    directory.directory = true;
    const u32 per_cluster = m_cluster_bytes / kEntrySize;

    u32 cluster;
    const int status = cluster_at(directory, index / per_cluster, extend, cluster);
    if (status < 0) return status;
    const u32 within = index % per_cluster;
    out.sector = cluster_sector(cluster) + within / kEntriesPerSector;
    out.offset = (within % kEntriesPerSector) * kEntrySize;
    return 0;
}

int Volume::update_entry(const Node& node) {
    if (node.is_root) return 0;
    Location location;
    int status = locate(node.parent_cluster, node.entry_index, false, location);
    if (status < 0) return status;

    u8 sector[kSectorSize];
    if ((status = read_sectors(location.sector, 1, sector)) < 0) return status;
    u8* entry = sector + location.offset;
    set_entry_cluster(entry, node.first_cluster);
    put32(entry + 28, node.directory ? 0 : node.size);
    put16(entry + 22, 0);            // write time
    put16(entry + 24, kFixedDate);   // write date
    return write_sectors(location.sector, 1, sector);
}

int Volume::write_entries(u32 directory_cluster, u32 first, const u8* entries, u32 count) {
    u8 sector[kSectorSize];
    for (u32 i = 0; i < count; i++) {
        Location location;
        int status = locate(directory_cluster, first + i, true, location);
        if (status < 0) return status;
        if ((status = read_sectors(location.sector, 1, sector)) < 0) return status;
        memcpy(sector + location.offset, entries + i * kEntrySize, kEntrySize);
        if ((status = write_sectors(location.sector, 1, sector)) < 0) return status;
    }
    return 0;
}

// --- public operations -----------------------------------------------------------

int Volume::mount(block::Device* device) {
    m_device = device;
    u8 boot[kSectorSize];
    int status = read_sectors(0, 1, boot);
    if (status < 0) return status;

    if (boot[510] != 0x55 || boot[511] != 0xaa) return -SIRIUS_EINVAL;
    const u32 bytes_per_sector = le16(boot + 11);
    m_sectors_per_cluster      = boot[13];
    m_reserved_sectors         = le16(boot + 14);
    m_fat_count                = boot[16];
    const u32 root_entries     = le16(boot + 17);
    const u32 total16          = le16(boot + 19);
    const u32 fat16_size       = le16(boot + 22);
    const u32 total32          = le32(boot + 32);
    m_fat_sectors              = le32(boot + 36);
    m_root_cluster             = le32(boot + 44);
    m_fsinfo_sector            = le16(boot + 48);

    // FAT32 is what the BPB says it is: no FAT16 sizes, no fixed root.
    if (bytes_per_sector != kSectorSize) return -SIRIUS_EINVAL;
    if (m_sectors_per_cluster == 0 || (m_sectors_per_cluster & (m_sectors_per_cluster - 1)) ||
        m_sectors_per_cluster > 128) return -SIRIUS_EINVAL;
    if (m_reserved_sectors == 0 || m_fat_count == 0 || m_fat_count > 2) return -SIRIUS_EINVAL;
    if (root_entries != 0 || fat16_size != 0 || m_fat_sectors == 0) return -SIRIUS_EINVAL;

    const u64 total = total16 ? total16 : total32;
    if (total > device->sector_count()) return -SIRIUS_EINVAL;
    m_data_start = m_reserved_sectors + static_cast<u64>(m_fat_count) * m_fat_sectors;
    if (m_data_start >= total) return -SIRIUS_EINVAL;
    const u64 clusters = (total - m_data_start) / m_sectors_per_cluster;
    if (clusters == 0 || clusters > 0x0ffffff5 || clusters + 2 > static_cast<u64>(m_fat_sectors) * (kSectorSize / 4))
        return -SIRIUS_EINVAL;
    m_cluster_count = static_cast<u32>(clusters);
    m_cluster_bytes = m_sectors_per_cluster * kSectorSize;
    if (!valid_cluster(m_root_cluster)) return -SIRIUS_EINVAL;

    // FSInfo hints are only hints: take them if they look sane.
    m_free_count = kUnknown;
    m_next_free = 2;
    if (m_fsinfo_sector && m_fsinfo_sector < m_reserved_sectors) {
        u8 info[kSectorSize];
        if (read_sectors(m_fsinfo_sector, 1, info) == 0 && le32(info) == 0x41615252 &&
            le32(info + 484) == 0x61417272) {
            const u32 free = le32(info + 488), next = le32(info + 492);
            if (free <= m_cluster_count) m_free_count = free;
            if (valid_cluster(next)) m_next_free = next;
        } else {
            m_fsinfo_sector = 0;
        }
    }
    return 0;
}

Node Volume::root() const {
    Node node{};
    node.first_cluster = m_root_cluster;
    node.directory = true;
    node.is_root = true;
    return node;
}

Geometry Volume::geometry() const { return {m_sectors_per_cluster, m_cluster_count, m_free_count}; }

int Volume::find(const Node& directory, const char* name, Node& out, u32* first_slot) {
    DirectoryScan scan(*this, directory.first_cluster, 0);
    Named entry;
    for (;;) {
        const int status = scan.next_named(entry);
        if (status < 0) return status;
        if (status == 0) return -SIRIUS_ENOENT;

        char short_form[13];
        format_short_name(entry.raw, 0, short_form);
        if (!names_equal(entry.name, name) && !names_equal(short_form, name)) continue;
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) continue;

        // Built aside and assigned last: `out` may be `directory` itself.
        Node result{};
        result.first_cluster = entry_cluster(entry.raw);
        result.directory = (entry.raw[11] & kAttrDirectory) != 0;
        result.size = result.directory ? 0 : le32(entry.raw + 28);
        result.parent_cluster = directory.first_cluster;
        result.entry_index = entry.index;
        if (first_slot) *first_slot = entry.first_index;
        out = result;
        return 0;
    }
}

int Volume::lookup(const Node& directory, const char* name, Node& out) {
    if (!directory.directory) return -SIRIUS_ENOTDIR;
    return find(directory, name, out);
}

int Volume::readdir(const Node& directory, u64& cookie, Entry& out) {
    if (!directory.directory) return -SIRIUS_ENOTDIR;
    if (cookie >= kMaxDirectoryEntries) return 0;
    DirectoryScan scan(*this, directory.first_cluster, static_cast<u32>(cookie));
    Named entry;
    for (;;) {
        const int status = scan.next_named(entry);
        if (status <= 0) { cookie = scan.position(); return status; }
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) continue;

        memcpy(out.name, entry.name, sizeof(out.name));
        out.node = {};
        out.node.first_cluster = entry_cluster(entry.raw);
        out.node.directory = (entry.raw[11] & kAttrDirectory) != 0;
        out.node.size = out.node.directory ? 0 : le32(entry.raw + 28);
        out.node.parent_cluster = directory.first_cluster;
        out.node.entry_index = entry.index;
        cookie = entry.index + 1;
        return 1;
    }
}

i64 Volume::read(Node& file, u64 offset, void* buffer, u64 length) {
    if (file.directory) return -SIRIUS_EISDIR;
    if (offset >= file.size) return 0;
    if (length > file.size - offset) length = file.size - offset;

    auto* out = static_cast<u8*>(buffer);
    u8 sector[kSectorSize];
    u64 done = 0;
    while (done < length) {
        const u64 position = offset + done;
        u32 cluster;
        const int status = cluster_at(file, static_cast<u32>(position / m_cluster_bytes), false, cluster);
        if (status < 0) return done ? static_cast<i64>(done) : status;

        const u32 within = static_cast<u32>(position % m_cluster_bytes);
        const u64 lba = cluster_sector(cluster) + within / kSectorSize;
        const u32 sector_offset = within % kSectorSize;
        u64 n = m_cluster_bytes - within;
        if (n > length - done) n = length - done;

        int io;
        if (sector_offset == 0 && n >= kSectorSize) {
            n -= n % kSectorSize;   // whole sectors, straight into the caller's buffer
            io = read_sectors(lba, static_cast<u32>(n / kSectorSize), out + done);
        } else {
            if (n > kSectorSize - sector_offset) n = kSectorSize - sector_offset;
            io = read_sectors(lba, 1, sector);
            if (io == 0) memcpy(out + done, sector + sector_offset, n);
        }
        if (io < 0) return done ? static_cast<i64>(done) : io;
        done += n;
    }
    return static_cast<i64>(done);
}

i64 Volume::write(Node& file, u64 offset, const void* buffer, u64 length) {
    if (file.directory) return -SIRIUS_EISDIR;
    if (length == 0) return 0;
    if (offset > 0xffffffffULL) return -SIRIUS_EFBIG;
    if (length > 0xffffffffULL - offset) length = 0xffffffffULL - offset;   // FAT's 4 GiB limit
    if (length == 0) return -SIRIUS_EFBIG;

    // A write past the end leaves a gap that must read back as zeros, and the
    // clusters it lands in may hold old data: fill it explicitly.
    if (offset > file.size) {
        const u8 zeros[kSectorSize] = {};
        while (file.size < offset) {
            const u64 gap = offset - file.size;
            const i64 n = write(file, file.size, zeros, gap < sizeof(zeros) ? gap : sizeof(zeros));
            if (n < 0) return n;
        }
    }

    const u32 first_before = file.first_cluster;
    const auto* in = static_cast<const u8*>(buffer);
    u8 sector[kSectorSize];
    u64 done = 0;
    int failure = 0;
    while (done < length) {
        const u64 position = offset + done;
        u32 cluster;
        failure = cluster_at(file, static_cast<u32>(position / m_cluster_bytes), true, cluster);
        if (failure < 0) break;

        const u32 within = static_cast<u32>(position % m_cluster_bytes);
        const u64 lba = cluster_sector(cluster) + within / kSectorSize;
        const u32 sector_offset = within % kSectorSize;
        u64 n = m_cluster_bytes - within;
        if (n > length - done) n = length - done;

        if (sector_offset == 0 && n >= kSectorSize) {
            n -= n % kSectorSize;
            failure = write_sectors(lba, static_cast<u32>(n / kSectorSize), in + done);
        } else {
            if (n > kSectorSize - sector_offset) n = kSectorSize - sector_offset;
            // Partial sector: read-modify-write, unless none of it is data yet.
            if (position - sector_offset >= file.size) memset(sector, 0, kSectorSize);
            else failure = read_sectors(lba, 1, sector);
            if (failure == 0) {
                memcpy(sector + sector_offset, in + done, n);
                failure = write_sectors(lba, 1, sector);
            }
        }
        if (failure < 0) break;
        done += n;
        if (position + n > file.size) file.size = static_cast<u32>(position + n);
    }

    if (done > 0 || file.first_cluster != first_before) {
        const int status = update_entry(file);
        if (status < 0 && failure == 0) failure = status;
    }
    if (done == 0 && failure < 0) return failure;
    return static_cast<i64>(done);
}

int Volume::truncate(Node& file, u64 size) {
    if (file.directory) return -SIRIUS_EISDIR;
    if (size > 0xffffffffULL) return -SIRIUS_EFBIG;
    if (size == file.size) return 0;

    if (size > file.size) {
        // Growing is writing zeros.
        const u8 zeros[kSectorSize] = {};
        while (file.size < size) {
            const u64 gap = size - file.size;
            const i64 n = write(file, file.size, zeros, gap < sizeof(zeros) ? gap : sizeof(zeros));
            if (n < 0) return static_cast<int>(n);
        }
        return 0;
    }

    const u32 keep = static_cast<u32>((size + m_cluster_bytes - 1) / m_cluster_bytes);
    int status = 0;
    if (keep == 0) {
        const u32 first = file.first_cluster;
        file.first_cluster = 0;
        file.size = 0;
        // The entry stops pointing at the chain before the chain is freed.
        if ((status = update_entry(file)) < 0) return status;
        if (first) status = free_chain(first);
    } else {
        u32 last, next;
        if ((status = cluster_at(file, keep - 1, false, last)) < 0) return status;
        if ((status = fat_get(last, next)) < 0) return status;
        file.size = static_cast<u32>(size);
        if ((status = update_entry(file)) < 0) return status;
        if (next < kEndOfChain) {
            if ((status = fat_set(last, kEndMark)) < 0) return status;
            if (valid_cluster(next)) status = free_chain(next);
        }
    }
    file.cursor_index = 0;
    file.cursor_cluster = 0;
    return status;
}

bool Volume::short_name_taken(const Node& directory, const u8 name[11]) {
    DirectoryScan scan(*this, directory.first_cluster, 0);
    u8 entry[kEntrySize];
    u32 index;
    while (scan.next(entry, index) > 0) {
        if (entry[0] == 0x00) break;
        if (entry[0] == kDeleted || (entry[11] & kAttrLongMask) == kAttrLongName) continue;
        if (memcmp(entry, name, 11) == 0) return true;
    }
    return false;
}

int Volume::find_free_run(const Node& directory, u32 count, u32& first) {
    DirectoryScan scan(*this, directory.first_cluster, 0);
    u8 entry[kEntrySize];
    u32 index = 0, run_start = 0, run_length = 0;
    for (;;) {
        const int status = scan.next(entry, index);
        if (status < 0) return status;
        if (status == 0) { index = scan.position(); break; }   // chain ends: extend from here
        if (entry[0] == 0x00) break;                          // everything from here is free
        if (entry[0] == kDeleted) {
            if (run_length++ == 0) run_start = index;
            if (run_length == count) { first = run_start; return 0; }
        } else {
            run_length = 0;
        }
    }
    first = run_length ? run_start : index;
    if (first + count > kMaxDirectoryEntries) return -SIRIUS_ENOSPC;
    return 0;
}

int Volume::create(const Node& directory, const char* name, bool is_directory, Node& out) {
    if (!directory.directory) return -SIRIUS_ENOTDIR;
    if (!valid_long_name(name)) return -SIRIUS_EINVAL;
    Node existing;
    const int found = find(directory, name, existing);
    if (found == 0) return -SIRIUS_EEXIST;
    if (found != -SIRIUS_ENOENT) return found;

    u8 short_name[11];
    u8 case_flags = 0;
    u16 long_name[path::kNameMax + 1];
    u32 long_entries = 0;

    if (!exact_short_name(name, short_name, case_flags)) {
        const int units = utf8_to_utf16(name, long_name, path::kNameMax);
        if (units <= 0) return -SIRIUS_EINVAL;
        long_entries = (static_cast<u32>(units) + kCharsPerLongEntry - 1) / kCharsPerLongEntry;
        // Pad: one NUL after the name (if room), then 0xffff to the end.
        for (u32 i = static_cast<u32>(units); i < long_entries * kCharsPerLongEntry; i++)
            long_name[i] = (i == static_cast<u32>(units)) ? 0 : 0xffff;

        u32 n = 1;
        for (;; n++) {
            if (n > 999999) return -SIRIUS_EEXIST;
            generated_short_name(name, n, short_name);
            if (!short_name_taken(directory, short_name)) break;
        }
    }

    const u32 total = long_entries + 1;
    u32 first;
    int status = find_free_run(directory, total, first);
    if (status < 0) return status;

    // A directory gets its cluster, with "." and "..", before anything points
    // at it.
    u32 cluster = 0;
    if (is_directory) {
        if ((status = allocate_cluster(0, true, cluster)) < 0) return status;
        u8 dots[kSectorSize] = {};   // the cluster is zeroed; this is its first sector
        for (int e = 0; e < 2; e++) {
            u8* d = dots + e * kEntrySize;
            memset(d, ' ', 11);
            d[0] = '.';
            if (e == 1) d[1] = '.';
            d[11] = kAttrDirectory;
            put16(d + 16, kFixedDate);
            put16(d + 18, kFixedDate);
            put16(d + 24, kFixedDate);
            // ".." in a directory directly under the root points at cluster 0.
            set_entry_cluster(d, e == 0 ? cluster : (directory.is_root ? 0 : directory.first_cluster));
        }
        if ((status = write_sectors(cluster_sector(cluster), 1, dots)) < 0) {
            free_chain(cluster);
            return status;
        }
    }

    auto* entries = static_cast<u8*>(kzalloc(total * kEntrySize));
    if (entries == nullptr) { if (cluster) free_chain(cluster); return -SIRIUS_ENOMEM; }

    const u8 checksum = short_name_checksum(short_name);
    const u8 offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (u32 k = 1; k <= long_entries; k++) {
        // Long entries are stored last-first, directly before the short one.
        u8* e = entries + (long_entries - k) * kEntrySize;
        e[0] = static_cast<u8>(k | (k == long_entries ? kLastLongEntry : 0));
        e[11] = kAttrLongName;
        e[13] = checksum;
        for (u32 i = 0; i < 13; i++) put16(e + offsets[i], long_name[(k - 1) * kCharsPerLongEntry + i]);
    }

    u8* s = entries + long_entries * kEntrySize;
    memcpy(s, short_name, 11);
    s[11] = is_directory ? kAttrDirectory : kAttrArchive;
    s[12] = case_flags;
    put16(s + 16, kFixedDate);   // creation date
    put16(s + 18, kFixedDate);   // access date
    put16(s + 24, kFixedDate);   // write date
    set_entry_cluster(s, cluster);

    status = write_entries(directory.first_cluster, first, entries, total);
    kfree(entries);
    if (status < 0) { if (cluster) free_chain(cluster); return status; }

    Node result{};   // `out` may alias `directory`
    result.first_cluster = cluster;
    result.directory = is_directory;
    result.parent_cluster = directory.first_cluster;
    result.entry_index = first + long_entries;
    out = result;
    return 0;
}

bool Volume::is_empty_directory(const Node& directory) {
    DirectoryScan scan(*this, directory.first_cluster, 0);
    Named entry;
    for (;;) {
        const int status = scan.next_named(entry);
        if (status < 0) return false;
        if (status == 0) return true;
        if (strcmp(entry.name, ".") != 0 && strcmp(entry.name, "..") != 0) return false;
    }
}

int Volume::unlink(const Node& directory, const char* name, const Node& victim) {
    if (!directory.directory) return -SIRIUS_ENOTDIR;
    Node found;
    u32 first_slot = 0;
    int status = find(directory, name, found, &first_slot);
    if (status < 0) return status;
    if (found.entry_index != victim.entry_index || found.parent_cluster != victim.parent_cluster)
        return -SIRIUS_ENOENT;
    if (found.directory && !is_empty_directory(found)) return -SIRIUS_ENOTEMPTY;

    // Mark every entry of the name deleted, then release the data: a crash
    // between the two loses clusters rather than leaving a name on freed ones.
    u8 sector[kSectorSize];
    for (u32 index = first_slot; index <= found.entry_index; index++) {
        Location location;
        if ((status = locate(directory.first_cluster, index, false, location)) < 0) return status;
        if ((status = read_sectors(location.sector, 1, sector)) < 0) return status;
        sector[location.offset] = kDeleted;
        if ((status = write_sectors(location.sector, 1, sector)) < 0) return status;
    }
    if (found.first_cluster) status = free_chain(found.first_cluster);
    return status;
}

int Volume::sync() {
    if (m_fsinfo_dirty && m_fsinfo_sector) {
        u8 info[kSectorSize];
        int status = read_sectors(m_fsinfo_sector, 1, info);
        if (status < 0) return status;
        put32(info + 488, m_free_count);
        put32(info + 492, m_next_free);
        if ((status = write_sectors(m_fsinfo_sector, 1, info)) < 0) return status;
    }
    m_fsinfo_dirty = false;
    return m_device->flush();
}

int Volume::count_free(u32& out) {
    u8 sector[kSectorSize];
    u64 loaded = ~0ULL;
    u32 free = 0;
    for (u32 cluster = 2; cluster < m_cluster_count + 2; cluster++) {
        const u64 offset = static_cast<u64>(cluster) * 4;
        const u64 lba = m_reserved_sectors + offset / kSectorSize;
        if (lba != loaded) {
            const int status = read_sectors(lba, 1, sector);
            if (status < 0) return status;
            loaded = lba;
        }
        if ((le32(sector + offset % kSectorSize) & kFatMask) == 0) free++;
    }
    out = free;
    return 0;
}

} // namespace fat
