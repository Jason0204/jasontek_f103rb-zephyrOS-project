/** @file
 *  @brief Custom monitor protocol logging over UART
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

#if defined(CONFIG_BLUETOOTH_DEBUG_MONITOR)

#define BT_MONITOR_NEW_INDEX	0
#define BT_MONITOR_DEL_INDEX	1
#define BT_MONITOR_COMMAND_PKT	2
#define BT_MONITOR_EVENT_PKT	3
#define BT_MONITOR_ACL_TX_PKT	4
#define BT_MONITOR_ACL_RX_PKT	5
#define BT_MONITOR_SCO_TX_PKT	6
#define BT_MONITOR_SCO_RX_PKT	7
#define BT_MONITOR_OPEN_INDEX	8
#define BT_MONITOR_CLOSE_INDEX	9
#define BT_MONITOR_INDEX_INFO	10
#define BT_MONITOR_VENDOR_DIAG	11
#define BT_MONITOR_SYSTEM_NOTE	12
#define BT_MONITOR_USER_LOGGING	13
#define BT_MONITOR_NOP		255

#define BT_MONITOR_TYPE_PRIMARY	0
#define BT_MONITOR_TYPE_AMP	1

/* Extended header types */
#define BT_MONITOR_COMMAND_DROPS 1
#define BT_MONITOR_EVENT_DROPS   2
#define BT_MONITOR_ACL_RX_DROPS  3
#define BT_MONITOR_ACL_TX_DROPS  4
#define BT_MONITOR_SCO_RX_DROPS  5
#define BT_MONITOR_SCO_TX_DROPS  6
#define BT_MONITOR_OTHER_DROPS   7
#define BT_MONITOR_TS32          8

struct bt_monitor_hdr {
	uint16_t  data_len;
	uint16_t  opcode;
	uint8_t   flags;
	uint8_t   hdr_len;

	/* Extended header */
	uint8_t   type;
	uint32_t  ts32;
} __packed;

struct bt_monitor_new_index {
	uint8_t  type;
	uint8_t  bus;
	uint8_t  bdaddr[6];
	char     name[8];
} __packed;

struct bt_monitor_user_logging {
	uint8_t  priority;
	uint8_t  ident_len;
} __packed;

static inline uint8_t bt_monitor_opcode(struct net_buf *buf)
{
	switch (bt_buf_get_type(buf)) {
	case BT_BUF_CMD:
		return BT_MONITOR_COMMAND_PKT;
	case BT_BUF_EVT:
		return BT_MONITOR_EVENT_PKT;
	case BT_BUF_ACL_OUT:
		return BT_MONITOR_ACL_TX_PKT;
	case BT_BUF_ACL_IN:
		return BT_MONITOR_ACL_RX_PKT;
	default:
		return BT_MONITOR_NOP;
	}
}

void bt_monitor_send(uint16_t opcode, const void *data, size_t len);

void bt_monitor_new_index(uint8_t type, uint8_t bus, bt_addr_t *addr,
			  const char *name);

#else /* !CONFIG_BLUETOOTH_DEBUG_MONITOR */

#define bt_monitor_send(opcode, data, len)
#define bt_monitor_new_index(type, bus, addr, name)

#endif
