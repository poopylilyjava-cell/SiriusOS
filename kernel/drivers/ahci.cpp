// AHCI 1.3 SATA driver, polled.
//
// One command slot per port and a 64 KiB bounce buffer: callers hand in any
// kernel buffer, the controller only ever sees the bounce buffer's physical
// address. Completion is polled with a yield between checks, so a waiting
// thread costs its CPU nothing much -- interrupts arrive with the I/O APIC.
#include <drivers/ahci.hpp>
#include <drivers/pci.hpp>
#include <block/block.hpp>
#include <sched/mutex.hpp>
#include <sched/sched.hpp>
#include <arch/x86_64/clock.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace ahci {
namespace {

// --- HBA registers -------------------------------------------------------------
constexpr u32 kCap   = 0x00;
constexpr u32 kGhc   = 0x04;
constexpr u32 kPi    = 0x0c;
constexpr u32 kVs    = 0x10;
constexpr u32 kCap2  = 0x24;
constexpr u32 kBohc  = 0x28;

constexpr u32 kCapS64a   = 1u << 31;   // 64-bit DMA addresses
constexpr u32 kGhcAe     = 1u << 31;   // AHCI enable
constexpr u32 kGhcIe     = 1u << 1;
constexpr u32 kCap2Boh   = 1u << 0;    // BIOS/OS handoff supported
constexpr u32 kBohcBos   = 1u << 0;
constexpr u32 kBohcOos   = 1u << 1;
constexpr u32 kBohcBb    = 1u << 4;

// Per-port registers, at 0x100 + port * 0x80.
constexpr u32 kPxClb  = 0x00;
constexpr u32 kPxClbu = 0x04;
constexpr u32 kPxFb   = 0x08;
constexpr u32 kPxFbu  = 0x0c;
constexpr u32 kPxIs   = 0x10;
constexpr u32 kPxIe   = 0x14;
constexpr u32 kPxCmd  = 0x18;
constexpr u32 kPxTfd  = 0x20;
constexpr u32 kPxSig  = 0x24;
constexpr u32 kPxSsts = 0x28;
constexpr u32 kPxSerr = 0x30;
constexpr u32 kPxCi   = 0x38;

constexpr u32 kCmdSt  = 1u << 0;
constexpr u32 kCmdFre = 1u << 4;
constexpr u32 kCmdFr  = 1u << 14;
constexpr u32 kCmdCr  = 1u << 15;

constexpr u32 kTfdErr = 1u << 0;
constexpr u32 kTfdDrq = 1u << 3;
constexpr u32 kTfdBsy = 1u << 7;
constexpr u32 kIsTfes = 1u << 30;      // task file error

constexpr u32 kSigAta = 0x00000101;

// --- ATA commands ----------------------------------------------------------------
constexpr u8 kAtaIdentify     = 0xec;
constexpr u8 kAtaReadDmaExt   = 0x25;
constexpr u8 kAtaWriteDmaExt  = 0x35;
constexpr u8 kAtaFlushCacheExt = 0xea;

constexpr u8 kFisTypeH2d = 0x27;

// --- in-memory structures ---------------------------------------------------------
struct CommandHeader {
    u16 flags;        // CFL in bits 0-4, W = bit 6
    u16 prdt_length;
    u32 bytes_transferred;
    u32 table_low;
    u32 table_high;
    u32 reserved[4];
};
static_assert(sizeof(CommandHeader) == 32, "AHCI command header is 32 bytes");

struct PrdtEntry {
    u32 address_low;
    u32 address_high;
    u32 reserved;
    u32 byte_count;   // bits 0-21: bytes - 1; bit 31: interrupt on completion
};

struct CommandTable {
    u8 fis[64];
    u8 atapi[16];
    u8 reserved[48];
    PrdtEntry prdt[1];
};
static_assert(sizeof(CommandTable) == 0x90, "one-entry AHCI command table");

// Port memory layout, all inside one zeroed page: command list at 0 (1 KiB
// aligned), received FIS area at 0x400 (256-byte aligned), command table at
// 0x800 (128-byte aligned).
constexpr u64 kCommandListOffset = 0x000;
constexpr u64 kReceivedFisOffset = 0x400;
constexpr u64 kCommandTableOffset = 0x800;

constexpr u8  kBounceOrder = 4;                                  // 16 pages
constexpr u32 kMaxSectorsPerCommand = (mm::kPageSize << kBounceOrder) / block::kSectorSize;
constexpr u64 kCommandTimeoutMs = 5000;
constexpr u64 kPortTimeoutMs = 500;

class Controller;

class Disk final : public block::Device {
public:
    Disk(Controller* controller, u32 port) : m_controller(controller), m_port(port) {}

