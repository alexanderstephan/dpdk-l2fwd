/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_atomic.h>

static volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating = 1;
/* Latency optimization disabled by default */
static int latency_optimized = 0;

/* Ports set in promiscuous mode off by default. */
static int promiscuous_on;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 256
#define DEFAULT_PKT_BURST 32
static uint16_t batch_size = DEFAULT_PKT_BURST;

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

struct __rte_cache_aligned port_pair_params {
#define NUM_PORTS	2
	uint16_t port[NUM_PORTS];
};

static struct port_pair_params port_pair_params_array[RTE_MAX_ETHPORTS / 2];
static struct port_pair_params *port_pair_params;
static uint16_t nb_port_pair_params;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

struct lcore_rx_queue {
	uint16_t port_id;
	uint16_t queue_id;
};

struct __rte_cache_aligned lcore_queue_conf {
	unsigned n_rx_queue;
	struct lcore_rx_queue rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
};
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS][MAX_TX_QUEUE_PER_PORT];

static struct rte_eth_conf port_conf = {
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

/* Per-port statistics struct */
struct __rte_cache_aligned l2fwd_port_statistics {
	rte_atomic64_t tx;
	rte_atomic64_t rx;
	rte_atomic64_t dropped;
};
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   rte_atomic64_read(&port_statistics[portid].tx),
			   rte_atomic64_read(&port_statistics[portid].rx),
			   rte_atomic64_read(&port_statistics[portid].dropped));

		total_packets_dropped += rte_atomic64_read(&port_statistics[portid].dropped);
		total_packets_tx += rte_atomic64_read(&port_statistics[portid].tx);
		total_packets_rx += rte_atomic64_read(&port_statistics[portid].rx);
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");

	fflush(stdout);
}

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
	struct rte_ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->dst_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->src_addr);
}

// This function is not used in the primary throughput test, but is fixed for completeness.
static void
l2fwd_main_loop_latency(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i, j, nb_rx, nb_tx;
	uint16_t portid, queueid;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	while (!force_quit) {
		for (i = 0; i < qconf->n_rx_queue; i++) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;
			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst, batch_size);

			if (unlikely(nb_rx == 0))
				continue;

			/* Removed atomic counter */

			// MAC swap and prefetch for the burst
			for (j = 0; j < nb_rx; j++) {
				rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
				l2fwd_mac_updating(pkts_burst[j], l2fwd_dst_ports[portid]);
			}

			// Send the entire burst immediately
			nb_tx = rte_eth_tx_burst(l2fwd_dst_ports[portid], queueid, pkts_burst, nb_rx);

			/* Removed atomic counter */

			// Free any packets that were not sent
			if (unlikely(nb_tx < nb_rx)) {
				/* Removed atomic counter */
				for (j = nb_tx; j < nb_rx; j++) {
					rte_pktmbuf_free(pkts_burst[j]);
				}
			}
		}
	}
}

/**
 * Main forwarding loop that processes and sends packets one by one.
 */
static void
l2fwd_main_loop_one_by_one(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m; /* A single packet mbuf */
	unsigned lcore_id;
	unsigned i, j, nb_rx, nb_tx;
	uint16_t portid, queueid, dst_port;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u (1-by-1 mode)\n", lcore_id);

	while (!force_quit) {
		for (i = 0; i < qconf->n_rx_queue; i++) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;

			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst, batch_size);

			if (unlikely(nb_rx == 0))
				continue;

			/* Removed atomic counter */

			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				dst_port = l2fwd_dst_ports[portid];
				l2fwd_mac_updating(m, dst_port);
				nb_tx = rte_eth_tx_burst(dst_port, queueid, &m, 1);

				if (likely(nb_tx == 1)) {
					/* Removed atomic counter */
				} else {
					/* Removed atomic counter */
					rte_pktmbuf_free(m);
				}
			}
		}
	}
}

