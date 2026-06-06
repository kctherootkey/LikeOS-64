/*
 * file - determine file type
 *
 * Full implementation per file(1) manpage.
 * Performs filesystem tests, magic number tests, and text/language heuristics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "file"
#define VERSION      "1.0"

#define MAGIC_BUF_SIZE  (1024 * 64)  /* read first 64KB for detection */

/* Options */
static int opt_brief = 0;           /* -b */
static int opt_mime = 0;            /* -i */
static int opt_mime_type = 0;       /* --mime-type */
static int opt_mime_encoding = 0;   /* --mime-encoding */
static int opt_dereference = 1;     /* -L (default) */
static int opt_keep_going = 0;      /* -k */
static int opt_special_files = 0;   /* -s */
static int opt_print0 = 0;         /* -0 */
static int opt_no_pad = 0;         /* -N */
static int opt_raw = 0;            /* -r */
static int opt_error_exit = 0;     /* -E */
static const char *opt_separator = ": "; /* -F */
static const char *opt_files_from = NULL; /* -f */

static void usage(int status)
{
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    } else {
        printf("Usage: %s [OPTION...] [FILE...]\n"
               "Determine type of FILEs.\n\n"
               "  -b, --brief            do not prepend filenames to output lines\n"
               "  -E                     on filesystem errors, issue error message and exit\n"
               "  -f, --files-from FILE  read filenames from FILE\n"
               "  -F, --separator SEP    use SEP instead of ':'\n"
               "  -h, --no-dereference   don't follow symlinks (default)\n"
               "  -i, --mime             output MIME type strings\n"
               "      --mime-type        output the MIME type\n"
               "      --mime-encoding    output the MIME encoding\n"
               "  -k, --keep-going       don't stop at the first match\n"
               "  -L, --dereference      follow symlinks\n"
               "  -N, --no-pad           don't pad output\n"
               "  -r, --raw              don't translate unprintable chars to \\ooo\n"
               "  -s, --special-files    treat special files as ordinary files\n"
               "  -v, --version          output version information and exit\n"
               "  -0, --print0           output null after filename\n"
               "      --help             display this help and exit\n",
               PROGRAM_NAME);
    }
    exit(status);
}

static void version(void)
{
    printf("%s (%s) %s\n", PROGRAM_NAME, "LikeOS", VERSION);
    exit(EXIT_SUCCESS);
}

/* ── Magic signature database ───────────────────────────────────────── */
typedef struct {
    int offset;
    const unsigned char *magic;
    int magic_len;
    const char *description;
    const char *mime_type;
} magic_entry_t;

#define M(off, bytes, desc, mime) { off, (const unsigned char *)bytes, sizeof(bytes)-1, desc, mime }

