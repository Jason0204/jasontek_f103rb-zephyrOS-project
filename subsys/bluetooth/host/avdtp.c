/*
 * Audio Video Distribution Protocol
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
 *
*/

#include <zephyr.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <atomic.h>
#include <misc/byteorder.h>
#include <misc/util.h>

#include <bluetooth/log.h>
#include <bluetooth/hci.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/avdtp.h>

#include "l2cap_internal.h"
#include "avdtp_internal.h"

#if !defined(CONFIG_BLUETOOTH_DEBUG_AVDTP)
#undef BT_DBG
#define BT_DBG(fmt, ...)
#endif

/* TODO add config file*/
#define CONFIG_BLUETOOTH_AVDTP_CONN CONFIG_BLUETOOTH_MAX_CONN

/* Pool for outgoing BR/EDR signaling packets, min MTU is 48 */
static struct k_fifo avdtp_sig;
static NET_BUF_POOL(avdtp_sig_pool, CONFIG_BLUETOOTH_AVDTP_CONN,
		    BT_AVDTP_BUF_SIZE(BT_AVDTP_MIN_MTU),
		    &avdtp_sig, NULL, BT_BUF_USER_DATA_MIN);

static struct bt_avdtp bt_avdtp_pool[CONFIG_BLUETOOTH_AVDTP_CONN];

static struct bt_avdtp_event_cb *event_cb;

static struct bt_avdtp_seid_lsep *lseps;

#define AVDTP_CHAN(_ch) CONTAINER_OF(_ch, struct bt_avdtp, br_chan.chan)

/* L2CAP Interface callbacks */
void bt_avdtp_l2cap_connected(struct bt_l2cap_chan *chan)
{
	if (!chan) {
		BT_ERR("Invalid AVDTP chan");
		return;
	}

	BT_DBG("chan %p session %p", chan, AVDTP_CHAN(chan));
}

void bt_avdtp_l2cap_disconnected(struct bt_l2cap_chan *chan)
{
	if (!chan) {
		BT_ERR("Invalid AVDTP chan");
		return;
	}

	BT_DBG("chan %p session %p", chan, AVDTP_CHAN(chan));
}

void bt_avdtp_l2cap_encrypt_changed(struct bt_l2cap_chan *chan, uint8_t status)
{
	BT_DBG("");
}

void bt_avdtp_l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	BT_DBG("");
}

/*A2DP Layer interface */
int bt_avdtp_connect(struct bt_conn *conn, struct bt_avdtp *session)
{
	static struct bt_l2cap_chan_ops ops = {
		.connected = bt_avdtp_l2cap_connected,
		.disconnected = bt_avdtp_l2cap_disconnected,
		.encrypt_change = bt_avdtp_l2cap_encrypt_changed,
		.recv = bt_avdtp_l2cap_recv
	};

	if (!session) {
		return -EINVAL;
	}

	session->br_chan.chan.ops = &ops;
	session->br_chan.chan.required_sec_level = BT_SECURITY_MEDIUM;

	return bt_l2cap_chan_connect(conn, &session->br_chan.chan,
				     BT_L2CAP_PSM_AVDTP);
}

int bt_avdtp_disconnect(struct bt_avdtp *session)
{
	if (!session) {
		return -EINVAL;
	}

	BT_DBG("session %p", session);

	return bt_l2cap_chan_disconnect(&session->br_chan.chan);
}

int bt_avdtp_l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
	int i;
	static struct bt_l2cap_chan_ops ops = {
		.connected = bt_avdtp_l2cap_connected,
		.disconnected = bt_avdtp_l2cap_disconnected,
		.recv = bt_avdtp_l2cap_recv,
	};

	BT_DBG("conn %p", conn);

	for (i = 0; i < ARRAY_SIZE(bt_avdtp_pool); i++) {
		struct bt_avdtp *avdtp = &bt_avdtp_pool[i];

		if (avdtp->br_chan.chan.conn) {
			continue;
		}

		avdtp->br_chan.chan.ops = &ops;
		avdtp->br_chan.rx.mtu = BT_AVDTP_MAX_MTU;
		*chan = &avdtp->br_chan.chan;
		return 0;
	}

	return -ENOMEM;
}

/* Application will register its callback */
int bt_avdtp_register(struct bt_avdtp_event_cb *cb)
{
	BT_DBG("");

	if (event_cb) {
		return -EALREADY;
	}

	event_cb = cb;

	return 0;
}

int bt_avdtp_register_sep(uint8_t media_type, uint8_t role,
			  struct bt_avdtp_seid_lsep *lsep)
{
	BT_DBG("");

	static uint8_t bt_avdtp_seid = BT_AVDTP_MIN_SEID;

	if (!lsep) {
		return -EIO;
	}

	if (bt_avdtp_seid == BT_AVDTP_MAX_SEID) {
		return -EIO;
	}

	lsep->sep.id = bt_avdtp_seid++;
	lsep->sep.inuse = 0;
	lsep->sep.media_type = media_type;
	lsep->sep.tsep = role;

	lsep->next = lseps;
	lseps = lsep;

	return 0;
}

/* init function */
int bt_avdtp_init(void)
{
	int err;
	static struct bt_l2cap_server avdtp_l2cap = {
		.psm = BT_L2CAP_PSM_AVDTP,
		.sec_level = BT_SECURITY_MEDIUM,
		.accept = bt_avdtp_l2cap_accept,
	};

	BT_DBG("");

	/* Memory Initialization */
	net_buf_pool_init(avdtp_sig_pool);

	/* Register AVDTP PSM with L2CAP */
	err = bt_l2cap_br_server_register(&avdtp_l2cap);
	if (err < 0) {
		BT_ERR("AVDTP L2CAP Registration failed %d", err);
	}

	return err;
}
