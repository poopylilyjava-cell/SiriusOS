// wltest: run libwayland's own test suite, the other half of the M6 exit
// test. The suite is a directory of programs (built by the wayland port,
// ports/recipes/wayland); each runs its tests in forked children and exits 0
// if all of them passed. This runs every one and reports.
//
//   wltest [directory] [test-program...]
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool ends_with(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

// Run one test program to completion; a description of how it ended in `outcome`.
bool run(const std::string& directory, const std::string& name, std::string& outcome) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { outcome = std::string("fork: ") + std::strerror(errno); return false; }
    if (pid == 0) {
        // The suite finds its helper program through TEST_BUILD_DIR, and
        // makes its sockets and lock files in a directory of its own under
        // XDG_RUNTIME_DIR.
        setenv("TEST_BUILD_DIR", directory.c_str(), 1);
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        const std::string path = directory + "/" + name;
        char* const argv[] = { const_cast<char*>(name.c_str()), nullptr };
        execv(path.c_str(), argv);
        std::fprintf(stderr, "[wltest] cannot execute %s: %s\n", path.c_str(), std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) { outcome = std::string("waitpid: ") + std::strerror(errno); return false; }
    if (WIFEXITED(status)) {
        outcome = "exit status " + std::to_string(WEXITSTATUS(status));
        return WEXITSTATUS(status) == 0;
    }
    outcome = "killed by signal " + std::to_string(WTERMSIG(status));
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const std::string directory = argc > 1 ? argv[1] : "/usr/libexec/wayland-tests";

    std::vector<std::string> programs;
    for (int i = 2; i < argc; i++) programs.emplace_back(argv[i]);
    if (programs.empty()) {
        DIR* dir = opendir(directory.c_str());
        if (dir == nullptr) {
            std::printf("[wltest] no test suite in %s: %s (build it with ./tools/ports.sh add wayland)\n",
                        directory.c_str(), std::strerror(errno));
            return 2;
        }
        while (const dirent* entry = readdir(dir))
            if (ends_with(entry->d_name, "-test")) programs.emplace_back(entry->d_name);
        closedir(dir);
        std::sort(programs.begin(), programs.end());
    }

    std::printf("[wltest] %zu test programs in %s\n", programs.size(), directory.c_str());
    std::vector<std::string> failed;
    for (const std::string& name : programs) {
        std::printf("[wltest] running %s\n", name.c_str());
        std::string outcome;
        const bool passed = run(directory, name, outcome);
        std::printf("[wltest] %s: %s (%s)\n", name.c_str(), passed ? "passed" : "FAILED", outcome.c_str());
        if (!passed) failed.push_back(name);
    }

    std::printf("[wltest] %zu of %zu test programs passed\n", programs.size() - failed.size(), programs.size());
    for (const std::string& name : failed) std::printf("[wltest] failed: %s\n", name.c_str());
    if (failed.empty() && !programs.empty()) std::printf("[wltest] wayland test suite passed\n");
    return failed.empty() && !programs.empty() ? 0 : 1;
}
