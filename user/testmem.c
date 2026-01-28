// testmem - Memory allocation test program for LikeOS-64
// Usage: testmem <size_mb> [small]
//   size_mb: Total memory to allocate in megabytes
//   small:   If specified, allocate in small chunks (4KB) instead of one big block

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SMALL_CHUNK_SIZE 4096  // 4KB chunks for "small" mode

static int test_single_allocation(size_t size_mb) {
    size_t size = size_mb * 1024 * 1024;
    
    printf("Allocating %lu MB as single block...\n", (unsigned long)size_mb);
    
    volatile uint8_t* buf = (volatile uint8_t*)malloc(size);
    if (!buf) {
        printf("FAILED: malloc returned NULL\n");
        return 1;
    }
    
    printf("  Allocated at %p\n", (void*)buf);
    
    // Write test values
    printf("  Writing test patterns...\n");
    buf[0] = 0xAA;                    // Beginning
    buf[size / 2] = 0x55;             // Middle
    buf[size - 1] = 0xBB;             // End
    
    // Verify reads
    printf("  Verifying...\n");
    int ok = 1;
    
    if (buf[0] != 0xAA) {
        printf("  FAIL: Beginning: expected 0xAA, got 0x%02x\n", buf[0]);
        ok = 0;
    } else {
        printf("  OK: Beginning = 0x%02x\n", buf[0]);
    }
    
    if (buf[size / 2] != 0x55) {
        printf("  FAIL: Middle: expected 0x55, got 0x%02x\n", buf[size / 2]);
        ok = 0;
    } else {
        printf("  OK: Middle = 0x%02x\n", buf[size / 2]);
    }
    
    if (buf[size - 1] != 0xBB) {
        printf("  FAIL: End: expected 0xBB, got 0x%02x\n", buf[size - 1]);
        ok = 0;
    } else {
        printf("  OK: End = 0x%02x\n", buf[size - 1]);
    }
    
    printf("  Freeing memory...\n");
    free((void*)buf);
    
    if (ok) {
        printf("SUCCESS: Single allocation test passed!\n");
        return 0;
    } else {
        printf("FAILED: Single allocation test failed!\n");
        return 1;
    }
}

static int test_small_allocations(size_t total_mb) {
    size_t total_size = total_mb * 1024 * 1024;
    size_t num_chunks = total_size / SMALL_CHUNK_SIZE;
    
    printf("Allocating %lu MB as %lu small chunks (%d bytes each)...\n", 
           (unsigned long)total_mb, (unsigned long)num_chunks, SMALL_CHUNK_SIZE);
    
    // Allocate array to hold chunk pointers
    volatile uint8_t** chunks = (volatile uint8_t**)malloc(num_chunks * sizeof(uint8_t*));
    if (!chunks) {
        printf("FAILED: Cannot allocate chunk pointer array\n");
        return 1;
    }
    
    // Allocate all chunks
    size_t allocated = 0;
    for (size_t i = 0; i < num_chunks; i++) {
        chunks[i] = (volatile uint8_t*)malloc(SMALL_CHUNK_SIZE);
        if (!chunks[i]) {
            printf("FAILED: malloc returned NULL at chunk %lu\n", (unsigned long)i);
            // Free already allocated chunks
            for (size_t j = 0; j < i; j++) {
                free((void*)chunks[j]);
            }
            free((void*)chunks);
            return 1;
        }
        allocated++;
        
        // Progress indicator every 1000 chunks
        if ((i + 1) % 1000 == 0 || i == num_chunks - 1) {
            printf("  Allocated %lu/%lu chunks (%lu MB)\r", 
                   (unsigned long)(i + 1), (unsigned long)num_chunks,
                   (unsigned long)((i + 1) * SMALL_CHUNK_SIZE / (1024 * 1024)));
        }
    }
    printf("\n");
    
    printf("  Writing test patterns to all chunks...\n");
    for (size_t i = 0; i < num_chunks; i++) {
        // Write at beginning, middle, and end of each chunk
        chunks[i][0] = (uint8_t)(i & 0xFF);
        chunks[i][SMALL_CHUNK_SIZE / 2] = (uint8_t)((i >> 8) & 0xFF);
        chunks[i][SMALL_CHUNK_SIZE - 1] = (uint8_t)((i ^ 0xAA) & 0xFF);
    }
    
    printf("  Verifying all chunks...\n");
    int errors = 0;
    for (size_t i = 0; i < num_chunks; i++) {
        uint8_t expected_begin = (uint8_t)(i & 0xFF);
        uint8_t expected_mid = (uint8_t)((i >> 8) & 0xFF);
        uint8_t expected_end = (uint8_t)((i ^ 0xAA) & 0xFF);
        
        if (chunks[i][0] != expected_begin) {
            if (errors < 5) {
                printf("  FAIL: Chunk %lu begin: expected 0x%02x, got 0x%02x\n",
                       (unsigned long)i, expected_begin, chunks[i][0]);
            }
            errors++;
        }
        if (chunks[i][SMALL_CHUNK_SIZE / 2] != expected_mid) {
            if (errors < 5) {
                printf("  FAIL: Chunk %lu middle: expected 0x%02x, got 0x%02x\n",
                       (unsigned long)i, expected_mid, chunks[i][SMALL_CHUNK_SIZE / 2]);
            }
            errors++;
        }
        if (chunks[i][SMALL_CHUNK_SIZE - 1] != expected_end) {
            if (errors < 5) {
                printf("  FAIL: Chunk %lu end: expected 0x%02x, got 0x%02x\n",
                       (unsigned long)i, expected_end, chunks[i][SMALL_CHUNK_SIZE - 1]);
            }
            errors++;
        }
    }
    
    if (errors > 5) {
        printf("  ... and %d more errors\n", errors - 5);
    }
    
    printf("  Freeing all chunks...\n");
    for (size_t i = 0; i < num_chunks; i++) {
        free((void*)chunks[i]);
    }
    free((void*)chunks);
    
    if (errors == 0) {
        printf("SUCCESS: Small allocations test passed! (%lu chunks verified)\n", 
               (unsigned long)num_chunks);
        return 0;
    } else {
        printf("FAILED: Small allocations test had %d errors\n", errors);
        return 1;
    }
}

int main(int argc, char** argv) {
    printf("=== LikeOS-64 Memory Test ===\n\n");
    
    if (argc < 2) {
        printf("Usage: testmem <size_mb> [small]\n");
        printf("  size_mb: Memory size to allocate in megabytes\n");
        printf("  small:   Use small 4KB allocations instead of one block\n");
        printf("\nExamples:\n");
        printf("  testmem 10       - Allocate 10MB as single block\n");
        printf("  testmem 100      - Allocate 100MB as single block\n");
        printf("  testmem 50 small - Allocate 50MB as 4KB chunks\n");
        return 1;
    }
    
    int size_mb = atoi(argv[1]);
    if (size_mb <= 0) {
        printf("Error: Invalid size '%s'\n", argv[1]);
        return 1;
    }
    
    int use_small = 0;
    if (argc >= 3 && strcmp(argv[2], "small") == 0) {
        use_small = 1;
    }
    
    printf("Test parameters:\n");
    printf("  Size: %d MB\n", size_mb);
    printf("  Mode: %s\n\n", use_small ? "small chunks (4KB)" : "single allocation");
    
    int result;
    if (use_small) {
        result = test_small_allocations((size_t)size_mb);
    } else {
        result = test_single_allocation((size_t)size_mb);
    }
    
    printf("\n=== Test %s ===\n", result == 0 ? "PASSED" : "FAILED");
    return result;
}
