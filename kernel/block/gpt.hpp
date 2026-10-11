#pragma once
#include <block/device.hpp>

// GUID partition table parsing. Reads through a block::Device, so it runs on
// the host against real images as well as in the kernel.
namespace gpt {

constexpr u32 kMaxPartitions = 16;

struct Partition {
    u32  number;            // 1-based, as tools like parted number them
    u64  first_lba;
    u64  last_lba;          // inclusive
    u8   type_guid[16];
    char name[37];          // the UTF-16 name, narrowed; non-ASCII becomes '?'
};

struct Table {
    u32 count;
    Partition partitions[kMaxPartitions];
};

enum class Error { None, Io, NoTable, BadHeaderCrc, BadEntriesCrc, BadGeometry };
const char* error_name(Error error);

// Read and validate the primary GPT: header CRC, entry array CRC, and every
// partition inside the disk's usable range. Empty entries are skipped.
Error read(block::Device& disk, Table& out);

} // namespace gpt
