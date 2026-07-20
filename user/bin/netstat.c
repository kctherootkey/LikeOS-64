/*
 * netstat - print network connections, routing tables, and interface statistics
 *
 * Display active network connections, routing tables, interface statistics,
 * masquerade connections, and multicast memberships. Supports continuous
 * monitoring and per-protocol statistics.
 *
 * Usage:
 *   netstat [-atulneopscvWrigM46FC] [--wide] [--numeric]
 *
 * Options:
 *   -a   all sockets         -t   TCP        -u   UDP
 *   -l   listening only      -n   numeric    -r   routing table
 *   -i   interfaces          -e   extended   -o   timers
 *   -p   PID/program         -s   statistics -c   continuous
 *   -v   verbose             -W   wide       -g   multicast groups
 *   -w   RAW sockets         -4   IPv4       -M   masquerade
 *   -F   FIB                 -C   route cache
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>

static const char *tcp_state_str(int state)
{
	switch (state) {
	case 0:
		return "CLOSED";
	case 1:
		return "LISTEN";
	case 2:
		return "SYN_SENT";
	case 3:
		return "SYN_RECV";
	case 4:
		return "ESTABLISHED";
	case 5:
		return "FIN_WAIT1";
	case 6:
		return "FIN_WAIT2";
	case 7:
		return "CLOSE_WAIT";
	case 8:
		return "CLOSING";
	case 9:
		return "LAST_ACK";
	case 10:
		return "TIME_WAIT";
	default:
		return "UNKNOWN";
	}
}

static const char *ip_to_str(uint32_t ip)
{
	static char bufs[4][16];
	static int idx = 0;
	char *buf = bufs[idx & 3];
	idx++;
	snprintf(buf, 16, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		 (ip >> 8) & 0xFF, ip & 0xFF);
	return buf;
}

static void show_tcp(int listening_only, int show_all, int numeric,
		     int extended, int show_timers, int show_program, int wide)
{
	(void)show_timers;
	(void)show_program;
	net_tcp_info_t conns[64];
	int n = net_getinfo(NET_GET_TCP_CONNS, conns, 64);
	if (n <= 0 && !show_all)
		return;

	int addrw = wide ? 43 : 23;
	printf("Active Internet connections ");
	if (listening_only)
		printf("(only servers)\n");
	else if (show_all)
		printf("(servers and established)\n");
	else
		printf("(w/o servers)\n");

	printf("Proto Recv-Q Send-Q ");
	printf("%-*s %-*s %-11s", addrw, "Local Address", addrw,
	       "Foreign Address", "State");
	if (extended)
		printf("  User       Inode");
	if (show_timers)
		printf("  Timer");
	printf("\n");

	for (int i = 0; i < n; i++) {
		/* Filter based on listening_only and show_all */
		if (listening_only && conns[i].state != 1)
			continue;
		if (!show_all && !listening_only && conns[i].state == 1)
			continue;

		char local[64], remote[64];
		if (numeric || 1) { /* always numeric in LikeOS */
			snprintf(local, sizeof(local), "%s:%u",
				 ip_to_str(conns[i].local_ip),
				 conns[i].local_port);
			snprintf(remote, sizeof(remote), "%s:%u",
				 ip_to_str(conns[i].remote_ip),
				 conns[i].remote_port);
		}

		printf("tcp   %6u %6u %-*s %-*s %s", conns[i].rx_queue,
		       conns[i].tx_queue, addrw, local, addrw, remote,
		       tcp_state_str(conns[i].state));
		if (extended)
			printf("  %-8s %-5s", "root", "0");
		printf("\n");
	}
}

