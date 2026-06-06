/*
 * stat - display file or filesystem status
 *
 * Full implementation per GNU coreutils stat(1) manpage.
 * Supports all format sequences for both file and filesystem mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "stat"
#define VERSION      "1.0"

static int opt_dereference = 0;  /* -L */
static int opt_filesystem = 0;   /* -f */
static int opt_terse = 0;        /* -t */
static const char *opt_format = NULL;  /* -c / --format */
static int opt_printf = 0;       /* --printf (no trailing newline) */

static void usage(int status)
{
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    } else {
        printf("Usage: %s [OPTION]... FILE...\n"
               "Display file or file system status.\n\n"
               "  -L, --dereference     follow links\n"
               "  -f, --file-system     display file system status instead of file status\n"
               "  -c, --format=FORMAT   use the specified FORMAT instead of the default;\n"
               "                        output a newline after each use of FORMAT\n"
               "      --printf=FORMAT   like --format, but interpret backslash escapes,\n"
               "                        and do not output a mandatory trailing newline;\n"
               "                        if you want a newline, include \\n in FORMAT\n"
               "  -t, --terse           print the information in terse form\n"
               "      --help            display this help and exit\n"
               "      --version         output version information and exit\n\n"
               "Valid format sequences for files:\n"
               "  %%a  access rights in octal\n"
               "  %%A  access rights in human readable form\n"
               "  %%b  number of blocks allocated\n"
               "  %%B  the size in bytes of each block reported by %%b\n"
               "  %%d  device number in decimal\n"
               "  %%D  device number in hex\n"
               "  %%f  raw mode in hex\n"
               "  %%F  file type\n"
               "  %%g  group ID of owner\n"
               "  %%G  group name of owner\n"
               "  %%h  number of hard links\n"
               "  %%i  inode number\n"
               "  %%m  mount point\n"
               "  %%n  file name\n"
               "  %%N  quoted file name\n"
               "  %%o  optimal I/O transfer size hint\n"
               "  %%s  total size, in bytes\n"
               "  %%t  major device type in hex\n"
               "  %%T  minor device type in hex\n"
               "  %%u  user ID of owner\n"
               "  %%U  user name of owner\n"
               "  %%w  time of file birth; - if unknown\n"
               "  %%W  time of file birth, seconds since Epoch; 0 if unknown\n"
               "  %%x  time of last access, human-readable\n"
               "  %%X  time of last access, seconds since Epoch\n"
               "  %%y  time of last data modification, human-readable\n"
               "  %%Y  time of last data modification, seconds since Epoch\n"
               "  %%z  time of last status change, human-readable\n"
               "  %%Z  time of last status change, seconds since Epoch\n\n"
               "Valid format sequences for file systems:\n"
               "  %%a  free blocks available to non-superuser\n"
               "  %%b  total data blocks in file system\n"
               "  %%c  total file nodes in file system\n"
               "  %%d  free file nodes in file system\n"
               "  %%f  free blocks in file system\n"
               "  %%i  file system ID in hex\n"
               "  %%l  maximum length of filenames\n"
               "  %%n  file name\n"
               "  %%s  block size\n"
               "  %%S  fundamental block size\n"
               "  %%t  file system type in hex\n"
               "  %%T  file system type in human readable form\n",
               PROGRAM_NAME);
    }
    exit(status);
}

static void version(void)
{
    printf("%s (%s) %s\n", PROGRAM_NAME, "LikeOS coreutils", VERSION);
    exit(EXIT_SUCCESS);
}

