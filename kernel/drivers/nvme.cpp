// NVMe 1.4, polled: one admin queue pair and one I/O queue pair per
// controller, a command at a time, through a bounce buffer -- like the AHCI
// driver, which this mirrors. Interrupts are left off: a polled command on
// flash completes in tens of microseconds.
#include <drivers/nvme.hpp>
#include <drivers/pci.hpp>
#include <block/block.hpp>
#include <arch/x86_64/clock.hpp>
#include <mm/memory.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <sched/mutex.hpp>
#include <sched/sched.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace nvme {
namespace {

constexpr u32 kQueueEntries = 64;          // per queue: well within one page either way
constexpr u8  kBounceOrder = 1;            // two pages: PRP1 and PRP2, no PRP list needed
constexpr u64 kBounceBytes = mm::kPageSize << kBounceOrder;

// Registers.
constexpr u32 kCap = 0x00, kConfig = 0x14, kStatus = 0x1c, kAdminAttributes = 0x24, kAdminSq = 0x28, kAdminCq = 0x30;

struct Command {           // a submission queue entry
    u32 dword0;            // opcode, command id
    u32 nsid;
    u64 reserved;
    u64 metadata;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(Command) == 64);

struct Completion {
    u32 result;
    u32 reserved;
    u16 sq_head, sq_id;
    u16 command_id;
    u16 status;            // bit 0: phase; the rest: status code
};
static_assert(sizeof(Completion) == 16);

struct Queue {
    u64 sq = 0, cq = 0;    // physical
    u32 tail = 0, head = 0;
    bool phase = true;
    u16 next_id = 0;
    volatile u32* sq_doorbell = nullptr;
    volatile u32* cq_doorbell = nullptr;
};

class Controller {
public:
    bool start(const pci::Device& device);
    // Run a command to completion: its status code (0 is success), or -1
    // if the controller never answered.
    int run(Queue& queue, Command command, u32* result = nullptr);

    u8* mmio = nullptr;
    u32 stride = 4;
    u64 timeout_ms = 500;
    Queue admin, io;

    volatile u32& reg32(u32 offset) { return *reinterpret_cast<volatile u32*>(mmio + offset); }
    u64 reg64(u32 offset) { return reg32(offset) | (static_cast<u64>(reg32(offset + 4)) << 32); }
    void write64(u32 offset, u64 value) { reg32(offset) = static_cast<u32>(value); reg32(offset + 4) = static_cast<u32>(value >> 32); }
};

class Namespace final : public block::Device {
public:
    Namespace(Controller* controller, u32 nsid, u64 blocks, u32 block_size, u64 bounce, u32 index);
    int read(u64 lba, u32 count, void* buffer) override;
    int write(u64 lba, u32 count, const void* buffer) override;
    int flush() override;

private:
    // Whole device blocks, through the bounce buffer: `blocks` of them at `first`.
    int io(bool write, u64 first, u32 blocks);
    int transfer(bool write, u64 lba, u32 count, u8* buffer);

