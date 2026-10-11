// The applications built into the desktop.
#pragma once

#include "../shell/app.h"

#include <sys/types.h>

namespace desktop {

App* CreateTerminal(const std::string& directory);
App* CreateFiles(const std::string& directory);
App* CreateEditor(const std::string& path);
App* CreateMonitor(const std::string&);
App* CreateSettings(const std::string&);
App* CreateAbout(const std::string&);

// A child process an application started and is done with: the desktop
// collects its exit status when it comes, so that it does not stay a zombie.
void ReapLater(pid_t pid);

// "1.5 MiB", "312 bytes".
std::string FormatSize(unsigned long long bytes);

} // namespace desktop
