#pragma once
#include <klib/types.hpp>

// Path parsing, kept free of the VFS so the host tests can reach every edge:
// repeated slashes, trailing slashes, ".", "..", and names that are too long.
namespace path {

constexpr usize kNameMax = 255;    // bytes in one component, without the NUL
constexpr usize kPathMax = 1024;   // bytes in a whole path, without the NUL

// Splits a path into its components. Empty components (from "//" or a
// trailing "/") and "." are skipped; ".." is returned like any other name and
// resolved by the walker, which knows where mount points are.
class Components {
public:
    enum class Result { Name, End, NameTooLong };

    explicit Components(const char* path) : m_cursor(path), m_absolute(path[0] == '/') {}

    bool absolute() const { return m_absolute; }

    // Copy the next component into `name` (kNameMax + 1 bytes).
    Result next(char* name) {
        for (;;) {
            while (*m_cursor == '/') m_cursor++;
            if (*m_cursor == '\0') return Result::End;

            usize length = 0;
            while (m_cursor[length] && m_cursor[length] != '/') length++;
            const char* start = m_cursor;
            m_cursor += length;

            if (length == 1 && start[0] == '.') continue;
            if (length > kNameMax) return Result::NameTooLong;
            for (usize i = 0; i < length; i++) name[i] = start[i];
            name[length] = '\0';
            return Result::Name;
        }
    }

    // True when no component follows the one most recently returned. Used to
    // stop one short of the end, at the parent of a file being created.
    bool at_end() const {
        const char* p = m_cursor;
        for (;;) {
            while (*p == '/') p++;
            if (*p == '\0') return true;
            if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) { p++; continue; }
            return false;
        }
    }

    // True if the path ended with a slash, which demands a directory.
    static bool has_trailing_slash(const char* path) {
        usize length = 0;
        while (path[length]) length++;
        return length > 0 && path[length - 1] == '/';
    }

private:
    const char* m_cursor;
    bool m_absolute;
};

// A name that may be created in a directory: not empty, not "." or "..",
// no slash, no NUL in the middle, within kNameMax.
inline bool valid_new_name(const char* name) {
    usize length = 0;
    for (; name[length]; length++) if (name[length] == '/') return false;
    if (length == 0 || length > kNameMax) return false;
    if (name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.'))) return false;
    return true;
}

} // namespace path
