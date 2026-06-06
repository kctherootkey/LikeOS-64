/*
 * uname - print system information
 *
 * Usage: uname [OPTION]...
 * Print certain system information. With no OPTION, same as -s.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/utsname.h>

#define VERSION_STRING "uname (LikeOS coreutils) 0.2"

static void print_help(const char *progname)
{
    printf("Usage: %s [OPTION]...\n", progname);
    printf("Print certain system information.  With no OPTION, same as -s.\n\n");
    printf("  -a, --all                print all information, in the following order,\n");
    printf("                             except omit -p and -i if unknown:\n");
    printf("  -s, --kernel-name        print the kernel name\n");
    printf("  -n, --nodename           print the network node hostname\n");
    printf("  -r, --kernel-release     print the kernel release\n");
    printf("  -v, --kernel-version     print the kernel version\n");
    printf("  -m, --machine            print the machine hardware name\n");
    printf("  -p, --processor          print the processor type (non-portable)\n");
    printf("  -i, --hardware-platform  print the hardware platform (non-portable)\n");
    printf("  -o, --operating-system   print the operating system\n");
    printf("      --help     display this help and exit\n");
    printf("      --version  output version information and exit\n");
}

int main(int argc, char *argv[])
{
    struct utsname uts;
    int opt;
    int show_sysname = 0;
    int show_nodename = 0;
    int show_release = 0;
    int show_version = 0;
    int show_machine = 0;
    int show_processor = 0;
    int show_hardware = 0;
    int show_os = 0;
    int any_flag = 0;

    /* Handle long options manually since we only have short getopt */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--all") == 0) {
            argv[i] = "-a";
        } else if (strcmp(argv[i], "--kernel-name") == 0) {
            argv[i] = "-s";
        } else if (strcmp(argv[i], "--nodename") == 0) {
            argv[i] = "-n";
        } else if (strcmp(argv[i], "--kernel-release") == 0) {
            argv[i] = "-r";
        } else if (strcmp(argv[i], "--kernel-version") == 0) {
            argv[i] = "-v";
        } else if (strcmp(argv[i], "--machine") == 0) {
            argv[i] = "-m";
        } else if (strcmp(argv[i], "--processor") == 0) {
            argv[i] = "-p";
        } else if (strcmp(argv[i], "--hardware-platform") == 0) {
            argv[i] = "-i";
        } else if (strcmp(argv[i], "--operating-system") == 0) {
            argv[i] = "-o";
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], argv[i]);
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    while ((opt = getopt(argc, argv, "asnrvmpio")) != -1) {
        switch (opt) {
        case 'a':
            show_sysname = 1;
            show_nodename = 1;
            show_release = 1;
            show_version = 1;
            show_machine = 1;
            show_processor = 1;
            show_hardware = 1;
            show_os = 1;
            any_flag = 1;
            break;
        case 's':
            show_sysname = 1;
            any_flag = 1;
            break;
        case 'n':
            show_nodename = 1;
            any_flag = 1;
            break;
        case 'r':
            show_release = 1;
            any_flag = 1;
            break;
        case 'v':
            show_version = 1;
            any_flag = 1;
            break;
        case 'm':
            show_machine = 1;
            any_flag = 1;
            break;
        case 'p':
            show_processor = 1;
            any_flag = 1;
            break;
        case 'i':
            show_hardware = 1;
            any_flag = 1;
            break;
        case 'o':
            show_os = 1;
            any_flag = 1;
            break;
        default:
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    /* Default: print kernel name */
    if (!any_flag)
        show_sysname = 1;

    if (uname(&uts) != 0) {
        fprintf(stderr, "%s: cannot get system information\n", argv[0]);
        return 1;
    }

    /*
     * Processor type and hardware platform are derived from machine.
     * Operating system name is derived from sysname.
     */
    const char *processor = uts.machine;
    const char *hardware = uts.machine;
    const char *os_name = uts.sysname;

    int need_space = 0;

    if (show_sysname) {
        printf("%s", uts.sysname);
        need_space = 1;
    }
    if (show_nodename) {
        if (need_space) printf(" ");
        printf("%s", uts.nodename);
        need_space = 1;
    }
    if (show_release) {
        if (need_space) printf(" ");
        printf("%s", uts.release);
        need_space = 1;
    }
    if (show_version) {
        if (need_space) printf(" ");
        printf("%s", uts.version);
        need_space = 1;
    }
    if (show_machine) {
        if (need_space) printf(" ");
        printf("%s", uts.machine);
        need_space = 1;
    }
    if (show_processor) {
        if (need_space) printf(" ");
        printf("%s", processor);
        need_space = 1;
    }
    if (show_hardware) {
        if (need_space) printf(" ");
        printf("%s", hardware);
        need_space = 1;
    }
    if (show_os) {
        if (need_space) printf(" ");
        printf("%s", os_name);
        need_space = 1;
    }

    printf("\n");
    return 0;
}
