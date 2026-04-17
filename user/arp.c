/*
 * arp - manipulate the system ARP cache
 *
 * Display, add, or delete entries in the kernel ARP cache.
 * Maps IP addresses to hardware (MAC) addresses on the local network.
 *
 * Usage:
 *   arp [-vn] [-H type] [-i if] [-a [host] | -d host | -s host hw | -f [file]]
 *
 * Options:
 *   -a   display entries (BSD style)  -e   display entries (Linux style)
 *   -s   set a static entry           -d   delete an entry
 *   -f   read entries from file       -D   use device hw address
 *   -H   hardware type (default ether) -i   interface
 *   -n   numeric addresses            -v   verbose
 *
 * Entry modifiers for -s: temp, pub, trail
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <getopt.h>

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static int parse_mac(const char *mac_str, unsigned char *mac)
{
    unsigned int m[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++)
        mac[i] = (unsigned char)m[i];
    return 0;
}

static void show_entry_bsd(const char *ip, const unsigned char *mac,
                           const char *iface, int flags, int numeric)
{
    (void)numeric;
    printf("? (%s) at ", ip);
    if (flags & ATF_COM)
        printf("%02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        printf("<incomplete>");
    printf(" [ether]");
    if (flags & ATF_PERM) printf(" PERM");
    printf(" on %s\n", iface);
}

static void show_entry_linux(const char *ip, const unsigned char *mac,
                             const char *iface, int flags)
{
    char fstr[16] = "";
    int fi = 0;
    if (flags & ATF_COM)  fstr[fi++] = 'C';
    if (flags & ATF_PERM) fstr[fi++] = 'M';
    fstr[fi] = '\0';

    printf("%-24s ether   %02x:%02x:%02x:%02x:%02x:%02x   %-6s %s\n",
           ip, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           fstr, iface);
}

static void show_arp_table(int bsd_style, int numeric, const char *filter_iface,
                           const char *filter_host)
{
    (void)numeric;
    net_arp_info_t entries[64];
    int n = net_getinfo(NET_GET_ARP_TABLE, entries, 64);

    uint32_t filter_ip = 0;
    if (filter_host) {
        filter_ip = ntohl(inet_addr(filter_host));
        if (filter_ip == 0) {
            /* Try DNS resolve */
            uint32_t resolved;
            if (dns_resolve(filter_host, &resolved) == 0)
                filter_ip = resolved;
            else {
                fprintf(stderr, "arp: %s: Unknown host\n", filter_host);
                return;
            }
        }
    }

    if (!bsd_style) {
        printf("Address                  HWtype  HWaddress           Flags  Iface\n");
    }

    int found = 0;
    for (int i = 0; i < n; i++) {
        /* Kernel ARP entries have no device name; accept all for any iface */
        if (filter_host && entries[i].ip != filter_ip)
            continue;

        found++;
        const char *ipstr = ip_to_str(entries[i].ip);
        const char *iface = filter_iface ? filter_iface : "eth0";

        if (bsd_style) {
            show_entry_bsd(ipstr, entries[i].mac, iface,
                           ATF_COM, numeric);
        } else {
            show_entry_linux(ipstr, entries[i].mac, iface, ATF_COM);
        }
    }

    if (found == 0 && !filter_host)
        printf("(no ARP entries)\n");
    else if (found == 0 && filter_host)
        printf("%s -- no entry\n", filter_host);
}

static int delete_entry(const char *hostname, const char *iface,
                        int pub, int verbose)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct arpreq req;
    memset(&req, 0, sizeof(req));

    struct sockaddr_in *sin = (struct sockaddr_in *)&req.arp_pa;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(hostname);

    if (sin->sin_addr.s_addr == 0xFFFFFFFF) {
        /* Try DNS */
        uint32_t resolved;
        if (dns_resolve(hostname, &resolved) == 0)
            sin->sin_addr.s_addr = htonl(resolved);
        else {
            fprintf(stderr, "arp: %s: Unknown host\n", hostname);
            close(fd);
            return -1;
        }
    }

    if (iface)
        strncpy(req.arp_dev, iface, sizeof(req.arp_dev) - 1);

    if (pub)
        req.arp_flags |= ATF_PERM; /* ATF_PUBL if available */

    if (verbose)
        printf("arp: deleting entry for %s\n", hostname);

    int ret = ioctl(fd, SIOCDARP, &req);
    close(fd);
    return ret;
}

