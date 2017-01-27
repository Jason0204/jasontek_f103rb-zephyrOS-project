/** @file
 * @brief IPv6 and IPv4 definitions
 *
 * Generic IPv6 and IPv4 address definitions.
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

#ifndef __NET_IP_H
#define __NET_IP_H

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <misc/byteorder.h>
#include <toolchain.h>

#include <net/net_linkaddr.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol families */
#define PF_UNSPEC	0	/* Unspecified.  */
#define PF_INET		2	/* IP protocol family.  */
#define PF_INET6	10	/* IP version 6.  */

/** Address families.  */
#define AF_UNSPEC	PF_UNSPEC
#define AF_INET		PF_INET
#define AF_INET6	PF_INET6

/** Protocol numbers from IANA */
enum net_ip_protocol {
	IPPROTO_ICMP = 1,
	IPPROTO_TCP = 6,
	IPPROTO_UDP = 17,
	IPPROTO_ICMPV6 = 58,
};

/** Socket type */
enum net_sock_type {
	SOCK_DGRAM = 1,
	SOCK_STREAM,
};

#define ntohs(x) sys_be16_to_cpu(x)
#define ntohl(x) sys_be32_to_cpu(x)
#define htons(x) sys_cpu_to_be16(x)
#define htonl(x) sys_cpu_to_be32(x)

/** IPv6 address structure */
struct in6_addr {
	union {
		uint8_t		u6_addr8[16];
		uint16_t	u6_addr16[8]; /* In big endian */
		uint32_t	u6_addr32[4]; /* In big endian */
	} in6_u;
#define s6_addr			in6_u.u6_addr8
#define s6_addr16		in6_u.u6_addr16
#define s6_addr32		in6_u.u6_addr32
};

/** IPv4 address */
struct in_addr {
	union {
		uint8_t		u4_addr8[4];
		uint16_t	u4_addr16[2]; /* In big endian */
		uint32_t	u4_addr32[1]; /* In big endian */
	} in4_u;
#define s4_addr			in4_u.u4_addr8
#define s4_addr16		in4_u.u4_addr16
#define s4_addr32		in4_u.u4_addr32

#define s_addr			s4_addr32
};

typedef unsigned short int sa_family_t;
typedef size_t socklen_t;

/**
 * Note that the sin_port and sin6_port are in network byte order
 * in various sockaddr* structs.
 */
struct sockaddr_in6 {
	sa_family_t		sin6_family;   /* AF_INET6               */
	uint16_t		sin6_port;     /* Port number            */
	struct in6_addr		sin6_addr;     /* IPv6 address           */
	uint8_t			sin6_scope_id; /* interfaces for a scope */
};

struct sockaddr_in6_ptr {
	sa_family_t		sin6_family;   /* AF_INET6               */
	uint16_t		sin6_port;     /* Port number            */
	struct in6_addr		*sin6_addr;    /* IPv6 address           */
	uint8_t			sin6_scope_id; /* interfaces for a scope */
};

struct sockaddr_in {
	sa_family_t		sin_family;    /* AF_INET      */
	uint16_t		sin_port;      /* Port number  */
	struct in_addr		sin_addr;      /* IPv4 address */
};

struct sockaddr_in_ptr {
	sa_family_t		sin_family;    /* AF_INET      */
	uint16_t		sin_port;      /* Port number  */
	struct in_addr		*sin_addr;     /* IPv4 address */
};

#if defined(CONFIG_NET_IPV6)
#define NET_SOCKADDR_MAX_SIZE (sizeof(struct sockaddr_in6))
#define NET_SOCKADDR_PTR_MAX_SIZE (sizeof(struct sockaddr_in6_ptr))
#elif defined(CONFIG_NET_IPV4)
#define NET_SOCKADDR_MAX_SIZE (sizeof(struct sockaddr_in))
#define NET_SOCKADDR_PTR_MAX_SIZE (sizeof(struct sockaddr_in_ptr))
#else
#if !defined(CONFIG_NET_L2_RAW_CHANNEL)
#error "Either IPv6 or IPv4 needs to be selected."
#else
#define NET_SOCKADDR_MAX_SIZE (sizeof(struct sockaddr_in6))
#define NET_SOCKADDR_PTR_MAX_SIZE (sizeof(struct sockaddr_in6_ptr))
#endif
#endif

