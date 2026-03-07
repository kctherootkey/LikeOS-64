#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdint.h>

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 4096

/* Buffering modes for setvbuf */
#define _IONBF 0   /* unbuffered */
#define _IOLBF 1   /* line buffered */
#define _IOFBF 2   /* fully buffered */

typedef struct {
    int fd;
    /* Read buffer */
    unsigned char* buffer;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_end;
    /* Write buffer */
    unsigned char* wbuf;
    size_t wbuf_size;
    size_t wbuf_pos;
    /* Buffering mode and state */
    int buf_mode;       /* _IONBF, _IOLBF, _IOFBF */
    int flags;
    int error;
    int eof;
    int ungetc_buf;     /* -1 if empty, else the ungotten char */
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
int setvbuf(FILE* stream, char* buf, int mode, size_t size);
void setbuf(FILE* stream, char* buf);
void setlinebuf(FILE* stream);
int fileno(FILE* stream);

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

// va_list formatted I/O (include <stdarg.h> before using these)
#ifdef __GNUC__
#include <stdarg.h>
int vfprintf(FILE* stream, const char* format, va_list ap);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
#endif

int scanf(const char* format, ...);
int fscanf(FILE* stream, const char* format, ...);
int sscanf(const char* str, const char* format, ...);

// Error reporting
void perror(const char* s);

#endif
