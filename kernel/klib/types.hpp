#pragma once
// Freestanding fixed-width types. There is no libc here; these come from the
// compiler's own freestanding headers.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

using u8  = uint8_t;   using i8  = int8_t;
using u16 = uint16_t;  using i16 = int16_t;
using u32 = uint32_t;  using i32 = int32_t;
using u64 = uint64_t;  using i64 = int64_t;

using usize = size_t;
using uptr  = uintptr_t;