struct sockaddr {
	sa_family_t family;
	char data[NET_SOCKADDR_MAX_SIZE - sizeof(sa_family_t)];
};

struct sockaddr_ptr {
	sa_family_t family;
	char data[NET_SOCKADDR_PTR_MAX_SIZE - sizeof(sa_family_t)];
};

struct net_addr {
	sa_family_t family;
	union {
#if defined(CONFIG_NET_IPV6)
		struct in6_addr in6_addr;
#endif
#if defined(CONFIG_NET_IPV4)
		struct in_addr in_addr;
#endif
	};
};

#define IN6ADDR_ANY_INIT { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, \
				0, 0, 0, 0, 0, 0, 0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0, 0, 0, 0, 0, 0, 0, \
				0, 0, 0, 0, 0, 0, 0, 0, 1 } } }

#define INET6_ADDRSTRLEN 46
#define NET_IPV6_ADDR_LEN sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx")
#define NET_IPV4_ADDR_LEN sizeof("xxx.xxx.xxx.xxx")

#define INADDR_ANY 0

#define NET_IPV6_MTU 1280

/** IPv6 extension headers types */
#define NET_IPV6_NEXTHDR_HBHO        0
#define NET_IPV6_NEXTHDR_DESTO       60
#define NET_IPV6_NEXTHDR_ROUTING     43
#define NET_IPV6_NEXTHDR_FRAG        44
#define NET_IPV6_NEXTHDR_NONE        59

/** IPv6/IPv4 network connection tuple */
struct net_tuple {
	/** IPv6/IPv4 remote address */
	struct net_addr *remote_addr;
	/** IPv6/IPv4 local address */
	struct net_addr *local_addr;
	/** UDP/TCP remote port */
	uint16_t remote_port;
	/** UDP/TCP local port */
	uint16_t local_port;
	/** IP protocol */
	enum net_ip_protocol ip_proto;
};

/** How the network address is assigned to network interface */
enum net_addr_type {
	NET_ADDR_ANY = 0,
	NET_ADDR_AUTOCONF,
	NET_ADDR_DHCP,
	NET_ADDR_MANUAL,
};

#if NET_DEBUG > 0
static inline char *net_addr_type2str(enum net_addr_type type)
{
	switch (type) {
	case NET_ADDR_AUTOCONF:
		return "AUTO";
	case NET_ADDR_DHCP:
		return "DHCP";
	case NET_ADDR_MANUAL:
		return "MANUAL";
	case NET_ADDR_ANY:
	default:
		break;
	}

	return "<unknown>";
}
#else
static inline char *net_addr_type2str(enum net_addr_type type)
{
	return NULL;
}
#endif

/** What is the current state of the network address */
enum net_addr_state {
	NET_ADDR_ANY_STATE = -1,
	NET_ADDR_TENTATIVE = 0,
	NET_ADDR_PREFERRED,
	NET_ADDR_DEPRECATED,
};

struct net_ipv6_hdr {
	uint8_t vtc;
	uint8_t tcflow;
	uint16_t flow;
	uint8_t len[2];
	uint8_t nexthdr;
	uint8_t hop_limit;
	struct in6_addr src;
	struct in6_addr dst;
} __packed;

struct net_ipv4_hdr {
	uint8_t vhl;
	uint8_t tos;
	uint8_t len[2];
	uint8_t id[2];
	uint8_t offset[2];
	uint8_t ttl;
	uint8_t proto;
	uint16_t chksum;
	struct in_addr src;
	struct in_addr dst;
} __packed;

struct net_icmp_hdr {
	uint8_t type;
	uint8_t code;
	uint16_t chksum;
} __packed;

struct net_udp_hdr {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t len;
	uint16_t chksum;
} __packed;

struct net_tcp_hdr {
	uint16_t src_port;
	uint16_t dst_port;
	uint8_t seq[4];
	uint8_t ack[4];
	uint8_t offset;
	uint8_t flags;
	uint8_t wnd[2];
	uint16_t chksum;
	uint8_t urg[2];
	uint8_t optdata[0];
} __packed;

#define NET_UDPH_LEN	8			/* Size of UDP header */
#define NET_TCPH_LEN	20			/* Size of TCP header */
#define NET_ICMPH_LEN	4			/* Size of ICMP header */

