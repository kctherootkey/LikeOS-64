/*
 * usbtest — measure USB MSC throughput by writing then reading a 100 MB file
 * in /tmp.  Prints rolling per-chunk speed during the run and final averages.
 *
 * Useful for isolating whether the kernel's USB / FAT32 write path is the
 * bottleneck during slow curl downloads.  If usbtest reports multi-MB/s and
 * curl is at 300 KB/s, the storage stack is fine and the problem is in the
 * network/TLS/userspace pipeline.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define TOTAL_BYTES   (100UL * 1024UL * 1024UL)   /* 100 MB */
#define CHUNK_BYTES   (64UL * 1024UL)             /* 64 KB per write/read */
#define REPORT_BYTES  (4UL * 1024UL * 1024UL)     /* progress line every 4 MB */
#define TEST_PATH     "/tmp/usbtest_100m.bin"

static double seconds_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec  - t0->tv_sec) +
           (double)(now.tv_nsec - t0->tv_nsec) / 1e9;
}

static void print_speed(const char *phase, unsigned long bytes, double secs) {
    double kbps = (bytes / 1024.0) / (secs > 0 ? secs : 1e-9);
    if (kbps >= 1024.0)
        printf("  %s: %lu MB in %.2f s = %.2f MB/s\n",
               phase, bytes / (1024UL * 1024UL), secs, kbps / 1024.0);
    else
        printf("  %s: %lu KB in %.2f s = %.2f KB/s\n",
               phase, bytes / 1024UL, secs, kbps);
}

static void print_progress(const char *phase, unsigned long done,
                           unsigned long total, double chunk_secs,
                           unsigned long chunk_bytes) {
    double chunk_kbps = (chunk_bytes / 1024.0) / (chunk_secs > 0 ? chunk_secs : 1e-9);
    int pct = (int)((done * 100UL) / total);
    if (chunk_kbps >= 1024.0)
        printf("  [%s] %3d%% (%lu / %lu MB)  cur %.2f MB/s\n",
               phase, pct,
               done  / (1024UL * 1024UL),
               total / (1024UL * 1024UL),
               chunk_kbps / 1024.0);
    else
        printf("  [%s] %3d%% (%lu / %lu MB)  cur %.2f KB/s\n",
               phase, pct,
               done  / (1024UL * 1024UL),
               total / (1024UL * 1024UL),
               chunk_kbps);
    fflush(stdout);
}

int main(void) {
    static unsigned char buf[CHUNK_BYTES];

    /* Fill the buffer with a recognisable pattern so a later sha256 of the
     * file can verify content integrity if needed. */
    for (unsigned long i = 0; i < CHUNK_BYTES; i++)
        buf[i] = (unsigned char)(i & 0xFF);

    printf("usbtest: writing %lu MB to %s in %lu KB chunks\n",
           TOTAL_BYTES / (1024UL * 1024UL),
           TEST_PATH,
           CHUNK_BYTES / 1024UL);

    /* ---- WRITE phase ---- */
    unlink(TEST_PATH);
    int fd = open(TEST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "usbtest: open(%s) for write failed: %s\n",
                TEST_PATH, strerror(errno));
        return 1;
    }

    struct timespec wstart;
    clock_gettime(CLOCK_MONOTONIC, &wstart);

    unsigned long written = 0;
    unsigned long since_report = 0;
    struct timespec chunk_t0 = wstart;
    while (written < TOTAL_BYTES) {
        unsigned long want = TOTAL_BYTES - written;
        if (want > CHUNK_BYTES) want = CHUNK_BYTES;
        ssize_t n = write(fd, buf, want);
        if (n < 0) {
            fprintf(stderr, "usbtest: write at offset %lu failed: %s\n",
                    written, strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "usbtest: write returned 0 at offset %lu\n",
                    written);
            break;
        }
        written     += (unsigned long)n;
        since_report += (unsigned long)n;

        if (since_report >= REPORT_BYTES || written >= TOTAL_BYTES) {
            double chunk_secs = seconds_since(&chunk_t0);
            print_progress("WRITE", written, TOTAL_BYTES,
                           chunk_secs, since_report);
            since_report = 0;
            clock_gettime(CLOCK_MONOTONIC, &chunk_t0);
        }
    }

    double wsecs = seconds_since(&wstart);
    close(fd);
    print_speed("write total", written, wsecs);

    /* ---- READ phase ---- */
    printf("\nusbtest: reading %lu MB back from %s\n",
           TOTAL_BYTES / (1024UL * 1024UL), TEST_PATH);

    fd = open(TEST_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "usbtest: open(%s) for read failed: %s\n",
                TEST_PATH, strerror(errno));
        return 1;
    }

    struct timespec rstart;
    clock_gettime(CLOCK_MONOTONIC, &rstart);

    unsigned long read_total = 0;
    since_report = 0;
    chunk_t0 = rstart;
    while (read_total < TOTAL_BYTES) {
        unsigned long want = TOTAL_BYTES - read_total;
        if (want > CHUNK_BYTES) want = CHUNK_BYTES;
        ssize_t n = read(fd, buf, want);
        if (n < 0) {
            fprintf(stderr, "usbtest: read at offset %lu failed: %s\n",
                    read_total, strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "usbtest: short read: got %lu of %lu\n",
                    read_total, TOTAL_BYTES);
            break;
        }
        read_total   += (unsigned long)n;
        since_report += (unsigned long)n;

        if (since_report >= REPORT_BYTES || read_total >= TOTAL_BYTES) {
            double chunk_secs = seconds_since(&chunk_t0);
            print_progress("READ ", read_total, TOTAL_BYTES,
                           chunk_secs, since_report);
            since_report = 0;
            clock_gettime(CLOCK_MONOTONIC, &chunk_t0);
        }
    }

    double rsecs = seconds_since(&rstart);
    close(fd);
    print_speed("read  total", read_total, rsecs);

    /* ---- Summary ---- */
    unlink(TEST_PATH);

    double wavg_kbps = (written    / 1024.0) / (wsecs > 0 ? wsecs : 1e-9);
    double ravg_kbps = (read_total / 1024.0) / (rsecs > 0 ? rsecs : 1e-9);

    printf("\n=== usbtest summary ===\n");
    printf("  WRITE: %lu bytes in %.3f s, avg %.2f KB/s (%.2f MB/s)\n",
           written, wsecs, wavg_kbps, wavg_kbps / 1024.0);
    printf("  READ : %lu bytes in %.3f s, avg %.2f KB/s (%.2f MB/s)\n",
           read_total, rsecs, ravg_kbps, ravg_kbps / 1024.0);
    printf("=======================\n");

    return 0;
}
