// Limine boot requests.
//
// Every object here must survive --gc-sections and must land inside the
// delimited request block, which is why each carries `used` and an explicit
// section. The linker script KEEPs those sections for the same reason.
#include <boot/requests.hpp>

// The delimiters let the bootloader find the request block without scanning the
// whole image. From base revision 2 onwards they are honoured, not hints, so a
// request placed outside them is simply never seen.
[[gnu::used, gnu::section(".limine_requests_start")]]
volatile u64 g_limine_requests_start[4] = LIMINE_REQUESTS_START_MARKER;

[[gnu::used, gnu::section(".limine_requests_end")]]
volatile u64 g_limine_requests_end[2] = LIMINE_REQUESTS_END_MARKER;

// The bootloader writes element [2] to zero to acknowledge the revision, and
// element [1] to the revision it actually used.
[[gnu::used, gnu::section(".limine_requests")]]
volatile u64 g_limine_base_revision[3] = LIMINE_BASE_REVISION(kLimineBaseRevision);

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_framebuffer_request g_framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_memmap_request g_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_hhdm_request g_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_cmdline_request g_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_executable_address_request g_executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_bootloader_info_request g_bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

// Application processors. Limine starts them and parks each one spinning on
// its goto_address; the kernel releases them in smp::start_aps(). No x2APIC
// flag: the kernel handles whichever APIC mode the firmware left.
[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_mp_request g_mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .flags = 0,
};

// Boot modules: the user programs listed with module_path in limine.conf.
[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_module_request g_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

// The ACPI root pointer, from which the interrupt routing tables are found.
[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

bool limine_base_revision_supported() {
    return LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision);
}

u64 limine_loaded_base_revision() {
    if (!LIMINE_LOADED_BASE_REVISION_VALID(g_limine_base_revision)) return 0;
    return LIMINE_LOADED_BASE_REVISION(g_limine_base_revision);
}
