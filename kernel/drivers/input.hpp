#pragma once
#include <klib/types.hpp>

// The input event queue behind /dev/input: every keyboard and mouse driver
// posts into it, and user space reads `struct sirius_input_event` records
// out (see sirius/input.h).
namespace input {

// Create the queue and publish /dev/input. Call once devfs exists.
void init();

// Queue one event, stamped with the current time. Safe from an interrupt
// handler. If nobody is reading and the queue is full, the oldest events are
// dropped: input nobody collected is stale, and recent input is not.
void post(u16 type, u16 code, i32 value);

} // namespace input
