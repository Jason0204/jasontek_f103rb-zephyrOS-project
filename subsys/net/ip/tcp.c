/** @file
 * @brief TCP handler
 *
 * Handle TCP connections.
 */

/*
 * Copyright (c) 2016 Intel Corporation
 * Copyright 2011-2015 by Andrey Butok. FNET Community.
 * Copyright 2008-2010 by Andrey Butok. Freescale Semiconductor, Inc.
 * Copyright 2003 by Alexey Shervashidze, Andrey Butok. Motorola SPS.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(CONFIG_NET_DEBUG_TCP)
#define SYS_LOG_DOMAIN "net/tcp"
#define NET_DEBUG 1
#endif

#include <kernel.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <net/nbuf.h>
#include <net/net_ip.h>
#include <net/net_context.h>
#include <misc/byteorder.h>

#include "connection.h"
#include "net_private.h"

#include "ipv6.h"
#include "ipv4.h"
#include "tcp.h"

/*
 * Each TCP connection needs to be tracked by net_context, so
 * we need to allocate equal number of control structures here.
 */
#define NET_MAX_TCP_CONTEXT CONFIG_NET_MAX_CONTEXTS
static struct net_tcp tcp_context[NET_MAX_TCP_CONTEXT];

#define INIT_RETRY_MS 200

static struct k_sem tcp_lock;

struct tcp_segment {
	uint32_t seq;
	uint32_t ack;
	uint16_t wnd;
	uint8_t flags;
	uint8_t optlen;
	void *options;
	struct sockaddr_ptr *src_addr;
	const struct sockaddr *dst_addr;
};

#if NET_DEBUG > 0
static char upper_if_set(char chr, bool set)
{
	if (set) {
		return chr & ~0x20;
	}

	return chr | 0x20;
}

static void net_tcp_trace(char *str, struct net_buf *buf)
{
	uint8_t flags = NET_TCP_FLAGS(buf);

	NET_INFO("%s[TCP header]", str);
	NET_INFO("|(SrcPort)         %5u |(DestPort)      %5u |",
		 ntohs(NET_TCP_BUF(buf)->src_port),
		 ntohs(NET_TCP_BUF(buf)->dst_port));
	NET_INFO("|(Sequence number)                 0x%010x |",
		 sys_get_be32(NET_TCP_BUF(buf)->seq));
	NET_INFO("|(ACK number)                      0x%010x |",
		 sys_get_be32(NET_TCP_BUF(buf)->ack));
	NET_INFO("|(HL) %2u |(F)  %c%c%c%c%c%c |(Window)           %5u |",
		 (NET_TCP_BUF(buf)->offset >> 4) * 4,
		 upper_if_set('u', flags & NET_TCP_URG),
		 upper_if_set('a', flags & NET_TCP_ACK),
		 upper_if_set('p', flags & NET_TCP_PSH),
		 upper_if_set('r', flags & NET_TCP_RST),
		 upper_if_set('s', flags & NET_TCP_SYN),
		 upper_if_set('f', flags & NET_TCP_FIN),
		 sys_get_be16(NET_TCP_BUF(buf)->wnd));
	NET_INFO("|(Checksum)    0x%04x |(Urgent)           %5u |",
		 ntohs(NET_TCP_BUF(buf)->chksum),
		 sys_get_be16(NET_TCP_BUF(buf)->urg));
}
#else
#define net_tcp_trace(...)
#endif

static inline uint32_t init_isn(void)
{
	/* Randomise initial seq number */
	return sys_rand32_get();
}

static void tcp_retry_expired(struct k_timer *timer)
{
	struct net_tcp *tcp = CONTAINER_OF(timer, struct net_tcp, retry_timer);
	struct net_buf *buf;

	/* Double the retry period for exponential backoff and resent
	 * the first (only the first!) unack'd packet.
	 */
	if (!sys_slist_is_empty(&tcp->sent_list)) {
		tcp->retry_timeout_ms *= 2;
		k_timer_start(&tcp->retry_timer, tcp->retry_timeout_ms, 0);

		buf = CONTAINER_OF(sys_slist_peek_head(&tcp->sent_list),
				   struct net_buf, sent_list);
		net_tcp_send_buf(buf);
	}
}

