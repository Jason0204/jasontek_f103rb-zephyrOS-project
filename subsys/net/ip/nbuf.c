/** @file
 @brief Network buffers for IP stack

 Network data is passed between components using nbuf.
 */

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

#if defined(CONFIG_NET_DEBUG_NET_BUF)
#define SYS_LOG_DOMAIN "net/nbuf"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#define NET_DEBUG 1
#endif

#include <kernel.h>
#include <toolchain.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#include <misc/util.h>

#include <net/net_ip.h>
#include <net/buf.h>
#include <net/nbuf.h>

#include <net/net_core.h>

#include "net_private.h"

/* Available (free) buffers queue */
#define NBUF_RX_COUNT	CONFIG_NET_NBUF_RX_COUNT
#define NBUF_TX_COUNT	CONFIG_NET_NBUF_TX_COUNT
#define NBUF_DATA_COUNT	CONFIG_NET_NBUF_DATA_COUNT
#define NBUF_DATA_LEN	CONFIG_NET_NBUF_DATA_SIZE
#define NBUF_USER_DATA_LEN CONFIG_NET_NBUF_USER_DATA_SIZE

#if defined(CONFIG_NET_TCP)
#define APP_PROTO_LEN NET_TCPH_LEN
#else
#if defined(CONFIG_NET_UDP)
#define APP_PROTO_LEN NET_UDPH_LEN
#else
#define APP_PROTO_LEN 0
#endif /* UDP */
#endif /* TCP */

#if defined(CONFIG_NET_IPV6) || defined(CONFIG_NET_L2_RAW_CHANNEL)
#define IP_PROTO_LEN NET_IPV6H_LEN
#else
#if defined(CONFIG_NET_IPV4)
#define IP_PROTO_LEN NET_IPV4H_LEN
#else
#error "Either IPv6 or IPv4 needs to be selected."
#endif /* IPv4 */
#endif /* IPv6 */

#define EXTRA_PROTO_LEN NET_ICMPH_LEN

/* Make sure that IP + TCP/UDP header fit into one
 * fragment. This makes possible to cast a protocol header
 * struct into memory area.
 */
#if NBUF_DATA_LEN < (IP_PROTO_LEN + APP_PROTO_LEN)
#if defined(STRING2)
#undef STRING2
#endif
#if defined(STRING)
#undef STRING
#endif
#define STRING2(x) #x
#define STRING(x) STRING2(x)
#pragma message "Data len " STRING(NBUF_DATA_LEN)
#pragma message "Minimum len " STRING(IP_PROTO_LEN + APP_PROTO_LEN)
#error "Too small net_buf fragment size"
#endif

#if NET_DEBUG

#define NET_BUF_CHECK_IF_IN_USE(buf, ref)				\
	do {								\
		if (ref) {						\
			NET_ERR("**ERROR** buf %p in use (%s:%s():%d)", \
				buf, __FILE__, __func__, __LINE__);     \
		}                                                       \
	} while (0)

#define NET_BUF_CHECK_IF_NOT_IN_USE(buf, ref)				\
	do {								\
		if (!(ref)) {                                           \
			NET_ERR("**ERROR** buf %p not in use (%s:%s():%d)", \
				buf, __FILE__, __func__, __LINE__);     \
		}                                                       \
	} while (0)

static int num_free_rx_bufs = NBUF_RX_COUNT;
static int num_free_tx_bufs = NBUF_TX_COUNT;
static int num_free_data_bufs = NBUF_DATA_COUNT;

static inline void dec_free_rx_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	num_free_rx_bufs--;
	if (num_free_rx_bufs < 0) {
		NET_DBG("*** ERROR *** Invalid RX buffer count.");
		num_free_rx_bufs = 0;
	}
}

static inline void inc_free_rx_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	if (num_free_rx_bufs > NBUF_RX_COUNT) {
		num_free_rx_bufs = NBUF_RX_COUNT;
	} else {
		num_free_rx_bufs++;
	}
}

static inline void dec_free_tx_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	num_free_tx_bufs--;
	if (num_free_tx_bufs < 0) {
		NET_DBG("*** ERROR *** Invalid TX buffer count.");
		num_free_tx_bufs = 0;
	}
}

static inline void inc_free_tx_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	if (num_free_tx_bufs > NBUF_TX_COUNT) {
		num_free_tx_bufs = NBUF_TX_COUNT;
	} else {
		num_free_tx_bufs++;
	}
}

static inline void dec_free_data_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	num_free_data_bufs--;
	if (num_free_data_bufs < 0) {
		NET_DBG("*** ERROR *** Invalid data buffer count.");
		num_free_data_bufs = 0;
	}
}

