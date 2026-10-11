#pragma once

namespace vfs {
// In-kernel VFS tests on a scratch tmpfs mounted at /tmp: the dentry cache,
// mount crossing, "..", and the rules for unlinking. Needs /tmp to exist.
// Returns true if everything passed.
bool run_selftests();
}