struct net_tcp *net_tcp_alloc(struct net_context *context)
{
	int i, key;

	key = irq_lock();
	for (i = 0; i < NET_MAX_TCP_CONTEXT; i++) {
		if (!net_tcp_is_used(&tcp_context[i])) {
			tcp_context[i].flags |= NET_TCP_IN_USE;
			break;
		}
	}
	irq_unlock(key);

	if (i >= NET_MAX_TCP_CONTEXT) {
		return NULL;
	}

	memset(&tcp_context[i], 0, sizeof(struct net_tcp));

	tcp_context[i].flags = NET_TCP_IN_USE;
	tcp_context[i].state = NET_TCP_CLOSED;
	tcp_context[i].context = context;

	tcp_context[i].send_seq = init_isn();
	tcp_context[i].recv_max_ack = tcp_context[i].send_seq + 1u;

	k_timer_init(&tcp_context[i].retry_timer, tcp_retry_expired, NULL);

	return &tcp_context[i];
}

int net_tcp_release(struct net_tcp *tcp)
{
	int key;

	if (!PART_OF_ARRAY(tcp_context, tcp)) {
		return -EINVAL;
	}

	if (tcp->state == NET_TCP_FIN_WAIT_1 ||
	    tcp->state == NET_TCP_FIN_WAIT_2 ||
	    tcp->state == NET_TCP_CLOSING ||
	    tcp->state == NET_TCP_TIME_WAIT) {
		k_delayed_work_cancel(&tcp->fin_timer);
	}

	tcp->state = NET_TCP_CLOSED;
	tcp->context = NULL;

	key = irq_lock();
	tcp->flags &= ~NET_TCP_IN_USE;
	irq_unlock(key);

	NET_DBG("Disposed of TCP connection state");

	return 0;
}

static inline int net_tcp_add_options(struct net_buf *header, size_t len,
				      void *data)
{
	uint8_t optlen;

	memcpy(net_buf_add(header, len), data, len);

	/* Set the length (this value is saved in 4-byte words format) */
	if ((len & 0x3u) != 0u) {
		optlen = (len & 0xfffCu) + 4u;
	} else {
		optlen = len;
	}

	return 0;
}

static int finalize_segment(struct net_context *context, struct net_buf *buf)
{
	int ret = 0;
#if defined(CONFIG_NET_IPV4)
	if (net_nbuf_family(buf) == AF_INET) {
		net_ipv4_finalize(context, buf);
	} else
#endif
#if defined(CONFIG_NET_IPV6)
	if (net_nbuf_family(buf) == AF_INET6) {
		net_ipv6_finalize(context, buf);
	} else
#endif
	{
		ret = -EPROTOTYPE;
	}
	return ret;
}

static struct net_buf *prepare_segment(struct net_tcp *tcp,
				       struct tcp_segment *segment,
				       struct net_buf *buf)
{
	int err;
	struct net_buf *header, *tail = NULL;
	struct net_tcp_hdr *tcphdr;
	struct net_context *context = tcp->context;
	uint16_t dst_port, src_port;

	NET_ASSERT(context);

