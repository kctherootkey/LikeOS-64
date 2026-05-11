#include "../../include/stdio.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"
#include "../../include/errno.h"
#include "../../include/unistd.h"
#include "../../include/stdarg.h"
#include "../../include/fcntl.h"

// Standard file streams - with buffering
static unsigned char __stdin_rbuf[BUFSIZ];
static unsigned char __stdout_wbuf[BUFSIZ];

static FILE __stdin  = { .fd = 0, .buffer = __stdin_rbuf, .buf_size = BUFSIZ,
                         .buf_pos = 0, .buf_end = 0,
                         .wbuf = NULL, .wbuf_size = 0, .wbuf_pos = 0,
                         .buf_mode = _IOFBF, .flags = 0, .eof = 0, .error = 0,
                         .ungetc_buf = -1 };
static FILE __stdout = { .fd = 1, .buffer = NULL, .buf_size = 0,
                         .buf_pos = 0, .buf_end = 0,
                         .wbuf = __stdout_wbuf, .wbuf_size = BUFSIZ, .wbuf_pos = 0,
                         .buf_mode = _IOLBF, .flags = 0, .eof = 0, .error = 0,
                         .ungetc_buf = -1 };
static FILE __stderr = { .fd = 2, .buffer = NULL, .buf_size = 0,
                         .buf_pos = 0, .buf_end = 0,
                         .wbuf = NULL, .wbuf_size = 0, .wbuf_pos = 0,
                         .buf_mode = _IONBF, .flags = 0, .eof = 0, .error = 0,
                         .ungetc_buf = -1 };

FILE* stdin = &__stdin;
FILE* stdout = &__stdout;
FILE* stderr = &__stderr;

FILE* fopen(const char* pathname, const char* mode) {
    if (!pathname || !mode) {
        return NULL;
    }
    
    int flags = 0;
    
    // Parse mode string
    switch (mode[0]) {
        case 'r':
            flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
            break;
        case 'w':
            flags = O_CREAT | O_TRUNC | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
            break;
        case 'a':
            flags = O_CREAT | O_APPEND | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
            break;
        default:
            return NULL;
    }
    
    int fd = open(pathname, flags);
    if (fd < 0) {
        return NULL;
    }
    
    FILE* fp = malloc(sizeof(FILE));
    if (!fp) {
        close(fd);
        return NULL;
    }
    
    fp->fd = fd;
    fp->buffer = malloc(BUFSIZ);   /* read buffer */
    fp->buf_size = fp->buffer ? BUFSIZ : 0;
    fp->buf_pos = 0;
    fp->buf_end = 0;
    fp->wbuf = malloc(BUFSIZ);     /* write buffer */
    fp->wbuf_size = fp->wbuf ? BUFSIZ : 0;
    fp->wbuf_pos = 0;
    fp->buf_mode = _IOFBF;         /* fully buffered by default */
    fp->flags = flags;
    fp->error = 0;
    fp->eof = 0;
    fp->ungetc_buf = -1;
    
    return fp;
}

int fclose(FILE* stream) {
    if (!stream) {
        return EOF;
    }
    
    fflush(stream);
    int result = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream->buffer);
        free(stream->wbuf);
        free(stream);
    }
    return result == 0 ? 0 : EOF;
}