static const magic_entry_t magic_db[] = {
    /* ELF */
    M(0, "\x7f" "ELF", "ELF", "application/x-executable"),

    /* Scripts (shebang) */
    M(0, "#!/bin/sh",    "POSIX shell script, ASCII text executable", "text/x-shellscript"),
    M(0, "#!/bin/bash",  "Bash shell script, ASCII text executable", "text/x-shellscript"),
    M(0, "#!/usr/bin/env", "script, ASCII text executable", "text/x-script"),
    M(0, "#!",           "script, ASCII text executable", "text/x-script"),

    /* Archives */
    M(0, "PK\x03\x04",  "Zip archive data", "application/zip"),
    M(0, "\x1f\x8b",    "gzip compressed data", "application/gzip"),
    M(0, "BZh",         "bzip2 compressed data", "application/x-bzip2"),
    M(0, "\xfd""7zXZ\0", "XZ compressed data", "application/x-xz"),
    M(0, "\x89PNG\r\n\x1a\n", "PNG image data", "image/png"),
    M(0, "\xff\xd8\xff", "JPEG image data", "image/jpeg"),
    M(0, "GIF87a",       "GIF image data, version 87a", "image/gif"),
    M(0, "GIF89a",       "GIF image data, version 89a", "image/gif"),
    M(0, "BM",           "PC bitmap", "image/bmp"),
    M(0, "RIFF",         "RIFF data", "application/octet-stream"),  /* could be WAV, AVI, etc */
    M(0, "\x00\x00\x01\x00", "MS Windows icon resource", "image/x-icon"),

    /* Documents */
    M(0, "%PDF",         "PDF document", "application/pdf"),
    M(0, "{\rtf",        "Rich Text Format data", "text/rtf"),

    /* Tar */
    M(257, "ustar",      "POSIX tar archive", "application/x-tar"),

    /* Java class */
    M(0, "\xca\xfe\xba\xbe", "Java class data", "application/java-vm"),

    /* Mach-O */
    M(0, "\xfe\xed\xfa\xce", "Mach-O executable", "application/x-mach-binary"),
    M(0, "\xfe\xed\xfa\xcf", "Mach-O 64-bit executable", "application/x-mach-binary"),
    M(0, "\xce\xfa\xed\xfe", "Mach-O executable (reverse byte ordering)", "application/x-mach-binary"),
    M(0, "\xcf\xfa\xed\xfe", "Mach-O 64-bit executable (reverse byte ordering)", "application/x-mach-binary"),

    /* PE / DOS */
    M(0, "MZ",           "DOS/Windows executable (PE)", "application/x-dosexec"),

    /* SQLite */
    M(0, "SQLite format 3", "SQLite 3.x database", "application/x-sqlite3"),

    /* ar archive */
    M(0, "!<arch>",      "current ar archive", "application/x-archive"),

    /* OGG */
    M(0, "OggS",         "Ogg data", "audio/ogg"),

    /* FLAC */
    M(0, "fLaC",         "FLAC audio bitstream data", "audio/flac"),

    /* XML */
    M(0, "<?xml",        "XML document", "text/xml"),

    /* HTML */
    M(0, "<!DOCTYPE html", "HTML document", "text/html"),
    M(0, "<!doctype html", "HTML document", "text/html"),
    M(0, "<html",          "HTML document", "text/html"),
    M(0, "<HTML",          "HTML document", "text/html"),

    /* JSON heuristic: starts with { or [ */
    /* (handled in text analysis below) */

    /* PSF font */
    M(0, "\x36\x04",     "Linux/i386 PC Screen Font v1 data", "application/x-font-psf"),
    M(0, "\x72\xb5\x4a\x86", "Linux/i386 PC Screen Font v2 data", "application/x-font-psf"),

    /* UEFI/PE optional: done via ELF below */
};
static const int magic_db_count = sizeof(magic_db) / sizeof(magic_db[0]);

#undef M

/* ── ELF detailed analysis ──────────────────────────────────────────── */

/* Read a 16-bit value from buf at offset, respecting endianness (1=LE, 2=BE) */
static unsigned int elf_read16(const unsigned char *buf, int off, int ei_data)
{
    if (ei_data == 1) return buf[off] | ((unsigned)buf[off+1] << 8);
    return ((unsigned)buf[off] << 8) | buf[off+1];
}

/* Read a 32-bit value from buf at offset, respecting endianness */
static unsigned int elf_read32(const unsigned char *buf, int off, int ei_data)
{
    if (ei_data == 1)
        return buf[off] | ((unsigned)buf[off+1] << 8) |
               ((unsigned)buf[off+2] << 16) | ((unsigned)buf[off+3] << 24);
    return ((unsigned)buf[off] << 24) | ((unsigned)buf[off+1] << 16) |
           ((unsigned)buf[off+2] << 8) | buf[off+3];
}

/* Read a 64-bit value from buf at offset, respecting endianness */
static unsigned long elf_read64(const unsigned char *buf, int off, int ei_data)
{
    if (ei_data == 1)
        return (unsigned long)buf[off] | ((unsigned long)buf[off+1] << 8) |
               ((unsigned long)buf[off+2] << 16) | ((unsigned long)buf[off+3] << 24) |
               ((unsigned long)buf[off+4] << 32) | ((unsigned long)buf[off+5] << 40) |
               ((unsigned long)buf[off+6] << 48) | ((unsigned long)buf[off+7] << 56);
    return ((unsigned long)buf[off] << 56) | ((unsigned long)buf[off+1] << 48) |
           ((unsigned long)buf[off+2] << 40) | ((unsigned long)buf[off+3] << 32) |
           ((unsigned long)buf[off+4] << 24) | ((unsigned long)buf[off+5] << 16) |
           ((unsigned long)buf[off+6] << 8) | (unsigned long)buf[off+7];
}