static inline void inc_free_data_bufs(struct net_buf *buf)
{
	if (!buf) {
		return;
	}

	if (num_free_data_bufs > NBUF_DATA_COUNT) {
		num_free_data_bufs = NBUF_DATA_COUNT;
	} else {
		num_free_data_bufs++;
	}
}

static inline int get_frees(enum net_nbuf_type type)
{
	switch (type) {
	case NET_NBUF_RX:
		return num_free_rx_bufs;
	case NET_NBUF_TX:
		return num_free_tx_bufs;
	case NET_NBUF_DATA:
		return num_free_data_bufs;
	}

	return 0xffffffff;
}

#define inc_free_rx_bufs_func inc_free_rx_bufs
#define inc_free_tx_bufs_func inc_free_tx_bufs
#define inc_free_data_bufs_func inc_free_data_bufs

#else /* NET_DEBUG */
#define dec_free_rx_bufs(...)
#define inc_free_rx_bufs(...)
#define dec_free_tx_bufs(...)
#define inc_free_tx_bufs(...)
#define dec_free_data_bufs(...)
#define inc_free_data_bufs(...)
#define inc_free_rx_bufs_func(...)
#define inc_free_tx_bufs_func(...)
#define inc_free_data_bufs_func(...)
#define NET_BUF_CHECK_IF_IN_USE(buf, ref)
#define NET_BUF_CHECK_IF_NOT_IN_USE(buf, ref)
#endif /* NET_DEBUG */

static struct k_fifo free_rx_bufs;
static struct k_fifo free_tx_bufs;
static struct k_fifo free_data_bufs;

static inline void free_rx_bufs_func(struct net_buf *buf)
{
	inc_free_rx_bufs_func(buf);

	k_fifo_put(buf->free, buf);
}

static inline void free_tx_bufs_func(struct net_buf *buf)
{
	inc_free_tx_bufs_func(buf);

	k_fifo_put(buf->free, buf);
}

static inline void free_data_bufs_func(struct net_buf *buf)
{
	inc_free_data_bufs_func(buf);

	k_fifo_put(buf->free, buf);
}

/* The RX and TX pools do not store any data. Only bearer / protocol
 * related data is stored here.
 */
static NET_BUF_POOL(rx_buffers, NBUF_RX_COUNT, 0,	\
		    &free_rx_bufs, free_rx_bufs_func,	\
		    sizeof(struct net_nbuf));
static NET_BUF_POOL(tx_buffers, NBUF_TX_COUNT, 0,	\
		    &free_tx_bufs, free_tx_bufs_func,	\
		    sizeof(struct net_nbuf));

/* The data fragment pool is for storing network data. */
static NET_BUF_POOL(data_buffers, NBUF_DATA_COUNT,	\
		    NBUF_DATA_LEN, &free_data_bufs,	\
		    free_data_bufs_func, NBUF_USER_DATA_LEN);

static inline bool is_from_data_pool(struct net_buf *buf)
{
	if (buf->free == &free_data_bufs) {
		return true;
	}

	return false;
}

#if NET_DEBUG
static inline const char *type2str(enum net_nbuf_type type)
{
	switch (type) {
	case NET_NBUF_RX:
		return "RX";
	case NET_NBUF_TX:
		return "TX";
	case NET_NBUF_DATA:
		return "DATA";
	}

	return NULL;
}

void net_nbuf_print_frags(struct net_buf *buf)
{
	struct net_buf *frag;
	size_t total = 0;
	int count = 0, frag_size = 0, ll_overhead = 0;

	NET_DBG("Buf %p frags %p", buf, buf->frags);

	NET_ASSERT(buf->frags);

	frag = buf->frags;

	while (frag) {
		total += frag->len;

		frag_size = frag->size;
		ll_overhead = net_buf_headroom(frag);

		NET_DBG("[%d] frag %p len %d size %d reserve %d",
			count, frag, frag->len, frag_size, ll_overhead);

		count++;

		frag = frag->frags;
	}

	NET_DBG("Total data size %zu, occupied %d bytes, ll overhead %d, "
		"utilization %zu%%",
		total, count * frag_size - count * ll_overhead,
		count * ll_overhead, (total * 100) / (count * frag_size));
}

static struct net_buf *net_nbuf_get_reserve_debug(enum net_nbuf_type type,
						  uint16_t reserve_head,
						  const char *caller,
						  int line)
#else
static struct net_buf *net_nbuf_get_reserve(enum net_nbuf_type type,
					    uint16_t reserve_head)
