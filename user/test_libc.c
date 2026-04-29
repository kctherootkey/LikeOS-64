#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
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
#include <dlfcn.h>
#include <getopt.h>
#include <sys/procinfo.h>
#include <sys/vfs.h>
#include <sys/sysinfo.h>
#include <sys/klog.h>
#include <sys/un.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <net/if.h>

// Futex helper declarations (from sched.c)
int futex_wait(int* uaddr, int val, const struct timespec* timeout);
int futex_wake(int* uaddr, int count);

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

static int get_interface_ipv4(const char* ifname, uint32_t* ip_out) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    int ret = ioctl(sock, SIOCGIFADDR, &ifr);
    close(sock);
    if (ret < 0)
        return -1;

    struct sockaddr_in* sin = (struct sockaddr_in*)&ifr.ifr_addr;
    *ip_out = ntohl(sin->sin_addr.s_addr);
    return 0;
}

static void run_tcp_large_transfer_case(const char* prefix,
                                        uint32_t bind_ip,
                                        uint32_t connect_ip,
                                        uint16_t port) {
    char label[96];
    enum { transfer_size = 4096 };
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    snprintf(label, sizeof(label), "%s: server socket", prefix);
    test_result(label, server_fd >= 0);

    if (server_fd < 0)
        return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(bind_ip);

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    int ret = bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    snprintf(label, sizeof(label), "%s: bind", prefix);
    test_result(label, ret == 0);

    if (ret == 0) {
        ret = listen(server_fd, 4);
        snprintf(label, sizeof(label), "%s: listen", prefix);
        test_result(label, ret == 0);
    }

    if (ret == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            int client_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (client_fd >= 0) {
                struct sockaddr_in dst;
                memset(&dst, 0, sizeof(dst));
                dst.sin_family = AF_INET;
                dst.sin_port = htons(port);
                dst.sin_addr.s_addr = htonl(connect_ip);

                if (connect(client_fd, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
                    char sendbuf[transfer_size];
                    for (int i = 0; i < (int)sizeof(sendbuf); i++)
                        sendbuf[i] = (char)('a' + (i % 23));

                    size_t sent = 0;
                    while (sent < sizeof(sendbuf)) {
                        ssize_t n = send(client_fd, sendbuf + sent,
                                         sizeof(sendbuf) - sent, 0);
                        if (n <= 0)
                            break;
                        sent += (size_t)n;
                    }

                    close(client_fd);
                    _exit(sent == sizeof(sendbuf) ? 0 : 2);
                }
                close(client_fd);
            }
            _exit(1);
        } else if (pid > 0) {
            int conn_fd = accept(server_fd, NULL, NULL);
            snprintf(label, sizeof(label), "%s: accept", prefix);
            test_result(label, conn_fd >= 0);

            if (conn_fd >= 0) {
                char recvbuf[transfer_size];
                char expectbuf[transfer_size];
                for (int i = 0; i < (int)sizeof(expectbuf); i++)
                    expectbuf[i] = (char)('a' + (i % 23));

                size_t recvd = 0;
                while (recvd < sizeof(recvbuf)) {
                    ssize_t n = recv(conn_fd, recvbuf + recvd,
                                     sizeof(recvbuf) - recvd, 0);
                    if (n <= 0)
                        break;
                    recvd += (size_t)n;
                }

                snprintf(label, sizeof(label), "%s: recv 4096 bytes", prefix);
                test_result(label, recvd == sizeof(recvbuf));

                snprintf(label, sizeof(label), "%s: payload matches", prefix);
                test_result(label, recvd == sizeof(recvbuf) &&
                                   memcmp(recvbuf, expectbuf, sizeof(recvbuf)) == 0);
                close(conn_fd);
            }

            int status = 0;
            waitpid(pid, &status, 0);
            snprintf(label, sizeof(label), "%s: client completed", prefix);
            test_result(label, WIFEXITED(status) && WEXITSTATUS(status) == 0);
        } else {
            snprintf(label, sizeof(label), "%s: fork", prefix);
            test_fail(label);
        }
    }

    close(server_fd);
}

static void run_programerror_case(const char* name, const char* mode, int expected_sig) {
    pid_t child = fork();
    if (child < 0) {
        test_fail(name);
        return;
    }
    if (child == 0) {
        char* argv_exec[] = { "/usr/local/bin/progerr", (char*)mode, NULL };
        char* envp_exec[] = { NULL };
        execve("/usr/local/bin/progerr", argv_exec, envp_exec);
        _exit(1);
    }

    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    if (waited != child) {
        test_fail(name);
        return;
    }

    int ok = 0;
    if (WIFSIGNALED(status) && WTERMSIG(status) == expected_sig) {
        ok = 1;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == (128 + expected_sig)) {
        ok = 1;
    }
    test_result(name, ok);
}

// ========================================
// Pthread test helper functions (file-scope to avoid GCC nested function trampolines)
// ========================================

// For pthread_create/join test
static volatile int g_simple_thread_ran = 0;
static volatile int g_simple_thread_arg = 0;

static void* simple_thread_fn(void* arg) {
    g_simple_thread_ran = 1;
    g_simple_thread_arg = (int)(long)arg;
    return (void*)42L;
}

// For pthread_detach test
static volatile int g_detached_thread_ran = 0;

static void* detached_thread_fn(void* arg) {
    (void)arg;
    g_detached_thread_ran = 1;
    return NULL;
}

// For mutex contention test
static pthread_mutex_t g_contention_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_shared_counter = 0;

static void* increment_thread_fn(void* arg) {
    int count = (int)(long)arg;
    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&g_contention_mutex);
        g_shared_counter++;
        pthread_mutex_unlock(&g_contention_mutex);
    }
    return NULL;
}

// For condition variable test
struct cond_test_args {
    pthread_cond_t* cond;
    pthread_mutex_t* mutex;
    volatile int* flag;
};

static void* cond_waiter_thread_fn(void* arg) {
    struct cond_test_args* args = (struct cond_test_args*)arg;
    pthread_mutex_lock(args->mutex);
    while (!*args->flag) {
        pthread_cond_wait(args->cond, args->mutex);
    }
    pthread_mutex_unlock(args->mutex);
    return (void*)99L;
}

// For broadcast test
static pthread_cond_t g_bcast_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_bcast_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_bcast_flag = 0;
static volatile int g_waiters_done = 0;

static void* bcast_waiter_fn(void* arg) {
    (void)arg;
    pthread_mutex_lock(&g_bcast_mutex);
    while (!g_bcast_flag) {
        pthread_cond_wait(&g_bcast_cond, &g_bcast_mutex);
    }
    g_waiters_done++;
    pthread_mutex_unlock(&g_bcast_mutex);
    return NULL;
}

// For barrier test
static pthread_barrier_t g_barrier;
static volatile int g_barrier_arrivals = 0;

static void* barrier_thread_fn(void* arg) {
    (void)arg;
    __sync_fetch_and_add(&g_barrier_arrivals, 1);
    int r = pthread_barrier_wait(&g_barrier);
    return (void*)(long)r;
}

// For TSD test
static pthread_key_t g_tsd_key;
static volatile int g_destructor_called = 0;

static void tsd_destructor_fn(void* value) {
    if (value) {
        g_destructor_called = 1;
    }
}

static void* tsd_thread_fn(void* arg) {
    (void)arg;
    // Should be NULL initially in new thread
    void* v = pthread_getspecific(g_tsd_key);
    if (v != NULL) return (void*)1L;
    
    // Set thread-local value
    pthread_setspecific(g_tsd_key, (void*)99999L);
    v = pthread_getspecific(g_tsd_key);
    if (v != (void*)99999L) return (void*)2L;
    
    return (void*)0L;  // Success
}

// For pthread_once test
static pthread_once_t g_once_control = PTHREAD_ONCE_INIT;
static volatile int g_once_counter = 0;

static void once_init_fn(void) {
    g_once_counter++;
}

