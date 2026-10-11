// A block::Device backed by a disk image file, for host tests that run the
// kernel's storage code against images made by real tools.
#pragma once
#include <block/device.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

class FileDevice final : public block::Device {
public:
    explicit FileDevice(const std::string& path, u64 offset_bytes = 0, u64 length_bytes = 0)
        : m_offset(offset_bytes) {
        m_fd = ::open(path.c_str(), O_RDWR);
        if (m_fd < 0) { perror(path.c_str()); exit(2); }
        struct stat st;
        fstat(m_fd, &st);
        const u64 bytes = length_bytes ? length_bytes : static_cast<u64>(st.st_size) - offset_bytes;
        set_identity("file", bytes / block::kSectorSize);
    }
    ~FileDevice() override { ::close(m_fd); }

    int read(u64 lba, u32 count, void* buffer) override {
        if (lba + count > sector_count()) return -EIO;
        reads++;
        const ssize_t n = pread(m_fd, buffer, count * block::kSectorSize, m_offset + lba * block::kSectorSize);
        return n == static_cast<ssize_t>(count * block::kSectorSize) ? 0 : -EIO;
    }
    int write(u64 lba, u32 count, const void* buffer) override {
        if (lba + count > sector_count()) return -EIO;
        writes++;
        const ssize_t n = pwrite(m_fd, buffer, count * block::kSectorSize, m_offset + lba * block::kSectorSize);
        return n == static_cast<ssize_t>(count * block::kSectorSize) ? 0 : -EIO;
    }

    u64 reads = 0, writes = 0;

private:
    int m_fd;
    u64 m_offset;
};

// Run a shell command; true if it succeeded.
inline bool sh(const std::string& command) { return system((command + " >/dev/null 2>&1").c_str()) == 0; }

// True if a host tool is on PATH.
inline bool have_tool(const char* tool) { return sh(std::string("command -v ") + tool); }
