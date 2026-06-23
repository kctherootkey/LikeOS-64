/* ext4test - exercise ext4-specific write paths: extended attributes and the
 * hash-indexed (htree) directory code.
 * Usage: ext4test [base]   (default base /tmp/ext4test)
 *   - xattr tests run on   <base>.dat
 *   - htree tests run in   <base>.d/
 * Run, then on the host: sudo e2fsck -fn /dev/sdbN  (must stay clean). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
        if (cond) printf("  PASS: %s\n", (msg)); \
        else { printf("  FAIL: %s (errno=%d)\n", (msg), errno); fails++; } \
    } while (0)

/* ---------------------------------------------------------------- xattr ---- */
static void xattr_tests(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("ext4test: cannot create %s (errno=%d)\n", path, errno); fails++; return; }
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
        n = getxattr(path, "user.x", buf, sizeof(buf));
        CHECK(n == 1, "small attr still readable with block present");
        r = removexattr(path, "user.big");
        CHECK(r == 0, "removexattr large value (frees the block)");
        n = getxattr(path, "user.big", bigbuf, sizeof(bigbuf));
        CHECK(n == -1 && errno == ENODATA, "getxattr removed large -> ENODATA");
    }
    unlink(path);
}

/* ---------------------------------------------------------------- htree ---- */
/* Names long enough that ~145 fit per 4K block, so a few hundred files force
 * the linear->htree conversion (at the 1->2 block transition) plus several leaf
 * splits and dx index-entry inserts.  The dirhash decides each name's leaf; the
 * on-disk index/checksums are the e2fsck gate, so run e2fsck -fn after. */
#define HT_N 600
static void ht_name(char *out, size_t cap, const char *dir, int i) {
    snprintf(out, cap, "%s/htree_entry_%06d", dir, i);
}

static int count_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int c = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        c++;
    }
    closedir(d);
    return c;
}

static void htree_tests(const char *dir) {
    char path[256];
    printf("[htree] dir = %s  (%d entries)\n", dir, HT_N);

    /* Start clean (also removes leftovers from a previous run). */
    for (int i = 0; i < HT_N; i++) { ht_name(path, sizeof(path), dir, i); unlink(path); }
    rmdir(dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        printf("  FAIL: mkdir %s (errno=%d)\n", dir, errno); fails++; return;
    }

    /* Create HT_N entries -> forces conversion to htree + multiple leaf splits. */
    int created = 0;
    for (int i = 0; i < HT_N; i++) {
        ht_name(path, sizeof(path), dir, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { created++; close(fd); }
    }
    CHECK(created == HT_N, "created all entries (linear->htree + leaf splits)");

    /* readdir must still see every entry (linear leaf scan across all blocks). */
    CHECK(count_entries(dir) == HT_N, "readdir returns all entries");

    /* Every entry must be reachable by name (lookup over the grown directory). */
    int found = 0;
    for (int i = 0; i < HT_N; i++) {
        ht_name(path, sizeof(path), dir, i);
        struct stat st;
        if (stat(path, &st) == 0) found++;
    }
    CHECK(found == HT_N, "stat finds every entry by name");

    /* Content integrity across the growth: write/verify a few files' data. */
    {
        int probe[3] = { 7, HT_N / 2, HT_N - 1 }; int ok = 1;
        for (int k = 0; k < 3; k++) {
            ht_name(path, sizeof(path), dir, probe[k]);
            int fd = open(path, O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) { ok = 0; break; }
            write(fd, &probe[k], sizeof(int)); close(fd);
        }
        for (int k = 0; k < 3 && ok; k++) {
            ht_name(path, sizeof(path), dir, probe[k]);
            int fd = open(path, O_RDONLY), v = -1;
            if (fd < 0 || read(fd, &v, sizeof(int)) != (ssize_t)sizeof(int) || v != probe[k]) ok = 0;
            if (fd >= 0) close(fd);
        }
        CHECK(ok, "file contents intact after htree growth");
    }

    /* Delete the odd-numbered entries (dir_del on htree leaves). */
    int deleted = 0;
    for (int i = 1; i < HT_N; i += 2) {
        ht_name(path, sizeof(path), dir, i);
        if (unlink(path) == 0) deleted++;
    }
    CHECK(deleted == HT_N / 2, "deleted odd-numbered entries");

    /* Evens must remain, odds must be gone. */
    int parity_ok = 1;
    for (int i = 0; i < HT_N; i++) {
        ht_name(path, sizeof(path), dir, i);
        struct stat st; int exists = (stat(path, &st) == 0);
        if ((i % 2 == 0) != exists) { parity_ok = 0; break; }
    }
    CHECK(parity_ok, "after delete: evens present, odds gone");
    CHECK(count_entries(dir) == HT_N - HT_N / 2, "readdir count matches after deletes");

    /* Re-add a few odds (add into an already-indexed directory): i = 1,3,5,7,9. */
    int readded = 0;
    for (int i = 1; i < 10; i += 2) {
        ht_name(path, sizeof(path), dir, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { readded++; close(fd); }
    }
    int reok = (readded == 5);
    for (int i = 1; i < 10 && reok; i += 2) {
        ht_name(path, sizeof(path), dir, i);
        struct stat st; if (stat(path, &st) != 0) reok = 0;
    }
    CHECK(reok, "re-added entries reachable in indexed dir");

    /* Cleanup. */
    for (int i = 0; i < HT_N; i++) { ht_name(path, sizeof(path), dir, i); unlink(path); }
    CHECK(rmdir(dir) == 0, "rmdir emptied htree directory");
}

int main(int argc, char **argv) {
    const char *base = (argc > 1) ? argv[1] : "/tmp/ext4test";
    char datpath[256], dirpath[256];
    snprintf(datpath, sizeof(datpath), "%s.dat", base);
    snprintf(dirpath, sizeof(dirpath), "%s.d", base);

    xattr_tests(datpath);
    htree_tests(dirpath);

    printf("[ext4test] %s (%d failure%s)\n", fails ? "FAILED" : "OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
