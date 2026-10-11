#pragma once
#include <fs/vfs.hpp>

namespace devfs {

// The device filesystem, with console, null and zero built in. A single
// instance; mount it on /dev.
vfs::Filesystem* instance();

// Publish a device node. `node` must belong to instance() (construct it with
// instance() as its filesystem); devfs takes over the reference.
bool add(const char* name, vfs::Vnode* node);

} // namespace devfs