/* ------------------------------------------------------------------ */
/* Internal: flush write buffer to fd                                   */
/* ------------------------------------------------------------------ */
static int __flush_wbuf(FILE* stream) {
    if (!stream || stream->wbuf_pos == 0) return 0;
    
    size_t remaining = stream->wbuf_pos;
    const unsigned char* p = stream->wbuf;
    while (remaining > 0) {
        ssize_t n = write(stream->fd, p, remaining);
        if (n < 0) {
            stream->error = 1;
            return EOF;
        }
        p += n;
        remaining -= n;
    }
    stream->wbuf_pos = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Internal: fill read buffer from fd                                   */
/* ------------------------------------------------------------------ */
static int __fill_rbuf(FILE* stream) {
    if (!stream || !stream->buffer || stream->buf_size == 0) return -1;

    stream->buf_pos = 0;
    stream->buf_end = 0;
    ssize_t n = read(stream->fd, stream->buffer, stream->buf_size);
    if (n < 0) {
        stream->error = 1;
        return -1;
    }
    if (n == 0) {
        stream->eof = 1;
        return -1;
    }
    stream->buf_end = n;
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) return 0;
    
    size_t total = size * nmemb;
    unsigned char* dst = (unsigned char*)ptr;
    size_t got = 0;

    /* First: return any ungotten char */
    if (stream->ungetc_buf >= 0 && got < total) {
        *dst++ = (unsigned char)stream->ungetc_buf;
        stream->ungetc_buf = -1;
        got++;
    }

    /* Drain read buffer first */
    if (stream->buffer && stream->buf_pos < stream->buf_end) {
        size_t avail = stream->buf_end - stream->buf_pos;
        size_t take = (total - got < avail) ? total - got : avail;
        memcpy(dst, stream->buffer + stream->buf_pos, take);
        stream->buf_pos += take;
        dst += take;
        got += take;
    }

    /* If we still need data: for large reads, bypass buffer */
    if (got < total && (total - got) >= (stream->buf_size > 0 ? stream->buf_size : 1)) {
        ssize_t n = read(stream->fd, dst, total - got);
        if (n < 0) { stream->error = 1; }
        else if (n == 0) { stream->eof = 1; }
        else { got += n; }
        return got / size;
    }

    /* For small remaining reads, fill buffer and copy */
    while (got < total) {
        if (!stream->buffer || stream->buf_size == 0) {
            /* No read buffer — direct read */
            ssize_t n = read(stream->fd, dst, total - got);
            if (n < 0) { stream->error = 1; break; }
            if (n == 0) { stream->eof = 1; break; }
            got += n;
            break;
        }
        if (__fill_rbuf(stream) < 0) break;
        size_t avail = stream->buf_end - stream->buf_pos;
        size_t take = (total - got < avail) ? total - got : avail;
        memcpy(dst, stream->buffer + stream->buf_pos, take);
        stream->buf_pos += take;
        dst += take;
        got += take;
    }

    return got / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) return 0;
    
    size_t total = size * nmemb;
    const unsigned char* src = (const unsigned char*)ptr;

    /* Unbuffered: write directly */
    if (stream->buf_mode == _IONBF || !stream->wbuf || stream->wbuf_size == 0) {
        ssize_t n = write(stream->fd, src, total);
        if (n < 0) { stream->error = 1; return 0; }
        return n / size;
    }

    /* Line buffered: copy to buffer, flush on '\n' or when full */
    if (stream->buf_mode == _IOLBF) {
        size_t written = 0;
        while (written < total) {
            /* Find next newline in remaining data */
            size_t remaining = total - written;
            const unsigned char* nl = memchr(src + written, '\n', remaining);
            size_t chunk;
            int do_flush = 0;

            if (nl) {
                chunk = (nl - (src + written)) + 1; /* include the '\n' */
                do_flush = 1;
            } else {
                chunk = remaining;
            }

            /* Copy to buffer, flushing as needed */
            while (chunk > 0) {
                size_t space = stream->wbuf_size - stream->wbuf_pos;
                size_t copy = (chunk < space) ? chunk : space;
                memcpy(stream->wbuf + stream->wbuf_pos, src + written, copy);
                stream->wbuf_pos += copy;
                written += copy;
                chunk -= copy;
                if (stream->wbuf_pos >= stream->wbuf_size) {
                    if (__flush_wbuf(stream) == EOF) return written / size;
                }
            }
            if (do_flush) {
                if (__flush_wbuf(stream) == EOF) return written / size;
            }
        }
        return written / size;
    }

    /* Fully buffered: copy to buffer, flush when full */
    size_t written = 0;
    while (written < total) {
        size_t space = stream->wbuf_size - stream->wbuf_pos;
        size_t chunk = total - written;
        if (chunk <= space) {
            memcpy(stream->wbuf + stream->wbuf_pos, src + written, chunk);
            stream->wbuf_pos += chunk;
            written += chunk;
        } else {
            /* Fill remainder of buffer and flush */
            memcpy(stream->wbuf + stream->wbuf_pos, src + written, space);
            stream->wbuf_pos += space;
            written += space;
            if (__flush_wbuf(stream) == EOF) return written / size;
        }
    }
    return written / size;
}

int fgetc(FILE* stream) {
    if (!stream) return EOF;

    /* Check ungetc buffer first */
    if (stream->ungetc_buf >= 0) {
        int c = stream->ungetc_buf;
        stream->ungetc_buf = -1;
        return c;
    }

    /* Read from buffer */
    if (stream->buffer && stream->buf_size > 0) {
        if (stream->buf_pos >= stream->buf_end) {
            if (__fill_rbuf(stream) < 0) return EOF;
        }
        return stream->buffer[stream->buf_pos++];
    }

    /* No read buffer — direct 1-byte read */
    unsigned char c;
    ssize_t n = read(stream->fd, &c, 1);
    if (n <= 0) {
        if (n == 0) stream->eof = 1;
        else stream->error = 1;
        return EOF;
    }
    return c;
}

int getchar(void) {
    return fgetc(stdin);
}

