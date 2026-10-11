#include <block/block.hpp>
#include <block/gpt.hpp>
#include <sched/mutex.hpp>
#include <mm/heap.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace block {
namespace {

constexpr u32 kMaxDevices = 32;
constexpr u32 kCacheSectors = 1024;      // 512 KiB per disk
constexpr u32 kCacheMaxRequest = 8;      // larger transfers bypass the cache

// A write-through, direct-mapped sector cache. Write-through keeps the disk
// always current, so a request that bypasses the cache is never stale and a
// crash loses nothing that a write() returned for.
class CachedDisk final : public Device {
public:
    explicit CachedDisk(Device* disk) : m_disk(disk) {
        set_identity(disk->name(), disk->sector_count());
        m_lbas = static_cast<u64*>(kmalloc(kCacheSectors * sizeof(u64)));
        m_data = static_cast<u8*>(kmalloc(static_cast<u64>(kCacheSectors) * kSectorSize));
        if (m_lbas == nullptr || m_data == nullptr) {
            LOG_WARN("block: %s: no memory for a cache, running uncached", name());
            kfree(m_lbas); kfree(m_data);
            m_lbas = nullptr; m_data = nullptr;
            return;
        }
        for (u32 i = 0; i < kCacheSectors; i++) m_lbas[i] = kEmpty;
    }

    int read(u64 lba, u32 count, void* buffer) override {
        if (m_data == nullptr || count > kCacheMaxRequest) return m_disk->read(lba, count, buffer);

        sched::ScopedMutex guard(m_lock);
        auto* out = static_cast<u8*>(buffer);
        for (u32 i = 0; i < count; i++) {
            const u64 sector = lba + i;
            const u32 slot = static_cast<u32>(sector % kCacheSectors);
            u8* data = m_data + static_cast<u64>(slot) * kSectorSize;
            if (m_lbas[slot] != sector) {
                const int status = m_disk->read(sector, 1, data);
                if (status < 0) { m_lbas[slot] = kEmpty; return status; }
                m_lbas[slot] = sector;
                m_misses++;
            } else {
                m_hits++;
            }
            memcpy(out + static_cast<u64>(i) * kSectorSize, data, kSectorSize);
        }
        return 0;
    }

    int write(u64 lba, u32 count, const void* buffer) override {
        sched::ScopedMutex guard(m_lock);
        const int status = m_disk->write(lba, count, buffer);
        if (m_data == nullptr) return status;

        const auto* in = static_cast<const u8*>(buffer);
        for (u32 i = 0; i < count; i++) {
            const u64 sector = lba + i;
            const u32 slot = static_cast<u32>(sector % kCacheSectors);
            if (status < 0) {
                // The disk's contents are now unknown: forget, don't guess.
                if (m_lbas[slot] == sector) m_lbas[slot] = kEmpty;
                continue;
            }
            // Fill on write too: FAT sectors are read straight back.
            memcpy(m_data + static_cast<u64>(slot) * kSectorSize, in + static_cast<u64>(i) * kSectorSize, kSectorSize);
            m_lbas[slot] = sector;
        }
        return status;
    }

    int flush() override { return m_disk->flush(); }

private:
    static constexpr u64 kEmpty = ~0ULL;
    Device* m_disk;
    sched::Mutex m_lock;
    u64* m_lbas = nullptr;
    u8*  m_data = nullptr;
    u64  m_hits = 0, m_misses = 0;
};

class Partition final : public Device {
public:
    Partition(Device* disk, const gpt::Partition& entry) : m_disk(disk), m_start(entry.first_lba) {
        char name[24];
        ksnprintf(name, sizeof(name), "%sp%u", disk->name(), entry.number);
        set_identity(name, entry.last_lba - entry.first_lba + 1);
        str_copy(m_label, sizeof(m_label), entry.name);
    }

    int read(u64 lba, u32 count, void* buffer) override {
        if (lba >= sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
        return m_disk->read(m_start + lba, count, buffer);
    }
    int write(u64 lba, u32 count, const void* buffer) override {
        if (lba >= sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
        return m_disk->write(m_start + lba, count, buffer);
    }
    int flush() override { return m_disk->flush(); }

    const char* label() const { return m_label; }

private:
    Device* m_disk;
    u64 m_start;
    char m_label[37];
};

struct Registered {
    Device* device;
    const char* label;   // GPT name, partitions only
};

Registered g_devices[kMaxDevices];
u32 g_count = 0;
sched::Mutex g_lock;

void register_device(Device* device, const char* label) {
    sched::ScopedMutex guard(g_lock);
    if (g_count == kMaxDevices) { LOG_WARN("block: registry full, %s ignored", device->name()); return; }
    g_devices[g_count++] = {device, label};
}

} // namespace

void add_disk(Device* raw) {
    Device* disk = new CachedDisk(raw);
    register_device(disk, nullptr);
    LOG_INFO("block: %s: %lu MiB", disk->name(), disk->sector_count() * kSectorSize / (1024 * 1024));

    auto* table = static_cast<gpt::Table*>(kmalloc(sizeof(gpt::Table)));
    if (table == nullptr) return;
    const gpt::Error error = gpt::read(*disk, *table);
    if (error != gpt::Error::None) {
        LOG_INFO("block: %s: no partitions (%s)", disk->name(), gpt::error_name(error));
        kfree(table);
        return;
    }

    for (u32 i = 0; i < table->count; i++) {
        auto* partition = new Partition(disk, table->partitions[i]);
        register_device(partition, partition->label());
        LOG_INFO("block: %s: \"%s\", %lu MiB at sector %lu", partition->name(), partition->label(),
                 partition->sector_count() * kSectorSize / (1024 * 1024), table->partitions[i].first_lba);
    }
    kfree(table);
}

u32 count() { return g_count; }
Device* at(u32 index) { return index < g_count ? g_devices[index].device : nullptr; }

Device* find(const char* name) {
    for (u32 i = 0; i < g_count; i++)
        if (strcmp(g_devices[i].device->name(), name) == 0) return g_devices[i].device;
    return nullptr;
}

Device* find_by_label(const char* label) {
    for (u32 i = 0; i < g_count; i++)
        if (g_devices[i].label && strcmp(g_devices[i].label, label) == 0) return g_devices[i].device;
    return nullptr;
}

} // namespace block
