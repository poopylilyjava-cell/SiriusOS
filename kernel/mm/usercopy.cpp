#include <mm/usercopy.hpp>
#include <mm/vmm.hpp>
#include <mm/paging_math.hpp>
#include <arch/x86_64/percpu.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace usercopy {
namespace {

constexpr u64 kUserMin = 0x1000;                   // the null page is never mapped
constexpr u64 kUserEnd = 0x0000800000000000ULL;

bool check(u64 address, u64 length, bool write) {
    if (length == 0) return true;
    if (address < kUserMin || address >= kUserEnd || length > kUserEnd - address) return false;

    vmm::AddressSpace* space = percpu::current()->address_space;
    if (space == nullptr) return false;

    for (u64 page = mm::align_down(address, mm::kPageSize); page < address + length; page += mm::kPageSize) {
        const u64* entry = space->entry_for(page, false);
        if (entry == nullptr || !(*entry & vmm::flags::kUser)) return false;
        const u64 e = *entry;
        if (e & vmm::flags::kPresent) {
            // Copy-on-write pages are writable: the fault makes the copy.
            if (write && !(e & (vmm::flags::kWrite | vmm::flags::kCopyOnWrite))) return false;
        } else if (e & vmm::flags::kDemand) {
            if (write && !(e & vmm::flags::kWrite)) return false;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

bool readable(u64 address, u64 length) { return check(address, length, false); }
bool writable(u64 address, u64 length) { return check(address, length, true); }

int copy_from(void* destination, u64 source, u64 length) {
    if (!readable(source, length)) return -SIRIUS_EFAULT;
    memcpy(destination, reinterpret_cast<const void*>(source), length);
    return 0;
}

int copy_to(u64 destination, const void* source, u64 length) {
    if (!writable(destination, length)) return -SIRIUS_EFAULT;
    memcpy(reinterpret_cast<void*>(destination), source, length);
    return 0;
}

i64 string_from(char* destination, u64 source, u64 size) {
    if (size == 0) return -SIRIUS_ENAMETOOLONG;
    // Page by page: the string may end well before an unmapped page that a
    // whole-buffer check would have rejected.
    for (u64 copied = 0; copied < size;) {
        const u64 address = source + copied;
        const u64 page_left = mm::kPageSize - (address % mm::kPageSize);
        const u64 chunk = page_left < size - copied ? page_left : size - copied;
        if (!readable(address, chunk)) return -SIRIUS_EFAULT;

        const char* in = reinterpret_cast<const char*>(address);
        for (u64 i = 0; i < chunk; i++) {
            destination[copied + i] = in[i];
            if (in[i] == '\0') return static_cast<i64>(copied + i);
        }
        copied += chunk;
    }
    destination[size - 1] = '\0';
    return -SIRIUS_ENAMETOOLONG;
}

} // namespace usercopy
