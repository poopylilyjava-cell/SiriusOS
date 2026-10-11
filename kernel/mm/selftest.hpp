#pragma once

namespace mm {
// In-kernel memory tests. Returns true if everything passed. Run with the
// boot cmdline flag `selftest`.
bool run_selftests();
}
