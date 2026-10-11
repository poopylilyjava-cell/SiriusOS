// ticker: print a numbered line every few tens of milliseconds, then exit.
//
// Two of these, started together, are the M2 exit test: their lines must
// interleave, and they must report different CPUs. Between lines each one also
// does floating-point work across yields and checks the result, which fails if
// the kernel lets another thread's FPU state leak into this one.
#include "runtime.hpp"

namespace {

constexpr int kTicks = 8;

// The same computation twice: once straight through, once yielding the CPU
// every few steps. With FPU state saved and restored per thread the two agree
// to the last bit.
double drift(double seed, bool yield) {
    double value = seed;
    for (int i = 0; i < 2000; i++) {
        value = value * 1.000001 + 0.25;
        if (yield && (i % 200) == 0) rt::yield();
    }
    return value;
}

} // namespace

extern "C" int main(int argc, char** argv, char**) {
    const char* name = argc > 1 ? argv[1] : argv[0];
    const long pid = rt::getpid();
    const double seed = static_cast<double>(pid) + 0.5;
    const double expected = drift(seed, false);

    for (int tick = 1; tick <= kTicks; tick++) {
        rt::Line().str("[").str(name).str("] tick ").num(tick).str("/").num(kTicks)
                  .str(" pid ").num(pid).str(" cpu ").num(rt::getcpu()).print();

        if (drift(seed, true) != expected) {
            rt::Line().str("[").str(name).str("] FPU state corrupted across a yield").print();
            return 1;
        }
        rt::sleep_ms(30);
    }

    rt::Line().str("[").str(name).str("] done").print();
    return 0;
}