	if (buf) {
		/* TCP transmit data comes in with a pre-allocated
		 * nbuf at the head (so that net_context_send can find
		 * the context), and the data after.  Rejigger so we
		 * can insert a TCP header cleanly
		 */
		tail = buf->frags;
		buf->frags = NULL;
	} else {
		buf = net_nbuf_get_tx(context);
	}

#if defined(CONFIG_NET_IPV4)
	if (net_nbuf_family(buf) == AF_INET) {
		net_ipv4_create(context, buf,
				&(net_sin(segment->dst_addr)->sin_addr));
		dst_port = net_sin(segment->dst_addr)->sin_port;
		src_port = ((struct sockaddr_in_ptr *)&context->local)->
								sin_port;
		NET_IPV4_BUF(buf)->proto = IPPROTO_TCP;
	} else
#endif
#if defined(CONFIG_NET_IPV6)
	if (net_nbuf_family(buf) == AF_INET6) {
		net_ipv6_create(tcp->context, buf,
				&(net_sin6(segment->dst_addr)->sin6_addr));
		dst_port = net_sin6(segment->dst_addr)->sin6_port;
		src_port = ((struct sockaddr_in6_ptr *)&context->local)->
								sin6_port;
		NET_IPV6_BUF(buf)->nexthdr = IPPROTO_TCP;
	} else
#endif
	{
		goto proto_err;
	}

	header = buf->frags;

	tcphdr = (struct net_tcp_hdr *)net_buf_add(header, NET_TCPH_LEN);

	if (segment->options && segment->optlen) {
		net_tcp_add_options(header, segment->optlen, segment->options);
	} else {
		tcphdr->offset = NET_TCPH_LEN << 2;
	}

	tcphdr->src_port = src_port;
	tcphdr->dst_port = dst_port;
	tcphdr->seq[0] = segment->seq >> 24;
	tcphdr->seq[1] = segment->seq >> 16;
	tcphdr->seq[2] = segment->seq >> 8;
	tcphdr->seq[3] = segment->seq;
	tcphdr->ack[0] = segment->ack >> 24;
	tcphdr->ack[1] = segment->ack >> 16;
	tcphdr->ack[2] = segment->ack >> 8;
	tcphdr->ack[3] = segment->ack;
	tcphdr->flags = segment->flags;
	tcphdr->wnd[0] = segment->wnd >> 8;
	tcphdr->wnd[1] = segment->wnd;
	tcphdr->urg[0] = 0;
	tcphdr->urg[1] = 0;

	if (tail) {
		net_buf_frag_add(buf, tail);
	}

	err = finalize_segment(context, buf);
	if (err) {
	proto_err:
		NET_DBG("Protocol family %d not supported",
			net_nbuf_family(buf));
		net_nbuf_unref(buf);
		return NULL;
	}

	buf = net_nbuf_compact(buf);

	net_tcp_trace("", buf);

	return buf;
}

static inline uint32_t get_recv_wnd(struct net_tcp *tcp)
{
	/* We don't queue received data inside the stack, we hand off
	 * packets to synchronous callbacks (who can queue if they
	 * want, but it's not our business).  So the available window
	 * size is always the same.  There are two configurables to
	 * check though.
	 */
	return min(NET_TCP_MAX_WIN, NET_TCP_BUF_MAX_LEN);
}

/* True if the (signed!) difference "seq1 - seq2" is positive and less
 * than 2^29.  That is, seq1 is "after" seq2.
 */
static inline bool seq_greater(uint32_t seq1, uint32_t seq2)
{
	int d = (int)(seq1 - seq2);
	return d > 0 && d < 0x20000000;
}

int net_tcp_prepare_segment(struct net_tcp *tcp, uint8_t flags,
			    void *options, size_t optlen,
			    const struct sockaddr *remote,
			    struct net_buf **send_buf)
{
	uint32_t seq;
	uint16_t wnd;
	struct tcp_segment segment = { 0 };

	seq = tcp->send_seq;

	if (flags & NET_TCP_ACK) {
		if (tcp->state == NET_TCP_FIN_WAIT_1) {
			if (flags & NET_TCP_FIN) {
				/* FIN is used here only to determine which
				 * state to go to next; it's not to be used
				 * in the sent segment.
				 */
				flags &= ~NET_TCP_FIN;
				net_tcp_change_state(tcp, NET_TCP_TIME_WAIT);
			} else {
				net_tcp_change_state(tcp, NET_TCP_CLOSING);
			}
		} else if (tcp->state == NET_TCP_FIN_WAIT_2) {
			net_tcp_change_state(tcp, NET_TCP_TIME_WAIT);
		} else if (tcp->state == NET_TCP_CLOSE_WAIT) {
			tcp->flags |= NET_TCP_IS_SHUTDOWN;
			flags |= NET_TCP_FIN;
			net_tcp_change_state(tcp, NET_TCP_LAST_ACK);
		}
	}

