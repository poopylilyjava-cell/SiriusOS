#pragma once
#include <klib/types.hpp>

// The saved user state, as syscall.S lays it out on the kernel stack: the
// last register pushed comes first. fork() copies one; execve() rewrites one.
struct SyscallFrame {
    u64 r15, r14, r13, r12, rbp, rbx;
    u64 r9, r8, r10, rdx, rsi, rdi;
    u64 rax;                       // number on entry, result on exit
    u64 rip, cs, rflags, rsp, ss;  // rip and rflags arrived in rcx and r11
};

static_assert(sizeof(SyscallFrame) == 18 * 8, "SyscallFrame must match syscall.S");

namespace syscall {

// Program this CPU's SYSCALL MSRs. Needs EFER.SCE, set by cpu::init_features().
void init_cpu();

// Leave for ring 3 with exactly the registers in `frame`, as if returning
// from a system call. The frame is copied to the top of the current thread's
// kernel stack first, so `frame` may live anywhere. Used to start a forked
// child.
[[noreturn]] void return_to_user(const SyscallFrame& frame);

} // namespace syscall
