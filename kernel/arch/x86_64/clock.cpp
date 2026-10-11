#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/cpu.hpp>
#include <klib/print.hpp>

namespace clock {
namespace {

u64 g_start = 0;
// Nanoseconds per count as a 32.32 fixed-point number: a multiply and a
// shift per reading, no division.
u64 g_multiplier = 0;
u64 g_realtime_offset = 0;   // realtime minus monotonic

} // namespace

void init(u64 tsc_hz) {
    if (tsc_hz == 0) return;
    g_start = cpu::read_tsc();
    g_multiplier = static_cast<u64>((static_cast<unsigned __int128>(1'000'000'000) << 32) / tsc_hz);
    LOG_INFO("clock: tsc at %lu MHz", tsc_hz / 1'000'000);
}

u64 now_ns() {
    if (g_multiplier == 0) return 0;
    const u64 elapsed = cpu::read_tsc() - g_start;
    return static_cast<u64>((static_cast<unsigned __int128>(elapsed) * g_multiplier) >> 32);
}

void set_realtime(u64 epoch_ns) {
    __atomic_store_n(&g_realtime_offset, epoch_ns - now_ns(), __ATOMIC_RELEASE);
}

u64 realtime_ns() { return now_ns() + __atomic_load_n(&g_realtime_offset, __ATOMIC_ACQUIRE); }

} // namespace clock