	if (flags & NET_TCP_FIN) {
		tcp->flags |= NET_TCP_FINAL_SENT;
		seq++;

		if (tcp->state == NET_TCP_ESTABLISHED ||
		    tcp->state == NET_TCP_SYN_RCVD) {
			net_tcp_change_state(tcp, NET_TCP_FIN_WAIT_1);
		}
	}

	if (flags & NET_TCP_SYN) {
		seq++;
	}

	wnd = get_recv_wnd(tcp);

	segment.src_addr = &tcp->context->local;
	segment.dst_addr = remote;
	segment.seq = tcp->send_seq;
	segment.ack = tcp->send_ack;
	segment.flags = flags;
	segment.wnd = wnd;
	segment.options = options;
	segment.optlen = optlen;

	*send_buf = prepare_segment(tcp, &segment, *send_buf);

	tcp->send_seq = seq;

	if (seq_greater(tcp->send_seq, tcp->recv_max_ack)) {
		tcp->recv_max_ack = tcp->send_seq;
	}

	return 0;
}

static inline uint32_t get_size(uint32_t pos1, uint32_t pos2)
{
	uint32_t size;

	if (pos1 <= pos2) {
		size = pos2 - pos1;
	} else {
		size = NET_TCP_MAX_SEQ - pos1 + pos2 + 1;
	}

	return size;
}

#if defined(CONFIG_NET_IPV4)
#ifndef NET_IP_MAX_PACKET
#define NET_IP_MAX_PACKET (10 * 1024)
#endif

#define NET_IP_MAX_OPTIONS 40 /* Maximum option field length */

static inline size_t ip_max_packet_len(struct in_addr *dest_ip)
{
	ARG_UNUSED(dest_ip);

	return (NET_IP_MAX_PACKET - (NET_IP_MAX_OPTIONS +
		      sizeof(struct net_ipv4_hdr))) & (~0x3LU);
}
#else /* CONFIG_NET_IPV4 */
#define ip_max_packet_len(...) 0
#endif /* CONFIG_NET_IPV4 */

static void net_tcp_set_syn_opt(struct net_tcp *tcp, uint8_t *options,
				uint8_t *optionlen)
{
	*optionlen = 0;

	/* If 0, detect MSS based on interface MTU minus "TCP,IP header size"
	 */
	if (tcp->recv_mss == 0) {
		sa_family_t family = net_context_get_family(tcp->context);

		if (family == AF_INET) {
#if defined(CONFIG_NET_IPV4)
			struct net_if *iface =
				net_context_get_iface(tcp->context);

			if (iface) {
				/* MTU - [TCP,IP header size]. */
				tcp->recv_mss = iface->mtu - 40;
			}
#else
			tcp->recv_mss = 0;
#endif /* CONFIG_NET_IPV4 */
		}
#if defined(CONFIG_NET_IPV6)
		else if (family == AF_INET6) {
			tcp->recv_mss = 1280;
		}
#endif /* CONFIG_NET_IPV6 */
		else {
			tcp->recv_mss = 0;
		}
	}

	*((uint32_t *)(options + *optionlen)) =
		htonl((uint32_t)(tcp->recv_mss | NET_TCP_MSS_HEADER));
	*optionlen += NET_TCP_MSS_SIZE;

	return;
}

int net_tcp_prepare_ack(struct net_tcp *tcp, const struct sockaddr *remote,
			struct net_buf **buf)
{
	uint8_t options[NET_TCP_MAX_OPT_SIZE];
	uint8_t optionlen;

