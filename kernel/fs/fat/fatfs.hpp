#pragma once
#include <fs/vfs.hpp>
#include <block/device.hpp>

namespace fatfs {

// Mount the FAT32 filesystem on `device`. Null, with `error` set to -errno, if
// the device holds no usable FAT32 volume.
vfs::Filesystem* mount(block::Device* device, int& error);

} // namespace fatfs
