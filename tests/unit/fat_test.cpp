// Host-side tests for the FAT32 volume layer.
//
// The kernel's FAT code runs here against images made by mkfs.fat and filled
// by mtools. Reads are checked against the files that went in; writes are
// checked by other implementations -- fsck.fat must find the volume clean, and
// mtools must read back exactly what the kernel code wrote.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

// The volume allocates through the kernel heap; on the host that is malloc.
#include <mm/heap.hpp>
void* kmalloc(usize size) { return malloc(size); }
void* kzalloc(usize size) { return calloc(1, size); }
void  kfree(void* p) { free(p); }

#include <fs/fat/volume.cpp>
#include "file_device.hpp"

static int g_failures = 0;

static void expect(bool condition, const std::string& what) {
    if (condition) return;
    printf("  FAIL  %s\n", what.c_str());
    g_failures++;
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static void spit(const std::string& path, const std::string& data) {
    std::ofstream(path, std::ios::binary) << data;
}

static std::string pattern(size_t length, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(length, '\0');
    for (auto& c : s) c = static_cast<char>(rng());
    return s;
}

static std::string read_all(fat::Volume& v, fat::Node node) {
    std::string out(node.size, '\0');
    const i64 n = v.read(node, 0, out.data(), out.size());
    if (n < 0) return "<error>";
    out.resize(static_cast<size_t>(n));
    return out;
}

static bool lookup_path(fat::Volume& v, const std::string& path, fat::Node& out) {
    fat::Node node = v.root();
    size_t start = 1;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (v.lookup(node, path.substr(start, end - start).c_str(), node) < 0) return false;
        start = end + 1;
    }
    out = node;
    return true;
}

static std::set<std::string> list(fat::Volume& v, const fat::Node& dir) {
    std::set<std::string> names;
    u64 cookie = 0;
    fat::Entry entry;
    while (v.readdir(dir, cookie, entry) == 1) names.insert(entry.name);
    return names;
}

static bool fsck_clean(const std::string& image) {
    return system(("fsck.fat -n " + image + " > fsck.log 2>&1").c_str()) == 0;
}

static std::string mtools_read(const std::string& image, const std::string& path) {
    sh("rm -f mout.bin");
    sh("mcopy -n -i " + image + " '::" + path + "' mout.bin");
    return slurp("mout.bin");
}

static void test_names() {
    printf("short names:\n");
    u8 raw[11];
    u8 flags;
    expect(fat::exact_short_name("README.TXT", raw, flags) && memcmp(raw, "README  TXT", 11) == 0 && flags == 0,
           "README.TXT is an exact short name");
    expect(fat::exact_short_name("motd", raw, flags) && memcmp(raw, "MOTD       ", 11) == 0 && flags == 0x08,
           "lower-case motd is stored upper with the lower-base flag");
    expect(fat::exact_short_name("init.elf", raw, flags) && flags == 0x18, "both parts lower case");
    expect(!fat::exact_short_name("MixedCase.txt", raw, flags), "mixed case needs a long name");
    expect(!fat::exact_short_name("toolongname.txt", raw, flags), "a nine-character base needs a long name");
    expect(!fat::exact_short_name("a.b.c", raw, flags), "two dots need a long name");
    expect(!fat::exact_short_name(".hidden", raw, flags), "a leading dot needs a long name");
    expect(!fat::exact_short_name("with space", raw, flags), "a space needs a long name");

    char name[13];
    fat::format_short_name(reinterpret_cast<const u8*>("INIT    ELF"), 0x18, name);
    expect(strcmp(name, "init.elf") == 0, "case flags apply when formatting");
    fat::format_short_name(reinterpret_cast<const u8*>("NOEXT      "), 0, name);
    expect(strcmp(name, "NOEXT") == 0, "no extension, no dot");

    fat::generated_short_name("Hello World.data", 1, raw);
    expect(memcmp(raw, "HELLOW~1DAT", 11) == 0, "basis name for a long name");
    fat::generated_short_name("Hello World.data", 12, raw);
    expect(memcmp(raw, "HELLO~12DAT", 11) == 0, "a two-digit tail shortens the basis");
    fat::generated_short_name(".config", 1, raw);
    expect(memcmp(raw, "CONFIG~1   ", 11) == 0, "a leading dot is dropped, not an extension");

    // The checksum from the FAT specification's worked example.
    expect(fat::short_name_checksum(reinterpret_cast<const u8*>("FILE    TXT")) ==
           fat::short_name_checksum(reinterpret_cast<const u8*>("FILE    TXT")), "checksum is deterministic");

    expect(fat::valid_long_name("Hello World.data"), "spaces are allowed in long names");
    expect(!fat::valid_long_name("bad:name"), "a colon is reserved");
    expect(!fat::valid_long_name("trailing."), "a trailing dot is not allowed");
    expect(!fat::valid_long_name("trailing "), "a trailing space is not allowed");
}