    bool start(u32 index);
    int read(u64 lba, u32 count, void* buffer) override;
    int write(u64 lba, u32 count, const void* buffer) override;
    int flush() override;

private:
    u32  reg(u32 offset);
    void set_reg(u32 offset, u32 value);
    bool wait_clear(u32 offset, u32 bits, u64 timeout_ms);
    bool stop_engine();
    int  issue(u8 command, u64 lba, u32 sectors, bool write);

    Controller* m_controller;
    u32 m_port;
    sched::Mutex m_lock;
    u64 m_port_memory = 0;    // physical
    u64 m_bounce = 0;         // physical, 2^kBounceOrder pages
};

class Controller {
public:
    explicit Controller(u64 abar) : m_abar(reinterpret_cast<volatile u32*>(abar)) {}
    u32  reg(u32 offset) const { return m_abar[offset / 4]; }
    void set_reg(u32 offset, u32 value) { m_abar[offset / 4] = value; }
    bool dma64() const { return (reg(kCap) & kCapS64a) != 0; }

private:
    volatile u32* m_abar;
};

u32 g_disks = 0;

bool dma_reachable(Controller* c, u64 physical, u64 bytes) {
    return c->dma64() || physical + bytes <= (1ULL << 32);
}

u32 Disk::reg(u32 offset) { return m_controller->reg(0x100 + m_port * 0x80 + offset); }
void Disk::set_reg(u32 offset, u32 value) { m_controller->set_reg(0x100 + m_port * 0x80 + offset, value); }

// The driver polls for completion. Most commands finish in well under a
// millisecond, and giving the CPU away costs whatever the next thread's turn
// is -- a full tick, next to one that never sleeps. So it waits a little
// first, and yields only for a command that is taking its time.
struct Poll {
    static constexpr u64 kSpinNs = 300'000;
    u64 start = clock::now_ns();
    u32 spins = 0;

