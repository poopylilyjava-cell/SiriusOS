#pragma once
#include <block/device.hpp>

// The kernel's registry of block devices.
namespace block {

// Register a whole disk, put a sector cache in front of it, and register each
// GPT partition on it as a device of its own ("<disk>p<N>").
void add_disk(Device* disk);

u32 count();
Device* at(u32 index);
Device* find(const char* name);

// The partition whose GPT name is `label`, or null.
Device* find_by_label(const char* label);

} // namespace block