char* fgets(char* s, int size, FILE* stream) {
    if (!s || size <= 0 || !stream) {
        return NULL;
    }
    
    int i;
    for (i = 0; i < size - 1; i++) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) {
                return NULL;
            }
            break;
        }
        s[i] = c;
        if (c == '\n') {
            i++;
            break;
        }
    }
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE* stream) {
    unsigned char ch = c;
    if (fwrite(&ch, 1, 1, stream) != 1) {
        return EOF;
    }
    return (unsigned char)c;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int getc(FILE* stream) {
    return fgetc(stream);
}

int putc(int c, FILE* stream) {
    return fputc(c, stream);
}

FILE* fdopen(int fd, const char* mode) {
    if (fd < 0 || !mode) return NULL;
    
    FILE* fp = malloc(sizeof(FILE));
    if (!fp) return NULL;

    int flags = 0;
    switch (mode[0]) {
        case 'r': flags = (mode[1] == '+') ? O_RDWR : O_RDONLY; break;
        case 'w': flags = O_CREAT | O_TRUNC | ((mode[1] == '+') ? O_RDWR : O_WRONLY); break;
        case 'a': flags = O_CREAT | O_APPEND | ((mode[1] == '+') ? O_RDWR : O_WRONLY); break;
        default: free(fp); return NULL;
    }

    fp->fd = fd;
    fp->buffer = malloc(BUFSIZ);
    fp->buf_size = fp->buffer ? BUFSIZ : 0;
    fp->buf_pos = 0;
    fp->buf_end = 0;
    fp->wbuf = malloc(BUFSIZ);
    fp->wbuf_size = fp->wbuf ? BUFSIZ : 0;
    fp->wbuf_pos = 0;
    fp->buf_mode = _IOFBF;
    fp->flags = flags;
    fp->error = 0;
    fp->eof = 0;
    fp->ungetc_buf = -1;
    return fp;
}

int fputs(const char* s, FILE* stream) {
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) {
        return EOF;
    }
    return 0;
}

int puts(const char* s) {
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF) {
        return EOF;
    }
    return 0;
}

int fflush(FILE* stream) {
    if (!stream) {
        /* Flush all open streams — at minimum stdout */
        __flush_wbuf(stdout);
        return 0;
    }
    return __flush_wbuf(stream);
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream) {
        return -1;
    }
    
    /* Flush any pending writes */
    __flush_wbuf(stream);
    
    /* Invalidate read buffer */
    stream->buf_pos = 0;
    stream->buf_end = 0;
    stream->ungetc_buf = -1;
    
    /* Clear EOF flag on seek */
    stream->eof = 0;
    
    off_t result = lseek(stream->fd, offset, whence);
    if (result < 0) {
        stream->error = 1;
        return -1;
    }
    return 0;
}

long ftell(FILE* stream) {
    if (!stream) {
        return -1;
    }
    
    /* Flush any pending writes so the fd position is up to date */
    __flush_wbuf(stream);
    
    long pos = (long)lseek(stream->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    
    /* Subtract unread data still sitting in the read buffer.
     * The kernel fd position is *ahead* of what the user has consumed. */
    if (stream->buffer && stream->buf_end > stream->buf_pos)
        pos -= (long)(stream->buf_end - stream->buf_pos);
    
    /* Account for ungetc'd character */
    if (stream->ungetc_buf >= 0)
        pos--;
    
    return pos;
}

void rewind(FILE* stream) {
    fseek(stream, 0, SEEK_SET);
}

int feof(FILE* stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE* stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE* stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

int ungetc(int c, FILE* stream) {
    if (!stream || c == EOF) return EOF;
    stream->ungetc_buf = (unsigned char)c;
    stream->eof = 0;
    return (unsigned char)c;
}

int fileno(FILE* stream) {
    if (!stream) return -1;
    return stream->fd;
}

int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    if (!stream) return -1;
    if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF) return -1;

    /* Flush any pending output first */
    __flush_wbuf(stream);

    stream->buf_mode = mode;

    if (mode == _IONBF) {
        /* Unbuffered: no write buffer needed */
        return 0;
    }

    /* If user provides a buffer, we can't really use it for both r+w,
       so just set the mode and keep our own buffers */
    (void)buf;
    (void)size;
    return 0;
}

void setbuf(FILE* stream, char* buf) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void setlinebuf(FILE* stream) {
    setvbuf(stream, NULL, _IOLBF, BUFSIZ);
}

