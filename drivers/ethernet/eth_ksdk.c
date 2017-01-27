/* KSDK Ethernet Driver
 *
 *  Copyright (c) 2016 ARM Ltd
 *  Copyright (c) 2016 Linaro Ltd
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

/* The driver performs one shot PHY setup.  There is no support for
 * PHY disconnect, reconnect or configuration change.  The PHY setup,
 * implemented via KSDK contains polled code that can block the
 * initialization thread for a few seconds.
 *
 * There is no statistics collection for either normal operation or
 * error behaviour.
 */

#define SYS_LOG_DOMAIN "dev/eth_ksdk"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <misc/sys_log.h>

#include <board.h>
#include <device.h>
#include <misc/util.h>
#include <kernel.h>
#include <net/nbuf.h>
#include <net/net_if.h>

#include "fsl_enet.h"
#include "fsl_phy.h"
#include "fsl_port.h"

struct eth_context {
	struct net_if *iface;
	enet_handle_t enet_handle;
	struct nano_sem tx_buf_sem;
	uint8_t mac_addr[6];
	/* TODO: FIXME. This Ethernet frame sized buffer is used for
	 * interfacing with KSDK. How it works is that hardware uses
	 * DMA scatter buffers to receive a frame, and then public
	 * KSDK call gathers them into this buffer (there's no other
	 * public interface). All this happens only for this driver
	 * to scatter this buffer again into Zephyr fragment buffers.
	 * This is not efficient, but proper resolution of this issue
	 * depends on introduction of zero-copy networking support
	 * in Zephyr, and adding needed interface to KSDK (or
	 * bypassing it and writing a more complex driver working
	 * directly with hardware).
	 */
	uint8_t frame_buf[1500];
};

static void eth_0_config_func(void);

static enet_rx_bd_struct_t __aligned(ENET_BUFF_ALIGNMENT)
rx_buffer_desc[CONFIG_ETH_KSDK_TX_BUFFERS];

static enet_tx_bd_struct_t __aligned(ENET_BUFF_ALIGNMENT)
tx_buffer_desc[CONFIG_ETH_KSDK_TX_BUFFERS];

/* Use ENET_FRAME_MAX_VALNFRAMELEN for VLAN frame size
 * Use ENET_FRAME_MAX_FRAMELEN for ethernet frame size
 */
#define ETH_KSDK_BUFFER_SIZE \
	ROUND_UP(ENET_FRAME_MAX_VALNFRAMELEN, ENET_BUFF_ALIGNMENT)

static uint8_t __aligned(ENET_BUFF_ALIGNMENT)
rx_buffer[CONFIG_ETH_KSDK_RX_BUFFERS][ETH_KSDK_BUFFER_SIZE];

static uint8_t __aligned(ENET_BUFF_ALIGNMENT)
tx_buffer[CONFIG_ETH_KSDK_TX_BUFFERS][ETH_KSDK_BUFFER_SIZE];

static int eth_tx(struct net_if *iface, struct net_buf *buf)
{
	struct eth_context *context = iface->dev->driver_data;
	const struct net_buf *frag;
	uint8_t *dst;
	status_t status;
	unsigned int imask;

	uint16_t total_len = net_nbuf_ll_reserve(buf) + net_buf_frags_len(buf);

	k_sem_take(&context->tx_buf_sem, K_FOREVER);

	/* As context->frame_buf is shared resource used by both eth_tx
	 * and eth_rx, we need to protect it with irq_lock.
	 */
	imask = irq_lock();

	/* Gather fragment buffers into flat Ethernet frame buffer
	 * which can be fed to KSDK Ethernet functions. First
	 * fragment is special - it contains link layer (Ethernet
	 * in our case) headers and must be treated specially.
	 */
	dst = context->frame_buf;
	memcpy(dst, net_nbuf_ll(buf),
	       net_nbuf_ll_reserve(buf) + buf->frags->len);
	dst += net_nbuf_ll_reserve(buf) + buf->frags->len;

	/* Continue with the rest of fragments (which contain only data) */
	frag = buf->frags->frags;
	while (frag) {
		memcpy(dst, frag->data, frag->len);
		dst += frag->len;
		frag = frag->frags;
	}

	status = ENET_SendFrame(ENET, &context->enet_handle, context->frame_buf,
				total_len);

	irq_unlock(imask);

	if (status) {
		SYS_LOG_ERR("ENET_SendFrame error: %d\n", status);
		return -1;
	}

	net_nbuf_unref(buf);
	return 0;
}

