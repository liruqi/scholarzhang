#include "connmanager.h"
#include "dstmaintain.h"
#include "fingerprint.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pcap.h>
#include <libnet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#define _CONNMANAGER_

static libnet_t *l = NULL;
static libnet_ptag_t tcp = 0, ip = 0;
static char errbuf[LIBNET_ERRBUF_SIZE];
static pcap_t *pd = NULL;
static char pcap_errbuf[PCAP_ERRBUF_SIZE];
static u_int16_t linktype, linkoffset;

#define STATUS_CHECK 0x08
#define STATUS_TYPE1 0x10
#define STATUS_TYPE2 0x20
#define STATUS_ERROR 0x40
#define STATUS_MASK 0x07

struct conncontent {
	char status;
	/*
	   0: request a (da, dp)
	   1: s
	   2: a
	   3: pa;a---fpa;a
	   4: s;r
	   5: expire
	   STATUS_CHECK: check if (da, dp) works here.
	*/
	char hit; // HK_TYPE
	char type; // we are checking keyword of this HK_TYPE
	char times; // times left for the packet to send
	u_int16_t sp;
	u_int32_t seq, ack, new_seq;
	struct dstinfo *dst;
	char *content;
	int length, next;
//	int hit_at;
	char *result;
	gk_callback_f callback;
	void *arg;
	struct hash_t **hash;
};

#define ST_TO_HK_TYPE(status) ((status >> 4) & 3)
#define HK_TO_ST_TYPE(result) (result << 4)
#define FP_TO_HK_TYPE(fingerprint) (fingerprint >> 8)

static struct connlist_t {
	struct conncontent *head;
	struct conncontent *list;
} connlist = {NULL, NULL};
// connlist has capacity = event_capa

/* event queue maintanence */
#include "heap.h"
static struct heap_t *event = NULL;
static int event_count, event_capa;
static struct dstlist *dest = NULL;

/* hash table maintanence */
#include "dst_hash.c"
// static struct hash_t **hash = NULL;
/* defined above, hash has capacity = event_capa * 3 */

static char times = 8;
static int time_interval = 30;
static int expire_timeout = 200;
static int tcp_mss = 1300;
static double kps = 0;
static int pps = 0;

static u_int32_t sa;

//static char running = -1;

static const char const youtube[] = "GET http://www.youtube.com HTTP/1.1\r\n\r\n";
static const int  youtube_len = sizeof(youtube) - 1;

/* (da, dp) recycle */
#include "return_dst.c"

/* connlist maintain */
static inline struct conncontent *new_conn() {
	struct conncontent *conn = connlist.head;
	if (conn)
		connlist.head = *(struct conncontent **)connlist.head;
	return conn;
}

static inline void del_conn(struct conncontent *conn) {
	*(struct conncontent **)conn = connlist.head;
	connlist.head = conn;
}

static inline void empty_connlist() {
	register struct conncontent *last;
	connlist.head = connlist.list + event_capa - 1;
	*(struct conncontent **)(connlist.head) = NULL;
	while (connlist.head > connlist.list) {
		last = connlist.head;
		*(struct conncontent **)(connlist.head = last - 1) = last;
	}
}

/* send and receive */

static inline void clear_queue() {
	struct conncontent *conn;

	while (event_count) {
		conn = event->data;
		if (conn->dst)
			return_dst_delete_hash(dest, conn->dst, conn->type, HK_TO_ST_TYPE(conn->hit), conn->hash);
		heap_delmin(event, &event_count);
		conn->callback(conn->content, HK_TO_ST_TYPE(conn->type) | RESULT_ERROR, conn->arg);
		del_conn(conn);
	}
}