static int set_entry(const char *hostname, const char *hw_addr,
                     const char *iface, int temp, int pub,
                     int use_device, int verbose)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct arpreq req;
    memset(&req, 0, sizeof(req));

    struct sockaddr_in *sin = (struct sockaddr_in *)&req.arp_pa;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(hostname);

    if (sin->sin_addr.s_addr == 0xFFFFFFFF) {
        uint32_t resolved;
        if (dns_resolve(hostname, &resolved) == 0)
            sin->sin_addr.s_addr = htonl(resolved);
        else {
            fprintf(stderr, "arp: %s: Unknown host\n", hostname);
            close(fd);
            return -1;
        }
    }

    if (use_device) {
        /* -D: read MAC from device instead of using hw_addr as MAC */
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, hw_addr, IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
            fprintf(stderr, "arp: cannot get HW address for %s\n", hw_addr);
            close(fd);
            return -1;
        }
        memcpy(req.arp_ha.sa_data, ifr.ifr_hwaddr.sa_data, 6);
    } else {
        unsigned char mac[6];
        if (parse_mac(hw_addr, mac) < 0) {
            fprintf(stderr, "arp: invalid hardware address: %s\n", hw_addr);
            close(fd);
            return -1;
        }
        for (int i = 0; i < 6; i++)
            req.arp_ha.sa_data[i] = (char)mac[i];
    }

    req.arp_ha.sa_family = 1; /* ARPHRD_ETHER */
    req.arp_flags = ATF_COM;
    if (!temp)
        req.arp_flags |= ATF_PERM;
    (void)pub; /* ATF_PUBL if kernel supports it */

    if (iface)
        strncpy(req.arp_dev, iface, sizeof(req.arp_dev) - 1);

    if (verbose)
        printf("arp: setting entry %s -> %s\n", hostname, hw_addr);

    int ret = ioctl(fd, SIOCSARP, &req);
    close(fd);
    return ret;
}

static int process_file(const char *filename, const char *iface,
                        int use_device, int verbose)
{
    FILE *fp;
    if (strcmp(filename, "-") == 0)
        fp = stdin;
    else
        fp = fopen(filename, "r");

    if (!fp) {
        fprintf(stderr, "arp: cannot open %s\n", filename);
        return -1;
    }

    char line[256];
    int errors = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;

        char host[64], mac[32];
        char extra[32] = "";
        int n = sscanf(line, "%63s %31s %31s", host, mac, extra);
        if (n < 2) continue;

        int temp = 0, pub = 0;
        if (n >= 3) {
            if (strcmp(extra, "temp") == 0) temp = 1;
            else if (strcmp(extra, "pub") == 0) pub = 1;
        }

        if (set_entry(host, mac, iface, temp, pub, use_device, verbose) < 0)
            errors++;
    }

    if (fp != stdin) fclose(fp);
    return errors ? -1 : 0;
}

static void print_help(void)
{
    printf("Usage:\n");
    printf("  arp [-vn] [-H type] [-i if] -a [hostname]\n");
    printf("  arp [-v]  [-i if] -d hostname [pub]\n");
    printf("  arp [-v]  [-H type] [-i if] -s hostname hw_addr [temp] [nopub|pub|trail]\n");
    printf("  arp [-vnD] [-H type] [-i if] -f [filename]\n\n");
    printf("Options:\n");
    printf("  -a               display (all) entries in BSD style\n");
    printf("  -e               display in default (Linux) style\n");
    printf("  -d hostname      delete an ARP entry\n");
    printf("  -s hostname hw   set a new ARP entry\n");
    printf("  -D, --use-device use device's hardware address\n");
    printf("  -H type, -t type set hardware type (default: ether)\n");
    printf("  -i if            limit to device\n");
    printf("  -f filename      read entries from file (default /etc/ethers)\n");
    printf("  -n, --numeric    don't resolve hostnames\n");
    printf("  -v, --verbose    be verbose\n");
    printf("  -V, --version    display version\n");
    printf("  -h, --help       display this help\n");
}