static void eth_rx(struct device *iface)
{
	struct eth_context *context = iface->driver_data;
	struct net_buf *buf, *prev_frag;
	const uint8_t *src;
	uint32_t frame_length = 0;
	status_t status;
	unsigned int imask;

	status = ENET_GetRxFrameSize(&context->enet_handle, &frame_length);
	if (status) {
		enet_data_error_stats_t error_stats;

		SYS_LOG_ERR("ENET_GetRxFrameSize return: %d", status);

		ENET_GetRxErrBeforeReadFrame(&context->enet_handle,
					     &error_stats);
		/* Flush the current read buffer.  This operation can
		 * only report failure if there is no frame to flush,
		 * which cannot happen in this context.
		 */
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	buf = net_nbuf_get_reserve_rx(0);
	if (!buf) {
		/* We failed to get a receive buffer.  We don't add
		 * any further logging here because the allocator
		 * issued a diagnostic when it failed to allocate.
		 *
		 * Flush the current read buffer.  This operation can
		 * only report failure if there is no frame to flush,
		 * which cannot happen in this context.
		 */
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	if (sizeof(context->frame_buf) < frame_length) {
		SYS_LOG_ERR("frame too large (%d)\n", frame_length);
		net_buf_unref(buf);
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	/* As context->frame_buf is shared resource used by both eth_tx
	 * and eth_rx, we need to protect it with irq_lock.
	 */
	imask = irq_lock();

	status = ENET_ReadFrame(ENET, &context->enet_handle,
				context->frame_buf, frame_length);
	if (status) {
		irq_unlock(imask);
		SYS_LOG_ERR("ENET_ReadFrame failed: %d\n", status);
		net_buf_unref(buf);
		return;
	}

	src = context->frame_buf;
	prev_frag = buf;
	do {
		struct net_buf *pkt_buf;
		size_t frag_len;

		pkt_buf = net_nbuf_get_reserve_data(0);
		if (!pkt_buf) {
			irq_unlock(imask);
			SYS_LOG_ERR("Failed to get fragment buf\n");
			net_buf_unref(buf);
			assert(status == kStatus_Success);
			return;
		}

		net_buf_frag_insert(prev_frag, pkt_buf);
		prev_frag = pkt_buf;
		frag_len = net_buf_tailroom(pkt_buf);
		if (frag_len > frame_length) {
			frag_len = frame_length;
		}

		memcpy(pkt_buf->data, src, frag_len);
		net_buf_add(pkt_buf, frag_len);
		src += frag_len;
		frame_length -= frag_len;
	} while (frame_length > 0);

	irq_unlock(imask);

	net_recv_data(context->iface, buf);
}

static void eth_callback(ENET_Type *base, enet_handle_t *handle,
			 enet_event_t event, void *param)
{
	struct device *iface = param;
	struct eth_context *context = iface->driver_data;

	switch (event) {
	case kENET_RxEvent:
		eth_rx(iface);
		break;
	case kENET_TxEvent:
		/* Free the TX buffer. */
		k_sem_give(&context->tx_buf_sem);
		break;
	case kENET_ErrEvent:
		/* Error event: BABR/BABT/EBERR/LC/RL/UN/PLR.  */
		break;
	case kENET_WakeUpEvent:
		/* Wake up from sleep mode event. */
		break;
#ifdef ENET_ENHANCEDBUFFERDESCRIPTOR_MODE
	case kENET_TimeStampEvent:
		/* Time stamp event.  */
		break;
	case kENET_TimeStampAvailEvent:
		/* Time stamp available event.  */
		break;
#endif
	}
}

#if defined(CONFIG_ETH_KSDK_0_RANDOM_MAC)
static void generate_mac(uint8_t *mac_addr)
{
	uint32_t entropy;

	entropy = sys_rand32_get();

	/* Locally administered, unicast */
	mac_addr[0] = ((entropy >> 0) & 0xfc) | 0x02;

	mac_addr[1] = entropy >> 8;
	mac_addr[2] = entropy >> 16;
	mac_addr[3] = entropy >> 24;

	entropy = sys_rand32_get();

	mac_addr[4] = entropy >> 0;
	mac_addr[5] = entropy >> 8;
}
#endif

static int eth_0_init(struct device *dev)
{
	struct eth_context *context = dev->driver_data;
	enet_config_t enet_config;
	uint32_t sys_clock;
	const uint32_t phy_addr = 0x0;
	bool link;
	status_t status;
	enet_buffer_config_t buffer_config = {
		.rxBdNumber = CONFIG_ETH_KSDK_RX_BUFFERS,
		.txBdNumber = CONFIG_ETH_KSDK_TX_BUFFERS,
		.rxBuffSizeAlign = ETH_KSDK_BUFFER_SIZE,
		.txBuffSizeAlign = ETH_KSDK_BUFFER_SIZE,
		.rxBdStartAddrAlign = rx_buffer_desc,
		.txBdStartAddrAlign = tx_buffer_desc,
		.rxBufferAlign = rx_buffer[0],
		.txBufferAlign = tx_buffer[0],
	};

	k_sem_init(&context->tx_buf_sem, 0, UINT_MAX);
	for (int i = 0; i < CONFIG_ETH_KSDK_TX_BUFFERS; i++) {
		k_sem_give(&context->tx_buf_sem);
	}

	sys_clock = CLOCK_GetFreq(kCLOCK_CoreSysClk);

	ENET_GetDefaultConfig(&enet_config);
	enet_config.interrupt |= kENET_RxFrameInterrupt;
	enet_config.interrupt |= kENET_TxFrameInterrupt;

	status = PHY_Init(ENET, phy_addr, sys_clock);
	if (status) {
		SYS_LOG_ERR("PHY_Init() failed: %d", status);
		return 1;
	}

	PHY_GetLinkStatus(ENET, phy_addr, &link);
	if (link) {
		phy_speed_t phy_speed;
		phy_duplex_t phy_duplex;

		PHY_GetLinkSpeedDuplex(ENET, phy_addr, &phy_speed, &phy_duplex);
		enet_config.miiSpeed = (enet_mii_speed_t) phy_speed;
		enet_config.miiDuplex = (enet_mii_duplex_t) phy_duplex;

		SYS_LOG_INF("Enabled %dM %s-duplex mode.",
			    (phy_speed ? 100 : 10),
			    (phy_duplex ? "full" : "half"));
	} else {
		SYS_LOG_INF("Link down.");
	}

#if defined(CONFIG_ETH_KSDK_0_RANDOM_MAC)
	generate_mac(context->mac_addr);
#endif

	ENET_Init(ENET,
		  &context->enet_handle,
		  &enet_config,
		  &buffer_config,
		  context->mac_addr,
		  sys_clock);

	SYS_LOG_DBG("MAC %02x:%02x:%02x:%02x:%02x:%02x",
		    context->mac_addr[0], context->mac_addr[1],
		    context->mac_addr[2], context->mac_addr[3],
		    context->mac_addr[4], context->mac_addr[5]);

	ENET_SetCallback(&context->enet_handle, eth_callback, dev);
	eth_0_config_func();
	ENET_ActiveRead(ENET);
	return 0;
}

static void eth_0_iface_init(struct net_if *iface)
{
	struct device *dev = net_if_get_device(iface);
	struct eth_context *context = dev->driver_data;

	net_if_set_link_addr(iface, context->mac_addr,
			     sizeof(context->mac_addr));
	context->iface = iface;
}

static struct net_if_api api_funcs_0 = {
	.init	= eth_0_iface_init,
	.send	= eth_tx,
};

static void eth_ksdk_rx_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;

	ENET_ReceiveIRQHandler(ENET, &context->enet_handle);
}

static void eth_ksdk_tx_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;

	ENET_TransmitIRQHandler(ENET, &context->enet_handle);
}

static void eth_ksdk_error_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;

	ENET_ErrorIRQHandler(ENET, &context->enet_handle);
}

