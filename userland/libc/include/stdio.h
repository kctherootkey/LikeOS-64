#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdint.h>

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 1024

typedef struct {
    int fd;
    unsigned char* buffer;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_end;
    int flags;
    int error;
    int eof;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// File operations
FILE* fopen(const char* pathname, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
int fflush(FILE* stream);

// Character I/O
int fgetc(FILE* stream);
int getc(FILE* stream);
int getchar(void);
int fputc(int c, FILE* stream);
int putc(int c, FILE* stream);
int putchar(int c);
int ungetc(int c, FILE* stream);

// Line I/O
char* fgets(char* s, int size, FILE* stream);
int fputs(const char* s, FILE* stream);
int puts(const char* s);

// Formatted I/O
int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);

int scanf(const char* format, ...);
int fscanf(FILE* stream, const char* format, ...);
int sscanf(const char* str, const char* format, ...);

// Error reporting
void perror(const char* s);

#endif