int main(int argc, char *argv[])
{
    int numeric = 0, verbose = 0;
    int bsd_style = 0;  /* -a => BSD, -e => Linux (default) */
    int use_device = 0;
    const char *iface = NULL;
    const char *hw_type = "ether";
    int action = 0; /* 0=show, 'd'=delete, 's'=set, 'f'=file */
    const char *action_host = NULL;
    const char *action_hw = NULL;
    const char *action_file = "/etc/ethers";

    static struct option long_options[] = {
        {"all",        no_argument,       0, 'a'},
        {"numeric",    no_argument,       0, 'n'},
        {"verbose",    no_argument,       0, 'v'},
        {"version",    no_argument,       0, 'V'},
        {"help",       no_argument,       0, 'h'},
        {"use-device", no_argument,       0, 'D'},
        {"device",     required_argument, 0, 'i'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "aed:s:f::i:H:t:nDvVh",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'a': bsd_style = 1; break;
            case 'e': bsd_style = 0; break;
            case 'n': numeric = 1; break;
            case 'v': verbose = 1; break;
            case 'D': use_device = 1; break;
            case 'H': case 't':
                hw_type = optarg;
                if (strcmp(hw_type, "ether") != 0) {
                    fprintf(stderr, "arp: hardware type '%s' not supported\n", hw_type);
                    return 1;
                }
                break;
            case 'i': iface = optarg; break;
            case 'd':
                action = 'd';
                action_host = optarg;
                break;
            case 's':
                action = 's';
                action_host = optarg;
                break;
            case 'f':
                action = 'f';
                if (optarg) action_file = optarg;
                break;
            case 'V':
                printf("arp (LikeOS net-tools) 1.0\n");
                return 0;
            case 'h': print_help(); return 0;
            default: print_help(); return 1;
        }
    }

    switch (action) {
        case 'd': {
            /* Check for trailing "pub" */
            int pub = 0;
            if (optind < argc && strcmp(argv[optind], "pub") == 0)
                pub = 1;
            if (delete_entry(action_host, iface, pub, verbose) < 0) {
                fprintf(stderr, "arp: SIOCDARP(%s): No such entry\n", action_host);
                return 1;
            }
            return 0;
        }
        case 's': {
            if (optind >= argc) {
                fprintf(stderr, "arp: need hardware address\n");
                return 1;
            }
            action_hw = argv[optind++];

            /* Parse trailing modifiers: temp, pub, nopub, trail */
            int temp = 0, pub = 0;
            while (optind < argc) {
                if (strcmp(argv[optind], "temp") == 0) temp = 1;
                else if (strcmp(argv[optind], "pub") == 0) pub = 1;
                else if (strcmp(argv[optind], "nopub") == 0) pub = 0;
                else if (strcmp(argv[optind], "trail") == 0) { /* accepted, not used */ }
                optind++;
            }

            if (set_entry(action_host, action_hw, iface, temp, pub,
                          use_device, verbose) < 0) {
                fprintf(stderr, "arp: SIOCSARP: Operation failed\n");
                return 1;
            }
            return 0;
        }
        case 'f': {
            /* -f with optional filename, or remaining arg */
            if (optind < argc)
                action_file = argv[optind];
            if (process_file(action_file, iface, use_device, verbose) < 0)
                return 1;
            return 0;
        }
        default: {
            /* Show table, optionally filtering by hostname */
            const char *filter = NULL;
            if (optind < argc)
                filter = argv[optind];
            show_arp_table(bsd_style, numeric, iface, filter);
            return 0;
        }
    }
}
