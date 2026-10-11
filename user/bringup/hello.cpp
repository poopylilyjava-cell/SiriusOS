// hello: report what execve handed over -- arguments, environment, pid and
// parent -- and exit with status 7, which the M3 exit test checks for.
#include "runtime.hpp"

extern "C" int main(int argc, char** argv, char** envp) {
    rt::Line line;
    line.str("[hello] pid ").num(rt::getpid()).str(" ppid ").num(rt::getppid()).str(" argv:");
    for (int i = 0; i < argc; i++) line.str(" ").str(argv[i]);
    line.print();

    for (char** e = envp; *e; e++) rt::Line().str("[hello] env ").str(*e).print();
    return 7;
}