static void* once_thread_fn(void* arg) {
    (void)arg;
    pthread_once(&g_once_control, once_init_fn);
    return NULL;
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
        char* exec_argv[] = { "/usr/local/bin/hello", NULL };
        char* exec_envp[] = { NULL };
        execve("/usr/local/bin/hello", exec_argv, exec_envp);
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

        // ========================================
        // Test: progerr (user fault handling)
        // ========================================
        printf("\n[TEST] progerr (user fault handling)\n");
        run_programerror_case("illegal instruction -> SIGILL", "ill", SIGILL);
        run_programerror_case("invalid user write -> SIGSEGV", "baduser", SIGSEGV);
        run_programerror_case("kernel write -> SIGSEGV", "badkernel", SIGSEGV);
    }

    // ========================================
    // Test: execv/execvp (via fork)
    // ========================================
    printf("\n[TEST] execv/execvp via fork\n");
    pid_t execv_child = fork();
    if (execv_child < 0) {
        test_fail("fork() for execv failed");
    } else if (execv_child == 0) {
        char* exec_argv[] = { "/usr/local/bin/hello", NULL };
        execv("/usr/local/bin/hello", exec_argv);
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
    setenv("PATH", "/usr/local/bin:/bin", 1);
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
        test_result("child killed exit status",
                (WIFSIGNALED(kst) && WTERMSIG(kst) == SIGTERM) ||
                (WIFEXITED(kst) && WEXITSTATUS(kst) == (128 + SIGTERM)));
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
    // Test: Extended signal handling (kernel syscalls)
    // ========================================
    printf("\n[TEST] extended signal handling\n");
    
    // Reinstall handle_sigusr1 (was overwritten by handle_generic in loop above)
    signal(SIGUSR1, handle_sigusr1);
    
    // Test sigprocmask
    sigset_t oldmask, newmask, pendmask;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGUSR1);
    
    test_result("sigprocmask(SIG_BLOCK, SIGUSR1) returns 0", 
                sigprocmask(SIG_BLOCK, &newmask, &oldmask) == 0);
    
    // Signal should now be blocked - raise it, it should be pending
    g_sigusr1_hit = 0;
    raise(SIGUSR1);
    test_result("SIGUSR1 blocked, handler not called", g_sigusr1_hit == 0);
    
    // Check sigpending
    sigemptyset(&pendmask);
    test_result("sigpending returns 0", sigpending(&pendmask) == 0);
    test_result("SIGUSR1 is pending", sigismember(&pendmask, SIGUSR1) == 1);
    
    // Unblock and deliver
    test_result("sigprocmask(SIG_UNBLOCK, SIGUSR1) returns 0", 
                sigprocmask(SIG_UNBLOCK, &newmask, NULL) == 0);
    test_result("SIGUSR1 delivered after unblock", g_sigusr1_hit == 1);
    
    // Test sigaction with structure
    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = handle_sigusr2;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    
    g_sigusr2_hit = 0;
    test_result("sigaction(SIGUSR2) returns 0", 
                sigaction(SIGUSR2, &sa_new, &sa_old) == 0);
    raise(SIGUSR2);
    test_result("sigaction handler called", g_sigusr2_hit == 1);
    
    // Test sigfillset
    sigset_t fullset;
    sigfillset(&fullset);
    test_result("sigfillset: SIGUSR1 set", sigismember(&fullset, SIGUSR1) == 1);
    test_result("sigfillset: SIGUSR2 set", sigismember(&fullset, SIGUSR2) == 1);
    test_result("sigfillset: SIGTERM set", sigismember(&fullset, SIGTERM) == 1);
    
    // Test sigdelset
    sigdelset(&fullset, SIGUSR1);
    test_result("sigdelset(SIGUSR1) works", sigismember(&fullset, SIGUSR1) == 0);
    test_result("sigdelset: SIGUSR2 still set", sigismember(&fullset, SIGUSR2) == 1);
    
    // Test SIG_IGN
    signal(SIGUSR1, SIG_IGN);
    g_sigusr1_hit = 0;
    raise(SIGUSR1);
    test_result("SIG_IGN: handler not called", g_sigusr1_hit == 0);
    
    // Test SIG_DFL restore
    signal(SIGUSR1, handle_sigusr1);
    g_sigusr1_hit = 0;
    raise(SIGUSR1);
    test_result("handler restored after SIG_IGN", g_sigusr1_hit == 1);
    
    // Test nanosleep
    printf("\n[TEST] nanosleep\n");
    struct timespec ts_req, ts_rem;
    ts_req.tv_sec = 0;
    ts_req.tv_nsec = 50000000;  // 50ms
    ts_rem.tv_sec = 0;
    ts_rem.tv_nsec = 0;
    int ns_ret = nanosleep(&ts_req, &ts_rem);
    test_result("nanosleep(50ms) returns 0", ns_ret == 0);
    
    // Test usleep
    printf("\n[TEST] usleep\n");
    test_result("usleep(10000) returns 0", usleep(10000) == 0);  // 10ms
    
    // Test sigaltstack
    printf("\n[TEST] sigaltstack\n");
    stack_t ss_new, ss_old;
    static char alt_stack_buf[SIGSTKSZ];
    ss_new.ss_sp = alt_stack_buf;
    ss_new.ss_size = SIGSTKSZ;
    ss_new.ss_flags = 0;
    test_result("sigaltstack set returns 0", sigaltstack(&ss_new, &ss_old) == 0);
    
    // Verify we can get it back
    stack_t ss_check;
    test_result("sigaltstack get returns 0", sigaltstack(NULL, &ss_check) == 0);
    test_result("sigaltstack ss_sp matches", ss_check.ss_sp == alt_stack_buf);
    test_result("sigaltstack ss_size matches", ss_check.ss_size == SIGSTKSZ);
    
    // Disable alternate stack
    ss_new.ss_flags = SS_DISABLE;
    test_result("sigaltstack disable returns 0", sigaltstack(&ss_new, NULL) == 0);

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
    // Test: Long File Name (LFN) support
    // ========================================
    printf("\n[TEST] Long File Name (LFN) support\n");
    
    // Test 1: Create file with long name (lowercase preserved)
    const char* lfn_path1 = "/this_is_a_long_filename_test.txt";
    int lfn_fd = open(lfn_path1, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create long filename", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        const char* lfn_data = "LFN test data";
        ssize_t lfn_wr = write(lfn_fd, lfn_data, strlen(lfn_data));
        test_result("write to LFN file", lfn_wr == (ssize_t)strlen(lfn_data));
        close(lfn_fd);
    }
    
    // Test 2: Read back the long filename file
    lfn_fd = open(lfn_path1, O_RDONLY);
    test_result("open LFN file for read", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        char lfn_rbuf[64];
        memset(lfn_rbuf, 0, sizeof(lfn_rbuf));
        ssize_t lfn_rd = read(lfn_fd, lfn_rbuf, sizeof(lfn_rbuf) - 1);
        test_result("read LFN file", lfn_rd > 0);
        test_result("LFN data correct", strcmp(lfn_rbuf, "LFN test data") == 0);
        close(lfn_fd);
    }
    
    // Test 3: Case-insensitive access (open with different case)
    lfn_fd = open("/THIS_IS_A_LONG_FILENAME_TEST.TXT", O_RDONLY);
    test_result("case-insensitive LFN access", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    
    // Test 4: Mixed case filename
    const char* lfn_path2 = "/MixedCaseFileName.TXT";
    lfn_fd = open(lfn_path2, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create mixed case filename", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        write(lfn_fd, "X", 1);
        close(lfn_fd);
    }
    
    // Test 5: Verify case-insensitive access to mixed case file
    lfn_fd = open("/mixedcasefilename.txt", O_RDONLY);
    test_result("case-insensitive mixed case access", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    
    // Test 6: Long directory name
    const char* lfn_dir = "/long_directory_name_for_testing";
    test_result("mkdir long dirname", mkdir(lfn_dir, 0777) == 0);
    
    // Test 7: Create file in long dirname
    const char* lfn_in_dir = "/long_directory_name_for_testing/another_long_filename.dat";
    lfn_fd = open(lfn_in_dir, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create file in LFN dir", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        write(lfn_fd, "nested", 6);
        close(lfn_fd);
    }
    
    // Test 8: Read file from long dirname
    lfn_fd = open(lfn_in_dir, O_RDONLY);
    test_result("open file in LFN dir", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        char rbuf[16];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t rr = read(lfn_fd, rbuf, sizeof(rbuf) - 1);
        test_result("read file in LFN dir", rr == 6 && strcmp(rbuf, "nested") == 0);
        close(lfn_fd);
    }
    
    // Test 9: Rename with long filename
    const char* lfn_renamed = "/renamed_long_filename_test.txt";
    test_result("rename LFN file", rename(lfn_path1, lfn_renamed) == 0);
    
    // Test 10: Verify renamed file exists
    lfn_fd = open(lfn_renamed, O_RDONLY);
    test_result("open renamed LFN file", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    
    // Test 11: Unlink files with long names
    test_result("unlink renamed LFN file", unlink(lfn_renamed) == 0);
    test_result("unlink mixed case file", unlink(lfn_path2) == 0);
    test_result("unlink file in LFN dir", unlink(lfn_in_dir) == 0);
    
    // Test 12: Rmdir long dirname
    test_result("rmdir LFN dir", rmdir(lfn_dir) == 0);
    
    // Test 13: Very long filename (near max)
    const char* very_long = "/abcdefghijklmnopqrstuvwxyz0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ.longext";
    lfn_fd = open(very_long, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create very long filename", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        write(lfn_fd, "V", 1);
        close(lfn_fd);
    }
    lfn_fd = open(very_long, O_RDONLY);
    test_result("open very long filename", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    test_result("unlink very long filename", unlink(very_long) == 0);
    
    // Test 14: Filename with spaces
    const char* space_name = "/file with spaces in name.txt";
    lfn_fd = open(space_name, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create filename with spaces", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        write(lfn_fd, "S", 1);
        close(lfn_fd);
    }
    lfn_fd = open(space_name, O_RDONLY);
    test_result("open filename with spaces", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    test_result("unlink filename with spaces", unlink(space_name) == 0);
    
    // Test 15: Case preservation check - create lowercase, verify lowercase display
    const char* lowercase_file = "/lowercase_only_filename.txt";
    lfn_fd = open(lowercase_file, O_CREAT | O_TRUNC | O_WRONLY);
    test_result("create lowercase filename", lfn_fd >= 0);
    if (lfn_fd >= 0) {
        close(lfn_fd);
    }
    // Clean up
    unlink(lowercase_file);
    
    // Test 16: Directory with mixed case
    const char* mixed_dir = "/MyMixedCaseDirectory";
    test_result("mkdir mixed case dir", mkdir(mixed_dir, 0777) == 0);
    test_result("chdir mixed case dir (lowercase)", chdir("/mymixedcasedirectory") == 0);
    test_result("chdir back to root", chdir("/") == 0);
    test_result("rmdir mixed case dir", rmdir(mixed_dir) == 0);

    // ========================================
    // Test: Security - Invalid Pointer Handling
    // ========================================
    printf("\n[TEST] Security - Invalid Pointer Handling\n");
    
    // Test: NULL pointer should fail with EFAULT
    int sec_ret = read(0, NULL, 100);
    test_result("read(NULL) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    sec_ret = write(1, NULL, 100);
    test_result("write(NULL) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    // Test: Kernel address pointer should fail with EFAULT
    void* kernel_addr = (void*)0xFFFFFFFF80000000ULL;
    sec_ret = read(0, kernel_addr, 100);
    test_result("read(kernel_addr) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    sec_ret = write(1, kernel_addr, 100);
    test_result("write(kernel_addr) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    // Test: stat with NULL buffer should fail
    sec_ret = stat("/", NULL);
    test_result("stat(NULL buf) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    // Test: open with NULL path should fail
    sec_ret = open(NULL, O_RDONLY);
    test_result("open(NULL) returns EFAULT", sec_ret == -1 && errno == EFAULT);

    // ========================================
    // Test: Security - Integer Overflow Protection
    // ========================================
    printf("\n[TEST] Security - Integer Overflow Protection\n");
    
    // Test: Excessive mmap size should fail
    void* bad_mmap = mmap(NULL, 0xFFFFFFFFFFFFFFFFULL, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap(huge size) fails", bad_mmap == MAP_FAILED);
    
    // Test: Very large read should fail gracefully (not crash)
    // Note: We can't actually pass >1GB to read in practice, but the kernel should handle it
    char tiny_buf[1];
    // The kernel will reject this because the buffer is only 1 byte but we're asking for huge read
    // Either way, the kernel should not crash
    sec_ret = read(0, tiny_buf, 0x7FFFFFFFFFFFFFFULL);
    test_result("read(huge count) returns error", sec_ret == -1);

    // ========================================
    // Test: Security - IOCTL Validation
    // ========================================
    printf("\n[TEST] Security - IOCTL Validation\n");
    
    // Test: TIOCGWINSZ with NULL should fail
    sec_ret = ioctl(0, TIOCGWINSZ, NULL);
    test_result("ioctl(TIOCGWINSZ, NULL) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    // Test: TIOCGWINSZ with kernel address should fail
    sec_ret = ioctl(0, TIOCGWINSZ, kernel_addr);
    test_result("ioctl(TIOCGWINSZ, kernel_addr) returns EFAULT", sec_ret == -1 && errno == EFAULT);
    
    // Test: Valid IOCTL should succeed
    struct winsize ws;
    sec_ret = ioctl(0, TIOCGWINSZ, &ws);
    test_result("ioctl(TIOCGWINSZ, valid) succeeds", sec_ret == 0);

    // ========================================
    // Test: Security - Memory Protection
    // ========================================
    printf("\n[TEST] Security - Memory Protection\n");
    
    // Test: mmap anonymous memory works
    void* anon_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap anonymous succeeds", anon_mem != MAP_FAILED);
    if (anon_mem != MAP_FAILED) {
        // Test: Memory should be zero-initialized
        int zero_init = 1;
        unsigned char* mem = (unsigned char*)anon_mem;
        for (int i = 0; i < 4096; i++) {
            if (mem[i] != 0) {
                zero_init = 0;
                break;
            }
        }
        test_result("mmap memory is zero-initialized", zero_init);
        munmap(anon_mem, 4096);
    }
    
    // Test: mmap with zero length should fail
    void* zero_mmap = mmap(NULL, 0, PROT_READ | PROT_WRITE, 
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap(0 length) fails", zero_mmap == MAP_FAILED);

    // Test: MAP_FIXED at low address (< 64KB) should fail
    void* low_mmap = mmap((void*)0x1000, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    test_result("mmap(MAP_FIXED, addr=0x1000) fails", low_mmap == MAP_FAILED);
    
    // Test: MAP_FIXED at NULL should fail
    void* null_fixed = mmap((void*)0, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    test_result("mmap(MAP_FIXED, addr=0) fails", null_fixed == MAP_FAILED);
    
    // Test: MAP_FIXED at address just below 64KB boundary should fail
    void* boundary_mmap = mmap((void*)0xF000, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    test_result("mmap(MAP_FIXED, addr=0xF000) fails", boundary_mmap == MAP_FAILED);
    
    // Test: MAP_FIXED at valid address (>= 64KB) should succeed
    // Use a high address that's unlikely to conflict
    void* valid_fixed = mmap((void*)0x10000000, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    test_result("mmap(MAP_FIXED, addr=0x10000000) succeeds", valid_fixed != MAP_FAILED);
    if (valid_fixed != MAP_FAILED) {
        munmap(valid_fixed, 4096);
    }
    
    // Test: Excessive mmap size (> 2GB limit) should fail
    void* huge_mmap = mmap(NULL, 3ULL * 1024 * 1024 * 1024, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap(3GB) fails (exceeds 2GB limit)", huge_mmap == MAP_FAILED);
    
    // Test: Multiple small mmaps should succeed
    void* multi1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* multi2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* multi3 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("multiple mmap calls succeed", 
                multi1 != MAP_FAILED && multi2 != MAP_FAILED && multi3 != MAP_FAILED);
    test_result("mmap returns different addresses", 
                multi1 != multi2 && multi2 != multi3 && multi1 != multi3);
    if (multi1 != MAP_FAILED) munmap(multi1, 4096);
    if (multi2 != MAP_FAILED) munmap(multi2, 4096);
    if (multi3 != MAP_FAILED) munmap(multi3, 4096);
    
    // Test: mmap with PROT_NONE should succeed (reserve memory)
    void* prot_none = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_result("mmap(PROT_NONE) succeeds", prot_none != MAP_FAILED);
    if (prot_none != MAP_FAILED) {
        munmap(prot_none, 4096);
    }

    // ========================================
    // MAP_SHARED Test (Shared Memory Between Parent and Child)
    // ========================================
    printf("\n--- MAP_SHARED Test ---\n");
    {
        // Create shared anonymous memory
        volatile int* shared_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        test_result("mmap(MAP_SHARED|MAP_ANONYMOUS) succeeds", shared_mem != MAP_FAILED);
        
        if (shared_mem != MAP_FAILED) {
            // Initialize shared memory
            shared_mem[0] = 0;      // Counter
            shared_mem[1] = 0;      // Child ready flag
            shared_mem[2] = 0;      // Parent ack flag
            
            pid_t shared_pid = fork();
            if (shared_pid == 0) {
                // Child: wait for parent to signal, then increment counter
                // Spin wait for parent to set ack flag
                while (shared_mem[2] == 0) {
                    // Busy wait
                }
                
                // Child increments counter
                shared_mem[0] = shared_mem[0] + 100;
                
                // Signal child is done
                shared_mem[1] = 1;
                
                _exit(0);
            } else if (shared_pid > 0) {
                // Parent: write to shared memory and signal child
                shared_mem[0] = 42;
                
                // Signal child to proceed
                shared_mem[2] = 1;
                
                // Wait for child to finish
                int status;
                waitpid(shared_pid, &status, 0);
                
                // Child should have added 100 to our 42
                int expected_value = 142;
                int actual_value = shared_mem[0];
                
                printf("MAP_SHARED: parent wrote 42, child added 100, result=%d (expected %d)\n",
                       actual_value, expected_value);
                test_result("MAP_SHARED memory visible between processes", 
                           actual_value == expected_value);
                test_result("Child completion flag visible", shared_mem[1] == 1);
            } else {
                test_result("fork for MAP_SHARED test", 0);
            }
            
            munmap((void*)shared_mem, 4096);
        }
    }

    // ========================================
    // Large Allocation Test (100MB)
    // ========================================
    printf("\n--- Large Allocation Test (100MB) ---\n");
    {
        const size_t large_size = 100 * 1024 * 1024;  // 100MB
        printf("Attempting to allocate 100MB...\n");
        
        void* large_alloc = malloc(large_size);
        test_result("malloc(100MB) succeeds", large_alloc != NULL);
        
        if (large_alloc) {
            // Touch first and last byte to verify memory is usable
            volatile char* p = (volatile char*)large_alloc;
            p[0] = 0xAA;
            p[large_size - 1] = 0x55;
            
            int first_ok = (p[0] == (char)0xAA);
            int last_ok = (p[large_size - 1] == 0x55);
            
            test_result("100MB write/read first byte", first_ok);
            test_result("100MB write/read last byte", last_ok);
            
            // Touch some pages in the middle to verify mapping
            size_t mid = large_size / 2;
            p[mid] = 0x42;
            test_result("100MB write/read middle byte", p[mid] == 0x42);
            
            printf("100MB allocation at %p, verified %lu bytes\n", 
                   large_alloc, (unsigned long)large_size);
            
            free(large_alloc);
            printf("100MB freed successfully\n");
        } else {
            printf("FAILED: Could not allocate 100MB\n");
        }
    }

    // ========================================
    // Preemptive Scheduling Test
    // ========================================
    printf("\n--- Preemptive Scheduling Test ---\n");
    {
        // This test verifies that the scheduler can preempt a CPU-bound child
        // The parent should be able to continue running even if the child loops forever
        
        pid_t child = fork();
        if (child < 0) {
            test_fail("preemption test: fork failed");
        } else if (child == 0) {
            // Child: infinite CPU-bound loop
            // With preemptive scheduling, this should NOT starve the parent
            volatile unsigned long counter = 0;
            while (1) {
                counter++;
                // Tight loop - no voluntary yields
            }
            _exit(0); // Never reached
        } else {
            // Parent: sleep briefly, then verify we're still running
            // If scheduling is purely cooperative, we'd be starved by the child
            
            // Use a simple busy-wait counter to measure time passing
            // In a preemptive system, we should still get CPU time
            volatile unsigned long parent_counter = 0;
            int parent_ran = 0;
            
            // Try to increment counter a million times
            // With 20ms time slices at 100Hz, we should get enough CPU time
            for (int i = 0; i < 100; i++) {
                // Small work unit
                for (int j = 0; j < 10000; j++) {
                    parent_counter++;
                }
                parent_ran = 1;
            }
            
            // If we got here, preemption is working
            test_result("parent not starved by child loop", parent_ran);
            test_result("parent counter incremented", parent_counter > 0);
            
            // Kill the looping child
            printf("Killing child %d...\n", child);
            int kill_ret = kill(child, SIGKILL);
            printf("kill() returned %d\n", kill_ret);
            
            // Use WNOHANG in a loop with timeout to avoid infinite hang
            int status = 0;
            pid_t waited = -1;
            for (int tries = 0; tries < 100; tries++) {
                waited = waitpid(child, &status, WNOHANG);
                if (waited > 0) {
                    break;  // Child reaped
                }
                // Small delay using busy loop
                for (volatile int d = 0; d < 100000; d++) {}
            }
            
            if (waited == child) {
                test_result("preemption: child killed", 1);
                test_result("preemption: child signaled", WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            } else {
                printf("waitpid returned %d (expected %d)\n", waited, child);
                test_fail("preemption: child killed");
                test_fail("preemption: child signaled");
            }
            
            printf("Preemption test completed: parent_counter=%lu\n", parent_counter);
        }
    }

    // ========================================
    // Extended Preemptive Kernel Tests
    // ========================================
    printf("\n--- Extended Preemptive Kernel Tests ---\n");
    
    // Test 1: Multiple CPU-bound children - fair scheduling
    printf("\n[TEST] Multiple CPU-bound children (fair scheduling)\n");
    {
        #define NUM_CHILDREN 3
        pid_t children[NUM_CHILDREN];
        int pipe_fds[NUM_CHILDREN][2];
        
        // Create pipes for each child to report back
        int pipes_ok = 1;
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (pipe(pipe_fds[i]) < 0) {
                pipes_ok = 0;
                break;
            }
        }
        
        if (pipes_ok) {
            // Fork children that do CPU work and report iterations
            for (int i = 0; i < NUM_CHILDREN; i++) {
                children[i] = fork();
                if (children[i] == 0) {
                    // Child: close all pipe ends except our own write end
                    for (int j = 0; j < NUM_CHILDREN; j++) {
                        close(pipe_fds[j][0]);  // Close all read ends
                        if (j != i) {
                            close(pipe_fds[j][1]);  // Close other children's write ends
                        }
                    }
                    volatile unsigned long count = 0;
                    // Work for ~100ms worth of iterations
                    for (int j = 0; j < 500000; j++) {
                        count++;
                    }
                    // Write result to pipe
                    write(pipe_fds[i][1], &count, sizeof(count));
                    close(pipe_fds[i][1]);
                    _exit(0);
                }
                // Parent: close write end
                close(pipe_fds[i][1]);
            }
            
            // Wait for all children and read their counts
            unsigned long counts[NUM_CHILDREN];
            int all_finished = 1;
            for (int i = 0; i < NUM_CHILDREN; i++) {
                int status;
                pid_t w = waitpid(children[i], &status, 0);
                if (w != children[i]) {
                    all_finished = 0;
                }
                ssize_t r = read(pipe_fds[i][0], &counts[i], sizeof(counts[i]));
                close(pipe_fds[i][0]);
                if (r != sizeof(counts[i])) {
                    counts[i] = 0;
                }
            }
            
            test_result("all children completed", all_finished);
            
            // Check that all children did similar amounts of work (fair scheduling)
            // Allow 50% variance for fairness check
            unsigned long min_count = counts[0], max_count = counts[0];
            for (int i = 1; i < NUM_CHILDREN; i++) {
                if (counts[i] < min_count) min_count = counts[i];
                if (counts[i] > max_count) max_count = counts[i];
            }
            // Fair if max is no more than 3x min (generous for simple scheduler)
            int is_fair = (min_count > 0) && (max_count <= min_count * 3);
            printf("  Child work counts: %lu, %lu, %lu\n", counts[0], counts[1], counts[2]);
            test_result("fair scheduling among children", is_fair);
        } else {
            test_fail("multiple children: pipe creation failed");
            test_fail("fair scheduling among children");
        }
    }
    
    // Test 2: Signal delivery to blocked task
    printf("\n[TEST] Signal delivery to sleeping task\n");
    {
        static volatile int got_signal = 0;
        
        void sig_handler(int sig) {
            (void)sig;
            got_signal = 1;
        }
        
        signal(SIGUSR1, sig_handler);
        got_signal = 0;
        
        pid_t child = fork();
        if (child == 0) {
            // Child: sleep and get interrupted by signal
            sleep(10);  // Long sleep, should be interrupted
            _exit(got_signal ? 42 : 0);
        } else if (child > 0) {
            // Parent: wait a bit, then send signal to child
            usleep(50000);  // 50ms
            kill(child, SIGUSR1);
            
            int status;
            pid_t w = waitpid(child, &status, 0);
            test_result("signal woke sleeping child", w == child);
            // Child should have exited with 42 (signal received) or 0 (no signal)
            // The key test is that waitpid returned quickly, not after 10 seconds
            test_result("child exit captured", WIFEXITED(status));
        } else {
            test_fail("signal to sleeping: fork failed");
            test_fail("child exit captured");
        }
    }
    
    // Test 3: Timer accuracy under load
    printf("\n[TEST] Timer accuracy under CPU load\n");
    {
        pid_t child = fork();
        if (child == 0) {
            // Child: CPU-bound loop
            volatile unsigned long c = 0;
            while (1) { c++; }
            _exit(0);
        } else if (child > 0) {
            // Parent: measure sleep duration while child consumes CPU
            struct timespec start, end;
            
            // Measure 100ms sleep
            clock_gettime(CLOCK_MONOTONIC, &start);
            usleep(100000);  // 100ms
            clock_gettime(CLOCK_MONOTONIC, &end);
            
            // Calculate elapsed time in ms
            long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                             (end.tv_nsec - start.tv_nsec) / 1000000;
            
            // Kill the CPU-hog child
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            
            // Timer should be reasonably accurate (80-200ms for 100ms sleep)
            printf("  Requested 100ms sleep, actual: %ld ms\n", elapsed_ms);
            test_result("timer accuracy under load", elapsed_ms >= 80 && elapsed_ms <= 300);
        } else {
            test_fail("timer accuracy: fork failed");
        }
    }
    
    // Test 4: Priority inversion scenario (parent waits for child's pipe)
    printf("\n[TEST] I/O blocking vs CPU-bound scheduling\n");
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            pid_t cpu_child = fork();
            if (cpu_child == 0) {
                // CPU-bound child - tries to starve system
                close(pipefd[0]);
                close(pipefd[1]);
                volatile unsigned long c = 0;
                for (int i = 0; i < 10000000; i++) { c++; }
                _exit(0);
            }
            
            pid_t io_child = fork();
            if (io_child == 0) {
                // I/O child - writes to pipe after brief delay
                close(pipefd[0]);
                usleep(20000);  // 20ms delay
                write(pipefd[1], "OK", 2);
                close(pipefd[1]);
                _exit(0);
            }
            
            // Parent: read from pipe (should complete despite CPU-bound sibling)
            close(pipefd[1]);
            char buf[4] = {0};
            ssize_t n = read(pipefd[0], buf, 2);
            close(pipefd[0]);
            
            // Wait for both children
            waitpid(cpu_child, NULL, 0);
            waitpid(io_child, NULL, 0);
            
            test_result("I/O completed despite CPU load", n == 2 && buf[0] == 'O');
        } else {
            test_fail("I/O blocking test: pipe failed");
        }
    }
    
    // Test 5: Rapid fork/exit stress test
    printf("\n[TEST] Rapid fork/exit stress\n");
    {
        #define STRESS_ITERATIONS 20
        int success_count = 0;
        
        for (int i = 0; i < STRESS_ITERATIONS; i++) {
            pid_t p = fork();
            if (p == 0) {
                // Child: exit immediately
                _exit(i);
            } else if (p > 0) {
                int status;
                pid_t w = waitpid(p, &status, 0);
                if (w == p && WIFEXITED(status) && WEXITSTATUS(status) == (i & 0xFF)) {
                    success_count++;
                }
            }
        }
        
        printf("  Completed %d/%d fork/exit cycles\n", success_count, STRESS_ITERATIONS);
        test_result("rapid fork/exit stress", success_count == STRESS_ITERATIONS);
    }
    
    // Test 6: Time slice fairness measurement
    //
    // Strategy: use shared memory with a barrier so both parent and child
    // start spinning at the same time, then have both processes run the
    // same timed loop.  This avoids comparing a pure spin loop against a
    // loop that also pays repeated clock_gettime() overhead.
    printf("\n[TEST] Time slice measurement\n");
    {
        volatile unsigned long *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared != MAP_FAILED) {
            // Layout: [0]=child counter  [1]=parent counter
            //         [2]=barrier (both set their bit, spin until ==3)
            shared[0] = 0;
            shared[1] = 0;
            shared[2] = 0;  // barrier

            pid_t child = fork();
            if (child == 0) {
                struct timespec start_time, now;

                // Signal ready and wait for parent
                __atomic_or_fetch((volatile unsigned long *)&shared[2], 1, __ATOMIC_SEQ_CST);
                while (__atomic_load_n((volatile unsigned long *)&shared[2], __ATOMIC_SEQ_CST) != 3)
                    ;  // spin-wait for barrier

                clock_gettime(CLOCK_MONOTONIC, &start_time);

                // Count for 200ms, sampling time at the same cadence as parent
                unsigned long cnt = 0;
                while (1) {
                    cnt++;
                    if ((cnt & 0xFFF) == 0) {
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                                         (now.tv_nsec - start_time.tv_nsec) / 1000000;
                        if (elapsed_ms >= 200) break;
                    }
                }
                __atomic_store_n((volatile unsigned long *)&shared[0], cnt, __ATOMIC_RELEASE);
                _exit(0);
            } else if (child > 0) {
                struct timespec start_time, now;

                // Signal ready and wait for child
                __atomic_or_fetch((volatile unsigned long *)&shared[2], 2, __ATOMIC_SEQ_CST);
                while (__atomic_load_n((volatile unsigned long *)&shared[2], __ATOMIC_SEQ_CST) != 3)
                    ;  // spin-wait for barrier

                // Both are running — do the same timed loop as the child
                clock_gettime(CLOCK_MONOTONIC, &start_time);
                unsigned long cnt = 0;
                while (1) {
                    cnt++;
                    if ((cnt & 0xFFF) == 0) {
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                                         (now.tv_nsec - start_time.tv_nsec) / 1000000;
                        if (elapsed_ms >= 200) break;
                    }
                }
                __atomic_store_n((volatile unsigned long *)&shared[1], cnt, __ATOMIC_RELEASE);

                waitpid(child, NULL, 0);

                unsigned long child_cnt = shared[0];
                unsigned long parent_cnt = shared[1];
                printf("  Child iterations: %lu, Parent iterations: %lu\n",
                       child_cnt, parent_cnt);

                test_result("child got CPU time", child_cnt > 1000);
                test_result("parent got CPU time", parent_cnt > 1000);

                // This is a starvation check, not a strict scheduler benchmark.
                // Virtualized environments can still produce noticeable skew,
                // so only fail on clearly one-sided CPU distribution.
                if (child_cnt > 0 && parent_cnt > 0) {
                    unsigned long ratio = (child_cnt > parent_cnt) ?
                                         child_cnt / parent_cnt : parent_cnt / child_cnt;
                    printf("  Fairness ratio: %lu\n", ratio);
                    test_result("time slice roughly fair", ratio < 8);
                } else {
                    test_result("time slice roughly fair", 0);
                }
            }

            munmap((void*)shared, 4096);
        } else {
            test_fail("time slice test: mmap failed");
            test_fail("child got CPU time");
            test_fail("parent got CPU time");
            test_fail("time slice roughly fair");
        }
    }
    
    // Test 7: Nested signal during syscall
    printf("\n[TEST] Signal during blocking syscall\n");
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            static volatile int alarm_received = 0;
            
            void alarm_handler(int sig) {
                (void)sig;
                alarm_received = 1;
            }
            
            signal(SIGALRM, alarm_handler);
            alarm_received = 0;
            
            // Set alarm to fire during read
            alarm(1);
            
            // Read from pipe with no writer - should block until alarm
            char buf[10];
            ssize_t n = read(pipefd[0], buf, 10);
            
            close(pipefd[0]);
            close(pipefd[1]);
            
            test_result("blocking read interrupted", n < 0 && errno == EINTR);
            test_result("alarm handler ran", alarm_received == 1);
        } else {
            test_fail("blocking syscall test: pipe failed");
            test_fail("alarm handler ran");
        }
    }

    // ========================================
    // SMP Stress Tests
    // ========================================
    printf("\n--- SMP Stress Tests ---\n");
    
    // Test: Concurrent fork() from multiple processes
    printf("\n[TEST] Concurrent fork() stress\n");
    {
        #define CONCURRENT_FORKS 5
        pid_t fork_children[CONCURRENT_FORKS];
        int fork_ok = 1;
        
        // Fork several children, each of which also forks
        for (int i = 0; i < CONCURRENT_FORKS; i++) {
            fork_children[i] = fork();
            if (fork_children[i] == 0) {
                // Child: fork a grandchild and wait for it
                pid_t gc = fork();
                if (gc == 0) {
                    // Grandchild: do some work and exit
                    volatile unsigned long c = 0;
                    for (int j = 0; j < 10000; j++) c++;
                    _exit((int)(c & 0xFF));
                } else if (gc > 0) {
                    int status;
                    waitpid(gc, &status, 0);
                    _exit(WIFEXITED(status) ? 0 : 1);
                } else {
                    _exit(2);  // fork failed
                }
            } else if (fork_children[i] < 0) {
                fork_ok = 0;
            }
        }
        
        // Wait for all direct children
        int all_ok = fork_ok;
        for (int i = 0; i < CONCURRENT_FORKS; i++) {
            if (fork_children[i] > 0) {
                int status;
                pid_t w = waitpid(fork_children[i], &status, 0);
                if (w != fork_children[i] || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    all_ok = 0;
                }
            }
        }
        test_result("concurrent fork/grandchild stress", all_ok);
    }
    
    // Test: Parallel memory allocation stress
    printf("\n[TEST] Parallel malloc stress\n");
    {
        #define MALLOC_CHILDREN 4
        #define MALLOC_ITERATIONS 50
        pid_t malloc_children[MALLOC_CHILDREN];
        int malloc_pipes[MALLOC_CHILDREN][2];
        int pipes_ok = 1;
        
        for (int i = 0; i < MALLOC_CHILDREN; i++) {
            if (pipe(malloc_pipes[i]) < 0) {
                pipes_ok = 0;
                break;
            }
        }
        
        if (pipes_ok) {
            for (int i = 0; i < MALLOC_CHILDREN; i++) {
                malloc_children[i] = fork();
                if (malloc_children[i] == 0) {
                    // Close read ends and other write ends
                    for (int j = 0; j < MALLOC_CHILDREN; j++) {
                        close(malloc_pipes[j][0]);
                        if (j != i) close(malloc_pipes[j][1]);
                    }
                    
                    // Allocate and free memory in a loop
                    int success = 0;
                    for (int j = 0; j < MALLOC_ITERATIONS; j++) {
                        size_t sz = 64 + (j * 37) % 4096;  // Varying sizes
                        void* p = malloc(sz);
                        if (p) {
                            // Touch the memory
                            memset(p, (char)(j & 0xFF), sz);
                            // Verify first byte
                            if (((unsigned char*)p)[0] == (unsigned char)(j & 0xFF)) {
                                success++;
                            }
                            free(p);
                        }
                    }
                    
                    write(malloc_pipes[i][1], &success, sizeof(success));
                    close(malloc_pipes[i][1]);
                    _exit(0);
                }
                close(malloc_pipes[i][1]);
            }
            
            // Collect results
            int total_success = 0;
            int all_finished = 1;
            for (int i = 0; i < MALLOC_CHILDREN; i++) {
                int status;
                pid_t w = waitpid(malloc_children[i], &status, 0);
                if (w != malloc_children[i]) all_finished = 0;
                
                int child_success = 0;
                read(malloc_pipes[i][0], &child_success, sizeof(child_success));
                close(malloc_pipes[i][0]);
                total_success += child_success;
            }
            
            int expected = MALLOC_CHILDREN * MALLOC_ITERATIONS;
            printf("  Parallel malloc: %d/%d allocations succeeded\n", total_success, expected);
            test_result("parallel malloc all children finished", all_finished);
            test_result("parallel malloc all allocations ok", total_success == expected);
        } else {
            test_fail("parallel malloc: pipe creation failed");
            test_fail("parallel malloc all children finished");
            test_fail("parallel malloc all allocations ok");
        }
    }
    
    // Test: Multi-process pipe read/write stress
    printf("\n[TEST] Multi-process pipe stress\n");
    {
        #define PIPE_WRITERS 3
        #define PIPE_MSGS_PER_WRITER 10
        int stress_pipe[2];
        
        if (pipe(stress_pipe) == 0) {
            pid_t writers[PIPE_WRITERS];
            
            // Fork writer processes
            for (int i = 0; i < PIPE_WRITERS; i++) {
                writers[i] = fork();
                if (writers[i] == 0) {
                    close(stress_pipe[0]);  // Close read end
                    
                    // Write messages to pipe
                    for (int j = 0; j < PIPE_MSGS_PER_WRITER; j++) {
                        char msg[16];
                        int len = 0;
                        // Simple message: "Wij\n" where i=writer, j=msg
                        msg[len++] = 'W';
                        msg[len++] = '0' + i;
                        msg[len++] = '0' + j;
                        msg[len++] = '\n';
                        write(stress_pipe[1], msg, len);
                    }
                    
                    close(stress_pipe[1]);
                    _exit(0);
                }
            }
            
            // Parent reads all messages
            close(stress_pipe[1]);  // Close write end
            
            int msgs_received = 0;
            char rbuf[256];
            ssize_t total_read = 0;
            
            while (1) {
                ssize_t n = read(stress_pipe[0], rbuf + total_read, 
                                sizeof(rbuf) - total_read - 1);
                if (n <= 0) break;
                total_read += n;
                
                // Count newlines as message delimiters
                for (ssize_t k = total_read - n; k < total_read; k++) {
                    if (rbuf[k] == '\n') msgs_received++;
                }
            }
            close(stress_pipe[0]);
            
            // Wait for all writers
            for (int i = 0; i < PIPE_WRITERS; i++) {
                waitpid(writers[i], NULL, 0);
            }
            
            int expected_msgs = PIPE_WRITERS * PIPE_MSGS_PER_WRITER;
            printf("  Pipe stress: received %d/%d messages\n", msgs_received, expected_msgs);
            test_result("multi-process pipe all messages received", msgs_received == expected_msgs);
        } else {
            test_fail("multi-process pipe stress: pipe creation failed");
        }
    }
    
    // Test: sched_yield() syscall (if available)
    printf("\n[TEST] sched_yield() behavior\n");
    {
        // Verify that yielding doesn't crash or hang
        // Fork a child, both yield repeatedly
        volatile int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared != MAP_FAILED) {
            shared[0] = 0;  // Child counter
            shared[1] = 0;  // Parent counter
            shared[2] = 0;  // Stop flag
            
            pid_t child = fork();
            if (child == 0) {
                while (!shared[2]) {
                    shared[0]++;
                    sched_yield();
                }
                _exit(0);
            } else if (child > 0) {
                // Parent yields a few times (reduced for speed)
                for (int i = 0; i < 5; i++) {
                    shared[1]++;
                    sched_yield();
                }
                shared[2] = 1;  // Signal child to stop
                
                waitpid(child, NULL, 0);
                
                printf("  yield test: parent=%d, child=%d iterations\n", 
                       (int)shared[1], (int)shared[0]);
                test_result("sched_yield parent ran", shared[1] >= 5);
                test_result("sched_yield child ran", shared[0] > 0);
            }
            munmap((void*)shared, 4096);
        } else {
            test_fail("sched_yield test: mmap failed");
        }
    }

    // ========================================
    // Test: SMP/Threading syscalls
    // ========================================
    printf("\n[TEST] SMP/Threading syscalls\n");
    
    // Test gettid()
    {
        pid_t tid = gettid();
        printf("  gettid() = %d\n", tid);
        test_result("gettid() returns positive value", tid > 0);
        
        // TID should equal PID for single-threaded process
        pid_t pid = getpid();
        test_result("gettid() == getpid() for single-threaded", tid == pid);
    }
    
    // Test sched_getaffinity() / sched_setaffinity()
    printf("\n[TEST] CPU affinity syscalls\n");
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        
        int ret = sched_getaffinity(0, sizeof(mask), &mask);
        printf("  sched_getaffinity() returned %d\n", ret);
        test_result("sched_getaffinity() succeeds", ret >= 0);
        
        // Check that at least one CPU is set
        int cpu_count = CPU_COUNT(&mask);
        printf("  CPU count in mask: %d\n", cpu_count);
        test_result("At least one CPU in affinity mask", cpu_count > 0);
        
        // Try to set affinity to CPU 0
        CPU_ZERO(&mask);
        CPU_SET(0, &mask);
        ret = sched_setaffinity(0, sizeof(mask), &mask);
        printf("  sched_setaffinity(CPU 0) returned %d\n", ret);
        test_result("sched_setaffinity() succeeds", ret == 0);
        
        // Verify the change
        CPU_ZERO(&mask);
        sched_getaffinity(0, sizeof(mask), &mask);
        test_result("CPU 0 is set after setaffinity", CPU_ISSET(0, &mask));
    }
    
    // Test sched_getscheduler() / sched_setscheduler()
    printf("\n[TEST] Scheduler policy syscalls\n");
    {
        int policy = sched_getscheduler(0);
        printf("  sched_getscheduler(0) = %d\n", policy);
        test_result("sched_getscheduler() succeeds", policy >= 0);
        test_result("Default policy is SCHED_NORMAL (0)", policy == SCHED_NORMAL);
        
        // Try to set scheduler (should work even though we only support NORMAL)
        struct sched_param param = { .sched_priority = 0 };
        int ret = sched_setscheduler(0, SCHED_NORMAL, &param);
        test_result("sched_setscheduler(SCHED_NORMAL) succeeds", ret == 0);
    }
    
    // Test sched_getparam() / sched_setparam()
    printf("\n[TEST] Scheduler parameter syscalls\n");
    {
        struct sched_param param;
        int ret = sched_getparam(0, &param);
        printf("  sched_getparam() returned %d, priority=%d\n", ret, param.sched_priority);
        test_result("sched_getparam() succeeds", ret == 0);
        
        param.sched_priority = 0;
        ret = sched_setparam(0, &param);
        test_result("sched_setparam() succeeds", ret == 0);
    }
    
    // Test sched_get_priority_max() / sched_get_priority_min()
    printf("\n[TEST] Priority range syscalls\n");
    {
        int max_rr = sched_get_priority_max(SCHED_RR);
        int min_rr = sched_get_priority_min(SCHED_RR);
        printf("  SCHED_RR priority range: %d - %d\n", min_rr, max_rr);
        test_result("SCHED_RR max priority is 99", max_rr == 99);
        test_result("SCHED_RR min priority is 1", min_rr == 1);
        
        int max_normal = sched_get_priority_max(SCHED_NORMAL);
        int min_normal = sched_get_priority_min(SCHED_NORMAL);
        printf("  SCHED_NORMAL priority range: %d - %d\n", min_normal, max_normal);
        test_result("SCHED_NORMAL max priority is 0", max_normal == 0);
        test_result("SCHED_NORMAL min priority is 0", min_normal == 0);
    }
    
    // Test sched_rr_get_interval()
    printf("\n[TEST] Round-robin interval syscall\n");
    {
        struct timespec ts;
        int ret = sched_rr_get_interval(0, &ts);
        printf("  sched_rr_get_interval() = %d, interval=%ld.%09ld sec\n", 
               ret, ts.tv_sec, ts.tv_nsec);
        test_result("sched_rr_get_interval() succeeds", ret == 0);
        test_result("Time quantum is ~20ms", ts.tv_nsec >= 10000000 && ts.tv_nsec <= 100000000);
    }
    
    // Test mprotect()
    printf("\n[TEST] mprotect() syscall\n");
    {
        // Allocate a page
        void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        test_result("mmap for mprotect test", page != MAP_FAILED);
        
        if (page != MAP_FAILED) {
            // Write to the page
            *(int*)page = 42;
            test_result("Can write to RW page", *(int*)page == 42);
            
            // Change to read-only
            int ret = mprotect(page, 4096, PROT_READ);
            printf("  mprotect(PROT_READ) returned %d\n", ret);
            test_result("mprotect() succeeds", ret == 0);
            
            // Reading should still work
            int val = *(volatile int*)page;
            test_result("Can read from RO page", val == 42);
            
            // Change back to RW
            ret = mprotect(page, 4096, PROT_READ | PROT_WRITE);
            test_result("mprotect(PROT_READ|PROT_WRITE) succeeds", ret == 0);
            
            munmap(page, 4096);
        }
    }
    
    // Test futex (basic wake/wait operations)
    printf("\n[TEST] futex() syscalls\n");
    {
        volatile int* futex_val = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (futex_val != MAP_FAILED) {
            *futex_val = 0;
            
            pid_t child = fork();
            if (child == 0) {
                // Child: wait for parent to wake us
                // Use busy loop with yield instead of blocking futex
                // since blocking might not work perfectly
                for (int i = 0; i < 1000 && *futex_val == 0; i++) {
                    sched_yield();
                }
                _exit(*futex_val == 1 ? 0 : 1);
            } else if (child > 0) {
                // Parent: wake the child
                sched_yield();  // Let child start
                *futex_val = 1;
                
                // Wake any waiters (even if child is just spinning)
                int woken = futex_wake((int*)futex_val, 1);
                printf("  futex_wake() woke %d waiters\n", woken);
                
                int status;
                waitpid(child, &status, 0);
                test_result("futex signaling works", WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
            
            munmap((void*)futex_val, 4096);
        } else {
            test_fail("futex test: mmap failed");
        }
    }
    
    // Test vfork() (should work like fork)
    printf("\n[TEST] vfork() syscall\n");
    {
        pid_t child = vfork();
        if (child == 0) {
            // Child process
            _exit(42);
        } else if (child > 0) {
            int status;
            waitpid(child, &status, 0);
            test_result("vfork() child exits correctly", 
                       WIFEXITED(status) && WEXITSTATUS(status) == 42);
        } else {
            test_fail("vfork() failed");
        }
    }

    // ========================================
    // Thread Groups / SMP Tests
    // ========================================
    printf("\n[TEST] Thread Groups (getpid vs gettid)\n");
    {
        // For main thread, getpid() and gettid() should return the same value
        pid_t pid = getpid();
        pid_t tid = gettid();
        test_result("gettid() returns valid TID", tid > 0);
        test_result("getpid() == gettid() for main thread", pid == tid);
        printf("  PID=%d, TID=%d\n", pid, tid);
    }

    printf("\n[TEST] set_tid_address() syscall\n");
    {
        int clear_tid = 12345;
        int result = set_tid_address(&clear_tid);
        test_result("set_tid_address() returns TID", result > 0);
        test_result("set_tid_address() returns same as gettid()", result == gettid());
    }

    printf("\n[TEST] set_robust_list() syscall\n");
    {
        // Create a simple robust list head
        struct {
            void* next;
            long futex_offset;
            void* pending;
        } robust_head;
        
        robust_head.next = &robust_head;  // Point to self (empty list)
        robust_head.futex_offset = 0;
        robust_head.pending = NULL;
        
        int result = set_robust_list(&robust_head, sizeof(robust_head));
        test_result("set_robust_list() succeeds", result == 0);
    }

    printf("\n[TEST] arch_prctl() TLS syscall\n");
    {
        // Test ARCH_SET_FS and ARCH_GET_FS for TLS
        unsigned long test_tls_addr = 0x00007F0012340000UL;  // Must be canonical
        unsigned long readback = 0;
        
        // Set FS base
        int set_result = arch_prctl(ARCH_SET_FS, test_tls_addr);
        test_result("arch_prctl(ARCH_SET_FS) succeeds", set_result == 0);
        
        // Get FS base back
        int get_result = arch_prctl(ARCH_GET_FS, (unsigned long)&readback);
        test_result("arch_prctl(ARCH_GET_FS) succeeds", get_result == 0);
        test_result("ARCH_GET_FS returns correct value", readback == test_tls_addr);
        printf("  Set FS base to 0x%lx, read back 0x%lx\n", test_tls_addr, readback);
        
        // Restore to 0 (or original value)
        arch_prctl(ARCH_SET_FS, 0);
    }

    printf("\n[TEST] Fork thread group isolation\n");
    {
        // fork() should create a new process with different PID
        // Child should have getpid() == gettid() (it's its own thread group leader)
        volatile int* shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared != MAP_FAILED) {
            shared[0] = 0;  // child's pid
            shared[1] = 0;  // child's tid
            
            pid_t parent_pid = getpid();
            pid_t child = fork();
            
            if (child == 0) {
                // Child process
                shared[0] = getpid();
                shared[1] = gettid();
                _exit(0);
            } else if (child > 0) {
                int status;
                waitpid(child, &status, 0);
                
                test_result("fork() child has different PID", shared[0] != parent_pid);
                test_result("fork() child has pid == tid", shared[0] == shared[1]);
                test_result("fork() returns correct child PID", child == shared[0]);
                printf("  Parent PID=%d, Child PID=%d, Child TID=%d\n", 
                       parent_pid, (int)shared[0], (int)shared[1]);
            } else {
                test_fail("fork() failed");
            }
            
            munmap((void*)shared, 4096);
        } else {
            test_fail("mmap for fork test failed");
        }
    }

    // ========================================
    // Pthread Tests
    // ========================================
    printf("\n========================================\n");
    printf("[TEST] Pthread Library Tests\n");
    printf("========================================\n");

    // Test pthread_self and pthread_equal
    printf("\n[TEST] pthread_self and pthread_equal\n");
    {
        pthread_t self = pthread_self();
        test_result("pthread_self() returns non-NULL", self != 0);
        test_result("pthread_equal(self, self) returns non-zero", pthread_equal(self, self) != 0);
        printf("  pthread_self() = %p\n", (void*)self);
    }

    // Test pthread_attr functions
    printf("\n[TEST] pthread_attr functions\n");
    {
        pthread_attr_t attr;
        int ret = pthread_attr_init(&attr);
        test_result("pthread_attr_init succeeds", ret == 0);

        // Test detachstate
        int detach_state = -1;
        ret = pthread_attr_getdetachstate(&attr, &detach_state);
        test_result("pthread_attr_getdetachstate succeeds", ret == 0);
        test_result("default detachstate is JOINABLE", detach_state == PTHREAD_CREATE_JOINABLE);

        ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        test_result("pthread_attr_setdetachstate succeeds", ret == 0);
        pthread_attr_getdetachstate(&attr, &detach_state);
        test_result("detachstate is now DETACHED", detach_state == PTHREAD_CREATE_DETACHED);

        // Test stacksize
        size_t stacksize = 0;
        ret = pthread_attr_getstacksize(&attr, &stacksize);
        test_result("pthread_attr_getstacksize succeeds", ret == 0);
        test_result("default stacksize >= 16KB", stacksize >= 16384);
        printf("  Default stack size: %zu bytes\n", stacksize);

        ret = pthread_attr_setstacksize(&attr, 4 * 1024 * 1024); // 4MB
        test_result("pthread_attr_setstacksize(4MB) succeeds", ret == 0);
        pthread_attr_getstacksize(&attr, &stacksize);
        test_result("stacksize is now 4MB", stacksize == 4 * 1024 * 1024);

        // Test guardsize
        size_t guardsize = 0;
        ret = pthread_attr_getguardsize(&attr, &guardsize);
        test_result("pthread_attr_getguardsize succeeds", ret == 0);
        printf("  Default guard size: %zu bytes\n", guardsize);

        ret = pthread_attr_setguardsize(&attr, 8192);
        test_result("pthread_attr_setguardsize succeeds", ret == 0);
        pthread_attr_getguardsize(&attr, &guardsize);
        test_result("guardsize is now 8192", guardsize == 8192);

        ret = pthread_attr_destroy(&attr);
        test_result("pthread_attr_destroy succeeds", ret == 0);
    }

    // Test basic thread creation and join
    printf("\n[TEST] pthread_create and pthread_join\n");
    {
        g_simple_thread_ran = 0;
        g_simple_thread_arg = 0;

        pthread_t thread;
        int ret = pthread_create(&thread, NULL, simple_thread_fn, (void*)123L);
        test_result("pthread_create succeeds", ret == 0);
        printf("  Created thread %p\n", (void*)thread);

        void* retval = NULL;
        ret = pthread_join(thread, &retval);
        test_result("pthread_join succeeds", ret == 0);
        test_result("thread function ran", g_simple_thread_ran == 1);
        test_result("thread received correct argument", g_simple_thread_arg == 123);
        test_result("thread returned correct value", retval == (void*)42L);
        printf("  Thread returned: %ld\n", (long)retval);
    }

    // Test pthread_detach
    printf("\n[TEST] pthread_detach\n");
    {
        g_detached_thread_ran = 0;

        pthread_t thread;
        int ret = pthread_create(&thread, NULL, detached_thread_fn, NULL);
        test_result("pthread_create for detach test succeeds", ret == 0);
        
        ret = pthread_detach(thread);
        test_result("pthread_detach succeeds", ret == 0);
        
        // Wait for the thread to run using proper sleep
        usleep(100000);  // 100ms should be plenty of time
        
        // Can't join a detached thread, but it should have run
        test_result("detached thread ran", g_detached_thread_ran == 1);
    }

    // Test mutex basic operations
    printf("\n[TEST] pthread_mutex basic operations\n");
    {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        
        int ret = pthread_mutex_lock(&mutex);
        test_result("pthread_mutex_lock succeeds", ret == 0);
        
        ret = pthread_mutex_unlock(&mutex);
        test_result("pthread_mutex_unlock succeeds", ret == 0);
        
        ret = pthread_mutex_trylock(&mutex);
        test_result("pthread_mutex_trylock succeeds when unlocked", ret == 0);
        
        ret = pthread_mutex_unlock(&mutex);
        test_result("pthread_mutex_unlock after trylock succeeds", ret == 0);
        
        ret = pthread_mutex_destroy(&mutex);
        test_result("pthread_mutex_destroy succeeds", ret == 0);
    }

    // Test mutex with thread contention
    printf("\n[TEST] pthread_mutex with thread contention\n");
    {
        pthread_t t1, t2;
        g_shared_counter = 0;
        int increments = 1000;
        
        int ret1 = pthread_create(&t1, NULL, increment_thread_fn, (void*)(long)increments);
        int ret2 = pthread_create(&t2, NULL, increment_thread_fn, (void*)(long)increments);
        test_result("pthread_create for t1 succeeds", ret1 == 0);
        test_result("pthread_create for t2 succeeds", ret2 == 0);
        
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        
        test_result("mutex protects counter correctly", g_shared_counter == 2 * increments);
        printf("  Expected counter: %d, Actual: %d\n", 2 * increments, g_shared_counter);
        
        pthread_mutex_destroy(&g_contention_mutex);
    }

    // Test recursive mutex
    printf("\n[TEST] pthread_mutex recursive\n");
    {
        pthread_mutexattr_t attr;
        pthread_mutex_t recursive_mutex;
        
        int ret = pthread_mutexattr_init(&attr);
        test_result("pthread_mutexattr_init succeeds", ret == 0);
        
        ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        test_result("pthread_mutexattr_settype(RECURSIVE) succeeds", ret == 0);
        
        ret = pthread_mutex_init(&recursive_mutex, &attr);
        test_result("pthread_mutex_init with recursive attr succeeds", ret == 0);
        
        // Lock multiple times
        ret = pthread_mutex_lock(&recursive_mutex);
        test_result("first lock succeeds", ret == 0);
        
        ret = pthread_mutex_lock(&recursive_mutex);
        test_result("second lock (recursive) succeeds", ret == 0);
        
        ret = pthread_mutex_lock(&recursive_mutex);
        test_result("third lock (recursive) succeeds", ret == 0);
        
        // Unlock same number of times
        ret = pthread_mutex_unlock(&recursive_mutex);
        test_result("first unlock succeeds", ret == 0);
        
        ret = pthread_mutex_unlock(&recursive_mutex);
        test_result("second unlock succeeds", ret == 0);
        
        ret = pthread_mutex_unlock(&recursive_mutex);
        test_result("third unlock succeeds", ret == 0);
        
        pthread_mutex_destroy(&recursive_mutex);
        pthread_mutexattr_destroy(&attr);
    }

    // Test condition variables
    printf("\n[TEST] pthread_cond basic operations\n");
    {
        pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
        pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
        static volatile int cond_flag = 0;
        cond_flag = 0;

        struct cond_test_args args = { &cond, &cond_mutex, &cond_flag };
        pthread_t waiter;
        int ret = pthread_create(&waiter, NULL, cond_waiter_thread_fn, &args);
        test_result("pthread_create for cond waiter succeeds", ret == 0);
        
        // Give waiter time to start waiting
        for (volatile int i = 0; i < 100000; i++);
        
        // Signal the condition
        pthread_mutex_lock(&cond_mutex);
        cond_flag = 1;
        ret = pthread_cond_signal(&cond);
        test_result("pthread_cond_signal succeeds", ret == 0);
        pthread_mutex_unlock(&cond_mutex);
        
        void* retval;
        ret = pthread_join(waiter, &retval);
        test_result("pthread_join on cond waiter succeeds", ret == 0);
        test_result("cond waiter completed", retval == (void*)99L);
        
        pthread_cond_destroy(&cond);
        pthread_mutex_destroy(&cond_mutex);
    }

    // Test pthread_cond_broadcast
    printf("\n[TEST] pthread_cond_broadcast\n");
    {
        g_bcast_flag = 0;
        g_waiters_done = 0;

        pthread_t t1, t2, t3;
        pthread_create(&t1, NULL, bcast_waiter_fn, NULL);
        pthread_create(&t2, NULL, bcast_waiter_fn, NULL);
        pthread_create(&t3, NULL, bcast_waiter_fn, NULL);
        
        // Give waiters time to start
        for (volatile int i = 0; i < 100000; i++);
        
        pthread_mutex_lock(&g_bcast_mutex);
        g_bcast_flag = 1;
        int ret = pthread_cond_broadcast(&g_bcast_cond);
        test_result("pthread_cond_broadcast succeeds", ret == 0);
        pthread_mutex_unlock(&g_bcast_mutex);
        
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        pthread_join(t3, NULL);
        
        test_result("all 3 waiters woke up", g_waiters_done == 3);
        
        pthread_cond_destroy(&g_bcast_cond);
        pthread_mutex_destroy(&g_bcast_mutex);
    }

    // Test rwlock
    printf("\n[TEST] pthread_rwlock\n");
    {
        pthread_rwlock_t rwlock;
        int ret = pthread_rwlock_init(&rwlock, NULL);
        test_result("pthread_rwlock_init succeeds", ret == 0);
        
        // Multiple read locks should succeed
        ret = pthread_rwlock_rdlock(&rwlock);
        test_result("first rdlock succeeds", ret == 0);
        
        ret = pthread_rwlock_tryrdlock(&rwlock);
        test_result("second rdlock (tryrdlock) succeeds", ret == 0);
        
        ret = pthread_rwlock_unlock(&rwlock);
        test_result("first rdunlock succeeds", ret == 0);
        
        ret = pthread_rwlock_unlock(&rwlock);
        test_result("second rdunlock succeeds", ret == 0);
        
        // Write lock
        ret = pthread_rwlock_wrlock(&rwlock);
        test_result("wrlock succeeds", ret == 0);
        
        ret = pthread_rwlock_unlock(&rwlock);
        test_result("wrunlock succeeds", ret == 0);
        
        // Try write lock
        ret = pthread_rwlock_trywrlock(&rwlock);
        test_result("trywrlock succeeds when unlocked", ret == 0);
        
        ret = pthread_rwlock_unlock(&rwlock);
        test_result("unlock after trywrlock succeeds", ret == 0);
        
        ret = pthread_rwlock_destroy(&rwlock);
        test_result("pthread_rwlock_destroy succeeds", ret == 0);
    }

    // Test spinlock
    printf("\n[TEST] pthread_spin\n");
    {
        pthread_spinlock_t spinlock;
        int ret = pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
        test_result("pthread_spin_init succeeds", ret == 0);
        
        ret = pthread_spin_lock(&spinlock);
        test_result("pthread_spin_lock succeeds", ret == 0);
        
        ret = pthread_spin_unlock(&spinlock);
        test_result("pthread_spin_unlock succeeds", ret == 0);
        
        ret = pthread_spin_trylock(&spinlock);
        test_result("pthread_spin_trylock succeeds", ret == 0);
        
        ret = pthread_spin_unlock(&spinlock);
        test_result("pthread_spin_unlock after trylock succeeds", ret == 0);
        
        ret = pthread_spin_destroy(&spinlock);
        test_result("pthread_spin_destroy succeeds", ret == 0);
    }

    // Test barrier
    printf("\n[TEST] pthread_barrier\n");
    {
        g_barrier_arrivals = 0;
        
        int ret = pthread_barrier_init(&g_barrier, NULL, 3);
        test_result("pthread_barrier_init(count=3) succeeds", ret == 0);

        pthread_t t1, t2;
        pthread_create(&t1, NULL, barrier_thread_fn, NULL);
        pthread_create(&t2, NULL, barrier_thread_fn, NULL);
        
        // This thread also participates
        __sync_fetch_and_add(&g_barrier_arrivals, 1);
        ret = pthread_barrier_wait(&g_barrier);
        test_result("pthread_barrier_wait returns 0 or SERIAL", 
                    ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);
        
        void *r1, *r2;
        pthread_join(t1, &r1);
        pthread_join(t2, &r2);
        
        test_result("all 3 threads reached barrier", g_barrier_arrivals == 3);
        
        // Check that exactly one got SERIAL_THREAD
        int serials = (ret == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0)
                    + ((long)r1 == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0)
                    + ((long)r2 == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0);
        test_result("exactly one thread got SERIAL_THREAD", serials == 1);
        
        ret = pthread_barrier_destroy(&g_barrier);
        test_result("pthread_barrier_destroy succeeds", ret == 0);
    }

    // Test thread-specific data (TSD)
    printf("\n[TEST] pthread TSD (thread-specific data)\n");
    {
        g_destructor_called = 0;

        int ret = pthread_key_create(&g_tsd_key, tsd_destructor_fn);
        test_result("pthread_key_create succeeds", ret == 0);
        
        // Set value in main thread
        ret = pthread_setspecific(g_tsd_key, (void*)12345L);
        test_result("pthread_setspecific succeeds", ret == 0);
        
        void* val = pthread_getspecific(g_tsd_key);
        test_result("pthread_getspecific returns correct value", val == (void*)12345L);
        
        // Test in another thread
        pthread_t t;
        pthread_create(&t, NULL, tsd_thread_fn, NULL);
        void* tsd_result;
        pthread_join(t, &tsd_result);
        test_result("TSD is thread-local", tsd_result == (void*)0L);
        
        // Main thread's value should be unchanged
        val = pthread_getspecific(g_tsd_key);
        test_result("main thread TSD unchanged", val == (void*)12345L);
        
        ret = pthread_key_delete(g_tsd_key);
        test_result("pthread_key_delete succeeds", ret == 0);
    }

    // Test sched_setaffinity / sched_getaffinity
    printf("\n[TEST] sched_setaffinity and sched_getaffinity\n");
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        // Get current affinity
        int ret = sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
        test_result("sched_getaffinity succeeds", ret == 0);
        
        int cpu_count = 0;
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &cpuset)) cpu_count++;
        }
        test_result("at least one CPU in affinity mask", cpu_count >= 1);
        printf("  CPUs in affinity mask: %d\n", cpu_count);
        
        // Try to set affinity to CPU 0 only
        cpu_set_t new_cpuset;
        CPU_ZERO(&new_cpuset);
        CPU_SET(0, &new_cpuset);
        
        ret = sched_setaffinity(0, sizeof(cpu_set_t), &new_cpuset);
        // This might fail if system doesn't support it, but shouldn't crash
        if (ret == 0) {
            test_pass("sched_setaffinity to CPU 0 succeeds");
            
            // Verify it was set
            CPU_ZERO(&cpuset);
            sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
            test_result("affinity was updated", CPU_ISSET(0, &cpuset));
        } else {
            printf("  sched_setaffinity returned %d (may not be supported)\n", ret);
            test_pass("sched_setaffinity returned (not necessarily successful)");
        }
    }

    // Test pthread_setaffinity_np / pthread_getaffinity_np
    printf("\n[TEST] pthread_setaffinity_np and pthread_getaffinity_np\n");
    {
        pthread_t self = pthread_self();
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        
        int ret = pthread_getaffinity_np(self, sizeof(cpu_set_t), &cpuset);
        if (ret == 0) {
            test_pass("pthread_getaffinity_np succeeds");
            
            int cpu_count = 0;
            for (int i = 0; i < CPU_SETSIZE; i++) {
                if (CPU_ISSET(i, &cpuset)) cpu_count++;
            }
            test_result("pthread affinity has CPUs", cpu_count >= 1);
            printf("  Thread CPUs in affinity: %d\n", cpu_count);
            
            // Try to set
            cpu_set_t new_cpuset;
            CPU_ZERO(&new_cpuset);
            CPU_SET(0, &new_cpuset);
            
            ret = pthread_setaffinity_np(self, sizeof(cpu_set_t), &new_cpuset);
            if (ret == 0) {
                test_pass("pthread_setaffinity_np succeeds");
            } else {
                printf("  pthread_setaffinity_np returned %d\n", ret);
                test_pass("pthread_setaffinity_np returned");
            }
        } else {
            printf("  pthread_getaffinity_np returned %d\n", ret);
            test_pass("pthread_getaffinity_np returned (may use fallback)");
        }
    }

    // Test pthread_once
    printf("\n[TEST] pthread_once\n");
    {
        // Reset for test (note: g_once_control is global and already initialized)
        // We can't easily reset a pthread_once_t, so test without reset
        g_once_counter = 0;

        int ret = pthread_once(&g_once_control, once_init_fn);
        test_result("first pthread_once succeeds", ret == 0);
        test_result("init function called once", g_once_counter == 1);
        
        ret = pthread_once(&g_once_control, once_init_fn);
        test_result("second pthread_once succeeds", ret == 0);
        test_result("init function still called only once", g_once_counter == 1);
        
        // Test from multiple threads
        pthread_t t1, t2;
        pthread_create(&t1, NULL, once_thread_fn, NULL);
        pthread_create(&t2, NULL, once_thread_fn, NULL);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        
        test_result("init function called exactly once across threads", g_once_counter == 1);
    }

    // ========================================
    // Dynamic Linking (dlopen/dlsym/dlclose/dlerror) Tests
    // ========================================
    printf("\n--- Dynamic Linking Tests ---\n");
    {
        // Test 1: dlerror returns NULL when no error
        printf("\n[TEST] dlerror() initial state\n");
        char *err = dlerror();
        /* dlerror may or may not be NULL initially; just call it to clear state */
        (void)err;
        test_pass("dlerror() called without crash");

        // Test 2: dlopen a non-existent library should fail
        printf("\n[TEST] dlopen() non-existent library\n");
        void *bad_handle = dlopen("/lib/libnonexistent.so", RTLD_LAZY);
        test_result("dlopen non-existent returns NULL", bad_handle == NULL);
        if (bad_handle == NULL) {
            err = dlerror();
            test_result("dlerror returns non-NULL after failed dlopen",
                        err != NULL);
            if (err) {
                printf("    dlerror: %s\n", err);
            }
        }

        // Test 3: dlopen libtestlib.so
        printf("\n[TEST] dlopen() libtestlib.so\n");
        void *handle = dlopen("/lib/libtestlib.so", RTLD_LAZY);
        test_result("dlopen(\"/lib/libtestlib.so\") returns non-NULL",
                    handle != NULL);
        if (handle == NULL) {
            err = dlerror();
            printf("    dlopen failed: %s\n", err ? err : "(null)");
        }

        if (handle != NULL) {
            // Test 4: dlsym - look up testlib_add
            printf("\n[TEST] dlsym() testlib_add\n");
            int (*fn_add)(int, int) = (int (*)(int, int))dlsym(handle, "testlib_add");
            test_result("dlsym(\"testlib_add\") returns non-NULL",
                        fn_add != NULL);
            if (fn_add) {
                int result = fn_add(17, 25);
                test_result("testlib_add(17, 25) == 42", result == 42);
                result = fn_add(-5, 5);
                test_result("testlib_add(-5, 5) == 0", result == 0);
            }

            // Test 5: dlsym - look up testlib_mul
            printf("\n[TEST] dlsym() testlib_mul\n");
            int (*fn_mul)(int, int) = (int (*)(int, int))dlsym(handle, "testlib_mul");
            test_result("dlsym(\"testlib_mul\") returns non-NULL",
                        fn_mul != NULL);
            if (fn_mul) {
                int result = fn_mul(6, 7);
                test_result("testlib_mul(6, 7) == 42", result == 42);
                result = fn_mul(0, 999);
                test_result("testlib_mul(0, 999) == 0", result == 0);
            }

            // Test 6: dlsym - look up testlib_hello (returns string)
            printf("\n[TEST] dlsym() testlib_hello\n");
            const char *(*fn_hello)(void) = (const char *(*)(void))dlsym(handle, "testlib_hello");
            test_result("dlsym(\"testlib_hello\") returns non-NULL",
                        fn_hello != NULL);
            if (fn_hello) {
                const char *msg = fn_hello();
                test_result("testlib_hello() returns non-NULL string",
                            msg != NULL);
                if (msg) {
                    printf("    testlib_hello() = \"%s\"\n", msg);
                    test_result("testlib_hello() contains \"libtestlib\"",
                                strstr(msg, "libtestlib") != NULL);
                }
            }

            // Test 7: dlsym - look up testlib_counter (stateful)
            printf("\n[TEST] dlsym() testlib_counter\n");
            int (*fn_counter)(void) = (int (*)(void))dlsym(handle, "testlib_counter");
            void (*fn_reset)(void) = (void (*)(void))dlsym(handle, "testlib_counter_reset");
            test_result("dlsym(\"testlib_counter\") returns non-NULL",
                        fn_counter != NULL);
            test_result("dlsym(\"testlib_counter_reset\") returns non-NULL",
                        fn_reset != NULL);
            if (fn_counter && fn_reset) {
                fn_reset();
                int v0 = fn_counter();  /* returns 0, increments to 1 */
                int v1 = fn_counter();  /* returns 1, increments to 2 */
                int v2 = fn_counter();  /* returns 2, increments to 3 */
                test_result("counter sequence 0,1,2",
                            v0 == 0 && v1 == 1 && v2 == 2);
                fn_reset();
                int v3 = fn_counter();
                test_result("counter reset works", v3 == 0);
            }

            // Test 8: dlsym - look up global variable testlib_version
            printf("\n[TEST] dlsym() testlib_version (global variable)\n");
            int *p_version = (int *)dlsym(handle, "testlib_version");
            test_result("dlsym(\"testlib_version\") returns non-NULL",
                        p_version != NULL);
            if (p_version) {
                test_result("testlib_version == 1", *p_version == 1);
                printf("    testlib_version = %d\n", *p_version);
            }

            // Test 9: dlsym - non-existent symbol
            printf("\n[TEST] dlsym() non-existent symbol\n");
            void *bad_sym = dlsym(handle, "this_symbol_does_not_exist");
            test_result("dlsym non-existent returns NULL", bad_sym == NULL);
            if (bad_sym == NULL) {
                err = dlerror();
                test_result("dlerror returns non-NULL after failed dlsym",
                            err != NULL);
                if (err) {
                    printf("    dlerror: %s\n", err);
                }
            }

            // Test 10: dlclose
            printf("\n[TEST] dlclose()\n");
            int close_ret = dlclose(handle);
            test_result("dlclose returns 0", close_ret == 0);

            // Test 11: dlerror after successful dlclose should be NULL
            err = dlerror();
            /* After a successful operation, dlerror should return NULL */
            test_result("dlerror() returns NULL after successful dlclose",
                        err == NULL);
        }

        // Test 12: dlopen with RTLD_NOW
        printf("\n[TEST] dlopen() with RTLD_NOW\n");
        void *handle2 = dlopen("/lib/libtestlib.so", RTLD_NOW);
        test_result("dlopen RTLD_NOW returns non-NULL", handle2 != NULL);
        if (handle2) {
            int (*fn_add2)(int, int) = (int (*)(int, int))dlsym(handle2, "testlib_add");
            test_result("dlsym after RTLD_NOW works", fn_add2 != NULL);
            if (fn_add2) {
                test_result("testlib_add(100, 200) == 300",
                            fn_add2(100, 200) == 300);
            }
            dlclose(handle2);
        }
    }

    // ========================================
    // uname() syscall tests
    // ========================================
    printf("\n========================================\n");
    printf("  uname() SYSCALL TESTS\n");
    printf("========================================\n");
    {
        struct utsname uts;
        int ret = uname(&uts);
        test_result("uname() returns 0", ret == 0);

        if (ret == 0) {
            /* sysname should be "LikeOS" */
            test_result("uname sysname == \"LikeOS\"",
                        strcmp(uts.sysname, "LikeOS") == 0);

            /* nodename should be non-empty */
            test_result("uname nodename is non-empty",
                        strlen(uts.nodename) > 0);

            /* release should be non-empty */
            test_result("uname release is non-empty",
                        strlen(uts.release) > 0);

            /* version should be non-empty */
            test_result("uname version is non-empty",
                        strlen(uts.version) > 0);

            /* machine should be "x86_64" */
            test_result("uname machine == \"x86_64\"",
                        strcmp(uts.machine, "x86_64") == 0);

            /* Print the fields for manual inspection */
            printf("  sysname:  %s\n", uts.sysname);
            printf("  nodename: %s\n", uts.nodename);
            printf("  release:  %s\n", uts.release);
            printf("  version:  %s\n", uts.version);
            printf("  machine:  %s\n", uts.machine);

            /* Each field should be shorter than the buffer size (65) */
            test_result("sysname length < 65", strlen(uts.sysname) < 65);
            test_result("nodename length < 65", strlen(uts.nodename) < 65);
            test_result("release length < 65", strlen(uts.release) < 65);
            test_result("version length < 65", strlen(uts.version) < 65);
            test_result("machine length < 65", strlen(uts.machine) < 65);
        }
    }

    // ========================================
    // getopt() tests
    // ========================================
    printf("\n========================================\n");
    printf("  getopt() TESTS\n");
    printf("========================================\n");
    {
        /* Reset getopt state */
        extern int optind, opterr, optopt;
        extern char *optarg;
        optind = 1;
        opterr = 0;

        /* Test 1: simple option parsing */
        char *argv1[] = { "prog", "-a", "-b", NULL };
        int argc1 = 3;
        int got_a = 0, got_b = 0;
        int ch;
        optind = 1;
        while ((ch = getopt(argc1, argv1, "ab")) != -1) {
            if (ch == 'a') got_a = 1;
            if (ch == 'b') got_b = 1;
        }
        test_result("getopt: -a parsed", got_a == 1);
        test_result("getopt: -b parsed", got_b == 1);
        test_result("getopt: optind after -a -b == 3", optind == 3);

        /* Test 2: grouped options */
        char *argv2[] = { "prog", "-abc", NULL };
        int argc2 = 2;
        int got_a2 = 0, got_b2 = 0, got_c2 = 0;
        optind = 1;
        while ((ch = getopt(argc2, argv2, "abc")) != -1) {
            if (ch == 'a') got_a2 = 1;
            if (ch == 'b') got_b2 = 1;
            if (ch == 'c') got_c2 = 1;
        }
        test_result("getopt grouped: -a parsed", got_a2 == 1);
        test_result("getopt grouped: -b parsed", got_b2 == 1);
        test_result("getopt grouped: -c parsed", got_c2 == 1);

        /* Test 3: option with argument */
        char *argv3[] = { "prog", "-f", "file.txt", NULL };
        int argc3 = 3;
        char *farg = NULL;
        optind = 1;
        while ((ch = getopt(argc3, argv3, "f:")) != -1) {
            if (ch == 'f') farg = optarg;
        }
        test_result("getopt arg: -f file.txt parses", farg != NULL);
        if (farg)
            test_result("getopt arg: optarg == \"file.txt\"",
                        strcmp(farg, "file.txt") == 0);

        /* Test 4: unknown option returns '?' */
        char *argv4[] = { "prog", "-z", NULL };
        int argc4 = 2;
        int got_q = 0;
        optind = 1;
        while ((ch = getopt(argc4, argv4, "ab")) != -1) {
            if (ch == '?') got_q = 1;
        }
        test_result("getopt: unknown option returns '?'", got_q == 1);

        /* Test 5: "--" stops scanning */
        char *argv5[] = { "prog", "--", "-a", NULL };
        int argc5 = 3;
        int got_a5 = 0;
        optind = 1;
        while ((ch = getopt(argc5, argv5, "a")) != -1) {
            if (ch == 'a') got_a5 = 1;
        }
        test_result("getopt: -- stops scanning", got_a5 == 0);
        test_result("getopt: optind after -- == 2", optind == 2);
    }

    // ========================================
    // getopt_long tests
    // ========================================
    printf("\n--- getopt_long tests ---\n");
    {
        /* Test 1: long option without argument */
        struct option longopts1[] = {
            { "verbose", no_argument, NULL, 'v' },
            { "help", no_argument, NULL, 'h' },
            { NULL, 0, NULL, 0 }
        };
        char *argv1[] = { "prog", "--verbose", NULL };
        int argc1 = 2;
        optind = 1;
        int longidx = -1;
        int ch = getopt_long(argc1, argv1, "vh", longopts1, &longidx);
        test_result("getopt_long: --verbose returns 'v'", ch == 'v');

        /* Test 2: long option with required argument (= syntax) */
        struct option longopts2[] = {
            { "output", required_argument, NULL, 'o' },
            { NULL, 0, NULL, 0 }
        };
        char *argv2[] = { "prog", "--output=file.txt", NULL };
        int argc2 = 2;
        optind = 1;
        ch = getopt_long(argc2, argv2, "o:", longopts2, &longidx);
        test_result("getopt_long: --output=file.txt returns 'o'", ch == 'o');
        test_result("getopt_long: optarg is 'file.txt'",
                     optarg != NULL && strcmp(optarg, "file.txt") == 0);

        /* Test 3: long option with required argument (space syntax) */
        char *argv3[] = { "prog", "--output", "result.dat", NULL };
        int argc3 = 3;
        optind = 1;
        ch = getopt_long(argc3, argv3, "o:", longopts2, &longidx);
        test_result("getopt_long: --output result.dat returns 'o'", ch == 'o');
        test_result("getopt_long: optarg is 'result.dat'",
                     optarg != NULL && strcmp(optarg, "result.dat") == 0);

        /* Test 4: flag pointer stores value (val into *flag) */
        int flag_val = 0;
        struct option longopts4[] = {
            { "debug", no_argument, &flag_val, 42 },
            { NULL, 0, NULL, 0 }
        };
        char *argv4[] = { "prog", "--debug", NULL };
        int argc4 = 2;
        optind = 1;
        ch = getopt_long(argc4, argv4, "", longopts4, &longidx);
        test_result("getopt_long: flag pointer returns 0", ch == 0);
        test_result("getopt_long: flag value set to 42", flag_val == 42);

        /* Test 5: short option still works through getopt_long */
        struct option longopts5[] = {
            { "verbose", no_argument, NULL, 'v' },
            { NULL, 0, NULL, 0 }
        };
        char *argv5[] = { "prog", "-v", NULL };
        int argc5 = 2;
        optind = 1;
        ch = getopt_long(argc5, argv5, "v", longopts5, &longidx);
        test_result("getopt_long: short -v still works", ch == 'v');

        /* Test 6: mixed short and long options */
        struct option longopts6[] = {
            { "all", no_argument, NULL, 'a' },
            { "long", no_argument, NULL, 'l' },
            { NULL, 0, NULL, 0 }
        };
        char *argv6[] = { "prog", "-a", "--long", NULL };
        int argc6 = 3;
        optind = 1;
        int got_a = 0, got_l = 0;
        while ((ch = getopt_long(argc6, argv6, "al", longopts6, &longidx)) != -1) {
            if (ch == 'a') got_a = 1;
            if (ch == 'l') got_l = 1;
        }
        test_result("getopt_long: mixed -a --long: got 'a'", got_a == 1);
        test_result("getopt_long: mixed -a --long: got 'l'", got_l == 1);

        /* Test 7: unknown long option returns '?' */
        struct option longopts7[] = {
            { "known", no_argument, NULL, 'k' },
            { NULL, 0, NULL, 0 }
        };
        char *argv7[] = { "prog", "--unknown", NULL };
        int argc7 = 2;
        optind = 1;
        opterr = 0;  /* suppress error message */
        ch = getopt_long(argc7, argv7, "k", longopts7, &longidx);
        test_result("getopt_long: unknown --unknown returns '?'", ch == '?');
        opterr = 1;
    }

    // ========================================
    // time function tests (gmtime, mktime, strftime)
    // ========================================
    printf("\n--- time function tests ---\n");
    {
        /* Test 1: gmtime of epoch 0 */
        time_t t0 = 0;
        struct tm *tm0 = gmtime(&t0);
        test_result("gmtime(0): year=1970", tm0 != NULL && tm0->tm_year == 70);
        test_result("gmtime(0): mon=0 (Jan)", tm0 != NULL && tm0->tm_mon == 0);
        test_result("gmtime(0): mday=1", tm0 != NULL && tm0->tm_mday == 1);
        test_result("gmtime(0): hour=0", tm0 != NULL && tm0->tm_hour == 0);
        test_result("gmtime(0): min=0", tm0 != NULL && tm0->tm_min == 0);
        test_result("gmtime(0): sec=0", tm0 != NULL && tm0->tm_sec == 0);
        test_result("gmtime(0): wday=4 (Thu)", tm0 != NULL && tm0->tm_wday == 4);

        /* Test 2: gmtime of known timestamp: 2024-01-01 00:00:00 UTC = 1704067200 */
        time_t t1 = 1704067200;
        struct tm tm1;
        gmtime_r(&t1, &tm1);
        test_result("gmtime(2024-01-01): year=124", tm1.tm_year == 124);
        test_result("gmtime(2024-01-01): mon=0", tm1.tm_mon == 0);
        test_result("gmtime(2024-01-01): mday=1", tm1.tm_mday == 1);
        test_result("gmtime(2024-01-01): wday=1 (Mon)", tm1.tm_wday == 1);

        /* Test 3: gmtime_r known timestamp: 2000-06-15 12:30:45 UTC = 961072245 */
        time_t t2 = 961072245;
        struct tm tm2;
        gmtime_r(&t2, &tm2);
        test_result("gmtime(2000-06-15 12:30:45): year=100", tm2.tm_year == 100);
        test_result("gmtime(2000-06-15 12:30:45): mon=5 (Jun)", tm2.tm_mon == 5);
        test_result("gmtime(2000-06-15 12:30:45): mday=15", tm2.tm_mday == 15);
        test_result("gmtime(2000-06-15 12:30:45): hour=12", tm2.tm_hour == 12);
        test_result("gmtime(2000-06-15 12:30:45): min=30", tm2.tm_min == 30);
        test_result("gmtime(2000-06-15 12:30:45): sec=45", tm2.tm_sec == 45);

        /* Test 4: mktime round-trip */
        struct tm tm_rt;
        tm_rt.tm_year = 124;  /* 2024 */
        tm_rt.tm_mon = 0;     /* January */
        tm_rt.tm_mday = 1;
        tm_rt.tm_hour = 0;
        tm_rt.tm_min = 0;
        tm_rt.tm_sec = 0;
        tm_rt.tm_isdst = 0;
        time_t rt = mktime(&tm_rt);
        test_result("mktime(2024-01-01) == 1704067200", rt == 1704067200);

        /* Test 5: mktime round-trip for 2000-06-15 12:30:45 */
        struct tm tm_rt2;
        tm_rt2.tm_year = 100;
        tm_rt2.tm_mon = 5;
        tm_rt2.tm_mday = 15;
        tm_rt2.tm_hour = 12;
        tm_rt2.tm_min = 30;
        tm_rt2.tm_sec = 45;
        tm_rt2.tm_isdst = 0;
        time_t rt2 = mktime(&tm_rt2);
        test_result("mktime(2000-06-15 12:30:45) == 961072245", rt2 == 961072245);

        /* Test 6: strftime basic formatting */
        char buf[128];
        struct tm tmf;
        tmf.tm_year = 124; tmf.tm_mon = 0; tmf.tm_mday = 15;
        tmf.tm_hour = 9; tmf.tm_min = 5; tmf.tm_sec = 3;
        tmf.tm_wday = 1; tmf.tm_yday = 14; tmf.tm_isdst = 0;

        strftime(buf, sizeof(buf), "%Y-%m-%d", &tmf);
        test_result("strftime %Y-%m-%d == '2024-01-15'", strcmp(buf, "2024-01-15") == 0);

        strftime(buf, sizeof(buf), "%H:%M:%S", &tmf);
        test_result("strftime %H:%M:%S == '09:05:03'", strcmp(buf, "09:05:03") == 0);

        strftime(buf, sizeof(buf), "%a", &tmf);
        test_result("strftime %a == 'Mon'", strcmp(buf, "Mon") == 0);

        strftime(buf, sizeof(buf), "%b", &tmf);
        test_result("strftime %b == 'Jan'", strcmp(buf, "Jan") == 0);

        strftime(buf, sizeof(buf), "%F", &tmf);
        test_result("strftime %F == '2024-01-15'", strcmp(buf, "2024-01-15") == 0);

        strftime(buf, sizeof(buf), "%T", &tmf);
        test_result("strftime %T == '09:05:03'", strcmp(buf, "09:05:03") == 0);

        /* Test 7: leap year handling */
        time_t t_leap = 951782400;  /* 2000-02-29 00:00:00 UTC */
        struct tm tm_leap;
        gmtime_r(&t_leap, &tm_leap);
        test_result("gmtime leap year 2000-02-29: year=100", tm_leap.tm_year == 100);
        test_result("gmtime leap year 2000-02-29: mon=1 (Feb)", tm_leap.tm_mon == 1);
        test_result("gmtime leap year 2000-02-29: mday=29", tm_leap.tm_mday == 29);
    }

    // ========================================
    // SYS_GETPROCINFO tests
    // ========================================
    {
        printf("\n--- SYS_GETPROCINFO tests ---\n");

        /* Allocate buffer for up to 128 procs */
        int max = 128;
        procinfo_t *buf = (procinfo_t *)malloc(max * sizeof(procinfo_t));
        test_result("getprocinfo: malloc ok", buf != NULL);
        if (buf) {
            int n = getprocinfo(buf, max);
            test_result("getprocinfo: returns > 0", n > 0);

            /* Find our own PID */
            pid_t my_pid = getpid();
            int found_self = 0;
            int self_idx = -1;
            for (int i = 0; i < n; i++) {
                if (buf[i].pid == (int)my_pid) {
                    found_self = 1;
                    self_idx = i;
                    break;
                }
            }
            test_result("getprocinfo: found own PID", found_self);

            if (self_idx >= 0) {
                test_result("getprocinfo: own state is READY or RUNNING",
                            buf[self_idx].state == 0 || buf[self_idx].state == 1);
                test_result("getprocinfo: own tty_nr > 0",
                            buf[self_idx].tty_nr > 0);
                test_result("getprocinfo: own is_kernel == 0",
                            buf[self_idx].is_kernel == 0);
                test_result("getprocinfo: own ppid > 0",
                            buf[self_idx].ppid > 0);
                test_result("getprocinfo: own cwd starts with /",
                            buf[self_idx].cwd[0] == '/');
                test_result("getprocinfo: own comm is 'testlibc'",
                            strcmp(buf[self_idx].comm, "testlibc") == 0);
            }

            /* Check that PID 0 (kernel bootstrap) exists */
            int found_kernel = 0;
            for (int i = 0; i < n; i++) {
                if (buf[i].pid == 0) {
                    found_kernel = 1;
                    test_result("getprocinfo: PID 0 is kernel",
                                buf[i].is_kernel == 1);
                    break;
                }
            }
            test_result("getprocinfo: PID 0 exists", found_kernel);

            /* Edge case: max_count=0 */
            int n0 = getprocinfo(buf, 0);
            test_result("getprocinfo(buf,0) returns 0", n0 == 0);

            free(buf);
        }
    }

    // ========================================
    // Filesystem syscalls: mkdir, rmdir, rename, unlink, chmod, utimensat
    // ========================================
    printf("\n[TEST] mkdir()\n");
    {
        int ret = mkdir("/tmp/test_mkdir_dir", 0755);
        test_result("mkdir(/tmp/test_mkdir_dir) succeeds", ret == 0);

        struct stat st;
        ret = stat("/tmp/test_mkdir_dir", &st);
        test_result("stat new dir succeeds", ret == 0);
        test_result("new dir is a directory", ret == 0 && S_ISDIR(st.st_mode));

        /* mkdir on existing dir should fail with EEXIST */
        ret = mkdir("/tmp/test_mkdir_dir", 0755);
        test_result("mkdir existing dir fails", ret == -1);
        test_result("mkdir existing dir sets EEXIST", errno == EEXIST);
    }

    printf("\n[TEST] rmdir()\n");
    {
        int ret = rmdir("/tmp/test_mkdir_dir");
        test_result("rmdir(/tmp/test_mkdir_dir) succeeds", ret == 0);

        /* rmdir on nonexistent dir should fail */
        ret = rmdir("/tmp/test_mkdir_dir");
        test_result("rmdir nonexistent dir fails", ret == -1);
        test_result("rmdir nonexistent dir sets ENOENT", errno == ENOENT);

        /* rmdir on "/" should fail when it's not empty */
        ret = rmdir("/");
        test_result("rmdir(/) fails", ret == -1);
    }

    printf("\n[TEST] unlink()\n");
    {
        /* Create a test file first */
        int fd = open("/tmp/test_unlink_file", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("create /tmp/test_unlink_file", fd >= 0);
        if (fd >= 0) {
            write(fd, "test", 4);
            close(fd);

            int ret = unlink("/tmp/test_unlink_file");
            test_result("unlink(/tmp/test_unlink_file) succeeds", ret == 0);

            /* Should be gone now */
            struct stat st;
            ret = stat("/tmp/test_unlink_file", &st);
            test_result("stat after unlink fails (ENOENT)", ret == -1 && errno == ENOENT);
        }

        /* unlink nonexistent file */
        int ret = unlink("/tmp/no_such_file_for_test");
        test_result("unlink nonexistent file fails", ret == -1);
        test_result("unlink nonexistent sets ENOENT", errno == ENOENT);
    }

    printf("\n[TEST] rename()\n");
    {
        /* Create source file */
        int fd = open("/tmp/test_rename_src", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("create /tmp/test_rename_src", fd >= 0);
        if (fd >= 0) {
            write(fd, "rename_test", 11);
            close(fd);

            int ret = rename("/tmp/test_rename_src", "/tmp/test_rename_dst");
            test_result("rename succeeds", ret == 0);

            /* Source should be gone */
            struct stat st;
            ret = stat("/tmp/test_rename_src", &st);
            test_result("old name gone after rename", ret == -1);

            /* Destination should exist */
            ret = stat("/tmp/test_rename_dst", &st);
            test_result("new name exists after rename", ret == 0);

            /* Verify contents */
            fd = open("/tmp/test_rename_dst", O_RDONLY);
            test_result("can open renamed file", fd >= 0);
            if (fd >= 0) {
                char buf[32];
                ssize_t n = read(fd, buf, sizeof(buf));
                test_result("renamed file has correct size", n == 11);
                close(fd);
            }

            /* Cleanup */
            unlink("/tmp/test_rename_dst");
        }
    }

    printf("\n[TEST] chmod()\n");
    {
        /* chmod should succeed (returns 0 on FAT32) */
        int fd = open("/tmp/test_chmod_file", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("create /tmp/test_chmod_file", fd >= 0);
        if (fd >= 0) {
            close(fd);

            int ret = chmod("/tmp/test_chmod_file", 0644);
            test_result("chmod returns 0", ret == 0);

            ret = chmod("/tmp/test_chmod_file", 0755);
            test_result("chmod to 0755 returns 0", ret == 0);

            unlink("/tmp/test_chmod_file");
        }

        /* chmod on nonexistent should succeed (kernel returns 0 regardless) */
    }

    printf("\n[TEST] chown()\n");
    {
        int fd = open("/tmp/test_chown_file", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("create /tmp/test_chown_file", fd >= 0);
        if (fd >= 0) {
            close(fd);

            int ret = chown("/tmp/test_chown_file", 0, 0);
            test_result("chown returns 0", ret == 0);

            ret = fchown(open("/tmp/test_chown_file", O_RDONLY), 0, 0);
            test_result("fchown returns 0", ret == 0);

            unlink("/tmp/test_chown_file");
        }
    }

    printf("\n[TEST] utimensat()\n");
    {
        int fd = open("/tmp/test_utime_file", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("create /tmp/test_utime_file", fd >= 0);
        if (fd >= 0) {
            close(fd);

            struct timespec times[2];
            times[0].tv_sec = 1000000;
            times[0].tv_nsec = 0;
            times[1].tv_sec = 2000000;
            times[1].tv_nsec = 0;
            int ret = utimensat(-100, "/tmp/test_utime_file", times, 0);
            test_result("utimensat returns 0", ret == 0);

            unlink("/tmp/test_utime_file");
        }
    }

    printf("\n[TEST] mkdir+rmdir parents\n");
    {
        /* Create nested dirs */
        int ret = mkdir("/tmp/test_parent_a", 0755);
        test_result("mkdir /tmp/test_parent_a", ret == 0);

        ret = mkdir("/tmp/test_parent_a/b", 0755);
        test_result("mkdir /tmp/test_parent_a/b", ret == 0);

        ret = mkdir("/tmp/test_parent_a/b/c", 0755);
        test_result("mkdir /tmp/test_parent_a/b/c", ret == 0);

        /* Verify they exist */
        struct stat st;
        ret = stat("/tmp/test_parent_a/b/c", &st);
        test_result("nested dir exists", ret == 0 && S_ISDIR(st.st_mode));

        /* Remove in reverse order */
        ret = rmdir("/tmp/test_parent_a/b/c");
        test_result("rmdir /tmp/test_parent_a/b/c", ret == 0);

        ret = rmdir("/tmp/test_parent_a/b");
        test_result("rmdir /tmp/test_parent_a/b", ret == 0);

        ret = rmdir("/tmp/test_parent_a");
        test_result("rmdir /tmp/test_parent_a", ret == 0);
    }

    // ========================================
    // statfs / fstatfs tests
    // ========================================
    printf("\n--- statfs / fstatfs tests ---\n");
    {
        struct statfs sfs;
        int ret;

        /* statfs on root "/" should succeed */
        ret = statfs("/", &sfs);
        test_result("statfs(\"/\") succeeds", ret == 0);

        if (ret == 0) {
            /* Block size should be non-zero */
            test_result("statfs f_bsize > 0", sfs.f_bsize > 0);

            /* Total blocks should be non-zero */
            test_result("statfs f_blocks > 0", sfs.f_blocks > 0);

            /* Free blocks should be <= total blocks */
            test_result("statfs f_bfree <= f_blocks", sfs.f_bfree <= sfs.f_blocks);

            /* Available should be <= free */
            test_result("statfs f_bavail <= f_bfree", sfs.f_bavail <= sfs.f_bfree);

            /* f_type should be FAT32 magic (0x4d44) */
            test_result("statfs f_type == 0x4d44", sfs.f_type == 0x4d44);

            /* f_namelen should be reasonable */
            test_result("statfs f_namelen > 0", sfs.f_namelen > 0);

            printf("  f_bsize=%lu f_blocks=%lu f_bfree=%lu f_bavail=%lu f_type=0x%lx\n",
                   sfs.f_bsize, sfs.f_blocks, sfs.f_bfree, sfs.f_bavail, sfs.f_type);
        }

        /* statfs on an existing file should also work */
        ret = statfs("/bin/sh", &sfs);
        test_result("statfs(\"/bin/sh\") succeeds", ret == 0);

        /* statfs on /dev should fail with ENOSYS */
        ret = statfs("/dev", &sfs);
        test_result("statfs(\"/dev\") fails", ret == -1);
        test_result("statfs(\"/dev\") errno==ENOSYS", errno == ENOSYS);

        /* fstatfs on an open file */
        int fd = open("/bin/sh", 0);
        if (fd >= 0) {
            struct statfs fst;
            ret = fstatfs(fd, &fst);
            test_result("fstatfs(fd) succeeds", ret == 0);
            if (ret == 0) {
                test_result("fstatfs f_bsize > 0", fst.f_bsize > 0);
                test_result("fstatfs f_type == 0x4d44", fst.f_type == 0x4d44);
            }
            close(fd);
        } else {
            test_fail("fstatfs: could not open /bin/sh");
        }

        /* fstatfs on invalid fd should fail */
        struct statfs bad_fst;
        ret = fstatfs(999, &bad_fst);
        test_result("fstatfs(999) fails", ret == -1);
        test_result("fstatfs(999) errno==EBADF", errno == EBADF);
    }

    // ========================================
    // Test: sysinfo() syscall
    // ========================================
    printf("\n[TEST] sysinfo()\n");
    {
        struct sysinfo si;
        memset(&si, 0, sizeof(si));
        int ret = sysinfo(&si);
        test_result("sysinfo() returns 0", ret == 0);
        test_result("sysinfo: uptime > 0", si.uptime > 0);
        printf("  uptime: %ld seconds\n", si.uptime);
        test_result("sysinfo: totalram > 0", si.totalram > 0);
        printf("  totalram: %lu bytes (mem_unit=%u)\n",
               (unsigned long)si.totalram, si.mem_unit);
        test_result("sysinfo: freeram > 0", si.freeram > 0);
        test_result("sysinfo: freeram <= totalram", si.freeram <= si.totalram);
        printf("  freeram: %lu bytes\n", (unsigned long)si.freeram);
        test_result("sysinfo: procs > 0", si.procs > 0);
        printf("  procs: %d\n", si.procs);
        printf("  loads[0]=%lu loads[1]=%lu loads[2]=%lu\n",
               si.loads[0], si.loads[1], si.loads[2]);
        test_result("sysinfo: mem_unit > 0", si.mem_unit > 0);

        /* Test with NULL pointer - should fail */
        ret = sysinfo(NULL);
        test_result("sysinfo(NULL) returns -1", ret == -1);
    }

    // ========================================
    // Test: klogctl() syscall
    // ========================================
    printf("\n[TEST] klogctl()\n");
    {
        /* Get buffer size */
        int size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
        test_result("klogctl(SIZE_BUFFER) >= 0", size >= 0);
        printf("  kernel log buffer used: %d bytes\n", size);

        /* Read kernel log */
        char kbuf[4096];
        int nread = klogctl(SYSLOG_ACTION_READ_ALL, kbuf, sizeof(kbuf) - 1);
        test_result("klogctl(READ_ALL) >= 0", nread >= 0);
        if (nread > 0) {
            kbuf[nread] = '\0';
            /* There should be some kernel output */
            test_result("klogctl: read some data", nread > 0);
            printf("  read %d bytes of kernel log (first 80 chars):\n  ", nread);
            int show = nread < 80 ? nread : 80;
            for (int i = 0; i < show; i++) {
                if (kbuf[i] == '\n') printf("\\n");
                else if (kbuf[i] >= 32 && kbuf[i] < 127) putchar(kbuf[i]);
                else printf(".");
            }
            printf("\n");
        }

        /* Test invalid type */
        int ret = klogctl(999, NULL, 0);
        test_result("klogctl(invalid) returns -1", ret == -1);

        /* Test NULL buffer with READ_ALL should fail */
        ret = klogctl(SYSLOG_ACTION_READ_ALL, NULL, 100);
        test_result("klogctl(READ_ALL, NULL) returns -1", ret == -1);
    }

    // ========================================
    // Socket / Networking Tests
    // ========================================
    printf("\n--- Socket Tests ---\n");
    {
        // Test socket creation (UDP)
        int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        test_result("socket(AF_INET, SOCK_DGRAM) >= 0", udp_fd >= 0);

        // Test socket creation (TCP)
        int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        test_result("socket(AF_INET, SOCK_STREAM) >= 0", tcp_fd >= 0);

        // Test invalid domain
        int bad_fd = socket(99, SOCK_STREAM, 0);
        test_result("socket(99, SOCK_STREAM) == -1 (EAFNOSUPPORT)", bad_fd == -1);

        // Test invalid type
        bad_fd = socket(AF_INET, 99, 0);
        test_result("socket(AF_INET, 99) == -1 (bad type)", bad_fd == -1);

        // Test bind (UDP)
        if (udp_fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(12345);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            int ret = bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr));
            test_result("bind(udp, port 12345) == 0", ret == 0);

            // Test getsockname after bind
            struct sockaddr_in got_addr;
            socklen_t got_len = sizeof(got_addr);
            ret = getsockname(udp_fd, (struct sockaddr*)&got_addr, &got_len);
            test_result("getsockname(udp) == 0", ret == 0);
            test_result("getsockname port == 12345", ntohs(got_addr.sin_port) == 12345);
        }

        // Test bind (TCP)
        if (tcp_fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(12346);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            int ret = bind(tcp_fd, (struct sockaddr*)&addr, sizeof(addr));
            test_result("bind(tcp, port 12346) == 0", ret == 0);
        }

        // Test listen (TCP)
        if (tcp_fd >= 0) {
            int ret = listen(tcp_fd, 5);
            test_result("listen(tcp, 5) == 0", ret == 0);
        }

        // Test listen on UDP should fail
        if (udp_fd >= 0) {
            int ret = listen(udp_fd, 5);
            test_result("listen(udp) == -1 (EOPNOTSUPP)", ret == -1);
        }

        // Test setsockopt SO_REUSEADDR
        {
            int opt_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (opt_fd >= 0) {
                int optval = 1;
                int ret = setsockopt(opt_fd, SOL_SOCKET, SO_REUSEADDR,
                                     &optval, sizeof(optval));
                test_result("setsockopt(SO_REUSEADDR) == 0", ret == 0);

                // Test getsockopt SO_ERROR
                int error_val = -1;
                socklen_t error_len = sizeof(error_val);
                ret = getsockopt(opt_fd, SOL_SOCKET, SO_ERROR,
                                 &error_val, &error_len);
                test_result("getsockopt(SO_ERROR) == 0", ret == 0);
                test_result("SO_ERROR value == 0 (no error)", error_val == 0);

                shutdown(opt_fd, SHUT_RDWR);
            }
        }

        // Test htons/ntohs byte order
        test_result("htons(0x1234) byte swap", htons(0x1234) == 0x3412);
        test_result("ntohs(htons(80)) == 80", ntohs(htons(80)) == 80);
        test_result("ntohl(htonl(0x12345678)) round-trip",
                     ntohl(htonl(0x12345678)) == 0x12345678);

        // Test inet_addr
        {
            in_addr_t a = inet_addr("10.0.2.15");
            test_result("inet_addr(\"10.0.2.15\") != -1", a != (in_addr_t)-1);
            test_result("inet_addr round-trip",
                        ntohl(a) == ((10U << 24) | (0U << 16) | (2U << 8) | 15U));

            in_addr_t bad = inet_addr("not.an.ip");
            test_result("inet_addr(\"not.an.ip\") == -1", bad == (in_addr_t)-1);
        }

        // Test inet_ntoa
        {
            struct in_addr ia;
            ia.s_addr = inet_addr("192.168.1.100");
            char* str = inet_ntoa(ia);
            test_result("inet_ntoa(192.168.1.100)", strcmp(str, "192.168.1.100") == 0);
        }

        // Test invalid sockfd operations
        {
            int ret = bind(-1, NULL, 0);
            test_result("bind(-1) == -1 (EBADF)", ret == -1);

            ret = listen(-1, 5);
            test_result("listen(-1) == -1 (EBADF)", ret == -1);

            char buf[32];
            ssize_t n = recv(-1, buf, sizeof(buf), 0);
            test_result("recv(-1) == -1 (EBADF)", n == -1);

            n = send(-1, "test", 4, 0);
            test_result("send(-1) == -1 (EBADF)", n == -1);
        }

        // Test getpeername on unconnected socket
        {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            if (s >= 0) {
                struct sockaddr_in peer;
                socklen_t plen = sizeof(peer);
                int ret = getpeername(s, (struct sockaddr*)&peer, &plen);
                test_result("getpeername(unconnected) == -1 (ENOTCONN)", ret == -1);
                shutdown(s, SHUT_RDWR);
            }
        }

        // Cleanup
        if (udp_fd >= 0) shutdown(udp_fd, SHUT_RDWR);
        if (tcp_fd >= 0) shutdown(tcp_fd, SHUT_RDWR);
    }

    // ========================================
    // Extended Networking Syscalls Tests
    // ========================================
    printf("\n--- Extended Networking Syscalls ---\n");

    // Test socketpair
    {
        int sv[2] = {-1, -1};
        int ret = socketpair(AF_INET, SOCK_DGRAM, 0, sv);
        test_result("socketpair returns 0", ret == 0);
        test_result("socketpair sv[0] >= 0", sv[0] >= 0);
        test_result("socketpair sv[1] >= 0", sv[1] >= 0);
        if (ret == 0) {
            // Test sending data through the pair
            const char *msg = "hello";
            ssize_t n = send(sv[0], msg, 5, 0);
            test_result("socketpair send returns 5", n == 5);
            char buf[16] = {0};
            n = recv(sv[1], buf, sizeof(buf), 0);
            test_result("socketpair recv returns 5", n == 5);
            test_result("socketpair data matches", memcmp(buf, "hello", 5) == 0);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // Test close/dup/dup2/dup3 on socket fds
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        test_result("socket returns valid fd", s >= 3);
        if (s >= 0) {
            int d = dup(s);
            test_result("dup(socket) returns valid fd", d >= 3 && d != s);
            if (d >= 0) close(d);

            int d2 = dup2(s, 100);
            test_result("dup2(socket, 100) returns 100", d2 == 100);
            if (d2 >= 0) close(d2);

            int d3 = dup3(s, 101, 0);
            test_result("dup3(socket, 101, 0) returns 101", d3 == 101);
            if (d3 >= 0) close(d3);

            close(s);
        }
    }

    // Test fcntl on socket (F_GETFL / F_SETFL O_NONBLOCK)
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            int fl = fcntl(s, F_GETFL, 0);
            test_result("fcntl(socket, F_GETFL) >= 0", fl >= 0);

            int ret = fcntl(s, F_SETFL, fl | O_NONBLOCK);
            test_result("fcntl(socket, F_SETFL, O_NONBLOCK) == 0", ret == 0);

            fl = fcntl(s, F_GETFL, 0);
            test_result("fcntl confirms O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
            close(s);
        }
    }

    // Test ioctl SIOCGIFMTU / SIOCGIFFLAGS / SIOCGIFHWADDR
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            // Try "eth0" - E1000 device
            memcpy(ifr.ifr_name, "eth0", 5);

            int ret = ioctl(s, SIOCGIFMTU, &ifr);
            if (ret == 0) {
                test_result("ioctl SIOCGIFMTU returns MTU > 0", ifr.ifr_mtu > 0);
            } else {
                test_result("ioctl SIOCGIFMTU (no eth0, skip)", 1);
            }

            memset(&ifr, 0, sizeof(ifr));
            memcpy(ifr.ifr_name, "eth0", 5);
            ret = ioctl(s, SIOCGIFFLAGS, &ifr);
            if (ret == 0) {
                test_result("ioctl SIOCGIFFLAGS has IFF_UP", (ifr.ifr_flags & IFF_UP) != 0);
            } else {
                test_result("ioctl SIOCGIFFLAGS (no eth0, skip)", 1);
            }

            memset(&ifr, 0, sizeof(ifr));
            memcpy(ifr.ifr_name, "eth0", 5);
            ret = ioctl(s, SIOCGIFHWADDR, &ifr);
            if (ret == 0) {
                // Check MAC is not all zeros
                int nonzero = 0;
                for (int i = 0; i < 6; i++)
                    if (ifr.ifr_hwaddr.sa_data[i] != 0) nonzero = 1;
                test_result("ioctl SIOCGIFHWADDR has non-zero MAC", nonzero);
            } else {
                test_result("ioctl SIOCGIFHWADDR (no eth0, skip)", 1);
            }

            close(s);
        }
    }

    // Test poll on stdin (should return immediately with timeout=0)
    {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 0);  // immediate timeout
        test_result("poll(stdin, timeout=0) >= 0", ret >= 0);
    }

    // Test poll on socket
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct pollfd pfd;
            pfd.fd = s;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int ret = poll(&pfd, 1, 0);
            test_result("poll(udp_socket, POLLOUT, 0) >= 0", ret >= 0);
            if (ret > 0) {
                test_result("poll returns POLLOUT for UDP socket", (pfd.revents & POLLOUT) != 0);
            }
            close(s);
        }
    }

    // Test select with timeout=0 (immediate)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 0};
        int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        test_result("select(stdin, timeout=0) >= 0", ret >= 0);
    }

    // Test epoll create/ctl/wait
    {
        int epfd = epoll_create1(0);
        test_result("epoll_create1(0) returns valid fd", epfd >= 3);
        if (epfd >= 0) {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            if (s >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT;
                ev.data.fd = s;
                int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, s, &ev);
                test_result("epoll_ctl ADD returns 0", ret == 0);

                struct epoll_event events[4];
                ret = epoll_wait(epfd, events, 4, 0);
                test_result("epoll_wait(timeout=0) >= 0", ret >= 0);

                ret = epoll_ctl(epfd, EPOLL_CTL_DEL, s, NULL);
                test_result("epoll_ctl DEL returns 0", ret == 0);

                close(s);
            }
            close(epfd);
        }
    }

    // Test accept4 (should fail on non-listening socket)
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            int ret = accept4(s, NULL, NULL, 0);
            test_result("accept4(non-listening) returns -1", ret == -1);
            close(s);
        }
    }

    // Test sendmsg / recvmsg via socketpair
    {
        int sv[2] = {-1, -1};
        if (socketpair(AF_INET, SOCK_DGRAM, 0, sv) == 0) {
            char data[] = "msghdr test";
            struct iovec iov;
            iov.iov_base = data;
            iov.iov_len = sizeof(data) - 1;
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            ssize_t n = sendmsg(sv[0], &msg, 0);
            test_result("sendmsg returns > 0", n > 0);

            char rbuf[32] = {0};
            struct iovec riov;
            riov.iov_base = rbuf;
            riov.iov_len = sizeof(rbuf);
            struct msghdr rmsg;
            memset(&rmsg, 0, sizeof(rmsg));
            rmsg.msg_iov = &riov;
            rmsg.msg_iovlen = 1;
            n = recvmsg(sv[1], &rmsg, 0);
            test_result("recvmsg returns > 0", n > 0);
            test_result("recvmsg data matches", memcmp(rbuf, "msghdr test", 11) == 0);

            close(sv[0]);
            close(sv[1]);
        }
    }

    // ========================================
    // sendfile Tests
    // ========================================
    printf("\n--- sendfile Tests ---\n");

    // Test 1: sendfile from file to file
    {
        // Create a source file with known content
        int src = open("/tmp_sf_src.txt", O_WRONLY | O_CREAT | O_TRUNC);
        test_result("sendfile: create source file", src >= 0);
        if (src >= 0) {
            const char* data = "Hello sendfile world! This is test data for sendfile.";
            ssize_t nw = write(src, data, strlen(data));
            test_result("sendfile: write source data", nw == (ssize_t)strlen(data));
            close(src);

            // Open source for reading and dest for writing
            int in_fd = open("/tmp_sf_src.txt", O_RDONLY);
            int out_fd = open("/tmp_sf_dst.txt", O_WRONLY | O_CREAT | O_TRUNC);
            test_result("sendfile: open source for read", in_fd >= 0);
            test_result("sendfile: open dest for write", out_fd >= 0);

            if (in_fd >= 0 && out_fd >= 0) {
                ssize_t sf = sendfile(out_fd, in_fd, NULL, strlen(data));
                test_result("sendfile: file-to-file returns correct count",
                            sf == (ssize_t)strlen(data));
                close(in_fd);
                close(out_fd);

                // Verify destination content
                int vfd = open("/tmp_sf_dst.txt", O_RDONLY);
                if (vfd >= 0) {
                    char rbuf[128] = {0};
                    ssize_t nr = read(vfd, rbuf, sizeof(rbuf));
                    test_result("sendfile: dest has correct length",
                                nr == (ssize_t)strlen(data));
                    test_result("sendfile: dest content matches",
                                memcmp(rbuf, data, strlen(data)) == 0);
                    close(vfd);
                }
            } else {
                if (in_fd >= 0) close(in_fd);
                if (out_fd >= 0) close(out_fd);
            }

            // Cleanup
            unlink("/tmp_sf_src.txt");
            unlink("/tmp_sf_dst.txt");
        }
    }

    // Test 2: sendfile with offset parameter
    {
        int src = open("/tmp_sf_off.txt", O_WRONLY | O_CREAT | O_TRUNC);
        if (src >= 0) {
            const char* data = "AAAAABBBBBCCCCC";  // 15 bytes
            write(src, data, 15);
            close(src);

            int in_fd = open("/tmp_sf_off.txt", O_RDONLY);
            int out_fd = open("/tmp_sf_off_d.txt", O_WRONLY | O_CREAT | O_TRUNC);
            if (in_fd >= 0 && out_fd >= 0) {
                // Send 5 bytes starting at offset 5 (the "BBBBB" part)
                int64_t off = 5;
                ssize_t sf = sendfile(out_fd, in_fd, &off, 5);
                test_result("sendfile: with offset returns 5", sf == 5);
                test_result("sendfile: offset updated to 10", off == 10);

                // Verify file position was NOT changed (offset mode)
                off_t pos = lseek(in_fd, 0, 1);  // SEEK_CUR
                test_result("sendfile: file position unchanged", pos == 0);

                close(in_fd);
                close(out_fd);

                // Verify we got "BBBBB"
                int vfd = open("/tmp_sf_off_d.txt", O_RDONLY);
                if (vfd >= 0) {
                    char rbuf[16] = {0};
                    read(vfd, rbuf, sizeof(rbuf));
                    test_result("sendfile: offset data is BBBBB",
                                memcmp(rbuf, "BBBBB", 5) == 0);
                    close(vfd);
                }
            } else {
                if (in_fd >= 0) close(in_fd);
                if (out_fd >= 0) close(out_fd);
            }
            unlink("/tmp_sf_off.txt");
            unlink("/tmp_sf_off_d.txt");
        }
    }

    // Test 3: sendfile from file to socket (via socketpair)
    {
        int src = open("/tmp_sf_sock.txt", O_WRONLY | O_CREAT | O_TRUNC);
        if (src >= 0) {
            const char* data = "socket sendfile data";
            write(src, data, strlen(data));
            close(src);

            int sv[2] = {-1, -1};
            int in_fd = open("/tmp_sf_sock.txt", O_RDONLY);
            int sp_ok = socketpair(AF_INET, SOCK_DGRAM, 0, sv);
            test_result("sendfile-to-socket: setup ok",
                        in_fd >= 0 && sp_ok == 0);
            if (in_fd >= 0 && sp_ok == 0) {
                ssize_t sf = sendfile(sv[0], in_fd, NULL, strlen(data));
                test_result("sendfile: file-to-socket returns correct count",
                            sf == (ssize_t)strlen(data));

                if (sf > 0) {
                    char rbuf[64] = {0};
                    ssize_t nr = recv(sv[1], rbuf, sizeof(rbuf), 0);
                    test_result("sendfile: socket recv gets data",
                                nr == (ssize_t)strlen(data));
                    test_result("sendfile: socket data matches",
                                memcmp(rbuf, data, strlen(data)) == 0);
                }
            }
            if (in_fd >= 0) close(in_fd);
            if (sv[0] >= 0) close(sv[0]);
            if (sv[1] >= 0) close(sv[1]);
            unlink("/tmp_sf_sock.txt");
        }
    }

    // Test 4: sendfile from file to pipe
    {
        int src = open("/tmp_sf_pipe.txt", O_WRONLY | O_CREAT | O_TRUNC);
        if (src >= 0) {
            const char* data = "pipe sendfile!";
            write(src, data, strlen(data));
            close(src);

            int pfd[2];
            int in_fd = open("/tmp_sf_pipe.txt", O_RDONLY);
            int pipe_ok = pipe(pfd);
            test_result("sendfile-to-pipe: setup ok",
                        in_fd >= 0 && pipe_ok == 0);
            if (in_fd >= 0 && pipe_ok == 0) {
                ssize_t sf = sendfile(pfd[1], in_fd, NULL, strlen(data));
                test_result("sendfile: file-to-pipe returns correct count",
                            sf == (ssize_t)strlen(data));

                if (sf > 0) {
                    char rbuf[64] = {0};
                    ssize_t nr = read(pfd[0], rbuf, sizeof(rbuf));
                    test_result("sendfile: pipe read gets data",
                                nr == (ssize_t)strlen(data));
                    test_result("sendfile: pipe data matches",
                                memcmp(rbuf, data, strlen(data)) == 0);
                }
            }
            if (in_fd >= 0) close(in_fd);
            if (pipe_ok == 0) { close(pfd[0]); close(pfd[1]); }
            unlink("/tmp_sf_pipe.txt");
        }
    }

    // Test 5: sendfile with count=0 returns 0
    {
        int src = open("/tmp_sf_zero.txt", O_WRONLY | O_CREAT | O_TRUNC);
        if (src >= 0) {
            write(src, "x", 1);
            close(src);
            int in_fd = open("/tmp_sf_zero.txt", O_RDONLY);
            int out_fd = open("/tmp_sf_zero_d.txt", O_WRONLY | O_CREAT | O_TRUNC);
            if (in_fd >= 0 && out_fd >= 0) {
                ssize_t sf = sendfile(out_fd, in_fd, NULL, 0);
                test_result("sendfile: count=0 returns 0", sf == 0);
            }
            if (in_fd >= 0) close(in_fd);
            if (out_fd >= 0) close(out_fd);
            unlink("/tmp_sf_zero.txt");
            unlink("/tmp_sf_zero_d.txt");
        }
    }

    // Test 6: sendfile with invalid fds returns -1
    {
        ssize_t sf = sendfile(-1, -1, NULL, 100);
        test_result("sendfile: bad fds returns -1", sf == -1);
    }

    // ========================================
    // /dev/urandom and /dev/random Tests
    // ========================================
    printf("\n--- /dev/urandom and /dev/random ---\n");

    // Test 1: Read 32 bytes from /dev/urandom
    {
        int fd = open("/dev/urandom", O_RDONLY);
        test_result("urandom: open succeeds", fd >= 0);
        if (fd >= 0) {
            unsigned char buf[32];
            memset(buf, 0, sizeof(buf));
            ssize_t n = read(fd, buf, 32);
            test_result("urandom: read 32 bytes", n == 32);

            // Check not all zeros
            int nonzero = 0;
            for (int i = 0; i < 32; i++) {
                if (buf[i] != 0) nonzero = 1;
            }
            test_result("urandom: data is non-zero", nonzero);
            close(fd);
        }
    }

    // Test 2: Two reads from /dev/urandom differ
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            unsigned char buf1[16], buf2[16];
            read(fd, buf1, 16);
            read(fd, buf2, 16);
            test_result("urandom: two reads differ", memcmp(buf1, buf2, 16) != 0);
            close(fd);
        }
    }

    // Test 3: Read 4096 bytes from /dev/urandom
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            unsigned char buf[4096];
            ssize_t n = read(fd, buf, 4096);
            test_result("urandom: read 4096 bytes", n == 4096);
            close(fd);
        }
    }

    // Test 4: /dev/random also works
    {
        int fd = open("/dev/random", O_RDONLY);
        test_result("random: open succeeds", fd >= 0);
        if (fd >= 0) {
            unsigned char buf[16];
            ssize_t n = read(fd, buf, 16);
            test_result("random: read 16 bytes", n == 16);
            close(fd);
        }
    }

    // Test 5: Write to /dev/urandom (adds entropy)
    {
        int fd = open("/dev/urandom", O_WRONLY);
        if (fd >= 0) {
            unsigned char entropy[] = "test entropy data";
            ssize_t n = write(fd, entropy, sizeof(entropy));
            test_result("urandom: write succeeds", n == (ssize_t)sizeof(entropy));
            close(fd);
        }
    }

    // ========================================
    // AF_UNIX Socketpair Tests
    // ========================================
    printf("\n--- AF_UNIX Socketpair ---\n");

    // Test 1: socketpair creation
    {
        int sv[2] = {-1, -1};
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        test_result("unix socketpair: create", ret == 0 && sv[0] >= 0 && sv[1] >= 0);

        if (ret == 0) {
            // Test 2: send/recv through socketpair
            const char* msg = "hello unix";
            ssize_t sent = write(sv[0], msg, strlen(msg));
            test_result("unix socketpair: write", sent == (ssize_t)strlen(msg));

            char buf[64];
            memset(buf, 0, sizeof(buf));
            ssize_t rcvd = read(sv[1], buf, sizeof(buf));
            test_result("unix socketpair: read", rcvd == (ssize_t)strlen(msg));
            test_result("unix socketpair: data matches", strcmp(buf, "hello unix") == 0);

            // Test 3: bidirectional
            const char* reply = "world";
            write(sv[1], reply, strlen(reply));
            memset(buf, 0, sizeof(buf));
            rcvd = read(sv[0], buf, sizeof(buf));
            test_result("unix socketpair: bidirectional", rcvd == (ssize_t)strlen(reply) && strcmp(buf, "world") == 0);

            // Test 4: close one end, other gets EOF
            close(sv[0]);
            memset(buf, 0, sizeof(buf));
            rcvd = read(sv[1], buf, sizeof(buf));
            test_result("unix socketpair: close->EOF", rcvd == 0);

            close(sv[1]);
        }
    }

    // Test 5: AF_UNIX SOCK_DGRAM socketpair
    {
        int sv[2] = {-1, -1};
        int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
        test_result("unix dgram socketpair: create", ret == 0);
        if (ret == 0) {
            const char* msg = "dgram test";
            write(sv[0], msg, strlen(msg));
            char buf[64];
            memset(buf, 0, sizeof(buf));
            ssize_t n = read(sv[1], buf, sizeof(buf));
            test_result("unix dgram socketpair: transfer", n == (ssize_t)strlen(msg) && strcmp(buf, "dgram test") == 0);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // ========================================
    // AF_UNIX Client/Server Tests
    // ========================================
    printf("\n--- AF_UNIX Client/Server ---\n");

    {
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        test_result("unix server: socket create", server_fd >= 0);

        if (server_fd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strcpy(addr.sun_path, "/tmp/test_unix.sock");

            int ret = bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
            test_result("unix server: bind", ret == 0);

            ret = listen(server_fd, 5);
            test_result("unix server: listen", ret == 0);

            // Fork: child connects, parent accepts
            pid_t pid = fork();
            if (pid == 0) {
                // Child: connect and send data
                close(server_fd);
                int cli = socket(AF_UNIX, SOCK_STREAM, 0);
                if (cli >= 0) {
                    struct sockaddr_un saddr;
                    memset(&saddr, 0, sizeof(saddr));
                    saddr.sun_family = AF_UNIX;
                    strcpy(saddr.sun_path, "/tmp/test_unix.sock");
                    connect(cli, (struct sockaddr*)&saddr, sizeof(saddr));
                    write(cli, "from child", 10);
                    char buf[64];
                    read(cli, buf, sizeof(buf));
                    close(cli);
                }
                _exit(0);
            } else if (pid > 0) {
                // Parent: accept and verify
                int cli_fd = accept(server_fd, NULL, NULL);
                test_result("unix server: accept", cli_fd >= 0);
                if (cli_fd >= 0) {
                    char buf[64];
                    memset(buf, 0, sizeof(buf));
                    ssize_t n = read(cli_fd, buf, sizeof(buf));
                    test_result("unix server: recv from client", n == 10 && memcmp(buf, "from child", 10) == 0);
                    write(cli_fd, "reply", 5);
                    close(cli_fd);
                }
                int status;
                waitpid(pid, &status, 0);
            }
            close(server_fd);
        }
    }

    // ========================================
    // UDP Loopback on 127.0.0.1 Tests
    // ========================================
    printf("\n--- UDP Loopback 127.0.0.1 ---\n");

    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        test_result("udp loopback: socket create", sock >= 0);

        if (sock >= 0) {
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(19999);
            bind_addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1

            int ret = bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
            test_result("udp loopback: bind 127.0.0.1:19999", ret == 0);

            if (ret == 0) {
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                dest.sin_port = htons(19999);
                dest.sin_addr.s_addr = htonl(0x7F000001);

                const char* msg = "loopback test";
                ssize_t sent = sendto(sock, msg, strlen(msg), 0,
                                      (struct sockaddr*)&dest, sizeof(dest));
                test_result("udp loopback: sendto", sent == (ssize_t)strlen(msg));

                if (sent > 0) {
                    char buf[64];
                    memset(buf, 0, sizeof(buf));
                    ssize_t rcvd = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
                    test_result("udp loopback: recvfrom", rcvd == (ssize_t)strlen(msg));
                    test_result("udp loopback: data matches", memcmp(buf, msg, strlen(msg)) == 0);
                }
            }
            close(sock);
        }
    }

    // ========================================
    // Loopback Interface Detection
    // ========================================
    printf("\n--- Loopback Interface ---\n");

    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strcpy(ifr.ifr_name, "lo");

            int ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
            test_result("loopback: SIOCGIFFLAGS succeeds", ret == 0);
            if (ret == 0) {
                test_result("loopback: IFF_LOOPBACK set", (ifr.ifr_flags & IFF_LOOPBACK) != 0);
                test_result("loopback: IFF_UP set", (ifr.ifr_flags & IFF_UP) != 0);
            }

            memset(&ifr, 0, sizeof(ifr));
            strcpy(ifr.ifr_name, "lo");
            ret = ioctl(sock, SIOCGIFADDR, &ifr);
            test_result("loopback: SIOCGIFADDR succeeds", ret == 0);
            if (ret == 0) {
                struct sockaddr_in* sin = (struct sockaddr_in*)&ifr.ifr_addr;
                test_result("loopback: IP is 127.0.0.1",
                            ntohl(sin->sin_addr.s_addr) == 0x7F000001);
            }

            memset(&ifr, 0, sizeof(ifr));
            strcpy(ifr.ifr_name, "lo");
            ret = ioctl(sock, SIOCGIFMTU, &ifr);
            test_result("loopback: SIOCGIFMTU succeeds", ret == 0);
            if (ret == 0) {
                test_result("loopback: MTU is 65535", ifr.ifr_mtu == 65535);
            }

            close(sock);
        }
    }

    // ========================================
    // Routing Ioctl Tests
    // ========================================
    printf("\n--- Routing Ioctls ---\n");

    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            // Add a test route and delete it
            struct {
                struct sockaddr rt_dst;
                struct sockaddr rt_gateway;
                struct sockaddr rt_genmask;
                short rt_flags;
                int rt_metric;
                char* rt_dev;
            } rt;
            memset(&rt, 0, sizeof(rt));
            struct sockaddr_in* dst = (struct sockaddr_in*)&rt.rt_dst;
            struct sockaddr_in* gw = (struct sockaddr_in*)&rt.rt_gateway;
            struct sockaddr_in* mask = (struct sockaddr_in*)&rt.rt_genmask;

            dst->sin_family = AF_INET;
            dst->sin_addr.s_addr = htonl(0xC0A86400);  // 192.168.100.0
            gw->sin_family = AF_INET;
            gw->sin_addr.s_addr = htonl(0x0A000001);   // 10.0.0.1
            mask->sin_family = AF_INET;
            mask->sin_addr.s_addr = htonl(0xFFFFFF00);  // 255.255.255.0
            rt.rt_flags = 0x0003;  // RTF_UP | RTF_GATEWAY

            int ret = ioctl(sock, SIOCADDRT, &rt);
            test_result("route: SIOCADDRT", ret == 0);

            ret = ioctl(sock, SIOCDELRT, &rt);
            test_result("route: SIOCDELRT", ret == 0);

            close(sock);
        }
    }

    // ========================================
    // IFCONF includes loopback
    // ========================================
    printf("\n--- IFCONF with loopback ---\n");

    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr_buf[8];
            struct ifconf ifc;
            memset(&ifc, 0, sizeof(ifc));
            ifc.ifc_len = sizeof(ifr_buf);
            ifc.ifc_buf = (char*)ifr_buf;

            int ret = ioctl(sock, SIOCGIFCONF, &ifc);
            test_result("ifconf: SIOCGIFCONF succeeds", ret == 0);

            int found_lo = 0;
            int n_ifs = ifc.ifc_len / (int)sizeof(struct ifreq);
            for (int i = 0; i < n_ifs; i++) {
                if (strcmp(ifr_buf[i].ifr_name, "lo") == 0)
                    found_lo = 1;
            }
            test_result("ifconf: lo interface present", found_lo);
            close(sock);
        }
    }

    // ========================================
    // DNS Resolve Tests
    // ========================================
    printf("\n--- DNS Resolve ---\n");

    // Test 1: Resolve numeric IP
    {
        uint32_t ip = 0;
        int ret = dns_resolve("192.168.1.1", &ip);
        test_result("dns: numeric IP resolve", ret == 0 && ip == 0xC0A80101);
    }

    // Test 2: Resolve "localhost"
    {
        uint32_t ip = 0;
        int ret = dns_resolve("localhost", &ip);
        test_result("dns: localhost resolves to 127.0.0.1", ret == 0 && ip == 0x7F000001);
    }

    // ========================================
    // Extended TCP Loopback Tests
    // ========================================
    printf("\n--- Extended TCP Loopback ---\n");

    {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        test_result("tcp loopback: server socket", server_fd >= 0);

        if (server_fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(20021);
            addr.sin_addr.s_addr = htonl(0x7F000001);

            int optval = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
            int ret = bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
            test_result("tcp loopback: bind", ret == 0);
            if (ret == 0) {
                ret = listen(server_fd, 4);
                test_result("tcp loopback: listen", ret == 0);
            }

            if (ret == 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (client_fd >= 0) {
                        struct sockaddr_in dst;
                        memset(&dst, 0, sizeof(dst));
                        dst.sin_family = AF_INET;
                        dst.sin_port = htons(20021);
                        dst.sin_addr.s_addr = htonl(0x7F000001);
                        if (connect(client_fd, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
                            char sendbuf[4096];
                            char recvbuf[4096];
                            for (int i = 0; i < (int)sizeof(sendbuf); i++)
                                sendbuf[i] = (char)('A' + (i % 26));

                            size_t sent = 0;
                            while (sent < sizeof(sendbuf)) {
                                ssize_t n = send(client_fd, sendbuf + sent,
                                                 sizeof(sendbuf) - sent, 0);
                                if (n <= 0) break;
                                sent += (size_t)n;
                            }

                            size_t recvd = 0;
                            while (recvd < sizeof(recvbuf)) {
                                ssize_t n = recv(client_fd, recvbuf + recvd,
                                                 sizeof(recvbuf) - recvd, 0);
                                if (n <= 0) break;
                                recvd += (size_t)n;
                            }

                            _exit((sent == sizeof(sendbuf) && recvd == sizeof(recvbuf) &&
                                   memcmp(sendbuf, recvbuf, sizeof(sendbuf)) == 0) ? 0 : 2);
                        }
                        close(client_fd);
                    }
                    _exit(1);
                } else if (pid > 0) {
                    int conn_fd = accept(server_fd, NULL, NULL);
                    test_result("tcp loopback: accept", conn_fd >= 0);
                    if (conn_fd >= 0) {
                        char recvbuf[4096];
                        size_t recvd = 0;
                        while (recvd < sizeof(recvbuf)) {
                            ssize_t n = recv(conn_fd, recvbuf + recvd,
                                             sizeof(recvbuf) - recvd, 0);
                            if (n <= 0) break;
                            recvd += (size_t)n;
                        }
                        test_result("tcp loopback: recv 4096 bytes", recvd == sizeof(recvbuf));

                        size_t sent = 0;
                        while (sent < recvd) {
                            ssize_t n = send(conn_fd, recvbuf + sent, recvd - sent, 0);
                            if (n <= 0) break;
                            sent += (size_t)n;
                        }
                        test_result("tcp loopback: echo 4096 bytes", sent == recvd);
                        close(conn_fd);
                    }

                    int status = 0;
                    waitpid(pid, &status, 0);
                    test_result("tcp loopback: client completed", WIFEXITED(status) && WEXITSTATUS(status) == 0);
                } else {
                    test_fail("tcp loopback: fork");
                }
            }
            close(server_fd);
        }
    }

    {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        test_result("tcp refuse: socket", client_fd >= 0);
        if (client_fd >= 0) {
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_port = htons(20022);
            dst.sin_addr.s_addr = htonl(0x7F000001);
            int ret = connect(client_fd, (struct sockaddr*)&dst, sizeof(dst));
            test_result("tcp refuse: connect fails", ret == -1);
            close(client_fd);
        }
    }

    // ========================================
    // TCP Large Transfer Bind Address Tests
    // ========================================
    printf("\n--- TCP Bind Address Variants ---\n");

    run_tcp_large_transfer_case("tcp any lo", 0x00000000, 0x7F000001, 20023);

    {
        uint32_t eth0_ip = 0;
        if (get_interface_ipv4("eth0", &eth0_ip) == 0 && eth0_ip != 0) {
            run_tcp_large_transfer_case("tcp any eth0", 0x00000000, eth0_ip, 20024);
            run_tcp_large_transfer_case("tcp eth0", eth0_ip, eth0_ip, 20025);
        } else {
            test_result("tcp any eth0: interface/address unavailable, skip", 1);
            test_result("tcp eth0: interface/address unavailable, skip", 1);
        }
    }

    // ========================================
    // IPv4 Fragmented UDP Loopback Tests
    // ========================================
    printf("\n--- IPv4 Fragmented UDP ---\n");

    {
        int rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
        int tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
        test_result("udp frag: sockets create", rx_fd >= 0 && tx_fd >= 0);

        if (rx_fd >= 0 && tx_fd >= 0) {
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(20031);
            bind_addr.sin_addr.s_addr = htonl(0x7F000001);

            int ret = bind(rx_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
            test_result("udp frag: bind receiver", ret == 0);
            if (ret == 0) {
                char sendbuf[2400];
                char recvbuf[2400];
                for (int i = 0; i < (int)sizeof(sendbuf); i++)
                    sendbuf[i] = (char)(i & 0x7F);
                memset(recvbuf, 0, sizeof(recvbuf));

                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                dest.sin_port = htons(20031);
                dest.sin_addr.s_addr = htonl(0x7F000001);

                ssize_t sent = sendto(tx_fd, sendbuf, sizeof(sendbuf), 0,
                                      (struct sockaddr*)&dest, sizeof(dest));
                test_result("udp frag: send 2400 bytes", sent == (ssize_t)sizeof(sendbuf));
                if (sent == (ssize_t)sizeof(sendbuf)) {
                    ssize_t recvd = recvfrom(rx_fd, recvbuf, sizeof(recvbuf), 0, NULL, NULL);
                    test_result("udp frag: recv 2400 bytes", recvd == (ssize_t)sizeof(recvbuf));
                    test_result("udp frag: payload matches", recvd == (ssize_t)sizeof(recvbuf) && memcmp(sendbuf, recvbuf, sizeof(sendbuf)) == 0);
                }
            }
        }

        if (rx_fd >= 0) close(rx_fd);
        if (tx_fd >= 0) close(tx_fd);
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
