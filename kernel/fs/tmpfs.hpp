#pragma once
#include <fs/vfs.hpp>

namespace tmpfs {

// A new, empty in-memory filesystem. Its contents are freed when it is.
vfs::Filesystem* create();

} // namespace tmpfs
