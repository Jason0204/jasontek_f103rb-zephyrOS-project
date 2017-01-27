/*
 * Copyright (c) 2016 Intel Corporation.
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

/**
 * @file
 * @brief Public API for offloading IP stack
 */

#ifndef __OFFLOAD_IP_H__
#define __OFFLOAD_IP_H__

#if defined(CONFIG_NET_L2_OFFLOAD_IP)

#include <net/buf.h>
#include <net/net_ip.h>
#include <net/net_context.h>

#ifdef __cplusplus
extern "C" {
#endif

/** For return parameters and return values of the elements in this
 * struct, see similarly named functions in net_context.h
 */
struct net_l2_offload_ip {
	/**
	 * This function is called when the socket is to be opened.
	 */
	int (*get)(sa_family_t family,
		   enum net_sock_type type,
		   enum net_ip_protocol ip_proto,
		   struct net_context **context);

	/**
	 * This function is called when user wants to bind to local IP address.
	 */
	int (*bind)(struct net_context *context,
		    const struct sockaddr *addr,
		    socklen_t addrlen);

	/**
	 * This function is called when user wants to mark the socket
	 * to be a listening one.
	 */
	int (*listen)(struct net_context *context, int backlog);

	/**
	 * This function is called when user wants to create a connection
	 * to a peer host.
	 */
	int (*connect)(struct net_context *context,
		       const struct sockaddr *addr,
		       socklen_t addrlen,
		       net_context_connect_cb_t cb,
		       int32_t timeout,
		       void *user_data);

	/**
	 * This function is called when user wants to accept a connection
	 * being established.
	 */
	int (*accept)(struct net_context *context,
		      net_context_accept_cb_t cb,
		      int32_t timeout,
		      void *user_data);

	/**
	 * This function is called when user wants to send data to peer host.
	 */
	int (*send)(struct net_buf *buf,
		    net_context_send_cb_t cb,
		    int32_t timeout,
		    void *token,
		    void *user_data);

	/**
	 * This function is called when user wants to send data to peer host.
	 */
	int (*sendto)(struct net_buf *buf,
		      const struct sockaddr *dst_addr,
		      socklen_t addrlen,
		      net_context_send_cb_t cb,
		      int32_t timeout,
		      void *token,
		      void *user_data);

	/**
	 * This function is called when user wants to receive data from peer
	 * host.
	 */
	int (*recv)(struct net_context *context,
		    net_context_recv_cb_t cb,
		    int32_t timeout,
		    void *user_data);

	/**
	 * This function is called when user wants to close the socket.
	 */
	int (*put)(struct net_context *context);
};

/**
 * @brief Get a network socket/context from the offloaded IP stack.
 *
 * @details Network socket is used to define the connection
 * 5-tuple (protocol, remote address, remote port, source
 * address and source port). This is similar as BSD socket()
 * function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param family IP address family (AF_INET or AF_INET6)
 * @param type Type of the socket, SOCK_STREAM or SOCK_DGRAM
 * @param ip_proto IP protocol, IPPROTO_UDP or IPPROTO_TCP
 * @param context The allocated context is returned to the caller.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_get(struct net_if *iface,
					sa_family_t family,
					enum net_sock_type type,
					enum net_ip_protocol ip_proto,
					struct net_context **context)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->get);

	return iface->l2->offload_ip->get(family, type, ip_proto, context);
}

/**
 * @brief Assign a socket a local address.
 *
 * @details This is similar as BSD bind() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param context The context to be assigned.
 * @param addr Address to assigned.
 * @param addrlen Length of the address.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_bind(struct net_if *iface,
					 struct net_context *context,
					 const struct sockaddr *addr,
					 socklen_t addrlen)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->bind);

	return iface->l2->offload_ip->bind(context, addr, addrlen);
}

/**
 * @brief Mark the context as a listening one.
 *
 * @details This is similar as BSD listen() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param context The context to use.
 * @param backlog The size of the pending connections backlog.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_listen(struct net_if *iface,
					   struct net_context *context,
					   int backlog)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->listen);

	return iface->l2->offload_ip->listen(context, backlog);
}

/**
 * @brief            Create a network connection.
 *
 * @details          The net_context_connect function creates a network
 *                   connection to the host specified by addr. After the
 *                   connection is established, the user supplied callback (cb)
 *                   is executed. cb is called even if the timeout was set to
 *                   K_FOREVER. cb is not called if the timeout expires.
 *                   For datagram sockets (SOCK_DGRAM), this function only sets
 *                   the peer address.
 *                   This function is similar to the BSD connect() function.
 *
 * @param iface      Network interface where the offloaded IP stack can be
 *                   reached.
 * @param context    The network context.
 * @param addr       The peer address to connect to.
 * @param addrlen    Peer address length.
 * @param cb         Callback function. Set to NULL if not required.
 * @param timeout    The timeout value for the connection. Possible values:
 *                   * K_NO_WAIT: this function will return immediately,
 *                   * K_FOREVER: this function will block until the
 *                                      connection is established,
 *                   * >0: this function will wait the specified ms.
 * @param user_data  Data passed to the callback function.
 *
 * @return           0 on success.
 * @return           -EINVAL if an invalid parameter is passed as an argument.
 * @return           -ENOTSUP if the operation is not supported or implemented.
 */
static inline int net_l2_offload_ip_connect(struct net_if *iface,
					    struct net_context *context,
					    const struct sockaddr *addr,
					    socklen_t addrlen,
					    net_context_connect_cb_t cb,
					    int32_t timeout,
					    void *user_data)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->connect);

	return iface->l2->offload_ip->connect(context, addr, addrlen, cb,
					      timeout, user_data);
}