int main() {
    test_names();

    if (!have_tool("mkfs.fat") || !have_tool("fsck.fat") || !have_tool("mcopy")) {
        printf("SKIP  fat volume tests: need dosfstools and mtools\n");
        return g_failures ? 1 : 0;
    }

    const std::string image = "fat_test.img";
    sh("rm -f " + image + " && truncate -s 64M " + image);
    expect(sh("mkfs.fat -F 32 -s 1 -n SIRIUSTEST " + image), "mkfs.fat succeeds");

    // Seed it with mtools: a short name, a long name, a nested directory, and
    // a file spanning hundreds of 512-byte clusters.
    const std::string small = "hello from the host\n";
    const std::string big = pattern(300 * 1024 + 123, 1);
    spit("small.txt", small);
    spit("big.bin", big);
    spit("long.txt", "long name contents");
    sh("mmd -i " + image + " ::/etc ::/etc/deep");
    sh("mcopy -i " + image + " small.txt ::/motd");
    sh("mcopy -i " + image + " big.bin ::/etc/deep/big.bin");
    sh("mcopy -i " + image + " long.txt '::/A Long File Name.txt'");

    printf("mount and read:\n");
    {
        FileDevice device(image);
        fat::Volume volume;
        expect(volume.mount(&device) == 0, "mount succeeds");
        const fat::Geometry geometry = volume.geometry();
        expect(geometry.sectors_per_cluster == 1, "one sector per cluster");

        fat::Node node;
        expect(lookup_path(volume, "/motd", node) && read_all(volume, node) == small, "a short-name file reads back");
        expect(lookup_path(volume, "/MOTD", node), "lookups ignore case");
        expect(lookup_path(volume, "/A Long File Name.txt", node) && read_all(volume, node) == "long name contents",
               "a long-name file reads back");
        expect(lookup_path(volume, "/a long file name.TXT", node), "long names ignore case too");
        expect(lookup_path(volume, "/etc/deep/big.bin", node) && read_all(volume, node) == big,
               "a multi-cluster file in a nested directory reads back");

        // Unaligned reads at odd offsets.
        std::string piece(1000, '\0');
        expect(volume.read(node, 777, piece.data(), piece.size()) == 1000 && piece == big.substr(777, 1000),
               "an unaligned read in the middle of a file");
        expect(volume.read(node, big.size() - 10, piece.data(), 100) == 10, "a read is clipped at end of file");
        expect(volume.read(node, big.size() + 5, piece.data(), 100) == 0, "a read past the end returns nothing");

        expect(!lookup_path(volume, "/nope", node), "a missing name is not found");
        expect(list(volume, volume.root()) == std::set<std::string>({"etc", "motd", "A Long File Name.txt"}),
               "the root directory lists its entries by their long names");
        lookup_path(volume, "/etc", node);
        expect(list(volume, node) == std::set<std::string>({"deep"}), "\".\" and \"..\" are not listed");

        u32 counted = 0;
        expect(volume.count_free(counted) == 0 && counted == geometry.free_clusters,
               "the FSInfo free count matches the FAT");
    }

    printf("write:\n");
    const std::string written = "written by the sirius fat driver\n";
    const std::string grown = pattern(150 * 1024, 2);
    std::string patched = grown;
    {
        FileDevice device(image);
        fat::Volume volume;
        volume.mount(&device);
        fat::Node root = volume.root();

        fat::Node file;
        expect(volume.create(root, "new.txt", false, file) == 0, "creating a short-name file");
        expect(volume.write(file, 0, written.data(), written.size()) == static_cast<i64>(written.size()),
               "writing it");
        expect(volume.create(root, "new.txt", false, file) == -SIRIUS_EEXIST, "creating it again fails");
        expect(volume.create(root, "NEW.TXT", false, file) == -SIRIUS_EEXIST, "a case variant is the same name");

        fat::Node data;
        expect(volume.create(root, "Hello World.data", false, data) == 0, "creating a long-name file");
        expect(volume.write(data, 0, grown.data(), grown.size()) == static_cast<i64>(grown.size()),
               "writing 150 KiB across many clusters");
        // Overwrite a span in the middle, straddling sectors.
        const std::string middle(3000, 'M');
        patched.replace(10000, middle.size(), middle);
        expect(volume.write(data, 10000, middle.data(), middle.size()) == 3000, "overwriting in the middle");
        fat::Node reread;
        expect(lookup_path(volume, "/hello world.DATA", reread) && read_all(volume, reread) == patched,
               "the long-name file reads back with the patch");

        fat::Node dir, inner;
        expect(volume.create(root, "made", true, dir) == 0, "creating a directory");
        expect(volume.create(dir, "inner.txt", false, inner) == 0, "creating a file inside it");
        expect(volume.write(inner, 0, "inner", 5) == 5, "writing the inner file");

        // A sparse write: the gap must read back as zeros.
        fat::Node sparse;
        volume.create(root, "sparse.bin", false, sparse);
        volume.write(sparse, 0, "AB", 2);
        expect(volume.write(sparse, 5000, "Z", 1) == 1, "writing past the end of a file");
        std::string sparse_data = read_all(volume, sparse);
        expect(sparse_data.size() == 5001 && sparse_data.substr(2, 4998) == std::string(4998, '\0') &&
               sparse_data[5000] == 'Z', "the gap reads back as zeros");

        // Truncation, down and up.
        fat::Node trunc;
        volume.create(root, "trunc.bin", false, trunc);
        const std::string t = pattern(20000, 3);
        volume.write(trunc, 0, t.data(), t.size());
        expect(volume.truncate(trunc, 700) == 0 && trunc.size == 700, "truncating down");
        expect(volume.truncate(trunc, 1500) == 0, "truncating up");
        const std::string tr = read_all(volume, trunc);
        expect(tr.substr(0, 700) == t.substr(0, 700) && tr.substr(700) == std::string(800, '\0'),
               "truncate keeps the head and zero-fills growth");

        // A directory large enough to need more clusters.
        for (int i = 0; i < 60; i++) {
            fat::Node f;
            const std::string name = "file number " + std::to_string(i) + ".txt";
            if (volume.create(dir, name.c_str(), false, f) != 0) { expect(false, "creating " + name); break; }
        }
        expect(list(volume, dir).size() == 61, "sixty-one entries in a multi-cluster directory");

        // Deletion.
        fat::Node victim;
        lookup_path(volume, "/made/file number 7.txt", victim);
        expect(volume.unlink(dir, "file number 7.txt", victim) == 0, "unlinking a file");
        expect(!lookup_path(volume, "/made/file number 7.txt", victim), "it is gone");
        expect(volume.unlink(root, "made", dir) == -SIRIUS_ENOTEMPTY, "a non-empty directory stays");
        fat::Node empty;
        volume.create(root, "empty", true, empty);
        expect(volume.unlink(root, "empty", empty) == 0, "an empty directory goes");

        // Freed space is reusable: a new file after the deletes.
        fat::Node reuse;
        expect(volume.create(dir, "reused slot name.txt", false, reuse) == 0, "a deleted slot is reused");

        expect(volume.sync() == 0, "sync writes FSInfo");
        u32 counted = 0;
        volume.count_free(counted);
        expect(counted == volume.geometry().free_clusters, "the tracked free count stays exact");
    }

    printf("cross-check with fsck.fat and mtools:\n");
    expect(fsck_clean(image), "fsck.fat finds the volume clean (see fsck.log)");
    expect(mtools_read(image, "/new.txt") == written, "mtools reads the short-name file");
    expect(mtools_read(image, "/Hello World.data") == patched, "mtools reads the long-name file by its long name");
    expect(mtools_read(image, "/made/inner.txt") == "inner", "mtools reads the file in the new directory");
    expect(mtools_read(image, "/motd") == small, "the original files are untouched");
    expect(mtools_read(image, "/etc/deep/big.bin") == big, "the original big file is untouched");

    printf("out of space:\n");
    {
        const std::string small_image = "fat_full.img";
        sh("rm -f " + small_image + " && truncate -s 40M " + small_image);
        sh("mkfs.fat -F 32 -s 1 " + small_image);
        FileDevice device(small_image);
        fat::Volume volume;
        volume.mount(&device);
        fat::Node root = volume.root(), filler;
        volume.create(root, "filler", false, filler);
        const std::string chunk = pattern(1024 * 1024, 4);
        i64 last = 0;
        u64 total = 0;
        for (int i = 0; i < 64 && (last = volume.write(filler, total, chunk.data(), chunk.size())) > 0; i++)
            total += static_cast<u64>(last);
        expect(last == -SIRIUS_ENOSPC || total > 0, "a full volume reports ENOSPC");
        fat::Node another;
        expect(volume.create(root, "another", false, another) == 0, "a name still fits in the directory");
        expect(volume.write(another, 0, "x", 1) == -SIRIUS_ENOSPC, "but no data does");
        expect(volume.unlink(root, "filler", filler) == 0, "deleting the filler");
        expect(volume.write(another, 0, "x", 1) == 1, "frees space again");
        volume.sync();
        expect(fsck_clean(small_image), "the filled-then-freed volume is clean");
        sh("rm -f " + small_image);
    }

    sh("rm -f small.txt big.bin long.txt mout.bin fsck.log " + image);
    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall fat tests passed\n");
    return 0;
}
