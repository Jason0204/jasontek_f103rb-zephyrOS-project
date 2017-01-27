/* main.c - Application main entry point */

/*
 * Copyright (c) 2016 Intel Corporation
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

#include <zephyr.h>
#include <sections.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <device.h>
#include <init.h>
#include <misc/printk.h>
#include <net/buf.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_ip.h>
#include <net/ethernet.h>

#include <tc_util.h>

#if defined(CONFIG_NET_DEBUG_UDP)
#define DBG(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

#include "udp.h"
#include "net_private.h"

static bool fail = true;
static struct k_sem recv_lock;

struct net_udp_context {
	uint8_t mac_addr[sizeof(struct net_eth_addr)];
	struct net_linkaddr ll_addr;
};

int net_udp_dev_init(struct device *dev)
{
	struct net_udp_context *net_udp_context = dev->driver_data;

	net_udp_context = net_udp_context;

	return 0;
}

static uint8_t *net_udp_get_mac(struct device *dev)
{
	struct net_udp_context *context = dev->driver_data;

	if (context->mac_addr[0] == 0x00) {
		/* 10-00-00-00-00 to 10-00-00-00-FF Documentation RFC7042 */
		context->mac_addr[0] = 0x10;
		context->mac_addr[1] = 0x00;
		context->mac_addr[2] = 0x00;
		context->mac_addr[3] = 0x00;
		context->mac_addr[4] = 0x00;
		context->mac_addr[5] = sys_rand32_get();
	}

	return context->mac_addr;
}

static void net_udp_iface_init(struct net_if *iface)
{
	uint8_t *mac = net_udp_get_mac(net_if_get_device(iface));

	net_if_set_link_addr(iface, mac, 6);
}

static int send_status = -EINVAL;

static int tester_send(struct net_if *iface, struct net_buf *buf)
{
	if (!buf->frags) {
		DBG("No data to send!\n");
		return -ENODATA;
	}

	DBG("Data was sent successfully\n");

	net_nbuf_unref(buf);

	send_status = 0;

	return 0;
}

static inline struct in_addr *if_get_addr(struct net_if *iface)
{
	int i;

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->ipv4.unicast[i].is_used &&
		    iface->ipv4.unicast[i].address.family == AF_INET &&
		    iface->ipv4.unicast[i].addr_state == NET_ADDR_PREFERRED) {
			return &iface->ipv4.unicast[i].address.in_addr;
		}
	}

	return NULL;
}

struct net_udp_context net_udp_context_data;

static struct net_if_api net_udp_if_api = {
	.init = net_udp_iface_init,
	.send = tester_send,
};

#define _ETH_L2_LAYER DUMMY_L2
#define _ETH_L2_CTX_TYPE NET_L2_GET_CTX_TYPE(DUMMY_L2)

NET_DEVICE_INIT(net_udp_test, "net_udp_test",
		net_udp_dev_init, &net_udp_context_data, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		&net_udp_if_api, _ETH_L2_LAYER, _ETH_L2_CTX_TYPE, 127);

struct ud {
	const struct sockaddr *remote_addr;
	const struct sockaddr *local_addr;
	uint16_t remote_port;
	uint16_t local_port;
	char *test;
	void *handle;
};

static struct ud *returned_ud;

static enum net_verdict test_ok(struct net_conn *conn,
				struct net_buf *buf,
				void *user_data)
{
	struct ud *ud = (struct ud *)user_data;

	k_sem_give(&recv_lock);

	if (!ud) {
		fail = true;

		DBG("Test %s failed.", ud->test);

		return NET_DROP;
	}

	fail = false;

	returned_ud = user_data;

	net_nbuf_unref(buf);

	return NET_OK;
}

static enum net_verdict test_fail(struct net_conn *conn,
				  struct net_buf *buf,
				  void *user_data)
{
	/* This function should never be called as there should not
	 * be a matching UDP connection.
	 */

	fail = true;
	return NET_DROP;
}

static void setup_ipv6_udp(struct net_buf *buf,
			   struct in6_addr *remote_addr,
			   struct in6_addr *local_addr,
			   uint16_t remote_port,
			   uint16_t local_port)
{
	NET_IPV6_BUF(buf)->vtc = 0x60;
	NET_IPV6_BUF(buf)->tcflow = 0;
	NET_IPV6_BUF(buf)->flow = 0;
	NET_IPV6_BUF(buf)->len[0] = 0;
	NET_IPV6_BUF(buf)->len[1] = NET_UDPH_LEN;

	NET_IPV6_BUF(buf)->nexthdr = IPPROTO_UDP;
	NET_IPV6_BUF(buf)->hop_limit = 255;

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, remote_addr);
	net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, local_addr);

	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	NET_UDP_BUF(buf)->src_port = htons(remote_port);
	NET_UDP_BUF(buf)->dst_port = htons(local_port);

	net_nbuf_set_ext_len(buf, 0);

	net_buf_add(buf->frags, net_nbuf_ip_hdr_len(buf) +
				sizeof(struct net_udp_hdr));
}

