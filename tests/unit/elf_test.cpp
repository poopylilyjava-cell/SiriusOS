// Host-side tests for the ELF64 validator. Every executable the kernel loads
// passes through elf::parse(), so each rejection path is exercised here with a
// deliberately broken image -- under asan, which catches any read it lets
// slip past the end of the buffer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <exec/elf.cpp>

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

namespace {

struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };

// A small but well-formed executable: a text segment holding the entry point
// and a data segment with a .bss tail.
struct Builder {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x3000, 0);
    std::vector<Phdr> phdrs;
    uint64_t entry = 0x401000;
    uint16_t type = 2, machine = 62;

    Builder() {
        phdrs.push_back({1, 5, 0x1000, 0x401000, 0x401000, 0x100, 0x100, 0x1000});   // r-x
        phdrs.push_back({1, 6, 0x2000, 0x402000, 0x402000, 0x80, 0x2000, 0x1000});   // rw-, bss
    }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out = bytes;
        uint8_t* h = out.data();
        const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        memcpy(h, ident, sizeof(ident));
        memcpy(h + 16, &type, 2);
        memcpy(h + 18, &machine, 2);
        const uint32_t version = 1;   memcpy(h + 20, &version, 4);
        memcpy(h + 24, &entry, 8);
        const uint64_t phoff = 64;    memcpy(h + 32, &phoff, 8);
        const uint16_t ehsize = 64;   memcpy(h + 52, &ehsize, 2);
        const uint16_t phentsize = 56; memcpy(h + 54, &phentsize, 2);
        const uint16_t phnum = static_cast<uint16_t>(phdrs.size()); memcpy(h + 56, &phnum, 2);
        for (size_t i = 0; i < phdrs.size(); i++) memcpy(h + 64 + i * 56, &phdrs[i], 56);
        return out;
    }
};

elf::Error parse(const std::vector<uint8_t>& image, elf::Image& out) {
    // A heap copy of exactly the right size, so asan sees any overread.
    auto* copy = static_cast<uint8_t*>(malloc(image.size() ? image.size() : 1));
    memcpy(copy, image.data(), image.size());
    const elf::Error error = elf::parse(copy, image.size(), out);
    free(copy);
    return error;
}

elf::Error parse(const Builder& b) {
    elf::Image image;
    return parse(b.build(), image);
}

} // namespace

