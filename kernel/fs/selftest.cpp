// In-kernel tests for the VFS core. Path parsing and the FAT and GPT code are
// tested on the host; the dentry cache and mounts depend on the scheduler's
// Mutex, so they are tested here, on a throwaway tmpfs.
#include <fs/selftest.hpp>
#include <fs/tmpfs.hpp>
#include <fs/vfs.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>
#include <sirius/errno.h>

namespace vfs {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    LOG_ERROR("selftest FAILED: %s", what);
}

bool write_file(const char* path, const char* text) {
    File* file = nullptr;
    if (open(nullptr, path, SIRIUS_O_WRONLY | SIRIUS_O_CREAT | SIRIUS_O_TRUNC, file) < 0) return false;
    const u64 length = strlen(text);
    const bool ok = write(file, text, length) == static_cast<i64>(length);
    file_close(file);
    return ok;
}

bool file_is(const char* path, const char* text) {
    File* file = nullptr;
    if (open(nullptr, path, SIRIUS_O_RDONLY, file) < 0) return false;
    char buffer[128] = {};
    const i64 n = read(file, buffer, sizeof(buffer) - 1);
    file_close(file);
    return n >= 0 && strcmp(buffer, text) == 0;
}

} // namespace

bool run_selftests() {
    LOG_INFO("selftest: vfs on a scratch tmpfs at /tmp/vfstest");
    check(mkdir(nullptr, "/tmp/vfstest") == 0, "mkdir on the /tmp mount");
    check(mkdir(nullptr, "/tmp/vfstest") == -SIRIUS_EEXIST, "mkdir of an existing name fails");
    check(mkdir(nullptr, "/tmp/vfstest/a") == 0 && mkdir(nullptr, "/tmp/vfstest/a/b") == 0, "nested mkdir");

    check(write_file("/tmp/vfstest/a/b/file", "contents"), "create and write a file");
    check(file_is("/tmp/vfstest/a/b/file", "contents"), "read it back");
    check(file_is("/tmp//vfstest/./a/b/../b/file", "contents"), "\".\", \"..\" and repeated slashes resolve");

    // A second filesystem mounted inside the first: ".." must lead back out.
    Filesystem* inner = tmpfs::create();
    check(mount(inner, "/tmp/vfstest/a") == 0, "mount a tmpfs on a directory");
    check(write_file("/tmp/vfstest/a/inner", "on the mount"), "write onto the mounted filesystem");
    check(file_is("/tmp/vfstest/a/../a/inner", "on the mount"), "\"..\" out of a mount and back in");
    sirius_stat st;
    check(stat(nullptr, "/tmp/vfstest/a/b/file", st) == -SIRIUS_ENOENT, "the mount hides what it covers");
    check(unlink(nullptr, "/tmp/vfstest/a") == -SIRIUS_EBUSY, "a mount point cannot be unlinked");

    Dentry* directory = nullptr;
    check(open_directory(nullptr, "/tmp/vfstest/a", directory) == 0, "open a directory by path");
    char path[path::kPathMax + 1];
    check(path_of(directory, path, sizeof(path)) > 0 && strcmp(path, "/tmp/vfstest/a") == 0,
          "path_of crosses mounts back to the root");
    File* relative = nullptr;
    check(open(directory, "inner", SIRIUS_O_RDONLY, relative) == 0, "a relative path resolves from a directory");
    file_close(relative);
    dentry_unref(directory);

    // Unlink rules.
    File* held = nullptr;
    check(open(nullptr, "/tmp/vfstest/a/inner", SIRIUS_O_RDONLY, held) == 0, "open a file");
    // tmpfs keeps a file's data in the node, so the name can go while the
    // file is open; FAT cannot (fstest checks that on the disk).
    char byte = 0;
    check(unlink(nullptr, "/tmp/vfstest/a/inner") == 0, "an open file on tmpfs can be unlinked");
    check(stat(nullptr, "/tmp/vfstest/a/inner", st) == -SIRIUS_ENOENT, "its name is gone at once");
    check(read(held, &byte, 1) == 1 && byte == 'o', "its contents live until the last close");
    file_close(held);
    check(write_file("/tmp/vfstest/a/inner", "again"), "and the name can be used again");
    check(unlink(nullptr, "/tmp/vfstest/a/inner") == 0, "a closed file is unlinked too");

    check(mkdir(nullptr, "/tmp/vfstest/full") == 0 && write_file("/tmp/vfstest/full/x", "x"), "a non-empty directory");
    check(unlink(nullptr, "/tmp/vfstest/full") == -SIRIUS_ENOTEMPTY, "a non-empty directory cannot be unlinked");
    check(unlink(nullptr, "/tmp/vfstest/full/x") == 0 && unlink(nullptr, "/tmp/vfstest/full") == 0,
          "once emptied it can");

    // Error paths.
    File* file = nullptr;
    check(open(nullptr, "/tmp/vfstest/nope", SIRIUS_O_RDONLY, file) == -SIRIUS_ENOENT, "a missing file");
    check(open(nullptr, "/tmp/vfstest", SIRIUS_O_WRONLY, file) == -SIRIUS_EISDIR, "a directory opened for writing");
    check(open(nullptr, "/tmp/vfstest/a/b", SIRIUS_O_RDONLY, file) == -SIRIUS_ENOENT, "covered names stay hidden");
    check(write_file("/tmp/vfstest/plain", "p") && open(nullptr, "/tmp/vfstest/plain/x", 0, file) == -SIRIUS_ENOTDIR,
          "a file used as a directory");
    check(open(nullptr, "/tmp/vfstest/plain/", SIRIUS_O_RDONLY, file) == -SIRIUS_ENOTDIR,
          "a trailing slash on a file");

    // Truncation and append through the VFS.
    check(open(nullptr, "/tmp/vfstest/plain", SIRIUS_O_WRONLY | SIRIUS_O_APPEND, file) == 0, "open for append");
    write(file, "q", 1);
    file_close(file);
    check(file_is("/tmp/vfstest/plain", "pq"), "append adds at the end");
    check(write_file("/tmp/vfstest/plain", "r"), "O_TRUNC rewrites");
    check(file_is("/tmp/vfstest/plain", "r"), "the old contents are gone");

    if (g_failures == 0) {
        LOG_INFO("selftest: all %u vfs checks passed", g_checks);
    } else {
        LOG_ERROR("selftest: %u of %u vfs checks FAILED", g_failures, g_checks);
    }
    return g_failures == 0;
}

} // namespace vfs
