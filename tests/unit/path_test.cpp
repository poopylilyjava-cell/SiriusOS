// Host-side tests for path component parsing.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fs/path.hpp>

static int g_failures = 0;

static void expect(bool condition, const std::string& what) {
    if (condition) return;
    printf("  FAIL  %s\n", what.c_str());
    g_failures++;
}

static std::vector<std::string> split(const char* p, bool* too_long = nullptr) {
    std::vector<std::string> out;
    path::Components components(p);
    char name[path::kNameMax + 1];
    for (;;) {
        const auto result = components.next(name);
        if (result == path::Components::Result::End) break;
        if (result == path::Components::Result::NameTooLong) { if (too_long) *too_long = true; break; }
        out.push_back(name);
    }
    return out;
}

static void expect_split(const char* p, std::vector<std::string> expected) {
    expect(split(p) == expected, std::string("split \"") + p + "\"");
}

int main() {
    printf("components:\n");
    expect_split("/", {});
    expect_split("", {});
    expect_split("/bin/init", {"bin", "init"});
    expect_split("bin/init", {"bin", "init"});
    expect_split("//bin///init//", {"bin", "init"});
    expect_split("/./bin/./init/.", {"bin", "init"});
    expect_split("/a/../b", {"a", "..", "b"});
    expect_split("...", {"..."});
    expect_split(".hidden/.x", {".hidden", ".x"});

    expect(path::Components("/x").absolute(), "a leading slash is absolute");
    expect(!path::Components("x/y").absolute(), "no leading slash is relative");

    printf("name length:\n");
    {
        std::string ok(path::kNameMax, 'a');
        expect(split(("/" + ok).c_str()).size() == 1, "a name of exactly kNameMax is accepted");
        bool too_long = false;
        split(("/" + ok + "b/c").c_str(), &too_long);
        expect(too_long, "a name one byte over kNameMax is rejected");
    }

    printf("at_end:\n");
    {
        path::Components c("/usr/lib/./");
        char name[path::kNameMax + 1];
        c.next(name);
        expect(!c.at_end(), "more components follow /usr");
        c.next(name);
        expect(c.at_end(), "a trailing ./ leaves nothing after lib");

        path::Components d("a/./b");
        d.next(name);
        expect(!d.at_end(), "./b still has b after it");
    }

    expect(path::Components::has_trailing_slash("/tmp/"), "trailing slash is seen");
    expect(!path::Components::has_trailing_slash("/tmp"), "no trailing slash");

    printf("valid names:\n");
    expect(path::valid_new_name("file.txt"), "an ordinary name is valid");
    expect(path::valid_new_name("..."), "three dots is an ordinary name");
    expect(!path::valid_new_name(""), "an empty name is invalid");
    expect(!path::valid_new_name("."), "\".\" cannot be created");
    expect(!path::valid_new_name(".."), "\"..\" cannot be created");
    expect(!path::valid_new_name("a/b"), "a slash is not part of a name");
    expect(!path::valid_new_name(std::string(path::kNameMax + 1, 'x').c_str()), "an overlong name is invalid");

    if (g_failures != 0) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    printf("\nall path tests passed\n");
    return 0;
}
