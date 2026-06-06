/*
 * poweroff, reboot, halt - Power off, reboot, or halt the machine
 *
 * These three commands share a single binary. The command name (argv[0])
 * determines the default action. All three accept the same options.
 *
 * Implements the poweroff(8)/reboot(8)/halt(8) command per systemd manpage.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/reboot.h>

enum action {
    ACTION_POWEROFF,
    ACTION_REBOOT,
    ACTION_HALT,
};

/*
 * Extract the basename from a path: "/bin/poweroff" -> "poweroff"
 */
static const char *basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/')
            last = p + 1;
        p++;
    }
    return last;
}

static void print_help(const char *progname)
{
    printf("%s [OPTIONS...]\n\n", progname);
    printf("Power off, reboot, or halt the machine.\n\n");
    printf("Options:\n");
    printf("     --help      Show this help\n");
    printf("     --halt      Halt the machine\n");
    printf("  -p, --poweroff Power-off the machine\n");
    printf("     --reboot    Reboot the machine\n");
    printf("  -f, --force    Force immediate halt/power-off/reboot\n");
    printf("  -w, --wtmp-only Only write wtmp shutdown entry, don't actually\n");
    printf("                   halt/power-off/reboot\n");
    printf("  -d, --no-wtmp  Don't write wtmp shutdown entry\n");
    printf("  -n, --no-sync  Don't sync hard disks/storage media before\n");
    printf("                   halt/power-off/reboot\n");
    printf("     --no-wall   Don't send wall message before halt/power-off/reboot\n");
}

int main(int argc, char *argv[])
{
    const char *progname = basename(argv[0]);
    enum action action;
    int force = 0;      /* -f, --force */
    int wtmp_only = 0;  /* -w, --wtmp-only */
    int no_wtmp = 0;    /* -d, --no-wtmp */
    int no_sync = 0;    /* -n, --no-sync */
    int no_wall = 0;    /* --no-wall */

    /* Determine default action from program name */
    if (strcmp(progname, "reboot") == 0) {
        action = ACTION_REBOOT;
    } else if (strcmp(progname, "halt") == 0) {
        action = ACTION_HALT;
    } else {
        action = ACTION_POWEROFF; /* default for "poweroff" or anything else */
    }

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(progname);
            return 0;
        } else if (strcmp(argv[i], "--halt") == 0) {
            action = ACTION_HALT;
        } else if (strcmp(argv[i], "--poweroff") == 0) {
            /* -p/--poweroff: power-off when halt or poweroff is invoked.
             * Ignored when reboot is invoked. */
            if (strcmp(progname, "reboot") != 0)
                action = ACTION_POWEROFF;
        } else if (strcmp(argv[i], "--reboot") == 0) {
            action = ACTION_REBOOT;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--wtmp-only") == 0) {
            wtmp_only = 1;
        } else if (strcmp(argv[i], "--no-wtmp") == 0) {
            no_wtmp = 1;
        } else if (strcmp(argv[i], "--no-sync") == 0) {
            no_sync = 1;
        } else if (strcmp(argv[i], "--no-wall") == 0) {
            no_wall = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '-') {
            /* Short options */
            for (const char *p = argv[i] + 1; *p; p++) {
                switch (*p) {
                case 'p':
                    /* Ignored when reboot is invoked */
                    if (strcmp(progname, "reboot") != 0)
                        action = ACTION_POWEROFF;
                    break;
                case 'f':
                    force = 1;
                    break;
                case 'w':
                    wtmp_only = 1;
                    break;
                case 'd':
                    no_wtmp = 1;
                    break;
                case 'n':
                    no_sync = 1;
                    break;
                default:
                    fprintf(stderr, "%s: invalid option -- '%c'\n", progname, *p);
                    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
                    return 1;
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[i]);
            return 1;
        } else {
            fprintf(stderr, "%s: too many arguments.\n", progname);
            return 1;
        }
    }

    /* wtmp_only: just pretend to write wtmp and exit */
    if (wtmp_only) {
        /* In a full system, we'd write /var/log/wtmp.
         * LikeOS doesn't have wtmp, so this is a no-op. */
        if (!no_wall) {
            const char *act_str = (action == ACTION_HALT) ? "halt" :
                                  (action == ACTION_REBOOT) ? "reboot" : "poweroff";
            printf("System %s recorded (wtmp-only mode).\n", act_str);
        }
        return 0;
    }

    /* Send wall message */
    if (!no_wall) {
        const char *act_str;
        switch (action) {
        case ACTION_HALT:
            act_str = "halted";
            break;
        case ACTION_REBOOT:
            act_str = "rebooted";
            break;
        case ACTION_POWEROFF:
        default:
            act_str = "powered off";
            break;
        }
        printf("The system is going to be %s NOW!\n", act_str);
    }

    /* Sync filesystems unless --no-sync */
    if (!no_sync) {
        sync();
    }

    (void)force;    /* In our simple OS, all shutdowns are "force" */
    (void)no_wtmp;  /* No wtmp support */

    /* Perform the action */
    switch (action) {
    case ACTION_HALT:
        reboot(RB_HALT_SYSTEM);
        break;
    case ACTION_REBOOT:
        reboot(RB_AUTOBOOT);
        break;
    case ACTION_POWEROFF:
    default:
        reboot(RB_POWER_OFF);
        break;
    }

    /* Should not reach here */
    fprintf(stderr, "%s: reboot syscall failed\n", progname);
    return 1;
}