// Helper function for number to string conversion
static int num_to_str(long long num, char* buf, int base, int uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char temp[64];
    int i = 0;
    int is_negative = 0;
    
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    
    if (num < 0 && base == 10) {
        is_negative = 1;
        num = -num;
    }
    
    unsigned long long unum = num;
    while (unum > 0) {
        temp[i++] = digits[unum % base];
        unum /= base;
    }
    
    int len = 0;
    if (is_negative) {
        buf[len++] = '-';
    }
    
    while (i > 0) {
        buf[len++] = temp[--i];
    }
    buf[len] = '\0';
    
    return len;
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    /* C99: when str is NULL or size is 0, return the number of characters
     * that would have been written (excluding the terminating NUL) had the
     * buffer been large enough.  asprintf/vasprintf depend on this two-pass
     * sizing behaviour. */
    if (!str || size == 0) {
        char tmpbuf[8192];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(tmpbuf, sizeof(tmpbuf), format, ap2);
        va_end(ap2);
        return n;
    }
    size_t pos = 0;

    while (*format && pos < size - 1) {
        if (*format != '%') {
            str[pos++] = *format++;
            continue;
        }
        format++; /* skip '%' */

        /* %% */
        if (*format == '%') {
            str[pos++] = '%';
            format++;
            continue;
        }

        /* --- Parse flags --- */
        int fl_minus = 0, fl_zero = 0, fl_plus = 0, fl_space = 0, fl_hash = 0;
        for (;;) {
            if      (*format == '-') { fl_minus = 1; format++; }
            else if (*format == '0') { fl_zero  = 1; format++; }
            else if (*format == '+') { fl_plus  = 1; format++; }
            else if (*format == ' ') { fl_space = 1; format++; }
            else if (*format == '#') { fl_hash  = 1; format++; }
            else break;
        }
        if (fl_minus) fl_zero = 0;

        /* --- Width --- */
        int width = 0;
        if (*format == '*') {
            width = va_arg(ap, int);
            if (width < 0) { fl_minus = 1; width = -width; }
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }
        }

        /* --- Precision --- */
        int prec = -1;
        if (*format == '.') {
            format++;
            prec = 0;
            if (*format == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    prec = prec * 10 + (*format - '0');
                    format++;
                }
            }
        }

        /* --- Length modifiers --- */
        int lng = 0;   /* 1 = long, 2 = long long */
        if (*format == 'l') {
            format++; lng = 1;
            if (*format == 'l') { format++; lng = 2; }
        } else if (*format == 'z' || *format == 'j') {
            format++; lng = 2;
        } else if (*format == 'h') {
            format++;
            if (*format == 'h') format++;
        }

        /* --- Specifier --- */
        char buf[64];
        int len = 0;

        switch (*format) {

        /* ---- signed integer ---- */
        case 'd': case 'i': {
            long long val;
            if (lng == 2)      val = va_arg(ap, long long);
            else if (lng == 1) val = va_arg(ap, long);
            else               val = va_arg(ap, int);

            char sign = 0;
            unsigned long long uval;
            if (val < 0) { sign = '-'; uval = (unsigned long long)(-(val + 1)) + 1; }
            else { uval = (unsigned long long)val; if (fl_plus) sign = '+'; else if (fl_space) sign = ' '; }

            len = num_to_str((long long)uval, buf, 10, 0);

            int total = len + (sign ? 1 : 0);
            int pad = (width > total) ? width - total : 0;

            if (!fl_minus && !fl_zero)
                for (int p = 0; p < pad && pos < size - 1; p++) str[pos++] = ' ';
            if (sign && pos < size - 1) str[pos++] = sign;
            if (!fl_minus && fl_zero)
                for (int p = 0; p < pad && pos < size - 1; p++) str[pos++] = '0';
            for (int i = 0; i < len && pos < size - 1; i++) str[pos++] = buf[i];
            if (fl_minus)
                for (int p = 0; p < pad && pos < size - 1; p++) str[pos++] = ' ';

            format++;
            continue;
        }

        /* ---- unsigned integer ---- */
        case 'u': {
            unsigned long long val;
            if (lng == 2)      val = va_arg(ap, unsigned long long);
            else if (lng == 1) val = va_arg(ap, unsigned long);
            else               val = va_arg(ap, unsigned int);

            len = num_to_str((long long)val, buf, 10, 0);

            int pad = (width > len) ? width - len : 0;
            if (!fl_minus) {
                char pc = fl_zero ? '0' : ' ';
                for (int p = 0; p < pad && pos < size - 1; p++) str[pos++] = pc;
            }
            for (int i = 0; i < len && pos < size - 1; i++) str[pos++] = buf[i];
            if (fl_minus)
                for (int p = 0; p < pad && pos < size - 1; p++) str[pos++] = ' ';

            format++;
            continue;
        }

        /* ---- hex ---- */
        case 'x': case 'X': {
            unsigned long long val;
            if (lng == 2)      val = va_arg(ap, unsigned long long);
            else if (lng == 1) val = va_arg(ap, unsigned long);
            else               val = va_arg(ap, unsigned int);

            len = num_to_str((long long)val, buf, 16, *format == 'X');
            int pfx = (fl_hash && val != 0) ? 2 : 0;
            int total = len + pfx;
            int pad = (width > total) ? width - total : 0;

            if (!fl_minus) {
                if (fl_zero) {
                    if (pfx) { if (pos < size-1) str[pos++]='0'; if (pos < size-1) str[pos++]=(*format=='X'?'X':'x'); }
                    for (int p=0; p<pad && pos<size-1; p++) str[pos++]='0';
                } else {
                    for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';
                    if (pfx) { if (pos < size-1) str[pos++]='0'; if (pos < size-1) str[pos++]=(*format=='X'?'X':'x'); }
                }
            } else {
                if (pfx) { if (pos < size-1) str[pos++]='0'; if (pos < size-1) str[pos++]=(*format=='X'?'X':'x'); }
            }
            for (int i=0; i<len && pos<size-1; i++) str[pos++]=buf[i];
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';

            format++;
            continue;
        }

        /* ---- octal ---- */
        case 'o': {
            unsigned long long val;
            if (lng == 2)      val = va_arg(ap, unsigned long long);
            else if (lng == 1) val = va_arg(ap, unsigned long);
            else               val = va_arg(ap, unsigned int);

            len = num_to_str((long long)val, buf, 8, 0);

            int pad = (width > len) ? width - len : 0;
            if (!fl_minus) {
                char pc = fl_zero ? '0' : ' ';
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=pc;
            }
            for (int i=0; i<len && pos<size-1; i++) str[pos++]=buf[i];
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';

            format++;
            continue;
        }

        /* ---- pointer ---- */
        case 'p': {
            void* ptr = va_arg(ap, void*);
            if (pos < size-1) str[pos++] = '0';
            if (pos < size-1) str[pos++] = 'x';
            len = num_to_str((long long)(unsigned long long)(unsigned long)ptr, buf, 16, 0);
            for (int i=0; i<len && pos<size-1; i++) str[pos++]=buf[i];
            format++;
            continue;
        }

        /* ---- string ---- */
        case 's': {
            char* s = va_arg(ap, char*);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (prec >= 0 && prec < slen) slen = prec;
            int pad = (width > slen) ? width - slen : 0;

            if (!fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';
            for (int i=0; i<slen && pos<size-1; i++) str[pos++]=s[i];
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';

            format++;
            continue;
        }

        /* ---- character ---- */
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';
            if (pos < size-1) str[pos++] = c;
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';
            format++;
            continue;
        }

        /* ---- floating-point ---- */
        case 'f': case 'F': {
            double val = va_arg(ap, double);
            int fprec = (prec < 0) ? 6 : prec;

            /* Handle negative / sign */
            int is_neg = 0;
            if (val < 0.0) { is_neg = 1; val = -val; }

            /* Separate integer and fractional parts.
             * Multiply fractional part by 10^fprec and round. */
            unsigned long long int_part = (unsigned long long)val;
            double frac = val - (double)int_part;

            /* Build the power-of-ten multiplier for the fractional digits */
            unsigned long long fmul = 1;
            for (int i = 0; i < fprec; i++) fmul *= 10;

            unsigned long long frac_val = (unsigned long long)(frac * (double)fmul + 0.5);
            /* Handle rounding overflow (e.g. 9.9999... rounds up) */
            if (frac_val >= fmul) {
                frac_val = 0;
                int_part++;
            }

            /* Convert integer part to string */
            char ibuf[24];
            int ilen = 0;
            if (int_part == 0) {
                ibuf[ilen++] = '0';
            } else {
                char tmp[24]; int ti = 0;
                unsigned long long v = int_part;
                while (v > 0) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
                while (ti > 0) ibuf[ilen++] = tmp[--ti];
            }

            /* Convert fractional part to string (zero-padded to fprec width) */
            char fbuf[24];
            if (fprec > 0) {
                for (int i = fprec - 1; i >= 0; i--) {
                    fbuf[i] = '0' + (char)(frac_val % 10);
                    frac_val /= 10;
                }
            }

            /* Total output length: sign + int + '.' + frac */
            char sign = 0;
            if (is_neg) sign = '-';
            else if (fl_plus) sign = '+';
            else if (fl_space) sign = ' ';

            int total = (sign ? 1 : 0) + ilen + (fprec > 0 ? 1 + fprec : 0);
            int pad = (width > total) ? width - total : 0;

            if (!fl_minus && !fl_zero)
                for (int p = 0; p < pad && pos < size-1; p++) str[pos++] = ' ';
            if (sign && pos < size-1) str[pos++] = sign;
            if (!fl_minus && fl_zero)
                for (int p = 0; p < pad && pos < size-1; p++) str[pos++] = '0';
            for (int i = 0; i < ilen && pos < size-1; i++) str[pos++] = ibuf[i];
            if (fprec > 0) {
                if (pos < size-1) str[pos++] = '.';
                for (int i = 0; i < fprec && pos < size-1; i++) str[pos++] = fbuf[i];
            }
            if (fl_minus)
                for (int p = 0; p < pad && pos < size-1; p++) str[pos++] = ' ';

            format++;
            continue;
        }

        case 'e': case 'E': {
            double val = va_arg(ap, double);
            int fprec = (prec < 0) ? 6 : prec;
            int is_neg = 0;
            if (val < 0.0) { is_neg = 1; val = -val; }

            /* Compute exponent: normalize val to [1.0, 10.0) */
            int exponent = 0;
            if (val != 0.0) {
                while (val >= 10.0) { val /= 10.0; exponent++; }
                while (val < 1.0)   { val *= 10.0; exponent--; }
            }

            /* Now val is in [1.0, 10.0). Separate integer and fraction */
            unsigned long long int_digit = (unsigned long long)val;
            double frac = val - (double)int_digit;
            unsigned long long fmul = 1;
            for (int i = 0; i < fprec; i++) fmul *= 10;
            unsigned long long frac_val = (unsigned long long)(frac * (double)fmul + 0.5);
            if (frac_val >= fmul) { frac_val = 0; int_digit++; if (int_digit >= 10) { int_digit = 1; exponent++; } }

            char sign = 0;
            if (is_neg) sign = '-';
            else if (fl_plus) sign = '+';
            else if (fl_space) sign = ' ';

            /* Build: [sign] digit '.' frac 'e' [+-] exp (at least 2 digits) */
            char ebuf[8];
            int abs_exp = exponent < 0 ? -exponent : exponent;
            int ei = 0;
            ebuf[ei++] = (*format == 'E') ? 'E' : 'e';
            ebuf[ei++] = exponent < 0 ? '-' : '+';
            if (abs_exp < 10) ebuf[ei++] = '0';
            { char tmp[8]; int ti=0; int ae=abs_exp; if(ae==0){tmp[ti++]='0';}else{while(ae>0){tmp[ti++]='0'+(char)(ae%10);ae/=10;}} while(ti>0) ebuf[ei++]=tmp[--ti]; }
            int elen = ei;

            char fbuf[24];
            if (fprec > 0) {
                for (int i = fprec - 1; i >= 0; i--) { fbuf[i] = '0' + (char)(frac_val % 10); frac_val /= 10; }
            }

            int total = (sign?1:0) + 1 + (fprec>0 ? 1+fprec : 0) + elen;
            int pad = (width > total) ? width - total : 0;

            if (!fl_minus && !fl_zero)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';
            if (sign && pos<size-1) str[pos++] = sign;
            if (!fl_minus && fl_zero)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]='0';
            if (pos<size-1) str[pos++] = '0' + (char)int_digit;
            if (fprec > 0) {
                if (pos<size-1) str[pos++] = '.';
                for (int i=0; i<fprec && pos<size-1; i++) str[pos++] = fbuf[i];
            }
            for (int i=0; i<elen && pos<size-1; i++) str[pos++] = ebuf[i];
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++]=' ';

            format++;
            continue;
        }

        case 'g': case 'G': {
            /* %g: use %e if exponent < -4 or >= precision, else %f style */
            double val = va_arg(ap, double);
            int fprec = (prec < 0) ? 6 : (prec == 0 ? 1 : prec);
            int is_neg = 0;
            if (val < 0.0) { is_neg = 1; val = -val; }

            int exponent = 0;
            double nval = val;
            if (nval != 0.0) {
                while (nval >= 10.0) { nval /= 10.0; exponent++; }
                while (nval < 1.0)   { nval *= 10.0; exponent--; }
            }

            /* Restore val (we just needed the exponent) */
            if (is_neg) {
                /* Re-format using %f or %e style. For simplicity, push the
                 * value back to its negative form and use 'f' or 'e' logic
                 * via a recursive snprintf into buf. */
            }

            char tmp_buf[80];
            int tlen;
            if (exponent < -4 || exponent >= fprec) {
                /* Use %e style with (fprec-1) digits after decimal */
                int ep = fprec - 1;
                if (ep < 0) ep = 0;
                /* Normalize to [1,10) */
                nval = val;
                int exp2 = 0;
                if (nval != 0.0) {
                    while (nval >= 10.0) { nval /= 10.0; exp2++; }
                    while (nval < 1.0)   { nval *= 10.0; exp2--; }
                }
                unsigned long long idig = (unsigned long long)nval;
                double fr = nval - (double)idig;
                unsigned long long fmul = 1;
                for (int i = 0; i < ep; i++) fmul *= 10;
                unsigned long long fv = (unsigned long long)(fr * (double)fmul + 0.5);
                if (fv >= fmul) { fv = 0; idig++; if (idig >= 10) { idig = 1; exp2++; } }
                tlen = 0;
                if (is_neg) tmp_buf[tlen++] = '-';
                tmp_buf[tlen++] = '0' + (char)idig;
                /* Strip trailing zeros unless # flag */
                char fbuf2[24]; int flen2 = ep;
                for (int i = ep-1; i >= 0; i--) { fbuf2[i] = '0'+(char)(fv%10); fv /= 10; }
                if (!fl_hash) { while (flen2>0 && fbuf2[flen2-1]=='0') flen2--; }
                if (flen2 > 0) {
                    tmp_buf[tlen++] = '.';
                    for (int i=0; i<flen2; i++) tmp_buf[tlen++] = fbuf2[i];
                }
                tmp_buf[tlen++] = (*format=='G') ? 'E' : 'e';
                tmp_buf[tlen++] = exp2 < 0 ? '-' : '+';
                int aexp = exp2 < 0 ? -exp2 : exp2;
                if (aexp < 10) tmp_buf[tlen++] = '0';
                { char et[8]; int ei2=0; int ae2=aexp; if(ae2==0){et[ei2++]='0';}else{while(ae2>0){et[ei2++]='0'+(char)(ae2%10);ae2/=10;}} while(ei2>0) tmp_buf[tlen++]=et[--ei2]; }
            } else {
                /* Use %f style with (fprec - 1 - exponent) fractional digits */
                int fp = fprec - 1 - exponent;
                if (fp < 0) fp = 0;
                unsigned long long ipart = (unsigned long long)val;
                double fr = val - (double)ipart;
                unsigned long long fmul = 1;
                for (int i = 0; i < fp; i++) fmul *= 10;
                unsigned long long fv = (unsigned long long)(fr * (double)fmul + 0.5);
                if (fv >= fmul) { fv = 0; ipart++; }
                tlen = 0;
                if (is_neg) tmp_buf[tlen++] = '-';
                /* Integer part */
                if (ipart == 0) { tmp_buf[tlen++] = '0'; }
                else { char it[24]; int ii=0; unsigned long long v=ipart; while(v>0){it[ii++]='0'+(char)(v%10);v/=10;} while(ii>0) tmp_buf[tlen++]=it[--ii]; }
                /* Fractional part with trailing-zero stripping */
                char fbuf2[24]; int flen2 = fp;
                for (int i=fp-1; i>=0; i--) { fbuf2[i] = '0'+(char)(fv%10); fv /= 10; }
                if (!fl_hash) { while (flen2>0 && fbuf2[flen2-1]=='0') flen2--; }
                if (flen2 > 0) {
                    tmp_buf[tlen++] = '.';
                    for (int i=0; i<flen2; i++) tmp_buf[tlen++] = fbuf2[i];
                }
            }

            int pad = (width > tlen) ? width - tlen : 0;
            if (!fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++] = fl_zero ? '0' : ' ';
            for (int i=0; i<tlen && pos<size-1; i++) str[pos++] = tmp_buf[i];
            if (fl_minus)
                for (int p=0; p<pad && pos<size-1; p++) str[pos++] = ' ';

            format++;
            continue;
        }

        /* ---- unknown ---- */
        default:
            if (pos < size-1) str[pos++] = *format;
            format++;
            continue;
        }
    }

    str[pos] = '\0';
    return (int)pos;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char* str, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, 4096, format, ap);
    va_end(ap);
    return ret;
}