#endif
{
	struct net_buf *buf = NULL;

	/*
	 * The reserve_head variable in the function will tell
	 * the size of the link layer headers if there are any.
	 */
	switch (type) {
	case NET_NBUF_RX:
		buf = net_buf_get(&free_rx_bufs, 0);
		if (!buf) {
			return NULL;
		}

		NET_ASSERT_INFO(buf->ref, "RX buf %p ref %d", buf, buf->ref);

		dec_free_rx_bufs(buf);
		net_nbuf_set_type(buf, type);
		break;
	case NET_NBUF_TX:
		buf = net_buf_get(&free_tx_bufs, 0);
		if (!buf) {
			return NULL;
		}

		NET_ASSERT_INFO(buf->ref, "TX buf %p ref %d", buf, buf->ref);

		dec_free_tx_bufs(buf);
		net_nbuf_set_type(buf, type);
		break;
	case NET_NBUF_DATA:
		buf = net_buf_get(&free_data_bufs, 0);
		if (!buf) {
			return NULL;
		}

		NET_ASSERT_INFO(buf->ref, "DATA buf %p ref %d", buf, buf->ref);

		/* The buf->data will point to the start of the L3
		 * header (like IPv4 or IPv6 packet header) after the
		 * add() and pull().
		 */
		net_buf_add(buf, reserve_head);
		net_buf_pull(buf, reserve_head);

		dec_free_data_bufs(buf);
		break;
	default:
		NET_ERR("Invalid type %d for net_buf", type);
		return NULL;
	}

	if (!buf) {
#if NET_DEBUG
#define PRINT_CYCLE (30 * MSEC_PER_SEC)
		static int64_t next_print;
		int64_t curr = k_uptime_get();

		if (!next_print || (next_print < curr &&
				    (!((curr - next_print) > PRINT_CYCLE)))) {
			int64_t new_print;

			NET_ERR("Failed to get free %s buffer (%s():%d)",
				type2str(type), caller, line);

			new_print = curr + PRINT_CYCLE;
			if (new_print > curr) {
				next_print = new_print;
			} else {
				/* Overflow */
				next_print = PRINT_CYCLE -
					(LLONG_MAX - curr);
			}
		}
#endif /* NET_DEBUG */

		return NULL;
	}

	NET_BUF_CHECK_IF_NOT_IN_USE(buf, buf->ref + 1);

	if (type != NET_NBUF_DATA) {
		net_nbuf_set_context(buf, NULL);
		net_nbuf_ll_dst(buf)->addr = NULL;
		net_nbuf_ll_src(buf)->addr = NULL;

		/* Let's make sure ll_reserve is not set
		 * from a previous usage of the buffer.
		 */
		net_nbuf_set_ll_reserve(buf, 0);
	}

	NET_DBG("%s [%d] buf %p reserve %u ref %d (%s():%d)",
		type2str(type), get_frees(type),
		buf, reserve_head, buf->ref, caller, line);

	return buf;
}

#if NET_DEBUG
struct net_buf *net_nbuf_get_reserve_rx_debug(uint16_t reserve_head,
					      const char *caller, int line)
{
	return net_nbuf_get_reserve_debug(NET_NBUF_RX, reserve_head,
					  caller, line);
}

struct net_buf *net_nbuf_get_reserve_tx_debug(uint16_t reserve_head,
					      const char *caller, int line)
{
	return net_nbuf_get_reserve_debug(NET_NBUF_TX, reserve_head,
					  caller, line);
}

struct net_buf *net_nbuf_get_reserve_data_debug(uint16_t reserve_head,
						const char *caller, int line)
{
	return net_nbuf_get_reserve_debug(NET_NBUF_DATA, reserve_head,
					  caller, line);
}

#else

struct net_buf *net_nbuf_get_reserve_rx(uint16_t reserve_head)
{
	return net_nbuf_get_reserve(NET_NBUF_RX, reserve_head);
}

struct net_buf *net_nbuf_get_reserve_tx(uint16_t reserve_head)
{
	return net_nbuf_get_reserve(NET_NBUF_TX, reserve_head);
}

struct net_buf *net_nbuf_get_reserve_data(uint16_t reserve_head)
{
	return net_nbuf_get_reserve(NET_NBUF_DATA, reserve_head);
}

#endif /* NET_DEBUG */


#if NET_DEBUG
static struct net_buf *net_nbuf_get_debug(enum net_nbuf_type type,
					  struct net_context *context,
					  const char *caller, int line)
#else
static struct net_buf *net_nbuf_get(enum net_nbuf_type type,
				    struct net_context *context)