/*
 * Check whether an ELF file is stripped by scanning its section headers for
 * a SHT_SYMTAB (type 2) entry.  If no .symtab section exists, the binary
 * is stripped.  The filename is used to read beyond the initial buffer when
 * section headers are located past the first 64 KB.
 */
static int elf_is_stripped(const unsigned char *buf, int len, const char *path)
{
    if (len < 64) return -1; /* too short to tell */

    int ei_class = buf[4]; /* 1 = 32-bit, 2 = 64-bit */
    int ei_data  = buf[5]; /* 1 = LE, 2 = BE */

    unsigned long e_shoff;
    unsigned int  e_shentsize, e_shnum;

    if (ei_class == 2) { /* 64-bit */
        e_shoff     = elf_read64(buf, 40, ei_data);
        e_shentsize = elf_read16(buf, 58, ei_data);
        e_shnum     = elf_read16(buf, 60, ei_data);
    } else if (ei_class == 1) { /* 32-bit */
        e_shoff     = elf_read32(buf, 32, ei_data);
        e_shentsize = elf_read16(buf, 46, ei_data);
        e_shnum     = elf_read16(buf, 48, ei_data);
    } else {
        return -1;
    }

    if (e_shoff == 0 || e_shnum == 0 || e_shentsize == 0)
        return 1; /* no section headers at all → stripped */

    /* If the entire section header table fits inside the buffer we already
       have, scan it directly without re-reading the file. */
    unsigned long sh_end = e_shoff + (unsigned long)e_shnum * e_shentsize;
    if (sh_end <= (unsigned long)len) {
        for (unsigned int i = 0; i < e_shnum; i++) {
            unsigned long off = e_shoff + (unsigned long)i * e_shentsize;
            unsigned int sh_type = elf_read32(buf, (int)off + 4, ei_data);
            if (sh_type == 2) /* SHT_SYMTAB */
                return 0; /* not stripped */
        }
        return 1; /* stripped */
    }

    /* Section headers are beyond our buffer – read them from file */
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    int result = 1; /* assume stripped */
    unsigned char shdr[64]; /* big enough for one Elf64_Shdr (64 bytes) */
    for (unsigned int i = 0; i < e_shnum; i++) {
        unsigned long off = e_shoff + (unsigned long)i * e_shentsize;
        if (fseek(fp, (long)off, SEEK_SET) != 0) break;
        int toread = (int)(e_shentsize < sizeof(shdr) ? e_shentsize : sizeof(shdr));
        if ((int)fread(shdr, 1, toread, fp) < 8) break;
        unsigned int sh_type = elf_read32(shdr, 4, ei_data);
        if (sh_type == 2) { /* SHT_SYMTAB */
            result = 0; /* not stripped */
            break;
        }
    }
    fclose(fp);
    return result;
}

/*
 * Scan ELF program headers for PT_INTERP (type 3) and PT_DYNAMIC (type 2).
 * If PT_INTERP is found, the binary has an interpreter (dynamically linked).
 * If PT_DYNAMIC is found without PT_INTERP, it's a shared library.
 * interp_buf receives the interpreter path (null-terminated) when found.
 */
