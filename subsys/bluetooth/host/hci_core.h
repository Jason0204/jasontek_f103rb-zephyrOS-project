/* hci_core.h - Bluetooth HCI core access */

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

/* LL connection parameters */
#define LE_CONN_LATENCY		0x0000
#define LE_CONN_TIMEOUT		0x002a

#if defined(CONFIG_BLUETOOTH_BREDR)
#define LMP_FEAT_PAGES_COUNT	3
#else
#define LMP_FEAT_PAGES_COUNT	1
#endif

/* bt_dev flags: the flags defined here represent BT controller state */
enum {
	BT_DEV_ENABLE,
	BT_DEV_READY,
	BT_DEV_ID_STATIC_RANDOM,
	BT_DEV_HAS_PUB_KEY,
	BT_DEV_PUB_KEY_BUSY,

	BT_DEV_ADVERTISING,
	BT_DEV_KEEP_ADVERTISING,
	BT_DEV_SCANNING,
	BT_DEV_EXPLICIT_SCAN,
	BT_DEV_ACTIVE_SCAN,

	BT_DEV_RPA_VALID,

#if defined(CONFIG_BLUETOOTH_BREDR)
	BT_DEV_ISCAN,
	BT_DEV_PSCAN,
	BT_DEV_INQUIRY,
#endif /* CONFIG_BLUETOOTH_BREDR */

	/* Total number of flags - must be at the end of the enum */
	BT_DEV_NUM_FLAGS,
};

struct bt_dev_le {
	/* LE features */
	uint8_t			features[1][8];
	/* LE states */
	uint64_t                states;

	/* Controller buffer information */
	uint16_t		mtu;
	struct k_sem		pkts;
};

#if defined(CONFIG_BLUETOOTH_BREDR)
struct bt_dev_br {
	/* Max controller's acceptable ACL packet length */
	uint16_t		mtu;
	struct k_sem		pkts;
};
#endif

/* State tracking for the local Bluetooth controller */
struct bt_dev {
	/* Local Identity Address */
	bt_addr_le_t		id_addr;

	/* Current local Random Address */
	bt_addr_le_t		random_addr;

	/* Controller version & manufacturer information */
	uint8_t			hci_version;
	uint8_t			lmp_version;
	uint16_t		hci_revision;
	uint16_t		lmp_subversion;
	uint16_t		manufacturer;

	/* LMP features (pages 0, 1, 2) */
	uint8_t			features[LMP_FEAT_PAGES_COUNT][8];

	/* Supported commands */
	uint8_t			supported_commands[64];

	ATOMIC_DEFINE(flags, BT_DEV_NUM_FLAGS);

	/* LE controller specific features */
	struct bt_dev_le	le;

#if defined(CONFIG_BLUETOOTH_BREDR)
	/* BR/EDR controller specific features */
	struct bt_dev_br	br;
#endif

	/* Number of commands controller can accept */
	struct k_sem		ncmd_sem;

	/* Last sent HCI command */
	struct net_buf		*sent_cmd;

	/* Queue for incoming HCI events & ACL data */
	struct k_fifo		rx_queue;

	/* Queue for high priority HCI events which may unlock waiters
	 * in other threads. Such events include Number of Completed
	 * Packets, as well as the Command Complete/Status events.
	 */
	struct k_fifo		rx_prio_queue;

	/* Queue for outgoing HCI commands */
	struct k_fifo		cmd_tx_queue;

	/* Registered HCI driver */
	struct bt_hci_driver	*drv;

#if defined(CONFIG_BLUETOOTH_PRIVACY)
	/* Local Identity Resolving Key */
	uint8_t			irk[16];

	/* Work used for RPA rotation */
	struct k_delayed_work rpa_update;
#endif
};

extern struct bt_dev bt_dev;
extern const struct bt_storage *bt_storage;
#if defined(CONFIG_BLUETOOTH_SMP) || defined(CONFIG_BLUETOOTH_BREDR)
extern const struct bt_conn_auth_cb *bt_auth;
#endif /* CONFIG_BLUETOOTH_SMP || CONFIG_BLUETOOTH_BREDR */

static inline bool bt_le_conn_params_valid(uint16_t min, uint16_t max,
					   uint16_t latency, uint16_t timeout)
{
	if (min > max || min < 6 || max > 3200) {
		return false;
	}

	/* Limits according to BT Core spec 4.2 [Vol 2, Part E, 7.8.12] */
	if (timeout < 10 || timeout > 3200) {
		return false;
	}

	/* Limits according to BT Core spec 4.2 [Vol 6, Part B, 4.5.1] */
	if (latency > 499 || ((latency + 1) * max) > (timeout * 4)) {
		return false;
	}

	return true;
}

struct net_buf *bt_hci_cmd_create(uint16_t opcode, uint8_t param_len);
int bt_hci_cmd_send(uint16_t opcode, struct net_buf *buf);
int bt_hci_cmd_send_sync(uint16_t opcode, struct net_buf *buf,
			 struct net_buf **rsp);

/* The helper is only safe to be called from internal threads as it's
 * not multi-threading safe
 */
#if defined(CONFIG_BLUETOOTH_DEBUG)
const char *bt_addr_str(const bt_addr_t *addr);
const char *bt_addr_le_str(const bt_addr_le_t *addr);
#endif

int bt_le_scan_update(bool fast_scan);

bool bt_addr_le_is_bonded(const bt_addr_le_t *addr);

int bt_send(struct net_buf *buf);

uint16_t bt_hci_get_cmd_opcode(struct net_buf *buf);
