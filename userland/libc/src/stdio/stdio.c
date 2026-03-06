#include "../../include/stdio.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"
#include "../../include/unistd.h"
#include "../../include/stdarg.h"
#include "../../include/fcntl.h"

// Standard file streams
static FILE __stdin = { .fd = 0, .buffer = NULL, .buf_size = 0, .buf_pos = 0, .buf_end = 0, .flags = 0, .eof = 0, .error = 0 };
static FILE __stdout = { .fd = 1, .buffer = NULL, .buf_size = 0, .buf_pos = 0, .buf_end = 0, .flags = 0, .eof = 0, .error = 0 };
static FILE __stderr = { .fd = 2, .buffer = NULL, .buf_size = 0, .buf_pos = 0, .buf_end = 0, .flags = 0, .eof = 0, .error = 0 };

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
    fp->buffer = NULL;
    fp->buf_size = 0;
    fp->buf_pos = 0;
    fp->buf_end = 0;
    fp->flags = flags;
    fp->error = 0;
    fp->eof = 0;
    
    return fp;
}

int fclose(FILE* stream) {
    if (!stream) {
        return EOF;
    }
    
    int result = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return result == 0 ? 0 : EOF;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr) {
        return 0;
    }
    
    size_t total = size * nmemb;
    ssize_t bytes = read(stream->fd, ptr, total);
    
    if (bytes < 0) {
        stream->error = 1;
        return 0;
    }
    
    if ((size_t)bytes < total) {
        stream->eof = 1;
    }
    
    return bytes / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr) {
        return 0;
    }
    
    size_t total = size * nmemb;
    ssize_t bytes = write(stream->fd, ptr, total);
    
    if (bytes < 0) {
        stream->error = 1;
        return 0;
    }
    
    return bytes / size;
}

int fgetc(FILE* stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1) {
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
    return ch;
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
    // Currently unbuffered I/O - fread/fwrite go directly to syscalls
    // No buffered data to flush, so this is a no-op
    (void)stream;
    return 0;
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream) {
        return -1;
    }
    
    // Clear EOF flag on seek
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
    
    // Use lseek with offset 0 and SEEK_CUR to get current position
    return (long)lseek(stream->fd, 0, SEEK_CUR);
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