int main() {
    printf("valid executable:\n");
    {
        elf::Image image;
        const elf::Error error = parse(Builder().build(), image);
        expect(error == elf::Error::None, "a well-formed executable parses");
        expect(image.entry == 0x401000, "the entry point is reported");
        expect(image.segment_count == 2, "both loadable segments are reported");
        expect(image.program_headers == 0, "program headers outside every segment are not reported");
        expect(image.segments[0].executable && !image.segments[0].writable, "text is r-x");
        expect(image.segments[1].writable && !image.segments[1].executable, "data is rw-");
        expect(image.segments[1].memory_size == 0x2000 && image.segments[1].file_size == 0x80,
               "a .bss tail keeps its memory size");
    }

    printf("program headers in memory:\n");
    {
        // What a real linker emits: the first segment starts at file offset 0
        // and so carries the ELF header and the program header table.
        Builder b;
        b.phdrs.insert(b.phdrs.begin(), {1, 4, 0, 0x400000, 0x400000, 0x200, 0x200, 0x1000});   // r--
        elf::Image image;
        expect(parse(b.build(), image) == elf::Error::None, "an executable with a header segment parses");
        expect(image.program_headers == 0x400040, "AT_PHDR is the table's address inside that segment");
        expect(image.program_header_count == 3, "AT_PHNUM counts every program header");

        // A segment that covers only part of the table does not count.
        b.phdrs[0].filesz = b.phdrs[0].memsz = 64 + 56;
        expect(parse(b.build(), image) == elf::Error::None, "a short header segment still parses");
        expect(image.program_headers == 0, "a partly loaded table is not reported");
    }

    printf("header rejections:\n");
    {
        elf::Image image;
        std::vector<uint8_t> tiny(63, 0);
        expect(parse(tiny, image) == elf::Error::TooSmall, "a file shorter than the header is rejected");

        auto bad_magic = Builder().build(); bad_magic[1] = 'X';
        expect(parse(bad_magic, image) == elf::Error::BadMagic, "bad magic is rejected");

        auto elf32 = Builder().build(); elf32[4] = 1;
        expect(parse(elf32, image) == elf::Error::NotElf64, "ELF32 is rejected");

        auto big_endian = Builder().build(); big_endian[5] = 2;
        expect(parse(big_endian, image) == elf::Error::NotLittleEndian, "big-endian is rejected");

        Builder shared; shared.type = 3;
        expect(parse(shared) == elf::Error::NotExecutable, "a shared object is rejected");

        Builder arm; arm.machine = 183;
        expect(parse(arm) == elf::Error::WrongMachine, "a non-x86-64 machine is rejected");

        // A program header table that runs off the end of the file.
        auto truncated = Builder().build();
        truncated.resize(64 + 56);
        expect(parse(truncated, image) == elf::Error::BadProgramHeaders,
               "a truncated program header table is rejected");

        auto huge_phoff = Builder().build();
        const uint64_t phoff = ~0ULL - 8; memcpy(huge_phoff.data() + 32, &phoff, 8);
        expect(parse(huge_phoff, image) == elf::Error::BadProgramHeaders,
               "a program header offset that wraps is rejected");
    }

    printf("segment rejections:\n");
    {
        Builder past_end; past_end.phdrs[0].filesz = 0x10000;
        expect(parse(past_end) == elf::Error::SegmentOutsideFile, "file contents past EOF are rejected");

        Builder wrap; wrap.phdrs[0].offset = ~0ULL - 0x10;
        expect(parse(wrap) == elf::Error::SegmentOutsideFile, "a wrapping file offset is rejected");

        Builder file_gt_mem; file_gt_mem.phdrs[1].memsz = 0x40;
        expect(parse(file_gt_mem) == elf::Error::SegmentFileLargerThanMemory,
               "file size above memory size is rejected");

        Builder null_page; null_page.phdrs[0].vaddr = 0; null_page.entry = 0;
        expect(parse(null_page) == elf::Error::SegmentOutsideUserSpace, "a segment at address 0 is rejected");

        Builder kernel; kernel.phdrs[0].vaddr = 0xffffffff80000000ULL; kernel.entry = kernel.phdrs[0].vaddr;
        expect(parse(kernel) == elf::Error::SegmentOutsideUserSpace, "a kernel-half segment is rejected");

        Builder straddle; straddle.phdrs[1].vaddr = 0x00007ffffffff000ULL; straddle.phdrs[1].memsz = 0x2000;
        expect(parse(straddle) == elf::Error::SegmentOutsideUserSpace,
               "a segment crossing the top of user space is rejected");

        Builder overlap; overlap.phdrs[1].vaddr = 0x401080;
        expect(parse(overlap) == elf::Error::SegmentsOverlap, "overlapping segments are rejected");

        Builder same_page; same_page.phdrs[1].vaddr = 0x401100; same_page.phdrs[1].memsz = 0x80;
        expect(parse(same_page) == elf::Error::None, "segments sharing a page but not bytes are accepted");

        Builder none; none.phdrs.clear();
        expect(parse(none) == elf::Error::NoSegments, "an image with nothing to load is rejected");

        Builder many;
        for (int i = 0; i < 9; i++) many.phdrs.push_back({1, 4, 0, 0x500000ULL + i * 0x1000ULL, 0, 0, 0x10, 0x1000});
        expect(parse(many) == elf::Error::TooManySegments, "more segments than the loader holds are rejected");

        Builder data_entry; data_entry.entry = 0x402000;
        expect(parse(data_entry) == elf::Error::EntryNotExecutable,
               "an entry point in a non-executable segment is rejected");

        Builder outside_entry; outside_entry.entry = 0x900000;
        expect(parse(outside_entry) == elf::Error::EntryNotExecutable,
               "an entry point outside every segment is rejected");
    }

    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall elf tests passed\n");
    return 0;
}
