// LikeOS-64 Kernel
// A minimal 64-bit kernel that displays a message using VGA text mode

// VGA text mode buffer address
#define VGA_BUFFER ((volatile unsigned short*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

// VGA color attributes (foreground: white, background: black)
#define VGA_COLOR_WHITE_ON_BLACK 0x0F00

// Function prototypes
void kernel_main(void);
void clear_screen(void);
void print_string(const char* str, int x, int y);
void print_char(char c, int x, int y);
int strlen(const char* str);

// Kernel entry point
void kernel_main(void) {
    // Clear the screen
    clear_screen();
    
    // Print our boot message
    print_string("LikeOS-64 Booting", 30, 12);
    
    // Print additional info
    print_string("64-bit Long Mode Active", 28, 14);
    print_string("Kernel loaded successfully", 27, 15);
    
    // Halt the CPU (infinite loop)
    while (1) {
        __asm__ volatile ("hlt");
    }
}

// Clear the entire screen
void clear_screen(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = VGA_COLOR_WHITE_ON_BLACK | ' ';
    }
}

// Print a string at the specified position
void print_string(const char* str, int x, int y) {
    int len = strlen(str);
    for (int i = 0; i < len; i++) {
        print_char(str[i], x + i, y);
    }
}

// Print a character at the specified position
void print_char(char c, int x, int y) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        int index = y * VGA_WIDTH + x;
        VGA_BUFFER[index] = VGA_COLOR_WHITE_ON_BLACK | c;
    }
}

// Simple string length function
int strlen(const char* str) {
    int length = 0;
    while (str[length] != '\0') {
        length++;
    }
    return length;
}