/* Format a time_t value as human-readable string */
static void format_time(char *buf, size_t bufsz, uint64_t t)
{
    if (t == 0) {
        snprintf(buf, bufsz, "-");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d:%02d.000000000 +0000",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Convert mode to human-readable permission string like "drwxr-xr-x" */
static void mode_to_str(uint32_t mode, char *buf)
{
    switch (mode & S_IFMT) {
    case S_IFDIR:  buf[0] = 'd'; break;
    case S_IFLNK:  buf[0] = 'l'; break;
    case S_IFCHR:  buf[0] = 'c'; break;
    case S_IFBLK:  buf[0] = 'b'; break;
    case S_IFIFO:  buf[0] = 'p'; break;
    case S_IFSOCK: buf[0] = 's'; break;
    case S_IFREG:  buf[0] = '-'; break;
    default:       buf[0] = '?'; break;
    }
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    if (mode & S_ISUID)
        buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    else
        buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    if (mode & S_ISGID)
        buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    else
        buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    if (mode & S_ISVTX)
        buf[9] = (mode & S_IXOTH) ? 't' : 'T';
    else
        buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

static const char *file_type_str(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFREG:  return (0) ? "regular empty file" : "regular file";
    case S_IFDIR:  return "directory";
    case S_IFCHR:  return "character special file";
    case S_IFBLK:  return "block special file";
    case S_IFIFO:  return "fifo";
    case S_IFLNK:  return "symbolic link";
    case S_IFSOCK: return "socket";
    default:       return "unknown";
    }
}

static const char *fstype_name(unsigned long type)
{
    switch (type) {
    case 0x4d44:  return "vfat";
    case 0xEF53:  return "ext2/ext3/ext4";
    case 0x6969:  return "nfs";
    case 0x9fa0:  return "proc";
    case 0x62646576: return "devfs";
    case 0x01021994: return "tmpfs";
    case 0x858458f6: return "ramfs";
    default:      return "UNKNOWN";
    }
}

static void process_format_char(char spec, const char *filename, struct stat *st)
{
    char timebuf[64];

    switch (spec) {
    case 'a':
        printf("%04o", st->st_mode & 07777);
        break;
    case 'A':
    {
        char perms[12];
        mode_to_str(st->st_mode, perms);
        printf("%s", perms);
        break;
    }
    case 'b':
        printf("%lu", (unsigned long)st->st_blocks);
        break;
    case 'B':
        printf("512");
        break;
    case 'd':
        printf("%lu", (unsigned long)st->st_dev);
        break;
    case 'D':
        printf("%lx", (unsigned long)st->st_dev);
        break;
    case 'f':
        printf("%lx", (unsigned long)st->st_mode);
        break;
    case 'F':
        printf("%s", file_type_str(st->st_mode));
        break;
    case 'g':
        printf("%u", st->st_gid);
        break;
    case 'G':
        if (st->st_gid == 0)
            printf("root");
        else
            printf("%u", st->st_gid);
        break;
    case 'h':
        printf("%u", st->st_nlink);
        break;
    case 'i':
        printf("%lu", (unsigned long)st->st_ino);
        break;
    case 'm':
        printf("/");
        break;
    case 'n':
        printf("%s", filename);
        break;
    case 'N':
        printf("'%s'", filename);
        break;
    case 'o':
        printf("%lu", (unsigned long)st->st_blksize);
        break;
    case 's':
        printf("%lu", (unsigned long)st->st_size);
        break;
    case 't':
        printf("%lx", (unsigned long)(st->st_rdev >> 8));
        break;
    case 'T':
        printf("%lx", (unsigned long)(st->st_rdev & 0xff));
        break;
    case 'u':
        printf("%u", st->st_uid);
        break;
    case 'U':
        if (st->st_uid == 0)
            printf("root");
        else
            printf("%u", st->st_uid);
        break;
    case 'w':
        printf("-");
        break;
    case 'W':
        printf("0");
        break;
    case 'x':
        format_time(timebuf, sizeof(timebuf), st->st_atime);
        printf("%s", timebuf);
        break;
    case 'X':
        printf("%lu", (unsigned long)st->st_atime);
        break;
    case 'y':
        format_time(timebuf, sizeof(timebuf), st->st_mtime);
        printf("%s", timebuf);
        break;
    case 'Y':
        printf("%lu", (unsigned long)st->st_mtime);
        break;
    case 'z':
        format_time(timebuf, sizeof(timebuf), st->st_ctime);
        printf("%s", timebuf);
        break;
    case 'Z':
        printf("%lu", (unsigned long)st->st_ctime);
        break;
    default:
        putchar('%');
        putchar(spec);
        break;
    }
}

static void process_fs_format_char(char spec, const char *filename, struct statfs *vfs)
{
    switch (spec) {
    case 'a':
        printf("%lu", (unsigned long)vfs->f_bavail);
        break;
    case 'b':
        printf("%lu", (unsigned long)vfs->f_blocks);
        break;
    case 'c':
        printf("%lu", (unsigned long)vfs->f_files);
        break;
    case 'd':
        printf("%lu", (unsigned long)vfs->f_ffree);
        break;
    case 'f':
        printf("%lu", (unsigned long)vfs->f_bfree);
        break;
    case 'i':
        printf("%lx", (unsigned long)vfs->f_fsid);
        break;
    case 'l':
        printf("%lu", (unsigned long)vfs->f_namelen);
        break;
    case 'n':
        printf("%s", filename);
        break;
    case 's':
        printf("%lu", (unsigned long)vfs->f_bsize);
        break;
    case 'S':
        printf("%lu", (unsigned long)vfs->f_frsize);
        break;
    case 't':
        printf("%lx", (unsigned long)vfs->f_type);
        break;
    case 'T':
        printf("%s", fstype_name(vfs->f_type));
        break;
    default:
        putchar('%');
        putchar(spec);
        break;
    }
}

static void print_escaped_char(const char **pp)
{
    const char *p = *pp;
    switch (*p) {
    case 'a':  putchar('\a'); break;
    case 'b':  putchar('\b'); break;
    case 'e':  putchar('\033'); break;
    case 'f':  putchar('\f'); break;
    case 'n':  putchar('\n'); break;
    case 'r':  putchar('\r'); break;
    case 't':  putchar('\t'); break;
    case 'v':  putchar('\v'); break;
    case '\\': putchar('\\'); break;
    case '"':  putchar('"'); break;
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
    {
        int val = *p - '0';
        if (p[1] >= '0' && p[1] <= '7') {
            p++; val = val * 8 + (*p - '0');
            if (p[1] >= '0' && p[1] <= '7') {
                p++; val = val * 8 + (*p - '0');
            }
        }
        putchar(val);
        break;
    }
    case 'x':
    {
        p++;
        int val = 0;
        int digits = 0;
        while (digits < 2) {
            int c = *p;
            if (c >= '0' && c <= '9') { val = val * 16 + (c - '0'); }
            else if (c >= 'a' && c <= 'f') { val = val * 16 + (c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { val = val * 16 + (c - 'A' + 10); }
            else break;
            digits++;
            p++;
        }
        if (digits > 0) putchar(val);
        p--;
        break;
    }
    default:
        putchar('\\');
        putchar(*p);
        break;
    }
    *pp = p;
}

static void print_format(const char *fmt, const char *filename,
                         struct stat *st, struct statfs *vfs,
                         int is_fs, int add_newline)
{
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            p++;
            if (!*p) { putchar('\\'); break; }
            print_escaped_char(&p);
        } else if (*p == '%') {
            p++;
            if (!*p) { putchar('%'); break; }
            if (*p == '%') {
                putchar('%');
            } else {
                while (*p == '#' || *p == '0' || *p == '-' ||
                       *p == ' ' || *p == '+')
                    p++;
                while (*p >= '0' && *p <= '9')
                    p++;
                if (*p == '.') {
                    p++;
                    while (*p >= '0' && *p <= '9')
                        p++;
                }
                if (!*p) break;
                if (is_fs)
                    process_fs_format_char(*p, filename, vfs);
                else
                    process_format_char(*p, filename, st);
            }
        } else {
            putchar(*p);
        }
    }
    if (add_newline)
        putchar('\n');
}

static void print_default_file(const char *filename, struct stat *st)
{
    char perms[12];
    mode_to_str(st->st_mode, perms);

    printf("  File: %s\n", filename);
    printf("  Size: %-15lu Blocks: %-10lu IO Block: %-6lu %s\n",
           (unsigned long)st->st_size,
           (unsigned long)st->st_blocks,
           (unsigned long)st->st_blksize,
           file_type_str(st->st_mode));
    printf("Device: %lxh/%lud\tInode: %-10lu  Links: %u\n",
           (unsigned long)st->st_dev,
           (unsigned long)st->st_dev,
           (unsigned long)st->st_ino,
           st->st_nlink);
    printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
           st->st_mode & 07777, perms,
           st->st_uid, st->st_uid == 0 ? "root" : "UNKNOWN",
           st->st_gid, st->st_gid == 0 ? "root" : "UNKNOWN");

    char tbuf[64];
    format_time(tbuf, sizeof(tbuf), st->st_atime);
    printf("Access: %s\n", tbuf);
    format_time(tbuf, sizeof(tbuf), st->st_mtime);
    printf("Modify: %s\n", tbuf);
    format_time(tbuf, sizeof(tbuf), st->st_ctime);
    printf("Change: %s\n", tbuf);
    printf(" Birth: -\n");
}

static void print_default_fs(const char *filename, struct statfs *vfs)
{
    printf("  File: \"%s\"\n", filename);
    printf("    ID: %-16lx Namelen: %-8lu Type: %s\n",
           (unsigned long)vfs->f_fsid,
           (unsigned long)vfs->f_namelen,
           fstype_name(vfs->f_type));
    printf("Block size: %-11lu Fundamental block size: %lu\n",
           (unsigned long)vfs->f_bsize,
           (unsigned long)vfs->f_frsize);
    printf("Blocks: Total: %-11lu Free: %-11lu Available: %lu\n",
           (unsigned long)vfs->f_blocks,
           (unsigned long)vfs->f_bfree,
           (unsigned long)vfs->f_bavail);
    printf("Inodes: Total: %-11lu Free: %lu\n",
           (unsigned long)vfs->f_files,
           (unsigned long)vfs->f_ffree);
}

static void print_terse_file(const char *filename, struct stat *st)
{
    printf("%s %lu %lu %lx %u %u %lx %lu %u %u %lx %lu %lu %lu %lu %lu\n",
           filename,
           (unsigned long)st->st_size,
           (unsigned long)st->st_blocks,
           (unsigned long)st->st_mode,
           st->st_uid, st->st_gid,
           (unsigned long)st->st_dev,
           (unsigned long)st->st_ino,
           st->st_nlink,
           0u,
           (unsigned long)st->st_rdev,
           (unsigned long)st->st_atime,
           (unsigned long)st->st_mtime,
           (unsigned long)st->st_ctime,
           (unsigned long)st->st_blksize,
           (unsigned long)st->st_blocks);
}

static void print_terse_fs(const char *filename, struct statfs *vfs)
{
    printf("%s %lx %lu %lu %lu %lu %lu %lu %lu %lu %lu %s\n",
           filename,
           (unsigned long)vfs->f_fsid,
           (unsigned long)vfs->f_namelen,
           (unsigned long)vfs->f_type,
           (unsigned long)vfs->f_bsize,
           (unsigned long)vfs->f_frsize,
           (unsigned long)vfs->f_blocks,
           (unsigned long)vfs->f_bfree,
           (unsigned long)vfs->f_bavail,
           (unsigned long)vfs->f_files,
           (unsigned long)vfs->f_ffree,
           fstype_name(vfs->f_type));
}

static int do_stat_file(const char *filename)
{
    struct stat st;
    int ret;

    if (opt_dereference)
        ret = stat(filename, &st);
    else
        ret = lstat(filename, &st);

    if (ret != 0) {
        fprintf(stderr, "%s: cannot stat '%s': %s\n",
                PROGRAM_NAME, filename, strerror(errno));
        return 1;
    }

    if (opt_format) {
        print_format(opt_format, filename, &st, NULL, 0, !opt_printf);
    } else if (opt_terse) {
        print_terse_file(filename, &st);
    } else {
        print_default_file(filename, &st);
    }
    return 0;
}

static int do_stat_fs(const char *filename)
{
    struct statfs vfs;
    if (statfs(filename, &vfs) != 0) {
        fprintf(stderr, "%s: cannot read file system information for '%s': %s\n",
                PROGRAM_NAME, filename, strerror(errno));
        return 1;
    }

    if (opt_format) {
        print_format(opt_format, filename, NULL, &vfs, 1, !opt_printf);
    } else if (opt_terse) {
        print_terse_fs(filename, &vfs);
    } else {
        print_default_fs(filename, &vfs);
    }
    return 0;
}

int main(int argc, char **argv)
{
    static struct option long_options[] = {
        {"dereference",  no_argument,       0, 'L'},
        {"file-system",  no_argument,       0, 'f'},
        {"format",       required_argument, 0, 'c'},
        {"printf",       required_argument, 0, 'P'},
        {"terse",        no_argument,       0, 't'},
        {"help",         no_argument,       0, 'H'},
        {"version",      no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int c;
    optind = 1;

    while ((c = getopt_long(argc, argv, "Lfc:t", long_options, NULL)) != -1) {
        switch (c) {
        case 'L':
            opt_dereference = 1;
            break;
        case 'f':
            opt_filesystem = 1;
            break;
        case 'c':
            opt_format = optarg;
            opt_printf = 0;
            break;
        case 'P':
            opt_format = optarg;
            opt_printf = 1;
            break;
        case 't':
            opt_terse = 1;
            break;
        case 'H':
            usage(EXIT_SUCCESS);
            break;
        case 'V':
            version();
            break;
        default:
            usage(EXIT_FAILURE);
            break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: missing operand\n", PROGRAM_NAME);
        usage(EXIT_FAILURE);
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        int r;
        if (opt_filesystem)
            r = do_stat_fs(argv[i]);
        else
            r = do_stat_file(argv[i]);
        if (r != 0)
            ret = 1;
    }

    return ret;
}
