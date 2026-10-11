#pragma once
#include <block/device.hpp>

namespace block {

// A disk in memory: a filesystem image the bootloader loaded, as the root of
// a system booted from a USB stick (no USB storage driver needed: the
// firmware read it). Writes change the memory, and are gone at the next boot.
class RamDisk final : public Device {
public:
    RamDisk(const char* name, void* data, u64 bytes);
    int read(u64 lba, u32 count, void* buffer) override;
    int write(u64 lba, u32 count, const void* buffer) override;

private:
    u8* m_data;
};

} // namespace block