#endif /* NET_DEBUG */
{
	struct net_if *iface = net_context_get_iface(context);
	struct in6_addr *addr6 = NULL;
	struct net_buf *buf;
	uint16_t reserve;

	NET_ASSERT_INFO(context && iface, "context %p iface %p",
			context, iface);

	if (context && net_context_get_family(context) == AF_INET6) {
		addr6 = &((struct sockaddr_in6 *) &context->remote)->sin6_addr;
	}

	reserve = net_if_get_ll_reserve(iface, addr6);

#if NET_DEBUG
	buf = net_nbuf_get_reserve_debug(type, reserve, caller, line);
#else
	buf = net_nbuf_get_reserve(type, reserve);
#endif
	if (!buf) {
		return buf;
	}

	if (type != NET_NBUF_DATA) {
		net_nbuf_set_context(buf, context);
		net_nbuf_set_ll_reserve(buf, reserve);
		net_nbuf_set_family(buf, net_context_get_family(context));
		net_nbuf_set_iface(buf, iface);
	}

	return buf;
}

#if NET_DEBUG
struct net_buf *net_nbuf_get_rx_debug(struct net_context *context,
				      const char *caller, int line)
{
	return net_nbuf_get_debug(NET_NBUF_RX, context, caller, line);
}

struct net_buf *net_nbuf_get_tx_debug(struct net_context *context,
				      const char *caller, int line)
{
	return net_nbuf_get_debug(NET_NBUF_TX, context, caller, line);
}

struct net_buf *net_nbuf_get_data_debug(struct net_context *context,
					const char *caller, int line)
{
	return net_nbuf_get_debug(NET_NBUF_DATA, context, caller, line);
}

#else /* NET_DEBUG */

struct net_buf *net_nbuf_get_rx(struct net_context *context)
{
	NET_ASSERT_INFO(context, "RX context not set");

	return net_nbuf_get(NET_NBUF_RX, context);
}

struct net_buf *net_nbuf_get_tx(struct net_context *context)
{
	NET_ASSERT_INFO(context, "TX context not set");

	return net_nbuf_get(NET_NBUF_TX, context);
}

struct net_buf *net_nbuf_get_data(struct net_context *context)
{
	NET_ASSERT_INFO(context, "Data context not set");

	return net_nbuf_get(NET_NBUF_DATA, context);
}

#endif /* NET_DEBUG */