static inline char sendpacket(char times, long *time, u_int32_t sa, u_int32_t da, u_int16_t sp, u_int16_t dp, u_int8_t flag, u_int32_t seq, u_int32_t ack, u_int8_t *payload, u_int16_t len) {
	static double next = 0;
	static int i;

	tcp = libnet_build_tcp(sp, dp, seq, ack, flag, 16384, 0, 0, len + LIBNET_TCP_H, payload, len, l, tcp);
	len += LIBNET_IPV4_H + LIBNET_TCP_H;
	ip = libnet_build_ipv4(len, 0, 0, IP_DF, 128, IPPROTO_TCP, 0, sa, da, NULL, 0, l, ip);
	*time = gettime();

	for (i = 0; i < times; ++i) {
		if (next > *time + 1) {
			*time = next;
			break;
		}
		if (-1 == libnet_write(l))
			fputs(errbuf, stderr);
		else {
			if (pps) next += 1000000 / pps;
			if (kps >= 1) next += 1000 * len / kps;
		}
	}
	return i;
}

inline long gk_cm_conn_next_time() {
	return event->time;
}

long gk_cm_conn_step() {
	static struct conncontent *conn;
	static char status;
	static char type, result, hit;
	static u_int16_t piece;
	static long inittime, time;

	if (event_count == 0)
		return -1;
	inittime = gettime();
	if (inittime < event->time)
		return event->time;

	time = inittime;
	do {
		conn = event->data;
		status = conn->status & STATUS_MASK;
		type = conn->type;

		if (status == 5) {
			hit = conn->hit;
			result = hit & type;
			if (conn->status & STATUS_CHECK) {
				/* connection with this dst finished,
				   if it does not work with this GFW
				   type, we will set status = 0
				   again */
				if (result) {
					// GFW's working, so this is not a keyword
					if (conn->result) *conn->result = 0;
					conn->callback(conn->content, HK_TO_ST_TYPE(type), conn->arg);
					heap_delmin(event, &event_count);
				}
				else {
					/* else we know nothing about if it
					   contains any keyword. */
					fprintf(stderr, "[information]: GFW type%d is not working on (local:%d, %s:%d).\n", type, conn->sp, inet_ntoa(*(struct in_addr *)&conn->dst->da), conn->dst->dport);
					conn->status = 0;
					conn->hit = 0;
					status = 0;
				}
				return_dst_delete_hash(dest, conn->dst, type, STATUS_CHECK | HK_TO_ST_TYPE(hit), conn->hash);
				if (conn->status & STATUS_ERROR) {
					conn->status = 0;
					conn->hit = 0;
					status = 0;
				}
				else if (result) {
					del_conn(conn);
					goto round;
				}
			}
			else if (result == 0) {
				/* GFW no response to this type check
				   if GFW is working on this (da,
				   dp) */
				status = 1;
				conn->status = STATUS_CHECK | 1;
				conn->times = times;
			}
			else {
				// Hit or in secondary resetting status
				if (conn->status & STATUS_ERROR) {
					conn->status = 0;
					conn->hit = 0;
					status = 0;
				}
				else {
					//fprintf(stderr, "hit: %d, (local:%d, %s:%d)\n", hit, conn->sp, inet_ntoa(*(struct in_addr *)&conn->dst->da), conn->dst->dport);
					if (conn->result) *conn->result = result;
					conn->callback(conn->content, HK_TO_ST_TYPE(type) | result, conn->arg);
					heap_delmin(event, &event_count);
					del_conn(conn);
				}
				return_dst_delete_hash(dest, conn->dst, type, HK_TO_ST_TYPE(hit), conn->hash);
				if ((conn->status & STATUS_ERROR) == 0)
					goto round;
			}
		}

		//int i;
		switch (status) {
		case 0:
			//for (i = 11; conn->content[i] != ' '; ++i)
			//	putchar(conn->content[i]);
			//putchar(' ');
			conn->sp = libnet_get_prand(LIBNET_PR16) + 32768;
			conn->dst = (type == HK_TYPE1)?
				get_type1(dest, &time):
				get_type2(dest, &time);
			if (conn->dst == NULL) {
				// there is no (da, dp) available currently
				event->time = time;
				goto next;
			}
			else {
				//printf("(local:%d, %s:%d)\n", conn->sp, inet_ntoa(*(struct in_addr *)&conn->dst->da), conn->dst->dport);
				conn->hash = hash_insert(conn->dst->da, conn->dst->dport, conn);
			}
			conn->times = times;

		case 1:
			if (conn->times == times)
				conn->seq = libnet_get_prand(LIBNET_PR32);
			conn->times -= sendpacket(conn->times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, TH_SYN, conn->seq++, 0, NULL, 0);
			if (conn->times > 0) {
				event->time = time;
				goto next;
			}
			else {
				conn->status = (conn->status & ~STATUS_MASK) | 2;
				conn->times = times;
			}
			break;

		case 2:
			if (conn->times == times)
				conn->ack = libnet_get_prand(LIBNET_PR32) + 1;
			conn->times -= sendpacket(conn->times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, TH_ACK, conn->seq, conn->ack, NULL, 0);
			if (conn->times > 0) {
				event->time = time;
				goto next;
			}
			else {
				conn->next = 0;
				conn->status = (conn->status & ~STATUS_MASK) | 3;
				conn->times = times << 1;
			}
			break;

		case 3:
			piece = ((conn->status & STATUS_CHECK)?youtube_len:conn->length) - conn->next;
			type = TH_ACK|TH_PUSH;
			if (piece > tcp_mss)
				piece = tcp_mss;
			else {
				status = 4;
				type |= TH_FIN;
			}

			if (conn->times > times) {
				conn->times -= sendpacket(conn->times - times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, type, conn->seq + conn->next, conn->ack + 1 + conn->next / tcp_mss % 16384, (u_int8_t *)((conn->status & STATUS_CHECK)?youtube:conn->content) + conn->next, piece);
				if (conn->times == times) {
					conn->next += piece;
					if (status != 4)
						conn->times = times << 1;
				}
				else {
					event->time = time;
					goto next;
				}
			}
			if (conn->times <= times) {
				conn->times -= sendpacket(conn->times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, TH_ACK, conn->seq + conn->next, conn->ack + 1 + conn->next / tcp_mss % 16384, NULL, 0);
				if (conn->times > 0) {
					event->time = time;
					goto next;
				}
				else {
					conn->times = times << 1;
					conn->status = (conn->status & ~STATUS_MASK) | 4;
				}
			}
			break;

		case 4:
			if (conn->times == times << 1)
				conn->new_seq = libnet_get_prand(LIBNET_PR32);
			if (conn->times > times) {
				conn->times -= sendpacket(conn->times - times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, TH_SYN, conn->new_seq++, 0, NULL, 0);
				if (conn->times > times) {
					event->time = time;
					goto next;
				}
			}
			if (conn->times <= times) {
				conn->times -= sendpacket(conn->times, &time, sa, conn->dst->da, conn->sp, conn->dst->dport, TH_RST, conn->new_seq, 0, NULL, 0);
				if (conn->times > 0) {
					event->time = time;
					goto next;
				}
				else
					conn->status = (conn->status & ~STATUS_MASK) | 5;
			}
			break;
		}

		if (status != 4)
			event->time = time + time_interval;
		else
			event->time = time + expire_timeout;
	next:
		heap_sink(event - 1, 1, event_count);
	round:
		if (event_count)
			time = event->time;
		else
			time = -1;
	} while (time == inittime);

	return time;
}