#define NET_IPV6H_LEN	   40			/* Size of IPv6 header */
#define NET_ICMPV6H_LEN	   NET_ICMPH_LEN	/* Size of ICMPv6 header */
#define NET_IPV6UDPH_LEN   (NET_UDPH_LEN + NET_IPV6H_LEN) /* IPv6 + UDP */
#define NET_IPV6TCPH_LEN   (NET_TCPH_LEN + NET_IPV6H_LEN) /* IPv6 + TCP */
#define NET_IPV6ICMPH_LEN  (NET_IPV6H_LEN + NET_ICMPH_LEN) /* ICMPv6 + IPv6 */
#define NET_IPV6_FRAGH_LEN 8

#define NET_IPV4H_LEN	   20			/* Size of IPv4 header */
#define NET_ICMPV4H_LEN	   NET_ICMPH_LEN	/* Size of ICMPv4 header */
#define NET_IPV4UDPH_LEN   (NET_UDPH_LEN + NET_IPV4H_LEN) /* IPv4 + UDP */
#define NET_IPV4TCPH_LEN   (NET_TCPH_LEN + NET_IPV4H_LEN) /* IPv4 + TCP */
#define NET_IPV4ICMPH_LEN  (NET_IPV4H_LEN + NET_ICMPH_LEN) /* ICMPv4 + IPv4 */

/**
 * @brief Check if the IPv6 address is a loopback address (::1).
 *
 * @param addr IPv6 address
 *
 * @return True if address is a loopback address, False otherwise.
 */
static inline bool net_is_ipv6_addr_loopback(struct in6_addr *addr)
{
	return addr->s6_addr32[0] == 0 &&
		addr->s6_addr32[1] == 0 &&
		addr->s6_addr32[2] == 0 &&
		ntohl(addr->s6_addr32[3]) == 1;
}

/**
 * @brief Check if the IPv6 address is a multicast address.
 *
 * @param addr IPv6 address
 *
 * @return True if address is multicast address, False otherwise.
 */
static inline bool net_is_ipv6_addr_mcast(const struct in6_addr *addr)
{
	return addr->s6_addr[0] == 0xFF;
}

struct net_if;

extern struct net_if_addr *net_if_ipv6_addr_lookup(const struct in6_addr *addr,
						   struct net_if **iface);

/**
 * @brief Check if IPv6 address is found in one of the network interfaces.
 *
 * @param addr IPv6 address
 *
 * @return True if address was found, False otherwise.
 */
static inline bool net_is_my_ipv6_addr(struct in6_addr *addr)
{
	return net_if_ipv6_addr_lookup(addr, NULL) != NULL;
}

extern struct net_if_mcast_addr *net_if_ipv6_maddr_lookup(const struct in6_addr *addr,
							  struct net_if **iface);

/**
 * @brief Check if IPv6 multicast address is found in one of the
 * network interfaces.
 *
 * @param maddr Multicast IPv6 address
 *
 * @return True if address was found, False otherwise.
 */
static inline bool net_is_my_ipv6_maddr(struct in6_addr *maddr)
{
	return net_if_ipv6_maddr_lookup(maddr, NULL) != NULL;
}

/**
 * @brief Check if two IPv6 addresses are same when compared after prefix mask.
 *
 * @param addr1 First IPv6 address.
 * @param addr2 Second IPv6 address.
 * @param length Prefix length (max length is 128).
 *
 * @return True if addresses are the same, False otherwise.
 */
static inline bool net_is_ipv6_prefix(const uint8_t *addr1,
				      const uint8_t *addr2,
				      uint8_t length)
{
	uint8_t bits = 128 - length;
	uint8_t bytes = bits / 8;
	uint8_t remain = bits % 8;

	if (length > 128) {
		return false;
	}

	if (memcmp(addr1, addr2, 16 - bytes)) {
		return false;
	}

	return ((addr1[16 - bytes] & ((8 - remain) << 8))
		==
		(addr2[16 - bytes] & ((8 - remain) << 8)));
}

/**
 * @brief Check if the IPv4 address is a loopback address (127.0.0.0/8).
 *
 * @param addr IPv4 address
 *
 * @return True if address is a loopback address, False otherwise.
 */
static inline bool net_is_ipv4_addr_loopback(struct in_addr *addr)
{
	return addr->s4_addr[0] == 127;
}