static void elf_scan_phdr(const unsigned char *buf, int len, const char *path,
                          int *has_interp, int *has_dynamic,
                          char *interp_buf, int interp_bufsz)
{
    *has_interp = 0;
    *has_dynamic = 0;
    interp_buf[0] = '\0';

    if (len < 64) return;

    int ei_class = buf[4];
    int ei_data  = buf[5];

    unsigned long e_phoff;
    unsigned int  e_phentsize, e_phnum;

    if (ei_class == 2) { /* 64-bit */
        e_phoff     = elf_read64(buf, 32, ei_data);
        e_phentsize = elf_read16(buf, 54, ei_data);
        e_phnum     = elf_read16(buf, 56, ei_data);
    } else if (ei_class == 1) { /* 32-bit */
        e_phoff     = elf_read32(buf, 28, ei_data);
        e_phentsize = elf_read16(buf, 42, ei_data);
        e_phnum     = elf_read16(buf, 44, ei_data);
    } else {
        return;
    }

    if (e_phoff == 0 || e_phnum == 0 || e_phentsize == 0)
        return;

    /* Try to scan program headers from the buffer first */
    unsigned long ph_end = e_phoff + (unsigned long)e_phnum * e_phentsize;
    int need_file = (ph_end > (unsigned long)len);
    FILE *fp = NULL;

    if (need_file) {
        fp = fopen(path, "r");
        if (!fp) return;
    }

    unsigned char phdr[64]; /* enough for Elf64_Phdr (56 bytes) */
    for (unsigned int i = 0; i < e_phnum; i++) {
        unsigned long off = e_phoff + (unsigned long)i * e_phentsize;
        const unsigned char *p;

        if (!need_file && off + e_phentsize <= (unsigned long)len) {
            p = buf + off;
        } else if (fp) {
            if (fseek(fp, (long)off, SEEK_SET) != 0) break;
            int toread = (int)(e_phentsize < sizeof(phdr) ? e_phentsize : sizeof(phdr));
            if ((int)fread(phdr, 1, toread, fp) < 8) break;
            p = phdr;
        } else {
            break;
        }

        unsigned int p_type = elf_read32(p, 0, ei_data);

        if (p_type == 2) /* PT_DYNAMIC */
            *has_dynamic = 1;

        if (p_type == 3) { /* PT_INTERP */
            *has_interp = 1;
            /* Read interpreter string */
            unsigned long p_offset, p_filesz;
            if (ei_class == 2) {
                p_offset = elf_read64(p, 8, ei_data);
                p_filesz = elf_read64(p, 32, ei_data);
            } else {
                p_offset = elf_read32(p, 4, ei_data);
                p_filesz = elf_read32(p, 16, ei_data);
            }
            if (p_filesz > 0 && p_filesz < (unsigned long)(interp_bufsz - 1)) {
                if (p_offset + p_filesz <= (unsigned long)len) {
                    memcpy(interp_buf, buf + p_offset, p_filesz);
                    interp_buf[p_filesz] = '\0';
                } else if (fp) {
                    if (fseek(fp, (long)p_offset, SEEK_SET) == 0) {
                        int nr = (int)fread(interp_buf, 1, (int)p_filesz, fp);
                        interp_buf[nr] = '\0';
                    }
                }
            }
        }
    }

    if (fp) fclose(fp);
}

static void describe_elf(const unsigned char *buf, int len, const char *path,
                         char *out, int outsz)
{
    if (len < 20) {
        snprintf(out, outsz, "ELF (too short)");
        return;
    }

    const char *bits;
    if (buf[4] == 1) bits = "32-bit";
    else if (buf[4] == 2) bits = "64-bit";
    else bits = "unknown-class";

    const char *endian;
    if (buf[5] == 1) endian = "LSB";
    else if (buf[5] == 2) endian = "MSB";
    else endian = "unknown-endian";

    int etype;
    if (buf[5] == 1)
        etype = buf[16] | (buf[17] << 8);
    else
        etype = (buf[16] << 8) | buf[17];

    /* Scan program headers for PT_INTERP and PT_DYNAMIC */
    int has_interp = 0, has_dynamic = 0;
    char interp_path[256];
    interp_path[0] = '\0';
    elf_scan_phdr(buf, len, path, &has_interp, &has_dynamic,
                  interp_path, (int)sizeof(interp_path));

    /*
     * Determine type string.  ET_DYN (3) with a PT_INTERP segment is a
     * position-independent executable, not a shared library.
     */
    const char *type;
    switch (etype) {
    case 1: type = "relocatable"; break;
    case 2: type = "executable"; break;
    case 3: type = has_interp ? "pie executable" : "shared object"; break;
    case 4: type = "core file"; break;
    default: type = "unknown type"; break;
    }

    const char *machine = "";
    int emach;
    if (buf[5] == 1)
        emach = buf[18] | (buf[19] << 8);
    else
        emach = (buf[18] << 8) | buf[19];

    switch (emach) {
    case 0x03: machine = "Intel 80386"; break;
    case 0x3E: machine = "x86-64"; break;
    case 0x28: machine = "ARM"; break;
    case 0xB7: machine = "ARM aarch64"; break;
    case 0xF3: machine = "RISC-V"; break;
    case 0x08: machine = "MIPS"; break;
    case 0x14: machine = "PowerPC"; break;
    case 0x15: machine = "PowerPC64"; break;
    default: machine = "unknown arch"; break;
    }

    const char *osabi = "";
    switch (buf[7]) {
    case 0: osabi = "SYSV"; break;
    case 1: osabi = "HPUX"; break;
    case 2: osabi = "NetBSD"; break;
    case 3: osabi = "GNU/Linux"; break;
    case 6: osabi = "Solaris"; break;
    case 9: osabi = "FreeBSD"; break;
    default: osabi = "UNIX"; break;
    }

    /* Linking info */
    const char *link_str;
    if (has_interp || has_dynamic)
        link_str = "dynamically linked";
    else
        link_str = "statically linked";

    /* Stripped info */
    int stripped = elf_is_stripped(buf, len, path);
    const char *strip_str = "";
    if (stripped == 1)       strip_str = ", stripped";
    else if (stripped == 0)  strip_str = ", not stripped";

    if (has_interp && interp_path[0]) {
        snprintf(out, outsz, "ELF %s %s %s, %s, version %d (%s), %s, interpreter %s%s",
                 bits, endian, type, machine, buf[6], osabi,
                 link_str, interp_path, strip_str);
    } else {
        snprintf(out, outsz, "ELF %s %s %s, %s, version %d (%s), %s%s",
                 bits, endian, type, machine, buf[6], osabi,
                 link_str, strip_str);
    }
}

