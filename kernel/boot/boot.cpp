// Kernel entry point: the first Sirius code the CPU executes.
//
// Limine hands us a 64-bit long-mode CPU in the higher half with paging on,
// interrupts off, its own GDT loaded and no IDT at all. The kernel takes
// ownership of that state, then of memory, then of the other CPUs, and finally
// hands the machine to the scheduler.
#include <boot/requests.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/lapic.hpp>
#include <arch/x86_64/refclock.hpp>
#include <drivers/fbconsole.hpp>
#include <drivers/usb/xhci.hpp>
#include <drivers/nvme.hpp>
#include <drivers/i2c/i2c_hid.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/serial.hpp>
#include <arch/x86_64/smp.hpp>
#include <arch/x86_64/syscall.hpp>
#include <drivers/framebuffer.hpp>
#include <klib/panic.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <mm/memory.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <mm/heap.hpp>
#include <mm/selftest.hpp>
#include <sched/process.hpp>
#include <sched/sched.hpp>
#include <sched/selftest.hpp>
#include <drivers/ahci.hpp>
#include <block/block.hpp>
#include <fs/devfs.hpp>
#include <fs/fat/fatfs.hpp>
#include <fs/selftest.hpp>
#include <fs/tmpfs.hpp>
#include <block/ramdisk.hpp>
#include <fs/vfs.hpp>
#include <sirius/syscall.h>
#include <drivers/pci.hpp>
#include <drivers/fbdev.hpp>
#include <drivers/input.hpp>
#include <drivers/ps2.hpp>
#include <drivers/rtc.hpp>
#include <acpi/acpi.hpp>
#include <arch/x86_64/ioapic.hpp>

extern "C" {
extern char __kernel_start[];
extern char __kernel_end[];
// Emitted by the linker script; see .init_array in linker.ld.
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
}

namespace {

// Any object with a non-trivial static initializer is uninitialised until this
// runs. Nothing before this line may depend on one.
void call_global_constructors() {
    for (auto** fn = __init_array_start; fn != __init_array_end; fn++) (*fn)();
}

const char* boot_cmdline() {
    if (g_cmdline_request.response == nullptr) return "";
    const char* cmdline = g_cmdline_request.response->cmdline;
    return cmdline ? cmdline : "";
}

void apply_cmdline_loglevel(const char* cmdline) {
    if (str_contains(cmdline, "loglevel=debug"))      klog_set_level(LogLevel::Debug);
    else if (str_contains(cmdline, "loglevel=warn"))  klog_set_level(LogLevel::Warn);
    else if (str_contains(cmdline, "loglevel=error")) klog_set_level(LogLevel::Error);
    else                                              klog_set_level(LogLevel::Info);
}

const char* memmap_type_name(u64 type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi-reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader-reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "executable+modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved-mapped";
    default:                                   return "unknown";
    }
}

void report_memory_map() {
    if (g_memmap_request.response == nullptr) {
        LOG_WARN("memmap: bootloader returned no memory map");
        return;
    }

    const auto* response = g_memmap_request.response;
    u64 usable = 0, reclaimable = 0, total = 0;

    LOG_INFO("memmap: %lu entries", response->entry_count);
    for (u64 i = 0; i < response->entry_count; i++) {
        const auto* entry = response->entries[i];
        total += entry->length;
        if (entry->type == LIMINE_MEMMAP_USABLE) usable += entry->length;
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) reclaimable += entry->length;

        LOG_DEBUG("  [%2lu] %016lx-%016lx  %8lu KiB  %s",
                  i, entry->base, entry->base + entry->length,
                  entry->length / 1024, memmap_type_name(entry->type));
    }

    LOG_INFO("memmap: %lu MiB usable, %lu KiB bootloader-reclaimable, %lu MiB total",
             usable / (1024 * 1024), reclaimable / 1024, total / (1024 * 1024));
}