// Main loop optimized for throughput (buffered send)
static void
l2fwd_main_loop_throughput(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc;
	unsigned i, j, nb_rx, sent;
	uint16_t portid, queueid;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
				BURST_TX_DRAIN_US;
	struct rte_eth_dev_tx_buffer *buffer;

	prev_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	while (!force_quit) {
		cur_tsc = rte_rdtsc();

		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {
			for (i = 0; i < qconf->n_rx_queue; i++) {
				portid = l2fwd_dst_ports[qconf->rx_queue_list[i].port_id];
				queueid = qconf->rx_queue_list[i].queue_id;
				buffer = tx_buffer[portid][queueid];
				sent = rte_eth_tx_buffer_flush(portid, queueid, buffer);
				if (sent) {
					/* Removed atomic counter */
                }
			}
			prev_tsc = cur_tsc;
		}

		for (i = 0; i < qconf->n_rx_queue; i++) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;
			nb_rx = rte_eth_rx_burst(portid, queueid,
						   pkts_burst, batch_size);
			if (unlikely(nb_rx == 0))
				continue;

			/* Removed atomic counter */

			for (j = 0; j < nb_rx; j++) {
				unsigned dst_port = l2fwd_dst_ports[portid];
				buffer = tx_buffer[dst_port][queueid];
				l2fwd_mac_updating(pkts_burst[j], dst_port);
				rte_eth_tx_buffer(dst_port, queueid, buffer, pkts_burst[j]);
			}
		}
	}
}


static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
	if (latency_optimized) {
		l2fwd_main_loop_latency();
		// l2fwd_main_loop_one_by_one();
	} else {
		l2fwd_main_loop_throughput();
	}
	return 0;
}

static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-P] [-b BSZ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -P : Enable promiscuous mode\n"
	       "  -b BSZ: batch size for RX/TX (default is %u, max is %u)\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  --no-mac-updating: Disable MAC addresses updating (enabled by default)\n"
	       "  --latency-opt: Enable latency-optimized forwarding path (disables TX buffering)\n"
	       "  --portmap: Configure forwarding port pair mapping\n"
	       "             Default: alternate port pairs\n\n",
	       prgname, DEFAULT_PKT_BURST, MAX_PKT_BURST);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	return pm;
}

static int
l2fwd_parse_port_pair_config(const char *q_arg)
{
	enum fieldnames { FLD_PORT1 = 0, FLD_PORT2, _NUM_FLD };
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	unsigned int size;
	char s[256];
	char *end;
	int i;

	nb_port_pair_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL) return -1;
		size = p0 - p;
		if (size >= sizeof(s)) return -1;
		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD) return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] >= RTE_MAX_ETHPORTS) return -1;
		}
		if (nb_port_pair_params >= RTE_MAX_ETHPORTS/2) {
			printf("exceeded max number of port pair params: %hu\n", nb_port_pair_params);
			return -1;
		}
		port_pair_params_array[nb_port_pair_params].port[0] = (uint16_t)int_fld[FLD_PORT1];
		port_pair_params_array[nb_port_pair_params].port[1] = (uint16_t)int_fld[FLD_PORT2];
		++nb_port_pair_params;
	}
	port_pair_params = port_pair_params_array;
	return 0;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0')) return -1;
	if (n >= MAX_TIMER_PERIOD) return -1;
	return n;
}

static int
l2fwd_parse_batch_size(const char *bs_arg)
{
	char *end = NULL;
	unsigned long n;
	n = strtoul(bs_arg, &end, 10);
	if ((bs_arg[0] == '\0') || (end == NULL) || (*end != '\0')) return -1;
	if (n == 0) return -1;
	if (n > MAX_PKT_BURST) {
		printf("Batch size must be <= %u\n", MAX_PKT_BURST);
		return -1;
	}
	return n;
}

static const char short_options[] = "p:Pb:T:";

#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_LATENCY_OPT "latency-opt"
#define CMD_LINE_OPT_PORTMAP_CONFIG "portmap"

enum {
	CMD_LINE_OPT_NO_MAC_UPDATING_NUM = 256,
	CMD_LINE_OPT_LATENCY_OPT_NUM,
	CMD_LINE_OPT_PORTMAP_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, 0, CMD_LINE_OPT_NO_MAC_UPDATING_NUM},
	{ CMD_LINE_OPT_LATENCY_OPT, no_argument, 0, CMD_LINE_OPT_LATENCY_OPT_NUM},
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM},
	{NULL, 0, 0, 0}
};