int vasprintf(char **strp, const char* format, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    /* First pass: measure */
    int n = vsnprintf(NULL, 0, format, ap2);
    va_end(ap2);
    if (n < 0) { *strp = NULL; return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *strp = NULL; return -1; }
    va_copy(ap2, ap);
    vsnprintf(buf, (size_t)n + 1, format, ap2);
    va_end(ap2);
    *strp = buf;
    return n;
}

int asprintf(char **strp, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vasprintf(strp, format, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE* stream, const char* format, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        fwrite(buf, 1, len, stream);
    }
    return len;
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stdout, format, ap);
    va_end(ap);
    return ret;
}

/* ------------------------------------------------------------------ */
/* sscanf / vsscanf - minimal implementation                           */
/* Supports: %d, %i, %u, %x, %X, %o, %s, %c, %n, %ld, %li, %lu,    */
/*           %lx, %hd, %hi, %hu, %hx, %hX, %[, width modifiers,     */
/*           and the * (suppress) flag.                                */
/* ------------------------------------------------------------------ */

static int _is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int _hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int vsscanf(const char *str, const char *format, va_list ap)
{
    int matched = 0;
    const char *s = str;

    while (*format) {
        if (_is_space((unsigned char)*format)) {
            format++;
            while (_is_space((unsigned char)*s)) s++;
            continue;
        }

        if (*format != '%') {
            if (*s != *format) break;
            s++; format++;
            continue;
        }

        format++; /* skip '%' */
        if (*format == '%') { if (*s == '%') { s++; format++; continue; } else break; }

        int suppress = 0;
        if (*format == '*') { suppress = 1; format++; }

        /* width */
        int width = 0;
        while (*format >= '0' && *format <= '9')
            width = width * 10 + (*format++ - '0');
        if (width == 0) width = -1; /* unlimited */

        /* length modifier */
        int len_mod = 0; /* 0=int, 'h'=short, 'l'=long */
        if (*format == 'h') { len_mod = 'h'; format++; }
        else if (*format == 'l') { len_mod = 'l'; format++; }

        char spec = *format++;
        if (!spec) break;

        switch (spec) {
        case 'd': case 'i': {
            while (_is_space((unsigned char)*s)) s++;
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            else if (*s == '+') s++;
            if (!(*s >= '0' && *s <= '9')) goto done;
            long val = 0;
            int cnt = 0;
            while (*s >= '0' && *s <= '9' && (width < 0 || cnt < width)) {
                val = val * 10 + (*s - '0'); s++; cnt++;
            }
            if (neg) val = -val;
            if (!suppress) {
                if (len_mod == 'h') *va_arg(ap, short *) = (short)val;
                else if (len_mod == 'l') *va_arg(ap, long *) = val;
                else *va_arg(ap, int *) = (int)val;
                matched++;
            }
            break;
        }
        case 'u': {
            while (_is_space((unsigned char)*s)) s++;
            if (!(*s >= '0' && *s <= '9')) goto done;
            unsigned long val = 0;
            int cnt = 0;
            while (*s >= '0' && *s <= '9' && (width < 0 || cnt < width)) {
                val = val * 10 + (*s - '0'); s++; cnt++;
            }
            if (!suppress) {
                if (len_mod == 'h') *va_arg(ap, unsigned short *) = (unsigned short)val;
                else if (len_mod == 'l') *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'x': case 'X': {
            while (_is_space((unsigned char)*s)) s++;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            if (_hex_val((unsigned char)*s) < 0) goto done;
            unsigned long val = 0;
            int cnt = 0;
            int h;
            while ((h = _hex_val((unsigned char)*s)) >= 0 && (width < 0 || cnt < width)) {
                val = val * 16 + h; s++; cnt++;
            }
            if (!suppress) {
                if (len_mod == 'h') *va_arg(ap, unsigned short *) = (unsigned short)val;
                else if (len_mod == 'l') *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'o': {
            while (_is_space((unsigned char)*s)) s++;
            if (!(*s >= '0' && *s <= '7')) goto done;
            unsigned long val = 0;
            int cnt = 0;
            while (*s >= '0' && *s <= '7' && (width < 0 || cnt < width)) {
                val = val * 8 + (*s - '0'); s++; cnt++;
            }
            if (!suppress) {
                if (len_mod == 'h') *va_arg(ap, unsigned short *) = (unsigned short)val;
                else if (len_mod == 'l') *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 's': {
            while (_is_space((unsigned char)*s)) s++;
            if (!*s) goto done;
            char *dest = suppress ? NULL : va_arg(ap, char *);
            int cnt = 0;
            while (*s && !_is_space((unsigned char)*s) && (width < 0 || cnt < width)) {
                if (dest) dest[cnt] = *s;
                s++; cnt++;
            }
            if (dest) dest[cnt] = '\0';
            if (!suppress) matched++;
            break;
        }
        case 'c': {
            if (!*s) goto done;
            if (width < 0) width = 1;
            char *dest = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < width && *s; i++) {
                if (dest) dest[i] = *s;
                s++;
            }
            if (!suppress) matched++;
            break;
        }
        case 'n': {
            if (!suppress) {
                *va_arg(ap, int *) = (int)(s - str);
            }
            break;
        }
        default:
            goto done;
        }
    }
done:
    return matched;
}

int sscanf(const char *str, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf(str, format, ap);
    va_end(ap);
    return ret;
}

/* fseeko / ftello - large-file aliases. We use 64-bit off_t already so
 * these are simple wrappers around fseek / ftell. */
int fseeko(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream) {
    return (off_t)ftell(stream);
}

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errno));
}