/**
 *  @brief Check if the IPv4 address is unspecified (all bits zero)
 *
 *  @param addr IPv4 address.
 *
 *  @return True if the address is unspecified, false otherwise.
 */
static inline bool net_is_ipv4_addr_unspecified(const struct in_addr *addr)
{
	return addr->s_addr[0] == 0;
}

/**
 * @brief Check if the IPv4 address is a multicast address.
 *
 * @param addr IPv4 address
 *
 * @return True if address is multicast address, False otherwise.
 */
static inline bool net_is_ipv4_addr_mcast(const struct in_addr *addr)
{
	return (addr->s_addr[0] & 0xE0000000) == 0xE0000000;
}

extern struct net_if_addr *net_if_ipv4_addr_lookup(const struct in_addr *addr,
						   struct net_if **iface);

/**
 * @brief Check if the IPv4 address is assigned to any network interface
 * in the system.
 *
 * @param addr A valid pointer on an IPv4 address
 *
 * @return True if IPv4 address is found in one of the network interfaces,
 * False otherwise.
 */
static inline bool net_is_my_ipv4_addr(const struct in_addr *addr)
{
	return net_if_ipv4_addr_lookup(addr, NULL) != NULL;
}

/**
 *  @def net_ipaddr_copy
 *  @brief Copy an IPv4 or IPv6 address
 *
 *  @param dest Destination IP address.
 *  @param src Source IP address.
 *
 *  @return Destination address.
 */
#define net_ipaddr_copy(dest, src) \
	UNALIGNED_PUT(UNALIGNED_GET(src), dest)

/**
 *  @brief Compare two IPv4 addresses
 *
 *  @param addr1 Pointer to IPv4 address.
 *  @param addr2 Pointer to IPv4 address.
 *
 *  @return True if the addresses are the same, false otherwise.
 */
static inline bool net_ipv4_addr_cmp(const struct in_addr *addr1,
				     const struct in_addr *addr2)
{
	return addr1->s_addr[0] == addr2->s_addr[0];
}

/**
 *  @brief Compare two IPv6 addresses
 *
 *  @param addr1 Pointer to IPv6 address.
 *  @param addr2 Pointer to IPv6 address.
 *
 *  @return True if the addresses are the same, false otherwise.
 */
static inline bool net_ipv6_addr_cmp(const struct in6_addr *addr1,
				     const struct in6_addr *addr2)
{
	return !memcmp(addr1, addr2, sizeof(struct in6_addr));
}

/**
 * @brief Check if the given IPv6 address is a link local address.
 *
 * @param addr A valid pointer on an IPv6 address
 *
 * @return True if it is, false otherwise.
 */
static inline bool net_is_ipv6_ll_addr(const struct in6_addr *addr)
{
	return ((addr->s6_addr[0]) == 0xFE) &&
		((addr->s6_addr[1]) == 0x80);
}

/**
 * @brief Return pointer to any (all bits zeros) IPv6 address.
 *
 * @return Any IPv6 address.
 */
const struct in6_addr *net_ipv6_unspecified_address(void);

/**
 * @brief Return pointer to any (all bits zeros) IPv4 address.
 *
 * @return Any IPv4 address.
 */
const struct in_addr *net_ipv4_unspecified_address(void);

/**
 * @brief Return pointer to broadcast (all bits ones) IPv4 address.
 *
 * @return Broadcast IPv4 address.
 */
const struct in_addr *net_ipv4_broadcast_address(void);

struct net_if;
extern bool net_if_ipv4_addr_mask_cmp(struct net_if *iface,
				      struct in_addr *addr);

/**
 * @brief Check if the given address belongs to same subnet that
 * has been configured for the interface.
 *
 * @param iface A valid pointer on an interface
 * @param addr pointer on an address
 *
 * @return True if address is in same subnet, false otherwise.
 */
static inline bool net_ipv4_addr_mask_cmp(struct net_if *iface,
					  struct in_addr *addr)
{
	return net_if_ipv4_addr_mask_cmp(iface, addr);
}

/**
 *  @brief Check if the IPv6 address is unspecified (all bits zero)
 *
 *  @param addr IPv6 address.
 *
 *  @return True if the address is unspecified, false otherwise.
 */
static inline bool net_is_ipv6_addr_unspecified(const struct in6_addr *addr)
{
	return addr->s6_addr32[0] == 0 && addr->s6_addr32[1] == 0 &&
		addr->s6_addr32[2] == 0 && addr->s6_addr32[3] == 0;
}