/**
 * @brief Accept a network connection attempt.
 *
 * @details Accept a connection being established. This function
 * will return immediately if the timeout is set to K_NO_WAIT.
 * In this case the context will call the supplied callback when ever
 * there is a connection established to this context. This is "a register
 * handler and forget" type of call (async).
 * If the timeout is set to K_FOREVER, the function will wait
 * until the connection is established. Timeout value > 0, will wait as
 * many ms.
 * After the connection is established a caller supplied callback is called.
 * The callback is called even if timeout was set to K_FOREVER, the
 * callback is called before this function will return in this case.
 * The callback is not called if the timeout expires.
 * This is similar as BSD accept() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param context The context to use.
 * @param cb Caller supplied callback function.
 * @param timeout Timeout for the connection. Possible values
 * are K_FOREVER, K_NO_WAIT, >0.
 * @param user_data Caller supplied user data.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_accept(struct net_if *iface,
					   struct net_context *context,
					   net_context_accept_cb_t cb,
					   int32_t timeout,
					   void *user_data)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->accept);

	return iface->l2->offload_ip->accept(context, cb, timeout, user_data);
}

/**
 * @brief Send a network buffer to a peer.
 *
 * @details This function can be used to send network data to a peer
 * connection. This function will return immediately if the timeout
 * is set to K_NO_WAIT. If the timeout is set to K_FOREVER, the function
 * will wait until the network buffer is sent. Timeout value > 0 will
 * wait as many ms. After the network buffer is sent,
 * a caller supplied callback is called. The callback is called even
 * if timeout was set to K_FOREVER, the callback is called
 * before this function will return in this case. The callback is not
 * called if the timeout expires. For context of type SOCK_DGRAM,
 * the destination address must have been set by the call to
 * net_context_connect().
 * This is similar as BSD send() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param buf The network buffer to send.
 * @param cb Caller supplied callback function.
 * @param timeout Timeout for the connection. Possible values
 * are K_FOREVER, K_NO_WAIT, >0.
 * @param token Caller specified value that is passed as is to callback.
 * @param user_data Caller supplied user data.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_send(struct net_if *iface,
					 struct net_buf *buf,
					 net_context_send_cb_t cb,
					 int32_t timeout,
					 void *token,
					 void *user_data)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->send);

	return iface->l2->offload_ip->send(buf, cb, timeout, token, user_data);
}

/**
 * @brief Send a network buffer to a peer specified by address.
 *
 * @details This function can be used to send network data to a peer
 * specified by address. This variant can only be used for datagram
 * connections of type SOCK_DGRAM. This function will return immediately
 * if the timeout is set to K_NO_WAIT. If the timeout is set to K_FOREVER,
 * the function will wait until the network buffer is sent. Timeout
 * value > 0 will wait as many ms. After the network buffer
 * is sent, a caller supplied callback is called. The callback is called
 * even if timeout was set to K_FOREVER, the callback is called
 * before this function will return. The callback is not called if the
 * timeout expires.
 * This is similar as BSD sendto() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param buf The network buffer to send.
 * @param dst_addr Destination address. This will override the address
 * already set in network buffer.
 * @param addrlen Length of the address.
 * @param cb Caller supplied callback function.
 * @param timeout Timeout for the connection. Possible values
 * are K_FOREVER, K_NO_WAIT, >0.
 * @param token Caller specified value that is passed as is to callback.
 * @param user_data Caller supplied user data.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_sendto(struct net_if *iface,
					   struct net_buf *buf,
					   const struct sockaddr *dst_addr,
					   socklen_t addrlen,
					   net_context_send_cb_t cb,
					   int32_t timeout,
					   void *token,
					   void *user_data)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->sendto);

	return iface->l2->offload_ip->sendto(buf, dst_addr, addrlen, cb,
					     timeout, token, user_data);
}

/**
 * @brief Receive network data from a peer specified by context.
 *
 * @details This function can be used to register a callback function
 * that is called by the network stack when network data has been received
 * for this context. As this function registers a callback, then there
 * is no need to call this function multiple times if timeout is set to
 * K_NO_WAIT.
 * If callback function or user data changes, then the function can be called
 * multiple times to register new values.
 * This function will return immediately if the timeout is set to K_NO_WAIT.
 * If the timeout is set to K_FOREVER, the function will wait until the
 * network buffer is received. Timeout value > 0 will wait as many ms.
 * After the network buffer is received, a caller supplied callback is
 * called. The callback is called even if timeout was set to K_FOREVER,
 * the callback is called before this function will return in this case.
 * The callback is not called if the timeout expires. The timeout functionality
 * can be compiled out if synchronous behaviour is not needed. The sync call
 * logic requires some memory that can be saved if only async way of call is
 * used. If CONFIG_NET_CONTEXT_SYNC_RECV is not set, then the timeout parameter
 * value is ignored.
 * This is similar as BSD recv() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param context The network context to use.
 * @param cb Caller supplied callback function.
 * @param timeout Caller supplied timeout. Possible values
 * are K_FOREVER, K_NO_WAIT, >0.
 * @param user_data Caller supplied user data.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_recv(struct net_if *iface,
					 struct net_context *context,
					 net_context_recv_cb_t cb,
					 int32_t timeout,
					 void *user_data)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->recv);

	return iface->l2->offload_ip->recv(context, cb, timeout, user_data);
}

/**
 * @brief Free/close a network context.
 *
 * @details This releases the context. It is not possible to
 * send or receive data via this context after this call.
 * This is similar as BSD shutdown() function.
 *
 * @param iface Network interface where the offloaded IP stack can be
 * reached.
 * @param context The context to be closed.
 *
 * @return 0 if ok, < 0 if error
 */
static inline int net_l2_offload_ip_put(struct net_if *iface,
					struct net_context *context)
{
	NET_ASSERT(iface);
	NET_ASSERT(iface->l2);
	NET_ASSERT(iface->l2->offload_ip);
	NET_ASSERT(iface->l2->offload_ip->put);

	return iface->l2->offload_ip->put(context);
}

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_NET_L2_OFFLOAD_IP */

#endif /* __OFFLOAD_IP_H__ */
