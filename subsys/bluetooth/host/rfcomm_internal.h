/** @file
 *  @brief Internal APIs for Bluetooth RFCOMM handling.
 */

/*
 * Copyright (c) 2015-2016 Intel Corporation
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

#include <bluetooth/rfcomm.h>

/* RFCOMM signalling connection specific context */
struct bt_rfcomm_session {
	/* L2CAP channel this context is associated with */
	struct bt_l2cap_br_chan br_chan;
	struct bt_rfcomm_dlc *dlcs;
	uint16_t mtu;
	uint8_t state;
	bt_rfcomm_role_t role;
};

enum {
	BT_RFCOMM_STATE_IDLE,
	BT_RFCOMM_STATE_INIT,
	BT_RFCOMM_STATE_SECURITY_PENDING,
	BT_RFCOMM_STATE_CONNECTING,
	BT_RFCOMM_STATE_CONNECTED,
	BT_RFCOMM_STATE_CONFIG,
	BT_RFCOMM_STATE_USER_DISCONNECT,
	BT_RFCOMM_STATE_DISCONNECTING,
	BT_RFCOMM_STATE_DISCONNECTED,
};

struct bt_rfcomm_hdr {
	uint8_t address;
	uint8_t control;
	uint8_t length;
} __packed;

#define BT_RFCOMM_SABM  0x2f
#define BT_RFCOMM_UA    0x63
#define BT_RFCOMM_UIH   0xef

struct bt_rfcomm_msg_hdr {
	uint8_t type;
	uint8_t len;
} __packed;

#define BT_RFCOMM_PN    0x20
struct bt_rfcomm_pn {
	uint8_t  dlci;
	uint8_t  flow_ctrl;
	uint8_t  priority;
	uint8_t  ack_timer;
	uint16_t mtu;
	uint8_t  max_retrans;
	uint8_t  credits;
} __packed;

#define BT_RFCOMM_MSC    0x38
struct bt_rfcomm_msc {
	uint8_t  dlci;
	uint8_t  v24_signal;
} __packed;

#define BT_RFCOMM_DISC  0x43
#define BT_RFCOMM_DM    0x0f

/* DV = 1 IC = 0 RTR = 1 RTC = 1 FC = 0 EXT = 0 */
#define BT_RFCOMM_DEFAULT_V24_SIG 0x8d

#define BT_RFCOMM_SIG_MIN_MTU   23
#define BT_RFCOMM_SIG_MAX_MTU   32767

#define BT_RFCOMM_CHECK_MTU(mtu) (!!((mtu) >= BT_RFCOMM_SIG_MIN_MTU && \
				     (mtu) <= BT_RFCOMM_SIG_MAX_MTU))

/* Helper to calculate needed outgoing buffer size.
 * Length in rfcomm header can be two bytes depending on user data length.
 * One byte in the tail should be reserved for FCS.
 */
#define BT_RFCOMM_BUF_SIZE(mtu) (CONFIG_BLUETOOTH_HCI_SEND_RESERVE + \
				 sizeof(struct bt_hci_acl_hdr) + \
				 sizeof(struct bt_l2cap_hdr) + \
				 sizeof(struct bt_rfcomm_hdr) + 1 + (mtu) + \
				 BT_RFCOMM_FCS_SIZE)

#define BT_RFCOMM_GET_DLCI(addr)           (((addr) & 0xfc) >> 2)
#define BT_RFCOMM_GET_FRAME_TYPE(ctrl)     ((ctrl) & 0xef)
#define BT_RFCOMM_GET_MSG_TYPE(type)       (((type) & 0xfc) >> 2)
#define BT_RFCOMM_GET_MSG_CR(type)         (((type) & 0x02) >> 1)
#define BT_RFCOMM_GET_LEN(len)             (((len) & 0xfe) >> 1)
#define BT_RFCOMM_GET_CHANNEL(dlci)        ((dlci) >> 1)
#define BT_RFCOMM_GET_PF(ctrl)             (((ctrl) & 0x10) >> 4)

#define BT_RFCOMM_SET_ADDR(dlci, cr)       ((((dlci) & 0x3f) << 2) | \
					    ((cr) << 1) | 0x01)
#define BT_RFCOMM_SET_CTRL(type, pf)       (((type) & 0xef) | ((pf) << 4))
#define BT_RFCOMM_SET_LEN_8(len)           (((len) << 1) | 1)
#define BT_RFCOMM_SET_LEN_16(len)          ((len) << 1)
#define BT_RFCOMM_SET_MSG_TYPE(type, cr)   (((type) << 2) | (cr << 1) | 0x01)

#define BT_RFCOMM_LEN_EXTENDED(len)        (!((len) & 0x01))

/* For CR in UIH Packet header
 * Initiating station have the C/R bit set to 1 and those sent by the
 * responding station have the C/R bit set to 0
 */
#define BT_RFCOMM_UIH_CR(role)             ((role) == BT_RFCOMM_ROLE_INITIATOR)

/* For CR in Non UIH Packet header
 * Command
 * Initiator --> Responder 1
 * Responder --> Initiator 0
 * Response
 * Initiator --> Responder 0
 * Responder --> Initiator 1
 */
#define BT_RFCOMM_CMD_CR(role)             ((role) == BT_RFCOMM_ROLE_INITIATOR)
#define BT_RFCOMM_RESP_CR(role)            ((role) == BT_RFCOMM_ROLE_ACCEPTOR)

/* For CR in MSG header
 * If the C/R bit is set to 1 the message is a command,
 * if it is set to 0 the message is a response.
 */
#define BT_RFCOMM_MSG_CMD_CR               1
#define BT_RFCOMM_MSG_RESP_CR              0

#define BT_RFCOMM_DLCI(role, channel)      ((((channel) & 0x1f) << 1) | \
					    ((role) == BT_RFCOMM_ROLE_ACCEPTOR))

/* Excluding ext bit */
#define BT_RFCOMM_MAX_LEN_8 127

/* Length can be 2 bytes depending on data size */
#define BT_RFCOMM_HDR_SIZE  (sizeof(struct bt_rfcomm_hdr) + 1)
#define BT_RFCOMM_FCS_SIZE  1

#define BT_RFCOMM_FCS_LEN_UIH      2
#define BT_RFCOMM_FCS_LEN_NON_UIH  3

/* For non UIH packets
 * The P bit set to 1 shall be used to solicit a response frame with the
 * F bit set to 1 from the other station.
 */
#define BT_RFCOMM_PF_NON_UIH         1

/* For UIH packets
 * Both stations set the P-bit to 0
 * If credit based flow control is used, If P/F is 1 then one credit byte
 * will be there after control in the frame else no credit byte.
 */
#define BT_RFCOMM_PF_UIH             0
#define BT_RFCOMM_PF_UIH_CREDIT      1
#define BT_RFCOMM_PF_UIH_NO_CREDIT   0

/* Initialize RFCOMM signal layer */
void bt_rfcomm_init(void);
