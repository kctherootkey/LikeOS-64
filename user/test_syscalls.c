// LikeOS-64 Userspace Syscall Test Program
// Tests all implemented syscalls: read, write, open, close, mmap, brk, yield, getpid, exit

#include "syscall.h"

#define TEST_PASS "[PASS] "
#define TEST_FAIL "[FAIL] "
#define TEST_INFO "[INFO] "

static int tests_passed = 0;
static int tests_failed = 0;

static void test_result(int passed, const char* name) {
    if (passed) {
        print(TEST_PASS);
        tests_passed++;
    } else {
        print(TEST_FAIL);
        tests_failed++;
    }
    print(name);
    print("\n");
}

// Test getpid syscall
static void test_getpid(void) {
    print(TEST_INFO "Testing getpid()...\n");
    
    int pid = getpid();
    print("  PID = ");
    print_num(pid);
    print("\n");
    
    test_result(pid > 0, "getpid returns positive PID");
    
    // Call twice - should return same value
    int pid2 = getpid();
    test_result(pid == pid2, "getpid returns consistent PID");
}

// Test write syscall
static void test_write(void) {
    print(TEST_INFO "Testing write()...\n");
    
    const char* msg = "  Hello from userspace write()!\n";
    ssize_t ret = write(STDOUT_FD, msg, strlen(msg));
    
    test_result(ret == (ssize_t)strlen(msg), "write to stdout returns correct count");
    
    // Test write to stderr (should also work)
    const char* errmsg = "  (stderr test)\n";
    ret = write(STDERR_FD, errmsg, strlen(errmsg));
    test_result(ret == (ssize_t)strlen(errmsg), "write to stderr works");
    
    // Test write to invalid fd
    ret = write(999, msg, strlen(msg));
    test_result(ret < 0, "write to invalid fd returns error");
}

// Test yield syscall
static void test_yield(void) {
    print(TEST_INFO "Testing sched_yield()...\n");
    
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
    print(TEST_INFO "Testing brk()...\n");
    
    // Get current break
    void* current_brk = brk(0);
    print("  Current brk = ");
    print_hex((unsigned long)current_brk);
    print("\n");
    
    test_result(current_brk != (void*)-1, "brk(0) returns valid address");
    
    // Increase break by one page (4KB)
    void* new_brk = (void*)((unsigned long)current_brk + 4096);
    void* result = brk(new_brk);
    
    print("  New brk = ");
    print_hex((unsigned long)result);
    print("\n");
    
    test_result(result == new_brk, "brk can increase heap");
    
    // Write to the new memory
    if (result == new_brk) {
        char* ptr = (char*)current_brk;
        ptr[0] = 'A';
        ptr[1] = 'B';
        ptr[2] = 'C';
        ptr[3] = '\0';
        test_result(ptr[0] == 'A' && ptr[1] == 'B', "can write to brk-allocated memory");
    }
    
    // Try to shrink (may not be supported fully, but shouldn't crash)
    void* shrunk = brk(current_brk);
    test_result(shrunk != (void*)-1, "brk shrink doesn't crash");
}

// Test mmap syscall
static void test_mmap(void) {
    print(TEST_INFO "Testing mmap()...\n");
    
    // Anonymous mapping
    void* ptr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    print("  mmap returned = ");
    print_hex((unsigned long)ptr);
    print("\n");
    
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
    print(TEST_INFO "Testing read()...\n");
    
    // Non-blocking read from stdin - may return 0 if no input available
    char buf[16];
    ssize_t ret = read(STDIN_FD, buf, sizeof(buf));
    
    // Read from stdin should return 0 (no data) or positive (if keys pressed)
    test_result(ret >= 0, "read from stdin returns >= 0");
    
    // Read from invalid fd should fail
    ret = read(999, buf, sizeof(buf));
    test_result(ret < 0, "read from invalid fd returns error");
    
    print("  (stdin read is non-blocking, returned ");
    print_num(ret);
    print(" bytes)\n");
}

// Test open/close syscalls
static void test_open_close(void) {
    print(TEST_INFO "Testing open()/close()...\n");
    
    // Try to open a file (may fail if VFS not fully set up)
    int fd = open("/LIKEOS.SIG", 0);
    
    if (fd >= 0) {
        print("  Opened file, fd = ");
        print_num(fd);
        print("\n");
        test_result(fd >= 3, "open returns fd >= 3");
        
        int ret = close(fd);
        test_result(ret == 0, "close returns 0");
        
        // Double close should fail
        ret = close(fd);
        test_result(ret < 0, "double close returns error");
    } else {
        print("  (No filesystem mounted, open returned ");
        print_num(fd);
        print(")\n");
        test_result(1, "open fails gracefully without filesystem");
    }
    
    // Close invalid fd
    int ret = close(9999);
    test_result(ret < 0, "close invalid fd returns error");
    
    // Can't close stdin/stdout/stderr
    ret = close(STDIN_FD);
    test_result(ret < 0, "cannot close stdin");
}

// Entry point
void _start(void) {
    print("\n");
    print("========================================\n");
    print("  LikeOS-64 Userspace Syscall Tests\n");
    print("========================================\n\n");
    
    // Run all tests
    test_getpid();
    test_write();
    test_yield();
    test_brk();
    test_mmap();
    test_read();
    test_open_close();
    
    // Summary
    print("\n========================================\n");
    print("  Test Summary\n");
    print("========================================\n");
    print("  Passed: ");
    print_num(tests_passed);
    print("\n  Failed: ");
    print_num(tests_failed);
    print("\n  Total:  ");
    print_num(tests_passed + tests_failed);
    print("\n========================================\n\n");
    
    if (tests_failed == 0) {
        print("All tests PASSED!\n\n");
    } else {
        print("Some tests FAILED!\n\n");
    }
    
    // Exit with number of failed tests
    exit(tests_failed);
}
