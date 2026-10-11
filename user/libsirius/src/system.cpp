#include <sirius/system.hpp>

#ifdef __sirius__

#include <cstring>

#include <sirius/syscall.h>
#include <sirius/sysinfo.h>

namespace {

long system_call(long number, long a = 0, long b = 0) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return result;
}

} // namespace

namespace sirius {

bool system_info(SystemInfo& out) {
    static sirius_sysinfo raw;
    if (system_call(SYS_SYSINFO, reinterpret_cast<long>(&raw)) != 0) return false;
    out.uptime_seconds = static_cast<double>(raw.uptime_ns) / 1e9;
    out.memory_total = raw.memory_total;
    out.memory_free = raw.memory_free;
    out.process_count = raw.process_count;
    out.tick_hz = raw.tick_hz;
    out.cpu_ticks.assign(raw.cpu_ticks, raw.cpu_ticks + raw.cpu_count);
    out.cpu_idle_ticks.assign(raw.cpu_idle_ticks, raw.cpu_idle_ticks + raw.cpu_count);
    return true;
}

std::vector<ProcessInfo> processes() {
    static sirius_procinfo raw[256];
    const long count = system_call(SYS_PROCLIST, reinterpret_cast<long>(raw), 256);
    std::vector<ProcessInfo> list;
    for (long i = 0; i < count; i++) {
        ProcessInfo info;
        info.pid = raw[i].pid;
        info.parent = raw[i].parent;
        info.cpu = raw[i].cpu;
        info.state = raw[i].state == SIRIUS_PROC_ZOMBIE ? ProcessState::Zombie
                   : raw[i].state == SIRIUS_PROC_SLEEPING ? ProcessState::Sleeping : ProcessState::Running;
        info.cpu_ticks = raw[i].cpu_ticks;
        info.name.assign(raw[i].name, strnlen(raw[i].name, sizeof(raw[i].name)));
        list.push_back(info);
    }
    return list;
}

} // namespace sirius

#else // not Sirius: plausible numbers that move, for the host harness

#include <cmath>
#include <ctime>
#include <unistd.h>

namespace sirius {

bool system_info(SystemInfo& out) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
    out.uptime_seconds = seconds;
    out.memory_total = 2048ull << 20;
    out.memory_free = (1200ull << 20) + static_cast<uint64_t>(200.0 * std::sin(seconds / 5.0)) * (1ull << 20);
    out.process_count = 3;
    out.tick_hz = 100;
    out.cpu_ticks.assign(4, static_cast<uint64_t>(seconds * 100.0));
    out.cpu_idle_ticks.clear();
    for (int cpu = 0; cpu < 4; cpu++)   // each CPU idles a different, slowly drifting share of the time
        out.cpu_idle_ticks.push_back(static_cast<uint64_t>(seconds * 100.0 * (0.5 + 0.1 * cpu)
                                                           + 300.0 * std::sin(seconds / (3.0 + cpu))));
    return true;
}

std::vector<ProcessInfo> processes() {
    ProcessInfo desktop{ 1, 0, 0, ProcessState::Running, 1234, "siriusd" };
    ProcessInfo shell{ 2, 1, 1, ProcessState::Sleeping, 12, "sh" };
    ProcessInfo self{ static_cast<unsigned>(getpid()), 1, 2, ProcessState::Running, 99, "host-harness" };
    return { desktop, shell, self };
}

} // namespace sirius

#endif
