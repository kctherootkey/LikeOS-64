#include "../../include/stdio.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"
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
    if (!str || size == 0) return 0;
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
