#pragma once

namespace sched {
// In-kernel scheduler, lock and SMP tests. Must run in a kernel thread, after
// the application processors are online. Returns true if everything passed.
bool run_selftests();
}