static void show_udp(int show_all, int numeric, int extended, int wide)
{
	(void)show_all;
	(void)numeric;
	net_udp_info_t socks[64];
	int n = net_getinfo(NET_GET_UDP_SOCKS, socks, 64);
	if (n <= 0)
		return;

	int addrw = wide ? 43 : 23;
	printf("Proto Recv-Q Send-Q ");
	printf("%-*s %-*s %-11s", addrw, "Local Address", addrw,
	       "Foreign Address", "State");
	if (extended)
		printf("  User       Inode");
	printf("\n");

	for (int i = 0; i < n; i++) {
		char local[64];
		snprintf(local, sizeof(local), "%s:%u",
			 ip_to_str(socks[i].local_ip), socks[i].local_port);
		printf("udp   %6u %6d %-*s %-*s", socks[i].rx_queue, 0, addrw,
		       local, addrw, "0.0.0.0:*");
		if (extended)
			printf("  %-8s %-5s", "root", "0");
		printf("\n");
	}
}

static void show_route(int numeric, int extended)
{
	net_route_info_t routes[32];
	int n = net_getinfo(NET_GET_ROUTE_TABLE, routes, 32);

	printf("Kernel IP routing table\n");
	if (extended >= 2) {
		/* -ee: extended format with MSS, Window, irtt */
		printf("%-16s %-16s %-16s %-6s %-6s %-3s %-3s %-5s %-6s %-5s %s\n",
		       "Destination", "Gateway", "Genmask", "Flags", "Metric",
		       "Ref", "Use", "MSS", "Window", "irtt", "Iface");
	} else if (extended) {
		/* -e: netstat format */
		printf("%-16s %-16s %-16s %-6s %-6s %-3s %-3s %s\n",
		       "Destination", "Gateway", "Genmask", "Flags", "Metric",
		       "Ref", "Use", "Iface");
	} else {
		printf("%-16s %-16s %-16s %-6s Metric Ref Use Iface\n",
		       "Destination", "Gateway", "Genmask", "Flags");
	}

	for (int i = 0; i < n; i++) {
		char flags[8] = "U";
		int fi = 1;
		if (routes[i].flags & RTF_GATEWAY)
			flags[fi++] = 'G';
		if (routes[i].flags & RTF_HOST)
			flags[fi++] = 'H';
		if (routes[i].flags & RTF_DYNAMIC)
			flags[fi++] = 'D';
		if (routes[i].flags & RTF_MODIFIED)
			flags[fi++] = 'M';
		if (routes[i].flags & RTF_REINSTATE)
			flags[fi++] = 'R';
		flags[fi] = '\0';

		const char *dest_str = (routes[i].dst_net == 0 && !numeric) ?
					       "default" :
					       ip_to_str(routes[i].dst_net);
		const char *gw_str = (routes[i].gateway == 0) ?
					     "*" :
					     ip_to_str(routes[i].gateway);

		if (extended >= 2) {
			printf("%-16s %-16s %-16s %-6s %-6d %-3d %-3d %-5d %-6d %-5d %s\n",
			       dest_str, gw_str, ip_to_str(routes[i].netmask),
			       flags, routes[i].metric, 0, 0, 0, 0, 0,
			       routes[i].dev_name);
		} else {
			printf("%-16s %-16s %-16s %-6s %-6d %-3d %-3d %s\n",
			       dest_str, gw_str, ip_to_str(routes[i].netmask),
			       flags, routes[i].metric, 0, 0,
			       routes[i].dev_name);
		}
	}
}

