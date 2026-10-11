#pragma once
#include <klib/types.hpp>

// Checked access to the calling process's memory, from system calls.
//
// A range passes if every page in it is user memory that is mapped, or
// reserved to be paged in on first touch, with the permission asked for.
// Processes are single-threaded, so nothing can unmap a page between the
// check and the access; the access itself may still fault, and the page
// fault handler resolves that (demand paging, copy-on-write) like any other.
namespace usercopy {

bool readable(u64 address, u64 length);
bool writable(u64 address, u64 length);

// 0, or -EFAULT.
int copy_from(void* destination, u64 source, u64 length);
int copy_to(u64 destination, const void* source, u64 length);

// Copy a NUL-terminated string of at most `size - 1` bytes. Returns its
// length, -EFAULT, or -ENAMETOOLONG if no NUL came within the limit.
i64 string_from(char* destination, u64 source, u64 size);

} // namespace usercopy