/**
 *  @brief Check if the IPv6 address is solicited node multicast address
 *  FF02:0:0:0:0:1:FFXX:XXXX defined in RFC 3513
 *
 *  @param addr IPv6 address.
 *
 *  @return True if the address is solicited node address, false otherwise.
 */
static inline bool net_is_ipv6_addr_solicited_node(const struct in6_addr *addr)
{
	return addr->s6_addr32[0] == htonl(0xff020000) &&
		addr->s6_addr32[1] == 0x00000000 &&
		addr->s6_addr32[2] == htonl(0x00000001) &&
		((addr->s6_addr32[3] & htonl(0xff000000)) == htonl(0xff000000));
}

/**
 * @brief Check if the IPv6 address is a global multicast address (FFxE::/16).
 *
 * @param addr IPv6 address.
 *
 * @return True if the address is global multicast address, false otherwise.
*/
static inline bool net_is_ipv6_addr_mcast_global(const struct in6_addr *addr)
{
	return addr->s6_addr[0] == 0xff &&
		(addr->s6_addr[1] & 0x0e) == 0x0e;
}

/**
 *  @brief Create solicited node IPv6 multicast address
 *  FF02:0:0:0:0:1:FFXX:XXXX defined in RFC 3513
 *
 *  @param src IPv6 address.
 *  @param dst IPv6 address.
 */
static inline void net_ipv6_addr_create_solicited_node(struct in6_addr *src,
						       struct in6_addr *dst)
{
	dst->s6_addr[0]   = 0xFF;
	dst->s6_addr[1]   = 0x02;
	dst->s6_addr16[1] = 0;
	dst->s6_addr16[2] = 0;
	dst->s6_addr16[3] = 0;
	dst->s6_addr16[4] = 0;
	dst->s6_addr[10]  = 0;
	dst->s6_addr[11]  = 0x01;
	dst->s6_addr[12]  = 0xFF;
	dst->s6_addr[13]  = src->s6_addr[13];
	dst->s6_addr16[7] = src->s6_addr16[7];
}

/** @brief Construct an IPv6 address from eight 16-bit words.
 *
 *  @param addr IPv6 address
 *  @param addr0 16-bit word which is part of the address
 *  @param addr1 16-bit word which is part of the address
 *  @param addr2 16-bit word which is part of the address
 *  @param addr3 16-bit word which is part of the address
 *  @param addr4 16-bit word which is part of the address
 *  @param addr5 16-bit word which is part of the address
 *  @param addr6 16-bit word which is part of the address
 *  @param addr7 16-bit word which is part of the address
 */
static inline void net_ipv6_addr_create(struct in6_addr *addr,
					uint16_t addr0, uint16_t addr1,
					uint16_t addr2, uint16_t addr3,
					uint16_t addr4, uint16_t addr5,
					uint16_t addr6, uint16_t addr7)
{
	addr->s6_addr16[0] = htons(addr0);
	addr->s6_addr16[1] = htons(addr1);
	addr->s6_addr16[2] = htons(addr2);
	addr->s6_addr16[3] = htons(addr3);
	addr->s6_addr16[4] = htons(addr4);
	addr->s6_addr16[5] = htons(addr5);
	addr->s6_addr16[6] = htons(addr6);
	addr->s6_addr16[7] = htons(addr7);
}

/**
 *  @brief Create link local allnodes multicast IPv6 address
 *
 *  @param addr IPv6 address
 */
static inline void net_ipv6_addr_create_ll_allnodes_mcast(struct in6_addr *addr)
{
	net_ipv6_addr_create(addr, 0xff02, 0, 0, 0, 0, 0, 0, 0x0001);
}

/**
 *  @brief Create IPv6 address interface identifier
 *
 *  @param addr IPv6 address
 *  @param lladdr Link local address
 */
static inline void net_ipv6_addr_create_iid(struct in6_addr *addr,
					    struct net_linkaddr *lladdr)
{
	addr->s6_addr[0] = 0xfe;
	addr->s6_addr[1] = 0x80;
	addr->s6_addr16[1] = 0;
	addr->s6_addr32[1] = 0;

