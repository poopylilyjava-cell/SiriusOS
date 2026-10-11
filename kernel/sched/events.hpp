#pragma once
#include <klib/types.hpp>

// One system-wide "something happened" channel: how a thread sleeps until a
// pipe has data, a socket a connection, a child an exit status, a timer an
// expiry -- or any of several at once, which is all poll() is.
//
// Every state change a sleeper could be waiting for ends in notify(). A
// sleeper takes a snapshot() *before* it examines the state it cares about
// (under whatever lock guards that state), and if it finds nothing, calls
// wait() with the snapshot: wait() returns at once if anything was notified
// since, so no wakeup is lost and no lock is shared between subsystems.
//
// Everyone is woken by everything and looks again. That is the right
// trade at this scale -- a handful of processes -- and it is what makes
// waiting on unrelated things at once trivial.
namespace events {

u64 snapshot();

// Something changed. Safe from interrupt handlers.
void notify();

// Sleep until the next notify() after `snapshot`. Returns
//   0            woken (or already notified): look again
//   -ETIMEDOUT   `deadline_ns` (on the clock:: time base; 0 for none) passed
//   -EINTR       a signal is waiting to be delivered to the caller, or its
//                process is being ended by another of its threads
int wait(u64 snapshot, u64 deadline_ns = 0);

// From the timer interrupt: wake sleepers whose deadline has come.
void tick();

} // namespace events