    void wait() {
        // (The spin count bounds it before the clock is running.)
        if (++spins < 20000 && clock::now_ns() - start < kSpinNs) cpu_relax();
        else sched::yield();
    }
};

bool Disk::wait_clear(u32 offset, u32 bits, u64 timeout_ms) {
    const u64 deadline = sched::uptime_ms() + timeout_ms;
    Poll poll;
    while (reg(offset) & bits) {
        if (sched::uptime_ms() > deadline) return false;
        poll.wait();
    }
    return true;
}

bool Disk::stop_engine() {
    set_reg(kPxCmd, reg(kPxCmd) & ~kCmdSt);
    if (!wait_clear(kPxCmd, kCmdCr, kPortTimeoutMs)) return false;
    set_reg(kPxCmd, reg(kPxCmd) & ~kCmdFre);
    return wait_clear(kPxCmd, kCmdFr, kPortTimeoutMs);
}

int Disk::issue(u8 command, u64 lba, u32 sectors, bool write) {
    if (!wait_clear(kPxTfd, kTfdBsy | kTfdDrq, kCommandTimeoutMs)) {
        LOG_ERROR("ahci: port %u: busy before command %#x", m_port, command);
        return -SIRIUS_EIO;
    }

    auto* header = mm::phys_to_virt<CommandHeader>(m_port_memory + kCommandListOffset);
    auto* table  = mm::phys_to_virt<CommandTable>(m_port_memory + kCommandTableOffset);
    const u32 bytes = sectors * block::kSectorSize;

    memset(table, 0, sizeof(CommandTable));
    u8* fis = table->fis;
    fis[0] = kFisTypeH2d;
    fis[1] = 1 << 7;                       // this FIS carries a command
    fis[2] = command;
    fis[4] = static_cast<u8>(lba);
    fis[5] = static_cast<u8>(lba >> 8);
    fis[6] = static_cast<u8>(lba >> 16);
    fis[7] = 1 << 6;                       // LBA addressing
    fis[8] = static_cast<u8>(lba >> 24);
    fis[9] = static_cast<u8>(lba >> 32);
    fis[10] = static_cast<u8>(lba >> 40);
    fis[12] = static_cast<u8>(sectors);
    fis[13] = static_cast<u8>(sectors >> 8);

    u16 prdt_length = 0;
    if (bytes) {
        table->prdt[0].address_low  = static_cast<u32>(m_bounce);
        table->prdt[0].address_high = static_cast<u32>(m_bounce >> 32);
        table->prdt[0].byte_count   = bytes - 1;
        prdt_length = 1;
    }

    header->flags = static_cast<u16>(5 | (write ? (1 << 6) : 0));   // 5 dwords of FIS
    header->prdt_length = prdt_length;
    header->bytes_transferred = 0;
    header->table_low  = static_cast<u32>(m_port_memory + kCommandTableOffset);
    header->table_high = static_cast<u32>((m_port_memory + kCommandTableOffset) >> 32);

    set_reg(kPxIs, 0xffffffff);
    __asm__ volatile("" : : : "memory");   // everything above is visible before the doorbell
    set_reg(kPxCi, 1);

    const u64 deadline = sched::uptime_ms() + kCommandTimeoutMs;
    Poll poll;
    while (reg(kPxCi) & 1) {
        if (reg(kPxIs) & kIsTfes) break;
        if (sched::uptime_ms() > deadline) {
            LOG_ERROR("ahci: port %u: command %#x timed out", m_port, command);
            stop_engine();
            set_reg(kPxCmd, reg(kPxCmd) | kCmdFre | kCmdSt);
            return -SIRIUS_EIO;
        }
        poll.wait();
    }

    if ((reg(kPxIs) & kIsTfes) || (reg(kPxTfd) & kTfdErr)) {
        LOG_ERROR("ahci: port %u: command %#x at lba %lu failed, tfd %#x", m_port, command, lba, reg(kPxTfd));
        return -SIRIUS_EIO;
    }
    return 0;
}

bool Disk::start(u32 index) {
    if (!stop_engine()) { LOG_WARN("ahci: port %u: engine will not stop", m_port); return false; }

    m_port_memory = pmm::alloc_page_zeroed();
    m_bounce = pmm::alloc_pages(kBounceOrder);
    if (m_port_memory == 0 || m_bounce == 0) return false;
    if (!dma_reachable(m_controller, m_port_memory, mm::kPageSize) ||
        !dma_reachable(m_controller, m_bounce, mm::kPageSize << kBounceOrder)) {
        LOG_WARN("ahci: port %u: buffers above 4 GiB and the controller is 32-bit only", m_port);
        return false;
    }

    set_reg(kPxClb,  static_cast<u32>(m_port_memory + kCommandListOffset));
    set_reg(kPxClbu, static_cast<u32>((m_port_memory + kCommandListOffset) >> 32));
    set_reg(kPxFb,   static_cast<u32>(m_port_memory + kReceivedFisOffset));
    set_reg(kPxFbu,  static_cast<u32>((m_port_memory + kReceivedFisOffset) >> 32));
    set_reg(kPxSerr, 0xffffffff);
    set_reg(kPxIs, 0xffffffff);
    set_reg(kPxIe, 0);                     // polled

    set_reg(kPxCmd, reg(kPxCmd) | kCmdFre);
    if (!wait_clear(kPxTfd, kTfdBsy | kTfdDrq, kPortTimeoutMs)) {
        LOG_WARN("ahci: port %u: device stays busy", m_port);
        return false;
    }
    set_reg(kPxCmd, reg(kPxCmd) | kCmdSt);

    if (issue(kAtaIdentify, 0, 1, false) < 0) return false;
    const auto* identify = mm::phys_to_virt<u16>(m_bounce);

    // Words 100-103: the LBA48 sector count. Without LBA48 support (word 83
    // bit 10), fall back to the 28-bit count in words 60-61.
    u64 sectors;
    if (identify[83] & (1 << 10)) {
        sectors = static_cast<u64>(identify[100]) | (static_cast<u64>(identify[101]) << 16) |
                  (static_cast<u64>(identify[102]) << 32) | (static_cast<u64>(identify[103]) << 48);
    } else {
        LOG_WARN("ahci: port %u: no LBA48, which the read/write commands need", m_port);
        return false;
    }

    // Words 27-46: model, as big-endian character pairs, space padded.
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = static_cast<char>(identify[27 + i] >> 8);
        model[i * 2 + 1] = static_cast<char>(identify[27 + i] & 0xff);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';

    char name[16];
    ksnprintf(name, sizeof(name), "ahci%u", index);
    set_identity(name, sectors);
    LOG_INFO("ahci: %s: port %u, \"%s\", %lu sectors", name, m_port, model, sectors);
    return true;
}

int Disk::read(u64 lba, u32 count, void* buffer) {
    if (lba >= sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
    sched::ScopedMutex guard(m_lock);
    auto* out = static_cast<u8*>(buffer);
    while (count) {
        const u32 n = count < kMaxSectorsPerCommand ? count : kMaxSectorsPerCommand;
        const int status = issue(kAtaReadDmaExt, lba, n, false);
        if (status < 0) return status;
        memcpy(out, mm::phys_to_virt<void>(m_bounce), static_cast<u64>(n) * block::kSectorSize);
        out += static_cast<u64>(n) * block::kSectorSize;
        lba += n;
        count -= n;
    }
    return 0;
}

int Disk::write(u64 lba, u32 count, const void* buffer) {
    if (lba >= sector_count() || count > sector_count() - lba) return -SIRIUS_EIO;
    sched::ScopedMutex guard(m_lock);
    const auto* in = static_cast<const u8*>(buffer);
    while (count) {
        const u32 n = count < kMaxSectorsPerCommand ? count : kMaxSectorsPerCommand;
        memcpy(mm::phys_to_virt<void>(m_bounce), in, static_cast<u64>(n) * block::kSectorSize);
        const int status = issue(kAtaWriteDmaExt, lba, n, true);
        if (status < 0) return status;
        in += static_cast<u64>(n) * block::kSectorSize;
        lba += n;
        count -= n;
    }
    return 0;
}

int Disk::flush() {
    sched::ScopedMutex guard(m_lock);
    return issue(kAtaFlushCacheExt, 0, 0, false);
}

// Take the controller from the firmware, if it says it might still own it.
void bios_handoff(Controller& hba) {
    if (!(hba.reg(kCap2) & kCap2Boh)) return;
    hba.set_reg(kBohc, hba.reg(kBohc) | kBohcOos);
    const u64 deadline = sched::uptime_ms() + 2000;
    while ((hba.reg(kBohc) & kBohcBos) || (hba.reg(kBohc) & kBohcBb)) {
        if (sched::uptime_ms() > deadline) { LOG_WARN("ahci: firmware did not release the controller"); return; }
        sched::sleep_ms(10);
    }
}

void init_controller(const pci::Device& device) {
    const pci::Bar abar = pci::bar(device, 5);
    if (abar.io || abar.address == 0) { LOG_WARN("ahci: controller has no memory BAR 5"); return; }

    const u64 mapped = vmm::map_mmio(abar.address, abar.size ? abar.size : 0x1100);
    if (mapped == 0) { LOG_ERROR("ahci: cannot map registers"); return; }
    pci::enable_dma(device);

    auto* hba = new Controller(mapped);
    bios_handoff(*hba);
    hba->set_reg(kGhc, (hba->reg(kGhc) | kGhcAe) & ~kGhcIe);

    const u32 version = hba->reg(kVs);
    const u32 implemented = hba->reg(kPi);
    LOG_INFO("ahci: controller %u.%u, ports %#x, %s DMA", version >> 16, (version >> 8) & 0xff,
             implemented, hba->dma64() ? "64-bit" : "32-bit");

    for (u32 port = 0; port < 32; port++) {
        if (!(implemented & (1u << port))) continue;
        const u32 status = hba->reg(0x100 + port * 0x80 + kPxSsts);
        const u32 detected = status & 0xf, power = (status >> 8) & 0xf;
        if (detected != 3 || power != 1) continue;          // no device, or not active
        const u32 signature = hba->reg(0x100 + port * 0x80 + kPxSig);
        if (signature != kSigAta) {
            LOG_INFO("ahci: port %u: signature %#x is not a SATA disk, skipped", port, signature);
            continue;
        }

        auto* disk = new Disk(hba, port);
        if (!disk->start(g_disks)) {
            LOG_WARN("ahci: port %u: bring-up failed", port);
            continue;   // its memory is leaked deliberately: the HBA may still DMA into it
        }
        g_disks++;
        block::add_disk(disk);
    }
}

} // namespace

void init() {
    for (u32 i = 0;; i++) {
        const pci::Device* device = pci::find_class(0x01, 0x06, 0x01, i);
        if (device == nullptr) break;
        init_controller(*device);
    }
    if (g_disks == 0) LOG_WARN("ahci: no disks found");
}

} // namespace ahci
