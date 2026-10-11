#pragma once

namespace vfs { struct File; }

namespace epoll {

// `file` is being destroyed: drop it from every epoll set that watches it.
void forget(vfs::File* file);

} // namespace epoll
