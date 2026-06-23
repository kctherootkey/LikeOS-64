/* xattrtest - exercise the extended-attribute syscalls on ext4.
 * Usage: xattrtest [path]   (default /tmp/xattrtest.dat)
 * Run, then on the host: sudo e2fsck -fn /dev/sdbN  (must stay clean). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/xattr.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
        if (cond) printf("  PASS: %s\n", (msg)); \
        else { printf("  FAIL: %s (errno=%d)\n", (msg), errno); fails++; } \
    } while (0)

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/tmp/xattrtest.dat";

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("xattrtest: cannot create %s (errno=%d)\n", path, errno); return 1; }
    write(fd, "hello", 5);
    close(fd);
    printf("[xattr] target = %s\n", path);

    char buf[128], list[256];
    ssize_t n;
    int r;

    r = setxattr(path, "user.color", "blue", 4, 0);
    CHECK(r == 0, "setxattr user.color=blue");

    n = getxattr(path, "user.color", NULL, 0);
    CHECK(n == 4, "getxattr size query == 4");

    memset(buf, 0, sizeof(buf));
    n = getxattr(path, "user.color", buf, sizeof(buf));
    CHECK(n == 4 && memcmp(buf, "blue", 4) == 0, "getxattr value == blue");

    r = setxattr(path, "user.x", "1", 1, 0);
    CHECK(r == 0, "setxattr user.x=1");

    r = setxattr(path, "user.color", "red", 3, XATTR_CREATE);
    CHECK(r == -1 && errno == EEXIST, "setxattr CREATE on existing -> EEXIST");

    r = setxattr(path, "user.color", "green", 5, XATTR_REPLACE);
    CHECK(r == 0, "setxattr REPLACE user.color=green");
    memset(buf, 0, sizeof(buf));
    n = getxattr(path, "user.color", buf, sizeof(buf));
    CHECK(n == 5 && memcmp(buf, "green", 5) == 0, "getxattr value == green");

    memset(list, 0, sizeof(list));
    n = listxattr(path, list, sizeof(list));
    CHECK(n > 0, "listxattr returns names");
    int seen_color = 0, seen_x = 0;
    for (char *p = list; n > 0 && p < list + n; p += strlen(p) + 1) {
        printf("    name: %s\n", p);
        if (strcmp(p, "user.color") == 0) seen_color = 1;
        if (strcmp(p, "user.x") == 0)     seen_x = 1;
    }
    CHECK(seen_color && seen_x, "listxattr contains user.color and user.x");

    n = getxattr(path, "user.nope", buf, sizeof(buf));
    CHECK(n == -1 && errno == ENODATA, "getxattr missing -> ENODATA");

    r = removexattr(path, "user.color");
    CHECK(r == 0, "removexattr user.color");
    n = getxattr(path, "user.color", buf, sizeof(buf));
    CHECK(n == -1 && errno == ENODATA, "getxattr removed -> ENODATA");

    r = removexattr(path, "user.color");
    CHECK(r == -1 && errno == ENODATA, "removexattr missing -> ENODATA");

    /* fd-based variant */
    fd = open(path, O_RDWR);
    if (fd >= 0) {
        r = fsetxattr(fd, "user.fd", "yes", 3, 0);
        CHECK(r == 0, "fsetxattr user.fd=yes");
        memset(buf, 0, sizeof(buf));
        n = fgetxattr(fd, "user.fd", buf, sizeof(buf));
        CHECK(n == 3 && memcmp(buf, "yes", 3) == 0, "fgetxattr value == yes");
        close(fd);
    }

    /* Large value: too big for the inode slack, so it spills to an external
     * xattr block (exercises block alloc + per-entry/block hashes + crc32c). */
    {
        char big[600], bigbuf[700];
        for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('A' + (i % 26));
        r = setxattr(path, "user.big", big, sizeof(big), 0);
        CHECK(r == 0, "setxattr large value (external block)");
        memset(bigbuf, 0, sizeof(bigbuf));
        n = getxattr(path, "user.big", bigbuf, sizeof(bigbuf));
        CHECK(n == (ssize_t)sizeof(big) && memcmp(bigbuf, big, sizeof(big)) == 0,
              "getxattr large value matches");
        /* small attrs must still be reachable alongside the block */
        n = getxattr(path, "user.x", buf, sizeof(buf));
        CHECK(n == 1, "small attr still readable with block present");
        r = removexattr(path, "user.big");
        CHECK(r == 0, "removexattr large value (frees the block)");
        n = getxattr(path, "user.big", bigbuf, sizeof(bigbuf));
        CHECK(n == -1 && errno == ENODATA, "getxattr removed large -> ENODATA");
    }

    printf("[xattr] %s (%d failure%s)\n", fails ? "FAILED" : "OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