int gk_cm_fd() {
	return pcap_get_selectable_fd(pd);
}

void gk_cm_read_cap() {
	static int ret;
	static struct pcap_pkthdr *pkthdr;
	static const u_int8_t *wire;
	static struct tcphdr *tcph;
	static struct iphdr *iph;
	static struct conncontent *conn;
	static char type;
	static u_int32_t hit_range;
	static u_int32_t seq;

	while (1) {
		ret = pcap_next_ex(pd, &pkthdr, &wire);
		if (ret != 1) {
			if (ret == -1)
				pcap_perror(pd, "listen_for_gfw");
			return;
		}
		switch(linktype){
		case DLT_EN10MB:
			if (pkthdr->caplen < 14)
				return;
			if (wire[12] == 8 && wire[13] == 0) {
				linkoffset = 14;
			} else if (wire[12] == 0x81 && wire[13] == 0) {
				linkoffset = 18;
			} else
				return;
			break;
		}
		if (pkthdr->caplen < linkoffset)
			return;
		iph = (struct iphdr *)(wire + linkoffset);
		tcph = (struct tcphdr *)(wire + linkoffset + (iph->ihl << 2));

		conn = hash_match(iph->saddr, ntohs(tcph->source));
		if (conn == NULL || conn->sp != ntohs(tcph->dest))
			continue;
		type = FP_TO_HK_TYPE(gfw_fingerprint(wire + linkoffset));
		if (type == 0)
			continue;

#define WRONG {						\
if (ST_TO_HK_TYPE(conn->status) == type) {	\
	fputs("[Warning]: Unexpected RESET from GFW. "			\
	      "The result must be wrong. Stop other applications which " \
	      "is connecting the same host:port connected with this "	\
	      "application, or adjust the application parameters. "	\
	      "Resume keyword testing after 90 seconds.\n", stderr);	\
	fprintf(stderr, "type%d, flag:%s%s%s, (local:%d, %s:%d), seq: %u, " \
		"ack: %u, conn->seq: %u, conn->ack: %u, conn->new_seq: %u, " \
		"conn->status: %x\n", type, tcph->syn?"s":"", tcph->rst?"r":"", \
		tcph->ack?"a":"", conn->sp, inet_ntoa(*(struct in_addr *)&conn->dst->da), \
		conn->dst->dport, ntohl(tcph->seq), ntohl(tcph->ack_seq), \
		conn->seq, conn->ack, conn->new_seq, conn->status);	\
	conn->status |= STATUS_ERROR;					\
}									\
		}

		if (tcph->syn) {
			if (type == 2) {
				conn->hit |= HK_TYPE2;
				seq = ntohl(tcph->ack_seq);
				if ( (conn->status & STATUS_MASK) < 4 || seq != conn->new_seq )
					WRONG
			}
		}
		else {
			hit_range = conn->ack + 1 + (((conn->status & STATUS_CHECK)?youtube_len:conn->length) + tcp_mss - 1) / tcp_mss;
			seq = ntohl(tcph->seq);

			if (type == 1) {
				conn->hit |= HK_TYPE1;
				if (seq <= conn->ack || seq > hit_range || (conn->status & STATUS_MASK) < 3)
					WRONG
//				else
//					conn->hit_at = seq - conn->ack - 1;
			}
			else {
				conn->hit |= HK_TYPE2;
				if ( (ntohl(tcph->ack_seq) != conn->new_seq) || (conn->status & STATUS_MASK) < 4 ) {
					if ((conn->status & STATUS_MASK) < 3)
						WRONG
					else if (seq > hit_range) {
						seq -= 1460;
						if (seq > hit_range) {
							seq -= 2920;
							if (seq <= conn->ack)
								WRONG
						}
						if (seq <= conn->ack)
							WRONG
					}
					else if (seq <= conn->ack)
						WRONG
//					else {
//						conn->hit_at = seq - conn->ack - 1;
//						fputs("[Information]: Special network environment. Please send the IP block of your ISP with this information to https://groups.google.com/scholarzhang-dev .\n", stderr);
//					}
				}
			}
		}
#undef WRONG
	}
}

