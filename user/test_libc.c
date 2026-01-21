#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

static void test_pass(const char* name) {
    tests_passed++;
    printf("  [PASS] %s\n", name);
}

static void test_fail(const char* name) {
    tests_failed++;
    printf("  [FAIL] %s\n", name);
}

static void test_result(const char* name, int condition) {
    if (condition) {
        test_pass(name);
    } else {
        test_fail(name);
    }
}

int main(int argc, char** argv) {
    printf("\n========================================\n");
    printf("  LikeOS-64 Libc Tests\n");
    printf("========================================\n\n");

    // ========================================
    // Test: printf
    // ========================================
    printf("[TEST] printf()\n");
    printf("  Hello from userland libc!\n");
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    test_pass("printf basic output");

    // ========================================
    // Test: malloc/free
    // ========================================
    printf("\n[TEST] malloc/free\n");
    char* buf = malloc(100);
    test_result("malloc(100) returns non-NULL", buf != NULL);
    
    if (buf) {
        strcpy(buf, "Hello, ");
        strcat(buf, "World!");
        size_t len = strlen(buf);
        printf("  String: %s (len=%zu)\n", buf, len);
        test_result("strcpy/strcat/strlen", len == 13);
        free(buf);
        test_pass("free() completed");
    }

    // ========================================
    // Test: atoi
    // ========================================
    printf("\n[TEST] atoi()\n");
    test_result("atoi(\"42\") == 42", atoi("42") == 42);
    test_result("atoi(\"-123\") == -123", atoi("-123") == -123);
    test_result("atoi(\"0\") == 0", atoi("0") == 0);
    printf("  atoi(\"777\") = %d\n", atoi("777"));
    test_result("atoi(\"777\") == 777", atoi("777") == 777);

    // ========================================
    // Test: printf format specifiers
    // ========================================
    printf("\n[TEST] printf format specifiers\n");
    char fmtbuf[64];
    sprintf(fmtbuf, "0x%x %d %s", 0xCAFE, 12345, "test");
    test_result("sprintf format specifiers", strcmp(fmtbuf, "0xcafe 12345 test") == 0);

    // ========================================
    // Test: write syscall
    // ========================================
    printf("\n[TEST] write() syscall\n");
    const char* msg = "  Direct write syscall!\n";
    ssize_t written = write(1, msg, strlen(msg));
    test_result("write() returns correct count", written == (ssize_t)strlen(msg));

    // ========================================
    // Test: getpid
    // ========================================
    printf("\n[TEST] getpid()\n");
    pid_t pid = getpid();
    printf("  PID: %d\n", pid);
    test_result("getpid() returns positive value", pid > 0);

    // ========================================
    // Test: FILE* functions - fopen/fread/fclose
    // ========================================
    printf("\n[TEST] FILE* functions\n");
    FILE* fp = fopen("/HELLO.TXT", "r");
    test_result("fopen(\"/HELLO.TXT\", \"r\") succeeds", fp != NULL);
    
    if (fp) {
        char readbuf[64];
        memset(readbuf, 0, sizeof(readbuf));
        size_t nread = fread(readbuf, 1, sizeof(readbuf) - 1, fp);
        printf("  fread() returned %zu bytes\n", nread);
        test_result("fread() returns > 0 bytes", nread > 0);
        
        if (nread > 0 && readbuf[nread-1] == '\n') readbuf[nread-1] = '\0';
        printf("  Contents: \"%s\"\n", readbuf);
        
        int rc = fclose(fp);
        test_result("fclose() returns 0", rc == 0);
    }
    
    // Test fopen with non-existent file
    fp = fopen("/NONEXISTENT.TXT", "r");
    test_result("fopen(non-existent) returns NULL", fp == NULL);

    // ========================================
    // Test: fputs/puts
    // ========================================
    printf("\n[TEST] fputs/puts\n");
    int fputs_rc = fputs("  fputs output\n", stdout);
    test_result("fputs() returns >= 0", fputs_rc >= 0);
    puts("  puts output");
    test_pass("puts() completed");

    // ========================================
    // Test: fprintf
    // ========================================
    printf("\n[TEST] fprintf\n");
    int fprintf_rc = fprintf(stdout, "  fprintf: int=%d, hex=0x%x\n", 42, 0xCAFE);
    test_result("fprintf() returns > 0", fprintf_rc > 0);

    // ========================================
    // Test: putchar/fputc
    // ========================================
    printf("\n[TEST] putchar/fputc\n");
    printf("  Characters: ");
    int pc = putchar('A');
    test_result("putchar('A') returns 'A'", pc == 'A');
    pc = fputc('B', stdout);
    test_result("fputc('B') returns 'B'", pc == 'B');
    putchar('\n');

    // ========================================
    // Test: sprintf/snprintf
    // ========================================
    printf("\n[TEST] sprintf/snprintf\n");
    char sprbuf[64];
    int len = sprintf(sprbuf, "Value: %d", 12345);
    test_result("sprintf returns correct length", len == 12);
    test_result("sprintf produces correct string", strcmp(sprbuf, "Value: 12345") == 0);
    
    len = snprintf(sprbuf, 10, "Long string that will be truncated");
    test_result("snprintf truncates correctly", strlen(sprbuf) == 9);

    // ========================================
    // Test: fseek/ftell/rewind
    // ========================================
    printf("\n[TEST] fseek/ftell/rewind\n");
    fp = fopen("/HELLO.TXT", "r");
    if (fp) {
        char seekbuf[32];
        memset(seekbuf, 0, sizeof(seekbuf));
        fread(seekbuf, 1, 5, fp);
        
        long pos = ftell(fp);
        printf("  ftell() after read 5 bytes = %ld\n", pos);
        test_result("ftell() returns 5 after reading 5 bytes", pos == 5);
        
        fseek(fp, 0, 0); // SEEK_SET
        pos = ftell(fp);
        test_result("fseek(0, SEEK_SET) resets to 0", pos == 0);
        
        rewind(fp);
        pos = ftell(fp);
        test_result("rewind() resets to 0", pos == 0);
        
        fclose(fp);
    } else {
        test_fail("fseek/ftell test - fopen failed");
    }

    // ========================================
    // Test: getenv/setenv/unsetenv
    // ========================================
    printf("\n[TEST] getenv/setenv/unsetenv\n");
    char* val = getenv("TEST_VAR");
    test_result("getenv() returns NULL for unset var", val == NULL);
    
    int rc = setenv("TEST_VAR", "hello_world", 1);
    test_result("setenv() returns 0", rc == 0);
    
    val = getenv("TEST_VAR");
    test_result("getenv() returns set value", val != NULL && strcmp(val, "hello_world") == 0);
    
    // Test setenv with overwrite=0
    rc = setenv("TEST_VAR", "new_value", 0);
    val = getenv("TEST_VAR");
    test_result("setenv with overwrite=0 keeps old value", val != NULL && strcmp(val, "hello_world") == 0);
    
    // Test unsetenv
    rc = unsetenv("TEST_VAR");
    val = getenv("TEST_VAR");
    test_result("unsetenv() clears variable", val == NULL);

    // ========================================
    // Test: fork/wait/getpid/getppid
    // ========================================
    printf("\n[TEST] fork/wait/getpid/getppid\n");
    pid_t my_pid = getpid();
    pid_t my_ppid = getppid();
    printf("  PID=%d, PPID=%d calling fork()...\n", my_pid, my_ppid);
    
    pid_t child_pid = fork();
    printf("  fork() returned %d in process %d\n", child_pid, getpid());
    
    if (child_pid < 0) {
        test_fail("fork() failed");
    } else if (child_pid == 0) {
        // Child process
        printf("  [CHILD] I am the child, my PID = %d, parent = %d\n", getpid(), getppid());
        printf("  [CHILD] Exiting with code 42\n");
        _exit(42);
    } else {
        // Parent process
        printf("  [PARENT] fork() returned child PID = %d\n", child_pid);
        test_result("fork() returns positive child PID", child_pid > 0);
        
        // Wait for child
        int status = 0;
        pid_t waited = waitpid(child_pid, &status, 0);
        printf("  [PARENT] waitpid(%d, ...) returned %d\n", child_pid, waited);
        test_result("waitpid() returns child PID", waited == child_pid);
        
        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            printf("  [PARENT] Child exited with status %d (raw status=0x%x)\n", exit_status, status);
            test_result("Child exit status is 42", exit_status == 42);
        } else {
            printf("  [PARENT] Child did not exit normally (status=0x%x)\n", status);
            test_fail("Child did not exit normally");
        }
    }

    // ========================================
    // Test: execve (via fork)
    // ========================================
    printf("\n[TEST] execve() via fork\n");
    pid_t exec_child = fork();
    if (exec_child < 0) {
        test_fail("fork() for execve failed");
    } else if (exec_child == 0) {
        char* exec_argv[] = { "/hello", NULL };
        char* exec_envp[] = { NULL };
        execve("/hello", exec_argv, exec_envp);
        printf("  [CHILD] execve failed: errno=%d\n", errno);
        _exit(1);
    } else {
        int status = 0;
        pid_t waited = waitpid(exec_child, &status, 0);
        test_result("waitpid() returns execve child PID", waited == exec_child);
        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            test_result("execve child exited 0", exit_status == 0);
        } else {
            test_fail("execve child did not exit normally");
        }
    }

    // ========================================
    // Test: pipe
    // ========================================
    printf("\n[TEST] pipe()\n");
    int fds[2];
    int prc = pipe(fds);
    test_result("pipe() returns 0", prc == 0);
    if (prc == 0) {
        const char* pipemsg = "pipe works";
        ssize_t pwr = write(fds[1], pipemsg, strlen(pipemsg));
        test_result("pipe write returns full length", pwr == (ssize_t)strlen(pipemsg));

        char pipebuf[32];
        memset(pipebuf, 0, sizeof(pipebuf));
        ssize_t prd = read(fds[0], pipebuf, sizeof(pipebuf) - 1);
        test_result("pipe read returns full length", prd == (ssize_t)strlen(pipemsg));
        test_result("pipe read matches data", prd > 0 && strcmp(pipebuf, pipemsg) == 0);

        close(fds[0]);
        close(fds[1]);
    }

    // ========================================
    // Test: munmap
    // ========================================
    printf("\n[TEST] munmap()\n");
    size_t map_len = 8192;
    void* map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap() returns non-NULL", map != MAP_FAILED);
    if (map != MAP_FAILED) {
        unsigned char* p = (unsigned char*)map;
        for (size_t i = 0; i < map_len; i++) {
            p[i] = (unsigned char)(i & 0xFF);
        }
        int mrc = munmap(map, map_len);
        test_result("munmap() returns 0", mrc == 0);
    }


    // ========================================
    // Test: dup/dup2
    // ========================================
    printf("\n[TEST] dup/dup2\n");
    int newfd = dup(1);  // Dup stdout
    printf("  dup(1) returned %d\n", newfd);
    test_result("dup(1) returns valid fd", newfd >= 0);
    
    if (newfd >= 0) {
        const char* dupmsg = "  Write via duped fd\n";
        ssize_t wr = write(newfd, dupmsg, strlen(dupmsg));
        test_result("write to duped fd succeeds", wr > 0);
        close(newfd);
    }

    // ========================================
    // Summary
    // ========================================
    printf("\n========================================\n");
    printf("  TEST SUMMARY\n");
    printf("========================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);
    printf("========================================\n");
    
    if (tests_failed == 0) {
        printf("  ALL TESTS PASSED!\n");
    } else {
        printf("  SOME TESTS FAILED!\n");
    }
    printf("========================================\n");
    
    return tests_failed > 0 ? 1 : 0;
}