static void setup_ipv4_udp(struct net_buf *buf,
			   struct in_addr *remote_addr,
			   struct in_addr *local_addr,
			   uint16_t remote_port,
			   uint16_t local_port)
{
	NET_IPV4_BUF(buf)->vhl = 0x45;
	NET_IPV4_BUF(buf)->tos = 0;
	NET_IPV4_BUF(buf)->len[0] = 0;
	NET_IPV4_BUF(buf)->len[1] = NET_UDPH_LEN +
		sizeof(struct net_ipv4_hdr);

	NET_IPV4_BUF(buf)->proto = IPPROTO_UDP;

	net_ipaddr_copy(&NET_IPV4_BUF(buf)->src, remote_addr);
	net_ipaddr_copy(&NET_IPV4_BUF(buf)->dst, local_addr);

	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv4_hdr));

	NET_UDP_BUF(buf)->src_port = htons(remote_port);
	NET_UDP_BUF(buf)->dst_port = htons(local_port);

	net_nbuf_set_ext_len(buf, 0);

	net_buf_add(buf->frags, net_nbuf_ip_hdr_len(buf) +
				sizeof(struct net_udp_hdr));
}

#define TIMEOUT 200

static bool send_ipv6_udp_msg(struct net_if *iface,
			      struct in6_addr *src,
			      struct in6_addr *dst,
			      uint16_t src_port,
			      uint16_t dst_port,
			      struct ud *ud,
			      bool expect_failure)
{
	struct net_buf *buf;
	struct net_buf *frag;
	int ret;

	buf = net_nbuf_get_reserve_tx(0);
	frag = net_nbuf_get_reserve_data(0);
	net_buf_frag_add(buf, frag);

	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_ll_reserve(buf, net_buf_headroom(frag));

	setup_ipv6_udp(buf, src, dst, src_port, dst_port);

	ret = net_recv_data(iface, buf);
	if (ret < 0) {
		printk("Cannot recv buf %p, ret %d\n", buf, ret);
		return false;
	}

	if (k_sem_take(&recv_lock, TIMEOUT)) {
		printk("Timeout, packet not received\n");
		if (expect_failure) {
			return false;
		} else {
			return true;
		}
	}

	/* Check that the returned user data is the same as what was given
	 * as a parameter.
	 */
	if (ud != returned_ud && !expect_failure) {
		printk("IPv6 wrong user data %p returned, expected %p\n",
		       returned_ud, ud);
		return false;
	}

	return !fail;
}

static bool send_ipv4_udp_msg(struct net_if *iface,
			      struct in_addr *src,
			      struct in_addr *dst,
			      uint16_t src_port,
			      uint16_t dst_port,
			      struct ud *ud,
			      bool expect_failure)
{
	struct net_buf *buf;
	struct net_buf *frag;
	int ret;

	buf = net_nbuf_get_reserve_tx(0);
	frag = net_nbuf_get_reserve_data(0);
	net_buf_frag_add(buf, frag);

	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_ll_reserve(buf, net_buf_headroom(frag));

	setup_ipv4_udp(buf, src, dst, src_port, dst_port);

	ret = net_recv_data(iface, buf);
	if (ret < 0) {
		printk("Cannot recv buf %p, ret %d\n", buf, ret);
		return false;
	}

	if (k_sem_take(&recv_lock, TIMEOUT)) {
		printk("Timeout, packet not received\n");
		if (expect_failure) {
			return false;
		} else {
			return true;
		}
	}

	/* Check that the returned user data is the same as what was given
	 * as a parameter.
	 */
	if (ud != returned_ud && !expect_failure) {
		printk("IPv4 wrong user data %p returned, expected %p\n",
		       returned_ud, ud);
		return false;
	}

	return !fail;
}

static void set_port(sa_family_t family, struct sockaddr *raddr,
		     struct sockaddr *laddr, uint16_t rport,
		     uint16_t lport)
{
	if (family == AF_INET6) {
		if (raddr) {
			((struct sockaddr_in6 *)raddr)->
				sin6_port = htons(rport);
		}
		if (laddr) {
			((struct sockaddr_in6 *)laddr)->
				sin6_port = htons(lport);
		}
	} else if (family == AF_INET) {
		if (raddr) {
			((struct sockaddr_in *)raddr)->
				sin_port = htons(rport);
		}
		if (laddr) {
			((struct sockaddr_in *)laddr)->
				sin_port = htons(lport);
		}
	}
}