/* connmanager */
void gk_cm_config(char _times, int _time_interval, int _expire_timeout, int _tcp_mss, double _kps, int _pps) {
	if (_times > 0)
		times = _times;
	if (_time_interval > 0)
		time_interval = _time_interval;
	if (_expire_timeout > 0)
		expire_timeout = _expire_timeout;
	if (_tcp_mss > 0)
		tcp_mss = _tcp_mss;
	if (_kps >= 1) {
		kps = _kps;
		pps = 0;
	}
	if (_pps >= 0) {
		kps = 0;
		pps = _pps;
	}
}

int gk_add_context(char * const content, const int length, char * const result, const int type, gk_callback_f cb, void *arg) {
	if (event_count == event_capa)
		return -1;

	struct conncontent *conn = new_conn();
	conn->content = content;
	conn->length = length;
	conn->result = result;
	if (result)
		*result = RESULT_ERROR;
	conn->status = 0;
	conn->type = type;
	conn->hit = 0;
	conn->callback = cb;
	conn->arg = arg;

	heap_insert(event, gettime(), conn, &event_count);
	return 0;
}

void gk_cm_finalize() {
	clear_queue();
	hash_empty(3 * event_capa);
	free(event);
	free(connlist.list);
	free(hash);

	if (pd)
		pcap_close(pd);
	if (l)
		libnet_destroy(l);
}

