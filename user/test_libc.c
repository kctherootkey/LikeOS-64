#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("\n========================================\n");
    printf("  LikeOS-64 Libc Tests\n");
    printf("========================================\n\n");

    // Test printf
    printf("[TEST] printf()\n");
    printf("  Hello from userland libc!\n");
    printf("  argc = %d\n", argc);
    
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    
    // Test malloc
    printf("\n[TEST] malloc/free\n");
    char* buf = malloc(100);
    if (buf) {
        printf("  malloc(100) = %p\n", buf);
        
        // Test string functions
        strcpy(buf, "Hello, ");
        strcat(buf, "World!");
        printf("  String: %s (len=%zu)\n", buf, strlen(buf));
        
        free(buf);
        printf("  Buffer freed\n");
    }
    
    // Test atoi and string conversions
    printf("\n[TEST] atoi()\n");
    const char* numbers[] = {"42", "-123", "0xFF", "777"};
    for (int i = 0; i < 4; i++) {
        printf("  atoi(\"%s\") = %d\n", numbers[i], atoi(numbers[i]));
    }
    
    // Test formatted output
    printf("\n[TEST] printf format specifiers\n");
    printf("  Hex: 0x%x, Decimal: %d, String: %s\n", 0xDEADBEEF, 12345, "test");
    
    // Test write syscall
    printf("\n[TEST] write() syscall\n");
    const char* msg = "  Direct write syscall!\n";
    write(1, msg, strlen(msg));
    
    printf("\n[TEST] getpid()\n");
    printf("  PID: %d\n", getpid());
    
    // ========================================
    // FILE* functions tests
    // ========================================
    printf("\n[TEST] FILE* functions\n");
    
    // Test fopen with a file we know exists
    FILE* fp = fopen("/HELLO.TXT", "r");
    if (fp) {
        printf("  fopen(\"/HELLO.TXT\", \"r\") = %p (fd=%d)\n", (void*)fp, fp->fd);
        
        // Test fread
        char readbuf[64];
        memset(readbuf, 0, sizeof(readbuf));
        size_t nread = fread(readbuf, 1, sizeof(readbuf) - 1, fp);
        printf("  fread() returned %zu bytes\n", nread);
        if (nread > 0) {
            // Remove trailing newline for cleaner output
            if (readbuf[nread-1] == '\n') readbuf[nread-1] = '\0';
            printf("  Contents: \"%s\"\n", readbuf);
        }
        
        // Test feof/ferror
        printf("  feof() = %d, ferror() = %d\n", feof(fp), ferror(fp));
        
        // Test fclose
        int rc = fclose(fp);
        printf("  fclose() = %d\n", rc);
    } else {
        printf("  fopen(\"/HELLO.TXT\", \"r\") failed\n");
    }
    
    // Test fopen with non-existent file
    fp = fopen("/NONEXISTENT.TXT", "r");
    printf("  fopen(\"/NONEXISTENT.TXT\") = %p (expected NULL)\n", (void*)fp);
    
    // Test fputs/puts to stdout
    printf("\n[TEST] fputs/puts\n");
    fputs("  fputs to stdout\n", stdout);
    puts("  puts to stdout");
    
    // Test fprintf
    printf("\n[TEST] fprintf\n");
    fprintf(stdout, "  fprintf: int=%d, hex=0x%x, str=%s\n", 42, 0xCAFE, "hello");
    fprintf(stderr, "  fprintf to stderr\n");
    
    // Test putchar/fputc
    printf("\n[TEST] putchar/fputc\n");
    printf("  Characters: ");
    putchar('A');
    putchar('B');
    fputc('C', stdout);
    fputc('\n', stdout);
    
    // Test sprintf/snprintf
    printf("\n[TEST] sprintf/snprintf\n");
    char sprbuf[64];
    int len = sprintf(sprbuf, "Value: %d", 12345);
    printf("  sprintf returned %d, result: \"%s\"\n", len, sprbuf);
    
    len = snprintf(sprbuf, 10, "Long string that will be truncated");
    printf("  snprintf(10) returned %d, result: \"%s\"\n", len, sprbuf);
    
    // Test fseek/ftell/rewind
    printf("\n[TEST] fseek/ftell/rewind\n");
    fp = fopen("/HELLO.TXT", "r");
    if (fp) {
        // Read first 5 bytes
        char seekbuf[32];
        memset(seekbuf, 0, sizeof(seekbuf));
        fread(seekbuf, 1, 5, fp);
        printf("  First 5 bytes: \"%s\"\n", seekbuf);
        
        // Check position with ftell
        long pos = ftell(fp);
        printf("  ftell() after read = %ld\n", pos);
        
        // Seek back to start
        fseek(fp, 0, 0); // SEEK_SET
        pos = ftell(fp);
        printf("  ftell() after fseek(0, SEEK_SET) = %ld\n", pos);
        
        // Seek to position 6
        fseek(fp, 6, 0); // SEEK_SET
        memset(seekbuf, 0, sizeof(seekbuf));
        fread(seekbuf, 1, 4, fp);
        printf("  4 bytes after seek to 6: \"%s\"\n", seekbuf);
        
        // Test rewind
        rewind(fp);
        pos = ftell(fp);
        printf("  ftell() after rewind = %ld\n", pos);
        
        fclose(fp);
    } else {
        printf("  fopen failed for seek test\n");
    }
    
    // Test getenv/setenv
    printf("\n[TEST] getenv/setenv\n");
    char* val = getenv("TEST_VAR");
    printf("  getenv(\"TEST_VAR\") before set = %s\n", val ? val : "(null)");
    
    int rc = setenv("TEST_VAR", "hello_world", 1);
    printf("  setenv(\"TEST_VAR\", \"hello_world\", 1) = %d\n", rc);
    
    val = getenv("TEST_VAR");
    printf("  getenv(\"TEST_VAR\") after set = %s\n", val ? val : "(null)");
    
    // Test setenv with overwrite=0
    rc = setenv("TEST_VAR", "new_value", 0);
    val = getenv("TEST_VAR");
    printf("  After setenv with overwrite=0, value = %s\n", val ? val : "(null)");
    
    // Test unsetenv
    rc = unsetenv("TEST_VAR");
    val = getenv("TEST_VAR");
    printf("  After unsetenv, getenv = %s\n", val ? val : "(null)");

    printf("\n========================================\n");
    printf("  All libc tests completed!\n");
    printf("========================================\n");
    
    return 0;
}
