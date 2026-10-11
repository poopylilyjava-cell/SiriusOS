#pragma once
#include <klib/types.hpp>

// The time since boot, from the CPU's time stamp counter: nanosecond
// resolution, and cheap enough to read on every input event and every frame.
// (The scheduler's own clock is the tick, in sched::uptime_ms().)
//
// Assumes a constant-rate TSC that is the same on every CPU -- true of QEMU
// and of every x86-64 CPU made in the last fifteen years.
namespace clock {

// `tsc_hz`: the counter's rate, as lapic.cpp measured it against the PIT.
// Time is counted from this call.
void init(u64 tsc_hz);

// Nanoseconds since init(); 0 before it.
u64 now_ns();

// The time of day: nanoseconds since 1970. set_realtime() says what it is
// now (the RTC driver, at boot); until then it is the time since boot.
void set_realtime(u64 epoch_ns);
u64 realtime_ns();

} // namespace clock