void report_boot_environment() {
    if (g_bootloader_info_request.response != nullptr) {
        LOG_INFO("bootloader: %s %s",
                 g_bootloader_info_request.response->name,
                 g_bootloader_info_request.response->version);
    }

    LOG_INFO("kernel: %p-%p (%lu KiB)",
             static_cast<void*>(__kernel_start), static_cast<void*>(__kernel_end),
             static_cast<u64>(__kernel_end - __kernel_start) / 1024);

    if (g_hhdm_request.response != nullptr)
        LOG_INFO("hhdm: physical memory mapped at offset %p",
                 reinterpret_cast<void*>(g_hhdm_request.response->offset));

    const char* cmdline = boot_cmdline();
    LOG_INFO("cmdline: \"%s\"", cmdline);
}

// Take the bootloader's framebuffer and clear it to black: whatever the
// firmware and the bootloader left there goes, and the screen stays dark
// until user space draws -- the desktop's loading screen is the first thing
// on it. Clearing through the direct map after the page table switch is a
// test too: it only works if the map was rebuilt correctly.
void setup_framebuffer() {
    if (g_framebuffer_request.response == nullptr ||
        g_framebuffer_request.response->framebuffer_count == 0) {
        LOG_WARN("framebuffer: bootloader provided none");
        return;
    }

    const auto* fb = g_framebuffer_request.response->framebuffers[0];
    g_framebuffer.init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp,
                       fb->red_mask_shift, fb->green_mask_shift, fb->blue_mask_shift);

    LOG_INFO("framebuffer: %lux%lu %u bpp, pitch %lu, at %p",
             fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    LOG_DEBUG("framebuffer: r<<%u g<<%u b<<%u",
              fb->red_mask_shift, fb->green_mask_shift, fb->blue_mask_shift);

    g_framebuffer.clear(g_framebuffer.pack(0, 0, 0));
}

