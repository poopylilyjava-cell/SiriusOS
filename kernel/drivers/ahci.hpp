#pragma once

// AHCI SATA controllers. Each disk found becomes a block device "ahciN".
namespace ahci {

// Find every AHCI controller on the PCI bus, bring up its ports, and
// register the disks. Must run in a thread: commands wait by yielding.
void init();

} // namespace ahci