	switch (tcp->state) {
	case NET_TCP_SYN_RCVD:
		/* In the SYN_RCVD state acknowledgment must be with the
		 * SYN flag.
		 */
		tcp->send_seq--;

		net_tcp_set_syn_opt(tcp, options, &optionlen);

		net_tcp_prepare_segment(tcp, NET_TCP_SYN | NET_TCP_ACK,
					options, optionlen, remote, buf);
		break;

	case NET_TCP_FIN_WAIT_1:
	case NET_TCP_LAST_ACK:
		/* In the FIN_WAIT_1 and LAST_ACK states acknowledgment must
		 * be with the FIN flag.
		 */
		tcp->send_seq--;

		net_tcp_prepare_segment(tcp, NET_TCP_FIN | NET_TCP_ACK,
					0, 0, remote, buf);
		break;

	default:
		net_tcp_prepare_segment(tcp, NET_TCP_ACK, 0, 0, remote, buf);
		break;
	}

	return 0;
}

int net_tcp_prepare_reset(struct net_tcp *tcp,
			  const struct sockaddr *remote,
			  struct net_buf **buf)
{
	struct tcp_segment segment = { 0 };

	if ((net_context_get_state(tcp->context) != NET_CONTEXT_UNCONNECTED) &&
	    (tcp->state != NET_TCP_SYN_SENT) &&
	    (tcp->state != NET_TCP_TIME_WAIT)) {
		if (tcp->state == NET_TCP_SYN_RCVD) {
			/* Send the reset segment with acknowledgment. */
			segment.seq = 0;
			segment.ack = tcp->send_ack;
			segment.flags = NET_TCP_RST | NET_TCP_ACK;
		} else {
			/* Send the reset segment without acknowledgment. */
			segment.seq = tcp->recv_ack;
			segment.ack = 0;
			segment.flags = NET_TCP_RST;
		}

		segment.src_addr = &tcp->context->local;
		segment.dst_addr = remote;
		segment.wnd = 0;
		segment.options = NULL;
		segment.optlen = 0;

		*buf = prepare_segment(tcp, &segment, NULL);
	}

	return 0;
}

const char * const net_tcp_state_str(enum net_tcp_state state)
{
#if NET_DEBUG
	switch (state) {
	case NET_TCP_CLOSED:
		return "CLOSED";
	case NET_TCP_LISTEN:
		return "LISTEN";
	case NET_TCP_SYN_SENT:
		return "SYN_SENT";
	case NET_TCP_SYN_RCVD:
		return "SYN_RCVD";
	case NET_TCP_ESTABLISHED:
		return "ESTABLISHED";
	case NET_TCP_CLOSE_WAIT:
		return "CLOSE_WAIT";
	case NET_TCP_LAST_ACK:
		return "LAST_ACK";
	case NET_TCP_FIN_WAIT_1:
		return "FIN_WAIT_1";
	case NET_TCP_FIN_WAIT_2:
		return "FIN_WAIT_2";
	case NET_TCP_TIME_WAIT:
		return "TIME_WAIT";
	case NET_TCP_CLOSING:
		return "CLOSING";
	}
#endif

	return "";
}

int tcp_queue_data(struct net_context *context, struct net_buf *buf)
{
	int ret, data_len;
	struct net_conn *conn = (struct net_conn *)context->conn_handler;

	data_len = net_buf_frags_len(buf);

	/* Set PSH on all packets, our window is so small that there's
	 * no point in the remote side trying to finesse things and
	 * coalesce packets.
	 */
	ret = net_tcp_prepare_segment(context->tcp, NET_TCP_PSH | NET_TCP_ACK,
				      NULL, 0, &conn->remote_addr, &buf);
	if (ret) {
		return ret;
	}

	context->tcp->send_seq += data_len;

	sys_slist_append(&context->tcp->sent_list, &buf->sent_list);
	net_buf_ref(buf);

	return 0;
}

