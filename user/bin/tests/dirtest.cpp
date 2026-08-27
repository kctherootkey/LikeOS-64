/*
 * Does std::filesystem::directory_iterator work here?
 *
 * WebKit finds a web extension by listing its directory, and it does that with
 * C++ filesystem, not readdir:
 *
 *     WTF/wtf/FileSystem.cpp, listDirectory():
 *         auto entries = std::filesystem::directory_iterator(path, ec);
 *         for (auto it = begin(entries); !ec && it != end; it.increment(ec))
 *             fileNames.append(it->path().filename());
 *
 * Note what happens when that fails: `ec' is set, the loop body never runs,
 * the function returns an EMPTY list, and nobody reports anything.  WebKit
 * then finds zero modules, loads no extension, and luakit's IPC accept() waits
 * for a connection that will never come -- which is exactly the observed
 * symptom, right down to there being nothing in any log.
 *
 * MiniBrowser renders the same page luakit leaves blank, and the difference
 * between them is precisely this: MiniBrowser loads no web extension.
 *
 * So: compare plain POSIX readdir against std::filesystem on the same
 * directory.  If readdir sees luakit.so and directory_iterator does not, the
 * fault is in libstdc++'s filesystem layer sitting on this libc -- and the
 * error_code printed below says which call underneath it failed.
 */
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <filesystem>
#include <system_error>

static int posix_listing(const char *path)
{
    printf("1. POSIX opendir/readdir on %s\n", path);
    DIR *d = opendir(path);
    if (!d) {
        printf("   opendir FAILED: %s\n", strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        printf("     %-28s d_type=%u\n", e->d_name, (unsigned)e->d_type);
        n++;
    }
    closedir(d);
    printf("   %d entries\n\n", n);
    return n;
}

static int std_listing(const char *path)
{
    printf("2. std::filesystem::directory_iterator on %s\n", path);
    std::error_code ec;
    auto entries = std::filesystem::directory_iterator(path, ec);
    if (ec) {
        printf("   CONSTRUCTION FAILED: %s (value %d)\n", ec.message().c_str(), ec.value());
        printf("   ^^ this is the bug: WebKit gets an empty list and says nothing\n\n");
        return -1;
    }
    int n = 0;
    for (auto it = std::filesystem::begin(entries), end = std::filesystem::end(entries);
         !ec && it != end; it.increment(ec)) {
        printf("     %s\n", it->path().filename().string().c_str());
        n++;
    }
    if (ec) {
        printf("   ITERATION FAILED after %d: %s (value %d)\n", n, ec.message().c_str(), ec.value());
        return -1;
    }
    printf("   %d entries\n\n", n);
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "/usr/lib/luakit";

    int a = posix_listing(path);
    int b = std_listing(path);

    printf("3. the specific question WebKit asks: is there a .so here?\n");
    printf("   readdir says             %s\n", a > 0 ? "yes, it can list the directory" : "NO");
    printf("   std::filesystem says     %s\n", b > 0 ? "yes" : "NO -- extension will never load");

    if (a > 0 && b <= 0) {
        printf("\nreaddir works and std::filesystem does not.  That is why luakit is\n"
               "blank and MiniBrowser is not: only luakit loads a web extension.\n");
        return 1;
    }
    printf("\n%s\n", (a > 0 && b > 0) ? "both work -- the extension search is not the bug"
                                      : "neither works -- the directory itself is the problem");
    return (a > 0 && b > 0) ? 0 : 1;
}
