#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 4096

/* Temporary file directory */
#define P_tmpdir "/tmp"
#define L_tmpnam 20

/* ISO C requires these three in <stdio.h>.  They were missing, which is not a
 * theoretical gap: libpng's tools use FILENAME_MAX to size a path buffer and
 * simply failed to compile.
 *
 * The values are this system's real limits rather than borrowed constants:
 *   FILENAME_MAX  PATH_MAX (limits.h) -- the longest path fopen() can open.
 *   FOPEN_MAX     OPEN_MAX (limits.h) -- a stream needs a descriptor, so no
 *                 program can have more streams open than the fd table holds.
 *   TMP_MAX       how many distinct names tmpnam() can return.  It builds
 *                 "/tmp/tn<pid>_<counter>" into L_tmpnam bytes, so with a
 *                 five-digit pid there is room for six counter digits; 10000
 *                 is comfortably inside that and far above the 25 the standard
 *                 demands.  Understating it is safe, overstating it is not.
 */
#define FILENAME_MAX 4096
#define FOPEN_MAX 256
#define TMP_MAX 10000

/* Buffering modes for setvbuf */
#define _IONBF 0   /* unbuffered */
#define _IOLBF 1   /* line buffered */
#define _IOFBF 2   /* fully buffered */

/* FILE is a tagged struct, and the typedef is behind a shared guard, so that
 * <wchar.h> can declare the wide stream functions without pulling all of
 * <stdio.h> in -- and so that including both, in either order, defines the
 * typedef exactly once. */
#ifndef __FILE_defined
#define __FILE_defined
typedef struct _IO_FILE FILE;
#endif

struct _IO_FILE {
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
    /* Orientation, per ISO C: 0 = not yet set, <0 = byte, >0 = wide.  A
     * stream is committed to one or the other by the first read or write and
     * cannot be mixed afterwards, so the two sets of functions never have to
     * agree about buffer contents. */
    int wide_mode;
    /* Shift state for the wide functions.  UTF-8 is stateless between
     * characters, but fgetwc has to be able to stop half way through a
     * sequence and continue on the next call. */
    unsigned wc_count;
    unsigned wc_value;
};

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// File operations
FILE* fopen(const char* pathname, const char* mode);
int fclose(FILE* stream);
FILE* popen(const char* command, const char* type);
int pclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fseeko(FILE* stream, off_t offset, int whence);
off_t ftello(FILE* stream);

/* fpos_t and its two functions, required by ISO C.
 *
 * A STRUCT, not a bare integer, deliberately.  The standard says fpos_t is an
 * opaque object type and that a value may only be obtained from fgetpos() and
 * handed back to fsetpos() -- arithmetic on it is not allowed.  Making it a
 * typedef for off_t would let such code compile here and then break on any
 * other system, and would tie our hands if a position ever has to carry more
 * than a byte offset (a wide-oriented stream's parse state, for instance). */
typedef struct {
	off_t __pos;
} fpos_t;

int fgetpos(FILE* stream, fpos_t* pos);
int fsetpos(FILE* stream, const fpos_t* pos);
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
int vsprintf(char* str, const char* format, va_list ap);
int vprintf(const char* format, va_list ap);
int vasprintf(char **strp, const char* format, va_list ap);
#endif
int asprintf(char **strp, const char* format, ...);

int scanf(const char* format, ...);
int fscanf(FILE* stream, const char* format, ...);
int sscanf(const char* str, const char* format, ...);
#if defined(_STDARG_H) || defined(va_start)
int vscanf(const char* format, va_list ap);
int vfscanf(FILE* stream, const char* format, va_list ap);
int vsscanf(const char* str, const char* format, va_list ap);
#endif

// Error reporting
void perror(const char* s);

// Additional I/O
#include <sys/types.h>
ssize_t getline(char** lineptr, size_t* n, FILE* stream);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);
FILE* tmpfile(void);
char* tmpnam(char* s);
int mkstemp(char* templ);
int mkstemps(char* templ, int suffixlen);
int remove(const char* pathname);
int rename(const char* oldpath, const char* newpath);
FILE* fdopen(int fd, const char *mode);
FILE* freopen(const char *pathname, const char *mode, FILE *stream);
int dprintf(int fd, const char *format, ...);
int vdprintf(int fd, const char *format, va_list ap);

/* Implement flockfile/funlockfile as no-ops (single-threaded) */
static inline void flockfile(FILE *f) { (void)f; }
static inline void funlockfile(FILE *f) { (void)f; }
static inline int ftrylockfile(FILE *f) { (void)f; return 0; }
static inline int getc_unlocked(FILE *f) { return fgetc(f); }

#endif