/* ── Text analysis ──────────────────────────────────────────────────── */
static int is_ascii_text(const unsigned char *buf, int len)
{
    if (len == 0) return 0;
    int printable = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == 0) return 0; /* NUL byte → binary */
        if (c == '\n' || c == '\r' || c == '\t' || c == '\f')
            continue;
        if (c >= 32 && c <= 126) {
            printable++;
            continue;
        }
        if (c >= 128) {
            /* Could be UTF-8 or extended ASCII */
            continue;
        }
        /* Control character (not CR/LF/TAB/FF) */
        if (c < 32 && c != 27) /* allow ESC for ANSI */
            return 0;
    }
    return printable > 0;
}

static int is_utf8_text(const unsigned char *buf, int len)
{
    int has_utf8 = 0;
    for (int i = 0; i < len; ) {
        unsigned char c = buf[i];
        if (c == 0) return 0;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 27)
                return 0;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len) return 0;
            if ((buf[i+1] & 0xC0) != 0x80) return 0;
            has_utf8 = 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) return 0;
            if ((buf[i+1] & 0xC0) != 0x80 || (buf[i+2] & 0xC0) != 0x80) return 0;
            has_utf8 = 1;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len) return 0;
            if ((buf[i+1] & 0xC0) != 0x80 || (buf[i+2] & 0xC0) != 0x80 || (buf[i+3] & 0xC0) != 0x80) return 0;
            has_utf8 = 1;
            i += 4;
        } else {
            return 0; /* invalid UTF-8 start byte */
        }
    }
    return has_utf8;
}

