// libsirius: what a Sirius program asks the system that POSIX has no call
// for. For now: the state of the machine and the list of processes.
//
// It builds on Linux too, where the answers are stand-ins good enough to
// develop a user interface against (tests/host_harness).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sirius {

struct SystemInfo {
    double   uptime_seconds = 0;
    uint64_t memory_total = 0;       // bytes
    uint64_t memory_free = 0;
    unsigned process_count = 0;
    unsigned tick_hz = 100;
    // Per CPU, since boot. Load over an interval is how much of the growth
    // in `ticks` was not growth in `idle_ticks`.
    std::vector<uint64_t> cpu_ticks;
    std::vector<uint64_t> cpu_idle_ticks;
};

enum class ProcessState { Running, Sleeping, Zombie };

struct ProcessInfo {
    unsigned pid = 0;
    unsigned parent = 0;
    unsigned cpu = 0;
    ProcessState state = ProcessState::Running;
    uint64_t cpu_ticks = 0;          // CPU time used, in ticks
    std::string name;
};

bool system_info(SystemInfo& out);
std::vector<ProcessInfo> processes();

} // namespace sirius
