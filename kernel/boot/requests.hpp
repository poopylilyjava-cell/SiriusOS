#pragma once
#include <klib/types.hpp>
#include <limine.h>

// Base revision 6 is the current, non-deprecated protocol revision (0-5 are
// deprecated upstream). On x86-64 it also guarantees a clean control-register
// state, a null IDT and TSS, masked legacy PICs and a quiesced local APIC --
// exactly the blank slate the kernel wants at entry.
inline constexpr u64 kLimineBaseRevision = 6;

extern volatile limine_framebuffer_request        g_framebuffer_request;
extern volatile limine_memmap_request             g_memmap_request;
extern volatile limine_hhdm_request               g_hhdm_request;
extern volatile limine_executable_cmdline_request g_cmdline_request;
extern volatile limine_executable_address_request g_executable_address_request;
extern volatile limine_bootloader_info_request    g_bootloader_info_request;
extern volatile limine_mp_request                 g_mp_request;
extern volatile limine_module_request             g_module_request;
extern volatile limine_rsdp_request               g_rsdp_request;

// True if the bootloader acknowledged the base revision we asked for. If it
// did not, it booted us under some older revision whose guarantees differ.
bool limine_base_revision_supported();

// The revision the bootloader actually used, or 0 if it did not report one.
u64 limine_loaded_base_revision();
