/* Clocks, as clock_get() names them. The ids are POSIX's (and Linux's). */
#ifndef SIRIUS_TIME_H
#define SIRIUS_TIME_H

/* Both are read from the CPU's time stamp counter, to the nanosecond.
 * CLOCK_MONOTONIC counts from boot; CLOCK_REALTIME is that plus what the
 * battery-backed clock said at boot, taken as it stands: there are no time
 * zones, so it is whatever zone the machine's clock is set to. */
#define SIRIUS_CLOCK_REALTIME   0
#define SIRIUS_CLOCK_MONOTONIC  1

#endif