// Deliberately fault, to demonstrate that the IDT produces a readable dump
// rather than a silent triple-fault reboot. Opt in with a boot cmdline flag.
[[noreturn]] void run_fault_test(const char* cmdline) {
    if (str_contains(cmdline, "faulttest=df")) {
        LOG_WARN("fault test: overflowing the kernel stack to force a #DF");
        // Unbounded recursion: the stack runs into its guard page, the page
        // fault cannot push its frame there, and the CPU escalates to a double
        // fault. Landing on the IST1 stack is what keeps this reportable.
        // The recursion is unbounded by design, so silence the warning that
        // correctly identifies it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
        struct Recurse {
            static void go(u64 depth) {
                [[maybe_unused]] volatile u8 pad[4096];
                pad[0] = 1;
                go(depth + 1);
            }
        };
#pragma GCC diagnostic pop
        Recurse::go(0);
    }

    if (str_contains(cmdline, "faulttest=gp")) {
        LOG_WARN("fault test: touching a non-canonical address to force a #GP");
        // Bit 47 set with zero bits above it is non-canonical, so the CPU
        // faults on the address itself and never reaches a page walk.
        auto* noncanonical = reinterpret_cast<volatile u64*>(0x0000'dead'0000'0000ULL);
        *noncanonical = 0x1234;
    }

    LOG_WARN("fault test: dereferencing an unmapped address to force a #PF");
    // Canonical (bits 63:47 all zero) but unmapped: base revision 1 and later
    // drop the identity map, so nothing lives down here.
    auto* unmapped = reinterpret_cast<volatile u64*>(0x0000'0000'dead'0000ULL);
    *unmapped = 0x1234;

    panic("fault test: the faulting access did not trap");
}

// The value of `key=` on the command line, copied into `out`. False if absent.
bool cmdline_value(const char* cmdline, const char* key, char* out, usize size) {
    const usize key_length = strlen(key);
    for (const char* p = cmdline; *p; p++) {
        if ((p != cmdline && p[-1] != ' ') || strncmp(p, key, key_length) != 0 || p[key_length] != '=') continue;
        const char* value = p + key_length + 1;
        usize n = 0;
        while (value[n] && value[n] != ' ' && n + 1 < size) { out[n] = value[n]; n++; }
        out[n] = '\0';
        return true;
    }
    return false;
}

// A boot module whose string is "rootfs" is not a program: it is the root
// filesystem, loaded into memory by the bootloader. Null if there is none.
const limine_file* rootfs_module() {
    const limine_module_response* response = g_module_request.response;
    for (u64 i = 0; response && i < response->module_count; i++)
        if (response->modules[i]->string && strcmp(response->modules[i]->string, "rootfs") == 0) return response->modules[i];
    return nullptr;
}

// The root filesystem: the image the bootloader loaded, if it loaded one (a
// system booted from a USB stick); else the GPT partition named SIRIUS-ROOT
// if it holds FAT32; else an empty tmpfs so that the rest of boot still has
// a namespace.
void mount_filesystems() {
    vfs::Filesystem* root = nullptr;
    if (const limine_file* image = rootfs_module()) {
        auto* disk = new block::RamDisk("ram0", image->address, image->size);
        int error = 0;
        root = fatfs::mount(disk, error);
        if (root == nullptr) LOG_ERROR("init: the rootfs module is not FAT32 (error %d)", -error);
        else LOG_INFO("init: root filesystem in memory, %lu MiB from %s", image->size >> 20, image->path);
    }
    if (root == nullptr) {
        if (block::Device* device = block::find_by_label("SIRIUS-ROOT")) {
            int error = 0;
            root = fatfs::mount(device, error);
            if (root == nullptr) LOG_ERROR("init: SIRIUS-ROOT on %s is not FAT32 (error %d)", device->name(), -error);
        } else {
            LOG_WARN("init: no partition labelled SIRIUS-ROOT");
        }
    }
    if (root == nullptr) {
        LOG_WARN("init: using an empty tmpfs as the root");
        root = tmpfs::create();
    }
    if (vfs::mount_root(root) < 0) panic("init: cannot mount the root filesystem");

    // Mount points must exist on the root. Creating them is harmless on tmpfs
    // and a one-time fix-up on a disk that lacks them.
    const char* const mount_points[] = {"/dev", "/tmp"};
    for (const char* directory : mount_points) {
        const int status = vfs::mkdir(nullptr, directory);
        if (status < 0 && status != -SIRIUS_EEXIST)
            LOG_WARN("init: cannot create %s (error %d)", directory, -status);
    }
    if (vfs::mount(devfs::instance(), "/dev") < 0) LOG_ERROR("init: cannot mount devfs on /dev");
    if (vfs::mount(tmpfs::create(), "/tmp") < 0) LOG_ERROR("init: cannot mount tmpfs on /tmp");
}

// Boot modules, if limine.conf lists any besides a root filesystem, are the
// user programs. Each is started with argv = { file name, module_string }.
// Otherwise the system starts /bin/init from the root filesystem (or
// whatever init= names).
void start_user_programs(const char* cmdline) {
    const limine_module_response* response = g_module_request.response;
    const u64 programs = response ? response->module_count - (rootfs_module() ? 1 : 0) : 0;
    if (programs > 0) {
        for (u64 i = 0; i < response->module_count; i++) {
            const limine_file* file = response->modules[i];
            if (file == rootfs_module()) continue;
            const char* name = file->path;
            for (const char* p = file->path; *p; p++) if (*p == '/') name = p + 1;
            const char* argv[] = {name, file->string};
            const u32 argc = (file->string && file->string[0]) ? 2 : 1;
            sched::spawn_process(name, file->address, file->size, argv, argc);
        }
        return;
    }

    char init[path::kPathMax + 1] = "/bin/init";
    cmdline_value(cmdline, "init", init, sizeof(init));
    sirius_stat st;
    if (vfs::stat(nullptr, init, st) < 0) {
        LOG_INFO("init: %s not found; no user programs to start", init);
        return;
    }
    LOG_INFO("init: starting %s", init);
    const char* argv[] = {init};
    sched::spawn_process_from_path(init, argv, 1);
}

bool g_memory_selftests_passed = true;

// The first kernel thread. It runs whatever needs a scheduler underneath it:
// device bring-up that waits on hardware, tests that block, then user space.
void init_main(void*) {
    const char* cmdline = boot_cmdline();

    pci::init();
    ahci::init();
    nvme::init();
    // What a desktop is made of: a display to map and input to read.
    rtc::init();
    fbdev::init();
    input::init();
    ps2::init();
    // A laptop's own keyboard is on USB, its touchpad usually on I2C.
    xhci::init();
    i2c_hid::init();
    mount_filesystems();

    if (str_contains(cmdline, "selftest")) {
        const bool scheduler = sched::run_selftests();
        const bool filesystem = vfs::run_selftests();
        const bool passed = scheduler && filesystem && g_memory_selftests_passed;
        kprintf(passed ? "sirius: selftest ok\n" : "sirius: selftest FAILED\n");
    }

    start_user_programs(cmdline);
}

u64 g_boot_stack_top = 0;

// Everything after the stack switch. Nothing from kmain's frame is reachable
// here, so state is re-read rather than passed.
[[noreturn]] void kmain_on_kernel_stack() {
    const char* cmdline = boot_cmdline();

    heap::init();
    // The firmware's tables first: they say which timers there are to
    // calibrate the APIC timer against, and how interrupts are wired.
    acpi::init();
    refclock::init();
    lapic::init_bsp();
    // Device interrupts: take over the I/O APIC. Nothing is routed until a
    // driver asks.
    ioapic::init();
    syscall::init_cpu();
    sched::init_bsp(g_boot_stack_top);

    setup_framebuffer();
    // console=fb: the log on screen too, until user space takes it.
    fbconsole::init(str_contains(cmdline, "console=fb"));

    // The marker CI greps for. Keep the exact text stable.
    kprintf("sirius: boot ok\n");

    // The memory tests switch CR3 and count pages, so they run before any
    // other CPU or thread exists to disturb either.
    if (str_contains(cmdline, "selftest")) g_memory_selftests_passed = mm::run_selftests();

    if (str_contains(cmdline, "faulttest")) run_fault_test(cmdline);

    smp::start_aps();

    if (sched::spawn("init", init_main, nullptr) == nullptr) panic("boot: cannot start the init thread");

    // This context is now the BSP's idle thread.
    sched::run_idle();
}

constexpr u64 kKernelStackPages = 16;  // 64 KiB, the same as the bootloader's

} // namespace

extern "C" [[noreturn]] void kmain() {
    serial_init();

    // Fixed banner first: if anything below goes wrong, this is still the proof
    // that the executable was loaded and entered.
    kprintf("\n\nSirius OS -- x86_64 kernel\n");

    if (!limine_base_revision_supported()) {
        kprintf("fatal: bootloader does not support Limine base revision %lu",
                kLimineBaseRevision);
        const u64 actual = limine_loaded_base_revision();
        if (actual != 0) kprintf(" (it used revision %lu)", actual);
        kprintf("\n");
        cpu::halt_forever();
    }

    call_global_constructors();

    const char* cmdline = boot_cmdline();
    apply_cmdline_loglevel(cmdline);

    LOG_INFO("limine: base revision %lu acknowledged", kLimineBaseRevision);

    // The per-CPU record comes first: the page fault handler and everything
    // after it find their CPU through GS.
    percpu::init_bsp();
    cpu::init_features();

    // Take ownership of the descriptor tables before anything can fault: until
    // the IDT is loaded, any exception is an instant triple fault and a reboot.
    gdt::init_bsp(*percpu::current());
    idt::init();

    // Physical access goes through the direct map, so its offset must be known
    // before any memory subsystem starts.
    if (g_hhdm_request.response == nullptr) panic("boot: bootloader supplied no HHDM");
    mm::set_hhdm_offset(g_hhdm_request.response->offset);

    report_boot_environment();
    report_memory_map();

    pmm::init(g_memmap_request.response);
    vmm::init(g_memmap_request.response);

    // Leave the bootloader's stack. It is addressed through the direct map,
    // where an overflow runs silently into the physical memory below it; the
    // kernel's own stack has an unmapped guard page instead.
    g_boot_stack_top = vmm::create_kernel_stack(kKernelStackPages);
    if (g_boot_stack_top == 0) panic("boot: cannot allocate the kernel stack");
    cpu::enter_on_stack(g_boot_stack_top, kmain_on_kernel_stack);
}
