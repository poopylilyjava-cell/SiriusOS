#include <exec/elf.hpp>

namespace elf {
namespace {

struct FileHeader {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 program_header_offset;
    u64 section_header_offset;
    u32 flags;
    u16 header_size;
    u16 program_header_size;
    u16 program_header_count;
    u16 section_header_size;
    u16 section_header_count;
    u16 section_name_index;
};

struct ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 virtual_address;
    u64 physical_address;
    u64 file_size;
    u64 memory_size;
    u64 alignment;
};

static_assert(sizeof(FileHeader) == 64, "ELF64 file header is 64 bytes");
static_assert(sizeof(ProgramHeader) == kProgramHeaderSize, "ELF64 program header is 56 bytes");

constexpr u8  kClass64      = 2;
constexpr u8  kLittleEndian = 1;
constexpr u16 kTypeExecutable = 2;
constexpr u16 kMachineX86_64  = 62;
constexpr u32 kPtLoad   = 1;
constexpr u32 kFlagExec  = 1;
constexpr u32 kFlagWrite = 2;

// The headers are read by copying, so a misaligned image or header offset is
// merely unusual rather than undefined behaviour.
template <typename T>
T read_at(const u8* base, u64 offset) {
    T value;
    auto* out = reinterpret_cast<u8*>(&value);
    for (usize i = 0; i < sizeof(T); i++) out[i] = base[offset + i];
    return value;
}

// [start, start + length) fits in [0, limit) without wrapping.
bool fits(u64 start, u64 length, u64 limit) {
    return start <= limit && length <= limit - start;
}

} // namespace

const char* error_name(Error error) {
    switch (error) {
    case Error::None:                        return "ok";
    case Error::TooSmall:                    return "file too small for an ELF header";
    case Error::BadMagic:                    return "not an ELF file";
    case Error::NotElf64:                    return "not ELF64";
    case Error::NotLittleEndian:             return "not little-endian";
    case Error::BadVersion:                  return "unknown ELF version";
    case Error::NotExecutable:               return "not an executable (ET_EXEC)";
    case Error::WrongMachine:                return "not x86-64";
    case Error::BadProgramHeaders:           return "program header table out of bounds";
    case Error::TooManySegments:             return "too many loadable segments";
    case Error::NoSegments:                  return "no loadable segments";
    case Error::SegmentOutsideFile:          return "segment extends past the end of the file";
    case Error::SegmentFileLargerThanMemory: return "segment file size exceeds its memory size";
    case Error::SegmentOutsideUserSpace:     return "segment outside user address space";
    case Error::SegmentsOverlap:             return "segments overlap";
    case Error::EntryNotExecutable:          return "entry point not in an executable segment";
    }
    return "unknown error";
}

Error parse(const void* data, usize size, Image& out) {
    const auto* bytes = static_cast<const u8*>(data);
    out = {};

    if (size < sizeof(FileHeader)) return Error::TooSmall;
    const auto header = read_at<FileHeader>(bytes, 0);

    if (header.ident[0] != 0x7f || header.ident[1] != 'E' ||
        header.ident[2] != 'L'  || header.ident[3] != 'F') return Error::BadMagic;
    if (header.ident[4] != kClass64)      return Error::NotElf64;
    if (header.ident[5] != kLittleEndian) return Error::NotLittleEndian;
    if (header.ident[6] != 1 || header.version != 1) return Error::BadVersion;
    if (header.type != kTypeExecutable)   return Error::NotExecutable;
    if (header.machine != kMachineX86_64) return Error::WrongMachine;

    if (header.program_header_size != sizeof(ProgramHeader)) return Error::BadProgramHeaders;
    const u64 table_bytes = static_cast<u64>(header.program_header_count) * sizeof(ProgramHeader);
    if (!fits(header.program_header_offset, table_bytes, size)) return Error::BadProgramHeaders;

    for (u32 i = 0; i < header.program_header_count; i++) {
        const auto ph = read_at<ProgramHeader>(
            bytes, header.program_header_offset + static_cast<u64>(i) * sizeof(ProgramHeader));
        if (ph.type != kPtLoad || ph.memory_size == 0) continue;

        if (out.segment_count == kMaxSegments) return Error::TooManySegments;
        if (!fits(ph.offset, ph.file_size, size)) return Error::SegmentOutsideFile;
        if (ph.file_size > ph.memory_size) return Error::SegmentFileLargerThanMemory;
        if (ph.virtual_address < kUserMin || !fits(ph.virtual_address, ph.memory_size, kUserEnd))
            return Error::SegmentOutsideUserSpace;

        // Distinct segments may share a page, but not bytes: the loader would
        // otherwise let a later segment's contents overwrite an earlier one's.
        const u64 start = ph.virtual_address;
        const u64 end = start + ph.memory_size;
        for (u32 j = 0; j < out.segment_count; j++) {
            const Segment& other = out.segments[j];
            if (start < other.virtual_address + other.memory_size && other.virtual_address < end)
                return Error::SegmentsOverlap;
        }

        out.segments[out.segment_count++] = {
            .virtual_address = ph.virtual_address,
            .memory_size     = ph.memory_size,
            .file_offset     = ph.offset,
            .file_size       = ph.file_size,
            .writable        = (ph.flags & kFlagWrite) != 0,
            .executable      = (ph.flags & kFlagExec) != 0,
        };
    }

    if (out.segment_count == 0) return Error::NoSegments;

    bool entry_ok = false;
    for (u32 i = 0; i < out.segment_count; i++) {
        const Segment& s = out.segments[i];
        if (s.executable && header.entry >= s.virtual_address &&
            header.entry - s.virtual_address < s.memory_size)
            entry_ok = true;
    }
    if (!entry_ok) return Error::EntryNotExecutable;

    // Linkers put the table inside the first segment, where a C library can
    // find its own PT_TLS and PT_GNU_RELRO entries at run time.
    for (u32 i = 0; i < out.segment_count; i++) {
        const Segment& s = out.segments[i];
        if (header.program_header_offset >= s.file_offset &&
            fits(header.program_header_offset - s.file_offset, table_bytes, s.file_size)) {
            out.program_headers = s.virtual_address + (header.program_header_offset - s.file_offset);
            out.program_header_count = header.program_header_count;
            break;
        }
    }

    out.entry = header.entry;
    return Error::None;
}

} // namespace elf