int net_tcp_send_buf(struct net_buf *buf)
{
	struct net_context *ctx = net_nbuf_context(buf);
	struct net_tcp_hdr *tcphdr = NET_TCP_BUF(buf);

	sys_put_be32(ctx->tcp->send_ack, tcphdr->ack);

	/* The data stream code always sets this flag, because
	 * existing stacks (Linux, anyway) seem to ignore data packets
	 * without a valid-but-already-transmitted ACK.  But set it
	 * anyway if we know we need it just to sanify edge cases.
	 */
	if (ctx->tcp->sent_ack != ctx->tcp->send_ack) {
		tcphdr->flags |= NET_TCP_ACK;
	}

	ctx->tcp->sent_ack = ctx->tcp->send_ack;

	net_nbuf_set_buf_sent(buf, true);

	return net_send_data(buf);
}

static void restart_timer(struct net_tcp *tcp)
{
	if (sys_slist_is_empty(&tcp->sent_list)) {
		tcp->flags |= NET_TCP_RETRYING;
		tcp->retry_timeout_ms = INIT_RETRY_MS;
		k_timer_start(&tcp->retry_timer, INIT_RETRY_MS, 0);
	} else {
		k_timer_stop(&tcp->retry_timer);
		tcp->flags &= ~NET_TCP_RETRYING;
	}
}

int tcp_send_data(struct net_context *context)
{
	struct net_buf *buf;
	sys_snode_t *node;

	/* For now, just send all queued data synchronously.  Need to
	 * add window handling and retry/ACK logic.
	 */
	SYS_SLIST_FOR_EACH_NODE(&context->tcp->sent_list, node) {
		buf = CONTAINER_OF(node, struct net_buf, sent_list);
		if (!net_nbuf_buf_sent(buf)) {
			net_tcp_send_buf(buf);
		}
	}

	return 0;
}

void net_tcp_ack_received(struct net_context *ctx, uint32_t ack)
{
	sys_slist_t *list = &ctx->tcp->sent_list;
	sys_snode_t *head;
	struct net_buf *buf;
	struct net_tcp_hdr *tcphdr;
	uint32_t seq;
	bool valid_ack = false;

	while (!sys_slist_is_empty(list)) {
		head = sys_slist_peek_head(list);
		buf = CONTAINER_OF(head, struct net_buf, sent_list);
		tcphdr = NET_TCP_BUF(buf);

		seq = sys_get_be32(tcphdr->seq)
			+ net_buf_frags_len(buf) - 1;

		if (seq_greater(ack, seq)) {
			sys_slist_remove(list, NULL, head);
			net_nbuf_unref(buf);
			valid_ack = true;
		} else {
			break;
		}
	}

	if (valid_ack) {
		/* Restart the timer on a valid inbound ACK.  This
		 * isn't quite the same behavior as per-packet retry
		 * timers, but is close in practice (it starts retries
		 * one timer period after the connection "got stuck")
		 * and avoids the need to track per-packet timers or
		 * sent times.
		 */
		restart_timer(ctx->tcp);

		/* And, if we had been retrying, mark all packets
		 * untransmitted and then resend them.  The stalled
		 * pipe is uncorked again.
		 */
		if (ctx->tcp->flags & NET_TCP_RETRYING) {
			struct net_buf *buf;
			sys_snode_t *node;

			SYS_SLIST_FOR_EACH_NODE(&ctx->tcp->sent_list, node) {
				buf = CONTAINER_OF(node, struct net_buf,
						   sent_list);
				net_nbuf_set_buf_sent(buf, false);
			}
			tcp_send_data(ctx);
		}
	}
}

void net_tcp_init(void)
{
	k_sem_init(&tcp_lock, 0, UINT_MAX);
	k_sem_give(&tcp_lock);
}

#define FIN_TIMEOUT (2 * NET_TCP_MAX_SEG_LIFETIME * MSEC_PER_SEC)

static void fin_timeout(struct k_work *work)
{
	struct net_tcp *tcp = CONTAINER_OF(work, struct net_tcp, fin_timer);

	NET_DBG("Remote peer didn't confirm connection close");

	net_context_put(tcp->context);
}

