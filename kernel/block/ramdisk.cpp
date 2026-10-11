#include <block/ramdisk.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace block {

RamDisk::RamDisk(const char* name, void* data, u64 bytes) : m_data(static_cast<u8*>(data)) {
    set_identity(name, bytes / kSectorSize);
}

int RamDisk::read(u64 lba, u32 count, void* buffer) {
    if (lba > sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
    memcpy(buffer, m_data + lba * kSectorSize, static_cast<usize>(count) * kSectorSize);
    return 0;
}

int RamDisk::write(u64 lba, u32 count, const void* buffer) {
    if (lba > sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
    memcpy(m_data + lba * kSectorSize, buffer, static_cast<usize>(count) * kSectorSize);
    return 0;
}

} // namespace block