static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;
	port_pair_params = NULL;

	while ((opt = getopt_long(argc, argvopt, short_options, lgopts, &option_index)) != EOF) {
		switch (opt) {
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;
		case 'P':
			promiscuous_on = 1;
			break;
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			timer_period = timer_secs;
			break;
		case 'b':
			ret = l2fwd_parse_batch_size(optarg);
			if (ret < 0) {
				printf("invalid batch size\n");
				l2fwd_usage(prgname);
				return -1;
			}
			batch_size = ret;
			break;
		case CMD_LINE_OPT_PORTMAP_NUM:
			ret = l2fwd_parse_port_pair_config(optarg);
			if (ret) {
				fprintf(stderr, "Invalid config\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;
		case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
			mac_updating = 0;
			break;
		case CMD_LINE_OPT_LATENCY_OPT_NUM:
			latency_optimized = 1;
			break;
		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind - 1;
	optind = 1; /* reset getopt lib */
	return ret;
}

static int
check_port_pair_config(void)
{
	uint32_t port_pair_config_mask = 0;
	uint32_t port_pair_mask = 0;
	uint16_t index, i, portid;

	for (index = 0; index < nb_port_pair_params; index++) {
		port_pair_mask = 0;
		for (i = 0; i < NUM_PORTS; i++) {
			portid = port_pair_params[index].port[i];
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
				printf("port %u is not enabled in port mask\n", portid);
				return -1;
			}
			if (!rte_eth_dev_is_valid_port(portid)) {
				printf("port %u is not present on the board\n", portid);
				return -1;
			}
			port_pair_mask |= 1 << portid;
		}
		if (port_pair_config_mask & port_pair_mask) {
			printf("port %u is used in other port pairs\n", portid);
			return -1;
		}
		port_pair_config_mask |= port_pair_mask;
	}
	l2fwd_enabled_port_mask &= port_pair_config_mask;
	return 0;
}

static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100
#define MAX_CHECK_TIME 90
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit) return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit) return;
			if ((port_mask & (1 << portid)) == 0) continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1) printf("Port %u link get failed: %s\n", portid, rte_strerror(-ret));
				continue;
			}
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid, link_status_text);
				continue;
			}
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		if (print_flag == 1) break;
		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	int ret;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid, last_port;
	unsigned lcore_id;
	unsigned nb_ports_in_mask = 0;
	unsigned int nb_mbufs;
	unsigned int queues_per_port[RTE_MAX_ETHPORTS] = {0};

	ret = rte_eal_init(argc, argv);
	if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	// High core counts require many queues. To avoid exceeding the NIC's total
	// descriptor limit, we reduce the number of descriptors per queue.
	unsigned int nb_lcores = rte_lcore_count();
	if (nb_lcores > 7) { // 7 lcores = 6 workers. More than this requires adjustment.
		printf("INFO: High core count (%u) detected. Reducing descriptors to 512 to conserve resources.\n", nb_lcores);
		nb_rxd = 512;
		nb_txd = 512;
	} else {
		printf("INFO: Low core count (%u). Using default descriptors (%u).\n", nb_lcores, RX_DESC_DEFAULT);
		nb_rxd = RX_DESC_DEFAULT;
		nb_txd = TX_DESC_DEFAULT;
	}

	printf("MAC updating %s, batch size %u\n", mac_updating ? "enabled" : "disabled", batch_size);

	timer_period *= rte_get_timer_hz();
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2) rte_exit(EXIT_FAILURE, "This application requires at least 2 Ethernet ports.\n");
	if (port_pair_params != NULL) {
		if (check_port_pair_config() < 0) rte_exit(EXIT_FAILURE, "Invalid port pair config\n");
	}
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n", (1 << nb_ports) - 1);

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	if (port_pair_params != NULL) {
		uint16_t idx, p;
		for (idx = 0; idx < (nb_port_pair_params << 1); idx++) {
			p = idx & 1;
			portid = port_pair_params[idx >> 1].port[p];
			l2fwd_dst_ports[portid] = port_pair_params[idx >> 1].port[p ^ 1];
		}
	} else {
		RTE_ETH_FOREACH_DEV(portid) {
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) continue;
			if (nb_ports_in_mask % 2) {
				l2fwd_dst_ports[portid] = last_port;
				l2fwd_dst_ports[last_port] = portid;
			} else {
				last_port = portid;
			}
			nb_ports_in_mask++;
		}
		if (nb_ports_in_mask % 2) {
			printf("Notice: odd number of ports in portmask.\n");
			l2fwd_dst_ports[last_port] = last_port;
		}
	}

	// --- Core and queue assignment logic for all scenarios ---
	if (nb_lcores < 1)
		rte_exit(EXIT_FAILURE, "Requires at least 1 core.\n");

	uint16_t enabled_ports[RTE_MAX_ETHPORTS];
	int nb_enabled_ports = 0;
	RTE_ETH_FOREACH_DEV(portid) {
		if (l2fwd_enabled_port_mask & (1 << portid)) {
			if (nb_enabled_ports < RTE_MAX_ETHPORTS) {
				enabled_ports[nb_enabled_ports++] = portid;
			}
		}
	}
	if (nb_enabled_ports != 2)
	   rte_exit(EXIT_FAILURE, "This application currently supports exactly two enabled ports.\n");

	uint16_t port0 = enabled_ports[0];
	uint16_t port1 = enabled_ports[1];

	if (nb_lcores == 1) {
		printf("INFO: Single core mode. Assigning both ports to main lcore.\n");
		lcore_id = rte_get_main_lcore();
		queues_per_port[port0] = 1;
		queues_per_port[port1] = 1;

		qconf = &lcore_queue_conf[lcore_id];
		qconf->rx_queue_list[qconf->n_rx_queue++] = (struct lcore_rx_queue){port0, 0};
		qconf->rx_queue_list[qconf->n_rx_queue++] = (struct lcore_rx_queue){port1, 0};

		printf("Lcore %u (main): RX (port %u, queue 0) and RX (port %u, queue 0)\n", lcore_id, port0, port1);
	} else {
		uint16_t nb_workers = rte_lcore_count() - 1;
		if (nb_workers == 0)
			rte_exit(EXIT_FAILURE, "Requires at least one worker core for multi-core mode.\n");

		printf("INFO: Using manager/worker model with %u worker lcores.\n", nb_workers);

		/* Determine per-port queue count as before (fallback to at least 1) */
		unsigned int queues_port0 = nb_workers / 2;
		unsigned int queues_port1 = nb_workers / 2;

		// If there's an odd number of workers, give the extra one to the first port.
		if (nb_workers % 2 != 0) {
			queues_port0++;
		}

		// Ensure at least one queue per port if there are any workers at all.
		if (nb_workers > 0 && queues_port0 == 0) queues_port0 = 1;
		if (nb_workers > 0 && queues_port1 == 0) queues_port1 = 1;

		queues_per_port[port0] = queues_port0;
		queues_per_port[port1] = queues_port1;

		/* Collect worker lcores into an array so we can assign round-robin */
		unsigned worker_lcores[RTE_MAX_LCORE];
		unsigned worker_count = 0;
		RTE_LCORE_FOREACH_WORKER(lcore_id) {
			if (worker_count < RTE_MAX_LCORE)
				worker_lcores[worker_count++] = lcore_id;
		}
		if (worker_count == 0)
			rte_exit(EXIT_FAILURE, "No worker lcores available unexpectedly.\n");

		/* Round-robin assign queues for port0 then port1 across the worker list.
		 * This allows a single worker to get multiple queues (if worker_count < total_queues).
		 */
		unsigned rr_index = 0;
		for (unsigned q = 0; q < queues_per_port[port0]; ++q) {
			unsigned target_lcore = worker_lcores[rr_index % worker_count];
			qconf = &lcore_queue_conf[target_lcore];
			qconf->rx_queue_list[qconf->n_rx_queue++] = (struct lcore_rx_queue){port0, q};
			printf("Lcore %u: RX (port %u, queue %u)\n", target_lcore, port0, q);
			rr_index++;
		}

		for (unsigned q = 0; q < queues_per_port[port1]; ++q) {
			unsigned target_lcore = worker_lcores[rr_index % worker_count];
			qconf = &lcore_queue_conf[target_lcore];
			qconf->rx_queue_list[qconf->n_rx_queue++] = (struct lcore_rx_queue){port1, q};
			printf("Lcore %u: RX (port %u, queue %u)\n", target_lcore, port1, q);
			rr_index++;
		}
	}


	nb_mbufs = RTE_MAX(nb_ports_in_mask * (nb_rxd + nb_txd + MAX_PKT_BURST) +
			       (nb_lcores * MEMPOOL_CACHE_SIZE) + 32768U, 8192U);
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL) rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;
		uint16_t q;

		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) continue;
		nb_ports_available++;

		printf("Initializing port %u with %u RX queues and %u TX queues...\n", portid, queues_per_port[portid], queues_per_port[portid]);
		fflush(stdout);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0) rte_exit(EXIT_FAILURE, "Error getting device info for port %u: %s\n", portid, strerror(-ret));

		local_port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
		uint64_t desired_rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;
		local_port_conf.rx_adv_conf.rss_conf.rss_hf = dev_info.flow_type_rss_offloads & desired_rss_hf;
		if (local_port_conf.rx_adv_conf.rss_conf.rss_hf == 0 && queues_per_port[portid] > 1) {
		   printf("Warning: Port %u has no supported RSS hash functions. Using non-RSS mode.\n", portid);
		   local_port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
		} else if (local_port_conf.rx_adv_conf.rss_conf.rss_hf != desired_rss_hf && queues_per_port[portid] > 1) {
				printf("INFO: Port %u does not support all desired RSS hash functions. Using supported subset 0x%"PRIx64".\n",
					   portid, local_port_conf.rx_adv_conf.rss_conf.rss_hf);
		}
		local_port_conf.rx_adv_conf.rss_conf.rss_key = NULL;

		ret = rte_eth_dev_configure(portid, queues_per_port[portid], queues_per_port[portid], &local_port_conf);
		if (ret < 0) rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n", ret, portid);
		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
		if (ret < 0) rte_exit(EXIT_FAILURE, "Cannot adjust number of descriptors: err=%d, port=%u\n", ret, portid);
		ret = rte_eth_macaddr_get(portid, &l2fwd_ports_eth_addr[portid]);
		if (ret < 0) rte_exit(EXIT_FAILURE, "Cannot get MAC address: err=%d, port=%u\n", ret, portid);

		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		for (q = 0; q < queues_per_port[portid]; q++) {
			ret = rte_eth_rx_queue_setup(portid, q, nb_rxd, rte_eth_dev_socket_id(portid), &rxq_conf, l2fwd_pktmbuf_pool);
			if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u, queue=%u\n", ret, portid, q);
		}

		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		for (q = 0; q < queues_per_port[portid]; q++) {
			ret = rte_eth_tx_queue_setup(portid, q, nb_txd, rte_eth_dev_socket_id(portid), &txq_conf);
			if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u, queue=%u\n", ret, portid, q);
		}

		if (!latency_optimized) {
			for (q = 0; q < queues_per_port[portid]; q++) {
				tx_buffer[portid][q] = rte_zmalloc_socket("tx_buffer", RTE_ETH_TX_BUFFER_SIZE(batch_size), 0, rte_eth_dev_socket_id(portid));
				if (tx_buffer[portid][q] == NULL)
					rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u queue %u\n", portid, q);
				rte_eth_tx_buffer_init(tx_buffer[portid][q], batch_size);
				/*
				ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid][q], rte_eth_tx_buffer_count_callback, &port_statistics[portid].dropped);
				if (ret < 0)
					rte_exit(EXIT_FAILURE, "Cannot set error callback for tx buffer on port %u queue %u\n", portid, q);
				*/
			}
		}

		ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL, 0);
		if (ret < 0) printf("Port %u, Failed to disable Ptype parsing\n", portid);

		ret = rte_eth_dev_start(portid);
		if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n", ret, portid);

		printf("done: \n");
		if (promiscuous_on) {
			ret = rte_eth_promiscuous_enable(portid);
			if (ret != 0) rte_exit(EXIT_FAILURE, "rte_eth_promiscuous_enable:err=%s, port=%u\n", rte_strerror(-ret), portid);
		}
		printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n", portid, RTE_ETHER_ADDR_BYTES(&l2fwd_ports_eth_addr[portid]));

		/*
		rte_atomic64_init(&port_statistics[portid].tx);
		rte_atomic64_init(&port_statistics[portid].rx);
		rte_atomic64_init(&port_statistics[portid].dropped);
		*/
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE, "All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(l2fwd_enabled_port_mask);

	ret = 0;
	if (nb_lcores == 1) {
		rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MAIN);
	} else {
		rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, SKIP_MAIN);

		// Main lcore stats loop for manager/worker model
		uint64_t main_prev_tsc = 0, main_cur_tsc;
		while(!force_quit) {
			main_cur_tsc = rte_rdtsc();
			if (timer_period > 0 && main_cur_tsc - main_prev_tsc > timer_period) {
				print_stats();
				main_prev_tsc = main_cur_tsc;
			}
			rte_delay_ms(100);
		}
	}

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) continue;
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0) printf("rte_eth_dev_stop: err=%d, port=%d\n", ret, portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	rte_eal_cleanup();
	printf("Bye...\n");

	return ret;
}

Can you remove the atomic counters from this code, please? But keep the struct.
