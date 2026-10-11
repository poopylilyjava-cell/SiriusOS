# ADR 0002 — mlibc as the C library

**Status:** accepted · 2026-09-16 · **blocks:** M4, and therefore all userland

## Context
Userland is C++20 and needs libstdc++, which needs a C library. The C library
determines the syscall ABI and how much of POSIX we must implement.

## Decision
Port mlibc, with a Sirius `sysdeps/` backend.

## Alternatives rejected
- **musl** — assumes Linux syscall numbers and semantics throughout; porting it
  means fighting the design rather than filling in a porting layer.
- **newlib** — a thin syscall stub layer, but weak POSIX coverage; libwayland
  and libstdc++ both want more than newlib offers.
- **Own libc** — the fastest way to lose a year. Every ported program would
  then find a different missing function.

## Consequences
- mlibc's `sysdeps/` interface effectively *specifies* the kernel's syscall
  surface. Read it before designing syscalls, not after.
- mlibc has prior art for hosting libwayland, which M8 depends on.
