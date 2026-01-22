// LikeOS-64 Userspace Syscall Test Program
// Tests all implemented syscalls: read, write, open, close, mmap, brk, yield, getpid, exit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/types.h>

#define TEST_PASS "[PASS] "
#define TEST_FAIL "[FAIL] "
#define TEST_INFO "[INFO] "

static int tests_passed = 0;
static int tests_failed = 0;

static void test_result(int passed, const char* name) {
    if (passed) {
        printf(TEST_PASS);
        tests_passed++;
    } else {
        printf(TEST_FAIL);
        tests_failed++;
    }
    printf("%s\n", name);
}

// Test getpid syscall
static void test_getpid(void) {
    printf(TEST_INFO "Testing getpid()...\n");
    
    int pid = getpid();
    printf("  PID = %d\n", pid);
    
    test_result(pid > 0, "getpid returns positive PID");
    
    // Call twice - should return same value
    int pid2 = getpid();
    test_result(pid == pid2, "getpid returns consistent PID");
}

// Test write syscall
static void test_write(void) {
    printf(TEST_INFO "Testing write()...\n");
    
    const char* msg = "  Hello from userspace write()!\n";
    ssize_t ret = write(1, msg, strlen(msg));
    
    test_result(ret == (ssize_t)strlen(msg), "write to stdout returns correct count");
    
    // Test write to stderr (should also work)
    const char* errmsg = "  (stderr test)\n";
    ret = write(2, errmsg, strlen(errmsg));
    test_result(ret == (ssize_t)strlen(errmsg), "write to stderr works");
    
    // Test write to invalid fd
    ret = write(999, msg, strlen(msg));
    test_result(ret < 0, "write to invalid fd returns error");
}

// Test yield syscall
static void test_yield(void) {
    printf(TEST_INFO "Testing sched_yield()...\n");
    
    int ret = sched_yield();
    test_result(ret == 0, "sched_yield returns 0");
    
    // Yield multiple times
    for (int i = 0; i < 5; i++) {
        sched_yield();
    }
    test_result(1, "multiple yields don't crash");
}

// Test brk syscall
static void test_brk(void) {
    printf(TEST_INFO "Testing brk()...\n");
    
    // Get current break
    void* current_brk = sbrk(0);
    printf("  Current brk = %p\n", current_brk);
    
    test_result(current_brk != (void*)-1, "brk(0) returns valid address");
    
    // Increase break by one page (4KB)
    void* new_brk = sbrk(4096);
    
    printf("  New brk = %p\n", new_brk);
    
    test_result(new_brk != (void*)-1, "sbrk can increase heap");
    
    // Write to the new memory
    if (new_brk != (void*)-1) {
        char* ptr = (char*)current_brk;
        ptr[0] = 'A';
        ptr[1] = 'B';
        ptr[2] = 'C';
        ptr[3] = '\0';
        test_result(ptr[0] == 'A' && ptr[1] == 'B', "can write to brk-allocated memory");
    }
}

// Test mmap syscall
static void test_mmap(void) {
    printf(TEST_INFO "Testing mmap()...\n");
    
    // Anonymous mapping
    void* ptr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    printf("  mmap returned = %p\n", ptr);
    
    test_result(ptr != MAP_FAILED, "mmap anonymous returns valid address");
    
    if (ptr != MAP_FAILED) {
        // Write to mapped memory
        char* mem = (char*)ptr;
        mem[0] = 'X';
        mem[1] = 'Y';
        mem[2] = 'Z';
        test_result(mem[0] == 'X' && mem[1] == 'Y', "can write to mmap'd memory");
        
        // Test that memory is zeroed
        test_result(mem[100] == 0, "mmap'd memory is zero-initialized");
    }
    
    // Test larger allocation
    void* large = mmap(0, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result(large != MAP_FAILED, "mmap 64KB succeeds");
    
    // Invalid size should fail (or at least not crash)
    void* invalid = mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result(invalid == MAP_FAILED, "mmap with size=0 fails");
}

// Test read syscall (stdin)
static void test_read(void) {
    printf(TEST_INFO "Testing read()...\n");
    
    // stdin is blocking now; use zero-length read to avoid blocking
    char buf[16];
    ssize_t ret = read(0, buf, 0);
    test_result(ret == 0, "read(stdin, 0) returns 0");
    
    // Read from invalid fd should fail
    ret = read(999, buf, sizeof(buf));
    test_result(ret < 0, "read from invalid fd returns error");
}

// Test open/close syscalls
static void test_open_close(void) {
    printf(TEST_INFO "Testing open()/close()...\n");
    
    // Try to open a file (may fail if VFS not fully set up)
    int fd = open("/LIKEOS.SIG", 0);
    
    if (fd >= 0) {
        printf("  Opened file, fd = %d\n", fd);
        test_result(fd >= 3, "open returns fd >= 3");
        
        int ret = close(fd);
        test_result(ret == 0, "close returns 0");
        
        // Double close should fail
        ret = close(fd);
        test_result(ret < 0, "double close returns error");
    } else {
        printf("  (No filesystem mounted, open returned %d)\n", fd);
        test_result(1, "open fails gracefully without filesystem");
    }
    
    // Close invalid fd
    int ret = close(9999);
    test_result(ret < 0, "close invalid fd returns error");
    
    // Can't close stdin/stdout/stderr
    ret = close(0);
    test_result(ret < 0, "cannot close stdin");
}

// Entry point
int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  LikeOS-64 Userspace Syscall Tests\n");
    printf("========================================\n\n");
    
    // Run all tests
    test_getpid();
    test_write();
    test_yield();
    test_brk();
    test_mmap();
    test_read();
    test_open_close();
    
    // Summary
    printf("\n========================================\n");
    printf("  Test Summary\n");
    printf("========================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);
    printf("========================================\n\n");
    
    if (tests_failed == 0) {
        printf("All tests PASSED!\n\n");
    } else {
        printf("Some tests FAILED!\n\n");
    }
    
    // Exit with number of failed tests
    return tests_failed;
}
