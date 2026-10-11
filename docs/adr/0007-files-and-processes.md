# ADR 0007 — A dentry-cached VFS under one lock, FAT32 root, fork by copy-on-write

**Status:** accepted · 2026-10-10 · **milestone:** M3

## Context
M3 gives user programs files, devices and process control: a VFS with
mounts, tmpfs and devfs, an AHCI disk with FAT32 on it, and `open` through
`waitpid`. The exit test reads a file from disk and forks a child that execs
another binary.

## Decisions

### One VFS lock, for now
A single sleeping mutex covers every namespace operation (lookup, create,
unlink, mount) and every data operation on a filesystem that asks for it.
Device filesystems opt out, so a slow device never waits behind the disk.

*Rejected for now:* per-dentry and per-inode locks. They are where this ends
up, but they are hard to get right while the FAT driver itself is new, and
nothing in M3 has enough concurrency to notice the difference. Splitting the
lock is mechanical once the boundaries are known.

### Dentries are cached for good; there are no negative entries
A name, once looked up, stays in the cache until it is unlinked. Memory grows
with the number of distinct names touched, which is fine at this scale and
avoids an eviction policy before there is pressure to tune one.

### An open file cannot be unlinked
POSIX keeps an unlinked file's data until the last close. FAT has no inode to
hold it, and freeing the clusters while a descriptor still reads them would
hand them to the next writer. `unlink` returns `EBUSY` instead. tmpfs could do
better, but one rule for every filesystem is simpler to rely on.

### The FAT32 driver is a volume layer plus a thin VFS adapter
`fs/fat/volume.cpp` knows only `block::Device` and is compiled on the host,
where the tests run it against images from `mkfs.fat` and mtools, and check
every write with `fsck.fat` and by reading it back with mtools. The adapter
keeps exactly one vnode per directory entry: FAT lookups ignore case, so two
spellings must not become two vnodes with two ideas of the file's size.

Writes are ordered so a crash loses space rather than corrupting it: a new
cluster is zeroed and marked used before anything points at it, and on delete
the name goes before the clusters are freed. Every change is flushed before
the call returns.

### The root is a GPT partition found by name
The kernel mounts the partition named `SIRIUS-ROOT` as `/`, devfs on `/dev`
and a tmpfs on `/tmp`, and runs `/bin/init`. Naming beats position: the ESP
and the BIOS boot partition come first and their number may change.

*Rejected:* the ESP as the root. It belongs to the firmware and the
bootloader, and keeping it read-mostly keeps the machine bootable.

### AHCI is polled; DMA goes through a bounce buffer
Commands wait by yielding, not by interrupt: interrupts arrive with the
I/O APIC. Each port has one command slot and a 64 KiB physically contiguous
bounce buffer, so callers may pass any kernel buffer -- or a validated user
buffer -- without caring about physical contiguity. A write-through sector
cache sits on each disk.

### fork shares the address space copy-on-write; exec builds the new one aside
`fork` marks every writable user page copy-on-write in both processes and
copies demand reservations; the child's first return to ring 3 replays the
parent's system call frame with `rax = 0`. `execve` loads the new image and
its argument stack into a fresh address space and only then swaps it in, so
every failure returns to the old program intact.

### Process status and errors follow Linux's encodings
`waitpid` statuses, `errno` values, open flags and `stat` mode bits use
Linux's numbers, and the entry stack is the System V one (`argc`, `argv`,
`envp`, auxv). mlibc's x86-64 sysdeps can then use their existing tables.
System call *numbers* stay provisional (ADR 0002).

### Wakeups across CPUs are delivered by IPI
A thread woken onto an idle CPU used to wait for that CPU's next tick. With
mutex handoffs across CPUs that made a contended lock run at tick speed (the
scheduler selftest took over 30 s; with the IPI, 150 ms). Waking or starting a
thread on another, idle CPU now sends it a reschedule IPI. A busy CPU is left
alone until its next tick, so its running thread keeps its time slice.

## Consequences
- `mmap` supports private mappings only; `MAP_SHARED` waits for shared memory
  objects in M6. File mappings are read in eagerly.
- Orphans are not reparented to init; an orphan frees itself when it exits.
- No timestamps: FAT entries get a fixed 2026-01-01 until there is an RTC driver.
- Each FAT write flushes the drive, which is slow but safe; a write-back
  cache with a `sync` is the obvious later step.
- PCI configuration goes through the legacy ports, which reach only the first
  256 bytes of each function. ECAM comes with ACPI.
