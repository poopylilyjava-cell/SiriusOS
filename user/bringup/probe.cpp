// probe: misbehave on purpose, to check that the kernel contains it.
//
// argv[1] -- the boot module's string -- picks the misbehaviour. Every mode except
// "badptr" should get the process killed, with the kernel and every other
// process carrying on unharmed.
#include "runtime.hpp"

extern "C" int main(int argc, char** argv, char**) {
    const char* mode = argc > 1 ? argv[1] : "";
    rt::Line().str("[probe ").str(mode).str("] pid ").num(rt::getpid()).str(" starting").print();

    if (rt::streq(mode, "null")) {
        *reinterpret_cast<volatile int*>(0) = 1;                       // #PF, not present
    } else if (rt::streq(mode, "kernel")) {
        (void)*reinterpret_cast<volatile long*>(0xffffffff80000000UL);  // #PF, supervisor page
    } else if (rt::streq(mode, "privileged")) {
        __asm__ volatile("cli");                                        // #GP in ring 3
    } else if (rt::streq(mode, "invalid")) {
        __asm__ volatile("ud2");                                        // #UD
    } else if (rt::streq(mode, "badptr")) {
        // The kernel must refuse to read kernel memory on our behalf.
        const long result = rt::write(1, reinterpret_cast<const void*>(0xffffffff80000000UL), 16);
        const long unmapped = rt::write(1, reinterpret_cast<const void*>(0x10), 16);
        const bool refused = result == -SIRIUS_EFAULT && unmapped == -SIRIUS_EFAULT;
        rt::Line().str("[probe badptr] write from bad pointers ")
                  .str(refused ? "refused" : "ACCEPTED").print();
        return refused ? 0 : 1;
    } else {
        rt::Line().str("[probe] unknown mode").print();
        return 2;
    }

    rt::Line().str("[probe ").str(mode).str("] survived -- the kernel did not stop it").print();
    return 3;
}
