// Host-side tests for CRC32 and the GPT reader, against tables written by
// parted. Corruptions are applied by hand to check each rejection.

#include <cstdio>
#include <cstring>
#include <string>

#include <block/gpt.cpp>
#include "file_device.hpp"

static int g_failures = 0;

static void expect(bool condition, const char* what) {
    if (condition) return;
    printf("  FAIL  %s\n", what);
    g_failures++;
}

static void corrupt_byte(const std::string& image, long offset) {
    FILE* f = fopen(image.c_str(), "r+b");
    fseek(f, offset, SEEK_SET);
    int c = fgetc(f);
    fseek(f, offset, SEEK_SET);
    fputc(c ^ 0x5a, f);
    fclose(f);
}

int main() {
    printf("crc32:\n");
    expect(crc32("123456789", 9) == 0xcbf43926u, "the standard check value");
    expect(crc32("", 0) == 0, "the empty string");

    if (!have_tool("parted")) {
        printf("SKIP  gpt: parted not installed\n");
        return g_failures ? 1 : 0;
    }

    const std::string image = "gpt_test.img";
    auto make_image = [&]() {
        sh("rm -f " + image + " && truncate -s 16M " + image);
        sh("parted -s " + image + " mklabel gpt");
        sh("parted -s " + image + " mkpart ESP fat32 1MiB 5MiB");
        sh("parted -s " + image + " mkpart SIRIUS-ROOT fat32 5MiB 15MiB");
    };

    printf("gpt:\n");
    make_image();
    {
        FileDevice disk(image);
        gpt::Table table;
        expect(gpt::read(disk, table) == gpt::Error::None, "a parted-made table parses");
        expect(table.count == 2, "both partitions are found");
        expect(strcmp(table.partitions[0].name, "ESP") == 0, "the first partition's name");
        expect(strcmp(table.partitions[1].name, "SIRIUS-ROOT") == 0, "the second partition's name");
        expect(table.partitions[0].first_lba == 2048, "the ESP starts at 1 MiB");
        expect(table.partitions[1].first_lba == 10240, "the root starts at 5 MiB");
        expect(table.partitions[1].last_lba == 30719, "the root ends just below 15 MiB");
        expect(table.partitions[1].number == 2, "partitions keep their table index");
    }

    {
        // No table at all.
        sh("rm -f blank.img && truncate -s 1M blank.img");
        FileDevice blank("blank.img");
        gpt::Table table;
        expect(gpt::read(blank, table) == gpt::Error::NoTable, "a blank disk has no table");
        sh("rm -f blank.img");
    }

    corrupt_byte(image, 512 + 40);   // first_usable_lba, inside the header CRC
    {
        FileDevice disk(image);
        gpt::Table table;
        expect(gpt::read(disk, table) == gpt::Error::BadHeaderCrc, "a damaged header is rejected");
    }

    make_image();
    corrupt_byte(image, 1024 + 128 + 56);  // second entry's name
    {
        FileDevice disk(image);
        gpt::Table table;
        expect(gpt::read(disk, table) == gpt::Error::BadEntriesCrc, "a damaged entry array is rejected");
    }

    sh("rm -f " + image);
    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall gpt tests passed\n");
    return 0;
}