static void show_interfaces(int extended)
{
	net_iface_info_t ifaces[8];
	int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);

	printf("Kernel Interface table\n");
	if (extended) {
		printf("%-10s %-5s %-7s RX-OK  RX-ERR RX-DRP RX-OVR TX-OK  TX-ERR TX-DRP TX-OVR Flg\n",
		       "Iface", "MTU", "Met");
	} else {
		printf("%-10s %-5s RX-OK  RX-ERR RX-DRP RX-OVR TX-OK  TX-ERR TX-DRP TX-OVR Flg\n",
		       "Iface", "MTU");
	}

	for (int i = 0; i < n; i++) {
		char flg[8];
		int fi = 0;
		if (ifaces[i].flags & IFF_BROADCAST)
			flg[fi++] = 'B';
		if (ifaces[i].flags & IFF_LOOPBACK)
			flg[fi++] = 'L';
		if (ifaces[i].flags & IFF_MULTICAST)
			flg[fi++] = 'M';
		if (ifaces[i].flags & IFF_RUNNING)
			flg[fi++] = 'R';
		if (ifaces[i].flags & IFF_UP)
			flg[fi++] = 'U';
		flg[fi] = '\0';

		if (extended) {
			printf("%-10s %-5u %-7d %-6lu %-6lu %-6lu %-6d %-6lu %-6lu %-6d %-6d %s\n",
			       ifaces[i].name, ifaces[i].mtu, 0,
			       (unsigned long)ifaces[i].rx_packets,
			       (unsigned long)ifaces[i].rx_errors,
			       (unsigned long)ifaces[i].rx_dropped, 0,
			       (unsigned long)ifaces[i].tx_packets,
			       (unsigned long)ifaces[i].tx_errors, 0, 0, flg);
		} else {
			printf("%-10s %-5u %-6lu %-6lu %-6lu %-6d %-6lu %-6lu %-6d %-6d %s\n",
			       ifaces[i].name, ifaces[i].mtu,
			       (unsigned long)ifaces[i].rx_packets,
			       (unsigned long)ifaces[i].rx_errors,
			       (unsigned long)ifaces[i].rx_dropped, 0,
			       (unsigned long)ifaces[i].tx_packets,
			       (unsigned long)ifaces[i].tx_errors, 0, 0, flg);
		}
	}
}

static void show_statistics(int show_tcp_flag, int show_udp_flag)
{
	net_stats_info_t st;
	int have_stats = (net_getinfo(NET_GET_STATS, &st, 1) == 1);
	if (!have_stats)
		memset(&st, 0, sizeof(st));

	int show_ip = (!show_tcp_flag && !show_udp_flag);
	if (show_ip) {
		printf("Ip:\n");
		printf("    %lu total packets received\n",
		       (unsigned long)st.ip_in_receives);
		printf("    %lu with invalid headers\n",
		       (unsigned long)st.ip_in_hdr_errors);
		printf("    %lu incoming packets delivered\n",
		       (unsigned long)st.ip_in_delivers);
		printf("    %lu requests sent out\n",
		       (unsigned long)st.ip_out_requests);
	}
	if (show_tcp_flag || show_ip) {
		net_tcp_info_t conns[64];
		int n = net_getinfo(NET_GET_TCP_CONNS, conns, 64);
		int estab = 0;
		for (int i = 0; i < n; i++)
			if (conns[i].state == 4)
				estab++;
		printf("Tcp:\n");
		printf("    %lu active connection openings\n",
		       (unsigned long)st.tcp_active_opens);
		printf("    %lu passive connection openings\n",
		       (unsigned long)st.tcp_passive_opens);
		printf("    %lu failed connection attempts\n",
		       (unsigned long)st.tcp_attempt_fails);
		printf("    %lu connection resets received\n",
		       (unsigned long)st.tcp_estab_resets);
		printf("    %d connections established\n", estab);
		printf("    %lu segments received\n",
		       (unsigned long)st.tcp_in_segs);
		printf("    %lu segments sent out\n",
		       (unsigned long)st.tcp_out_segs);
		printf("    %lu segments retransmitted\n",
		       (unsigned long)st.tcp_retrans_segs);
		printf("    %lu resets sent\n",
		       (unsigned long)st.tcp_out_rsts);
	}
	if (show_udp_flag || show_ip) {
		printf("Udp:\n");
		printf("    %lu packets received\n",
		       (unsigned long)st.udp_in_datagrams);
		printf("    %lu packets to unknown port received\n",
		       (unsigned long)st.udp_no_ports);
		printf("    %lu packet receive errors\n",
		       (unsigned long)st.udp_rcvbuf_errors);
		printf("    %lu packets sent\n",
		       (unsigned long)st.udp_out_datagrams);
	}
	if (show_ip) {
		/* Extended counters — the previously-silent discard paths.
		 * Only shown when nonzero to keep the report readable. */
		printf("TcpExt:\n");
		struct {
			const char *label;
			unsigned long val;
		} ext[] = {
			{ "PAWS-dropped segments", (unsigned long)st.tcp_paws_drop },
			{ "out-of-window segments dropped",
			  (unsigned long)st.tcp_oow_seq_drop },
			{ "challenge ACKs sent",
			  (unsigned long)st.tcp_challenge_ack },
			{ "reassembly-queue-full drops",
			  (unsigned long)st.tcp_ooo_queue_full },
			{ "oversize out-of-order drops",
			  (unsigned long)st.tcp_ooo_oversize_drop },
			{ "out-of-order FIN refused",
			  (unsigned long)st.tcp_ooo_fin_refused },
			{ "accept-queue-full drops",
			  (unsigned long)st.tcp_acceptq_full },
			{ "listener-gone resets",
			  (unsigned long)st.tcp_listener_gone_rst },
			{ "resets with data loss",
			  (unsigned long)st.tcp_rst_data_loss },
			{ "connection-table-full events",
			  (unsigned long)st.tcp_conn_table_full },
			{ "backlog overflow drops",
			  (unsigned long)st.tcp_backlog_drop },
			{ "zero-window persist probes",
			  (unsigned long)st.tcp_persist_probes },
			{ "SYN cookies sent",
			  (unsigned long)st.tcp_syncookie_sent },
			{ "SYN cookies validated",
			  (unsigned long)st.tcp_syncookie_recv },
			{ "SYN cookie failures",
			  (unsigned long)st.tcp_syncookie_fail },
			{ "TIME-WAIT sockets reused",
			  (unsigned long)st.tcp_tw_reused },
			{ "buffer allocation failures",
			  (unsigned long)st.skb_alloc_fail },
		};
		for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
			if (ext[i].val)
				printf("    %lu %s\n", ext[i].val, ext[i].label);
	}
}