int gk_cm_init(char *device, char *ip, struct dstlist *list, int capa) {
	if (device == NULL || device[0] == '\0') {
		pcap_if_t *alldevsp;
		if (pcap_findalldevs(&alldevsp, pcap_errbuf) != 0) {
			fprintf(stderr, "pcap_findalldevs: %s", pcap_errbuf);
			return -1;
		}
		strcpy(device, alldevsp->name);
		pcap_freealldevs(alldevsp);
	}

	if (list == NULL)
		return -1;
	else
		dest = list;

	if (capa > 0)
		event_capa = capa;
	else
		event_capa = DEFAULT_CONN;
	event = malloc(event_capa * sizeof(struct heap_t));
	connlist.list = malloc(event_capa * sizeof(struct conncontent));
	hash = malloc(3 * event_capa * sizeof(struct hash_t *));

	if (event == NULL || connlist.list == NULL || hash == NULL)
		goto quit;

	empty_connlist();
	memset(hash, 0, 3 * event_capa * sizeof(struct hash_t *));
	event_count = 0;

	if ((l = libnet_init(LIBNET_RAW4, device, errbuf)) == NULL) {
		fputs(errbuf, stderr);
		goto quit;
	}
	if (ip == NULL || ip[0] == '\0') {
		sa = libnet_get_ipaddr4(l);
		ip = inet_ntoa(*(struct in_addr *)&sa);
	}
	else
		sa = inet_addr(ip);

	if (libnet_seed_prand(l) == -1) {
		fputs(errbuf, stderr);
		goto quit;
	}
	pd = pcap_open_live(device, 100, 0, 1, pcap_errbuf);
	if (pd == NULL) {
		fprintf(stderr, "pcap_open_live: %s\n", pcap_errbuf);
		goto quit;
	}
	char filter_exp[50] = "tcp and dst ";
	struct bpf_program fp;
	strcat(filter_exp, ip);
	if ( pcap_compile(pd, &fp, filter_exp, 1, 0) == -1
	     || pcap_setfilter(pd, &fp) == -1 ) {
		pcap_perror(pd, "pcap_compile & pcap_setfilter");
		goto quit;
	}
	if (pcap_setnonblock(pd, 1, pcap_errbuf) == -1) {
		pcap_perror(pd, "pcap_setnonblock");
		goto quit;
	}

	linktype = pcap_datalink(pd);
	switch(linktype){
#ifdef DLT_NULL
		case DLT_NULL:
			linkoffset = 4;
			break;
#endif
		case DLT_EN10MB:
			linkoffset = 14;
			break;
		case DLT_PPP:
			linkoffset = 4;
			break;

		case DLT_RAW:
		case DLT_SLIP:
			linkoffset = 0;
			break;
#define DLT_LINUX_SLL	113
		case DLT_LINUX_SLL:
			linkoffset = 16;
			break;
#ifdef DLT_FDDI
		case DLT_FDDI:
			linkoffset = 21;
			break;
#endif
#ifdef DLT_PPP_SERIAL
		case DLT_PPP_SERIAL:
			linkoffset = 4;
			break;
#endif
		default:
			fprintf(stderr, "Unsupported link type: %d\n", linktype);
			goto quit;
	}


	fprintf(stderr, "Listening on device %s with IP address %s\n", device, ip);
	return 0;

quit:
	gk_cm_finalize();
	return -1;
}