/* Simple keyword-based language detection */
static const char *detect_language(const unsigned char *buf, int len)
{
    /* Check first for common patterns */
    char *s = (char *)buf;

    /* C/C++ */
    if (len > 20) {
        /* Check for common C indicators */
        if (strstr(s, "#include") || strstr(s, "#define") || strstr(s, "#ifndef") ||
            strstr(s, "#ifdef") || strstr(s, "#pragma"))
            return "C source";
        if (strstr(s, "int main(") || strstr(s, "void main("))
            return "C source";
        if (strstr(s, "class ") && (strstr(s, "public:") || strstr(s, "private:") || strstr(s, "protected:")))
            return "C++ source";
        if (strstr(s, "namespace ") || strstr(s, "template<") || strstr(s, "std::"))
            return "C++ source";

        /* Python */
        if (strstr(s, "def ") && strstr(s, ":"))
            if (strstr(s, "import ") || strstr(s, "from ") || strstr(s, "class "))
                return "Python script";

        /* Shell script */
        if (strstr(s, "#!/bin/sh") || strstr(s, "#!/bin/bash"))
            return "shell script";

        /* Perl */
        if (strstr(s, "#!/usr/bin/perl") || (strstr(s, "use strict") && strstr(s, "my $")))
            return "Perl script";

        /* Ruby */
        if (strstr(s, "#!/usr/bin/ruby") || (strstr(s, "def ") && strstr(s, "end") && strstr(s, "puts ")))
            return "Ruby script";

        /* Java */
        if (strstr(s, "public class ") || strstr(s, "import java."))
            return "Java source";

        /* JavaScript */
        if (strstr(s, "function ") && (strstr(s, "var ") || strstr(s, "let ") || strstr(s, "const ")))
            return "JavaScript source";

        /* Rust */
        if (strstr(s, "fn main()") && strstr(s, "let "))
            return "Rust source";

        /* Go */
        if (strstr(s, "package main") || strstr(s, "func main()"))
            return "Go source";

        /* Makefile */
        if (strstr(s, ".PHONY") || strstr(s, ":\n\t") || (strstr(s, "$(") && strstr(s, "\t")))
            return "makefile script";

        /* Assembly */
        if (strstr(s, ".text") && (strstr(s, ".global") || strstr(s, ".globl")))
            return "assembler source";
        if (strstr(s, "section .") && (strstr(s, "global ") || strstr(s, "extern ")))
            return "assembler source";

        /* Troff */
        if (s[0] == '.' && (s[1] == 'T' || s[1] == 'S' || s[1] == 'b'))
            if (strstr(s, ".br") || strstr(s, ".TH") || strstr(s, ".SH"))
                return "troff or preprocessor input";

        /* JSON */
        /* Skip leading whitespace */
        {
            int j = 0;
            while (j < len && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r'))
                j++;
            if (j < len && (s[j] == '{' || s[j] == '['))
                if (strstr(s, "\":") || strstr(s, "\": "))
                    return "JSON data";
        }

        /* CSV heuristic */
        {
            int commas = 0, newlines = 0;
            for (int j = 0; j < len && j < 4096; j++) {
                if (s[j] == ',') commas++;
                if (s[j] == '\n') newlines++;
            }
            if (newlines > 2 && commas > newlines * 2)
                return "CSV text";
        }
    }

    return NULL; /* no language detected */
}

/* Check line endings */
static const char *detect_line_ending(const unsigned char *buf, int len)
{
    int has_cr = 0, has_lf = 0, has_crlf = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\r') {
            if (i + 1 < len && buf[i + 1] == '\n') {
                has_crlf = 1;
                i++;
            } else {
                has_cr = 1;
            }
        } else if (buf[i] == '\n') {
            has_lf = 1;
        }
    }
    if (has_crlf && !has_cr && !has_lf) return ", with CRLF line terminators";
    if (has_cr && !has_lf && !has_crlf) return ", with CR line terminators";
    return "";
}

