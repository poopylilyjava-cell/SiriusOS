#pragma once
#include <block/device.hpp>
#include <fs/path.hpp>

// A FAT32 volume: on-disk structures only, no VFS. Compiles on the host,
// where the tests run it against images from mkfs.fat and check the result
// with fsck.fat.
//
// Not thread-safe: the caller serialises every call (in the kernel, the VFS
// lock does). Names are UTF-8; lookups are case-insensitive for ASCII, as on
// every other FAT implementation.
namespace fat {

// Where a file lives, and where its directory entry lives. The VFS layer
// keeps one per open vnode and passes it back in; write() and truncate()
// update it along with the entry on disk.
struct Node {
    u32  first_cluster;     // 0 for an empty file
    u32  size;              // bytes; always 0 for directories
    bool directory;
    bool is_root;
    u32  parent_cluster;    // first cluster of the containing directory
    u32  entry_index;       // index of the short entry in that directory

    // Cluster-chain position from the last access, so sequential I/O does
    // not walk the chain from the start each time.
    u32  cursor_index;
    u32  cursor_cluster;
};

struct Entry {
    char name[path::kNameMax + 1];
    Node node;
};

struct Geometry {
    u32 sectors_per_cluster;
    u32 cluster_count;
    u32 free_clusters;      // as tracked; 0xffffffff if unknown
};

class Volume {
public:
    // 0, or -errno if the device does not hold a FAT32 filesystem.
    int mount(block::Device* device);

    Node root() const;
    Geometry geometry() const;

    int lookup(const Node& directory, const char* name, Node& out);
    // The entry at `cookie`, then advances it. 1, 0 at the end, or -errno.
    // "." and ".." are not reported.
    int readdir(const Node& directory, u64& cookie, Entry& out);

    i64 read(Node& file, u64 offset, void* buffer, u64 length);
    i64 write(Node& file, u64 offset, const void* buffer, u64 length);
    int truncate(Node& file, u64 size);

    int create(const Node& directory, const char* name, bool is_directory, Node& out);
    // Remove `name`, which must be `victim`. A directory must be empty.
    int unlink(const Node& directory, const char* name, const Node& victim);

    // Write the free-space hints back to the FSInfo sector and flush.
    int sync();

    // Count free clusters by scanning the FAT, ignoring any hint.
    int count_free(u32& out);

private:
    struct Location;
    class DirectoryScan;

    int read_sectors(u64 sector, u32 count, void* buffer);
    int write_sectors(u64 sector, u32 count, const void* buffer);
    u64 cluster_sector(u32 cluster) const;
    bool valid_cluster(u32 cluster) const;

    int fat_get(u32 cluster, u32& value);
    int fat_set(u32 cluster, u32 value);
    int allocate_cluster(u32 previous, bool zero, u32& out);
    int free_chain(u32 first);
    int cluster_at(Node& node, u32 index, bool extend, u32& out);
    int zero_cluster(u32 cluster);

    int locate(u32 directory_cluster, u32 index, bool extend, Location& out);
    int update_entry(const Node& node);
    int find(const Node& directory, const char* name, Node& out, u32* first_slot = nullptr);
    int find_free_run(const Node& directory, u32 count, u32& first);
    bool short_name_taken(const Node& directory, const u8 name[11]);
    int write_entries(u32 directory_cluster, u32 first, const u8* entries, u32 count);
    bool is_empty_directory(const Node& directory);

    block::Device* m_device = nullptr;
    u32 m_sectors_per_cluster = 0;
    u32 m_cluster_bytes = 0;
    u32 m_reserved_sectors = 0;
    u32 m_fat_count = 0;
    u32 m_fat_sectors = 0;
    u64 m_data_start = 0;
    u32 m_cluster_count = 0;   // data clusters; valid numbers are 2..cluster_count+1
    u32 m_root_cluster = 0;
    u32 m_fsinfo_sector = 0;
    u32 m_free_count = 0xffffffff;
    u32 m_next_free = 2;
    bool m_fsinfo_dirty = false;
};

// --- name handling, exposed for the tests ------------------------------------

// If `name` is a valid 8.3 name exactly as written, fill the 11-byte short
// name and the NT case flags that make it read back in its original case.
bool exact_short_name(const char* name, u8 out[11], u8& case_flags);
// The "BASIS~N" short name for a long name.
void generated_short_name(const char* name, u32 n, u8 out[11]);
// The name a short entry reads as, honouring the NT case flags.
void format_short_name(const u8 raw[11], u8 case_flags, char* out);
u8 short_name_checksum(const u8 name[11]);
// Valid as a FAT long name: no reserved characters, no trailing dot or space.
bool valid_long_name(const char* name);

} // namespace fat
