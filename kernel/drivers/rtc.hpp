#pragma once
#include <klib/types.hpp>

// The battery-backed real-time clock (the CMOS RTC): read once at boot to
// learn what time it is. From then on the time of day is that plus the
// monotonic clock.
namespace rtc {

struct DateTime { u16 year; u8 month, day, hour, minute, second; };

// Seconds since 1970-01-01 00:00:00 for a calendar date and time, in whatever
// zone the fields are in. Pure; tested on the host.
u64 to_epoch(const DateTime& time);

// A raw register value as the chip's status says to read it: binary-coded
// decimal or plain binary. Pure.
u8 decode(u8 raw, bool bcd);

// Read the clock and set the time of day. The clock is taken to hold the
// time the user wants to see: there are no time zones yet.
void init();

} // namespace rtc
