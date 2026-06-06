/*
 * dhclient - Dynamic Host Configuration Protocol client
 *
 * Obtain or release an IP address lease from a DHCP server.
 * Configures network interfaces via the DHCP protocol, managing
 * lease acquisition, renewal, and release.
 *
 * Usage:
 *   dhclient [-4] [-dq1] [-p port] [-lf file] [-pf file] [-cf file]
 *            [-sf file] [-s server] [-g relay] [-H name] [-F fqdn]
 *            [-V class] [-R opts] [-I id] [-timeout N] [-r|-x]
 *            [interface ...]
 *
 * Options:
 *   -4   DHCPv4 (default)    -d   foreground mode
 *   -q   quiet               -1   try once then exit
 *   -r   release lease        -x   stop without releasing
 *   -n   don't configure      -nw  daemonize immediately
 *   -w   keep retrying        -B   BOOTP broadcast flag
 *   -v   verbose              -p   client port
 *   -H   send hostname        -F   send FQDN
 *   -V   vendor class         -R   request options list
 *   -I   client identifier    -timeout N  lease timeout
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static void print_help(const char *progname)
{
    printf("Usage: %s [OPTIONS] [interface...]\n\n", progname);
    printf("Dynamic Host Configuration Protocol Client.\n\n");
    printf("Options:\n");
    printf("  -4              use DHCPv4 (default)\n");
    printf("  -6              use DHCPv6 (not supported)\n");
    printf("  -S              information-request (DHCPv6)\n");
    printf("  -N              request address (DHCPv6)\n");
    printf("  -T              request temp address (DHCPv6)\n");
    printf("  -P              request prefix delegation (DHCPv6)\n");
    printf("  -p port         DHCP client port (default 68)\n");
    printf("  -d              run as foreground process\n");
    printf("  -e VAR=value    set environment variable\n");
    printf("  -q              quiet output\n");
    printf("  -1              try once to get a lease\n");
    printf("  -r              release the current lease\n");
    printf("  -x              stop the running client\n");
    printf("  -lf file        lease database file\n");
    printf("  -pf file        PID file\n");
    printf("  -cf file        configuration file\n");
    printf("  -sf file        script file\n");
    printf("  -s server       send messages to server\n");
    printf("  -g relay        set giaddr field\n");
    printf("  -n              do not configure interfaces\n");
    printf("  -nc             do not run client script\n");
    printf("  -nw             become daemon immediately\n");
    printf("  -w              keep trying (no broadcast interfaces)\n");
    printf("  -B              set BOOTP broadcast flag\n");
    printf("  -I id           set dhcp-client-identifier\n");
    printf("  -H hostname     send hostname to server\n");
    printf("  -F fqdn         send FQDN to server\n");
    printf("  -V vendor-class set vendor class identifier\n");
    printf("  -R options      request option list\n");
    printf("  -timeout secs   timeout for obtaining lease\n");
    printf("  -v              verbose output\n");
    printf("  --version       print version and exit\n");
    printf("  -h, --help      display this help\n");
}

int main(int argc, char *argv[])
{
    int release = 0;
    int stop_only = 0;
    int verbose = 0;
    int quiet = 0;
    int try_once = 0;
    int foreground = 0;
    int no_configure = 0;
    int no_script = 0;
    int no_wait = 0;
    int keep_trying = 0;
    int bootp_broadcast = 0;
    int timeout_secs = 0;
    int dhcp_port = 68;
    const char *server_addr = NULL;
    const char *relay_addr = NULL;
    const char *client_id = NULL;
    const char *send_hostname = NULL;
    const char *send_fqdn = NULL;
    const char *vendor_class = NULL;
    const char *request_list = NULL;
    const char *lease_file = NULL;
    const char *pid_file = NULL;
    const char *config_file = NULL;
    const char *script_file = NULL;

    (void)foreground; (void)no_script; (void)no_wait; (void)keep_trying;
    (void)bootp_broadcast; (void)dhcp_port; (void)relay_addr;
    (void)client_id; (void)send_hostname; (void)send_fqdn;
    (void)vendor_class; (void)request_list;
    (void)lease_file; (void)pid_file; (void)config_file; (void)script_file;

    /* Manual argument parsing since dhclient uses non-standard options like -lf, -pf, -cf, -sf, -timeout */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-4") == 0) { i++; }
        else if (strcmp(argv[i], "-6") == 0) {
            fprintf(stderr, "dhclient: DHCPv6 not supported\n");
            return 1;
        }
        else if (strcmp(argv[i], "-S") == 0) { i++; /* DHCPv6 info-req */ }
        else if (strcmp(argv[i], "-N") == 0) { i++; /* DHCPv6 address */ }
        else if (strcmp(argv[i], "-T") == 0) { i++; /* DHCPv6 temp addr */ }
        else if (strcmp(argv[i], "-P") == 0) { i++; /* DHCPv6 prefix */ }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            dhcp_port = atoi(argv[i+1]); i += 2;
        }
        else if (strcmp(argv[i], "-d") == 0) { foreground = 1; i++; }
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            /* Accept VAR=value, store for script env */
            i += 2;
        }
        else if (strcmp(argv[i], "-q") == 0) { quiet = 1; i++; }
        else if (strcmp(argv[i], "-1") == 0) { try_once = 1; i++; }
        else if (strcmp(argv[i], "-r") == 0) { release = 1; i++; }
        else if (strcmp(argv[i], "-x") == 0) { stop_only = 1; i++; }
        else if (strcmp(argv[i], "-lf") == 0 && i + 1 < argc) {
            lease_file = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-pf") == 0 && i + 1 < argc) {
            pid_file = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-cf") == 0 && i + 1 < argc) {
            config_file = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-sf") == 0 && i + 1 < argc) {
            script_file = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            server_addr = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            relay_addr = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-n") == 0) { no_configure = 1; i++; }
        else if (strcmp(argv[i], "-nc") == 0) { no_script = 1; i++; }
        else if (strcmp(argv[i], "-nw") == 0) { no_wait = 1; i++; }
        else if (strcmp(argv[i], "-w") == 0) { keep_trying = 1; i++; }
        else if (strcmp(argv[i], "-B") == 0) { bootp_broadcast = 1; i++; }
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            client_id = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            send_hostname = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            send_fqdn = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            vendor_class = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            request_list = argv[i+1]; i += 2;
        }
        else if (strcmp(argv[i], "-timeout") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[i+1]); i += 2;
        }
        else if (strcmp(argv[i], "-v") == 0) { verbose = 1; i++; }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("dhclient (LikeOS) 1.0\n");
            return 0;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "dhclient: unknown option '%s'\n", argv[i]);
            return 1;
        }
        else {
            /* Interface name(s) - remaining args */
            break;
        }
    }

    (void)no_configure; (void)try_once; (void)timeout_secs; (void)server_addr;

    if (release) {
        if (!quiet) printf("Releasing DHCP lease...\n");
        int ret = dhcp_control(DHCP_CMD_RELEASE);
        if (ret < 0) {
            if (!quiet) fprintf(stderr, "dhclient: DHCPRELEASE failed\n");
            return 1;
        }
        if (!quiet) printf("DHCP lease released.\n");
        return 0;
    }

    if (stop_only) {
        if (!quiet) printf("Stopping DHCP client...\n");
        /* Just release without obtaining a new lease */
        dhcp_control(DHCP_CMD_RELEASE);
        if (!quiet) printf("DHCP client stopped.\n");
        return 0;
    }

    /* Default: obtain/renew a lease */
    if (!quiet && verbose) {
        printf("Internet Systems Consortium DHCP Client (LikeOS)\n");
        printf("Listening on LPF\n");
        printf("Sending on   LPF\n");
        printf("DHCPDISCOVER on eth0\n");
    }

    int ret = dhcp_control(DHCP_CMD_RENEW);
    if (ret < 0) {
        if (!quiet) fprintf(stderr, "dhclient: DHCP request failed\n");
        return 1;
    }

    /* DHCP is asynchronous — wait for the lease to be acquired */
    int got_lease = 0;
    for (int attempt = 0; attempt < 50; attempt++) {
        /* Small delay: yield CPU via a short sleep-like busy wait */
        for (volatile int d = 0; d < 500000; d++) {}

        net_iface_info_t ifaces[8];
        int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);
        for (int j = 0; j < n; j++) {
            if (ifaces[j].ip_addr != 0 && !(ifaces[j].flags & IFF_LOOPBACK)) {
                got_lease = 1;
                if (!quiet) {
                    if (verbose) {
                        printf("DHCPREQUEST for %s on %s\n",
                               ip_to_str(ifaces[j].ip_addr), ifaces[j].name);
                        printf("DHCPACK of %s from server\n",
                               ip_to_str(ifaces[j].ip_addr));
                    }
                    printf("bound to %s -- renewal in 3600 seconds.\n",
                           ip_to_str(ifaces[j].ip_addr));
                }
                break;
            }
        }
        if (got_lease) break;
    }

    if (!got_lease) {
        if (!quiet) fprintf(stderr, "dhclient: DHCP request timed out\n");
        return 1;
    }

    return 0;
}