#if NET_DEBUG
void net_nbuf_unref_debug(struct net_buf *buf, const char *caller, int line)
{
	struct net_buf *frag;

#else
void net_nbuf_unref(struct net_buf *buf)
{
#endif
	if (!buf) {
		NET_DBG("*** ERROR *** buf %p (%s():%d)", buf, caller, line);
		return;
	}

	if (!buf->ref) {
		NET_DBG("*** ERROR *** buf %p is freed already (%s():%d)",
			buf, caller, line);
		return;
	}

	if (!is_from_data_pool(buf)) {
		NET_DBG("%s [%d] buf %p ref %d frags %p (%s():%d)",
			type2str(net_nbuf_type(buf)),
			get_frees(net_nbuf_type(buf)),
			buf, buf->ref - 1, buf->frags, caller, line);
	} else {
		NET_DBG("%s [%d] buf %p ref %d frags %p (%s():%d)",
			type2str(NET_NBUF_DATA),
			get_frees(NET_NBUF_DATA),
			buf, buf->ref - 1, buf->frags, caller, line);
	}

#if NET_DEBUG
	if (buf->ref > 1) {
		goto done;
	}

	/* Only remove fragments if debug is enabled since net_buf_unref takes
	 * care of removing all the fragments.
	 */
	frag = buf->frags;
	while (frag) {
		NET_DBG("%s [%d] buf %p ref %d frags %p (%s():%d)",
			type2str(NET_NBUF_DATA),
			get_frees(NET_NBUF_DATA),
			frag, frag->ref - 1, frag->frags, caller, line);

		if (!frag->ref) {
			NET_DBG("*** ERROR *** frag %p is freed already "
				"(%s():%d)", frag, caller, line);
		}

		frag = net_buf_frag_del(buf, frag);
	}

done:
#endif
	net_buf_unref(buf);
}


#if NET_DEBUG
struct net_buf *net_nbuf_ref_debug(struct net_buf *buf, const char *caller,
				   int line)
#else
struct net_buf *net_nbuf_ref(struct net_buf *buf)
#endif
{
	if (!buf) {
		NET_DBG("*** ERROR *** buf %p (%s():%d)", buf, caller, line);
		return NULL;
	}

	if (!is_from_data_pool(buf)) {
		NET_DBG("%s [%d] buf %p ref %d (%s():%d)",
			type2str(net_nbuf_type(buf)),
			get_frees(net_nbuf_type(buf)),
			buf, buf->ref + 1, caller, line);
	} else {
		NET_DBG("%s buf %p ref %d (%s():%d)",
			type2str(NET_NBUF_DATA),
			buf, buf->ref + 1, caller, line);
	}

	return net_buf_ref(buf);
}

struct net_buf *net_nbuf_copy(struct net_buf *orig, size_t amount,
			      size_t reserve)
{
	uint16_t ll_reserve = net_buf_headroom(orig);
	struct net_buf *frag, *first;

	if (!is_from_data_pool(orig)) {
		NET_ERR("Buffer %p is not a data fragment", orig);
		return NULL;
	}

	frag = net_nbuf_get_reserve_data(ll_reserve);

	if (reserve > net_buf_tailroom(frag)) {
		NET_ERR("Reserve %zu is too long, max is %zu",
			reserve, net_buf_tailroom(frag));
		net_nbuf_unref(frag);
		return NULL;
	}

	net_buf_add(frag, reserve);

	first = frag;

	NET_DBG("Copying frag %p with %zu bytes and reserving %zu bytes",
		first, amount, reserve);

	if (!orig->len) {
		/* No data in the first fragment in the original message */
		NET_DBG("Original buffer empty!");
		return frag;
	}

	while (orig && amount) {
		int left_len = net_buf_tailroom(frag);
		int copy_len;

		if (amount > orig->len) {
			copy_len = orig->len;
		} else {
			copy_len = amount;
		}

		if ((copy_len - left_len) >= 0) {
			/* Just copy the data from original fragment
			 * to new fragment. The old data will fit the
			 * new fragment and there could be some space
			 * left in the new fragment.
			 */
			amount -= left_len;

			memcpy(net_buf_add(frag, left_len), orig->data,
			       left_len);

			if (!net_buf_tailroom(frag)) {
				/* There is no space left in copy fragment.
				 * We must allocate a new one.
				 */
				struct net_buf *new_frag =
					net_nbuf_get_reserve_data(ll_reserve);

				net_buf_frag_add(frag, new_frag);

				frag = new_frag;
			}

			net_buf_pull(orig, left_len);

			continue;
		} else {
			/* We should be at the end of the original buf
			 * fragment list.
			 */
			amount -= copy_len;

			memcpy(net_buf_add(frag, copy_len), orig->data,
			       copy_len);
			net_buf_pull(orig, copy_len);
		}

		orig = orig->frags;
	}

	return first;
}

bool net_nbuf_is_compact(struct net_buf *buf)
{
	struct net_buf *last;
	size_t total = 0, calc;
	int count = 0;

	last = NULL;

	if (!is_from_data_pool(buf)) {
		/* Skip the first element that does not contain any data.
		 */
		buf = buf->frags;
	}

	while (buf) {
		total += buf->len;
		count++;

		last = buf;
		buf = buf->frags;
	}

	NET_ASSERT(last);

	if (!last) {
		return false;
	}

	calc = count * last->size - net_buf_tailroom(last) -
		count * net_buf_headroom(last);

	if (total == calc) {
		return true;
	}

	NET_DBG("Not compacted total %zu real %zu", total, calc);

	return false;
}

struct net_buf *net_nbuf_compact(struct net_buf *buf)
{
	struct net_buf *first, *prev;

	first = buf;

	if (!is_from_data_pool(buf)) {
		NET_DBG("Buffer %p is not a data fragment", buf);
		buf = buf->frags;
	}

	prev = NULL;

	NET_DBG("Compacting data to buf %p", first);

	while (buf) {
		if (buf->frags) {
			/* Copy amount of data from next fragment to this
			 * fragment.
			 */
			size_t copy_len;

			copy_len = buf->frags->len;
			if (copy_len > net_buf_tailroom(buf)) {
				copy_len = net_buf_tailroom(buf);
			}

			memcpy(net_buf_tail(buf), buf->frags->data, copy_len);
			net_buf_add(buf, copy_len);

			memmove(buf->frags->data,
				buf->frags->data + copy_len,
				buf->frags->len - copy_len);

			buf->frags->len -= copy_len;

			/* Is there any more space in this fragment */
			if (net_buf_tailroom(buf)) {
				struct net_buf *frag;

				/* There is. This also means that the next
				 * fragment is empty as otherwise we could
				 * not have copied all data.
				 */
				frag = buf->frags;

				/* Remove next fragment as there is no
				 * data in it any more.
				 */
				net_buf_frag_del(buf, buf->frags);

				/* Then check next fragment */
				continue;
			}
		} else {
			if (!buf->len) {
				/* Remove the last fragment because there is no
				 * data in it.
				 */
				NET_ASSERT_INFO(prev,
					"First element cannot be deleted!");

				net_buf_frag_del(prev, buf);
			}
		}

		prev = buf;
		buf = buf->frags;
	}

	/* If the buf exists, then it is the last fragment and can be removed.
	 */
	if (buf) {
		net_nbuf_unref(buf);

		if (prev) {
			prev->frags = NULL;
		}
	}

	return first;
}

struct net_buf *net_nbuf_push(struct net_buf *parent,
			      struct net_buf *buf,
			      size_t amount)
{
	struct net_buf *frag;

	NET_ASSERT_INFO(amount > 3,
			"Amount %zu very small and not recommended", amount);

	if (amount > buf->len) {
		NET_DBG("Cannot move amount %zu because the buf "
			"length is only %u bytes", amount, buf->len);
		return NULL;
	}

	frag = net_nbuf_get_reserve_data(net_buf_headroom(buf));

	net_buf_add(frag, amount);

	if (parent) {
		net_buf_frag_insert(parent, frag);
	} else {
		net_buf_frag_insert(frag, buf);
		parent = frag;
	}

	return net_nbuf_compact(parent);
}

struct net_buf *net_nbuf_pull(struct net_buf *buf, size_t amount)
{
	struct net_buf *first;
	ssize_t count = amount;

	if (amount == 0) {
		NET_DBG("No data to remove.");
		return buf;
	}

	first = buf;

	if (!is_from_data_pool(buf)) {
		NET_DBG("Buffer %p is not a data fragment", buf);
		buf = buf->frags;
	}

	NET_DBG("Removing first %zd bytes from the fragments (%zu bytes)",
		count, net_buf_frags_len(buf));

	while (buf && count > 0) {
		if (count < buf->len) {
			/* We can remove the data from this single fragment */
			net_buf_pull(buf, count);
			return first;
		}

		if (buf->len == count) {
			if (buf == first) {
				struct net_buf tmp;

				tmp.frags = buf;
				first = buf->frags;
				net_buf_frag_del(&tmp, buf);
			} else {
				net_buf_frag_del(first, buf);
			}

			return first;
		}

		count -= buf->len;

		if (buf == first) {
			struct net_buf tmp;

			tmp.frags = buf;
			first = buf->frags;
			net_buf_frag_del(&tmp, buf);
			buf = first;
		} else {
			net_buf_frag_del(first, buf);
			buf = first->frags;
		}
	}

	if (count > 0) {
		NET_ERR("Not enough data in the fragments");
	}

	return first;
}

/* This helper routine will append multiple bytes, if there is no place for
 * the data in current fragment then create new fragment and add it to
 * the buffer. It assumes that the buffer has at least one fragment.
 */
static inline bool net_nbuf_append_bytes(struct net_buf *buf, uint8_t *value,
					 uint16_t len)
{
	struct net_buf *frag = net_buf_frag_last(buf);
	uint16_t ll_reserve = net_nbuf_ll_reserve(buf);

	do {
		uint16_t count = min(len, net_buf_tailroom(frag));
		void *data = net_buf_add(frag, count);

		memcpy(data, value, count);
		len -= count;
		value += count;

		if (len == 0) {
			return true;
		}

		frag = net_nbuf_get_reserve_data(ll_reserve);
		if (!frag) {
			return false;
		}

		net_buf_frag_add(buf, frag);
	} while (1);

	return false;
}

bool net_nbuf_append(struct net_buf *buf, uint16_t len, uint8_t *data)
{
	struct net_buf *frag;

	if (!buf || !data) {
		return false;
	}

	if (is_from_data_pool(buf)) {
		/* The buf needs to be a net_buf that has user data
		 * part. Otherwise we cannot use the net_nbuf_ll_reserve()
		 * function to figure out the reserve amount.
		 */
		NET_DBG("Buffer %p is a data fragment", buf);
		return false;
	}

	if (!buf->frags) {
		frag = net_nbuf_get_reserve_data(net_nbuf_ll_reserve(buf));
		if (!frag) {
			return false;
		}

		net_buf_frag_add(buf, frag);
	}

	return net_nbuf_append_bytes(buf, data, len);
}

/* Helper routine to retrieve single byte from fragment and move
 * offset. If required byte is last byte in framgent then return
 * next fragment and set offset = 0.
 */
static inline struct net_buf *net_nbuf_read_byte(struct net_buf *buf,
						 uint16_t offset,
						 uint16_t *pos,
						 uint8_t *data)
{
	if (data) {
		*data = buf->data[offset];
	}

	*pos = offset + 1;

	if (*pos >= buf->len) {
		*pos = 0;

		return buf->frags;
	}

	return buf;
}

/* Helper function to adjust offset in net_nbuf_read() call
 * if given offset is more than current fragment length.
 */
static inline struct net_buf *adjust_offset(struct net_buf *buf,
					    uint16_t offset, uint16_t *pos)
{
	if (!buf || !is_from_data_pool(buf)) {
		NET_ERR("Invalid buffer or buffer is not a fragment");
		return NULL;
	}

	while (buf) {
		if (offset == buf->len) {
			*pos = 0;

			return buf->frags;
		} else if (offset < buf->len) {
			*pos = offset;

			return buf;
		}

		offset -= buf->len;
		buf = buf->frags;
	}

	NET_ERR("Invalid offset, failed to adjust");

	return NULL;
}

struct net_buf *net_nbuf_read(struct net_buf *buf, uint16_t offset,
			      uint16_t *pos, uint16_t len, uint8_t *data)
{
	uint16_t copy = 0;

	buf = adjust_offset(buf, offset, pos);
	if (!buf) {
		goto error;
	}

	while (len-- > 0 && buf) {
		if (data) {
			buf = net_nbuf_read_byte(buf, *pos, pos, data + copy++);
		} else {
			buf = net_nbuf_read_byte(buf, *pos, pos, NULL);
		}

		/* Error: Still reamining length to be read, but no data. */
		if (!buf && len) {
			NET_ERR("Not enough data to read");
			goto error;
		}
	}

	return buf;

error:
	*pos = 0xffff;

	return NULL;
}

struct net_buf *net_nbuf_read_be16(struct net_buf *buf, uint16_t offset,
				   uint16_t *pos, uint16_t *value)
{
	struct net_buf *retbuf;
	uint8_t v16[2];

	retbuf = net_nbuf_read(buf, offset, pos, sizeof(uint16_t), v16);

	*value = v16[0] << 8 | v16[1];

	return retbuf;
}

struct net_buf *net_nbuf_read_be32(struct net_buf *buf, uint16_t offset,
				   uint16_t *pos, uint32_t *value)
{
	struct net_buf *retbuf;
	uint8_t v32[4];

	retbuf = net_nbuf_read(buf, offset, pos, sizeof(uint32_t), v32);

	*value = v32[0] << 24 | v32[1] << 16 | v32[2] << 8 | v32[3];

	return retbuf;
}

static inline struct net_buf *check_and_create_data(struct net_buf *buf,
						    struct net_buf *data)
{
	struct net_buf *frag;

	if (data) {
		return data;
	}

	frag = net_nbuf_get_reserve_data(net_nbuf_ll_reserve(buf));
	if (!frag) {
		return NULL;
	}

	net_buf_frag_add(buf, frag);

	return frag;
}

static inline struct net_buf *adjust_write_offset(struct net_buf *buf,
						  struct net_buf *frag,
						  uint16_t offset,
						  uint16_t *pos)
{
	uint16_t tailroom;

	do {
		frag = check_and_create_data(buf, frag);
		if (!frag) {
			return NULL;
		}

		/* Offset is less than current fragment length, so new data
		 *  will start from this "offset".
		 */
		if (offset < frag->len) {
			*pos = offset;

			return frag;
		}

		/* Offset is equal to fragment length. If some tailtoom exists,
		 * offset start from same fragment otherwise offset starts from
		 * beginning of next fragment.
		 */
		if (offset == frag->len) {
			if (net_buf_tailroom(frag)) {
				*pos = offset;

				return frag;
			}

			*pos = 0;

			return check_and_create_data(buf, frag->frags);
		}

		/* If the offset is more than current fragment length, remove
		 * current fragment length and verify with tailroom in the
		 * current fragment. From here on create empty (space/fragments)
		 * to reach proper offset.
		 */
		if (offset > frag->len) {

			offset -= frag->len;
			tailroom = net_buf_tailroom(frag);

			if (offset < tailroom) {
				/* Create empty space */
				net_buf_add(frag, offset);

				*pos = frag->len;

				return frag;
			}

			if (offset == tailroom) {
				/* Create empty space */
				net_buf_add(frag, tailroom);

				*pos = 0;

				return check_and_create_data(buf, frag->frags);
			}

			if (offset > tailroom) {
				/* Creating empty space */
				net_buf_add(frag, tailroom);
				offset -= tailroom;

				frag = check_and_create_data(buf, frag->frags);
			}
		}

	} while (1);

	return NULL;
}

struct net_buf *net_nbuf_write(struct net_buf *buf, struct net_buf *frag,
			       uint16_t offset, uint16_t *pos,
			       uint16_t len, uint8_t *data)
{
	uint16_t ll_reserve;

	if (!buf || is_from_data_pool(buf)) {
		NET_ERR("Invalid buffer or it is data fragment");
		goto error;
	}

	ll_reserve = net_nbuf_ll_reserve(buf);

	frag = adjust_write_offset(buf, frag, offset, &offset);
	if (!frag) {
		NET_DBG("Failed to adjust offset");
		goto error;
	}

	do {
		uint16_t space = frag->size - net_buf_headroom(frag) - offset;
		uint16_t count = min(len, space);
		int size_to_add;

		memcpy(frag->data + offset, data, count);

		/* If we are overwriting on already available space then need
		 * not to update the length, otherwise increase it.
		 */
		size_to_add = offset + count - frag->len;
		if (size_to_add > 0) {
			net_buf_add(frag, size_to_add);
		}

		len -= count;
		if (len == 0) {
			*pos = offset + count;

			return frag;
		}

		data += count;
		offset = 0;
		frag = frag->frags;

		if (!frag) {
			frag = net_nbuf_get_reserve_data(ll_reserve);
			if (!frag) {
				goto error;
			}

			net_buf_frag_add(buf, frag);
		}
	} while (1);

error:
	*pos = 0xffff;

	return NULL;
}

static inline bool insert_data(struct net_buf *buf, struct net_buf *frag,
			       struct net_buf *temp, uint16_t offset,
			       uint16_t len, uint8_t *data)
{
	struct net_buf *insert;

	do {
		uint16_t count = min(len, net_buf_tailroom(frag));

		/* Copy insert data */
		memcpy(frag->data + offset, data, count);
		net_buf_add(frag, count);

		len -= count;
		if (len == 0) {
			/* Once insertion is done, then add the data if
			 * there is anything after original insertion
			 * offset.
			 */
			if (temp) {
				net_buf_frag_insert(frag, temp);
			}

			/* As we are creating temporary buffers to cache,
			 * compact the fragments to save space.
			 */
			net_nbuf_compact(buf->frags);

			return true;
		}

		data += count;
		offset = 0;

		insert = net_nbuf_get_reserve_data(net_nbuf_ll_reserve(buf));
		if (!insert) {
			return false;
		}

		net_buf_frag_insert(frag, insert);
		frag = insert;

	} while (1);

	return false;
}

static inline struct net_buf *adjust_insert_offset(struct net_buf *buf,
						   uint16_t offset,
						   uint16_t *pos)
{
	if (!buf || !is_from_data_pool(buf)) {
		NET_ERR("Invalid buffer or buffer is not a fragment");

		return NULL;
	}

	while (buf) {
		if (offset == buf->len) {
			*pos = 0;

			return buf->frags;
		}

		if (offset < buf->len) {
			*pos = offset;

			return buf;
		}

		if (offset > buf->len) {
			if (buf->frags) {
				offset -= buf->len;
				buf = buf->frags;
			} else {
				return NULL;
			}
		}
	}

	NET_ERR("Invalid offset, failed to adjust");

	return NULL;
}

bool net_nbuf_insert(struct net_buf *buf, struct net_buf *frag,
		     uint16_t offset, uint16_t len, uint8_t *data)
{
	struct net_buf *temp = NULL;
	uint16_t bytes;

	if (!buf || is_from_data_pool(buf)) {
		return false;
	}

	frag = adjust_insert_offset(frag, offset, &offset);
	if (!frag) {
		return false;
	}

	/* If there is any data after offset, store in temp fragment and
	 * add it after insertion is completed.
	 */
	bytes = frag->len - offset;
	if (bytes) {
		temp = net_nbuf_get_reserve_data(net_nbuf_ll_reserve(buf));
		if (!temp) {
			return false;
		}

		memcpy(net_buf_add(temp, bytes), frag->data + offset, bytes);

		frag->len -= bytes;
	}

	/* Insert data into current(frag) fragment from "offset". */
	return insert_data(buf, frag, temp, offset, len, data);
}

void net_nbuf_get_info(size_t *tx_size, size_t *rx_size, size_t *data_size,
		       int *tx, int *rx, int *data)
{
	if (tx_size) {
		*tx_size = sizeof(tx_buffers);
	}

	if (rx_size) {
		*rx_size = sizeof(rx_buffers);
	}

	if (data_size) {
		*data_size = sizeof(data_buffers);
	}

#if NET_DEBUG
	*tx = get_frees(NET_NBUF_TX);
	*rx = get_frees(NET_NBUF_RX);
	*data = get_frees(NET_NBUF_DATA);
#else
	*tx = BIT(31);
	*rx = BIT(31);
	*data = BIT(31);
#endif
}

#if NET_DEBUG
void net_nbuf_print(void)
{
	int tx, rx, data;

	net_nbuf_get_info(NULL, NULL, NULL, &tx, &rx, &data);

	NET_DBG("TX %d RX %d DATA %d", tx, rx, data);
}
#endif

void net_nbuf_init(void)
{
	NET_DBG("Allocating %u RX (%zu bytes), %u TX (%zu bytes) "
		"and %u data (%zu bytes) buffers",
		NBUF_RX_COUNT, sizeof(rx_buffers),
		NBUF_TX_COUNT, sizeof(tx_buffers),
		NBUF_DATA_COUNT, sizeof(data_buffers));

	net_buf_pool_init(rx_buffers);
	net_buf_pool_init(tx_buffers);
	net_buf_pool_init(data_buffers);
}