	switch (lladdr->len) {
	case 2:
		addr->s6_addr32[2] = 0;
		addr->s6_addr[11] = 0xff;
		addr->s6_addr[12] = 0xfe;
		addr->s6_addr[13] = 0;
		addr->s6_addr[14] = lladdr->addr[0];
		addr->s6_addr[15] = lladdr->addr[1];
		addr->s6_addr[8] ^= 0x02;
		break;
	case 6:
		memcpy(&addr->s6_addr[8], lladdr->addr, 3);
		addr->s6_addr[11] = 0xff;
		addr->s6_addr[12] = 0xfe;
		memcpy(&addr->s6_addr[13], lladdr->addr + 3, 3);
		addr->s6_addr[8] ^= 0x02;
		break;

	case 8:
		memcpy(&addr->s6_addr[8], lladdr->addr, lladdr->len);
		addr->s6_addr[8] ^= 0x02;
		break;
	}
}

/**
 *  @brief Check if given address is based on link layer address
 *
 *  @return True if it is, False otherwise
 */
static inline bool net_ipv6_addr_based_on_ll(const struct in6_addr *addr,
					     const struct net_linkaddr *lladdr)
{
	switch (lladdr->len) {
	case 2:
		if (!memcmp(&addr->s6_addr[14], lladdr->addr, lladdr->len) &&
		    (addr->s6_addr[8] ^ 0x02) == lladdr->addr[0] &&
		    addr->s6_addr[11] == 0xff &&
		    addr->s6_addr[12] == 0xfe) {
			return true;
		}

		break;
	case 6:
		if (!memcmp(&addr->s6_addr[9], &lladdr->addr[1], 2) &&
		    !memcmp(&addr->s6_addr[13], &lladdr->addr[3], 3) &&
		    addr->s6_addr[11] == 0xff &&
		    addr->s6_addr[12] == 0xfe &&
		    (addr->s6_addr[8] ^ 0x02) == lladdr->addr[0]) {
			return true;
		}

		break;
	case 8:
		if (!memcmp(&addr->s6_addr[9], &lladdr->addr[1],
			    lladdr->len - 1) &&
		    (addr->s6_addr[8] ^ 0x02) == lladdr->addr[0]) {
			return true;
		}

		break;
	}

	return false;
}

/**
 * @brief Get sockaddr_in6 from sockaddr. This is a helper so that
 * the code calling this function can be made shorter.
 *
 * @param addr Socket address
 *
 * @return Pointer to IPv6 socket address
 */
static inline struct sockaddr_in6 *net_sin6(const struct sockaddr *addr)
{
	return (struct sockaddr_in6 *)addr;
}

/**
 * @brief Get sockaddr_in from sockaddr. This is a helper so that
 * the code calling this function can be made shorter.
 *
 * @param addr Socket address
 *
 * @return Pointer to IPv4 socket address
 */
static inline struct sockaddr_in *net_sin(const struct sockaddr *addr)
{
	return (struct sockaddr_in *)addr;
}

/**
 * @brief Get sockaddr_in6_ptr from sockaddr_ptr. This is a helper so that
 * the code calling this function can be made shorter.
 *
 * @param addr Socket address
 *
 * @return Pointer to IPv6 socket address
 */
static inline
struct sockaddr_in6_ptr *net_sin6_ptr(const struct sockaddr_ptr *addr)
{
	return (struct sockaddr_in6_ptr *)addr;
}

/**
 * @brief Get sockaddr_in_ptr from sockaddr_ptr. This is a helper so that
 * the code calling this function can be made shorter.
 *
 * @param addr Socket address
 *
 * @return Pointer to IPv4 socket address
 */
static inline
struct sockaddr_in_ptr *net_sin_ptr(const struct sockaddr_ptr *addr)
{
	return (struct sockaddr_in_ptr *)addr;
}

/**
 * @brief Convert a string to IP address.
 *
 * @param family IP address family (AF_INET or AF_INET6)
 * @param src IP address in a null terminated string
 * @param dst Pointer to struct in_addr if family is AF_INET or
 * pointer to struct in6_addr if family is AF_INET6
 *
 * @note This function doesn't do precise error checking,
 * do not use for untrusted strings.
 *
 * @return 0 if ok, < 0 if error
 */
int net_addr_pton(sa_family_t family, const char *src,
		  struct sockaddr *dst);

#ifdef __cplusplus
}
#endif

#endif /* __NET_IP_H */
