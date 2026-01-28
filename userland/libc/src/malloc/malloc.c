#include "../../include/stdlib.h"
#include "../../include/string.h"
#include "../../include/unistd.h"
#include "../../include/errno.h"

// Simple block header for tracking allocations
typedef struct block {
    size_t size;
    int free;
    struct block* next;
} block_t;

#define BLOCK_SIZE sizeof(block_t)
#define align4(x) (((x) + 3) & ~3)
#define align8(x) (((x) + 7) & ~7)

static block_t* heap_start = NULL;

// Find a free block that fits
static block_t* find_free_block(block_t** last, size_t size) {
    block_t* current = heap_start;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

// Split a block if it's significantly larger than needed
// Returns the block to use (same as input, but potentially smaller)
static void split_block(block_t* block, size_t size) {
    // Only split if remaining space can hold a useful block
    // Need at least BLOCK_SIZE for header + some minimum payload (16 bytes)
    if (block->size >= size + BLOCK_SIZE + 16) {
        // Create new block after the allocated space
        block_t* new_block = (block_t*)((char*)(block + 1) + size);
        new_block->size = block->size - size - BLOCK_SIZE;
        new_block->free = 1;
        new_block->next = block->next;
        
        // Update original block
        block->size = size;
        block->next = new_block;
    }
}

// Request more space from the system
static block_t* request_space(block_t* last, size_t size) {
    block_t* block;
    block = sbrk(0);
    void* request = sbrk(BLOCK_SIZE + size);
    
    if (request == (void*)-1) {
        return NULL; // sbrk failed
    }
    
    if (last) {
        last->next = block;
    }
    
    block->size = size;
    block->free = 0;
    block->next = NULL;
    return block;
}

void* malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    size = align8(size);
    block_t* block;
    
    if (!heap_start) {
        // First call to malloc
        block = request_space(NULL, size);
        if (!block) {
            return NULL;
        }
        heap_start = block;
    } else {
        block_t* last = heap_start;
        block = find_free_block(&last, size);
        
        if (!block) {
            // No free block found, request more space
            block = request_space(last, size);
            if (!block) {
                return NULL;
            }
        } else {
            // Found a free block - split it if much larger than needed
            split_block(block, size);
            block->free = 0;
        }
    }
    
    return (void*)(block + 1);
}

// Get block from user pointer
static block_t* get_block_ptr(void* ptr) {
    return (block_t*)ptr - 1;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    
    block_t* block = get_block_ptr(ptr);
    block->free = 1;
    
    // Coalesce with next block if it's free
    if (block->next && block->next->free) {
        block->size += BLOCK_SIZE + block->next->size;
        block->next = block->next->next;
    }
    
    // Coalesce with previous block if it's free
    // Need to find the previous block by walking from heap_start
    block_t* prev = NULL;
    block_t* current = heap_start;
    while (current && current != block) {
        prev = current;
        current = current->next;
    }
    
    if (prev && prev->free) {
        prev->size += BLOCK_SIZE + block->size;
        prev->next = block->next;
    }
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    block_t* block = get_block_ptr(ptr);
    
    if (block->size >= size) {
        return ptr;
    }
    
    // Need to allocate new block
    void* new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;
    }
    
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}
