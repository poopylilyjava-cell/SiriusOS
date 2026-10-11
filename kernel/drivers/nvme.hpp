#pragma once

// NVMe solid-state drives: what a laptop's disk is. Each namespace becomes a
// block device (block/block.hpp), and its GPT partitions with it.
//
// Nothing is written to a drive unless one of its partitions is mounted, and
// only a partition labelled SIRIUS-ROOT ever is: another system's disk is
// read, to list its partitions, and left alone.
namespace nvme {

// Find every NVMe controller and bring up its first namespace. Sleeps.
void init();

} // namespace nvme