static void show_groups(void)
{
	printf("IPv4/IPv6 Group Memberships\n");
	printf("Interface       RefCnt Group\n");
	printf("---------- ---------- ---------------------\n");
	/* No multicast group support in LikeOS */
}

static void print_help(const char *progname)
{
	printf("Usage: %s [OPTIONS]\n", progname);
	printf("Print network connections, routing tables, interface statistics.\n\n");
	printf("  -r, --route          display routing table\n");
	printf("  -i, --interfaces     display interface table\n");
	printf("  -g, --groups         display multicast group memberships\n");
	printf("  -s, --statistics     display networking statistics\n");
	printf("  -M, --masquerade     display masqueraded connections\n");
	printf("\n  -t, --tcp            display TCP sockets\n");
	printf("  -u, --udp            display UDP sockets\n");
	printf("  -w, --raw            display RAW sockets\n");
	printf("  -x, --unix           display Unix domain sockets\n");
	printf("  -l, --listening      display listening server sockets\n");
	printf("  -a, --all            display all sockets (default: connected)\n");
	printf("  -n, --numeric        don't resolve names\n");
	printf("  --numeric-hosts      don't resolve host names\n");
	printf("  --numeric-ports      don't resolve port names\n");
	printf("  --numeric-users      don't resolve user names\n");
	printf("  -N, --symbolic       resolve hardware names\n");
	printf("  -e, --extend         display other/more information\n");
	printf("  -o, --timers         display timers\n");
	printf("  -p, --program        display PID/Program name for sockets\n");
	printf("  -v, --verbose        be verbose\n");
	printf("  -c, --continuous     continuous listing\n");
	printf("  -W, --wide           don't truncate IP addresses\n");
	printf("  -F                   display Forwarding Information Base (default)\n");
	printf("  -C                   display routing cache instead of FIB\n");
	printf("  -4, --inet           display IPv4 sockets\n");
	printf("  -6, --inet6          display IPv6 sockets\n");
	printf("  -V, --version        display version information\n");
	printf("  -h, --help           display this help\n");
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{ "all", no_argument, 0, 'a' },
		{ "tcp", no_argument, 0, 't' },
		{ "udp", no_argument, 0, 'u' },
		{ "raw", no_argument, 0, 'w' },
		{ "unix", no_argument, 0, 'x' },
		{ "listening", no_argument, 0, 'l' },
		{ "numeric", no_argument, 0, 'n' },
		{ "numeric-hosts", no_argument, 0, 1 },
		{ "numeric-ports", no_argument, 0, 2 },
		{ "numeric-users", no_argument, 0, 3 },
		{ "symbolic", no_argument, 0, 'N' },
		{ "extend", no_argument, 0, 'e' },
		{ "timers", no_argument, 0, 'o' },
		{ "program", no_argument, 0, 'p' },
		{ "verbose", no_argument, 0, 'v' },
		{ "continuous", no_argument, 0, 'c' },
		{ "wide", no_argument, 0, 'W' },
		{ "route", no_argument, 0, 'r' },
		{ "interfaces", no_argument, 0, 'i' },
		{ "groups", no_argument, 0, 'g' },
		{ "masquerade", no_argument, 0, 'M' },
		{ "statistics", no_argument, 0, 's' },
		{ "inet", no_argument, 0, '4' },
		{ "inet6", no_argument, 0, '6' },
		{ "version", no_argument, 0, 'V' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	int show_tcp_flag = 0, show_udp_flag = 0, show_raw_flag = 0,
	    show_unix_flag = 0;
	int show_route_flag = 0, show_ifaces = 0, show_groups_flag = 0;
	int show_stats = 0, show_masq = 0;
	int listening = 0, all = 0, numeric = 0, extended = 0;
	int show_timers = 0, show_program = 0, verbose = 0;
	int continuous = 0, wide = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "atuwxlnNeopvcWrigMs46FCVh",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'a':
			all = 1;
			break;
		case 't':
			show_tcp_flag = 1;
			break;
		case 'u':
			show_udp_flag = 1;
			break;
		case 'w':
			show_raw_flag = 1;
			break;
		case 'x':
			show_unix_flag = 1;
			break;
		case 'l':
			listening = 1;
			break;
		case 'n':
			numeric = 1;
			break;
		case 1:
			numeric = 1;
			break; /* --numeric-hosts */
		case 2:
			numeric = 1;
			break; /* --numeric-ports */
		case 3:
			numeric = 1;
			break; /* --numeric-users */
		case 'N': /* symbolic - accepted */
			break;
		case 'e':
			extended++;
			break;
		case 'o':
			show_timers = 1;
			break;
		case 'p':
			show_program = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'c':
			continuous = 1;
			break;
		case 'W':
			wide = 1;
			break;
		case 'r':
			show_route_flag = 1;
			break;
		case 'i':
			show_ifaces = 1;
			break;
		case 'g':
			show_groups_flag = 1;
			break;
		case 'M':
			show_masq = 1;
			break;
		case 's':
			show_stats = 1;
			break;
		case '4': /* IPv4 - default */
			break;
		case '6':
			fprintf(stderr, "netstat: IPv6 not supported\n");
			break;
		case 'F': /* FIB - default */
			break;
		case 'C': /* route cache - same as FIB for us */
			break;
		case 'V':
			printf("netstat (LikeOS net-tools) 1.0\n");
			return 0;
		case 'h':
			print_help(argv[0]);
			return 0;
		default:
			fprintf(stderr,
				"Try '%s --help' for more information.\n",
				argv[0]);
			return 1;
		}
	}

	(void)verbose;
	(void)show_raw_flag;
	(void)show_unix_flag;
	(void)show_masq;

	/* Default: show TCP if nothing specified */
	if (!show_tcp_flag && !show_udp_flag && !show_route_flag &&
	    !show_ifaces && !show_groups_flag && !show_stats &&
	    !show_raw_flag && !show_unix_flag) {
		show_tcp_flag = 1;
	}

	do {
		if (show_route_flag)
			show_route(numeric, extended);
		if (show_ifaces)
			show_interfaces(extended);
		if (show_groups_flag)
			show_groups();
		if (show_stats) {
			show_statistics(show_tcp_flag, show_udp_flag);
		} else {
			if (show_tcp_flag)
				show_tcp(listening, all, numeric, extended,
					 show_timers, show_program, wide);
			if (show_udp_flag)
				show_udp(all, numeric, extended, wide);
		}

		if (continuous) {
			sleep(1);
			printf("\n");
		}
	} while (continuous);

	return 0;
}
