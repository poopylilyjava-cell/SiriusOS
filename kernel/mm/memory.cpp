#include <mm/memory.hpp>

namespace mm {
namespace { u64 g_hhdm_offset = 0; }

void set_hhdm_offset(u64 offset) { g_hhdm_offset = offset; }
u64  hhdm_offset() { return g_hhdm_offset; }

} // namespace mm