#if NET_DEBUG
static void validate_state_transition(enum net_tcp_state current,
				      enum net_tcp_state new)
{
	static const uint16_t valid_transitions[] = {
		[NET_TCP_CLOSED] = 1 << NET_TCP_LISTEN |
			1 << NET_TCP_SYN_SENT,
		[NET_TCP_LISTEN] = 1 << NET_TCP_SYN_RCVD |
			1 << NET_TCP_SYN_SENT,
		[NET_TCP_SYN_RCVD] = 1 << NET_TCP_FIN_WAIT_1 |
			1 << NET_TCP_ESTABLISHED |
			1 << NET_TCP_LISTEN |
			1 << NET_TCP_CLOSED,
		[NET_TCP_SYN_SENT] = 1 << NET_TCP_CLOSED |
			1 << NET_TCP_ESTABLISHED |
			1 << NET_TCP_SYN_RCVD,
		[NET_TCP_ESTABLISHED] = 1 << NET_TCP_CLOSE_WAIT |
			1 << NET_TCP_FIN_WAIT_1,
		[NET_TCP_CLOSE_WAIT] = 1 << NET_TCP_LAST_ACK,
		[NET_TCP_LAST_ACK] = 1 << NET_TCP_CLOSED,
		[NET_TCP_FIN_WAIT_1] = 1 << NET_TCP_CLOSING |
			1 << NET_TCP_FIN_WAIT_2 |
			1 << NET_TCP_TIME_WAIT,
		[NET_TCP_FIN_WAIT_2] = 1 << NET_TCP_TIME_WAIT,
		[NET_TCP_CLOSING] = 1 << NET_TCP_TIME_WAIT,
		[NET_TCP_TIME_WAIT] = 1 << NET_TCP_CLOSED
	};

	if (!(valid_transitions[current] & 1 << new)) {
		NET_DBG("Invalid state transition: %s (%d) => %s (%d)",
			net_tcp_state_str(current), current,
			net_tcp_state_str(new), new);
	}
}
#endif /* NET_DEBUG */

void net_tcp_change_state(struct net_tcp *tcp,
			  enum net_tcp_state new_state)
{
	NET_ASSERT(tcp);

	if (tcp->state == new_state) {
		return;
	}

	NET_ASSERT(new_state >= NET_TCP_CLOSED &&
		   new_state <= NET_TCP_CLOSING);

	NET_DBG("state@%p %s (%d) => %s (%d)",
		tcp, net_tcp_state_str(tcp->state), tcp->state,
		net_tcp_state_str(new_state), new_state);

#if NET_DEBUG
	validate_state_transition(tcp->state, new_state);
#endif /* NET_DEBUG */

	tcp->state = new_state;

	if (tcp->state == NET_TCP_FIN_WAIT_1) {
		/* Wait up to 2 * MSL before destroying this socket. */
		k_delayed_work_cancel(&tcp->fin_timer);
		k_delayed_work_init(&tcp->fin_timer, fin_timeout);
		k_delayed_work_submit(&tcp->fin_timer, FIN_TIMEOUT);
	}

	if (tcp->state != NET_TCP_CLOSED) {
		return;
	}

	if (!tcp->context) {
		return;
	}

	/* Remove any port handlers if we are closing */
	if (tcp->context->conn_handler) {
		net_tcp_unregister(tcp->context->conn_handler);
		tcp->context->conn_handler = NULL;
	}

	if (tcp->context->accept_cb) {
		tcp->context->accept_cb(tcp->context,
					&tcp->context->remote,
					sizeof(struct sockaddr),
					-ENETRESET,
					tcp->context->user_data);
	}
}

void net_tcp_foreach(net_tcp_cb_t cb, void *user_data)
{
	int i, key;

	key = irq_lock();

	for (i = 0; i < NET_MAX_TCP_CONTEXT; i++) {
		if (!net_tcp_is_used(&tcp_context[i])) {
			continue;
		}

		irq_unlock(key);

		cb(&tcp_context[i], user_data);

		key = irq_lock();
	}

	irq_unlock(key);
}