static struct eth_context eth_0_context = {
#if !defined(CONFIG_ETH_KSDK_0_RANDOM_MAC)
	.mac_addr = {
		CONFIG_ETH_KSDK_0_MAC0,
		CONFIG_ETH_KSDK_0_MAC1,
		CONFIG_ETH_KSDK_0_MAC2,
		CONFIG_ETH_KSDK_0_MAC3,
		CONFIG_ETH_KSDK_0_MAC4,
		CONFIG_ETH_KSDK_0_MAC5
	}
#endif
};

#ifdef CONFIG_NET_L2_ETHERNET
#define _ETH_L2_LAYER ETHERNET_L2
#define _ETH_L2_CTX_TYPE NET_L2_GET_CTX_TYPE(ETHERNET_L2)
#endif

NET_DEVICE_INIT(eth_ksdk_0, CONFIG_ETH_KSDK_0_NAME,
		eth_0_init, &eth_0_context,
		NULL,
		CONFIG_ETH_INIT_PRIORITY,
		&api_funcs_0, _ETH_L2_LAYER, _ETH_L2_CTX_TYPE, 1500);

static void eth_0_config_func(void)
{
	IRQ_CONNECT(IRQ_ETH_RX, CONFIG_ETH_KSDK_0_IRQ_PRI,
		    eth_ksdk_rx_isr, DEVICE_GET(eth_ksdk_0), 0);
	irq_enable(IRQ_ETH_RX);

	IRQ_CONNECT(IRQ_ETH_TX, CONFIG_ETH_KSDK_0_IRQ_PRI,
		    eth_ksdk_tx_isr, DEVICE_GET(eth_ksdk_0), 0);
	irq_enable(IRQ_ETH_TX);

	IRQ_CONNECT(IRQ_ETH_ERR_MISC, CONFIG_ETH_KSDK_0_IRQ_PRI,
		    eth_ksdk_error_isr, DEVICE_GET(eth_ksdk_0), 0);
	irq_enable(IRQ_ETH_ERR_MISC);
}
