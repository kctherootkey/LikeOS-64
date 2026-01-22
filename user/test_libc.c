#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

static volatile int g_sigusr1_hit = 0;
static volatile int g_sigusr2_hit = 0;
static volatile int g_last_signal = 0;
static volatile int g_signal_hits = 0;
static volatile int g_sigalrm_hit = 0;

static void handle_sigusr1(int sig) {
    (void)sig;
    g_sigusr1_hit = 1;
}

static void handle_sigusr2(int sig) {
    (void)sig;
    g_sigusr2_hit = 1;
}

static void handle_generic(int sig) {
    g_last_signal = sig;
    g_signal_hits++;
}

static void handle_sigalrm(int sig) {
    (void)sig;
    g_sigalrm_hit = 1;
}

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
    // Test: execv/execvp (via fork)
    // ========================================
    printf("\n[TEST] execv/execvp via fork\n");
    pid_t execv_child = fork();
    if (execv_child < 0) {
        test_fail("fork() for execv failed");
    } else if (execv_child == 0) {
        char* exec_argv[] = { "/hello", NULL };
        execv("/hello", exec_argv);
        _exit(1);
    } else {
        int status = 0;
        pid_t waited = waitpid(execv_child, &status, 0);
        test_result("waitpid() returns execv child PID", waited == execv_child);
        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            test_result("execv child exited 0", exit_status == 0);
        } else {
            test_fail("execv child did not exit normally");
        }
    }

    // Ensure PATH for execvp
    setenv("PATH", "/", 1);
    pid_t execvp_child = fork();
    if (execvp_child < 0) {
        test_fail("fork() for execvp failed");
    } else if (execvp_child == 0) {
        char* exec_argv[] = { "hello", NULL };
        execvp("hello", exec_argv);
        _exit(1);
    } else {
        int status = 0;
        pid_t waited = waitpid(execvp_child, &status, 0);
        test_result("waitpid() returns execvp child PID", waited == execvp_child);
        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            test_result("execvp child exited 0", exit_status == 0);
        } else {
            test_fail("execvp child did not exit normally");
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
    // Test: stat/access/chdir/getcwd
    // ========================================
    printf("\n[TEST] stat/access/chdir/getcwd\n");
    struct stat st;
    int sret = stat("/HELLO.TXT", &st);
    test_result("stat(/HELLO.TXT) succeeds", sret == 0);
    if (sret == 0) {
        test_result("stat size > 0", st.st_size > 0);
    }
    test_result("access(/HELLO.TXT) succeeds", access("/HELLO.TXT", R_OK) == 0);
    char cwd[64];
    char* cwdret = getcwd(cwd, sizeof(cwd));
    test_result("getcwd returns non-NULL", cwdret != NULL);
    test_result("chdir('/') succeeds", chdir("/") == 0);
    cwdret = getcwd(cwd, sizeof(cwd));
    test_result("getcwd after chdir", cwdret != NULL);

    // ========================================
    // Test: uid/gid and time
    // ========================================
    printf("\n[TEST] uid/gid/time\n");
    test_result("getuid returns 0", getuid() == 0);
    test_result("getgid returns 0", getgid() == 0);
    struct timeval tv;
    test_result("gettimeofday succeeds", gettimeofday(&tv, NULL) == 0);
    test_result("gettimeofday tv_sec non-negative", tv.tv_sec >= 0);
    time_t tnow = time(NULL);
    test_result("time returns non-negative", tnow >= 0);
    test_result("time >= gettimeofday", tnow >= (time_t)tv.tv_sec);

    // ========================================
    // Test: gethostname/uname
    // ========================================
    printf("\n[TEST] gethostname/uname\n");
    char host[64];
    test_result("gethostname succeeds", gethostname(host, sizeof(host)) == 0);
    test_result("gethostname non-empty", host[0] != '\0');
    printf("  hostname: %s\n", host);
    struct utsname un;
    test_result("uname succeeds", uname(&un) == 0);
    test_result("uname sysname non-empty", un.sysname[0] != '\0');
    printf("  uname: sysname=%s nodename=%s release=%s version=%s machine=%s\n",
        un.sysname, un.nodename, un.release, un.version, un.machine);

    // ========================================
    // Test: file write/create/truncate/append
    // ========================================
    printf("\n[TEST] file write (create/truncate/append)\n");
    const char* wpath = "/WRITE.TXT";
    const char* wmsg1 = "HelloWrite";
    int wfd = open(wpath, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("open(O_CREAT|O_TRUNC|O_WRONLY) succeeds", wfd >= 0);
    if (wfd >= 0) {
        ssize_t w1 = write(wfd, wmsg1, strlen(wmsg1));
        test_result("write initial data", w1 == (ssize_t)strlen(wmsg1));
        close(wfd);
    }
    // read back
    wfd = open(wpath, O_RDONLY);
    test_result("open(O_RDONLY) succeeds", wfd >= 0);
    if (wfd >= 0) {
        char rbuf[64];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t r1 = read(wfd, rbuf, sizeof(rbuf) - 1);
        test_result("read back initial data", r1 == (ssize_t)strlen(wmsg1) && strcmp(rbuf, wmsg1) == 0);
        close(wfd);
    }
    // append
    const char* wmsg2 = "+APPEND";
    wfd = open(wpath, O_APPEND | O_WRONLY);
    test_result("open(O_APPEND|O_WRONLY) succeeds", wfd >= 0);
    if (wfd >= 0) {
        ssize_t w2 = write(wfd, wmsg2, strlen(wmsg2));
        test_result("append write", w2 == (ssize_t)strlen(wmsg2));
        close(wfd);
    }
    // read back combined
    wfd = open(wpath, O_RDONLY);
    test_result("open after append succeeds", wfd >= 0);
    if (wfd >= 0) {
        char rbuf[64];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t r2 = read(wfd, rbuf, sizeof(rbuf) - 1);
        char expect[64];
        snprintf(expect, sizeof(expect), "%s%s", wmsg1, wmsg2);
        test_result("read back appended data", r2 == (ssize_t)strlen(expect) && strcmp(rbuf, expect) == 0);
        close(wfd);
    }
    // overwrite via lseek
    wfd = open(wpath, O_WRONLY);
    test_result("open(O_WRONLY) succeeds", wfd >= 0);
    if (wfd >= 0) {
        lseek(wfd, 5, 0);
        const char* wmsg3 = "-";
        ssize_t w3 = write(wfd, wmsg3, 1);
        test_result("lseek+overwrite", w3 == 1);
        close(wfd);
    }
    // read back overwrite
    wfd = open(wpath, O_RDONLY);
    test_result("open after overwrite succeeds", wfd >= 0);
    if (wfd >= 0) {
        char rbuf[64];
        memset(rbuf, 0, sizeof(rbuf));
        read(wfd, rbuf, sizeof(rbuf) - 1);
        test_result("overwrite applied", rbuf[5] == '-');
        close(wfd);
    }

    // ========================================
    // Test: fstat/fsync/ftruncate
    // ========================================
    printf("\n[TEST] fstat/fsync/ftruncate\n");
    int tfd = open("/WRITE.TXT", O_WRONLY);
    test_result("open existing file for fstat", tfd >= 0);
    if (tfd >= 0) {
        test_result("fstat succeeds", fstat(tfd, &st) == 0);
        test_result("fsync succeeds", fsync(tfd) == 0);
        test_result("ftruncate to 4 bytes", ftruncate(tfd, 4) == 0);
        int fl = fcntl(tfd, F_GETFL);
        test_result("fcntl(F_GETFL) returns flags", fl >= 0);
        test_result("fcntl(F_SETFL) sets O_APPEND", fcntl(tfd, F_SETFL, O_APPEND) == 0);
        close(tfd);
    }
    // verify truncate
    tfd = open("/WRITE.TXT", O_RDONLY);
    if (tfd >= 0) {
        char rbuf[16];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t rr = read(tfd, rbuf, sizeof(rbuf) - 1);
        test_result("truncate reduced size", rr == 4);
        close(tfd);
    }
    // rename/unlink
    test_result("rename succeeds", rename("/WRITE.TXT", "/WRITE2.TXT") == 0);
    test_result("unlink succeeds", unlink("/WRITE2.TXT") == 0);

    // ========================================
    // Test: mkdir/rmdir
    // ========================================
    printf("\n[TEST] mkdir/rmdir\n");
    test_result("mkdir('/TESTDIR') succeeds", mkdir("/TESTDIR", 0777) == 0);
    test_result("chdir('/TESTDIR') succeeds", chdir("/TESTDIR") == 0);
    int dfd = open("/TESTDIR/FILE.TXT", O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create file in dir", dfd >= 0);
    if (dfd >= 0) {
        write(dfd, "X", 1);
        close(dfd);
    }
    test_result("unlink file in dir", unlink("/TESTDIR/FILE.TXT") == 0);
    test_result("chdir('/') succeeds", chdir("/") == 0);
    test_result("rmdir('/TESTDIR') succeeds", rmdir("/TESTDIR") == 0);

    // ========================================
    // Test: kill
    // ========================================
    printf("\n[TEST] kill\n");
    test_result("kill(getpid(), 0) succeeds", kill(getpid(), 0) == 0);
    test_result("kill(invalid, 0) fails", kill(99999, 0) == -1 && errno == ESRCH);
    pid_t kchild = fork();
    if (kchild == 0) {
        // child waits to be killed
        sleep(5);
        _exit(0);
    } else if (kchild > 0) {
        test_result("kill(child, SIGTERM) succeeds", kill(kchild, SIGTERM) == 0);
        int kst = 0;
        pid_t kw = waitpid(kchild, &kst, 0);
        test_result("waitpid returns child", kw == kchild);
        test_result("child killed exit status", WIFEXITED(kst) && WEXITSTATUS(kst) == (128 + SIGTERM));
    } else {
        test_fail("fork() for kill test failed");
    }

    // ========================================
    // Test: tty/pty + termios
    // ========================================
    printf("\n[TEST] tty/pty\n");
    int mfd = posix_openpt(O_RDWR);
    test_result("posix_openpt() succeeds", mfd >= 0);
    int pty_num = -1;
    if (mfd >= 0) {
        test_result("ioctl(TIOCGPTN) succeeds", ioctl(mfd, TIOCGPTN, &pty_num) == 0 && pty_num >= 0);
    }
    char pts_path[32];
    int sfd = -1;
    if (pty_num >= 0) {
        snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", pty_num);
        sfd = open(pts_path, O_RDWR);
        test_result("open pts slave succeeds", sfd >= 0);
    }

    if (mfd >= 0 && sfd >= 0) {
        struct termios tio;
        test_result("tcgetattr succeeds", tcgetattr(sfd, &tio) == 0);
        test_result("canonical enabled by default", (tio.c_lflag & ICANON) != 0);
        test_result("echo enabled by default", (tio.c_lflag & ECHO) != 0);

        cfmakeraw(&tio);
        test_result("tcsetattr(TCSANOW) succeeds", tcsetattr(sfd, TCSANOW, &tio) == 0);
        test_result("tcgetattr raw", tcgetattr(sfd, &tio) == 0);
        test_result("canonical disabled in raw", (tio.c_lflag & ICANON) == 0);

        const char* ping = "ping";
        test_result("write master->slave", write(mfd, ping, 4) == 4);
        char rbuf[8];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t rr = read(sfd, rbuf, 4);
        test_result("read slave receives data", rr == 4 && memcmp(rbuf, ping, 4) == 0);

        const char* pong = "pong";
        test_result("write slave->master", write(sfd, pong, 4) == 4);
        memset(rbuf, 0, sizeof(rbuf));
        rr = read(mfd, rbuf, 4);
        test_result("read master receives data", rr == 4 && memcmp(rbuf, pong, 4) == 0);

        test_result("tcsetpgrp succeeds", tcsetpgrp(sfd, getpgrp()) == 0);
        test_result("tcgetpgrp matches", tcgetpgrp(sfd) == getpgrp());

        close(sfd);
        close(mfd);
    } else {
        test_fail("pty master/slave setup failed");
        if (mfd >= 0) close(mfd);
        if (sfd >= 0) close(sfd);
    }

    // ========================================
    // Test: signals
    // ========================================
    printf("\n[TEST] signals\n");
    g_sigusr1_hit = 0;
    g_sigusr2_hit = 0;
    g_last_signal = 0;
    g_signal_hits = 0;
    test_result("signal(SIGUSR1) set", signal(SIGUSR1, handle_sigusr1) != SIG_ERR);
    test_result("raise(SIGUSR1) returns 0", raise(SIGUSR1) == 0);
    test_result("SIGUSR1 handler ran", g_sigusr1_hit == 1);

    signal(SIGUSR2, handle_sigusr2);
    test_result("kill(self,SIGUSR2) returns 0", kill(getpid(), SIGUSR2) == 0);
    test_result("SIGUSR2 handler ran", g_sigusr2_hit == 1);

    // Test a range of signals with a generic handler (skip SIGKILL/SIGSTOP)
    int sigs_to_test[] = { SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE,
                           SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD,
                           SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU };
    int sig_count = (int)(sizeof(sigs_to_test) / sizeof(sigs_to_test[0]));
    for (int i = 0; i < sig_count; i++) {
        int sig = sigs_to_test[i];
        g_last_signal = 0;
        signal(sig, handle_generic);
        int rr = raise(sig);
        char name[64];
        snprintf(name, sizeof(name), "raise signal %d", sig);
        test_result(name, rr == 0 && g_last_signal == sig);
    }

    // ========================================
    // Test: alarm/sleep
    // ========================================
    printf("\n[TEST] alarm/sleep\n");
    g_sigalrm_hit = 0;
    signal(SIGALRM, handle_sigalrm);
    unsigned int rem = alarm(1);
    test_result("alarm(1) returns remaining", rem == 0);
    sleep(2);
    test_result("SIGALRM delivered", g_sigalrm_hit == 1);

    // rename/unlink

    // large write to force multi-cluster
    printf("\n[TEST] large file write (multi-cluster)\n");
    const char* lpath = "/LARGE.TXT";
    size_t lsize = 7000;
    char* lbuf = (char*)malloc(lsize);
    if (lbuf) {
        for (size_t i = 0; i < lsize; i++) {
            lbuf[i] = (char)('A' + (i % 26));
        }
        int lfd = open(lpath, O_CREAT | O_TRUNC | O_WRONLY);
        test_result("open large file for write", lfd >= 0);
        if (lfd >= 0) {
            ssize_t lw = write(lfd, lbuf, lsize);
            test_result("write large buffer", lw == (ssize_t)lsize);
            close(lfd);
        }
        lfd = open(lpath, O_RDONLY);
        test_result("open large file for read", lfd >= 0);
        if (lfd >= 0) {
            char* lread = (char*)malloc(lsize + 1);
            if (lread) {
                memset(lread, 0, lsize + 1);
                ssize_t lr = read(lfd, lread, lsize);
                test_result("read large buffer", lr == (ssize_t)lsize);
                test_result("large data matches", lr == (ssize_t)lsize && memcmp(lbuf, lread, lsize) == 0);
                free(lread);
            } else {
                test_fail("malloc for large read buffer");
            }
            close(lfd);
        }
        free(lbuf);
    } else {
        test_fail("malloc for large write buffer");
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
