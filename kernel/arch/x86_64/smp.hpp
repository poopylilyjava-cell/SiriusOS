#pragma once
#include <klib/types.hpp>

namespace smp {

// Release every application processor Limine parked, and wait until each has
// brought itself up and is idling in the scheduler. Returns how many came up.
u32 start_aps();

} // namespace smp
