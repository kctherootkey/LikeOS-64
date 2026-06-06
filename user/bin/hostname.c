/*
 * hostname - show or set the system hostname
 *
 * Display or set the system's hostname, domain name, or FQDN.
 * When called without arguments, prints the current hostname.
 * With an argument, sets the hostname (requires root).
 *
 * Usage:
 *   hostname [-adfFhiInsVvy] [name]
 *
 * Options:
 *   -a   alias name          -d   DNS domain name
 *   -f   FQDN                -F   read hostname from file
 *   -i   IP address(es)      -I   all IP addresses
 *   -s   short hostname      -v   verbose
 *   -V   version             -n   node name
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>

static void print_version(void)
{
    printf("hostname (LikeOS net-tools) 1.0\n");
}

static void print_help(const char *progname)
{
    printf("Usage: %s [OPTION]... [NAME]\n", progname);
    printf("Show or set the system hostname.\n\n");
    printf("  -a, --alias            display the alias name of the host\n");
    printf("  -d, --domain           display the DNS domain name\n");
    printf("  -f, --fqdn, --long     display the long host name (FQDN)\n");
    printf("  -F, --file FILE        read the host name from the specified file\n");
    printf("  -h, --help             display this help and exit\n");
    printf("  -i, --ip-address       display the IP address(es) of the host name\n");
    printf("  -I, --all-ip-addresses display all IP addresses of the host\n");
    printf("  -n, --node             display or set the DECnet node name\n");
    printf("  -s, --short            display the short host name\n");
    printf("  -V, --version          display version information and exit\n");
    printf("  -v, --verbose          be verbose\n");
    printf("  -y, --yp, --nis        display the NIS/YP domain name\n");
}

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"alias",           no_argument,       0, 'a'},
        {"domain",          no_argument,       0, 'd'},
        {"fqdn",            no_argument,       0, 'f'},
        {"long",            no_argument,       0, 'f'},
        {"file",            required_argument, 0, 'F'},
        {"help",            no_argument,       0, 'h'},
        {"ip-address",      no_argument,       0, 'i'},
        {"all-ip-addresses",no_argument,       0, 'I'},
        {"node",            no_argument,       0, 'n'},
        {"short",           no_argument,       0, 's'},
        {"version",         no_argument,       0, 'V'},
        {"verbose",         no_argument,       0, 'v'},
        {"yp",              no_argument,       0, 'y'},
        {"nis",             no_argument,       0, 'y'},
        {0, 0, 0, 0}
    };

    int opt;
    int mode_alias = 0, mode_domain = 0, mode_fqdn = 0;
    int mode_ip = 0, mode_allip = 0, mode_short = 0;
    int mode_node = 0, mode_yp = 0;
    int verbose = 0;
    const char *file = NULL;

    while ((opt = getopt_long(argc, argv, "adfF:hiInsVvy", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a': mode_alias = 1; break;
            case 'd': mode_domain = 1; break;
            case 'f': mode_fqdn = 1; break;
            case 'F': file = optarg; break;
            case 'h': print_help(argv[0]); return 0;
            case 'i': mode_ip = 1; break;
            case 'I': mode_allip = 1; break;
            case 'n': mode_node = 1; break;
            case 's': mode_short = 1; break;
            case 'V': print_version(); return 0;
            case 'v': verbose = 1; break;
            case 'y': mode_yp = 1; break;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                return 1;
        }
    }

    /* Set hostname from file (-F) */
    if (file) {
        FILE *fp = fopen(file, "r");
        if (!fp) {
            fprintf(stderr, "%s: can't open '%s'\n", argv[0], file);
            return 1;
        }
        char buf[256];
        char *newname = NULL;
        while (fgets(buf, sizeof(buf), fp)) {
            /* Strip trailing whitespace/newline */
            size_t len = strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                               buf[len-1] == ' '  || buf[len-1] == '\t'))
                buf[--len] = '\0';
            /* Skip leading whitespace */
            const char *p = buf;
            while (*p == ' ' || *p == '\t') p++;
            /* Skip comment lines and empty lines */
            if (*p == '#' || *p == '\0')
                continue;
            newname = (char *)p;
            break;
        }
        if (!newname || *newname == '\0') {
            fclose(fp);
            fprintf(stderr, "%s: empty host name in file '%s'\n", argv[0], file);
            return 1;
        }
        if (verbose)
            printf("Setting hostname to '%s'\n", newname);
        if (sethostname(newname, strlen(newname)) < 0) {
            fprintf(stderr, "%s: sethostname: Operation not permitted\n", argv[0]);
            fclose(fp);
            return 1;
        }
        fclose(fp);
        return 0;
    }

    /* Set hostname from positional argument (no display flags active) */
    if (optind < argc && !mode_alias && !mode_domain && !mode_fqdn &&
        !mode_ip && !mode_allip && !mode_short && !mode_node && !mode_yp) {
        const char *name = argv[optind];
        if (verbose)
            printf("Setting hostname to '%s'\n", name);
        if (sethostname(name, strlen(name)) < 0) {
            fprintf(stderr, "%s: sethostname: Operation not permitted\n", argv[0]);
            return 1;
        }
        return 0;
    }

    /* Get current hostname */
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        fprintf(stderr, "%s: gethostname failed\n", argv[0]);
        return 1;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    if (verbose)
        printf("gethostname()='%s'\n", hostname);

    /* -n / --node: DECnet node name (LikeOS: just returns hostname) */
    if (mode_node) {
        printf("%s\n", hostname);
        return 0;
    }

    /* -y / --yp / --nis: NIS/YP domain name (not supported in LikeOS) */
    if (mode_yp) {
        printf("(none)\n");
        return 0;
    }

    /* -a / --alias: display alias from /etc/hosts (not supported, prints empty) */
    if (mode_alias) {
        printf("\n");
        return 0;
    }

    /* -s / --short: display short hostname (up to first '.') */
    if (mode_short) {
        char *dot = strchr(hostname, '.');
        if (dot) *dot = '\0';
        printf("%s\n", hostname);
        return 0;
    }

    /* -d / --domain: display DNS domain part */
    if (mode_domain) {
        char *dot = strchr(hostname, '.');
        if (dot)
            printf("%s\n", dot + 1);
        else
            printf("\n");
        return 0;
    }

    /* -f / --fqdn / --long: display FQDN */
    if (mode_fqdn) {
        printf("%s\n", hostname);
        return 0;
    }

    /* -i / --ip-address: display addresses for the hostname */
    if (mode_ip) {
        net_iface_info_t ifaces[8];
        int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);
        int printed = 0;
        for (int i = 0; i < n; i++) {
            if (ifaces[i].ip_addr != 0 && !(ifaces[i].flags & IFF_LOOPBACK)) {
                struct in_addr a;
                a.s_addr = htonl(ifaces[i].ip_addr);
                if (printed) printf(" ");
                printf("%s", inet_ntoa(a));
                printed = 1;
            }
        }
        if (printed) printf("\n");
        return 0;
    }

    /* -I / --all-ip-addresses: display all addresses of the host */
    if (mode_allip) {
        net_iface_info_t ifaces[8];
        int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);
        for (int i = 0; i < n; i++) {
            if (ifaces[i].ip_addr != 0) {
                struct in_addr a;
                a.s_addr = htonl(ifaces[i].ip_addr);
                printf("%s\n", inet_ntoa(a));
            }
        }
        return 0;
    }

    /* Default: just print hostname */
    printf("%s\n", hostname);
    return 0;
}