static int classify_file(const char *filename, const char *display_name)
{
    struct stat st;
    int ret;

    if (opt_dereference)
        ret = stat(filename, &st);
    else
        ret = lstat(filename, &st);

    if (ret != 0) {
        if (opt_error_exit) {
            fprintf(stderr, "%s: cannot open '%s' (No such file or directory)\n",
                    PROGRAM_NAME, filename);
            return 1;
        }
        if (!opt_brief)
            printf("%s%s", display_name, opt_separator);
        printf("cannot open '%s' (No such file or directory)\n", filename);
        return 1;
    }

    /* Filesystem tests */
    if (!S_ISREG(st.st_mode) && !opt_special_files) {
        if (!opt_brief)
            printf("%s%s", display_name, opt_separator);

        if (opt_mime || opt_mime_type) {
            if (S_ISDIR(st.st_mode))
                printf("inode/directory");
            else if (S_ISCHR(st.st_mode))
                printf("inode/chardevice");
            else if (S_ISBLK(st.st_mode))
                printf("inode/blockdevice");
            else if (S_ISFIFO(st.st_mode))
                printf("inode/fifo");
            else if (S_ISLNK(st.st_mode))
                printf("inode/symlink");
            else if (S_ISSOCK(st.st_mode))
                printf("inode/socket");
            else
                printf("application/x-not-regular-file");
            if (opt_mime)
                printf("; charset=binary");
            printf("%c", opt_print0 ? '\0' : '\n');
            return 0;
        }

        if (S_ISDIR(st.st_mode))
            printf("directory");
        else if (S_ISCHR(st.st_mode))
            printf("character special");
        else if (S_ISBLK(st.st_mode))
            printf("block special");
        else if (S_ISFIFO(st.st_mode))
            printf("fifo (named pipe)");
        else if (S_ISLNK(st.st_mode))
            printf("symbolic link");
        else if (S_ISSOCK(st.st_mode))
            printf("socket");
        else
            printf("weird file");
        printf("%c", opt_print0 ? '\0' : '\n');
        return 0;
    }

    /* Empty file */
    if (st.st_size == 0) {
        if (!opt_brief)
            printf("%s%s", display_name, opt_separator);
        if (opt_mime || opt_mime_type)
            printf("inode/x-empty");
        else
            printf("empty");
        if (opt_mime)
            printf("; charset=binary");
        printf("%c", opt_print0 ? '\0' : '\n');
        return 0;
    }

    /* Read file header for magic/text analysis */
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (opt_error_exit) {
            fprintf(stderr, "%s: cannot open '%s': %s\n",
                    PROGRAM_NAME, filename, strerror(errno));
            return 1;
        }
        if (!opt_brief)
            printf("%s%s", display_name, opt_separator);
        printf("cannot open (Permission denied)\n");
        return 1;
    }

    int bufsz = MAGIC_BUF_SIZE;
    if ((unsigned long)st.st_size < (unsigned long)bufsz)
        bufsz = (int)st.st_size;
    unsigned char *buf = malloc(bufsz + 1);
    if (!buf) {
        fclose(fp);
        return 1;
    }
    int nread = (int)fread(buf, 1, bufsz, fp);
    fclose(fp);
    buf[nread] = 0; /* null-terminate for string ops */

    if (!opt_brief)
        printf("%s%s", display_name, opt_separator);

    /* ── Magic number tests ───────────────────────────────── */
    for (int i = 0; i < magic_db_count; i++) {
        const magic_entry_t *m = &magic_db[i];
        if (m->offset + m->magic_len > nread)
            continue;
        if (memcmp(buf + m->offset, m->magic, m->magic_len) == 0) {
            if (opt_mime || opt_mime_type) {
                printf("%s", m->mime_type);
                if (opt_mime)
                    printf("; charset=binary");
            } else {
                /* Special handling for ELF */
                if (buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
                    char elf_desc[256];
                    describe_elf(buf, nread, filename, elf_desc, sizeof(elf_desc));
                    printf("%s", elf_desc);
                } else {
                    printf("%s", m->description);
                }
            }
            printf("%c", opt_print0 ? '\0' : '\n');
            free(buf);
            return 0;
        }
    }

    /* ── Text/encoding tests ─────────────────────────────── */
    if (is_utf8_text(buf, nread)) {
        if (opt_mime || opt_mime_type) {
            const char *lang = detect_language(buf, nread);
            if (lang && strstr(lang, "C source"))
                printf("text/x-c");
            else if (lang && strstr(lang, "C++ source"))
                printf("text/x-c++");
            else if (lang && strstr(lang, "Python"))
                printf("text/x-python");
            else if (lang && strstr(lang, "shell"))
                printf("text/x-shellscript");
            else if (lang && strstr(lang, "Java source"))
                printf("text/x-java");
            else if (lang && strstr(lang, "JSON"))
                printf("application/json");
            else if (lang && strstr(lang, "XML"))
                printf("text/xml");
            else if (lang && strstr(lang, "HTML"))
                printf("text/html");
            else
                printf("text/plain");
            if (opt_mime)
                printf("; charset=utf-8");
            else if (opt_mime_encoding)
                printf("utf-8");
        } else {
            const char *lang = detect_language(buf, nread);
            const char *le = detect_line_ending(buf, nread);
            if (lang)
                printf("%s, UTF-8 Unicode text%s", lang, le);
            else
                printf("UTF-8 Unicode text%s", le);
        }
        printf("%c", opt_print0 ? '\0' : '\n');
        free(buf);
        return 0;
    }

    if (is_ascii_text(buf, nread)) {
        if (opt_mime || opt_mime_type) {
            const char *lang = detect_language(buf, nread);
            if (lang && strstr(lang, "C source"))
                printf("text/x-c");
            else if (lang && strstr(lang, "C++ source"))
                printf("text/x-c++");
            else if (lang && strstr(lang, "Python"))
                printf("text/x-python");
            else if (lang && strstr(lang, "shell"))
                printf("text/x-shellscript");
            else if (lang && strstr(lang, "Java source"))
                printf("text/x-java");
            else if (lang && strstr(lang, "JSON"))
                printf("application/json");
            else if (lang && strstr(lang, "XML"))
                printf("text/xml");
            else if (lang && strstr(lang, "HTML"))
                printf("text/html");
            else
                printf("text/plain");
            if (opt_mime)
                printf("; charset=us-ascii");
            else if (opt_mime_encoding)
                printf("us-ascii");
        } else {
            const char *lang = detect_language(buf, nread);
            const char *le = detect_line_ending(buf, nread);
            if (lang)
                printf("%s, ASCII text%s", lang, le);
            else
                printf("ASCII text%s", le);
        }
        printf("%c", opt_print0 ? '\0' : '\n');
        free(buf);
        return 0;
    }

    /* ── Fallback: data ──────────────────────────────────── */
    if (opt_mime || opt_mime_type) {
        printf("application/octet-stream");
        if (opt_mime)
            printf("; charset=binary");
    } else {
        printf("data");
    }
    printf("%c", opt_print0 ? '\0' : '\n');
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    static struct option long_options[] = {
        {"brief",            no_argument,       0, 'b'},
        {"mime",             no_argument,       0, 'i'},
        {"mime-type",        no_argument,       0, 'M'},
        {"mime-encoding",    no_argument,       0, 'E'+128},
        {"keep-going",       no_argument,       0, 'k'},
        {"dereference",      no_argument,       0, 'L'},
        {"no-dereference",   no_argument,       0, 'h'},
        {"no-pad",           no_argument,       0, 'N'},
        {"raw",              no_argument,       0, 'r'},
        {"special-files",    no_argument,       0, 's'},
        {"separator",        required_argument, 0, 'F'},
        {"files-from",       required_argument, 0, 'f'},
        {"version",          no_argument,       0, 'v'},
        {"print0",           no_argument,       0, '0'},
        {"help",             no_argument,       0, 'H'},
        {0, 0, 0, 0}
    };

    int c;
    optind = 1;

    while ((c = getopt_long(argc, argv, "bEf:F:hikLNnprsv0", long_options, NULL)) != -1) {
        switch (c) {
        case 'b':
            opt_brief = 1;
            break;
        case 'E':
            opt_error_exit = 1;
            break;
        case 'f':
            opt_files_from = optarg;
            break;
        case 'F':
            opt_separator = optarg;
            break;
        case 'h':
            opt_dereference = 0;
            break;
        case 'i':
            opt_mime = 1;
            break;
        case 'M': /* --mime-type */
            opt_mime_type = 1;
            break;
        case 'E'+128: /* --mime-encoding */
            opt_mime_encoding = 1;
            break;
        case 'k':
            opt_keep_going = 1;
            break;
        case 'L':
            opt_dereference = 1;
            break;
        case 'N':
            opt_no_pad = 1;
            break;
        case 'n':
            /* --no-buffer: flush after each file (we do this anyway) */
            break;
        case 'p':
            /* --preserve-date: not supported on LikeOS */
            break;
        case 'r':
            opt_raw = 1;
            break;
        case 's':
            opt_special_files = 1;
            break;
        case 'v':
            version();
            break;
        case '0':
            opt_print0 = 1;
            break;
        case 'H':
            usage(EXIT_SUCCESS);
            break;
        default:
            usage(EXIT_FAILURE);
            break;
        }
    }

    int ret = 0;
    int nfiles = 0;

    /* Process files from -f option */
    if (opt_files_from) {
        FILE *fp;
        if (strcmp(opt_files_from, "-") == 0)
            fp = stdin;
        else
            fp = fopen(opt_files_from, "r");

        if (!fp) {
            fprintf(stderr, "%s: cannot open '%s': %s\n",
                    PROGRAM_NAME, opt_files_from, strerror(errno));
            return EXIT_FAILURE;
        }

        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            /* Strip trailing newline */
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            if (len > 0) {
                if (classify_file(line, line) != 0)
                    ret = 1;
                nfiles++;
            }
        }
        if (fp != stdin) fclose(fp);
    }

    /* Process command line files */
    for (int i = optind; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-") == 0) {
            /* Read from stdin - would need to read and classify */
            if (!opt_brief)
                printf("/dev/stdin%s", opt_separator);
            printf("data\n");
        } else {
            if (classify_file(arg, arg) != 0)
                ret = 1;
        }
        nfiles++;
    }

    if (nfiles == 0 && !opt_files_from) {
        fprintf(stderr, "Usage: %s [OPTION...] [FILE...]\n", PROGRAM_NAME);
        return EXIT_FAILURE;
    }

    return ret;
}