static bool run_tests(void)
{
	struct net_conn_handle *handlers[CONFIG_NET_MAX_CONN];
	struct net_if *iface = net_if_get_default();
	struct net_if_addr *ifaddr;
	struct ud *ud;
	int ret, i = 0;
	bool st;

	struct sockaddr_in6 any_addr6;
	const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;

	struct sockaddr_in6 my_addr6;
	struct in6_addr in6addr_my = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
					   0, 0, 0, 0, 0, 0, 0, 0x1 } } };

	struct sockaddr_in6 peer_addr6;
	struct in6_addr in6addr_peer = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
					  0, 0, 0, 0x4e, 0x11, 0, 0, 0x2 } } };

	struct sockaddr_in any_addr4;
	const struct in_addr in4addr_any = { { { 0 } } };

	struct sockaddr_in my_addr4;
	struct in_addr in4addr_my = { { { 192, 0, 2, 1 } } };

	struct sockaddr_in peer_addr4;
	struct in_addr in4addr_peer = { { { 192, 0, 2, 9 } } };

	net_ipaddr_copy(&any_addr6.sin6_addr, &in6addr_any);
	any_addr6.sin6_family = AF_INET6;

	net_ipaddr_copy(&my_addr6.sin6_addr, &in6addr_my);
	my_addr6.sin6_family = AF_INET6;

	net_ipaddr_copy(&peer_addr6.sin6_addr, &in6addr_peer);
	peer_addr6.sin6_family = AF_INET6;

	net_ipaddr_copy(&any_addr4.sin_addr, &in4addr_any);
	any_addr4.sin_family = AF_INET;

	net_ipaddr_copy(&my_addr4.sin_addr, &in4addr_my);
	my_addr4.sin_family = AF_INET;

	net_ipaddr_copy(&peer_addr4.sin_addr, &in4addr_peer);
	peer_addr4.sin_family = AF_INET;

	k_sem_init(&recv_lock, 0, UINT_MAX);

	ifaddr = net_if_ipv6_addr_add(iface, &in6addr_my, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		printk("Cannot add %s to interface %p\n",
		       net_sprint_ipv6_addr(&in6addr_my), iface);
		return false;
	}

	ifaddr = net_if_ipv4_addr_add(iface, &in4addr_my, NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		printk("Cannot add %s to interface %p\n",
		       net_sprint_ipv4_addr(&in4addr_my), iface);
		return false;
	}

#define REGISTER(family, raddr, laddr, rport, lport)			\
	({								\
		static struct ud user_data;				\
									\
		user_data.remote_addr = (struct sockaddr *)raddr;	\
		user_data.local_addr =  (struct sockaddr *)laddr;	\
		user_data.remote_port = rport;				\
		user_data.local_port = lport;				\
		user_data.test = #raddr"-"#laddr"-"#rport"-"#lport;	\
									\
		set_port(family, (struct sockaddr *)raddr,		\
			 (struct sockaddr *)laddr, rport, lport);	\
									\
		ret = net_udp_register((struct sockaddr *)raddr,	\
				       (struct sockaddr *)laddr,	\
				       rport, lport,			\
				       test_ok, &user_data,		\
				       &handlers[i]);			\
		if (ret) {						\
			printk("UDP register %s failed (%d)\n",		\
			       user_data.test, ret);			\
			return false;					\
		}							\
		user_data.handle = handlers[i++];			\
		&user_data;						\
	})

