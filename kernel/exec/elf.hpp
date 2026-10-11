#pragma once
#include <klib/types.hpp>

// ELF64 executable validation.
//
// Pure computation over an in-memory image: no allocation, no page tables. The
// loader in sched/process.cpp only ever sees an Image that passed parse(), so
// every bounds check on untrusted input lives here, where the host tests can
// reach it with crafted files.
namespace elf {

constexpr u32 kMaxSegments = 8;

// Lowest address a segment may occupy: the first page stays unmapped so a
// null pointer dereference in user code faults.
constexpr u64 kUserMin = 0x1000;
// One past the highest user address: the top of the canonical lower half.
constexpr u64 kUserEnd = 0x0000800000000000ULL;

struct Segment {
    u64 virtual_address;
    u64 memory_size;
    u64 file_offset;
    u64 file_size;
    bool writable;
    bool executable;
};

struct Image {
    u64 entry;
    u32 segment_count;
    Segment segments[kMaxSegments];
    // Where the program header table sits once loaded, for the auxiliary
    // vector's AT_PHDR: 0 if no loadable segment carries it.
    u64 program_headers;
    u16 program_header_count;
};

constexpr u64 kProgramHeaderSize = 56;

enum class Error {
    None,
    TooSmall,
    BadMagic,
    NotElf64,
    NotLittleEndian,
    BadVersion,
    NotExecutable,
    WrongMachine,
    BadProgramHeaders,
    TooManySegments,
    NoSegments,
    SegmentOutsideFile,
    SegmentFileLargerThanMemory,
    SegmentOutsideUserSpace,
    SegmentsOverlap,
    EntryNotExecutable,
};

const char* error_name(Error error);

// Validate `data` as a static x86-64 executable for user space and describe
// its loadable segments. `out` is only meaningful when this returns None.
Error parse(const void* data, usize size, Image& out);

} // namespace elf
