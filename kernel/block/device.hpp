#pragma once
#include <klib/types.hpp>

// A device addressed in 512-byte sectors. Deliberately free of kernel
// dependencies: the FAT32 driver is written against this interface alone, so
// the host tests can run it on a disk image in a file.
namespace block {

constexpr u32 kSectorSize = 512;

class Device {
public:
    virtual ~Device() = default;

    // 0 or -errno. Buffers are ordinary kernel memory of count * 512 bytes.
    virtual int read(u64 lba, u32 count, void* buffer) = 0;
    virtual int write(u64 lba, u32 count, const void* buffer) = 0;
    virtual int flush() { return 0; }

    u64 sector_count() const { return m_sector_count; }
    const char* name() const { return m_name; }

protected:
    void set_identity(const char* name, u64 sectors) {
        u32 i = 0;
        for (; name[i] && i + 1 < sizeof(m_name); i++) m_name[i] = name[i];
        m_name[i] = '\0';
        m_sector_count = sectors;
    }

private:
    char m_name[24] = {};
    u64 m_sector_count = 0;
};

} // namespace block