#define REGISTER_FAIL(raddr, laddr, rport, lport)			\
	ret = net_udp_register((struct sockaddr *)raddr,		\
			       (struct sockaddr *)laddr,		\
			       rport, lport,				\
			       test_fail, INT_TO_POINTER(0), NULL);	\
	if (!ret) {							\
		printk("UDP register invalid match %s failed\n",	\
		       #raddr"-"#laddr"-"#rport"-"#lport);		\
		return false;						\
	}

#define UNREGISTER(ud)							\
	ret = net_udp_unregister(ud->handle);				\
	if (ret) {							\
		printk("UDP unregister %p failed (%d)\n", ud->handle,	\
		       ret);						\
		return false;						\
	}

#define TEST_IPV6_OK(ud, raddr, laddr, rport, lport)			\
	st = send_ipv6_udp_msg(iface, raddr, laddr, rport, lport, ud,	\
			       false);					\
	if (!st) {							\
		printk("%d: UDP test \"%s\" fail\n", __LINE__,		\
		       ud->test);					\
		return false;						\
	}

#define TEST_IPV4_OK(ud, raddr, laddr, rport, lport)			\
	st = send_ipv4_udp_msg(iface, raddr, laddr, rport, lport, ud,	\
			       false);					\
	if (!st) {							\
		printk("%d: UDP test \"%s\" fail\n", __LINE__,		\
		       ud->test);					\
		return false;						\
	}

#define TEST_IPV6_FAIL(ud, raddr, laddr, rport, lport)			\
	st = send_ipv6_udp_msg(iface, raddr, laddr, rport, lport, ud,	\
			       true);					\
	if (st) {							\
		printk("%d: UDP neg test \"%s\" fail\n", __LINE__,	\
		       ud->test);					\
		return false;						\
	}

#define TEST_IPV4_FAIL(ud, raddr, laddr, rport, lport)			\
	st = send_ipv4_udp_msg(iface, raddr, laddr, rport, lport, ud,	\
			       true);					\
	if (st) {							\
		printk("%d: UDP neg test \"%s\" fail\n", __LINE__,	\
		       ud->test);					\
		return false;						\
	}

	ud = REGISTER(AF_INET6, &any_addr6, &any_addr6, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	UNREGISTER(ud);

	ud = REGISTER(AF_INET, &any_addr4, &any_addr4, 1234, 4242);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 4242);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 4242);
	TEST_IPV4_FAIL(ud, &in4addr_peer, &in4addr_my, 1234, 4325);
	TEST_IPV4_FAIL(ud, &in4addr_peer, &in4addr_my, 1234, 4325);
	UNREGISTER(ud);

	ud = REGISTER(AF_INET6, &any_addr6, NULL, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	UNREGISTER(ud);

	ud = REGISTER(AF_INET6, NULL, &any_addr6, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 61400);
	UNREGISTER(ud);

	ud = REGISTER(AF_INET6, &peer_addr6, &my_addr6, 1234, 4242);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 4242);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 1234, 4243);

	ud = REGISTER(AF_INET, &peer_addr4, &my_addr4, 1234, 4242);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 4242);
	TEST_IPV4_FAIL(ud, &in4addr_peer, &in4addr_my, 1234, 4243);

	ud = REGISTER(AF_UNSPEC, NULL, NULL, 1234, 42423);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 42423);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 42423);

	ud = REGISTER(AF_UNSPEC, NULL, NULL, 1234, 0);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 42422);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 42422);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 1234, 42422);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 1234, 42422);

	TEST_IPV4_FAIL(ud, &in4addr_peer, &in4addr_my, 12345, 42421);
	TEST_IPV6_FAIL(ud, &in6addr_peer, &in6addr_my, 12345, 42421);

	ud = REGISTER(AF_UNSPEC, NULL, NULL, 0, 0);
	TEST_IPV4_OK(ud, &in4addr_peer, &in4addr_my, 12345, 42421);
	TEST_IPV6_OK(ud, &in6addr_peer, &in6addr_my, 12345, 42421);

	/* Remote addr same as local addr, these two will never match */
	REGISTER(AF_INET6, &my_addr6, NULL, 1234, 4242);
	REGISTER(AF_INET, &my_addr4, NULL, 1234, 4242);

	/* IPv4 remote addr and IPv6 remote addr, impossible combination */
	REGISTER_FAIL(&my_addr4, &my_addr6, 1234, 4242);

	if (fail) {
		printk("Tests failed\n");
		return false;
	}

	i--;
	while (i) {
		ret = net_udp_unregister(handlers[i]);
		if (ret < 0 && ret != -ENOENT) {
			printk("Cannot unregister udp %d\n", i);
			return false;
		}

		i--;
	}

	if (!(net_udp_unregister(NULL) < 0)) {
		printk("Unregister udp failed\n");
		return false;
	}

	printk("Network UDP checks passed\n");
	return true;
}

void main_thread(void)
{
	if (run_tests()) {
		TC_END_REPORT(TC_PASS);
	} else {
		TC_END_REPORT(TC_FAIL);
	}
}

#define STACKSIZE 2000
char __noinit __stack thread_stack[STACKSIZE];

void main(void)
{
	k_thread_spawn(&thread_stack[0], STACKSIZE,
		       (k_thread_entry_t)main_thread,
		       NULL, NULL, NULL, K_PRIO_COOP(7), 0, 0);
}
