// teststress - stress test program that runs random commands in a loop
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define TIMEOUT_SECONDS (10 * 60)  // 10 minutes

// Simple linear congruential generator for random numbers
static unsigned int seed = 12345;

static unsigned int rand_simple(void) {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) & 0x7FFF;
}

// Commands to run (use full paths since execve doesn't search PATH)
static const char* commands[] = {
    "/bin/ls",
    "/testlibc",
    "/tests",
    "/testmem 400",
    "/testmem 500",
    "/hello",
    "/memstat",
    "/bin/cat /HELLO.TXT",
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

// Try to execute with path search
static int try_exec(char* argv[]) {
    static char path_buf[256];
    const char* paths[] = { "", "/", "/bin/" };
    
    for (int i = 0; i < 3; i++) {
        // Build full path
        int j = 0;
        const char* p = paths[i];
        while (*p && j < 250) path_buf[j++] = *p++;
        p = argv[0];
        // Skip leading / in argv[0] if path already has one
        if (path_buf[j-1] == '/' && *p == '/') p++;
        while (*p && j < 255) path_buf[j++] = *p++;
        path_buf[j] = '\0';
        
        // Try this path
        execve(path_buf, argv, NULL);
    }
    return -1;  // All failed
}

// Parse command string into argv array
static int parse_command(const char* cmd, char* argv[], int max_args) {
    static char buf[256];
    int argc = 0;
    int i = 0, j = 0;
    int in_word = 0;
    
    // Copy and parse
    while (cmd[i] && argc < max_args - 1) {
        if (cmd[i] == ' ' || cmd[i] == '\t') {
            if (in_word) {
                buf[j++] = '\0';
                in_word = 0;
            }
        } else {
            if (!in_word) {
                argv[argc++] = &buf[j];
                in_word = 1;
            }
            buf[j++] = cmd[i];
        }
        i++;
    }
    if (in_word) {
        buf[j] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    int iteration = 0;
    time_t start_time = time(NULL);
    
    printf("=== STRESS TEST STARTED ===\n");
    printf("Running random commands for up to 10 minutes...\n");
    printf("Press Ctrl+C to stop early\n\n");
    
    // Use pid as part of seed for some variation
    seed = (unsigned int)getpid() * 31337;
    
    while (1) {
        // Check if 10 minutes have passed
        time_t now = time(NULL);
        if (now - start_time >= TIMEOUT_SECONDS) {
            printf("\n=== 10 MINUTES ELAPSED - STRESS TEST COMPLETE ===\n");
            printf("Total iterations: %d\n", iteration);
            break;
        }
        
        // Pick a random command
        int cmd_idx = rand_simple() % NUM_COMMANDS;
        const char* cmd = commands[cmd_idx];
        
        iteration++;
        printf("[%d] Running: %s\n", iteration, cmd);
        
        // Fork and exec
        int pid = fork();
        if (pid < 0) {
            printf("fork failed!\n");
            // Wait a bit and retry
            for (volatile int i = 0; i < 1000000; i++);
            continue;
        }
        
        if (pid == 0) {
            // Child process
            char* child_argv[16];
            parse_command(cmd, child_argv, 16);
            
            if (child_argv[0]) {
                try_exec(child_argv);
                // If try_exec returns, all paths failed
                printf("execve failed for: %s\n", child_argv[0]);
            }
            exit(1);
        }
        
        // Parent: wait for child
        int status;
        waitpid(pid, &status, 0);
        
        // Small delay between commands
        for (volatile int i = 0; i < 100000; i++);
    }
    
    return 0;
}