    Controller* m_controller;
    u32 m_nsid;
    u32 m_block_size;      // 512 or 4096: the device's; this presents 512-byte sectors either way
    u64 m_bounce;          // physical
    sched::Mutex m_lock;
};

bool Controller::start(const pci::Device& device) {
    const pci::Bar bar = pci::bar(device, 0);
    if (bar.io || bar.address == 0) return false;
    pci::enable_dma(device);
    const u64 mapped = vmm::map_mmio(bar.address, bar.size ? bar.size : 0x4000);
    if (mapped == 0) return false;
    mmio = reinterpret_cast<u8*>(mapped);

    const u64 cap = reg64(kCap);
    stride = 4u << ((cap >> 32) & 0xf);
    timeout_ms = ((cap >> 24) & 0xff) * 500 + 500;
    const u32 max_entries = static_cast<u32>(cap & 0xffff) + 1;
    const u32 entries = max_entries < kQueueEntries ? max_entries : kQueueEntries;

    // Disable, and wait for it to say so.
    reg32(kConfig) = reg32(kConfig) & ~1u;
    const u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    while ((reg32(kStatus) & 1) && clock::now_ns() < deadline) sched::sleep_ms(1);
    if (reg32(kStatus) & 1) return false;

    admin.sq = pmm::alloc_page_zeroed();
    admin.cq = pmm::alloc_page_zeroed();
    io.sq = pmm::alloc_page_zeroed();
    io.cq = pmm::alloc_page_zeroed();
    if (!admin.sq || !admin.cq || !io.sq || !io.cq) return false;
    reg32(kAdminAttributes) = ((entries - 1) << 16) | (entries - 1);
    write64(kAdminSq, admin.sq);
    write64(kAdminCq, admin.cq);
    admin.sq_doorbell = reinterpret_cast<volatile u32*>(mmio + 0x1000);
    admin.cq_doorbell = reinterpret_cast<volatile u32*>(mmio + 0x1000 + stride);
    io.sq_doorbell = reinterpret_cast<volatile u32*>(mmio + 0x1000 + 2 * stride);
    io.cq_doorbell = reinterpret_cast<volatile u32*>(mmio + 0x1000 + 3 * stride);

    // Enabled: 4 KiB pages, 64-byte submissions, 16-byte completions.
    reg32(kConfig) = 1u | (6u << 16) | (4u << 20);
    const u64 ready = clock::now_ns() + timeout_ms * 1'000'000;
    while (!(reg32(kStatus) & 1) && clock::now_ns() < ready) {
        if (reg32(kStatus) & 2) return false;   // controller fatal status
        sched::sleep_ms(1);
    }
    if (!(reg32(kStatus) & 1)) return false;

    // The I/O queues: completion queue 1 first, then submission queue 1 on it.
    Command create{};
    create.dword0 = 0x05;
    create.prp1 = io.cq;
    create.cdw10 = ((entries - 1) << 16) | 1;
    create.cdw11 = 1;                     // physically contiguous, no interrupts
    if (run(admin, create) != 0) return false;
    create = Command{};
    create.dword0 = 0x01;
    create.prp1 = io.sq;
    create.cdw10 = ((entries - 1) << 16) | 1;
    create.cdw11 = (1u << 16) | 1;        // on completion queue 1, contiguous
    if (run(admin, create) != 0) return false;
    admin.next_id = io.next_id = 0;
    io.tail = io.head = 0;
    return true;
}

int Controller::run(Queue& queue, Command command, u32* result) {
    const u16 id = queue.next_id++;
    command.dword0 = (command.dword0 & 0xffff) | (static_cast<u32>(id) << 16);
    auto* sq = mm::phys_to_virt<Command>(queue.sq);
    auto* cq = mm::phys_to_virt<volatile Completion>(queue.cq);
    const u32 entries = kQueueEntries;
    sq[queue.tail] = command;
    queue.tail = (queue.tail + 1) % entries;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *queue.sq_doorbell = queue.tail;

    const u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    for (u32 spins = 0;; spins++) {
        const volatile Completion& c = cq[queue.head];
        if (((c.status & 1) != 0) == queue.phase && c.command_id == id) {
            const int status = (c.status >> 1) & 0x7fff;
            if (result) *result = c.result;
            queue.head = (queue.head + 1) % entries;
            if (queue.head == 0) queue.phase = !queue.phase;
            *queue.cq_doorbell = queue.head;
            return status;
        }
        if (clock::now_ns() >= deadline) return -1;
        // Flash answers in microseconds: spin a little before yielding.
        if (spins > 2000) sched::yield();
        else __builtin_ia32_pause();
    }
}

Namespace::Namespace(Controller* controller, u32 nsid, u64 blocks, u32 block_size, u64 bounce, u32 index)
    : m_controller(controller), m_nsid(nsid), m_block_size(block_size), m_bounce(bounce) {
    char name[16];
    ksnprintf(name, sizeof(name), "nvme%un%u", index, nsid);
    set_identity(name, blocks * (block_size / block::kSectorSize));
}

int Namespace::io(bool write, u64 first, u32 blocks) {
    Command c{};
    c.dword0 = write ? 0x01 : 0x02;
    c.nsid = m_nsid;
    c.prp1 = m_bounce;
    const u64 bytes = static_cast<u64>(blocks) * m_block_size;
    if (bytes > mm::kPageSize) c.prp2 = m_bounce + mm::kPageSize;
    c.cdw10 = static_cast<u32>(first);
    c.cdw11 = static_cast<u32>(first >> 32);
    c.cdw12 = blocks - 1;
    return m_controller->run(m_controller->io, c) == 0 ? 0 : -SIRIUS_EIO;
}

// 512-byte sectors on top of the device's blocks. Where those are 4 KiB, a
// write of less than a block reads the block first.
int Namespace::transfer(bool write, u64 lba, u32 count, u8* buffer) {
    if (lba > sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
    const u32 per_block = m_block_size / block::kSectorSize;
    u8* bounce = mm::phys_to_virt<u8>(m_bounce);
    while (count > 0) {
        const u64 first = lba / per_block;
        const u32 skip = static_cast<u32>(lba % per_block);
        u32 blocks = static_cast<u32>(kBounceBytes / m_block_size);
        const u64 needed = (skip + count + per_block - 1) / per_block;
        if (needed < blocks) blocks = static_cast<u32>(needed);
        u32 sectors = blocks * per_block - skip;
        if (sectors > count) sectors = count;
        const u64 bytes = static_cast<u64>(sectors) * block::kSectorSize;
        if (write) {
            const bool partial = skip != 0 || (skip + sectors) % per_block != 0;
            if (partial) if (const int e = io(false, first, blocks)) return e;
            memcpy(bounce + skip * block::kSectorSize, buffer, bytes);
            if (const int e = io(true, first, blocks)) return e;
        } else {
            if (const int e = io(false, first, blocks)) return e;
            memcpy(buffer, bounce + skip * block::kSectorSize, bytes);
        }
        lba += sectors;
        count -= sectors;
        buffer += bytes;
    }
    return 0;
}

int Namespace::read(u64 lba, u32 count, void* buffer) {
    sched::ScopedMutex guard(m_lock);
    return transfer(false, lba, count, static_cast<u8*>(buffer));
}

int Namespace::write(u64 lba, u32 count, const void* buffer) {
    sched::ScopedMutex guard(m_lock);
    return transfer(true, lba, count, static_cast<u8*>(const_cast<void*>(buffer)));
}

int Namespace::flush() {
    sched::ScopedMutex guard(m_lock);
    Command c{};
    c.dword0 = 0x00;
    c.nsid = m_nsid;
    return m_controller->run(m_controller->io, c) == 0 ? 0 : -SIRIUS_EIO;
}

} // namespace

void init() {
    u32 found = 0;
    for (u32 i = 0;; i++) {
        const pci::Device* device = pci::find_class(0x01, 0x08, 0x02, i);
        if (device == nullptr) break;
        auto* controller = new Controller();
        if (!controller->start(*device)) {
            LOG_ERROR("nvme: %02x:%02x.%u would not start", device->bus, device->slot, device->function);
            continue;
        }
        // Namespace 1, which is where the whole drive is on a laptop.
        const u64 page = pmm::alloc_page_zeroed();
        const u64 bounce = pmm::alloc_pages(kBounceOrder);
        if (!page || !bounce) continue;
        Command identify{};
        identify.dword0 = 0x06;
        identify.nsid = 1;
        identify.prp1 = page;
        identify.cdw10 = 0;       // CNS 0: the namespace
        if (controller->run(controller->admin, identify) != 0) {
            LOG_ERROR("nvme%u: namespace 1 does not identify itself", found);
            continue;
        }
        const u8* data = mm::phys_to_virt<u8>(page);
        u64 blocks;
        memcpy(&blocks, data, 8);
        const u8 format = data[26] & 0xf;
        u32 lbaf;
        memcpy(&lbaf, data + 128 + 4 * format, 4);
        const u32 block_size = 1u << ((lbaf >> 16) & 0xff);
        if (block_size != 512 && block_size != 4096) {
            LOG_ERROR("nvme%u: %u-byte blocks are not supported", found, block_size);
            continue;
        }
        auto* disk = new Namespace(controller, 1, blocks, block_size, bounce, found);
        LOG_INFO("nvme: %s: %lu MiB in %u-byte blocks", disk->name(), blocks * block_size >> 20, block_size);
        block::add_disk(disk);
        found++;
    }
}

} // namespace nvme
