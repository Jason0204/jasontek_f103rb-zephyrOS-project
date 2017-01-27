/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2016 Vinayak Kariappa Chettimada
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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <misc/util.h>
#include <device.h>
#include <clock_control.h>

#include "hal_work.h"

#include "defines.h"
#include "cpu.h"
#include "work.h"
#include "rand.h"
#include "ticker.h"
#include "mem.h"
#include "memq.h"
#include "util.h"
#include "ecb.h"
#include "ccm.h"
#include "radio.h"
#include "pdu.h"
#include "ctrl_internal.h"

#include "debug.h"

#define RADIO_PREAMBLE_TO_ADDRESS_US	40
#define RADIO_HCTO_US			(150 + 2 + 2 + \
					 RADIO_PREAMBLE_TO_ADDRESS_US)
#define RADIO_CONN_EVENTS(x, y)		((uint16_t)((x) / (y)))

#define RADIO_TICKER_JITTER_US			16
#define RADIO_TICKER_START_PART_US		200
#define RADIO_TICKER_XTAL_OFFSET_US		1500
#define RADIO_TICKER_PREEMPT_PART_US		0
#define RADIO_TICKER_PREEMPT_PART_MIN_US	0
#define RADIO_TICKER_PREEMPT_PART_MAX_US	RADIO_TICKER_XTAL_OFFSET_US

#define RADIO_RSSI_SAMPLE_COUNT	10
#define RADIO_RSSI_THRESHOLD	4

#define RADIO_IRK_COUNT_MAX	8

#define FAST_ENC_PROCEDURE	0
#define XTAL_ADVANCED		1
#define SCHED_ADVANCED		1
#define SILENT_CONNECTION	0

#define RADIO_PHY_ADV		0
#define RADIO_PHY_CONN		0

enum role {
	ROLE_NONE,
	ROLE_ADV,
	ROLE_OBS,
	ROLE_SLAVE,
	ROLE_MASTER,
};

enum state {
	STATE_NONE,
	STATE_RX,
	STATE_TX,
	STATE_CLOSE,
	STATE_STOP,
	STATE_ABORT,
};

struct advertiser {
	struct shdr hdr;

	uint8_t chl_map:3;
	uint8_t chl_map_current:3;
	uint8_t filter_policy:2;

	uint8_t filter_enable_bitmask;
	uint8_t filter_addr_type_bitmask;
	uint8_t filter_bdaddr[8][BDADDR_SIZE];

	struct radio_adv_data adv_data;
	struct radio_adv_data scan_data;

	struct connection *conn;
};

struct observer {
	struct shdr hdr;

	uint8_t scan_type:1;
	uint8_t scan_state:1;
	uint8_t scan_channel:2;
	uint8_t filter_policy:2;
	uint8_t adv_addr_type:1;
	uint8_t init_addr_type:1;

	uint8_t adv_addr[BDADDR_SIZE];
	uint8_t init_addr[BDADDR_SIZE];
	uint32_t ticks_window;

	uint8_t filter_enable_bitmask;
	uint8_t filter_addr_type_bitmask;
	uint8_t filter_bdaddr[8][BDADDR_SIZE];

	uint16_t conn_interval;
	uint16_t conn_latency;
	uint16_t conn_timeout;
	uint32_t ticks_conn_slot;
	struct connection *conn;

	uint32_t win_offset_us;
};

static struct {
	struct device *hf_clock;

	uint32_t ticks_anchor;
	uint32_t remainder_anchor;

	uint8_t volatile ticker_id_prepare;
	uint8_t volatile ticker_id_event;
	enum role volatile role;
	enum state state;

	uint8_t filter_enable_bitmask;
	uint8_t filter_addr_type_bitmask;
	uint8_t filter_bdaddr[8][BDADDR_SIZE];

	uint8_t nirk;
	uint8_t irk[RADIO_IRK_COUNT_MAX][16];

	struct advertiser advertiser;
	struct observer observer;

	void *conn_pool;
	void *conn_free;
	uint8_t connection_count;
	struct connection *conn_curr;

	uint8_t packet_counter;
	uint8_t crc_expire;

	uint8_t data_channel_map[5];
	uint8_t data_channel_count;
	uint8_t sca;

	/* DLE global settings */
	uint16_t default_tx_octets;
	uint16_t default_tx_time;

	/** @todo below members to be made role specific and quota managed for
	 * Rx-es.
	 */
	/* Advertiser, Observer, and Connections Rx data pool */
	void *pkt_rx_data_pool;
	void *pkt_rx_data_free;
	uint16_t packet_data_octets_max;
	uint16_t packet_rx_data_pool_size;
	uint16_t packet_rx_data_size;
	uint8_t packet_rx_data_count;
	/* Free queue Rx data buffers */
	struct radio_pdu_node_rx **packet_rx;
	uint8_t packet_rx_count;
	uint8_t volatile packet_rx_last;
	uint8_t packet_rx_acquire;

	/* Controller to Host event-cum-data queue */
	void *link_rx_pool;
	void *link_rx_free;
	void *link_rx_head;

	void *volatile link_rx_tail;
	uint8_t link_rx_data_quota;

	/* Connections common Tx ctrl and data pool */
	void *pkt_tx_ctrl_pool;
	void *pkt_tx_ctrl_free;
	void *pkt_tx_data_pool;
	void *pkt_tx_data_free;
	uint16_t packet_tx_data_pool_size;
	uint16_t packet_tx_data_size;

	/* Host to Controller Tx, and Controller to Host Num complete queue */
	struct pdu_data_q_tx *pkt_tx;
	struct pdu_data_q_tx *pkt_release;
	uint8_t packet_tx_count;
	uint8_t volatile packet_tx_first;
	uint8_t packet_tx_last;
	uint8_t packet_release_first;
	uint8_t volatile packet_release_last;

	uint16_t fc_handle[TRIPLE_BUFFER_SIZE];
	uint8_t volatile fc_req;
	uint8_t fc_ack;
	uint8_t fc_ena;

	uint32_t ticks_active_to_start;

	struct connection *conn_upd;
} _radio;

static uint16_t const gc_lookup_ppm[] = { 500, 250, 150, 100, 75, 50, 30, 20 };

static void common_init(void);
static void ticker_success_assert(uint32_t status, void *params);
static void event_inactive(uint32_t ticks_at_expire, uint32_t remainder,
			   uint16_t lazy, void *context);
static void adv_setup(void);
static void event_adv(uint32_t ticks_at_expire, uint32_t remainder,
		      uint16_t lazy, void *context);
static void event_obs(uint32_t ticks_at_expire, uint32_t remainder,
		      uint16_t lazy, void *context);
static void event_slave_prepare(uint32_t ticks_at_expire,
				uint32_t remainder, uint16_t lazy,
				void *context);
static void event_slave(uint32_t ticks_at_expire, uint32_t remainder,
			uint16_t lazy, void *context);
static void event_master_prepare(uint32_t ticks_at_expire,
				 uint32_t remainder, uint16_t lazy,
				 void *context);
static void event_master(uint32_t ticks_at_expire, uint32_t remainder,
			 uint16_t lazy, void *context);
static void rx_packet_set(struct connection *conn,
			  struct pdu_data *pdu_data_rx);
static void tx_packet_set(struct connection *conn,
				struct pdu_data *pdu_data_tx);
static void prepare_pdu_data_tx(struct connection *conn,
				struct pdu_data **pdu_data_tx);
static void packet_rx_allocate(uint8_t max);
static uint8_t packet_rx_acquired_count_get(void);
static struct radio_pdu_node_rx *packet_rx_reserve_get(uint8_t count);
static void packet_rx_enqueue(void);
static void packet_tx_enqueue(uint8_t max);
static struct pdu_data *empty_tx_enqueue(struct connection *conn);
static void ctrl_tx_enqueue(struct connection *conn,
			    struct radio_pdu_node_tx *node_tx);
static void pdu_node_tx_release(uint16_t handle,
				struct radio_pdu_node_tx *node_tx);
static void connection_release(struct connection *conn);
static uint32_t conn_update(struct connection *conn,
			    struct pdu_data *pdu_data_rx);
static uint32_t is_peer_compatible(struct connection *conn);
static uint32_t conn_update_req(struct connection *conn);
static uint32_t channel_map_update(struct connection *conn,
				   struct pdu_data *pdu_data_rx);
static void enc_req_reused_send(struct connection *conn,
				struct radio_pdu_node_tx *node_tx);
static void terminate_ind_rx_enqueue(struct connection *conn, uint8_t reason);
static void enc_rsp_send(struct connection *conn);
static void start_enc_rsp_send(struct connection *conn,
			       struct pdu_data *pdu_ctrl_tx);
static void unknown_rsp_send(struct connection *conn, uint8_t type);
static void feature_rsp_send(struct connection *conn);
static void pause_enc_rsp_send(struct connection *conn);
static void version_ind_send(struct connection *conn);
static void ping_resp_send(struct connection *conn);
static void reject_ind_ext_send(struct connection *conn,
				uint8_t reject_opcode,
				uint8_t error_code);
static void length_resp_send(struct connection *conn,
				uint16_t eff_rx_octets,
				uint16_t eff_tx_octets);
static uint32_t role_disable(uint8_t ticker_id_primary,
			     uint8_t ticker_id_stop);
static void rx_fc_lock(uint16_t handle);

/*****************************************************************************
 *RADIO
 ****************************************************************************/
uint32_t radio_init(void *hf_clock, uint8_t sca, uint8_t connection_count_max,
		    uint8_t rx_count_max, uint8_t tx_count_max,
		    uint16_t packet_data_octets_max, uint8_t *mem_radio,
		    uint16_t mem_size)
{
	uint32_t retcode;
	uint8_t *mem_radio_end;

	/* intialise hf_clock device to use in prepare */
	_radio.hf_clock = hf_clock;

	/* initialise SCA */
	_radio.sca = sca;

	/* initialised radio mem end variable */
	mem_radio_end = mem_radio + mem_size;

	/* initialise connection context memory */
	_radio.connection_count = connection_count_max;
	_radio.conn_pool = mem_radio;
	mem_radio += (sizeof(struct connection) * _radio.connection_count);

	/* initialise rx and tx queue counts */

	/* additional for pdu to NACK or receive empty PDU,
	 * 1 scan resp and 1* ctrl event.
	 */
	rx_count_max += 3;
	 /* additional pdu to send enc_req ctrl pdu */
	tx_count_max += 1;
	_radio.packet_rx_count = (rx_count_max + 1);
	_radio.packet_tx_count = (tx_count_max + 1);
	_radio.link_rx_data_quota = rx_count_max;

	/* initialise rx queue memory */
	_radio.packet_rx = (struct radio_pdu_node_rx **)mem_radio;
	mem_radio +=
	    (sizeof(struct radio_pdu_node_rx *)*_radio.packet_rx_count);

	/* initialise tx queue memory */
	_radio.pkt_tx = (struct pdu_data_q_tx *)mem_radio;
	mem_radio += (sizeof(struct pdu_data_q_tx) * _radio.packet_tx_count);

	/* initialise tx release queue memory */
	_radio.pkt_release = (struct pdu_data_q_tx *)mem_radio;
	mem_radio += (sizeof(struct pdu_data_q_tx) * _radio.packet_tx_count);

	/* initialise rx memory size and count */
	_radio.packet_data_octets_max = packet_data_octets_max;
	if ((RADIO_ACPDU_SIZE_MAX + 1) <
	    (offsetof(struct pdu_data, payload) +
			_radio.packet_data_octets_max)) {
		_radio.packet_rx_data_pool_size =
		    (ALIGN4(offsetof(struct radio_pdu_node_rx, pdu_data) +
			    offsetof(struct pdu_data, payload) +
			    _radio.packet_data_octets_max) * rx_count_max);
	} else {
		_radio.packet_rx_data_pool_size =
			(ALIGN4(offsetof(struct radio_pdu_node_rx, pdu_data) +
			  (RADIO_ACPDU_SIZE_MAX + 1)) * rx_count_max);
	}
	_radio.packet_rx_data_size = PACKET_RX_DATA_SIZE_MIN;
	_radio.packet_rx_data_count = (_radio.packet_rx_data_pool_size /
				       _radio.packet_rx_data_size);

	/* initialise rx data pool memory */
	_radio.pkt_rx_data_pool = mem_radio;
	mem_radio += _radio.packet_rx_data_pool_size;

	/* initialise rx link pool memory */
	_radio.link_rx_pool = mem_radio;
	mem_radio += (sizeof(void *) * 2 * (_radio.packet_rx_count +
					    _radio.connection_count));

	/* initialise tx ctrl pool memory */
	_radio.pkt_tx_ctrl_pool = mem_radio;
	mem_radio += PACKET_TX_CTRL_SIZE_MIN * PACKET_MEM_COUNT_TX_CTRL;

	/* initialise tx data memory size and count */
	_radio.packet_tx_data_size =
		ALIGN4(offsetof(struct radio_pdu_node_tx, pdu_data) +
		       offsetof(struct pdu_data, payload) +
		       _radio.packet_data_octets_max);
	_radio.packet_tx_data_pool_size =
		(_radio.packet_tx_data_size * tx_count_max);

	/* initialise tx data pool memory */
	_radio.pkt_tx_data_pool = mem_radio;
	mem_radio += _radio.packet_tx_data_pool_size;

	/* check for sufficient memory allocation for stack
	 * configuration.
	 */
	retcode = (mem_radio - mem_radio_end);
	if (retcode) {
		return (retcode + mem_size);
	}

	/* enable connection handle based on-off flow control feature.
	 * This is a simple flow control to rx data only on one selected
	 * connection handle.
	 * TODO: replace this feature with host-to-controller flowcontrol
	 * implementation/design.
	 */
	_radio.fc_ena = 1;

	/* memory allocations */
	common_init();

	return retcode;
}

void ctrl_reset(void)
{
	uint16_t conn_handle;

	/* disable advertiser events */
	role_disable(RADIO_TICKER_ID_ADV, RADIO_TICKER_ID_ADV_STOP);

	/* disable oberver events */
	role_disable(RADIO_TICKER_ID_OBS, RADIO_TICKER_ID_OBS_STOP);

	/* disable connection events */
	for (conn_handle = 0; conn_handle < _radio.connection_count;
	     conn_handle++) {
		role_disable(RADIO_TICKER_ID_FIRST_CONNECTION + conn_handle,
			     TICKER_NULL);
	}

	/* reset controller context members */
	_radio.filter_enable_bitmask = 0;
	_radio.nirk = 0;
	_radio.advertiser.conn = NULL;
	_radio.observer.conn = NULL;
	_radio.packet_rx_data_size = PACKET_RX_DATA_SIZE_MIN;
	_radio.packet_rx_data_count = (_radio.packet_rx_data_pool_size /
				       _radio.packet_rx_data_size);
	_radio.packet_rx_last = 0;
	_radio.packet_rx_acquire = 0;
	_radio.link_rx_data_quota = _radio.packet_rx_count - 1;
	_radio.packet_tx_first = 0;
	_radio.packet_tx_last = 0;
	_radio.packet_release_first = 0;
	_radio.packet_release_last = 0;

	/* memory allocations */
	common_init();
}

static void common_init(void)
{
	void *link;

	/* initialise connection pool. */
	if (_radio.connection_count) {
		mem_init(_radio.conn_pool, CONNECTION_T_SIZE,
			 _radio.connection_count,
			 &_radio.conn_free);
	} else {
		_radio.conn_free = NULL;
	}

	/* initialise rx pool. */
	mem_init(_radio.pkt_rx_data_pool,
		 _radio.packet_rx_data_size,
		 _radio.packet_rx_data_count,
		 &_radio.pkt_rx_data_free);

	/* initialise rx link pool. */
	mem_init(_radio.link_rx_pool, (sizeof(void *) * 2),
		 (_radio.packet_rx_count + _radio.connection_count),
		 &_radio.link_rx_free);

	/* initialise ctrl tx pool. */
	mem_init(_radio.pkt_tx_ctrl_pool, PACKET_TX_CTRL_SIZE_MIN,
		 PACKET_MEM_COUNT_TX_CTRL, &_radio.pkt_tx_ctrl_free);

	/* initialise data tx pool. */
	mem_init(_radio.pkt_tx_data_pool, _radio.packet_tx_data_size,
		 (_radio.packet_tx_count - 1), &_radio.pkt_tx_data_free);

	/* initialise the event-cum-data memq */
	link = mem_acquire(&_radio.link_rx_free);
	LL_ASSERT(link);
	memq_init(link, &_radio.link_rx_head, (void *)&_radio.link_rx_tail);

	/* initialise advertiser channel map */
	_radio.advertiser.chl_map = 0x07;

	/* initialise connection channel map */
	_radio.data_channel_map[0] = 0xFF;
	_radio.data_channel_map[1] = 0xFF;
	_radio.data_channel_map[2] = 0xFF;
	_radio.data_channel_map[3] = 0xFF;
	_radio.data_channel_map[4] = 0x1F;
	_radio.data_channel_count = 37;

	/* Initialize the DLE defaults */
	_radio.default_tx_octets = RADIO_LL_LENGTH_OCTETS_RX_MIN;
	_radio.default_tx_time = RADIO_LL_LENGTH_TIME_RX_MIN;

	/* allocate the rx queue */
	packet_rx_allocate(0xFF);
}

static inline void isr_radio_state_tx(void)
{
	_radio.state = STATE_RX;

	radio_switch_complete_and_tx();

	radio_tmr_hcto_configure(radio_tmr_end_get() +
				 RADIO_RX_CHAIN_DELAY_US + RADIO_HCTO_US -
				 RADIO_TX_CHAIN_DELAY_US);

	switch (_radio.role) {
	case ROLE_ADV:
		radio_pkt_rx_set(radio_pkt_scratch_get());

		if (_radio.advertiser.filter_policy && _radio.nirk) {
			radio_ar_configure(_radio.nirk, _radio.irk);
		}

		radio_tmr_end_capture();
		break;

	case ROLE_OBS:
		radio_pkt_rx_set(_radio.packet_rx[_radio.packet_rx_last]->
				    pdu_data);
		radio_rssi_measure();
		break;

	case ROLE_MASTER:
		if (_radio.packet_counter == 0) {
			radio_rssi_measure();
		}
		/* fall thru */

	case ROLE_SLAVE:
		rx_packet_set(_radio.conn_curr, (struct pdu_data *)_radio.
			      packet_rx[_radio.packet_rx_last]->pdu_data);

		radio_tmr_end_capture();

		/* Route the tx packet to respective connections */
		packet_tx_enqueue(1);
		break;

	case ROLE_NONE:
	default:
		LL_ASSERT(0);
		break;
	}

}

static inline uint32_t isr_rx_adv(uint8_t devmatch_ok, uint8_t irkmatch_ok,
				uint8_t irkmatch_id, uint8_t rssi_ready)
{
	struct pdu_adv *pdu_adv, *_pdu_adv;
	struct radio_pdu_node_rx *radio_pdu_node_rx;

	pdu_adv = (struct pdu_adv *)radio_pkt_scratch_get();

	if ((pdu_adv->type == PDU_ADV_TYPE_SCAN_REQ) &&
	    (pdu_adv->len == sizeof(struct pdu_adv_payload_scan_req)) &&
	    (((_radio.advertiser.filter_policy & 0x01) == 0) ||
	     (devmatch_ok) || (irkmatch_ok)) &&
	    (1 /** @todo own addr match check */)) {
		_radio.state = STATE_CLOSE;

		radio_switch_complete_and_disable();

		/* TODO use rssi_ready to generate proprietary scan_req event */
		rssi_ready = rssi_ready; /* unused for now */

		/* use the latest scan data, if any */
		if (_radio.advertiser.scan_data.first != _radio.
		    advertiser.scan_data.last) {
			uint8_t first;

			first = _radio.advertiser.scan_data.first + 1;
			if (first == DOUBLE_BUFFER_SIZE) {
				first = 0;
			}
			_radio.advertiser.scan_data.first = first;
		}

		radio_pkt_tx_set(&_radio.advertiser.scan_data.
		     data[_radio.advertiser.scan_data.first][0]);

		return 0;
	} else if ((pdu_adv->type == PDU_ADV_TYPE_CONNECT_REQ) &&
		   (pdu_adv->len == sizeof(struct pdu_adv_payload_connect_req)) &&
		   (((_radio.advertiser.filter_policy & 0x02) == 0) ||
		    (devmatch_ok) || (irkmatch_ok)) &&
		   (1 /** @todo own addr match check */) &&
		   ((_radio.fc_ena == 0) || (_radio.fc_req == _radio.fc_ack)) &&
		   (_radio.advertiser.conn)) {
		struct connection *conn;
		uint32_t ticker_status;
		uint32_t ticks_slot_offset;
		uint32_t conn_interval_us;
		struct pdu_data *pdu_data;
		struct radio_le_conn_cmplt *radio_le_conn_cmplt;

		 radio_pdu_node_rx = packet_rx_reserve_get(3);
		 if (radio_pdu_node_rx == 0) {
			return 1;
		 }

		_radio.state = STATE_STOP;
		radio_disable();

		/* acquire the slave context from advertiser */
		conn = _radio.advertiser.conn;
		_radio.advertiser.conn = NULL;

		/* Populate the slave context */
		conn->handle = mem_index_get(conn, _radio.conn_pool,
			CONNECTION_T_SIZE);
		memcpy(&conn->crc_init[0],
			 &pdu_adv->payload.connect_req.lldata.crc_init[0],
			 3);
		memcpy(&conn->access_addr[0],
			 &pdu_adv->payload.connect_req.lldata.access_addr[0],
			 4);
		memcpy(&conn->data_channel_map[0],
			 &pdu_adv->payload.connect_req.lldata.channel_map[0],
			 sizeof(conn->data_channel_map));
		conn->data_channel_count =
			util_ones_count_get(&conn->data_channel_map[0],
					sizeof(conn->data_channel_map));
		conn->data_channel_hop =
		    pdu_adv->payload.connect_req.lldata.hop;
		conn->conn_interval =
		    pdu_adv->payload.connect_req.lldata.interval;
		conn_interval_us =
		    pdu_adv->payload.connect_req.lldata.interval * 1250;
		conn->latency =
		    pdu_adv->payload.connect_req.lldata.latency;
		memcpy((void *)&conn->role.slave.force, &conn->access_addr[0],
			 sizeof(conn->role.slave.force));
		conn->supervision_reload =
			RADIO_CONN_EVENTS((pdu_adv->payload.connect_req.lldata.timeout
					   * 10 * 1000), conn_interval_us);
		conn->procedure_reload = RADIO_CONN_EVENTS((40 * 1000 * 1000),
							   conn_interval_us);
		conn->apto_reload = RADIO_CONN_EVENTS((30 * 1000 * 1000),
						      conn_interval_us);
		conn->appto_reload =
			(conn->apto_reload > (conn->latency + 2)) ?
			(conn->apto_reload - (conn->latency + 2)) :
			conn->apto_reload;

		/* Prepare the rx packet structure */
		radio_pdu_node_rx->hdr.handle = conn->handle;
		radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_CONNECTION;

		/* prepare connection complete structure */
		pdu_data = (struct pdu_data *)radio_pdu_node_rx->pdu_data;
		radio_le_conn_cmplt =
			(struct radio_le_conn_cmplt *)&pdu_data->payload;
		radio_le_conn_cmplt->status = 0x00;
		radio_le_conn_cmplt->role = 0x01;
		radio_le_conn_cmplt->peer_addr_type = pdu_adv->tx_addr;
		memcpy(&radio_le_conn_cmplt->peer_addr[0],
		       &pdu_adv->payload.connect_req.init_addr[0],
		       BDADDR_SIZE);
		radio_le_conn_cmplt->own_addr_type = pdu_adv->rx_addr;
		memcpy(&radio_le_conn_cmplt->own_addr[0],
		       &pdu_adv->payload.connect_req.adv_addr[0], BDADDR_SIZE);
		radio_le_conn_cmplt->peer_irk_index = irkmatch_id;
		radio_le_conn_cmplt->interval =
			pdu_adv->payload.connect_req.lldata.interval;
		radio_le_conn_cmplt->latency =
			pdu_adv->payload.connect_req.lldata.latency;
		radio_le_conn_cmplt->timeout =
			pdu_adv->payload.connect_req.lldata.timeout;
		radio_le_conn_cmplt->mca =
			pdu_adv->payload.connect_req.lldata.sca;

		/* enqueue connection complete structure into queue */
		rx_fc_lock(conn->handle);
		packet_rx_enqueue();

		/* calculate the window widening */
		conn->role.slave.sca = pdu_adv->payload.connect_req.lldata.sca;
		conn->role.slave.window_widening_periodic_us =
			(((gc_lookup_ppm[_radio.sca] +
			   gc_lookup_ppm[conn->role.slave.sca]) *
			  conn_interval_us) + (1000000 - 1)) / 1000000;
		conn->role.slave.window_widening_max_us =
			(conn_interval_us >> 1) - 150;
		conn->role.slave.window_size_event_us =
			pdu_adv->payload.connect_req.lldata.win_size * 1250;
		conn->role.slave.window_size_prepare_us = 0;

		/* calculate slave slot */
		conn->hdr.ticks_slot =
			TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US +
					   RADIO_RX_READY_DELAY_US + 328 +
					   328 + 150);
		conn->hdr.ticks_active_to_start = _radio.ticks_active_to_start;
		conn->hdr.ticks_xtal_to_start =
			TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US);
		conn->hdr.ticks_preempt_to_start =
			TICKER_US_TO_TICKS(RADIO_TICKER_PREEMPT_PART_MIN_US);
		ticks_slot_offset =
			(conn->hdr.ticks_active_to_start <
			 conn->hdr.ticks_xtal_to_start) ?
			conn->hdr.ticks_xtal_to_start :
			conn->hdr.ticks_active_to_start;
		conn_interval_us -=
			conn->role.slave.window_widening_periodic_us;

		/* Stop Advertiser */
		ticker_status = ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    RADIO_TICKER_ID_ADV,
		     ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		/* Stop Direct Adv Stopper */
		_pdu_adv = (struct pdu_adv *)&_radio.advertiser.adv_data.data
			[_radio.advertiser.adv_data.first][0];
		if (_pdu_adv->type == PDU_ADV_TYPE_DIRECT_IND) {
			ticker_status =
				ticker_stop(
					    RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    RADIO_TICKER_ID_ADV_STOP,
					    0, /* @todo ticker_success_assert */
					    0  /* @todo (void *) __LINE__*/);
			LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
				  (ticker_status == TICKER_STATUS_BUSY));
		}

		/* Start Slave */
		ticker_status = ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
		     RADIO_TICKER_USER_ID_WORKER,
		     RADIO_TICKER_ID_FIRST_CONNECTION
		     + conn->handle, (_radio.ticks_anchor - ticks_slot_offset),
		     TICKER_US_TO_TICKS(radio_tmr_end_get() -
		      RADIO_TX_CHAIN_DELAY_US + (((uint64_t) pdu_adv->
			payload.connect_req.lldata.win_offset + 1) * 1250) -
		      RADIO_RX_READY_DELAY_US - (RADIO_TICKER_JITTER_US << 1)),
		     TICKER_US_TO_TICKS(conn_interval_us),
		     TICKER_REMAINDER(conn_interval_us), TICKER_NULL_LAZY,
		     (ticks_slot_offset + conn->hdr.ticks_slot),
		     event_slave_prepare, conn, ticker_success_assert,
		     (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		return 0;
	}

	return 1;
}

static inline uint32_t isr_rx_obs(uint8_t irkmatch_id, uint8_t rssi_ready)
{
	struct pdu_adv *pdu_adv_rx;
	struct radio_pdu_node_rx *radio_pdu_node_rx;

	radio_pdu_node_rx = packet_rx_reserve_get(3);

	if (radio_pdu_node_rx == 0) {
		return 1;
	}

	pdu_adv_rx = (struct pdu_adv *)
		_radio.packet_rx[_radio.packet_rx_last]->pdu_data;

	/* Initiator */
	if ((_radio.observer.conn) && ((_radio.fc_ena == 0) ||
				       (_radio.fc_req == _radio.fc_ack)) &&
	    (((pdu_adv_rx->type == PDU_ADV_TYPE_ADV_IND) &&
	      (((_radio.observer.filter_policy & 0x01) != 0) ||
	       ((_radio.observer.adv_addr_type == pdu_adv_rx->tx_addr) &&
		(memcmp(&_radio.observer.adv_addr[0],
			&pdu_adv_rx->payload.adv_ind.addr[0],
			BDADDR_SIZE) == 0)))) ||
	     ((pdu_adv_rx->type == PDU_ADV_TYPE_DIRECT_IND) &&
	      (/* allow directed adv packets addressed to this device */
	       ((_radio.observer.init_addr_type == pdu_adv_rx->rx_addr) &&
		(memcmp(&_radio.observer.init_addr[0],
			&pdu_adv_rx->payload.direct_ind.init_addr[0],
			BDADDR_SIZE) == 0)) ||
	       /* allow directed adv packets where initiator address
		* is resolvable private address
		*/
	       (((_radio.observer.filter_policy & 0x02) != 0) &&
		(pdu_adv_rx->rx_addr != 0) &&
		((pdu_adv_rx->payload.direct_ind.init_addr[5] & 0xc0) == 0x40))))) &&
	    ((radio_tmr_end_get() + 502) <
	     TICKER_TICKS_TO_US(_radio.observer.hdr.ticks_slot))) {
		struct connection *conn;
		struct pdu_adv *pdu_adv_tx;
		struct pdu_data *pdu_data;
		struct radio_le_conn_cmplt
		    *radio_le_conn_cmplt;
		uint32_t ticker_status;
		uint32_t ticks_slot_offset;
		uint32_t conn_interval_us;
		uint32_t conn_space_us;

		_radio.state = STATE_STOP;

		/* acquire the master context from observer */
		conn = _radio.observer.conn;
		_radio.observer.conn = NULL;

		/* Tx the connect request packet */
		pdu_adv_tx = (struct pdu_adv *)radio_pkt_scratch_get();
		pdu_adv_tx->type = PDU_ADV_TYPE_CONNECT_REQ;
		pdu_adv_tx->tx_addr = _radio.observer.init_addr_type;
		pdu_adv_tx->rx_addr = pdu_adv_rx->tx_addr;
		pdu_adv_tx->len = sizeof(struct pdu_adv_payload_connect_req);
		memcpy(&pdu_adv_tx->payload.connect_req.init_addr[0],
		       &_radio.observer.init_addr[0], BDADDR_SIZE);
		memcpy(&pdu_adv_tx->payload.connect_req.adv_addr[0],
			 &pdu_adv_rx->payload.adv_ind.addr[0], BDADDR_SIZE);
		memcpy(&pdu_adv_tx->payload.connect_req.lldata.
		       access_addr[0], &conn->access_addr[0], 4);
		memcpy(&pdu_adv_tx->payload.connect_req.lldata.crc_init[0],
		       &conn->crc_init[0], 3);
		pdu_adv_tx->payload.connect_req.lldata. win_size = 1;

		conn_interval_us =
			(uint32_t)_radio.observer.conn_interval * 1250;
		if (_radio.observer.win_offset_us == 0) {
			conn_space_us = radio_tmr_end_get() -
				RADIO_TX_CHAIN_DELAY_US + 502 + 1250 -
				RADIO_TX_READY_DELAY_US;
			pdu_adv_tx->payload.connect_req.lldata.win_offset = 0;
		} else {
			conn_space_us = _radio.observer. win_offset_us;
			while ((conn_space_us & ((uint32_t)1 << 31)) ||
			       (conn_space_us < (radio_tmr_end_get() -
						 RADIO_TX_CHAIN_DELAY_US +
						 502 + 1250 -
						 RADIO_TX_READY_DELAY_US))) {
				conn_space_us += conn_interval_us;
			}
			pdu_adv_tx->payload.connect_req.lldata. win_offset =
				(conn_space_us - radio_tmr_end_get() +
				 RADIO_TX_CHAIN_DELAY_US - 502 - 1250 +
				 RADIO_TX_READY_DELAY_US) / 1250;
		}

		pdu_adv_tx->payload.connect_req.lldata.interval =
			_radio.observer.conn_interval;
		pdu_adv_tx->payload.connect_req.lldata.latency =
			_radio.observer.conn_latency;
		pdu_adv_tx->payload.connect_req.lldata.timeout =
			_radio.observer.conn_timeout;
		memcpy(&pdu_adv_tx->payload.connect_req.lldata.channel_map[0],
		       &conn->data_channel_map[0],
		       sizeof(pdu_adv_tx->payload.connect_req.lldata.channel_map));
		pdu_adv_tx->payload.connect_req.lldata.hop =
			conn->data_channel_hop;
		pdu_adv_tx->payload.connect_req.lldata.sca = _radio.sca;

		radio_switch_complete_and_disable();

		radio_pkt_tx_set(pdu_adv_tx);

		LL_ASSERT(!radio_is_ready());

		radio_tmr_end_capture();

		/* block CPU so that there is no CRC error on pdu tx,
		 * this is only needed if we want the CPU to sleep.
		 * while(!radio_has_disabled())
		 * {cpu_sleep();}
		 * radio_status_reset();
		 */

		/* Populate the master context */
		conn->handle = mem_index_get(conn, _radio.conn_pool,
					     CONNECTION_T_SIZE);

		/* Prepare the rx packet structure */
		radio_pdu_node_rx->hdr.handle = conn->handle;
		radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_CONNECTION;

		/* prepare connection complete structure */
		pdu_data = (struct pdu_data *)radio_pdu_node_rx->pdu_data;
		radio_le_conn_cmplt =
		    (struct radio_le_conn_cmplt *)&pdu_data->payload;
		radio_le_conn_cmplt->status = 0x00;
		radio_le_conn_cmplt->role = 0x00;
		radio_le_conn_cmplt->peer_addr_type = pdu_adv_tx->rx_addr;
		memcpy(&radio_le_conn_cmplt->peer_addr[0],
		       &pdu_adv_tx->payload. connect_req.adv_addr[0],
		       BDADDR_SIZE);
		radio_le_conn_cmplt->own_addr_type = pdu_adv_tx->tx_addr;
		memcpy(&radio_le_conn_cmplt->own_addr[0],
		       &pdu_adv_tx->payload. connect_req.init_addr[0],
		       BDADDR_SIZE);
		radio_le_conn_cmplt->peer_irk_index = irkmatch_id;
		radio_le_conn_cmplt->interval = _radio.observer.conn_interval;
		radio_le_conn_cmplt->latency = _radio.observer. conn_latency;
		radio_le_conn_cmplt->timeout = _radio.observer.conn_timeout;
		radio_le_conn_cmplt->mca =
			pdu_adv_tx->payload.connect_req.lldata.sca;

		/* enqueue connection complete structure into queue */
		rx_fc_lock(conn->handle);
		packet_rx_enqueue();

		/* Calculate master slot */
		conn->hdr.ticks_slot = _radio.observer.ticks_conn_slot;
		conn->hdr.ticks_active_to_start = _radio.ticks_active_to_start;
		conn->hdr.ticks_xtal_to_start =
			TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US);
		conn->hdr.ticks_preempt_to_start =
			TICKER_US_TO_TICKS(RADIO_TICKER_PREEMPT_PART_MIN_US);
		ticks_slot_offset =
			(conn->hdr. ticks_active_to_start <
			 conn->hdr.ticks_xtal_to_start) ?
			conn->hdr.ticks_xtal_to_start :
			conn->hdr.ticks_active_to_start;

		/* Stop Observer and start Master */
		ticker_status = ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    RADIO_TICKER_ID_OBS,
					    ticker_success_assert,
					    (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
		ticker_status = ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				RADIO_TICKER_USER_ID_WORKER,
				RADIO_TICKER_ID_OBS_STOP,
				0,	/* @todo ticker_success_assert */
				0	/* @todo (void *) __LINE__ */);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_FIRST_CONNECTION +
				     conn->handle,
				     (_radio.ticks_anchor - ticks_slot_offset),
				     TICKER_US_TO_TICKS(conn_space_us),
				     TICKER_US_TO_TICKS(conn_interval_us),
				     TICKER_REMAINDER(conn_interval_us),
				     TICKER_NULL_LAZY,
				     (ticks_slot_offset + conn->hdr.ticks_slot),
				     event_master_prepare, conn,
				     ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		return 0;
	}

	/* Active scanner */
	else if (((pdu_adv_rx->type == PDU_ADV_TYPE_ADV_IND) ||
		  (pdu_adv_rx->type == PDU_ADV_TYPE_SCAN_IND)) &&
		 (_radio.observer.scan_type != 0) &&
		 (_radio.observer.conn == 0)) {
		struct pdu_adv *pdu_adv_tx;

		/* save the RSSI value */
		((uint8_t *)pdu_adv_rx)[offsetof(struct pdu_adv, payload) +
			pdu_adv_rx->len] =
			(rssi_ready) ? (radio_rssi_get() & 0x7F) : 0x7F;

		/* save the adv packet */
		radio_pdu_node_rx->hdr.handle = 0xffff;
		radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_REPORT;
		packet_rx_enqueue();

		/* prepare the scan request packet */
		pdu_adv_tx = (struct pdu_adv *)radio_pkt_scratch_get();
		pdu_adv_tx->type = PDU_ADV_TYPE_SCAN_REQ;
		pdu_adv_tx->tx_addr = _radio.observer.init_addr_type;
		pdu_adv_tx->rx_addr = pdu_adv_rx->tx_addr;
		pdu_adv_tx->len = sizeof(struct pdu_adv_payload_scan_req);
		memcpy(&pdu_adv_tx->payload.scan_req.scan_addr[0],
		       &_radio.observer.init_addr[0], BDADDR_SIZE);
		memcpy(&pdu_adv_tx->payload.scan_req.adv_addr[0],
		       &pdu_adv_rx->payload.adv_ind.addr[0], BDADDR_SIZE);

		/* switch scanner state to active */
		_radio.observer.scan_state = 1;
		_radio.state = STATE_TX;

		radio_pkt_tx_set(pdu_adv_tx);
		radio_switch_complete_and_rx();
		radio_tmr_end_capture();

		return 0;
	}
	/* Passive scanner or scan responses */
	else if (((pdu_adv_rx->type == PDU_ADV_TYPE_ADV_IND) ||
		  ((pdu_adv_rx->type == PDU_ADV_TYPE_DIRECT_IND) &&
		   (/* allow directed adv packets addressed to this device */
		    ((_radio.observer.init_addr_type == pdu_adv_rx->rx_addr) &&
		     (memcmp(&_radio.observer.init_addr[0],
			     &pdu_adv_rx->payload.direct_ind.init_addr[0],
			     BDADDR_SIZE) == 0)) ||
		    /* allow directed adv packets where initiator address
		     * is resolvable private address
		     */
		    (((_radio.observer.filter_policy & 0x02) != 0) &&
		     (pdu_adv_rx->rx_addr != 0) &&
		     ((pdu_adv_rx->payload.direct_ind.init_addr[5] & 0xc0) == 0x40)))) ||
		  (pdu_adv_rx->type == PDU_ADV_TYPE_NONCONN_IND) ||
		  (pdu_adv_rx->type == PDU_ADV_TYPE_SCAN_IND) ||
		  ((pdu_adv_rx->type == PDU_ADV_TYPE_SCAN_RESP) &&
		   (_radio.observer.scan_state != 0))) &&
		 (pdu_adv_rx->len != 0) && (!_radio.observer.conn)) {
		/* save the RSSI value */
		((uint8_t *)pdu_adv_rx)[offsetof(struct pdu_adv, payload) +
			pdu_adv_rx->len] =
			(rssi_ready) ? (radio_rssi_get() & 0x7f) : 0x7f;

		/* save the scan response packet */
		radio_pdu_node_rx->hdr.handle = 0xffff;
		radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_REPORT;
		packet_rx_enqueue();
	}
	/* invalid PDU */
	else {
		/* ignore and close this rx/tx chain ( code below ) */
		return 1;
	}

	return 1;
}

static inline uint8_t isr_rx_conn_pkt_ack(struct pdu_data *pdu_data_tx,
					  struct radio_pdu_node_tx **node_tx)
{
	uint8_t terminate = 0;

	switch (pdu_data_tx->payload.llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_TERMINATE_IND:
		_radio.state = STATE_CLOSE;
		radio_disable();

		terminate_ind_rx_enqueue(_radio.conn_curr,
		     (pdu_data_tx->payload.llctrl.ctrldata.terminate_ind.
		      error_code == 0x13) ?  0x16 :
		     pdu_data_tx->payload.llctrl.ctrldata.terminate_ind.
		     error_code);

		/* Ack received, hence terminate */
		terminate = 1;
		break;

	case PDU_DATA_LLCTRL_TYPE_ENC_REQ:
		/* things from master stored for session key calculation */
		memcpy(&_radio.conn_curr->llcp.encryption.skd[0],
		       &pdu_data_tx->payload.llctrl.ctrldata.enc_req.skdm[0],
		       8);
		memcpy(&_radio.conn_curr->ccm_rx.iv[0],
		       &pdu_data_tx->payload.llctrl.ctrldata.enc_req.ivm[0],
		       4);

		/* pause data packet tx */
		_radio.conn_curr->pause_tx = 1;

		/* Start Procedure Timeout (this will not replace terminate
		 * procedure which always gets place before any packets
		 * going out, hence safe by design).
		 */
		_radio.conn_curr->procedure_expire =
			_radio.conn_curr->procedure_reload;
		break;

	case PDU_DATA_LLCTRL_TYPE_ENC_RSP:
		/* pause data packet tx */
		_radio.conn_curr->pause_tx = 1;
		break;

	case PDU_DATA_LLCTRL_TYPE_START_ENC_REQ:
		/* Nothing to do.
		 * Remember that we may have received encrypted START_ENC_RSP
		 * alongwith this tx ack at this point in time.
		 */
		break;

	case PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_REQ:
		/* pause data packet tx */
		_radio.conn_curr->pause_tx = 1;

		/* key refresh */
		_radio.conn_curr->refresh = 1;

		/* Start Procedure Timeout (this will not replace terminate
		 * procedure which always gets place before any packets
		 * going out, hence safe by design).
		 */
		_radio.conn_curr->procedure_expire =
			_radio.conn_curr->procedure_reload;
		break;

	case PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_RSP:
		if (_radio.role == ROLE_MASTER) {
			/* reused tx-ed PDU and send enc req */
			enc_req_reused_send(_radio.conn_curr, *node_tx);

			/* dont release ctrl PDU memory */
			*node_tx = NULL;
		} else {
			/* pause data packet tx */
			_radio.conn_curr->pause_tx = 1;
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_REJECT_IND:
		/* resume data packet rx and tx */
		_radio.conn_curr->pause_rx = 0;
		_radio.conn_curr->pause_tx = 0;

		/* Procedure complete */
		_radio.conn_curr->procedure_expire = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_LENGTH_REQ:
		if ((_radio.conn_curr->llcp_length.req !=
		     _radio.conn_curr->llcp_length.ack) &&
		    (_radio.conn_curr->llcp_length.state ==
		     LLCP_LENGTH_STATE_ACK_WAIT)){
			/* pause data packet tx */
			_radio.conn_curr->pause_tx = 1;

			/* wait for response */
			_radio.conn_curr->llcp_length.state =
				LLCP_LENGTH_STATE_RSP_WAIT;
		}
		break;

	default:
		/* Do nothing for other ctrl packet ack */
		break;
	}

	return terminate;
}

static inline void isr_rx_conn_pkt_release(struct radio_pdu_node_tx *node_tx)
{

	_radio.conn_curr->packet_tx_head_len = 0;
	_radio.conn_curr->packet_tx_head_offset = 0;

	/* release */
	if (_radio.conn_curr->pkt_tx_head == _radio.conn_curr->pkt_tx_ctrl) {
		if (node_tx) {
			_radio.conn_curr->pkt_tx_ctrl =
			    _radio.conn_curr->pkt_tx_ctrl->next;
			_radio.conn_curr->pkt_tx_head =
				_radio.conn_curr->pkt_tx_ctrl;
			if (_radio.conn_curr->pkt_tx_ctrl ==
			    _radio.conn_curr->pkt_tx_data) {
				_radio.conn_curr->pkt_tx_ctrl = NULL;
			}

			mem_release(node_tx, &_radio. pkt_tx_ctrl_free);
		}
	} else {
		if (_radio.conn_curr->pkt_tx_head ==
		    _radio.conn_curr->pkt_tx_data) {
			_radio.conn_curr->pkt_tx_data =
				_radio.conn_curr->pkt_tx_data->next;
		}
		_radio.conn_curr->pkt_tx_head =
			_radio.conn_curr->pkt_tx_head->next;

		pdu_node_tx_release(_radio.conn_curr->handle, node_tx);
	}
}

static inline void
isr_rx_conn_pkt_ctrl_rej(struct radio_pdu_node_rx *radio_pdu_node_rx,
			 struct pdu_data *pdu_data_rx, uint8_t *rx_enqueue)
{
	/* reset ctrl procedure */
	_radio.conn_curr->llcp_ack = _radio.conn_curr->llcp_req;

	switch (_radio.conn_curr->llcp_type) {
	case LLCP_CONNECTION_UPDATE:
		LL_ASSERT(_radio.conn_upd == _radio.conn_curr);

		/* reset mutex */
		_radio.conn_upd = NULL;

		/* update to next ticks offsets */
		if (_radio.conn_curr->role.slave.role != 0) {
			_radio.conn_curr->role.slave.ticks_to_offset =
			    _radio.conn_curr->llcp.connection_update.
			    ticks_to_offset_next;
		}

		/* conn param req procedure, if any, is complete */
		_radio.conn_curr->procedure_expire = 0;

		/* enqueue the reject ind ext */
		if (!_radio.conn_curr->llcp.connection_update.is_internal) {
			struct radio_le_conn_update_cmplt
				*radio_le_conn_update_cmplt;

			radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_CONN_UPDATE;

			/* prepare connection update complete structure */
			radio_le_conn_update_cmplt =
				(struct radio_le_conn_update_cmplt *)
				&pdu_data_rx->payload;
			radio_le_conn_update_cmplt->status = 0x00;
			radio_le_conn_update_cmplt->interval =
				_radio.conn_curr->conn_interval;
			radio_le_conn_update_cmplt->latency =
				_radio.conn_curr->latency;
			radio_le_conn_update_cmplt->timeout =
				_radio.conn_curr->supervision_reload *
				_radio.conn_curr->conn_interval * 125 / 1000;

			*rx_enqueue = 1;
		}
		break;

	default:
		LL_ASSERT(0);
		break;
	}

}

static inline void isr_rx_conn_pkt_ctrl_dle(struct pdu_data *pdu_data_rx,
		uint8_t *rx_enqueue)
{
	uint16_t eff_rx_octets;
	uint16_t eff_tx_octets;
	uint8_t no_resp = 0;

	eff_rx_octets = _radio.conn_curr->max_rx_octets;
	eff_tx_octets = _radio.conn_curr->max_tx_octets;

	if (/* Local idle, and peer request; complete the peer procedure
	     * with response.
	     */
	    ((_radio.conn_curr->llcp_length.req ==
	      _radio.conn_curr->llcp_length.ack) &&
	     (PDU_DATA_LLCTRL_TYPE_LENGTH_REQ ==
	      pdu_data_rx->payload.llctrl.opcode)) ||
	    /* or Local has requested... */
	    ((_radio.conn_curr->llcp_length.req !=
	      _radio.conn_curr->llcp_length.ack) &&
	     /* and Local request, and peer request; override with peer
	      * procedure, and complete the peer procedure with response.
	      */
	     (((LLCP_LENGTH_STATE_REQ == _radio.conn_curr->llcp_length.state) &&
	       (PDU_DATA_LLCTRL_TYPE_LENGTH_REQ ==
		pdu_data_rx->payload.llctrl.opcode)) ||
	      /* and Local wait, and peer response; complete the
	       * local procedure.
	       */
	      ((LLCP_LENGTH_STATE_RSP_WAIT ==
		_radio.conn_curr->llcp_length.state) &&
	       (PDU_DATA_LLCTRL_TYPE_LENGTH_RSP ==
		pdu_data_rx->payload.llctrl.opcode))))) {

		struct pdu_data_llctrl_length_req_rsp *lr;

		lr = (struct pdu_data_llctrl_length_req_rsp *)
			&pdu_data_rx->payload.llctrl.ctrldata.length_req;

		/* use the minimal of our default_tx_octets and
		 * peer max_rx_octets
		 */
		eff_tx_octets = min(lr->max_rx_octets,
				    _radio.conn_curr->default_tx_octets);

		/* use the minimal of our max supported and
		 * peer max_tx_octets
		 */
		eff_rx_octets = min(lr->max_tx_octets,
				    RADIO_LL_LENGTH_OCTETS_RX_MAX);

		/* check if change in rx octets */
		if (eff_rx_octets != _radio.conn_curr->max_rx_octets) {
			uint16_t free_count_rx;

			free_count_rx = packet_rx_acquired_count_get()
				+ mem_free_count_get(_radio.pkt_rx_data_free);
			LL_ASSERT(free_count_rx <= 0xFF);

			if (_radio.packet_rx_data_count == free_count_rx) {

				/* accept the effective tx */
				_radio.conn_curr->max_tx_octets = eff_tx_octets;

				/* trigger or retain the ctrl procedure so as
				 * to resize the rx buffers.
				 */
				_radio.conn_curr->llcp_length.rx_octets =
					eff_rx_octets;
				_radio.conn_curr->llcp_length.tx_octets =
					eff_tx_octets;
				_radio.conn_curr->llcp_length.ack =
					(_radio.conn_curr->llcp_length.req - 1);
				_radio.conn_curr->llcp_length.state =
					LLCP_LENGTH_STATE_RESIZE;

				/* close the current connection event, so as
				 * to perform rx octet change.
				 */
				_radio.state = STATE_CLOSE;
			} else {
				/* nack ctrl packet */
				_radio.conn_curr->nesn--;

				no_resp = 1;
			}
		} else {
			/* resume data packet tx */
			_radio.conn_curr->pause_tx = 0;

			/* accept the effective tx */
			_radio.conn_curr->max_tx_octets = eff_tx_octets;

			/* Procedure complete */
			_radio.conn_curr->llcp_length.ack =
				_radio.conn_curr->llcp_length.req;
			_radio.conn_curr->procedure_expire = 0;

			/* prepare event params */
			lr->max_rx_octets = eff_rx_octets;
			lr->max_rx_time = ((eff_rx_octets + 14) << 3);
			lr->max_tx_octets = eff_tx_octets;
			lr->max_tx_time = ((eff_tx_octets + 14) << 3);

			/* Enqueue data length change event (with no change in
			 * rx length happened), safe to enqueue rx.
			 */
			*rx_enqueue = 1;
		}
	} else {
		LL_ASSERT(0);
	}

	if ((PDU_DATA_LLCTRL_TYPE_LENGTH_REQ ==
	     pdu_data_rx->payload.llctrl.opcode) && !no_resp) {
		length_resp_send(_radio.conn_curr, eff_rx_octets,
				 eff_tx_octets);
	}
}

static inline void
isr_rx_conn_pkt_ctrl(struct radio_pdu_node_rx *radio_pdu_node_rx,
		     struct pdu_data *pdu_data_rx, uint8_t *rx_enqueue)
{
	switch (pdu_data_rx->payload.llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_REQ:
		if (conn_update(_radio.conn_curr, pdu_data_rx) == 0) {
			/* conn param req procedure, if any, is complete */
			_radio.conn_curr->procedure_expire = 0;
		} else {
			_radio.conn_curr->llcp_terminate.reason_peer = 0x28;
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_CHANNEL_MAP_REQ:
		if (channel_map_update(_radio.conn_curr, pdu_data_rx)) {
			_radio.conn_curr->llcp_terminate.reason_peer = 0x28;
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_TERMINATE_IND:
		/* Ack and then terminate */
		_radio.conn_curr->llcp_terminate.reason_peer =
			pdu_data_rx->payload.llctrl.ctrldata.terminate_ind.error_code;
		break;

	case PDU_DATA_LLCTRL_TYPE_ENC_REQ:
		/* things from master stored for session key calculation */
		memcpy(&_radio.conn_curr->llcp.encryption.skd[0],
		       &pdu_data_rx->payload.llctrl.ctrldata.enc_req.skdm[0],
		       8);
		memcpy(&_radio.conn_curr->ccm_rx.iv[0],
		       &pdu_data_rx->payload.llctrl.ctrldata.enc_req.ivm[0], 4);

		/* pause rx data packets */
		_radio.conn_curr->pause_rx = 1;

		/* Start Procedure Timeout (this will not replace terminate
		 * procedure which always gets place before any packets
		 * going out, hence safe by design)
		 */
		_radio.conn_curr->procedure_expire =
			_radio.conn_curr->procedure_reload;

		/* TODO: remove this code block.
		 * test peer master for overlapping contrl procedure.
		 */
#if 0
		if (_radio.conn_curr->llcp_version.tx == 0) {
			_radio.conn_curr->llcp_version.tx = 1;

			version_ind_send(_radio.conn_curr);
		}
#endif

#if !!FAST_ENC_PROCEDURE
		/* TODO BT Spec. text: may finalize the sending of additional
		 * data channel PDUs queued in the controller.
		 */
		enc_rsp_send(_radio.conn_curr);
#endif

		/* enqueue the enc req */
		*rx_enqueue = 1;
		break;

	case PDU_DATA_LLCTRL_TYPE_ENC_RSP:
		/* things sent by slave stored for session key calculation */
		memcpy(&_radio.conn_curr->llcp.encryption.skd[8],
		       &pdu_data_rx->payload.llctrl.ctrldata.enc_rsp.skds[0],
		       8);
		memcpy(&_radio.conn_curr->ccm_rx.iv[4],
		       &pdu_data_rx->payload.llctrl.ctrldata.enc_rsp.ivs[0],
		       4);

		/* pause rx data packets */
		_radio.conn_curr->pause_rx = 1;
		break;

	case PDU_DATA_LLCTRL_TYPE_START_ENC_REQ:
		LL_ASSERT(_radio.conn_curr->llcp_req ==
			  _radio.conn_curr->llcp_ack);

		/* start enc rsp to be scheduled in master prepare */
		_radio.conn_curr->llcp_type = LLCP_ENCRYPTION;
		_radio.conn_curr->llcp_ack--;
		break;

	case PDU_DATA_LLCTRL_TYPE_START_ENC_RSP:
		if (_radio.role == ROLE_SLAVE) {
#if !FAST_ENC_PROCEDURE
			LL_ASSERT(_radio.conn_curr->llcp_req ==
				  _radio.conn_curr->llcp_ack);

			/* start enc rsp to be scheduled in slave  prepare */
			_radio.conn_curr->llcp_type = LLCP_ENCRYPTION;
			_radio.conn_curr->llcp_ack--;
#else
			/* enable transmit encryption */
			_radio.conn_curr->enc_tx = 1;

			start_enc_rsp_send(_radio.conn_curr, 0);

			/* resume data packet rx and tx */
			_radio.conn_curr->pause_rx = 0;
			_radio.conn_curr->pause_tx = 0;
#endif
		} else {
			/* resume data packet rx and tx */
			_radio.conn_curr->pause_rx = 0;
			_radio.conn_curr->pause_tx = 0;
		}

		/* enqueue the start enc resp (encryption change/refresh) */
		if (_radio.conn_curr->refresh) {
			_radio.conn_curr->refresh = 0;

			/* key refresh event */
			radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_ENC_REFRESH;
		}
		*rx_enqueue = 1;

		/* Procedure complete */
		_radio.conn_curr->procedure_expire = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_FEATURE_REQ:
	case PDU_DATA_LLCTRL_TYPE_SLAVE_FEATURE_REQ:
		/* AND the feature set to get Feature USED */
		_radio.conn_curr->llcp_features &=
			pdu_data_rx->payload.llctrl.ctrldata.feature_req.features[0];

		feature_rsp_send(_radio.conn_curr);
		break;

	case PDU_DATA_LLCTRL_TYPE_FEATURE_RSP:
		/* AND the feature set to get Feature USED */
		_radio.conn_curr->llcp_features &=
			pdu_data_rx->payload.llctrl.ctrldata.feature_rsp.features[0];

		/* enqueue the feature resp */
		*rx_enqueue = 1;

		/* Procedure complete */
		_radio.conn_curr->procedure_expire = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_REQ:
		pause_enc_rsp_send(_radio.conn_curr);

		/* pause data packet rx */
		_radio.conn_curr->pause_rx = 1;

		/* key refresh */
		_radio.conn_curr->refresh = 1;

		/* disable receive encryption */
		_radio.conn_curr->enc_rx = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_RSP:
		if (_radio.role == ROLE_MASTER) {
			/* reply with pause enc rsp */
			pause_enc_rsp_send(_radio.conn_curr);

			/* disable receive encryption */
			_radio.conn_curr->enc_rx = 0;
		}

		/* pause data packet rx */
		_radio.conn_curr->pause_rx = 1;

		/* disable transmit encryption */
		_radio.conn_curr->enc_tx = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_VERSION_IND:
		_radio.conn_curr->llcp_version.version_number =
			pdu_data_rx->payload.llctrl.ctrldata.
			version_ind.version_number;
		_radio.conn_curr->llcp_version. company_id =
			pdu_data_rx->payload.llctrl.ctrldata.version_ind.company_id;
		_radio.conn_curr->llcp_version.sub_version_number =
			pdu_data_rx->payload.llctrl.ctrldata.version_ind.sub_version_number;

		if ((_radio.conn_curr->llcp_version.tx != 0) &&
		    (_radio.conn_curr->llcp_version.rx == 0)) {
			/* enqueue the version ind */
			*rx_enqueue = 1;

			/* Procedure complete */
			_radio.conn_curr->procedure_expire = 0;
		}

		_radio.conn_curr->llcp_version.rx = 1;

		if (_radio.conn_curr->llcp_version.tx == 0) {
			_radio.conn_curr->llcp_version.tx = 1;

			version_ind_send(_radio.conn_curr);
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_REJECT_IND:
		/* resume data packet rx and tx */
		_radio.conn_curr->pause_rx = 0;
		_radio.conn_curr->pause_tx = 0;

		/* Procedure complete */
		_radio.conn_curr->procedure_expire = 0;

		/* enqueue the reject ind */
		*rx_enqueue = 1;
		break;

	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ:
		/* connection update or params req in progress
		 * 1. if connection update in progress, then both master and
		 * slave ignore this param req (in out impl. we assert,
		 * see below assert).
		 * 2. if connection param req to be initiated, slave drop
		 * initiation and respond to this req, master ignore this
		 * param req and continue to initiate.
		 * 3. if connection param rsp waited for, slave drop waiting
		 * and respond to this req, master ignore this param req and
		 * master continue waiting.
		 */

		/* no ctrl procedures in progress or master req while slave
		 * waiting resp.
		 */
		if (((_radio.conn_curr->llcp_req == _radio.conn_curr->llcp_ack) &&
		     (_radio.conn_upd == 0)) ||
		    ((_radio.conn_curr->llcp_req != _radio.conn_curr->llcp_ack) &&
		     (_radio.conn_curr->role.slave.role != 0) &&
		     (_radio.conn_curr == _radio.conn_upd) &&
		     (_radio.conn_curr->llcp_type == LLCP_CONNECTION_UPDATE) &&
		     ((_radio.conn_curr->llcp.connection_update.state ==
		       LLCP_CONN_STATE_INITIATE) ||
		      (_radio.conn_curr->llcp.connection_update.state ==
		       LLCP_CONN_STATE_REQ) ||
		      (_radio.conn_curr->llcp.connection_update.state ==
		       LLCP_CONN_STATE_RSP_WAIT)))) {
			/* set mutex */
			if (_radio.conn_upd == 0) {
				_radio.conn_upd = _radio.conn_curr;
			}

			/* resp to be generated by app, for now save
			 * parameters
			 */
			_radio.conn_curr->llcp.connection_update.interval =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.interval_min;
			_radio.conn_curr->llcp.connection_update.latency =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.latency;
			_radio.conn_curr->llcp.connection_update.timeout =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.timeout;
			_radio.conn_curr->llcp.connection_update.preferred_periodicity =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.preferred_periodicity;
			_radio.conn_curr->llcp.connection_update.instant =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.
				reference_conn_event_count;
			_radio.conn_curr->llcp.connection_update.offset0 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset0;
			_radio.conn_curr->llcp.connection_update.offset1 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset1;
			_radio.conn_curr->llcp.connection_update.offset2 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset2;
			_radio.conn_curr->llcp.connection_update.offset3 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset3;
			_radio.conn_curr->llcp.connection_update.offset4 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset4;
			_radio.conn_curr->llcp.connection_update.offset5 =
				pdu_data_rx->payload.llctrl.ctrldata.conn_param_req.offset5;

			/* enqueue the conn param req, if parameters changed,
			 * else respond
			 */
			if ((_radio.conn_curr->llcp.connection_update.interval !=
			     _radio.conn_curr->conn_interval) ||
			    (_radio.conn_curr->llcp.connection_update.latency
			     != _radio.conn_curr->latency) ||
			    (_radio.conn_curr->llcp.connection_update.timeout !=
			     (_radio.conn_curr->conn_interval *
			      _radio.conn_curr->supervision_reload * 125 / 1000))) {
				*rx_enqueue = 1;

				_radio.conn_curr->llcp.connection_update.state =
					LLCP_CONN_STATE_APP_WAIT;
				_radio.conn_curr->llcp.connection_update.is_internal = 0;
				_radio.conn_curr->llcp_type = LLCP_CONNECTION_UPDATE;
				_radio.conn_curr->llcp_ack--;
			} else {
				_radio.conn_curr->llcp.connection_update.win_size = 1;
				_radio.conn_curr->llcp.connection_update.win_offset_us = 0;
				_radio.conn_curr->llcp.connection_update.state =
					LLCP_CONN_STATE_RSP;
				_radio.conn_curr->llcp.connection_update.is_internal = 0;

				_radio.conn_curr->llcp_type = LLCP_CONNECTION_UPDATE;
				_radio.conn_curr->llcp_ack--;
			}
		}
		/* master in conn update procedure, master in any state we
		 * ignore this req
		 */
		else if ((_radio.conn_curr->llcp_req !=
			  _radio.conn_curr->llcp_ack) &&
			 (_radio.conn_curr->role.master.role == 0) &&
			 (_radio.conn_curr == _radio.conn_upd) &&
			 (_radio.conn_curr->llcp_type == LLCP_CONNECTION_UPDATE)) {
			/* ignore this req, as master continue initiating or
			 * waiting for resp
			 */
		}
		/* no ctrl procedure in this connection, but conn update mutex
		 * set (another connection update in progress), hence reject
		 * this req.
		 */
		else if (_radio.conn_curr->llcp_req ==
			 _radio.conn_curr->llcp_ack) {
			reject_ind_ext_send(_radio.conn_curr,
					PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ,
					 /* @todo use correct error_code */
					0x20);
		} else {
			/* different transaction collision */
			/* 1. conn update in progress, instant not reached */
			/* 2. some other ctrl procedure in progress */

			LL_ASSERT(0);
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP:
		/* @todo send conn_update req */
		break;

	case PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT:
		if (_radio.conn_curr->llcp_req != _radio.conn_curr->llcp_ack) {
			isr_rx_conn_pkt_ctrl_rej(radio_pdu_node_rx,
					pdu_data_rx, rx_enqueue);

		} else {
			/* By spec. slave shall not generate a conn update
			 * complete on reject from master.
			 */
			LL_ASSERT(_radio.conn_curr->role.slave.role);
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_PING_REQ:
		ping_resp_send(_radio.conn_curr);
		break;

	case PDU_DATA_LLCTRL_TYPE_PING_RSP:
		/* Procedure complete */
		_radio.conn_curr->procedure_expire = 0;
		break;

	case PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP:
		if (_radio.conn_curr->llcp_req != _radio.conn_curr->llcp_ack) {
			/* reset ctrl procedure */
			_radio.conn_curr->llcp_ack = _radio.conn_curr->llcp_req;

			switch (_radio.conn_curr->llcp_type) {
			default:
				LL_ASSERT(0);
				break;
			}
		} else if (_radio.conn_curr->llcp_length.req !=
			   _radio.conn_curr->llcp_length.ack) {
			/* Procedure complete */
			_radio.conn_curr->procedure_expire = 0;
			_radio.conn_curr->llcp_length.ack =
				_radio.conn_curr->llcp_length.req;

			/* resume data packet tx */
			_radio.conn_curr->pause_tx = 0;

			/* propagate the data length procedure to
			 * host
			 */
			*rx_enqueue = 1;
		} else {
			/* enqueue the error and let HCI handle it */
			*rx_enqueue = 1;
			/* Procedure complete */
			_radio.conn_curr->procedure_expire = 0;
		}
		break;

	case PDU_DATA_LLCTRL_TYPE_LENGTH_RSP:
	case PDU_DATA_LLCTRL_TYPE_LENGTH_REQ:
		isr_rx_conn_pkt_ctrl_dle(pdu_data_rx, rx_enqueue);
		break;

	default:
		unknown_rsp_send(_radio.conn_curr,
				 pdu_data_rx->payload.llctrl.opcode);
		break;
	}

}

static inline uint32_t
isr_rx_conn_pkt(struct radio_pdu_node_rx *radio_pdu_node_rx,
		struct pdu_data *pdu_data_rx)
{
	uint8_t terminate = 0;
	struct pdu_data *pdu_data_tx;

	/* Reset CRC expiry counter */
	_radio.crc_expire = 0;

	/* Ack for transmitted data */
	if (pdu_data_rx->nesn != _radio.conn_curr->sn) {

		/* Increment serial number */
		_radio.conn_curr->sn++;

		if (_radio.conn_curr->empty == 0) {
			struct radio_pdu_node_tx *node_tx;
			uint8_t pdu_data_tx_len, pdu_data_tx_ll_id;

			node_tx = _radio.conn_curr->pkt_tx_head;
			pdu_data_tx = (struct pdu_data *)
				(node_tx->pdu_data +
				 _radio.conn_curr->packet_tx_head_offset);

			pdu_data_tx_len = pdu_data_tx->len;
			pdu_data_tx_ll_id = pdu_data_tx->ll_id;

			if (pdu_data_tx_len != 0) {
				/* if encrypted increment tx counter */
				if (_radio.conn_curr->enc_tx) {
					_radio.conn_curr->ccm_tx.counter++;
				}

				/* process ctrl packet on tx cmplt */
				if (pdu_data_tx_ll_id == PDU_DATA_LLID_CTRL) {
					terminate =
						isr_rx_conn_pkt_ack(pdu_data_tx,
								    &node_tx);
				}
			}

			_radio.conn_curr->packet_tx_head_offset += pdu_data_tx_len;
			if (_radio.conn_curr->packet_tx_head_offset ==
			    _radio.conn_curr->packet_tx_head_len) {
				isr_rx_conn_pkt_release(node_tx);
			}
		} else {
			_radio.conn_curr->empty = 0;
		}
	}

	/* local initiated disconnect procedure completed */
	if (terminate) {
		connection_release(_radio.conn_curr);
		_radio.conn_curr = NULL;

		return terminate;
	}

	/* process received data */
	if ((pdu_data_rx->sn == _radio.conn_curr->nesn) &&
	/* check so that we will NEVER use the rx buffer reserved for empty
	 * packet and internal control enqueue
	 */
	    (packet_rx_reserve_get(3) != 0) &&
	    ((_radio.fc_ena == 0) ||
	     ((_radio.link_rx_head == _radio.link_rx_tail) &&
	      (_radio.fc_req == _radio.fc_ack)) ||
	     ((_radio.link_rx_head != _radio.link_rx_tail) &&
	      (_radio.fc_req != _radio.fc_ack) &&
		(((_radio.fc_req == 0) &&
		  (_radio.fc_handle[TRIPLE_BUFFER_SIZE - 1] ==
		   _radio.conn_curr->handle)) ||
		 ((_radio.fc_req != 0) &&
		  (_radio.fc_handle[_radio.fc_req - 1] ==
		   _radio.conn_curr->handle)))))) {
		_radio.conn_curr->nesn++;

		if (pdu_data_rx->len != 0) {
			uint8_t rx_enqueue = 0;

			/* If required wait for CCM to finish and then
			 * increment counter
			 */
			if (_radio.conn_curr->enc_rx) {
				uint32_t done;

				done = radio_ccm_is_done();
				LL_ASSERT(done);

				_radio.conn_curr->ccm_rx.counter++;
			}

			/* MIC Failure Check or data rx during pause */
			if (((_radio.conn_curr->enc_rx)	&&
			     !radio_ccm_mic_is_valid()) ||
			    ((_radio.conn_curr->pause_rx) &&
			     (pdu_data_rx->ll_id != PDU_DATA_LLID_CTRL))) {
				_radio.state = STATE_CLOSE;
				radio_disable();

				terminate_ind_rx_enqueue(_radio.conn_curr,
							 0x3d);

				connection_release(_radio.conn_curr);
				_radio.conn_curr = NULL;

				return 1; /* terminated */
			}

			/* stop authenticated payload (pre) timeout */
			_radio.conn_curr->appto_expire = 0;
			_radio.conn_curr->apto_expire = 0;

			switch (pdu_data_rx->ll_id) {
			case PDU_DATA_LLID_DATA_CONTINUE:
			case PDU_DATA_LLID_DATA_START:
				/* enqueue data packet */
				rx_enqueue = 1;
				break;

			case PDU_DATA_LLID_CTRL:
				isr_rx_conn_pkt_ctrl(radio_pdu_node_rx,
						     pdu_data_rx, &rx_enqueue);
				break;
			case PDU_DATA_LLID_RESV:
			default:
				LL_ASSERT(0);
				break;
			}

			if (rx_enqueue) {
				rx_fc_lock(_radio.conn_curr->handle);

				/* as packet is to be enqueued, store the
				 * correct handle for it and enqueue it
				 */
				radio_pdu_node_rx->hdr.handle =
					_radio.conn_curr->handle;

				packet_rx_enqueue();
			}
		} else if ((_radio.conn_curr->enc_rx) ||
			   (_radio.conn_curr->pause_rx)) {
			/* start authenticated payload (pre) timeout */
			if (_radio.conn_curr->apto_expire == 0) {
				_radio.conn_curr->appto_expire =
					_radio.conn_curr->appto_reload;
				_radio.conn_curr->apto_expire =
					_radio.conn_curr->apto_reload;
			}
		}
	}

	return 0;
}

static inline void isr_rx_conn(uint8_t crc_ok, uint8_t trx_done,
			       uint8_t rssi_ready)
{

	struct radio_pdu_node_rx *radio_pdu_node_rx;
	struct pdu_data *pdu_data_rx;
	struct pdu_data *pdu_data_tx;
	uint8_t crc_close = 0;
	uint8_t is_empty_pdu_tx_retry;

	/* Collect RSSI for connection */
	if (_radio.packet_counter == 0) {
		if (rssi_ready) {
			uint8_t rssi = radio_rssi_get();

			_radio.conn_curr->rssi_latest = rssi;

			if (((_radio.conn_curr->rssi_reported - rssi) & 0xFF) >
			    RADIO_RSSI_THRESHOLD) {
				if (_radio.conn_curr->rssi_sample_count) {
					_radio.conn_curr->rssi_sample_count--;
				}
			} else {
				_radio.conn_curr->rssi_sample_count =
					RADIO_RSSI_SAMPLE_COUNT;
			}
		}
	}

	/* Increment packet counter for this connection event */
	_radio.packet_counter++;

	/* received data packet */
	radio_pdu_node_rx = _radio.packet_rx[_radio.packet_rx_last];
	radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_DC_PDU;
	pdu_data_rx = (struct pdu_data *)radio_pdu_node_rx->pdu_data;

	if (crc_ok) {
		uint32_t terminate;

		terminate = isr_rx_conn_pkt(radio_pdu_node_rx, pdu_data_rx);
		if (terminate) {
			return;
		}
	} else {
		/* Start CRC error countdown, if not already started */
		if (_radio.crc_expire == 0) {
			_radio.crc_expire = 2;
		}

		/* Check crc error countdown expiry */
		_radio.crc_expire--;
		crc_close = (_radio.crc_expire == 0);
	}

	/* prepare transmit packet */
	is_empty_pdu_tx_retry = _radio.conn_curr->empty;
	prepare_pdu_data_tx(_radio.conn_curr, &pdu_data_tx);

	/* silent connection */
	if (SILENT_CONNECTION) {
		/* slave silent, enter/be in supervision timeout */
		if (_radio.packet_counter == 0) {
			_radio.packet_counter = 0xFF;
		}

		/* master silent, hence avoid slave drift compensation, and
		 * close slave if no tx packets
		 */
		if (!trx_done) {
			/* avoid slave drift compensation if first packet
			 * missed
			 */
			if (_radio.packet_counter == 1) {
				_radio.packet_counter = 0xFF;
			}

			/* no Rx-ed packet and none to Tx, close event */
			if ((_radio.conn_curr->empty) &&
			    (pdu_data_tx->md == 0)) {
				_radio.state = STATE_CLOSE;
				radio_disable();
				return;
			}
		}
	}

	/* Decide on event continuation and hence Radio Shorts to use */
	_radio.state = ((_radio.state == STATE_CLOSE) || (crc_close) ||
			((crc_ok) && (pdu_data_rx->md == 0) &&
			 (pdu_data_tx->len == 0)) ||
			(_radio.conn_curr->llcp_terminate.reason_peer != 0)) ?
			STATE_CLOSE : STATE_TX;

	if (_radio.state == STATE_CLOSE) {
		/* Event close for master */
		if (_radio.role == ROLE_MASTER) {
			radio_disable();

			_radio.conn_curr->empty = is_empty_pdu_tx_retry;

			return;
		}
		/* Event close for slave */
		else {
			radio_switch_complete_and_disable();
		}
	} else {	/* if (_radio.state == STATE_TX) */

		radio_switch_complete_and_rx();
		radio_tmr_end_capture();
	}

	/* fill sn and nesn */
	pdu_data_tx->sn = _radio.conn_curr->sn;
	pdu_data_tx->nesn = _radio.conn_curr->nesn;

	/* Setup the radio tx packet buffer */
	tx_packet_set(_radio.conn_curr, pdu_data_tx);
}

static inline void isr_radio_state_rx(uint8_t trx_done, uint8_t crc_ok,
				      uint8_t devmatch_ok, uint8_t irkmatch_ok,
				      uint8_t irkmatch_id, uint8_t rssi_ready)
{
	uint32_t err;

	if (!((trx_done) || ((SILENT_CONNECTION) &&
			     (_radio.role == ROLE_SLAVE)))) {
		_radio.state = STATE_CLOSE;
		radio_disable();
		return;
	}

	switch (_radio.role) {
	case ROLE_ADV:
		if (crc_ok) {
			err = isr_rx_adv(devmatch_ok, irkmatch_ok,
					 irkmatch_id, rssi_ready);
		} else {
			err = 1;
		}
		if (err) {
			_radio.state = STATE_CLOSE;
			radio_disable();
		}
		break;

	case ROLE_OBS:
		if ((crc_ok) &&
		    (((_radio.observer.filter_policy & 0x01) == 0) ||
		     (devmatch_ok) || (irkmatch_ok))) {
			err = isr_rx_obs(irkmatch_id, rssi_ready);
		} else {
			err = 1;
		}
		if (err) {
			_radio.state = STATE_CLOSE;
			radio_disable();
			/* switch scanner state to idle */
			_radio.observer.scan_state = 0;
		}
		break;

	case ROLE_SLAVE:
	case ROLE_MASTER:
		isr_rx_conn(crc_ok, trx_done, rssi_ready);
		break;

	case ROLE_NONE:
	default:
		LL_ASSERT(0);
		break;
	}
}

static inline uint32_t isr_close_adv(void)
{
	uint32_t dont_close = 0;

	if ((_radio.state == STATE_CLOSE) &&
	    (_radio.advertiser.chl_map_current != 0)) {
		dont_close = 1;

		adv_setup();

		_radio.state = STATE_TX;

		radio_tx_enable();

		radio_tmr_end_capture();
	} else {
		struct pdu_adv *pdu_adv;

		radio_filter_disable();

		pdu_adv =
			(struct pdu_adv *)
			&_radio.advertiser.adv_data.data[_radio.advertiser.adv_data.first][0];
		if ((_radio.state == STATE_CLOSE) &&
		    (pdu_adv->type != PDU_ADV_TYPE_DIRECT_IND)) {
			uint32_t ticker_status;
			uint8_t random_delay;

			/** @todo use random 0-10 */
			random_delay = 10;

			ticker_status =
				ticker_update(RADIO_TICKER_INSTANCE_ID_RADIO,
					      RADIO_TICKER_USER_ID_WORKER,
					      RADIO_TICKER_ID_ADV,
					      TICKER_US_TO_TICKS(random_delay * 1000),
					      0, 0, 0, 0, 0,
					      ticker_success_assert,
					      (void *)__LINE__);
			LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
				  (ticker_status == TICKER_STATUS_BUSY));
		}
	}

	return dont_close;
}

static inline uint32_t isr_close_obs(void)
{
	uint32_t dont_close = 0;

	if (_radio.state == STATE_CLOSE) {
		dont_close = 1;

		radio_pkt_rx_set(_radio.packet_rx[_radio.packet_rx_last]->pdu_data);
		radio_switch_complete_and_tx();
		radio_rssi_measure();

		if (_radio.observer.filter_policy && _radio.nirk) {
			radio_ar_configure(_radio.nirk, _radio.irk);
		}

		_radio.state = STATE_RX;

		radio_rx_enable();

		radio_tmr_end_capture();
	} else {
		uint32_t ticker_status;

		radio_filter_disable();

		if (_radio.state == STATE_ABORT) {
			ticker_status =
				ticker_stop(
					    RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    RADIO_TICKER_ID_OBS_STOP,
					    0 /** @todo ticker_success_assert */,
					    0 /** @todo (void *) __LINE__ */);
			LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
				  (ticker_status == TICKER_STATUS_BUSY));
		}
	}

	return dont_close;
}

static inline void isr_close_conn(void)
{
	uint16_t ticks_drift_plus;
	uint16_t ticks_drift_minus;
	uint16_t latency_event;
	uint16_t elapsed_event;
	uint16_t lazy;
	uint8_t force;

	/* Local initiated terminate happened */
	if (_radio.conn_curr == 0) {
		return;
	}

	/* Remote Initiated terminate happened in this event for Slave */
	if ((_radio.role == ROLE_SLAVE) &&
	    (_radio.conn_curr->llcp_terminate.reason_peer != 0)) {
		terminate_ind_rx_enqueue(_radio.conn_curr,
					 _radio.conn_curr->llcp_terminate.reason_peer);

		connection_release(_radio.conn_curr);
		_radio.conn_curr = NULL;

		return;
	}

	ticks_drift_plus = 0;
	ticks_drift_minus = 0;
	latency_event = _radio.conn_curr->latency_event;
	elapsed_event = latency_event + 1;

	/* calculate drift if anchor point sync-ed */
	if ((_radio.packet_counter != 0) && ((!SILENT_CONNECTION) ||
					     (_radio.packet_counter != 0xFF))) {
		if (_radio.role == ROLE_SLAVE) {
			uint32_t start_to_address_actual_us;
			uint32_t start_to_address_expected_us;
			uint32_t window_widening_event_us;

			/* calculate the drift in ticks */
			start_to_address_actual_us = radio_tmr_aa_get();
			window_widening_event_us =
				_radio.conn_curr->role.slave.window_widening_event_us;
			start_to_address_expected_us =
				(RADIO_TICKER_JITTER_US << 1) +
				RADIO_PREAMBLE_TO_ADDRESS_US +
				window_widening_event_us;
			if (start_to_address_actual_us <=
			    start_to_address_expected_us) {
				ticks_drift_plus =
					TICKER_US_TO_TICKS(window_widening_event_us);
				ticks_drift_minus =
					TICKER_US_TO_TICKS((uint64_t)(start_to_address_expected_us -
								      start_to_address_actual_us));
			} else {
				ticks_drift_plus =
					TICKER_US_TO_TICKS(start_to_address_actual_us);
				ticks_drift_minus =
					TICKER_US_TO_TICKS((RADIO_TICKER_JITTER_US << 1) +
							   RADIO_PREAMBLE_TO_ADDRESS_US);
			}


			/* Reset window widening, as anchor point sync-ed */
			_radio.conn_curr->role.slave.window_widening_event_us = 0;
			_radio.conn_curr->role.slave.window_size_event_us = 0;

			/* apply latency if no more data */
			_radio.conn_curr->latency_event = _radio.conn_curr->latency;
			if (_radio.conn_curr->pkt_tx_head) {
				struct pdu_data *pdu_data_tx;

				pdu_data_tx = (struct pdu_data *)
					_radio.conn_curr->pkt_tx_head->pdu_data;
				if (pdu_data_tx->len ||
				    _radio.conn_curr->packet_tx_head_offset) {
					_radio.conn_curr->latency_event = 0;
				}
			}
		} else {
			/* Reset connection failed to establish procedure */
			_radio.conn_curr->role.master.connect_expire = 0;
		}

		/* Reset supervision counter */
		_radio.conn_curr->supervision_expire = 0;
	}

	/* Remote Initiated terminate happened in previous event for Master */
	else if ((_radio.role == ROLE_MASTER) &&
		 (_radio.conn_curr->llcp_terminate.reason_peer != 0)) {
		terminate_ind_rx_enqueue(_radio.conn_curr,
					 _radio.conn_curr->llcp_terminate.reason_peer);

		connection_release(_radio.conn_curr);
		_radio.conn_curr = NULL;

		return;
	}

	/* If master, check connection failed to establish */
	else if ((_radio.role == ROLE_MASTER) &&
		 (_radio.conn_curr->role.master.connect_expire != 0)) {
		if (_radio.conn_curr->role.master.connect_expire >
		    elapsed_event) {
			_radio.conn_curr->role.master.connect_expire -= elapsed_event;
		} else {
			terminate_ind_rx_enqueue(_radio.conn_curr, 0x3e);

			connection_release(_radio.conn_curr);
			_radio.conn_curr = NULL;

			return;
		}
	}

	/* if anchor point not sync-ed, start supervision timeout, and break
	 * latency if any.
	 */
	else {
		/* Start supervision timeout, if not started already */
		if (_radio.conn_curr->supervision_expire == 0) {
			_radio.conn_curr->supervision_expire =
				_radio.conn_curr->supervision_reload;
		}
	}

	/* check supervision timeout */
	force = 0;
	if (_radio.conn_curr->supervision_expire != 0) {
		if (_radio.conn_curr->supervision_expire > elapsed_event) {
			_radio.conn_curr->supervision_expire -= elapsed_event;

			/* break latency */
			_radio.conn_curr->latency_event = 0;

			/* Force both master and slave when close to
			 * supervision timeout.
			 */
			if (_radio.conn_curr->supervision_expire <= 6) {
				force = 1;
			}
			/* use randomness to force slave role when anchor
			 * points are being missed.
			 */
			else if (_radio.role == ROLE_SLAVE) {
				if (latency_event != 0) {
					force = 1;
				} else {
					force = _radio.conn_curr->role.slave.force & 0x01;

					/* rotate force bits */
					_radio.conn_curr->role.slave.force >>= 1;
					if (force) {
						_radio.conn_curr->role.slave.force |=
							((uint32_t)1 << 31);
					}
				}
			}
		} else {
			terminate_ind_rx_enqueue(_radio.conn_curr, 0x08);

			connection_release(_radio.conn_curr);
			_radio.conn_curr = NULL;

			return;
		}
	}

	/* check procedure timeout */
	if (_radio.conn_curr->procedure_expire != 0) {
		if (_radio.conn_curr->procedure_expire > elapsed_event) {
			_radio.conn_curr->procedure_expire -= elapsed_event;
		} else {
			terminate_ind_rx_enqueue(_radio.conn_curr, 0x22);

			connection_release(_radio.conn_curr);
			_radio.conn_curr = NULL;

			return;
		}
	}

	/* check apto */
	if (_radio.conn_curr->apto_expire != 0) {
		if (_radio.conn_curr->apto_expire > elapsed_event) {
			_radio.conn_curr->apto_expire -= elapsed_event;
		} else {
			struct radio_pdu_node_rx *radio_pdu_node_rx;

			_radio.conn_curr->apto_expire = 0;

			/* Prepare the rx packet structure */
			radio_pdu_node_rx = packet_rx_reserve_get(2);
			LL_ASSERT(radio_pdu_node_rx);

			radio_pdu_node_rx->hdr.handle = _radio.conn_curr->handle;
			radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_APTO;

			/* enqueue apto event into rx queue */
			packet_rx_enqueue();
		}
	}

	/* check appto */
	if (_radio.conn_curr->appto_expire != 0) {
		if (_radio.conn_curr->appto_expire > elapsed_event) {
			_radio.conn_curr->appto_expire -= elapsed_event;
		} else {
			_radio.conn_curr->appto_expire = 0;

			if ((_radio.conn_curr->procedure_expire == 0) &&
			    (_radio.conn_curr->llcp_req ==
			     _radio.conn_curr->llcp_ack)) {
				_radio.conn_curr->llcp_type = LLCP_PING;
				_radio.conn_curr->llcp_ack--;
			}
		}
	}

	/* generate RSSI event */
	if (_radio.conn_curr->rssi_sample_count == 0) {
		struct radio_pdu_node_rx *radio_pdu_node_rx;
		struct pdu_data *pdu_data_rx;

		radio_pdu_node_rx = packet_rx_reserve_get(2);
		if (radio_pdu_node_rx) {
			_radio.conn_curr->rssi_reported =
				_radio.conn_curr->rssi_latest;
			_radio.conn_curr->rssi_sample_count =
				RADIO_RSSI_SAMPLE_COUNT;

			/* Prepare the rx packet structure */
			radio_pdu_node_rx->hdr.handle =
				_radio.conn_curr->handle;
			radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_RSSI;

			/* prepare connection RSSI structure */
			pdu_data_rx = (struct pdu_data *)
				radio_pdu_node_rx->pdu_data;
			pdu_data_rx->payload.rssi =
				_radio.conn_curr->rssi_reported;

			/* enqueue connection RSSI structure into queue */
			packet_rx_enqueue();
		}
	}

	/* break latency based on ctrl procedure pending */
	if ((_radio.conn_curr->llcp_ack != _radio.conn_curr->llcp_req) &&
	    ((_radio.conn_curr->llcp_type == LLCP_CONNECTION_UPDATE) ||
	     (_radio.conn_curr->llcp_type == LLCP_CHANNEL_MAP))) {
		_radio.conn_curr->latency_event = 0;
	}

	/* check if latency needs update */
	lazy = 0;
	if ((force) || (latency_event != _radio.conn_curr->latency_event)) {
		lazy = _radio.conn_curr->latency_event + 1;
	}

	if ((ticks_drift_plus != 0) || (ticks_drift_minus != 0) ||
	    (lazy != 0) || (force != 0)) {
		uint32_t ticker_status;

		ticker_status =
			ticker_update(RADIO_TICKER_INSTANCE_ID_RADIO,
				      RADIO_TICKER_USER_ID_WORKER,
				      RADIO_TICKER_ID_FIRST_CONNECTION +
				      _radio.conn_curr->handle,
				      ticks_drift_plus, ticks_drift_minus, 0, 0,
				      lazy, force,
				      0 /** @todo ticker_success_assert */,
				      0 /** @todo (void *) __LINE__ */);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
}

static inline void isr_radio_state_close(void)
{
	uint32_t dont_close = 0;

	switch (_radio.role) {
	case ROLE_ADV:
		dont_close = isr_close_adv();
		break;

	case ROLE_OBS:
		dont_close = isr_close_obs();
		break;

	case ROLE_SLAVE:
	case ROLE_MASTER:
		isr_close_conn();
		break;

	case ROLE_NONE:
	default:
		LL_ASSERT(0);
		break;
	}

	if (dont_close) {
		return;
	}

	_radio.role = ROLE_NONE;
	_radio.state = STATE_NONE;
	_radio.ticker_id_event = 0;

	radio_tmr_stop();

	event_inactive(0, 0, 0, 0);

	clock_control_off(_radio.hf_clock, NULL);

	work_enable(WORK_TICKER_JOB0_IRQ);

	DEBUG_RADIO_CLOSE(0);
}

static void isr(void)
{
	uint8_t trx_done;
	uint8_t crc_ok;
	uint8_t devmatch_ok;
	uint8_t irkmatch_ok;
	uint8_t irkmatch_id;
	uint8_t rssi_ready;

	DEBUG_RADIO_ISR(1);

	/* Read radio status and events */
	trx_done = radio_is_done();
	if (trx_done) {
		crc_ok = radio_crc_is_valid();
		devmatch_ok = radio_filter_has_match();
		irkmatch_ok = radio_ar_has_match();
		irkmatch_id = radio_ar_match_get();
		rssi_ready = radio_rssi_is_ready();
	} else {
		crc_ok = devmatch_ok = irkmatch_ok = rssi_ready = 0;
		irkmatch_id = 0xFF;
	}

	/* Clear radio status and events */
	radio_status_reset();
	radio_tmr_status_reset();
	radio_filter_status_reset();
	radio_ar_status_reset();
	radio_rssi_status_reset();

	switch (_radio.state) {
	case STATE_TX:
		isr_radio_state_tx();
		break;

	case STATE_RX:
		isr_radio_state_rx(trx_done, crc_ok, devmatch_ok, irkmatch_ok,
				   irkmatch_id, rssi_ready);
		break;

	case STATE_ABORT:
	case STATE_STOP:
	case STATE_CLOSE:
		isr_radio_state_close();
		break;

	case STATE_NONE:
		/* Ignore Duplicate Radio Disabled IRQ due to forced stop
		 * using Radio Disable task.
		 */
		break;

	default:
		LL_ASSERT(0);
		break;
	}

	LL_ASSERT(((_radio.state != STATE_RX) && (_radio.state != STATE_TX)) ||
		  (!radio_is_ready()));

	DEBUG_RADIO_ISR(0);
}

#if (WORK_TICKER_WORKER0_IRQ_PRIORITY == WORK_TICKER_JOB0_IRQ_PRIORITY)
static void ticker_job_disable(uint32_t status, void *op_context)
{
	ARG_UNUSED(status);
	ARG_UNUSED(op_context);

	if (_radio.state != STATE_NONE) {
		work_disable(WORK_TICKER_JOB0_IRQ);
	}
}
#endif

static void ticker_if_done(uint32_t status, void *ops_context)
{
	*((uint32_t volatile *)ops_context) = status;
}

static void ticker_success_assert(uint32_t status, void *params)
{
	ARG_UNUSED(params);

	LL_ASSERT(status == TICKER_STATUS_SUCCESS);
}

static void work_radio_active(void *params)
{
	static uint8_t s_active;

	if ((uint32_t)params) {
		if (s_active++) {
			return;
		}

		DEBUG_RADIO_ACTIVE(1);

		radio_active_callback(1);
	} else {
		LL_ASSERT(s_active);

		if (--s_active) {
			return;
		}

		DEBUG_RADIO_ACTIVE(0);

		radio_active_callback(0);
	}
}

static void event_active(uint32_t ticks_at_expire, uint32_t remainder,
			 uint16_t lazy, void *context)
{
	static struct work s_work_radio_active = { 0, 0, 0,
		WORK_TICKER_WORKER0_IRQ, (work_fp) work_radio_active,
		(void *)1};
	uint32_t retval;

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	retval = work_schedule(&s_work_radio_active, 0);
	LL_ASSERT(!retval);
}

static void work_radio_inactive(void *params)
{
	ARG_UNUSED(params);

	work_radio_active(0);

	DEBUG_RADIO_CLOSE(0);
}

static void event_inactive(uint32_t ticks_at_expire, uint32_t remainder,
			   uint16_t lazy, void *context)
{
	static struct work s_work_radio_inactive = { 0, 0, 0,
		WORK_TICKER_WORKER0_IRQ, (work_fp) work_radio_inactive, 0};
	uint32_t retval;

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	retval = work_schedule(&s_work_radio_inactive, 0);
	LL_ASSERT(!retval);
}

static void work_xtal_start(void *params)
{
	ARG_UNUSED(params);

	/* turn on 16MHz clock, non-blocking mode. */
	clock_control_on(_radio.hf_clock, NULL);
}

static void event_xtal(uint32_t ticks_at_expire, uint32_t remainder,
				 uint16_t lazy, void *context)
{
	static struct work s_work_xtal_start = { 0, 0, 0,
		WORK_TICKER_WORKER0_IRQ, (work_fp) work_xtal_start, 0 };
	uint32_t retval;

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	retval = work_schedule(&s_work_xtal_start, 0);
	LL_ASSERT(!retval);
}

static void work_xtal_stop(void *params)
{
	ARG_UNUSED(params);

	clock_control_off(_radio.hf_clock, NULL);

	DEBUG_RADIO_CLOSE(0);
}

#if XTAL_ADVANCED
static void work_xtal_retain(uint8_t retain)
{
	static uint8_t s_xtal_retained;

	if (retain) {
		if (!s_xtal_retained) {
			static struct work s_work_xtal_start = { 0, 0, 0,
				WORK_TICKER_WORKER0_IRQ,
				(work_fp) work_xtal_start, 0 };
			uint32_t retval;

			s_xtal_retained = 1;

			retval = work_schedule(&s_work_xtal_start, 0);
			LL_ASSERT(!retval);
		}
	} else {
		if (s_xtal_retained) {
			static struct work s_work_xtal_stop = { 0, 0, 0,
				WORK_TICKER_WORKER0_IRQ,
				(work_fp) work_xtal_stop, 0 };
			uint32_t retval;

			s_xtal_retained = 0;

			retval = work_schedule(&s_work_xtal_stop, 0);
			LL_ASSERT(!retval);
		}
	}
}

static void prepare_reduced(uint32_t status, void *op_context)
{
	/* It is acceptable that ticker_update will fail, if ticker is stopped;
	 * for example, obs ticker is stopped on connection estblishment but
	 * is also preempted.
	 */
	if (status == 0) {
		struct shdr *hdr = (struct shdr *)op_context;

		hdr->ticks_xtal_to_start |= ((uint32_t)1 << 31);
	}
}

static void prepare_normal(uint32_t status, void *op_context)
{
	/* It is acceptable that ticker_update will fail, if ticker is stopped;
	 * for example, obs ticker is stopped on connection estblishment but
	 * is also preempted.
	 */
	if (status == 0) {
		struct shdr *hdr = (struct shdr *)op_context;

		hdr->ticks_xtal_to_start &= ~((uint32_t)1 << 31);
	}
}

static void prepare_normal_set(struct shdr *hdr,
					 uint8_t ticker_user_id,
					 uint8_t ticker_id)
{
	if (hdr->ticks_xtal_to_start & ((uint32_t)1 << 31)) {
		uint32_t ticker_status;
		uint32_t ticks_prepare_to_start =
			(hdr->ticks_active_to_start >
			 hdr->ticks_preempt_to_start) ? hdr->
			ticks_active_to_start : hdr->ticks_preempt_to_start;
		uint32_t ticks_drift_minus =
			(hdr->ticks_xtal_to_start & (~((uint32_t)1 << 31))) -
			ticks_prepare_to_start;

		ticker_status =
			ticker_update(RADIO_TICKER_INSTANCE_ID_RADIO,
				      ticker_user_id,
				      ticker_id, 0, ticks_drift_minus,
				      ticks_drift_minus, 0, 0, 0,
				      prepare_normal, hdr);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
}

#if (RADIO_TICKER_PREEMPT_PART_US <= RADIO_TICKER_PREEMPT_PART_MIN_US)
static uint32_t preempt_calc(struct shdr *hdr, uint8_t ticker_id,
			     uint32_t ticks_at_expire)
{
	uint32_t diff =
		ticker_ticks_diff_get(ticker_ticks_now_get(), ticks_at_expire);

	diff += 3;
	if (diff > TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US)) {
		work_xtal_retain(0);

		prepare_normal_set(hdr, RADIO_TICKER_USER_ID_WORKER, ticker_id);

		diff += hdr->ticks_preempt_to_start;
		if (diff <
			TICKER_US_TO_TICKS(RADIO_TICKER_PREEMPT_PART_MAX_US)) {
			hdr->ticks_preempt_to_start = diff;
		}

		return 1;
	}

	return 0;
}
#endif

/** @brief This function decides to start (additional call) xtal ahead of next
 * ticker, if next ticker is close to current ticker expire.
 *
 * @note This function also detects if two tickers of same interval are drifting
 * close and issues a conn param req or does a conn update.
 *
 * @todo Detect drift for overlapping tickers.
 */
static void work_xtal_stop_calc(void *params)
{
	uint32_t volatile ticker_status;
	uint8_t ticker_id;
	uint32_t ticks_current;
	uint32_t ticks_to_expire;

	ticker_id = 0xff;
	ticks_to_expire = 0;
	ticker_status =
		ticker_next_slot_get(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_JOB, &ticker_id,
				     &ticks_current, &ticks_to_expire,
				     ticker_if_done, (void *)&ticker_status);

	while (ticker_status == TICKER_STATUS_BUSY) {
		ticker_job_sched(RADIO_TICKER_INSTANCE_ID_RADIO);
	}

	LL_ASSERT(ticker_status == TICKER_STATUS_SUCCESS);

	if ((ticker_id != 0xff) &&
	    (ticks_to_expire < TICKER_US_TO_TICKS(10000))) {
		work_xtal_retain(1);

		if (ticker_id >= RADIO_TICKER_ID_ADV) {
#if SCHED_ADVANCED
			uint8_t ticker_id_current = ((uint32_t)params & 0xff);
			struct connection *conn_curr = NULL;
#endif
			uint32_t ticks_prepare_to_start;
			struct connection *conn = NULL;
			struct shdr *hdr = NULL;

			/* Select the role's scheduling header */
			if (ticker_id >= RADIO_TICKER_ID_FIRST_CONNECTION) {
				conn = mem_get(_radio.conn_pool,
					       CONNECTION_T_SIZE,
					       (ticker_id -
						RADIO_TICKER_ID_FIRST_CONNECTION));
				hdr = &conn->hdr;
			} else if (ticker_id == RADIO_TICKER_ID_ADV) {
				hdr = &_radio.advertiser.hdr;
			} else if (ticker_id == RADIO_TICKER_ID_OBS) {
				hdr = &_radio.observer.hdr;
			} else {
				LL_ASSERT(0);
			}

			/* compensate for reduced next ticker's prepare or
			 * reduce next ticker's prepare.
			 */
			ticks_prepare_to_start =
				(hdr->ticks_active_to_start >
				 hdr->ticks_preempt_to_start) ?
				hdr->ticks_active_to_start :
				hdr->ticks_preempt_to_start;
			if ((hdr->ticks_xtal_to_start & ((uint32_t)1 << 31)) != 0) {
				ticks_to_expire -= ((hdr->ticks_xtal_to_start &
						     (~((uint32_t)1 << 31))) -
						    ticks_prepare_to_start);
			} else {
				/* Postpone the primary because we dont have
				 * to start xtal.
				 */
				if (hdr->ticks_xtal_to_start >
				    ticks_prepare_to_start) {
					uint32_t ticks_drift_plus =
						hdr->ticks_xtal_to_start -
						ticks_prepare_to_start;

					ticker_status =
						ticker_update(
							      RADIO_TICKER_INSTANCE_ID_RADIO,
							      RADIO_TICKER_USER_ID_JOB,
							      ticker_id,
							      ticks_drift_plus, 0,
							      0, ticks_drift_plus,
							      0, 0,
							      prepare_reduced,
							      hdr);
					LL_ASSERT((TICKER_STATUS_SUCCESS ==
						   ticker_status) ||
						  (TICKER_STATUS_BUSY ==
						   ticker_status));
				}
			}

#if SCHED_ADVANCED
			if (ticker_id_current >= RADIO_TICKER_ID_FIRST_CONNECTION) {
				/* compensate the current ticker for reduced
				 * prepare.
				 */
				conn_curr =
					mem_get(_radio.conn_pool,
						CONNECTION_T_SIZE,
						(ticker_id_current -
						 RADIO_TICKER_ID_FIRST_CONNECTION));
				ticks_prepare_to_start =
					(conn_curr->hdr.ticks_active_to_start >
					conn_curr->hdr.ticks_preempt_to_start) ?
					conn_curr->hdr.ticks_active_to_start :
					conn_curr->hdr.ticks_preempt_to_start;
				if ((conn_curr->hdr.ticks_xtal_to_start &
						((uint32_t)1 << 31)) != 0) {
					ticks_to_expire +=
						((conn_curr->hdr.ticks_xtal_to_start &
						  (~((uint32_t)1 << 31))) -
						 ticks_prepare_to_start);
				}
			}

			/* auto conn param req or conn update procedure to
			 * avoid connection collisions.
			 */
			if ((conn) && (conn_curr) &&
			    (conn_curr->conn_interval == conn->conn_interval)) {
				uint32_t ticks_conn_interval =
					TICKER_US_TO_TICKS(conn->conn_interval * 1250);

				/* remove laziness, if any, from
				 * ticks_to_expire.
				 */
				while (ticks_to_expire > ticks_conn_interval) {
					ticks_to_expire -= ticks_conn_interval;
				}

				/* if next ticker close to this ticker, send
				 * conn param req.
				 */
				if ((conn_curr->role.slave.role != 0) &&
					(conn->role.master.role == 0) &&
					(ticks_to_expire <
					 (TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US +
							     625)
					  + conn_curr->hdr.ticks_slot))) {
					uint32_t status;

					status = conn_update_req(conn_curr);
					if ((status == 2) &&
					    (conn->llcp_version.rx)) {
						conn_update_req(conn);
					}
				} else if ((conn_curr->role.master.role == 0) &&
						(conn->role.slave.role != 0) &&
						(ticks_to_expire <
						 (TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US +
								     625) +
						  conn_curr->hdr.ticks_slot))) {
					uint32_t status;

					status = conn_update_req(conn);
					if ((status == 2) &&
					    (conn_curr->llcp_version.rx)) {
						conn_update_req(conn_curr);
					}
				}
			}
#endif /* SCHED_ADVANCED */
		}
	} else {
		work_xtal_retain(0);

		if ((ticker_id != 0xff) && (ticker_id >= RADIO_TICKER_ID_ADV)) {
			struct shdr *hdr = NULL;

			/* Select the role's scheduling header */
			if (ticker_id >= RADIO_TICKER_ID_FIRST_CONNECTION) {
				struct connection *conn;

				conn = mem_get(_radio.conn_pool,
					       CONNECTION_T_SIZE,
					       (ticker_id -
						RADIO_TICKER_ID_FIRST_CONNECTION));
				hdr = &conn->hdr;
			} else if (ticker_id == RADIO_TICKER_ID_ADV) {
				hdr = &_radio.advertiser.hdr;
			} else if (ticker_id == RADIO_TICKER_ID_OBS) {
				hdr = &_radio.observer.hdr;
			} else {
				LL_ASSERT(0);
			}

			/* Use normal prepare */
			prepare_normal_set(hdr, RADIO_TICKER_USER_ID_JOB,
					   ticker_id);
		}
	}
}
#endif /* XTAL_ADVANCED */

#if SCHED_ADVANCED
static void sched_after_master_free_slot_get(uint8_t user_id,
					     uint32_t ticks_slot_abs,
					     uint32_t *ticks_anchor,
					     uint32_t *us_offset)
{
	uint8_t ticker_id;
	uint8_t ticker_id_prev;
	uint32_t ticks_to_expire;
	uint32_t ticks_to_expire_prev;
	uint32_t ticks_slot_prev_abs;

	ticker_id = ticker_id_prev = 0xff;
	ticks_to_expire = ticks_to_expire_prev = *us_offset = 0;
	ticks_slot_prev_abs = 0;
	while (1) {
		uint32_t volatile ticker_status;
		struct connection *conn;

		ticker_status =
			ticker_next_slot_get(RADIO_TICKER_INSTANCE_ID_RADIO,
					     user_id, &ticker_id, ticks_anchor,
					     &ticks_to_expire, ticker_if_done,
					     (void *)&ticker_status);

		while (ticker_status == TICKER_STATUS_BUSY) {
			ticker_job_sched(RADIO_TICKER_INSTANCE_ID_RADIO);
		}

		LL_ASSERT(ticker_status == TICKER_STATUS_SUCCESS);

		if (ticker_id == 0xff) {
			break;
		}

		if (ticker_id < RADIO_TICKER_ID_FIRST_CONNECTION) {
			continue;
		}

		conn = mem_get(_radio.conn_pool, CONNECTION_T_SIZE,
			       (ticker_id - RADIO_TICKER_ID_FIRST_CONNECTION));
		if ((conn) && (conn->role.master.role == 0)) {
			uint32_t ticks_to_expire_normal = ticks_to_expire;

			if (conn->hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
				uint32_t ticks_prepare_to_start =
					(conn->hdr.ticks_active_to_start >
					 conn->hdr.ticks_preempt_to_start) ?
					conn->hdr.ticks_active_to_start :
					conn->hdr.ticks_preempt_to_start;

				ticks_to_expire_normal -=
					((conn->hdr.ticks_xtal_to_start &
					  (~((uint32_t)1 << 31))) -
					 ticks_prepare_to_start);
			}

			if ((ticker_id_prev != 0xFF) &&
			    (ticker_ticks_diff_get(ticks_to_expire_normal,
						   ticks_to_expire_prev) >
			     (ticks_slot_prev_abs + ticks_slot_abs +
			      TICKER_US_TO_TICKS(RADIO_TICKER_JITTER_US << 2)))) {
				break;
			}

			ticker_id_prev = ticker_id;
			ticks_to_expire_prev = ticks_to_expire_normal;
			ticks_slot_prev_abs =
				TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US) +
				conn->hdr.ticks_slot;
		}
	}

	if (ticker_id_prev != 0xff) {
		*us_offset = TICKER_TICKS_TO_US(ticks_to_expire_prev +
						ticks_slot_prev_abs) +
			(RADIO_TICKER_JITTER_US << 1);
	}
}

static void sched_after_master_free_offset_get(uint16_t conn_interval,
					       uint32_t ticks_slot,
					       uint32_t ticks_anchor,
					       uint32_t *win_offset_us)
{
	uint32_t ticks_anchor_offset = ticks_anchor;

	sched_after_master_free_slot_get(RADIO_TICKER_USER_ID_JOB,
					 (TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US) +
					  ticks_slot), &ticks_anchor_offset,
					 win_offset_us);

	if (ticks_anchor_offset != ticks_anchor) {
		*win_offset_us +=
			TICKER_TICKS_TO_US(ticker_ticks_diff_get(ticks_anchor_offset,
								 ticks_anchor));
	}

	if ((*win_offset_us & ((uint32_t)1 << 31)) == 0) {
		uint32_t conn_interval_us = conn_interval * 1250;

		while (*win_offset_us > conn_interval_us) {
			*win_offset_us -= conn_interval_us;
		}
	}
}

static void work_sched_after_master_free_offset_get(void *params)
{
	sched_after_master_free_offset_get(_radio.observer.conn_interval,
					   _radio.observer.ticks_conn_slot,
					   (uint32_t)params,
					   &_radio.observer.win_offset_us);
}

static void work_sched_win_offset_use(void *params)
{
	struct connection *conn = (struct connection *)params;
	uint16_t win_offset;

	sched_after_master_free_offset_get(conn->conn_interval,
					   conn->hdr.ticks_slot,
					   conn->llcp.connection_update.ticks_ref,
					   &conn->llcp.connection_update.win_offset_us);

	win_offset = conn->llcp.connection_update.win_offset_us / 1250;
	memcpy(conn->llcp.connection_update.pdu_win_offset, &win_offset,
	       sizeof(uint16_t));
}

static void sched_free_win_offset_calc(struct connection *conn_curr,
					   uint8_t is_select,
					   uint32_t *ticks_to_offset_next,
					   uint16_t conn_interval,
					   uint8_t *offset_max,
					   uint8_t *win_offset)
{
	uint32_t ticks_prepare_reduced = 0;
	uint32_t ticks_anchor;
	uint32_t ticks_anchor_prev;
	uint32_t ticks_to_expire_prev;
	uint32_t ticks_to_expire;
	uint32_t ticks_slot_prev_abs;
	uint8_t ticker_id;
	uint8_t ticker_id_prev;
	uint8_t ticker_id_other;
	uint8_t offset_index;
	uint16_t _win_offset;

	if (conn_curr->hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
		uint32_t ticks_prepare_to_start =
				(conn_curr->hdr.ticks_active_to_start >
				conn_curr->hdr.ticks_preempt_to_start) ?
				conn_curr->hdr.ticks_active_to_start :
				conn_curr->hdr.ticks_preempt_to_start;

		ticks_prepare_reduced = ((conn_curr->hdr.ticks_xtal_to_start &
					  (~((uint32_t)1 << 31))) -
					 ticks_prepare_to_start);
	}

	ticker_id = ticker_id_prev = ticker_id_other = 0xFF;
	ticks_to_expire = ticks_to_expire_prev = ticks_anchor =
		ticks_anchor_prev = offset_index = _win_offset = 0;
	ticks_slot_prev_abs = 0;
	do {
		uint32_t volatile ticker_status;
		struct connection *conn;

		ticker_status =
			ticker_next_slot_get(
					     RADIO_TICKER_INSTANCE_ID_RADIO,
					     RADIO_TICKER_USER_ID_JOB,
					     &ticker_id, &ticks_anchor,
					     &ticks_to_expire, ticker_if_done,
					     (void *)&ticker_status);

		while (ticker_status == TICKER_STATUS_BUSY) {
			ticker_job_sched(RADIO_TICKER_INSTANCE_ID_RADIO);
		}

		LL_ASSERT(ticker_status == TICKER_STATUS_SUCCESS);

		if (ticker_id == 0xff) {
			break;
		}

		if ((ticker_id_prev != 0xff) &&
		    (ticks_anchor != ticks_anchor_prev)) {
			LL_ASSERT(0);
		}

		if (ticker_id < RADIO_TICKER_ID_ADV) {
			continue;
		}

		if (ticker_id < RADIO_TICKER_ID_FIRST_CONNECTION) {
			/* non conn role found which could have preempted a
			 * conn role, hence do not consider this free space
			 * and any further as free slot for offset,
			 */
			ticker_id_other = ticker_id;
			continue;
		}

		if (ticker_id_other != 0xFF) {
			break;
		}

		conn = mem_get(_radio.conn_pool, CONNECTION_T_SIZE,
			       (ticker_id - RADIO_TICKER_ID_FIRST_CONNECTION));

		if ((conn != conn_curr) && ((is_select) ||
					    (conn->role.master.role == 0))) {
			uint32_t ticks_to_expire_normal =
				ticks_to_expire + ticks_prepare_reduced;

			if (conn->hdr.ticks_xtal_to_start &
			    ((uint32_t)1 << 31)) {
				uint32_t ticks_prepare_to_start =
					(conn->hdr.ticks_active_to_start >
					 conn->hdr.ticks_preempt_to_start) ?
					conn->hdr.ticks_active_to_start :
					conn->hdr.ticks_preempt_to_start;

				ticks_to_expire_normal -=
					((conn->hdr.ticks_xtal_to_start &
					  (~((uint32_t)1 << 31))) -
					 ticks_prepare_to_start);
			}

			if (*ticks_to_offset_next < ticks_to_expire_normal) {
				if (ticks_to_expire_prev < *ticks_to_offset_next) {
					ticks_to_expire_prev =
						*ticks_to_offset_next;
				}

				while ((offset_index < *offset_max) &&
				       (ticker_ticks_diff_get(ticks_to_expire_normal,
							      ticks_to_expire_prev) >=
					(ticks_slot_prev_abs +
					 TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US +
							    625 + 1250) +
					 conn->hdr.ticks_slot))) {
					_win_offset =
						TICKER_TICKS_TO_US(ticks_to_expire_prev +
								   ticks_slot_prev_abs) / 1250;
					if (_win_offset >= conn_interval) {
						ticks_to_expire_prev = 0;

						break;
					}

					memcpy(win_offset +
					       (sizeof(uint16_t) * offset_index),
					       &_win_offset, sizeof(uint16_t));
					offset_index++;

					ticks_to_expire_prev +=
						TICKER_US_TO_TICKS(1250);
				}

				*ticks_to_offset_next = ticks_to_expire_prev;

				if (_win_offset >= conn_interval) {
					break;
				}
			}

			ticks_anchor_prev = ticks_anchor;
			ticker_id_prev = ticker_id;
			ticks_to_expire_prev = ticks_to_expire_normal;
			ticks_slot_prev_abs =
				TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US +
						   625 + 1250) +
				conn->hdr.ticks_slot;
		}
	} while (offset_index < *offset_max);

	if (ticker_id == 0xFF) {
		if (ticks_to_expire_prev < *ticks_to_offset_next) {
			ticks_to_expire_prev = *ticks_to_offset_next;
		}

		while (offset_index < *offset_max) {
			_win_offset =
				TICKER_TICKS_TO_US(ticks_to_expire_prev +
						   ticks_slot_prev_abs) / 1250;
			if (_win_offset >= conn_interval) {
				ticks_to_expire_prev = 0;

				break;
			}

			memcpy(win_offset + (sizeof(uint16_t) * offset_index),
			       &_win_offset, sizeof(uint16_t));
			offset_index++;

			ticks_to_expire_prev += TICKER_US_TO_TICKS(1250);
		}

		*ticks_to_offset_next = ticks_to_expire_prev;
	}

	*offset_max = offset_index;
}

static void work_sched_free_win_offset_calc(void *params)
{
	struct connection *conn = (struct connection *)params;
	uint32_t ticks_to_offset_default = 0;
	uint32_t *ticks_to_offset_next;
	uint8_t offset_max = 6;

	ticks_to_offset_next = &ticks_to_offset_default;

	if (conn->role.slave.role != 0) {
		conn->llcp.connection_update.ticks_to_offset_next =
			conn->role.slave.ticks_to_offset;

		ticks_to_offset_next =
			&conn->llcp.connection_update.ticks_to_offset_next;
	}

	sched_free_win_offset_calc(conn, 0, ticks_to_offset_next,
				   conn->llcp.connection_update.interval,
				   &offset_max,
				   (uint8_t *)conn->llcp.connection_update.pdu_win_offset);
}

static void work_sched_win_offset_select(void *params)
{
#define OFFSET_S_MAX 6
#define OFFSET_M_MAX 6
	struct connection *conn = (struct connection *)params;
	uint32_t ticks_to_offset;
	uint16_t win_offset_m[OFFSET_M_MAX];
	uint8_t offset_m_max = OFFSET_M_MAX;
	uint16_t win_offset_s;
	uint8_t offset_index_s = 0;

	ticks_to_offset =
		TICKER_US_TO_TICKS(conn->llcp.connection_update.offset0 * 1250);

	sched_free_win_offset_calc(conn, 1, &ticks_to_offset,
				   conn->llcp.connection_update.interval,
				   &offset_m_max, (uint8_t *)&win_offset_m[0]);

	while (offset_index_s < OFFSET_S_MAX) {
		uint8_t offset_index_m = 0;

		memcpy((uint8_t *)&win_offset_s,
		       ((uint8_t *)&conn->llcp.connection_update.offset0 +
			(sizeof(uint16_t) * offset_index_s)), sizeof(uint16_t));

		while (offset_index_m < offset_m_max) {
			if ((win_offset_s != 0xffff) &&
			    (win_offset_s == win_offset_m[offset_index_m])) {
				break;
			}

			offset_index_m++;
		}

		if (offset_index_m < offset_m_max) {
			break;
		}

		offset_index_s++;
	}

	if (offset_index_s < OFFSET_S_MAX) {
		conn->llcp.connection_update.win_offset_us =
			win_offset_s * 1250;
		memcpy(conn->llcp.connection_update.pdu_win_offset,
		       &win_offset_s, sizeof(uint16_t));
	} else {
		struct pdu_data *pdu_ctrl_tx;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* reset mutex */
		_radio.conn_upd = NULL;

		/* send reject_ind_ext */
		pdu_ctrl_tx = (struct pdu_data *)
			((uint8_t *)conn->llcp.connection_update.pdu_win_offset -
			 offsetof(struct pdu_data,
				  payload.llctrl.ctrldata.conn_update_req.win_offset));
		pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_tx->len =
			offsetof(struct pdu_data_llctrl, ctrldata) +
			sizeof(struct pdu_data_llctrl_reject_ind_ext);
		pdu_ctrl_tx->payload.llctrl.opcode =
			PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT;
		pdu_ctrl_tx->payload.llctrl.ctrldata.reject_ind_ext.
			reject_opcode = PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ;
		pdu_ctrl_tx->payload.llctrl.ctrldata.reject_ind_ext.
			error_code = 0x20; /* Unsupported parameter value */
	}
}
#endif /* SCHED_ADVANCED */

static void work_radio_stop(void *params)
{
	enum state state = (enum state)((uint32_t)params & 0xff);
	uint32_t radio_used;

	LL_ASSERT((state == STATE_STOP) || (state == STATE_ABORT));

	radio_used = ((_radio.state != STATE_NONE) &&
		      (_radio.state != STATE_STOP) &&
		      (_radio.state != STATE_ABORT));
	if (radio_used || !radio_is_idle()) {
		if (radio_used) {
			_radio.state = state;
		}

		/** @todo try designing so as to not to abort tx packet */
		radio_disable();
	}
}

static void event_stop(uint32_t ticks_at_expire, uint32_t remainder,
		       uint16_t lazy, void *context)
{
	static struct work s_work_radio_stop = { 0, 0, 0,
			WORK_TICKER_WORKER0_IRQ, (work_fp) work_radio_stop, 0 };
	uint32_t retval;

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);

	/* Radio state requested (stop or abort) stored in context is supplied
	 * in params.
	 */
	s_work_radio_stop.params = context;

	/* Stop Radio Tx/Rx */
	retval = work_schedule(&s_work_radio_stop, 0);
	LL_ASSERT(!retval);
}

static void event_common_prepare(uint32_t ticks_at_expire,
				 uint32_t remainder,
				 uint32_t *ticks_xtal_to_start,
				 uint32_t *ticks_active_to_start,
				 uint32_t ticks_preempt_to_start,
				 uint8_t ticker_id,
				 ticker_timeout_func ticker_timeout_fp,
				 void *context)
{
	uint32_t ticker_status;
	uint32_t _ticks_xtal_to_start = *ticks_xtal_to_start;
	uint32_t _ticks_active_to_start = *ticks_active_to_start;
	uint32_t ticks_to_start;

	/* in case this event is short prepare, xtal to start duration will be
	 * active to start duration.
	 */
	if (_ticks_xtal_to_start & ((uint32_t)1 << 31)) {
		_ticks_xtal_to_start =
			(_ticks_active_to_start > ticks_preempt_to_start) ?
			_ticks_active_to_start :
			ticks_preempt_to_start;
	}

	/* decide whether its XTAL start or active event that is the current
	 * execution context and accordingly setup the ticker for the other
	 * event (XTAL or active event). These are oneshot ticker.
	 */
	if (_ticks_active_to_start < _ticks_xtal_to_start) {
		uint32_t ticks_to_active;

		/* XTAL is before Active */
		ticks_to_active = _ticks_xtal_to_start - _ticks_active_to_start;
		ticks_to_start = _ticks_xtal_to_start;

		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_MARKER_0, ticks_at_expire,
				     ticks_to_active, TICKER_NULL_PERIOD,
				     TICKER_NULL_REMAINDER, TICKER_NULL_LAZY,
				     TICKER_NULL_SLOT, event_active, 0,
				     ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		event_xtal(0, 0, 0, 0);
	} else if (_ticks_active_to_start > _ticks_xtal_to_start) {
		uint32_t ticks_to_xtal;

		/* Active is before XTAL */
		ticks_to_xtal = _ticks_active_to_start - _ticks_xtal_to_start;
		ticks_to_start = _ticks_active_to_start;

		event_active(0, 0, 0, 0);

		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_MARKER_0, ticks_at_expire,
				     ticks_to_xtal, TICKER_NULL_PERIOD,
				     TICKER_NULL_REMAINDER, TICKER_NULL_LAZY,
				     TICKER_NULL_SLOT, event_xtal, 0,
				     ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	} else {
		/* Active and XTAL are at the same time,
		 * no ticker required to be setup.
		 */
		ticks_to_start = _ticks_xtal_to_start;

		event_active(0, 0, 0, 0);
		event_xtal(0, 0, 0, 0);
	}

	/* remember the remainder to be used in pkticker */
	_radio.remainder_anchor = remainder;

	/* setup the start ticker */
	ticker_status =
		ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
			     RADIO_TICKER_USER_ID_WORKER, RADIO_TICKER_ID_EVENT,
			     ticks_at_expire, ticks_to_start,
			     TICKER_NULL_PERIOD, TICKER_NULL_REMAINDER,
			     TICKER_NULL_LAZY, TICKER_NULL_SLOT,
			     ticker_timeout_fp, context, ticker_success_assert,
			     (void *)__LINE__);
	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));

#define RADIO_DEFERRED_PREEMPT 0
#if RADIO_DEFERRED_PREEMPT
	/* setup pre-empt ticker if any running state present */
	if (_radio.state != STATE_NONE) {
		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_PRE_EMPT, ticks_at_expire,
				     (ticks_to_start - conn->hdr.ticks_preempt_to_start),
				     TICKER_NULL_PERIOD, TICKER_NULL_REMAINDER,
				     TICKER_NULL_LAZY, TICKER_NULL_SLOT,
				     event_stop, (void *)STATE_ABORT,
				     ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
#else
	event_stop(0, 0, 0, (void *)STATE_ABORT);
#endif
#undef RADIO_DEFERRED_PREEMPT

  /** Handle change in _ticks_active_to_start */
	if (_radio.ticks_active_to_start != _ticks_active_to_start) {
		uint32_t ticks_to_start_new =
			((_radio.ticks_active_to_start <
			  (*ticks_xtal_to_start & ~(((uint32_t)1 << 31)))) ?
			 (*ticks_xtal_to_start & ~(((uint32_t)1 << 31))) :
			 _radio.ticks_active_to_start);

		*ticks_active_to_start = _radio.ticks_active_to_start;

		if ((*ticks_xtal_to_start) & ((uint32_t)1 << 31)) {
			*ticks_xtal_to_start &= ~(((uint32_t)1 << 31));
		}

		/* drift the primary as required due to active line change */
		ticker_status =
			ticker_update(RADIO_TICKER_INSTANCE_ID_RADIO,
				      RADIO_TICKER_USER_ID_WORKER, ticker_id,
				      ticks_to_start, ticks_to_start_new,
				      ticks_to_start_new, ticks_to_start, 0, 0,
				      ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}

	/* route all packets queued for connections */
	packet_tx_enqueue(0xFF);

	/* calc whether xtal needs to be retained after this event */
#if XTAL_ADVANCED
	{
		static struct work s_work_xtal_stop_calc = {
			0, 0, 0, WORK_TICKER_JOB0_IRQ,
			(work_fp) work_xtal_stop_calc, 0 };
		uint32_t retval;

		s_work_xtal_stop_calc.params = (void *)(uint32_t)ticker_id;

		retval = work_schedule(&s_work_xtal_stop_calc, 1);
		LL_ASSERT(!retval);
	}
#endif
}

static uint8_t channel_calc(uint8_t *channel_use, uint8_t hop,
			    uint16_t latency, uint8_t *channel_map,
			    uint8_t channel_count)
{
	uint8_t channel_next;

	channel_next = ((*channel_use) + (hop * (1 + latency))) % 37;
	*channel_use = channel_next;

	if ((channel_map[channel_next >> 3] & (1 << (channel_next % 8))) == 0) {
		uint8_t channel_index;
		uint8_t byte_count;

		channel_index = channel_next % channel_count;
		channel_next = 0;

		byte_count = 5;
		while (byte_count--) {
			uint8_t bite;
			uint8_t bit_count;

			bite = *channel_map;
			bit_count = 8;
			while (bit_count--) {
				if (bite & 0x01) {
					if (channel_index == 0) {
						break;
					}
					channel_index--;
				}
				channel_next++;
				bite >>= 1;
			}

			if (bit_count < 8) {
				break;
			}

			channel_map++;
		}
	} else {
		/* channel can be used, return it */
	}

	return channel_next;
}

static void channel_set(uint32_t channel)
{
	switch (channel) {
	case 37:
		radio_freq_chnl_set(2);
		break;

	case 38:
		radio_freq_chnl_set(26);
		break;

	case 39:
		radio_freq_chnl_set(80);
		break;

	default:
		if (channel < 11) {
			radio_freq_chnl_set(4 + (2 * channel));
		} else if (channel < 40) {
			radio_freq_chnl_set(28 + (2 * (channel - 11)));
		} else {
			LL_ASSERT(0);
		}
		break;
	}

	radio_whiten_iv_set(channel);
}

/** @brief Prepare access address as per BT Spec.
 *
 * - It shall have no more than six consecutive zeros or ones.
 * - It shall not be the advertising channel packets' Access Address.
 * - It shall not be a sequence that differs from the advertising channel
 *   packets Access Address by only one bit.
 * - It shall not have all four octets equal.
 * - It shall have no more than 24 transitions.
 * - It shall have a minimum of two transitions in the most significant six
 *   bits.
 */
static uint32_t access_addr_get(void)
{
	uint32_t access_addr;
	uint8_t bit_idx;
	uint8_t transitions;
	uint8_t consecutive_cnt;
	uint8_t consecutive_bit;

	rand_get(sizeof(uint32_t), (uint8_t *)&access_addr);

	bit_idx = 31;
	transitions = 0;
	consecutive_cnt = 1;
	consecutive_bit = (access_addr >> bit_idx) & 0x01;
	while (bit_idx--) {
		uint8_t bit;

		bit = (access_addr >> bit_idx) & 0x01;
		if (bit == consecutive_bit) {
			consecutive_cnt++;
		} else {
			consecutive_cnt = 1;
			consecutive_bit = bit;
			transitions++;
		}

		/* It shall have no more than six consecutive zeros or ones. */
		/* It shall have a minimum of two transitions in the most
		 * significant six bits.
		 */
		if ((consecutive_cnt > 6)
		    || ((bit_idx < 28) && (transitions < 2))) {
			if (consecutive_bit) {
				consecutive_bit = 0;
				access_addr &= ~(1 << bit_idx);
			} else {
				consecutive_bit = 1;
				access_addr |= (1 << bit_idx);
			}

			consecutive_cnt = 1;
			transitions++;
		}

		/* It shall have no more than 24 transitions */
		if (transitions > 24) {
			if (consecutive_bit) {
				access_addr &= ~((1 << (bit_idx + 1)) - 1);
			} else {
				access_addr |= ((1 << (bit_idx + 1)) - 1);
			}

			break;
		}
	}

	/** @todo proper access address calculations
	 * It shall not be the advertising channel packets Access Address.
	 * It shall not be a sequence that differs from the advertising channel
	 * packets Access Address by only one bit.
	 * It shall not have all four octets equal.
	 */

	return access_addr;
}

static void adv_obs_conn_configure(uint8_t phy)
{
	radio_reset();
	radio_phy_set(phy);
	radio_tx_power_set(0);
	radio_tmr_tifs_set(150);
	radio_isr_set(isr);
}

static void adv_obs_configure(uint8_t phy)
{
	uint32_t aa = 0x8e89bed6;

	adv_obs_conn_configure(phy);
	radio_aa_set((uint8_t *)&aa);
	radio_pkt_configure(phy, 6, 37);
	radio_crc_configure(((0x5bUL) | ((0x06UL) << 8) | ((0x00UL) << 16)),
			    0x555555);
}

void radio_event_adv_prepare(uint32_t ticks_at_expire, uint32_t remainder,
			     uint16_t lazy, void *context)
{
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	DEBUG_RADIO_PREPARE_A(1);

	_radio.ticker_id_prepare = RADIO_TICKER_ID_ADV;

	event_common_prepare(ticks_at_expire, remainder,
			     &_radio.advertiser.hdr.ticks_xtal_to_start,
			     &_radio.advertiser.hdr.ticks_active_to_start,
			     _radio.advertiser.hdr.ticks_preempt_to_start,
			     RADIO_TICKER_ID_ADV, event_adv, 0);

	DEBUG_RADIO_PREPARE_A(0);
}

static void adv_setup(void)
{
	uint8_t bitmap;
	uint8_t channel;

	/* Use latest adv packet */
	if (_radio.advertiser.adv_data.first !=
	    _radio.advertiser.adv_data.last) {
		uint8_t first;

		first = _radio.advertiser.adv_data.first + 1;
		if (first == DOUBLE_BUFFER_SIZE) {
			first = 0;
		}
		_radio.advertiser.adv_data.first = first;
	}

	radio_pkt_tx_set(&_radio.advertiser.adv_data.data
			 [_radio.advertiser.adv_data.first][0]);
	radio_switch_complete_and_rx();

	bitmap = _radio.advertiser.chl_map_current;
	channel = 0;
	while ((bitmap & 0x01) == 0) {
		channel++;
		bitmap >>= 1;
	}
	_radio.advertiser.chl_map_current &=
		(_radio.advertiser.chl_map_current - 1);

	channel_set(37 + channel);
}

static void event_adv(uint32_t ticks_at_expire, uint32_t remainder,
		      uint16_t lazy, void *context)
{
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	DEBUG_RADIO_START_A(1);

	LL_ASSERT(_radio.role == ROLE_NONE);
	LL_ASSERT(_radio.ticker_id_prepare == RADIO_TICKER_ID_ADV);

	/** @todo check if XTAL is started,
	 * options 1: abort Radio Start,
	 * 2: wait for XTAL start.
	 */

	_radio.role = ROLE_ADV;
	_radio.state = STATE_TX;
	_radio.ticker_id_prepare = 0;
	_radio.ticker_id_event = RADIO_TICKER_ID_ADV;
	_radio.ticks_anchor = ticks_at_expire;

	adv_obs_configure(RADIO_PHY_ADV);

	_radio.advertiser.chl_map_current = _radio.advertiser.chl_map;
	adv_setup();

	/* Setup Radio Filter */
	if (_radio.advertiser.filter_policy) {
		radio_filter_configure(_radio.advertiser.filter_enable_bitmask,
				       _radio.advertiser.filter_addr_type_bitmask,
				       (uint8_t *)_radio.advertiser.filter_bdaddr);
	}

	radio_tmr_start(1,
			ticks_at_expire +
			TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
			_radio.remainder_anchor);
	radio_tmr_end_capture();

#if (XTAL_ADVANCED && (RADIO_TICKER_PREEMPT_PART_US \
			<= RADIO_TICKER_PREEMPT_PART_MIN_US))
	/* check if preempt to start has changed */
	if (preempt_calc(&_radio.advertiser.hdr, RADIO_TICKER_ID_ADV,
			 ticks_at_expire) != 0) {
		_radio.state = STATE_STOP;
		radio_disable();
	} else
#endif
		/* Ticker Job Silence */
#if (WORK_TICKER_WORKER0_IRQ_PRIORITY == WORK_TICKER_JOB0_IRQ_PRIORITY)
	{
		uint32_t ticker_status;

		ticker_status =
		    ticker_job_idle_get(RADIO_TICKER_INSTANCE_ID_RADIO,
					RADIO_TICKER_USER_ID_WORKER,
					ticker_job_disable, 0);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
#endif

	DEBUG_RADIO_START_A(0);
}

void event_adv_stop(uint32_t ticks_at_expire, uint32_t remainder,
		    uint16_t lazy, void *context)
{
	uint32_t ticker_status;
	struct radio_pdu_node_rx *radio_pdu_node_rx;
	struct pdu_data *pdu_data_rx;
	struct radio_le_conn_cmplt *radio_le_conn_cmplt;

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	/* Stop Direct Adv */
	ticker_status =
	    ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
			RADIO_TICKER_USER_ID_WORKER, RADIO_TICKER_ID_ADV,
			ticker_success_assert, (void *)__LINE__);
	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));
	/** @todo synchronize stopping of scanner, i.e. pre-event and event
	 * needs to complete
	 */
	/* below lines are temporary */
	ticker_status = ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_WORKER,
				    RADIO_TICKER_ID_MARKER_0,
				    ticker_success_assert, (void *)__LINE__);
	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));
	ticker_status = ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_WORKER, RADIO_TICKER_ID_EVENT,
				    ticker_success_assert, (void *)__LINE__);
	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));

	/* Prepare the rx packet structure */
	radio_pdu_node_rx = packet_rx_reserve_get(1);
	LL_ASSERT(radio_pdu_node_rx);

	/** Connection handle */
	radio_pdu_node_rx->hdr.handle = 0xffff;
					    /** @todo */
	radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_CONNECTION;

	/* prepare connection complete structure */
	pdu_data_rx = (struct pdu_data *)radio_pdu_node_rx->pdu_data;
	radio_le_conn_cmplt =
		(struct radio_le_conn_cmplt *)&pdu_data_rx->payload;
	memset(radio_le_conn_cmplt, 0x00, sizeof(struct radio_le_conn_cmplt));
	radio_le_conn_cmplt->status = 0x3c;

	/* enqueue connection complete structure into queue */
	packet_rx_enqueue();
}

static void event_obs_prepare(uint32_t ticks_at_expire, uint32_t remainder,
			      uint16_t lazy, void *context)
{
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	DEBUG_RADIO_PREPARE_O(1);

	_radio.ticker_id_prepare = RADIO_TICKER_ID_OBS;

	event_common_prepare(ticks_at_expire, remainder,
			     &_radio.observer.hdr.ticks_xtal_to_start,
			     &_radio.observer.hdr.ticks_active_to_start,
			     _radio.observer.hdr.ticks_preempt_to_start,
			     RADIO_TICKER_ID_OBS, event_obs, 0);

#if SCHED_ADVANCED
	/* calc next group in us for the anchor where first connection event
	 * to be placed
	 */
	if (_radio.observer.conn) {
		static struct work _work_sched_after_master_free_offset_get = {
			0, 0, 0, WORK_TICKER_JOB0_IRQ,
			(work_fp) work_sched_after_master_free_offset_get, 0 };
		uint32_t ticks_at_expire_normal = ticks_at_expire;
		uint32_t retval;

		if (_radio.observer.hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
			uint32_t ticks_prepare_to_start =
				(_radio.observer.hdr.ticks_active_to_start >
				 _radio.observer.hdr.ticks_preempt_to_start) ?
				_radio.observer.hdr.ticks_active_to_start :
				_radio.observer.hdr.ticks_preempt_to_start;

			ticks_at_expire_normal -=
				((_radio.observer.hdr.ticks_xtal_to_start &
				  (~((uint32_t)1 << 31))) -
				 ticks_prepare_to_start);
		}

		_work_sched_after_master_free_offset_get.params =
			(void *)ticks_at_expire_normal;

		retval = work_schedule(&_work_sched_after_master_free_offset_get,
				       1);
		LL_ASSERT(!retval);
	}
#endif

	DEBUG_RADIO_PREPARE_O(0);
}

static void event_obs(uint32_t ticks_at_expire, uint32_t remainder,
		      uint16_t lazy, void *context)
{
	uint32_t ticker_status;

	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(context);

	DEBUG_RADIO_START_O(1);

	LL_ASSERT(_radio.role == ROLE_NONE);
	LL_ASSERT(_radio.ticker_id_prepare == RADIO_TICKER_ID_OBS);

	/** @todo check if XTAL is started, options 1: abort Radio Start,
	 * 2: wait for XTAL start
	 */
	_radio.role = ROLE_OBS;
	_radio.state = STATE_RX;
	_radio.ticker_id_prepare = 0;
	_radio.ticker_id_event = RADIO_TICKER_ID_OBS;
	_radio.ticks_anchor = ticks_at_expire;
	_radio.observer.scan_state = 0;

	adv_obs_configure(RADIO_PHY_ADV);

	channel_set(37 + _radio.observer.scan_channel++);
	if (_radio.observer.scan_channel == 3) {
		_radio.observer.scan_channel = 0;
	}

	radio_pkt_rx_set(_radio.packet_rx[_radio.packet_rx_last]->pdu_data);
	radio_switch_complete_and_tx();
	radio_rssi_measure();

	/* Setup Radio Filter */
	if (_radio.observer.filter_policy) {
		radio_filter_configure(_radio.observer.filter_enable_bitmask,
				       _radio.observer.filter_addr_type_bitmask,
				       (uint8_t *)_radio.observer.filter_bdaddr);

		if (_radio.nirk) {
			radio_ar_configure(_radio.nirk, _radio.irk);
		}
	}

	radio_tmr_start(0,
			ticks_at_expire +
			TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
			_radio.remainder_anchor);
	radio_tmr_end_capture();

#if (XTAL_ADVANCED && (RADIO_TICKER_PREEMPT_PART_US\
			<= RADIO_TICKER_PREEMPT_PART_MIN_US))
	/* check if preempt to start has changed */
	if (preempt_calc(&_radio.observer.hdr, RADIO_TICKER_ID_OBS,
			 ticks_at_expire) != 0) {
		_radio.state = STATE_STOP;
		radio_disable();
	} else
#endif
	{
		/* start window close timeout */
		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_OBS_STOP, ticks_at_expire,
				     _radio.observer.ticks_window +
				     TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
				     TICKER_NULL_PERIOD, TICKER_NULL_REMAINDER,
				     TICKER_NULL_LAZY, TICKER_NULL_SLOT,
				     event_stop, (void *)STATE_STOP,
				     ticker_success_assert, (void *)__LINE__);

		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		/* Ticker Job Silence */
#if (WORK_TICKER_WORKER0_IRQ_PRIORITY == WORK_TICKER_JOB0_IRQ_PRIORITY)
		{
			uint32_t ticker_status;

			ticker_status =
				ticker_job_idle_get(RADIO_TICKER_INSTANCE_ID_RADIO,
						    RADIO_TICKER_USER_ID_WORKER,
						    ticker_job_disable, 0);

			LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
				  (ticker_status == TICKER_STATUS_BUSY));
		}
#endif
	}

	DEBUG_RADIO_START_O(0);
}

static inline void event_conn_update_st_init(struct connection *conn,
					     uint16_t event_counter,
					     struct pdu_data *pdu_ctrl_tx,
					     uint32_t ticks_at_expire,
					     struct work *work_sched_offset,
					     work_fp fp_work_select_or_use)
{
	/* move to in progress */
	conn->llcp.connection_update.state = LLCP_CONN_STATE_INPROG;

	/* set instant */
	conn->llcp.connection_update.instant =
		event_counter + conn->latency + 6;

	/* place the conn update req packet as next in tx queue */
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_conn_update_req);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_REQ;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.win_size =
		conn->llcp.connection_update.win_size;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.
		win_offset = conn->llcp.connection_update.win_offset_us / 1250;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.interval =
		conn->llcp.connection_update.interval;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.latency =
		conn->llcp.connection_update.latency;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.timeout =
		conn->llcp.connection_update.timeout;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.instant =
		conn->llcp.connection_update.instant;

#if SCHED_ADVANCED
	{
		uint32_t retval;

		/* calculate window offset that places the connection in the
		 * next available slot after existing masters.
		 */
		conn->llcp.connection_update.ticks_ref = ticks_at_expire;
		if (conn->hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
			uint32_t ticks_prepare_to_start =
				(conn->hdr.ticks_active_to_start >
				 conn->hdr.ticks_preempt_to_start) ?
				conn->hdr.ticks_active_to_start :
				conn->hdr.ticks_preempt_to_start;

			conn->llcp.connection_update.ticks_ref -=
				((conn->hdr.ticks_xtal_to_start &
				  (~((uint32_t)1 << 31))) -
				 ticks_prepare_to_start);
		}

		conn->llcp.connection_update.pdu_win_offset = (uint16_t *)
			&pdu_ctrl_tx->payload.llctrl.ctrldata.conn_update_req.win_offset;

		work_sched_offset->fp = fp_work_select_or_use;
		work_sched_offset->params = (void *)conn;

		retval = work_schedule(work_sched_offset, 1);
		LL_ASSERT(!retval);
	}
#else
	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(work_sched_offset);
	ARG_UNUSED(fp_work_select_or_use);
#endif
}

static inline void event_conn_update_st_req(struct connection *conn,
					    uint16_t event_counter,
					    struct pdu_data *pdu_ctrl_tx,
					    uint32_t ticks_at_expire,
					    struct work *work_sched_offset)
{
	/* move to wait for conn_update/rsp/rej */
	conn->llcp.connection_update.state = LLCP_CONN_STATE_RSP_WAIT;

	/* place the conn param req packet as next in tx queue */
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_conn_param_req);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.interval_min =
		conn->llcp.connection_update.interval;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.interval_max =
		conn->llcp.connection_update.interval;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.latency =
		conn->llcp.connection_update.latency;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.timeout =
		conn->llcp.connection_update.timeout;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.preferred_periodicity = 0;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.reference_conn_event_count = event_counter;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset0 = 0x0000;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset1 = 0xffff;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset2 = 0xffff;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset3 = 0xffff;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset4 = 0xffff;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset5 = 0xffff;

	/* Start Procedure Timeout */
	conn->procedure_expire = conn->procedure_reload;

#if SCHED_ADVANCED
	{
		uint32_t retval;

		conn->llcp.connection_update.ticks_ref = ticks_at_expire;
		if (conn->hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
			uint32_t ticks_prepare_to_start =
				(conn->hdr.ticks_active_to_start >
				 conn->hdr.ticks_preempt_to_start) ?
				conn->hdr.ticks_active_to_start :
				conn->hdr.ticks_preempt_to_start;

			conn->llcp.connection_update.ticks_ref -=
				((conn->hdr.ticks_xtal_to_start &
				  (~((uint32_t)1 << 31))) -
				 ticks_prepare_to_start);
		}

		conn->llcp.connection_update.pdu_win_offset = (uint16_t *)
			&pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset0;

		work_sched_offset->fp = work_sched_free_win_offset_calc;
		work_sched_offset->params = (void *)conn;

		retval = work_schedule(work_sched_offset, 1);
		LL_ASSERT(!retval);
	}
#else
	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(work_sched_offset);
#endif
}

static inline void event_conn_update_st_rsp(struct connection *conn,
					    struct pdu_data *pdu_ctrl_tx)
{
	/* procedure request acked */
	conn->llcp_ack = conn->llcp_req;

	/* reset mutex */
	_radio.conn_upd = NULL;

	/** @todo REJECT_IND_EXT */

	/* place the conn param rsp packet as next in tx queue */
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_conn_param_rsp);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.interval_min =
		conn->llcp.connection_update.interval;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.interval_max =
		conn->llcp.connection_update.interval;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.latency =
		conn->llcp.connection_update.latency;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.timeout =
		conn->llcp.connection_update.timeout;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.preferred_periodicity =
		conn->llcp.connection_update.preferred_periodicity;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.reference_conn_event_count =
		conn->llcp.connection_update.instant;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset0 =
		conn->llcp.connection_update.offset0;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset1 =
		conn->llcp.connection_update.offset1;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset2 =
		conn->llcp.connection_update.offset2;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset3 =
		conn->llcp.connection_update.offset3;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset4 =
		conn->llcp.connection_update.offset4;
	pdu_ctrl_tx->payload.llctrl.ctrldata.conn_param_req.offset5 =
		conn->llcp.connection_update.offset5;
}

static inline uint32_t event_conn_update_prep(struct connection *conn,
					      uint16_t event_counter,
					      uint32_t ticks_at_expire)
{
	struct connection *conn_upd;
	uint16_t instant_latency;

	conn_upd = _radio.conn_upd;

	/* set mutex */
	if (!conn_upd) {
		_radio.conn_upd = conn;
	}

	instant_latency =
		((event_counter - conn->llcp.connection_update.instant) &
		 0xffff);
	if (conn->llcp.connection_update.state) {
		if (((conn_upd == 0) || (conn_upd == conn)) &&
		    (conn->llcp.connection_update.state !=
		     LLCP_CONN_STATE_APP_WAIT) &&
		    (conn->llcp.connection_update.state !=
		     LLCP_CONN_STATE_RSP_WAIT)) {
#if SCHED_ADVANCED
			static struct work gs_work_sched_offset = {
				0, 0, 0, WORK_TICKER_JOB0_IRQ, 0, 0 };
			work_fp fp_work_select_or_use;
#endif
			struct radio_pdu_node_tx *node_tx;
			struct pdu_data *pdu_ctrl_tx;
			uint8_t state;

			node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
			if (!node_tx) {
				return 1;
			}

			pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;

#if SCHED_ADVANCED
			fp_work_select_or_use = work_sched_win_offset_use;
#endif
			state = conn->llcp.connection_update.state;
			if ((state == LLCP_CONN_STATE_RSP) &&
			    (conn->role.master.role == 0)) {
				state = LLCP_CONN_STATE_INITIATE;
#if SCHED_ADVANCED
				fp_work_select_or_use =
					work_sched_win_offset_select;
#endif
			}

			switch (state) {
			case LLCP_CONN_STATE_INITIATE:
				if (conn->role.master.role == 0) {
#if SCHED_ADVANCED
					event_conn_update_st_init(conn,
								  event_counter,
								  pdu_ctrl_tx,
								  ticks_at_expire,
								  &gs_work_sched_offset,
								  fp_work_select_or_use);
#else
					event_conn_update_st_init(conn,
								  event_counter,
								  pdu_ctrl_tx,
								  ticks_at_expire,
								  NULL,
								  NULL);
#endif
					break;
				}
				/* fall thru if slave */

			case LLCP_CONN_STATE_REQ:
#if SCHED_ADVANCED
				event_conn_update_st_req(conn,
							 event_counter,
							 pdu_ctrl_tx,
							 ticks_at_expire,
							 &gs_work_sched_offset);
#else
				event_conn_update_st_req(conn,
							 event_counter,
							 pdu_ctrl_tx,
							 ticks_at_expire,
							 NULL);
#endif
				break;

			case LLCP_CONN_STATE_RSP:
				event_conn_update_st_rsp(conn, pdu_ctrl_tx);
				break;

			default:
				LL_ASSERT(0);
				break;
			}

			ctrl_tx_enqueue(conn, node_tx);
		}
	} else if (instant_latency <= 0x7FFF) {
		struct radio_pdu_node_rx *radio_pdu_node_rx;
		struct pdu_data *pdu_data_rx;
		struct radio_le_conn_update_cmplt *radio_le_conn_update_cmplt;
		uint32_t ticker_status;
		uint32_t conn_interval_us;
		uint32_t periodic_us;
		uint32_t ticks_win_offset;
		uint32_t ticks_slot_offset;
		uint16_t conn_interval_old;
		uint16_t conn_interval_new;
		uint16_t latency;
		uint32_t work_was_enabled;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* Reset ticker_id_prepare as role is not continued further
		 * due to conn update at this event.
		 */
		_radio.ticker_id_prepare = 0;

		/* reset mutex */
		if (_radio.conn_upd == conn) {
			_radio.conn_upd = NULL;
		}

		/* Prepare the rx packet structure */
		if ((conn->llcp.connection_update.interval !=
		     conn->conn_interval) ||
		    (conn->llcp.connection_update.latency != conn->latency) ||
		    (conn->llcp.connection_update.timeout !=
		     (conn->conn_interval * conn->supervision_reload * 125 / 1000))) {
			radio_pdu_node_rx = packet_rx_reserve_get(2);
			LL_ASSERT(radio_pdu_node_rx);

			radio_pdu_node_rx->hdr.handle = conn->handle;
			radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_CONN_UPDATE;

			/* prepare connection update complete structure */
			pdu_data_rx =
				(struct pdu_data *)radio_pdu_node_rx->pdu_data;
			radio_le_conn_update_cmplt =
				(struct radio_le_conn_update_cmplt *)
				&pdu_data_rx->payload;
			radio_le_conn_update_cmplt->status = 0x00;
			radio_le_conn_update_cmplt->interval =
				conn->llcp.connection_update.interval;
			radio_le_conn_update_cmplt->latency =
				conn->llcp.connection_update.latency;
			radio_le_conn_update_cmplt->timeout =
				conn->llcp.connection_update.timeout;

			/* enqueue connection update complete structure
			 * into queue.
			 */
			packet_rx_enqueue();
		}

		/* restore to normal prepare */
		if (conn->hdr.ticks_xtal_to_start & ((uint32_t)1 << 31)) {
			uint32_t ticks_prepare_to_start =
				(conn->hdr.ticks_active_to_start >
				 conn->hdr.ticks_preempt_to_start) ?
				conn->hdr.ticks_active_to_start :
				conn->hdr.ticks_preempt_to_start;

			conn->hdr.ticks_xtal_to_start &= ~((uint32_t)1 << 31);
			ticks_at_expire -= (conn->hdr.ticks_xtal_to_start -
					    ticks_prepare_to_start);
		}

		/* compensate for instant_latency due to laziness */
		conn_interval_old = instant_latency * conn->conn_interval;
		latency = conn_interval_old /
			conn->llcp.connection_update.interval;
		conn_interval_new = latency *
			conn->llcp.connection_update.interval;
		if (conn_interval_new > conn_interval_old) {
			ticks_at_expire +=
				TICKER_US_TO_TICKS((conn_interval_new -
						    conn_interval_old) * 1250);
		} else {
			ticks_at_expire -=
				TICKER_US_TO_TICKS((conn_interval_old -
						    conn_interval_new) * 1250);
		}
		conn->latency_prepare -= (instant_latency - latency);

		/* calculate the offset, window widening and interval */
		ticks_slot_offset =
			(conn->hdr.ticks_active_to_start <
			 conn->hdr.ticks_xtal_to_start) ?
			conn->hdr.ticks_xtal_to_start :
			conn->hdr.ticks_active_to_start;
		conn_interval_us = conn->llcp.connection_update.interval * 1250;
		periodic_us = conn_interval_us;
		if (conn->role.slave.role != 0) {
			conn->role.slave.window_widening_prepare_us -=
				conn->role.slave.window_widening_periodic_us *
				instant_latency;

			conn->role.slave.window_widening_periodic_us =
				(((gc_lookup_ppm[_radio.sca] +
				   gc_lookup_ppm[conn->role.slave.sca]) *
				  conn_interval_us) + (1000000 - 1)) / 1000000;
			conn->role.slave.window_widening_max_us =
				(conn_interval_us >> 1) - 150;
			conn->role.slave.window_size_prepare_us =
				conn->llcp.connection_update.win_size * 1250;
			conn->role.slave.ticks_to_offset = 0;

			conn->role.slave.window_widening_prepare_us +=
				conn->role.slave.window_widening_periodic_us *
				latency;
			if (conn->role.slave.window_widening_prepare_us >
			    conn->role.slave.window_widening_max_us) {
				conn->role.slave.window_widening_prepare_us =
					conn->role.slave.window_widening_max_us;
			}

			ticks_at_expire -=
				TICKER_US_TO_TICKS(conn->role.slave.window_widening_periodic_us *
						   latency);
			ticks_win_offset =
				TICKER_US_TO_TICKS((conn->llcp.connection_update.win_offset_us /
						    1250) * 1250);
			periodic_us -=
				conn->role.slave.window_widening_periodic_us;

			if (conn->llcp.connection_update.is_internal == 2) {
				conn_update_req(conn);
			}
		} else {
			ticks_win_offset =
				TICKER_US_TO_TICKS(conn->llcp.connection_update.win_offset_us);
		}
		conn->conn_interval = conn->llcp.connection_update.interval;
		conn->latency = conn->llcp.connection_update.latency;
		conn->supervision_reload =
			RADIO_CONN_EVENTS((conn->llcp.connection_update.timeout
					   * 10 * 1000), conn_interval_us);
		conn->procedure_reload =
			RADIO_CONN_EVENTS((40 * 1000 * 1000), conn_interval_us);
		conn->apto_reload = RADIO_CONN_EVENTS((30 * 1000 * 1000),
						      conn_interval_us);
		conn->appto_reload =
			(conn->apto_reload > (conn->latency + 2)) ?
			(conn->apto_reload - (conn->latency + 2)) :
			conn->apto_reload;
		if (!conn->llcp.connection_update.is_internal) {
			conn->supervision_expire = 0;
		}

		/* disable ticker job, in order to chain stop and start
		 * to avoid RTC being stopped if no tickers active.
		 */
		work_was_enabled = work_is_enabled(WORK_TICKER_JOB0_IRQ);
		work_disable(WORK_TICKER_JOB0_IRQ);

		/* start slave/master with new timings */
		ticker_status =
			ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_WORKER,
				    RADIO_TICKER_ID_FIRST_CONNECTION +
				    conn->handle, ticker_success_assert,
				    (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_WORKER,
				     RADIO_TICKER_ID_FIRST_CONNECTION +
				     conn->handle,
				     ticks_at_expire, ticks_win_offset,
				     TICKER_US_TO_TICKS(periodic_us),
				     TICKER_REMAINDER(periodic_us),
				     TICKER_NULL_LAZY,
				     (ticks_slot_offset + conn->hdr.ticks_slot),
				     (conn->role.slave.role != 0) ?
				     event_slave_prepare : event_master_prepare,
				     conn, ticker_success_assert,
				     (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));

		/* enable ticker job, if disabled in this function */
		if (work_was_enabled) {
			work_enable(WORK_TICKER_JOB0_IRQ);
		}

		return 0;
	}

	return 1;
}

static inline void event_ch_map_prep(struct connection *conn,
				     uint16_t event_counter)
{
	if (conn->llcp.channel_map.initiate) {
		struct radio_pdu_node_tx *node_tx;

		node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
		if (node_tx) {
			struct pdu_data *pdu_ctrl_tx =
				(struct pdu_data *)node_tx->pdu_data;

			/* reset initiate flag */
			conn->llcp.channel_map.initiate = 0;

			/* set instant */
			conn->llcp.channel_map.instant =
				event_counter + conn->latency + 6;

			/* place the channel map req packet as next in
			 * tx queue
			 */
			pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
			pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl,
						    ctrldata) +
				sizeof(struct pdu_data_llctrl_channel_map_req);
			pdu_ctrl_tx->payload.llctrl.opcode =
				PDU_DATA_LLCTRL_TYPE_CHANNEL_MAP_REQ;
			memcpy(&pdu_ctrl_tx->payload.llctrl.
			       ctrldata.channel_map_req.chm[0],
			       &conn->llcp.channel_map.chm[0],
			       sizeof(pdu_ctrl_tx->payload.
				      llctrl.ctrldata.channel_map_req.chm));
			pdu_ctrl_tx->payload.llctrl.ctrldata.channel_map_req.instant =
				conn->llcp.channel_map.instant;

			ctrl_tx_enqueue(conn, node_tx);
		}
	} else
	    if (((event_counter - conn->llcp.channel_map.instant) & 0xFFFF)
			    <= 0x7FFF) {
		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* copy to active channel map */
		memcpy(&conn->data_channel_map[0],
		       &conn->llcp.channel_map.chm[0],
		       sizeof(conn->data_channel_map));
		conn->data_channel_count =
			util_ones_count_get(&conn->data_channel_map[0],
					    sizeof(conn->data_channel_map));
	}

}

static inline void event_enc_prep(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;

	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	if (node_tx) {
		struct pdu_data *pdu_ctrl_tx =
			(struct pdu_data *)node_tx->pdu_data;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* master sends encrypted enc start rsp in control priority */
		if (conn->role.master.role == 0) {
			/* calc the Session Key */
			ecb_encrypt(&conn->llcp.encryption.ltk[0],
				    &conn->llcp.encryption.skd[0],
				    0, &conn->ccm_rx.key[0]);

			/* copy the Session Key */
			memcpy(&conn->ccm_tx.key[0], &conn->ccm_rx.key[0],
			       sizeof(conn->ccm_tx.key));

			/* copy the IV */
			memcpy(&conn->ccm_tx.iv[0], &conn->ccm_rx.iv[0],
			       sizeof(conn->ccm_tx.iv));

			/* initialise counter */
			conn->ccm_rx.counter = 0;
			conn->ccm_tx.counter = 0;

			/* set direction: slave to master = 0,
			 * master to slave = 1
			 */
			conn->ccm_rx.direction = 0;
			conn->ccm_tx.direction = 1;

			/* enable receive and transmit encryption */
			conn->enc_rx = 1;
			conn->enc_tx = 1;

			/* send enc start resp */
			start_enc_rsp_send(conn, pdu_ctrl_tx);
		}
		/* slave send reject ind or start enc req at control priority */
		else if (!conn->pause_tx || conn->refresh) {
			/* ll ctrl packet */
			pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;

			/* place the reject ind packet as next in tx queue */
			if (conn->llcp.encryption.error_code) {
				pdu_ctrl_tx->len =
				    offsetof(struct pdu_data_llctrl, ctrldata) +
				    sizeof(struct pdu_data_llctrl_reject_ind);
				pdu_ctrl_tx->payload.llctrl.opcode =
					PDU_DATA_LLCTRL_TYPE_REJECT_IND;
				pdu_ctrl_tx->payload.llctrl.ctrldata.reject_ind.error_code =
					conn->llcp.encryption.error_code;

				conn->llcp.encryption.error_code = 0;
			}
			/* place the start enc req packet as next in tx queue */
			else {
#if !FAST_ENC_PROCEDURE
				/* TODO BT Spec. text: may finalize the sending
				 * of additional data channel PDUs queued in the
				 * controller.
				 */
				enc_rsp_send(conn);
#endif

				/* calc the Session Key */
				ecb_encrypt(&conn->llcp.encryption.ltk[0],
					    &conn->llcp.encryption.skd[0], 0,
					    &conn->ccm_rx.key[0]);

				/* copy the Session Key */
				memcpy(&conn->ccm_tx.key[0],
				       &conn->ccm_rx.key[0],
				       sizeof(conn->ccm_tx.key));

				/* copy the IV */
				memcpy(&conn->ccm_tx.iv[0], &conn->ccm_rx.iv[0],
				       sizeof(conn->ccm_tx.iv));

				/* initialise counter */
				conn->ccm_rx.counter = 0;
				conn->ccm_tx.counter = 0;

				/* set direction: slave to master = 0,
				 * master to slave = 1
				 */
				conn->ccm_rx.direction = 1;
				conn->ccm_tx.direction = 0;

				/* enable receive encryption (transmit turned
				 * on when start enc resp from master is
				 * received)
				 */
				conn->enc_rx = 1;

				/* prepare the start enc req */
				pdu_ctrl_tx->len =
					offsetof(struct pdu_data_llctrl,
						 ctrldata);
				pdu_ctrl_tx->payload.llctrl.opcode =
					PDU_DATA_LLCTRL_TYPE_START_ENC_REQ;
			}
		} else {
#if !FAST_ENC_PROCEDURE
			/* enable transmit encryption */
			_radio.conn_curr->enc_tx = 1;

			start_enc_rsp_send(_radio.conn_curr, 0);

			/* resume data packet rx and tx */
			_radio.conn_curr->pause_rx = 0;
			_radio.conn_curr->pause_tx = 0;
#else
			/* Fast Enc implementation shall have enqueued the
			 * start enc rsp in the radio ISR itself, we should
			 * not get here.
			 */
			LL_ASSERT(0);
#endif
		}

		ctrl_tx_enqueue(conn, node_tx);
	}
}

static inline void event_fex_prep(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;

	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	if (node_tx) {
		struct pdu_data *pdu_ctrl_tx =
			(struct pdu_data *)node_tx->pdu_data;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* use initial feature bitmap */
		conn->llcp_features = RADIO_BLE_FEATURES;

		/* place the feature exchange req packet as next in tx queue */
		pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
			sizeof(struct pdu_data_llctrl_feature_req);
		pdu_ctrl_tx->payload.llctrl.opcode =
			(conn->role.master.role == 0) ?
			PDU_DATA_LLCTRL_TYPE_FEATURE_REQ :
			PDU_DATA_LLCTRL_TYPE_SLAVE_FEATURE_REQ;
		memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.feature_req.features[0],
		       0x00,
		       sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.feature_req.features));

		pdu_ctrl_tx->payload.llctrl.ctrldata.feature_req.features[0] =
			conn->llcp_features;

		ctrl_tx_enqueue(conn, node_tx);

		/* Start Procedure Timeout (@todo this shall not replace
		 * terminate procedure)
		 */
		conn->procedure_expire = conn->procedure_reload;
	}

}

static inline void event_vex_prep(struct connection *conn)
{

	if (conn->llcp_version.tx == 0) {
		struct radio_pdu_node_tx *node_tx;

		node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
		if (node_tx) {
			struct pdu_data *pdu_ctrl_tx =
				(struct pdu_data *)node_tx->pdu_data;

			/* procedure request acked */
			conn->llcp_ack = conn->llcp_req;

			/* set version ind tx-ed flag */
			conn->llcp_version.tx = 1;

			/* place the version ind packet as next in tx queue */
			pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
			pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl,
						    ctrldata) +
				sizeof(struct pdu_data_llctrl_version_ind);
			pdu_ctrl_tx->payload.llctrl.opcode =
				PDU_DATA_LLCTRL_TYPE_VERSION_IND;
			pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.version_number =
				RADIO_BLE_VERSION_NUMBER;
			pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.company_id =
				RADIO_BLE_COMPANY_ID;
			pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.sub_version_number =
				RADIO_BLE_SUB_VERSION_NUMBER;

			ctrl_tx_enqueue(conn, node_tx);

			/* Start Procedure Timeout (@todo this shall not
			 * replace terminate procedure)
			 */
			conn->procedure_expire = conn->procedure_reload;
		}
	} else if (conn->llcp_version.rx != 0) {
		struct radio_pdu_node_rx *radio_pdu_node_rx;
		struct pdu_data *pdu_ctrl_rx;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* Prepare the rx packet structure */
		radio_pdu_node_rx = packet_rx_reserve_get(2);
		LL_ASSERT(radio_pdu_node_rx);

		radio_pdu_node_rx->hdr.handle = conn->handle;
		radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_DC_PDU;

		/* prepare version ind structure */
		pdu_ctrl_rx = (struct pdu_data *)radio_pdu_node_rx->pdu_data;
		pdu_ctrl_rx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_rx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
			sizeof(struct pdu_data_llctrl_version_ind);
		pdu_ctrl_rx->payload.llctrl.opcode =
			PDU_DATA_LLCTRL_TYPE_VERSION_IND;
		pdu_ctrl_rx->payload.llctrl.ctrldata.version_ind.version_number =
			conn->llcp_version.version_number;
		pdu_ctrl_rx->payload.llctrl.ctrldata.version_ind.company_id =
			conn->llcp_version.company_id;
		pdu_ctrl_rx->payload.llctrl.ctrldata.version_ind.sub_version_number =
			conn->llcp_version.sub_version_number;

		/* enqueue version ind structure into rx queue */
		packet_rx_enqueue();
	} else {
		/* tx-ed but no rx, and new request placed */
		LL_ASSERT(0);
	}

}

static inline void event_ping_prep(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;

	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	if (node_tx) {
		struct pdu_data *pdu_ctrl_tx =
			(struct pdu_data *)node_tx->pdu_data;

		/* procedure request acked */
		conn->llcp_ack = conn->llcp_req;

		/* place the ping req packet as next in tx queue */
		pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata);
		pdu_ctrl_tx->payload.llctrl.opcode =
			PDU_DATA_LLCTRL_TYPE_PING_REQ;

		ctrl_tx_enqueue(conn, node_tx);

		/* Start Procedure Timeout (@todo this shall not replace
		 * terminate procedure)
		 */
		conn->procedure_expire = conn->procedure_reload;
	}

}

static inline void event_len_prep(struct connection *conn)
{
	switch (conn->llcp_length.state) {
	case LLCP_LENGTH_STATE_REQ:
	{
		struct pdu_data_llctrl_length_req_rsp *lr;
		struct radio_pdu_node_tx *node_tx;
		struct pdu_data *pdu_ctrl_tx;
		uint16_t free_count_rx;

		free_count_rx = packet_rx_acquired_count_get() +
			mem_free_count_get(_radio.pkt_rx_data_free);
		LL_ASSERT(free_count_rx <= 0xFF);

		if (_radio.packet_rx_data_count != free_count_rx) {
			break;
		}

		node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
		if (!node_tx) {
			break;
		}

		/* wait for resp before completing the procedure */
		conn->llcp_length.state = LLCP_LENGTH_STATE_ACK_WAIT;

		/* set the default tx octets to requested value */
		conn->default_tx_octets = conn->llcp_length.tx_octets;

		/* place the length req packet as next in tx queue */
		pdu_ctrl_tx = (struct pdu_data *) node_tx->pdu_data;
		pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
			sizeof(struct pdu_data_llctrl_length_req_rsp);
		pdu_ctrl_tx->payload.llctrl.opcode =
			PDU_DATA_LLCTRL_TYPE_LENGTH_REQ;

		lr = (struct pdu_data_llctrl_length_req_rsp *)
			&pdu_ctrl_tx->payload.llctrl.ctrldata.length_req;
		lr->max_rx_octets = RADIO_LL_LENGTH_OCTETS_RX_MAX;
		lr->max_rx_time = ((RADIO_LL_LENGTH_OCTETS_RX_MAX + 14) << 3);
		lr->max_tx_octets = conn->default_tx_octets;
		lr->max_tx_time = ((conn->default_tx_octets + 14) << 3);

		ctrl_tx_enqueue(conn, node_tx);

		/* Start Procedure Timeout (@todo this shall not replace
		 * terminate procedure).
		 */
		conn->procedure_expire = conn->procedure_reload;
	}
	break;

	case LLCP_LENGTH_STATE_RESIZE:
	{
		struct pdu_data_llctrl_length_req_rsp *lr;
		struct radio_pdu_node_rx *node_rx;
		struct pdu_data *pdu_ctrl_rx;
		uint16_t packet_rx_data_size;
		uint16_t free_count_conn;
		uint16_t free_count_rx;

		/* Ensure the rx pool is not in use.
		 * This is important to be able to re-size the pool
		 * ensuring there is no chance that an operation on
		 * the pool is pre-empted causing memory corruption.
		 */
		free_count_rx = packet_rx_acquired_count_get() +
			mem_free_count_get(_radio.pkt_rx_data_free);
		LL_ASSERT(free_count_rx <= 0xFF);

		if (_radio.packet_rx_data_count != free_count_rx) {
			/** TODO another role instance has obtained
			 * memory from rx pool.
			 */
			LL_ASSERT(0);
		}

		/* Procedure complete */
		conn->llcp_length.ack = conn->llcp_length.req;
		conn->procedure_expire = 0;

		/* resume data packet tx */
		_radio.conn_curr->pause_tx = 0;

		/* Use the new rx octets in the connection */
		conn->max_rx_octets = conn->llcp_length.rx_octets;

		/** TODO This design is exception as memory initialization
		 * and allocation is done in radio context here, breaking the
		 * rule that the rx buffers are allocated in application
		 * context.
		 * Design mem_* such that mem_init could interrupt mem_acquire,
		 * when the pool is full?
		 */
		free_count_conn = mem_free_count_get(_radio.conn_free);
		if (_radio.advertiser.conn) {
			free_count_conn++;
		}
		if (_radio.observer.conn) {
			free_count_conn++;
		}
		packet_rx_data_size = ALIGN4(offsetof(struct radio_pdu_node_rx,
						      pdu_data) +
					     offsetof(struct pdu_data,
						      payload) +
					     conn->max_rx_octets);
		/* Resize to lower or higher size if this is the only active
		 * connection, or resize to only higher sizes as there may be
		 * other connections using the current size.
		 */
		if (((free_count_conn + 1) == _radio.connection_count) ||
		    (packet_rx_data_size > _radio.packet_rx_data_size)) {
			/* as rx mem is to be re-sized, release acquired
			 * memq link.
			 */
			while (_radio.packet_rx_acquire !=
				_radio.packet_rx_last) {

				struct radio_pdu_node_rx *node_rx;

				if (_radio.packet_rx_acquire == 0) {
					_radio.packet_rx_acquire =
						_radio.packet_rx_count - 1;
				} else {
					_radio.packet_rx_acquire -= 1;
				}

				node_rx = _radio.packet_rx[
						_radio.packet_rx_acquire];
				mem_release(node_rx->hdr.onion.link,
					&_radio.link_rx_free);

				LL_ASSERT(_radio.link_rx_data_quota <
					  (_radio.packet_rx_count - 1));
				_radio.link_rx_data_quota++;

				/* no need to release node_rx as we mem_init
				 * later down in code.
				 */
			}

			/* calculate the new rx node size and new count */
			if (conn->max_rx_octets < (RADIO_ACPDU_SIZE_MAX + 1)) {
				_radio.packet_rx_data_size =
					ALIGN4(offsetof(struct radio_pdu_node_rx,
							pdu_data) +
					       (RADIO_ACPDU_SIZE_MAX + 1));
			} else {
				_radio.packet_rx_data_size =
					packet_rx_data_size;
			}
			_radio.packet_rx_data_count =
				_radio.packet_rx_data_pool_size /
				_radio.packet_rx_data_size;
			LL_ASSERT(_radio.packet_rx_data_count);

			/* re-size (re-init) the free rx pool */
			mem_init(_radio.pkt_rx_data_pool,
				 _radio.packet_rx_data_size,
				 _radio.packet_rx_data_count,
				 &_radio.pkt_rx_data_free);

			/* allocate the rx queue include one extra for
			 * generating event in following lines.
			 */
			packet_rx_allocate(4);
		}

		/* Prepare the rx packet structure */
		node_rx = packet_rx_reserve_get(2);
		LL_ASSERT(node_rx);
		node_rx->hdr.handle = conn->handle;
		node_rx->hdr.type = NODE_RX_TYPE_DC_PDU;

		/* prepare version ind structure */
		pdu_ctrl_rx = (struct pdu_data *) node_rx->pdu_data;
		pdu_ctrl_rx->ll_id = PDU_DATA_LLID_CTRL;
		pdu_ctrl_rx->len = offsetof(struct pdu_data_llctrl,
					    ctrldata) +
			sizeof(struct pdu_data_llctrl_length_req_rsp);
		pdu_ctrl_rx->payload.llctrl.opcode =
			PDU_DATA_LLCTRL_TYPE_LENGTH_RSP;

		lr = (struct pdu_data_llctrl_length_req_rsp *)
			&pdu_ctrl_rx->payload.llctrl.ctrldata.length_req;
		lr->max_rx_octets = conn->max_rx_octets;
		lr->max_rx_time = ((conn->max_rx_octets + 14) << 3);
		lr->max_tx_octets = conn->max_tx_octets;
		lr->max_tx_time = ((conn->max_tx_octets + 14) << 3);

		/* enqueue version ind structure into rx queue */
		packet_rx_enqueue();
	}
	break;

	case LLCP_LENGTH_STATE_ACK_WAIT:
	case LLCP_LENGTH_STATE_RSP_WAIT:
		/* no nothing */
		break;

	default:
		LL_ASSERT(0);
		break;
	}
}


static void event_connection_prepare(uint32_t ticks_at_expire,
				     uint32_t remainder, uint16_t lazy,
				     struct connection *conn)
{
	uint16_t event_counter;

	_radio.ticker_id_prepare =
	    RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle;

	/* Calc window widening */
	if (conn->role.slave.role != 0) {
		conn->role.slave.window_widening_prepare_us +=
		    conn->role.slave.window_widening_periodic_us * (lazy + 1);
		if (conn->role.slave.window_widening_prepare_us >
		    conn->role.slave.window_widening_max_us) {
			conn->role.slave.window_widening_prepare_us =
				conn->role.slave.window_widening_max_us;
		}
	}

	/* save the latency for use in event */
	conn->latency_prepare += lazy;

	/* calc current event counter value */
	event_counter = conn->event_counter + conn->latency_prepare;

	/* check if procedure is requested */
	if (conn->llcp_ack != conn->llcp_req) {
		/* Stop previous event, to avoid Radio DMA corrupting the
		 * rx queue
		 */
		event_stop(0, 0, 0, (void *)STATE_ABORT);

		switch (conn->llcp_type) {
		case LLCP_CONNECTION_UPDATE:
			if (event_conn_update_prep(conn, event_counter,
						   ticks_at_expire) == 0) {
				return;
			}
			break;
		case LLCP_CHANNEL_MAP:
			event_ch_map_prep(conn, event_counter);
			break;

		case LLCP_ENCRYPTION:
			event_enc_prep(conn);
			break;

		case LLCP_FEATURE_EXCHANGE:
			event_fex_prep(conn);
			break;

		case LLCP_VERSION_EXCHANGE:
			event_vex_prep(conn);
			break;

		case LLCP_PING:
			event_ping_prep(conn);
			break;

		default:
			LL_ASSERT(0);
			break;
		}
	}

	/* Terminate Procedure Request */
	if (conn->llcp_terminate.ack != conn->llcp_terminate.req) {
		struct radio_pdu_node_tx *node_tx;

		/* Stop previous event, to avoid Radio DMA corrupting the rx
		 * queue
		 */
		event_stop(0, 0, 0, (void *)STATE_ABORT);

		node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
		if (node_tx) {
			struct pdu_data *pdu_ctrl_tx =
			    (struct pdu_data *)node_tx->pdu_data;

			/* Terminate Procedure acked */
			conn->llcp_terminate.ack = conn->llcp_terminate.req;

			/* place the terminate ind packet in tx queue */
			pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
			pdu_ctrl_tx->len =
				offsetof(struct pdu_data_llctrl, ctrldata) +
				sizeof(struct pdu_data_llctrl_terminate_ind);
			pdu_ctrl_tx->payload.llctrl.opcode =
				PDU_DATA_LLCTRL_TYPE_TERMINATE_IND;
			pdu_ctrl_tx->payload.llctrl.ctrldata.terminate_ind.
				error_code = conn->llcp_terminate.reason_own;

			ctrl_tx_enqueue(conn, node_tx);

			/* Terminate Procedure timeout is started, will
			 * replace any other timeout running
			 */
			conn->procedure_expire = conn->procedure_reload;
		}
	}

	/* check if procedure is requested */
	if (conn->llcp_length.ack != conn->llcp_length.req) {
		/* Stop previous event, to avoid Radio DMA corrupting the
		 * rx queue
		 */
		event_stop(0, 0, 0, (void *)STATE_ABORT);

		/* handle DLU state machine */
		event_len_prep(conn);
	}

	/* Setup XTAL startup and radio active events */
	event_common_prepare(ticks_at_expire, remainder,
			     &conn->hdr.ticks_xtal_to_start,
			     &conn->hdr.ticks_active_to_start,
			     conn->hdr.ticks_preempt_to_start,
			     (RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle),
			     (conn->role.slave.role != 0) ? event_slave : event_master,
			     conn);

	/* store the next event counter value */
	conn->event_counter = event_counter + 1;
}

static void connection_configure(struct connection *conn)
{
	uint8_t phy;

	phy = RADIO_PHY_CONN;
	adv_obs_conn_configure(phy);
	radio_aa_set(conn->access_addr);
	radio_crc_configure(((0x5bUL) | ((0x06UL) << 8) | ((0x00UL) << 16)),
			    (((uint32_t)conn->crc_init[2] << 16) |
			     ((uint32_t)conn->crc_init[1] << 8) |
			     ((uint32_t)conn->crc_init[0])));
}

static void event_slave_prepare(uint32_t ticks_at_expire, uint32_t remainder,
				uint16_t lazy, void *context)
{
	DEBUG_RADIO_PREPARE_S(1);

	event_connection_prepare(ticks_at_expire, remainder, lazy, context);

	DEBUG_RADIO_PREPARE_S(0);
}

static void event_slave(uint32_t ticks_at_expire, uint32_t remainder,
			uint16_t lazy, void *context)
{
	struct connection *conn;
	uint8_t data_channel_use;
	uint32_t remainder_us;

	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);

	DEBUG_RADIO_START_S(1);

	LL_ASSERT(_radio.role == ROLE_NONE);

	conn = (struct connection *)context;
	LL_ASSERT(_radio.ticker_id_prepare ==
		  (RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle));

	_radio.role = ROLE_SLAVE;
	_radio.state = STATE_RX;
	_radio.ticker_id_prepare = 0;
	_radio.ticker_id_event =
		(RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle);
	_radio.ticks_anchor = ticks_at_expire;
	_radio.packet_counter = 0;
	_radio.crc_expire = 0;

	_radio.conn_curr = conn;

	conn->latency_event = conn->latency_prepare;
	conn->latency_prepare = 0;

	connection_configure(conn);

	rx_packet_set(conn, (struct pdu_data *)
		      _radio.packet_rx[_radio.packet_rx_last]->pdu_data);

	radio_switch_complete_and_tx();
	radio_rssi_measure();

	/* Setup Radio Channel */
	data_channel_use = channel_calc(&conn->data_channel_use,
					conn->data_channel_hop,
					conn->latency_event,
					&conn->data_channel_map[0],
					conn->data_channel_count);
	channel_set(data_channel_use);

	/* current window widening */
	conn->role.slave.window_widening_event_us +=
		conn->role.slave.window_widening_prepare_us;
	conn->role.slave.window_widening_prepare_us = 0;
	if (conn->role.slave.window_widening_event_us >
	    conn->role.slave.window_widening_max_us) {
		conn->role.slave.window_widening_event_us =
			conn->role.slave.window_widening_max_us;
	}

	/* current window size */
	conn->role.slave.window_size_event_us +=
		conn->role.slave.window_size_prepare_us;
	conn->role.slave.window_size_prepare_us = 0;

	remainder_us =
		radio_tmr_start(0, ticks_at_expire +
				TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
				_radio.remainder_anchor);
	radio_tmr_aa_capture();
	radio_tmr_hcto_configure(0 + remainder_us + RADIO_RX_READY_DELAY_US +
				 (conn->role.slave.window_widening_event_us << 1) +
/* +/- 16 us of BLE jitter plus own implementation drift unit of 30.51 us. */
			       (RADIO_TICKER_JITTER_US << 2) +
			       RADIO_PREAMBLE_TO_ADDRESS_US +
			       conn->role.slave.window_size_event_us);
	radio_tmr_end_capture();

#if (XTAL_ADVANCED && (RADIO_TICKER_PREEMPT_PART_US \
		<= RADIO_TICKER_PREEMPT_PART_MIN_US))
	/* check if preempt to start has changed */
	if (preempt_calc(&conn->hdr, (RADIO_TICKER_ID_FIRST_CONNECTION +
				      conn->handle), ticks_at_expire) != 0) {
		_radio.state = STATE_STOP;
		radio_disable();
	} else
#endif

		/* Ticker Job Silence */
#if (WORK_TICKER_WORKER0_IRQ_PRIORITY == WORK_TICKER_JOB0_IRQ_PRIORITY)
	{
		uint32_t ticker_status;

		ticker_status =
			ticker_job_idle_get(RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    ticker_job_disable, 0);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
#endif

	/* Route the tx packet to respective connections */
	packet_tx_enqueue(2);

	DEBUG_RADIO_START_S(0);
}

static void event_master_prepare(uint32_t ticks_at_expire, uint32_t remainder,
				 uint16_t lazy, void *context)
{
	DEBUG_RADIO_PREPARE_M(1);

	event_connection_prepare(ticks_at_expire, remainder, lazy, context);

	DEBUG_RADIO_PREPARE_M(0);
}

static void event_master(uint32_t ticks_at_expire, uint32_t remainder,
			 uint16_t lazy, void *context)
{
	struct connection *conn;
	struct pdu_data *pdu_data_tx;
	uint8_t data_channel_use;

	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);

	DEBUG_RADIO_START_M(1);

	LL_ASSERT(_radio.role == ROLE_NONE);

	conn = (struct connection *)context;
	LL_ASSERT(_radio.ticker_id_prepare ==
		  (RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle));

	_radio.role = ROLE_MASTER;
	_radio.state = STATE_TX;
	_radio.ticker_id_prepare = 0;
	_radio.ticker_id_event =
	    (RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle);
	_radio.ticks_anchor = ticks_at_expire;
	_radio.packet_counter = 0;
	_radio.crc_expire = 0;

	_radio.conn_curr = conn;

	conn->latency_event = conn->latency_prepare;
	conn->latency_prepare = 0;

	/* Route the tx packet to respective connections */
	packet_tx_enqueue(2);

	/* prepare transmit packet */
	prepare_pdu_data_tx(conn, &pdu_data_tx);

	pdu_data_tx->sn = conn->sn;
	pdu_data_tx->nesn = conn->nesn;

	connection_configure(conn);

	tx_packet_set(conn, pdu_data_tx);
	radio_switch_complete_and_rx();

	/* Setup Radio Channel */
	data_channel_use = channel_calc(&conn->data_channel_use,
					conn->data_channel_hop,
					conn->latency_event,
					&conn->data_channel_map[0],
					conn->data_channel_count);
	channel_set(data_channel_use);

	/* normal connection! */
#if SILENT_CONNECTION
	if ((!conn->empty) || (pdu_data_tx->md) ||
	    ((conn->supervision_expire != 0) &&
	     (conn->supervision_expire <= 6)) ||
	    ((conn->role.master.connect_expire != 0) &&
	     (conn->role.master.connect_expire <= 6)))
#endif
	{
		radio_tmr_start(1, ticks_at_expire +
				TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
				_radio.remainder_anchor);
		radio_tmr_end_capture();
	}
#if SILENT_CONNECTION
	/* silent connection! */
	else {
		uint32_t remainder_us;

		/* start in RX state */
		_radio.state = STATE_RX;
		_radio.packet_counter = 0xFF;

		rx_packet_set(conn, (struct pdu_data *)_radio.
			      packet_rx[_radio.packet_rx_last]->pdu_data);
		radio_switch_complete_and_tx();

		/* setup pkticker and hcto */
		remainder_us =
			radio_tmr_start(0, ticks_at_expire +
					TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US),
					_radio.remainder_anchor);
		radio_tmr_aa_capture();
#define QUICK_FIX_EXTRA_WINDOW 230
		radio_tmr_hcto_configure(0 + remainder_us +
					 RADIO_TX_READY_DELAY_US + 230 +
					 RADIO_PREAMBLE_TO_ADDRESS_US +
					 QUICK_FIX_EXTRA_WINDOW);
#undef QUICK_FIX_EXTRA_WINDOW
	}
#endif

#if (XTAL_ADVANCED && (RADIO_TICKER_PREEMPT_PART_US \
			<= RADIO_TICKER_PREEMPT_PART_MIN_US))
	/* check if preempt to start has changed */
	if (0 !=
	    preempt_calc(&conn->hdr, (RADIO_TICKER_ID_FIRST_CONNECTION +
				      conn->handle), ticks_at_expire)) {
		_radio.state = STATE_STOP;
		radio_disable();
	} else
#endif

	/* Ticker Job Silence */
#if (WORK_TICKER_WORKER0_IRQ_PRIORITY == WORK_TICKER_JOB0_IRQ_PRIORITY)
	{
		uint32_t ticker_status;

		ticker_status =
			ticker_job_idle_get(RADIO_TICKER_INSTANCE_ID_RADIO,
					    RADIO_TICKER_USER_ID_WORKER,
					    ticker_job_disable, 0);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
#endif

	DEBUG_RADIO_START_M(0);
}

static void rx_packet_set(struct connection *conn, struct pdu_data *pdu_data_rx)
{
	uint8_t phy;

	phy = RADIO_PHY_CONN;
	if (conn->enc_rx) {
		radio_pkt_configure(phy, 8, (conn->max_rx_octets + 4));

		radio_pkt_rx_set(radio_ccm_rx_pkt_set(&conn->ccm_rx,
						      pdu_data_rx));
	} else {
		radio_pkt_configure(phy, 8, conn->max_rx_octets);

		radio_pkt_rx_set(pdu_data_rx);
	}
}

static void tx_packet_set(struct connection *conn,
			  struct pdu_data *pdu_data_tx)
{
	uint8_t phy;

	phy = RADIO_PHY_CONN;
	if (conn->enc_tx) {
		radio_pkt_configure(phy, 8, (conn->max_tx_octets + 4));

		radio_pkt_tx_set(radio_ccm_tx_pkt_set(&conn->ccm_tx,
						      pdu_data_tx));
	} else {
		radio_pkt_configure(phy, 8, conn->max_tx_octets);

		radio_pkt_tx_set(pdu_data_tx);
	}
}

static void prepare_pdu_data_tx(struct connection *conn,
				struct pdu_data **pdu_data_tx)
{
	struct pdu_data *_pdu_data_tx;

	/*@FIXME: assign before checking first 3 conditions */
	_pdu_data_tx = (struct pdu_data *)conn->pkt_tx_head->pdu_data;

	if ((conn->empty != 0) || /* empty packet */
	   /* no ctrl or data packet */
	   (conn->pkt_tx_head == 0) ||
	   /* data tx paused, only control packets allowed */
	   ((conn->pause_tx) && (_pdu_data_tx != 0) &&
	    (_pdu_data_tx->len != 0) &&
	    ((_pdu_data_tx->ll_id != PDU_DATA_LLID_CTRL) ||
	     ((conn->role.master.role == 0) &&
	      (((conn->refresh == 0) &&
		(_pdu_data_tx->payload.llctrl.opcode !=
		 PDU_DATA_LLCTRL_TYPE_TERMINATE_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT)) ||
	       ((conn->refresh != 0) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_TERMINATE_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_ENC_REQ) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT)))) ||
	     ((conn->role.slave.role != 0) &&
	      (((conn->refresh == 0) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_TERMINATE_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_REQ) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT)) ||
	       ((conn->refresh != 0) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_TERMINATE_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_REQ) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_START_ENC_RSP) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND) &&
		(_pdu_data_tx->payload.llctrl.opcode != PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT))))))) {
			_pdu_data_tx = empty_tx_enqueue(conn);
	} else {
		_pdu_data_tx =
			(struct pdu_data *)(conn->pkt_tx_head->pdu_data +
					    conn->packet_tx_head_offset);

		if (!conn->packet_tx_head_len) {
			conn->packet_tx_head_len = _pdu_data_tx->len;
		}

		if (conn->packet_tx_head_offset) {
			_pdu_data_tx->ll_id = PDU_DATA_LLID_DATA_CONTINUE;
		}

		_pdu_data_tx->len = conn->packet_tx_head_len -
		    conn->packet_tx_head_offset;
		_pdu_data_tx->md = 0;

		if (_pdu_data_tx->len > conn->max_tx_octets) {
			_pdu_data_tx->len = conn->max_tx_octets;
			_pdu_data_tx->md = 1;
		}

		if (conn->pkt_tx_head->next) {
			_pdu_data_tx->md = 1;
		}
	}

	_pdu_data_tx->rfu0 = 0;
	_pdu_data_tx->resv = 0;

	*pdu_data_tx = _pdu_data_tx;
}

static void packet_rx_allocate(uint8_t max)
{
	uint8_t acquire;

	if (max > _radio.link_rx_data_quota) {
		max = _radio.link_rx_data_quota;
	}

	acquire = _radio.packet_rx_acquire + 1;
	if (acquire == _radio.packet_rx_count) {
		acquire = 0;
	}

	while ((max--) && (acquire != _radio.packet_rx_last)) {
		void *link;
		struct radio_pdu_node_rx *radio_pdu_node_rx;

		link = mem_acquire(&_radio.link_rx_free);
		if (!link) {
			break;
		}

		radio_pdu_node_rx = mem_acquire(&_radio.pkt_rx_data_free);
		if (!radio_pdu_node_rx) {
			mem_release(link, &_radio.link_rx_free);
			break;
		}

		radio_pdu_node_rx->hdr.onion.link = link;

		_radio.packet_rx[_radio.packet_rx_acquire] = radio_pdu_node_rx;
		_radio.packet_rx_acquire = acquire;

		acquire = _radio.packet_rx_acquire + 1;
		if (acquire == _radio.packet_rx_count) {
			acquire = 0;
		}

		_radio.link_rx_data_quota--;
	}
}

static uint8_t packet_rx_acquired_count_get(void)
{
	if (_radio.packet_rx_acquire >=
	    _radio.packet_rx_last) {
		return (_radio.packet_rx_acquire -
			_radio.packet_rx_last);
	} else {
		return (_radio.packet_rx_count -
			_radio.packet_rx_last +
			_radio.packet_rx_acquire);
	}
}

static struct radio_pdu_node_rx *packet_rx_reserve_get(uint8_t count)
{
	struct radio_pdu_node_rx *radio_pdu_node_rx;

	if (_radio.packet_rx_last > _radio.packet_rx_acquire) {
		if (count >
		    ((_radio.packet_rx_count - _radio.packet_rx_last) +
		     _radio.packet_rx_acquire)) {
			return 0;
		}
	} else {
		if (count >
		    (_radio.packet_rx_acquire - _radio.packet_rx_last)) {
			return 0;
		}
	}

	radio_pdu_node_rx = _radio.packet_rx[_radio.packet_rx_last];
	radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_DC_PDU;

	return radio_pdu_node_rx;
}

static void packet_rx_enqueue(void)
{
	void *link;
	struct radio_pdu_node_rx *radio_pdu_node_rx;
	uint8_t last;

	LL_ASSERT(_radio.packet_rx_last != _radio.packet_rx_acquire);

	/* Remember the rx node and acquired link mem */
	radio_pdu_node_rx = _radio.packet_rx[_radio.packet_rx_last];
	link = radio_pdu_node_rx->hdr.onion.link;

	/* serialize release queue with rx queue by storing reference to last
	 * element in release queue
	 */
	radio_pdu_node_rx->hdr.onion.packet_release_last =
	    _radio.packet_release_last;

	/* dequeue from acquired rx queue */
	last = _radio.packet_rx_last + 1;
	if (last == _radio.packet_rx_count) {
		last = 0;
	}
	_radio.packet_rx_last = last;

	/* Enqueue into event-cum-data queue */
	link = memq_enqueue(radio_pdu_node_rx, link,
			    (void *)&_radio.link_rx_tail);
	LL_ASSERT(link);

	/* callback to trigger application action */
	radio_event_callback();
}

static void packet_tx_enqueue(uint8_t max)
{
	while ((max--) && (_radio.packet_tx_first != _radio.packet_tx_last)) {
		struct pdu_data_q_tx *pdu_data_q_tx;
		struct radio_pdu_node_tx *node_tx_new;
		struct connection *conn;
		uint8_t first;

		pdu_data_q_tx = &_radio.pkt_tx[_radio.packet_tx_first];
		node_tx_new = pdu_data_q_tx->node_tx;
		node_tx_new->next = NULL;
		conn = mem_get(_radio.conn_pool, CONNECTION_T_SIZE,
				pdu_data_q_tx->handle);

		if (conn->handle == pdu_data_q_tx->handle) {
			if (conn->pkt_tx_data == 0) {
				conn->pkt_tx_data = node_tx_new;

				if (conn->pkt_tx_head == 0) {
					conn->pkt_tx_head = node_tx_new;
					conn->pkt_tx_last = NULL;
				}
			}

			if (conn->pkt_tx_last) {
				conn->pkt_tx_last->next = node_tx_new;
			}

			conn->pkt_tx_last = node_tx_new;
		} else {
			struct pdu_data *pdu_data_tx;

			pdu_data_tx = (struct pdu_data *)node_tx_new->pdu_data;

			/* By setting it resv, when app gets num cmplt, no
			 * num cmplt is counted, but the buffer is released
			 */
			pdu_data_tx->ll_id = PDU_DATA_LLID_RESV;

			pdu_node_tx_release(pdu_data_q_tx->handle, node_tx_new);
		}

		first = _radio.packet_tx_first + 1;
		if (first == _radio.packet_tx_count) {
			first = 0;
		}
		_radio.packet_tx_first = first;
	}
}

static struct pdu_data *empty_tx_enqueue(struct connection *conn)
{
	struct pdu_data *pdu_data_tx;

	conn->empty = 1;

	pdu_data_tx = (struct pdu_data *)radio_pkt_empty_get();
	pdu_data_tx->ll_id = PDU_DATA_LLID_DATA_CONTINUE;
	pdu_data_tx->len = 0;
	if (conn->pkt_tx_head) {
		pdu_data_tx->md = 1;
	} else {
		pdu_data_tx->md = 0;
	}

	return pdu_data_tx;
}

static void ctrl_tx_enqueue(struct connection *conn,
			    struct radio_pdu_node_tx *node_tx)
{
	/* check if a packet was tx-ed and not acked by peer */
	if (
	    /* An explicit empty PDU is not enqueued */
	    (conn->empty == 0) &&
	    /* and data/ctrl packet is in the head */
	    (conn->pkt_tx_head) && (
				    /* data PDU tx is not paused */
				    (conn->pause_tx == 0) ||
				    /* or ctrl PDU already at head */
				    (conn->pkt_tx_head == conn->pkt_tx_ctrl))) {
		/* data or ctrl may have been transmitted once, but not acked
		 * by peer, hence place this new ctrl after head
		 */

		/* if data transmited once, keep it at head of the tx list,
		 * as we will insert a ctrl after it, hence advance the
		 * data pointer
		 */
		if (conn->pkt_tx_head == conn->pkt_tx_data) {
			conn->pkt_tx_data = conn->pkt_tx_data->next;
		}

		/* if no ctrl packet already queued, new ctrl added will be
		 * the ctrl pointer and is inserted after head.
		 */
		if (conn->pkt_tx_ctrl == 0) {
			node_tx->next = conn->pkt_tx_head->next;
			conn->pkt_tx_head->next = node_tx;
			conn->pkt_tx_ctrl = node_tx;
		} else {
			/* TODO support for more than 2 pending ctrl packets. */
			LL_ASSERT(conn->pkt_tx_ctrl->next == conn->pkt_tx_data);

			node_tx->next = conn->pkt_tx_ctrl->next;
			conn->pkt_tx_ctrl->next = node_tx;
		}
	} else {
		/* No packet needing ACK. */

		/* If first ctrl packet then add it as head else add it to the
		 * tail of the ctrl packets.
		 */
		if (!conn->pkt_tx_ctrl) {
			node_tx->next = conn->pkt_tx_head;
			conn->pkt_tx_head = node_tx;
			conn->pkt_tx_ctrl = node_tx;
		} else {
			/* TODO support for more than 2 pending ctrl packets. */
			LL_ASSERT(conn->pkt_tx_ctrl->next == conn->pkt_tx_data);

			node_tx->next = conn->pkt_tx_ctrl->next;
			conn->pkt_tx_ctrl->next = node_tx;
		}
	}

	/* Update last pointer if ctrl added at end of tx list */
	if (node_tx->next == 0) {
		conn->pkt_tx_last = node_tx;
	}
}

static void pdu_node_tx_release(uint16_t handle,
				struct radio_pdu_node_tx *node_tx)
{
	uint8_t last;

	last = _radio.packet_release_last + 1;
	if (last == _radio.packet_tx_count) {
		last = 0;
	}

	LL_ASSERT(last != _radio.packet_release_first);

	/* Enqueue app mem for release */
	_radio.pkt_release[_radio.packet_release_last].handle = handle;
	_radio.pkt_release[_radio.packet_release_last].node_tx = node_tx;
	_radio.packet_release_last = last;

	radio_event_callback();
}

static void connection_release(struct connection *conn)
{
	uint32_t ticker_status;

	/* Enable Ticker Job, we are in a radio event which disabled it if
	 * worker0 and job0 priority where same.
	 */
	work_enable(WORK_TICKER_JOB0_IRQ);

	/** @todo correctly stop tickers ensuring crystal and radio active are
	 * placed in right states
	 */

	/* Stop Master/Slave role ticker */
	ticker_status =
		ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
			    RADIO_TICKER_USER_ID_WORKER,
			    (RADIO_TICKER_ID_FIRST_CONNECTION + conn->handle),
			    ticker_success_assert, (void *)__LINE__);
	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));

	/* Stop Marker 0 and event single-shot tickers */
	if ((_radio.state == STATE_ABORT) &&
	    (_radio.ticker_id_prepare == (RADIO_TICKER_ID_FIRST_CONNECTION +
					  conn->handle))) {
		ticker_status =
			ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_WORKER,
				    RADIO_TICKER_ID_MARKER_0,
				    ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
		ticker_status =
		    ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				RADIO_TICKER_USER_ID_WORKER,
				RADIO_TICKER_ID_EVENT,
				ticker_success_assert, (void *)__LINE__);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}

	/* flush and release, data packet before ctrl */
	while ((conn->pkt_tx_head != conn->pkt_tx_ctrl) &&
	       (conn->pkt_tx_head != conn->pkt_tx_data)) {
		struct radio_pdu_node_tx *node_tx;
		struct pdu_data *pdu_data_tx;

		/* By setting it resv, when app gets num cmplt, no num cmplt
		 * is counted, but the buffer is released
		 */
		node_tx = conn->pkt_tx_head;
		pdu_data_tx = (struct pdu_data *)node_tx->pdu_data;
		pdu_data_tx->ll_id = PDU_DATA_LLID_RESV;

		conn->pkt_tx_head = conn->pkt_tx_head->next;

		pdu_node_tx_release(conn->handle, node_tx);
	}

	/* flush and release, ctrl packet before data */
	while ((conn->pkt_tx_head) &&
	       (conn->pkt_tx_head != conn->pkt_tx_data)) {
		void *release;

		release = conn->pkt_tx_head;
		conn->pkt_tx_head = conn->pkt_tx_head->next;
		conn->pkt_tx_ctrl = conn->pkt_tx_head;

		mem_release(release, &_radio.pkt_tx_ctrl_free);
	}
	conn->pkt_tx_ctrl = NULL;

	/* flush and release, rest of data */
	while (conn->pkt_tx_head) {
		struct radio_pdu_node_tx *node_tx;
		struct pdu_data *pdu_data_tx;

		/* By setting it resv, when app gets num cmplt, no num cmplt
		 * is counted, but the buffer is released
		 */
		node_tx = conn->pkt_tx_head;
		pdu_data_tx = (struct pdu_data *)node_tx->pdu_data;
		pdu_data_tx->ll_id = PDU_DATA_LLID_RESV;

		conn->pkt_tx_head = conn->pkt_tx_head->next;
		conn->pkt_tx_data = conn->pkt_tx_head;

		pdu_node_tx_release(conn->handle, node_tx);
	}

	conn->handle = 0xffff;

	/* reset mutex */
	if (_radio.conn_upd == conn) {
		_radio.conn_upd = NULL;
	}
}

static void terminate_ind_rx_enqueue(struct connection *conn, uint8_t reason)
{
	struct radio_pdu_node_rx *radio_pdu_node_rx;
	void *link;

	/* Prepare the rx packet structure */
	radio_pdu_node_rx =
		(struct radio_pdu_node_rx *)&conn->llcp_terminate.radio_pdu_node_rx;
	LL_ASSERT(radio_pdu_node_rx->hdr.onion.link);

	radio_pdu_node_rx->hdr.handle = conn->handle;
	radio_pdu_node_rx->hdr.type = NODE_RX_TYPE_TERMINATE;
	*((uint8_t *)radio_pdu_node_rx->pdu_data) = reason;

	/* Get the link mem reserved in the connection context */
	link = radio_pdu_node_rx->hdr.onion.link;

	/* Serialize release queue with rx queue by storing reference to
	 * last element in release queue
	 */
	radio_pdu_node_rx->hdr.onion.packet_release_last =
	    _radio.packet_release_last;

	/* Enqueue into event-cum-data queue */
	link = memq_enqueue(radio_pdu_node_rx, link,
			    (void *)&_radio.link_rx_tail);
	LL_ASSERT(link);

	/* callback to trigger application action */
	radio_event_callback();
}

static uint32_t conn_update(struct connection *conn,
			    struct pdu_data *pdu_data_rx)
{
	if (((pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.instant -
	      conn->event_counter) & 0xFFFF) > 0x7FFF) {
		return 1;
	}

	LL_ASSERT((conn->llcp_req == conn->llcp_ack) ||
		  ((conn->llcp_type == LLCP_CONNECTION_UPDATE) &&
		   (conn->llcp.connection_update.state ==
		    LLCP_CONN_STATE_RSP_WAIT)));

	/* set mutex, if only not already set. As a master the mutex shall
	 * be set, but a slave we accept it as new 'set' of mutex.
	 */
	if (_radio.conn_upd == 0) {
		LL_ASSERT(conn->role.slave.role != 0);

		_radio.conn_upd = conn;
	}

	conn->llcp.connection_update.win_size =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.win_size;
	conn->llcp.connection_update.win_offset_us =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.win_offset *
		1250;
	conn->llcp.connection_update.interval =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.interval;
	conn->llcp.connection_update.latency =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.latency;
	conn->llcp.connection_update.timeout =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.timeout;
	conn->llcp.connection_update.instant =
		pdu_data_rx->payload.llctrl.ctrldata.conn_update_req.instant;
	conn->llcp.connection_update.state = LLCP_CONN_STATE_INPROG;
	conn->llcp.connection_update.is_internal = 0;

	conn->llcp_type = LLCP_CONNECTION_UPDATE;
	conn->llcp_ack--;

	return 0;
}

static uint32_t is_peer_compatible(struct connection *conn)
{
	return ((conn->llcp_version.rx) &&
		(conn->llcp_version.version_number >= RADIO_BLE_VERSION_NUMBER) &&
		(conn->llcp_version.company_id == RADIO_BLE_COMPANY_ID) &&
		(conn->llcp_version.sub_version_number >= RADIO_BLE_SUB_VERSION_NUMBER));
}

static uint32_t conn_update_req(struct connection *conn)
{
	if (conn->llcp_req != conn->llcp_ack) {
		return 1;
	}

	if ((conn->role.master.role == 0) || (is_peer_compatible(conn))) {
		/** Perform slave intiated conn param req */
		conn->llcp.connection_update.win_size = 1;
		conn->llcp.connection_update.win_offset_us = 0;
		conn->llcp.connection_update.interval = conn->conn_interval;
		conn->llcp.connection_update.latency = conn->latency;
		conn->llcp.connection_update.timeout = conn->conn_interval *
			conn->supervision_reload * 125 / 1000;
		/* conn->llcp.connection_update.instant     = 0; */
		conn->llcp.connection_update.state =
			(conn->role.master.role == 0) ?
			LLCP_CONN_STATE_INITIATE : LLCP_CONN_STATE_REQ;
		conn->llcp.connection_update.is_internal = 1;

		conn->llcp_type = LLCP_CONNECTION_UPDATE;
		conn->llcp_ack--;

		return 0;
	}

	return 2;
}

static uint32_t channel_map_update(struct connection *conn,
				   struct pdu_data *pdu_data_rx)
{
	if (((pdu_data_rx->payload.llctrl.ctrldata.channel_map_req.instant -
	      conn->event_counter) & 0xffff) > 0x7fff) {
		return 1;
	}

	LL_ASSERT(conn->llcp_req == conn->llcp_ack);

	memcpy(&conn->llcp.channel_map.chm[0],
	       &pdu_data_rx->payload.llctrl.ctrldata.channel_map_req.chm[0],
	       sizeof(conn->llcp.channel_map.chm));
	conn->llcp.channel_map.instant =
		pdu_data_rx->payload.llctrl.ctrldata.channel_map_req.instant;
	conn->llcp.channel_map.initiate = 0;

	conn->llcp_type = LLCP_CHANNEL_MAP;
	conn->llcp_ack--;

	return 0;
}

static void enc_req_reused_send(struct connection *conn,
				struct radio_pdu_node_tx *node_tx)
{
	struct pdu_data *pdu_ctrl_tx;

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata)
		+ sizeof(struct pdu_data_llctrl_enc_req);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_ENC_REQ;
	memcpy(&pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.rand[0],
	       &conn->llcp.encryption.rand[0],
	       sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.rand));
	pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.ediv[0] =
		conn->llcp.encryption.ediv[0];
	pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.ediv[1] =
		conn->llcp.encryption.ediv[1];
	/** @todo */
	memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.skdm[0], 0xcc,
		sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.skdm));
	/** @todo */
	memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.ivm[0], 0xdd,
	       sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.enc_req.ivm));
}

static void enc_rsp_send(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata)
		+ sizeof(struct pdu_data_llctrl_enc_rsp);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_ENC_RSP;
	/** @todo */
	memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.skds[0], 0xaa,
		sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.skds));
	/** @todo */
	memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.ivs[0], 0xbb,
	       sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.ivs));

	/* things from slave stored for session key calculation */
	memcpy(&conn->llcp.encryption.skd[8],
	       &pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.skds[0], 8);
	memcpy(&conn->ccm_rx.iv[4],
	       &pdu_ctrl_tx->payload.llctrl.ctrldata.enc_rsp.ivs[0], 4);

	ctrl_tx_enqueue(conn, node_tx);
}

static void start_enc_rsp_send(struct connection *conn,
			       struct pdu_data *pdu_ctrl_tx)
{
	struct radio_pdu_node_tx *node_tx = NULL;

	if (!pdu_ctrl_tx) {
		/* acquire tx mem */
		node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
		LL_ASSERT(node_tx);

		pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	}

	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_START_ENC_RSP;

	if (node_tx) {
		ctrl_tx_enqueue(conn, node_tx);
	}
}

static void unknown_rsp_send(struct connection *conn, uint8_t type)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata)
		+ sizeof(struct pdu_data_llctrl_unknown_rsp);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP;
	pdu_ctrl_tx->payload.llctrl.ctrldata.unknown_rsp.type = type;

	ctrl_tx_enqueue(conn, node_tx);
}

static void feature_rsp_send(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_feature_rsp);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_FEATURE_RSP;
	memset(&pdu_ctrl_tx->payload.llctrl.ctrldata.feature_rsp.features[0],
		0x00,
		sizeof(pdu_ctrl_tx->payload.llctrl.ctrldata.feature_rsp.features));
	pdu_ctrl_tx->payload.llctrl.ctrldata.feature_rsp.features[0] =
		conn->llcp_features;

	ctrl_tx_enqueue(conn, node_tx);
}

static void pause_enc_rsp_send(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_RSP;

	ctrl_tx_enqueue(conn, node_tx);
}

static void version_ind_send(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_version_ind);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_VERSION_IND;
	pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.version_number =
		RADIO_BLE_VERSION_NUMBER;
	pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.company_id =
		RADIO_BLE_COMPANY_ID;
	pdu_ctrl_tx->payload.llctrl.ctrldata.version_ind.sub_version_number =
		RADIO_BLE_SUB_VERSION_NUMBER;

	ctrl_tx_enqueue(conn, node_tx);

	/* Apple work-around, add empty packet before version_ind */
	empty_tx_enqueue(conn);
}

static void ping_resp_send(struct connection *conn)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata);
	pdu_ctrl_tx->payload.llctrl.opcode = PDU_DATA_LLCTRL_TYPE_PING_RSP;

	ctrl_tx_enqueue(conn, node_tx);
}

static void reject_ind_ext_send(struct connection *conn,
				uint8_t reject_opcode, uint8_t error_code)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	/* acquire tx mem */
	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *)node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_reject_ind_ext);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_REJECT_IND_EXT;
	pdu_ctrl_tx->payload.llctrl.ctrldata.reject_ind_ext.reject_opcode =
		reject_opcode;
	pdu_ctrl_tx->payload.llctrl.ctrldata.reject_ind_ext.error_code =
		error_code;

	ctrl_tx_enqueue(conn, node_tx);
}

static void length_resp_send(struct connection *conn, uint16_t eff_rx_octets,
			     uint16_t eff_tx_octets)
{
	struct radio_pdu_node_tx *node_tx;
	struct pdu_data *pdu_ctrl_tx;

	node_tx = mem_acquire(&_radio.pkt_tx_ctrl_free);
	LL_ASSERT(node_tx);

	pdu_ctrl_tx = (struct pdu_data *) node_tx->pdu_data;
	pdu_ctrl_tx->ll_id = PDU_DATA_LLID_CTRL;
	pdu_ctrl_tx->len = offsetof(struct pdu_data_llctrl, ctrldata) +
		sizeof(struct pdu_data_llctrl_length_req_rsp);
	pdu_ctrl_tx->payload.llctrl.opcode =
		PDU_DATA_LLCTRL_TYPE_LENGTH_RSP;
	pdu_ctrl_tx->payload.llctrl.ctrldata.length_rsp.max_rx_octets =
		eff_rx_octets;
	pdu_ctrl_tx->payload.llctrl.ctrldata.length_rsp.max_rx_time =
		((eff_rx_octets + 14) << 3);
	pdu_ctrl_tx->payload.llctrl.ctrldata.length_rsp.max_tx_octets =
		eff_tx_octets;
	pdu_ctrl_tx->payload.llctrl.ctrldata.length_rsp.max_tx_time =
		((eff_tx_octets + 14) << 3);

	ctrl_tx_enqueue(conn, node_tx);
}

void radio_ticks_active_to_start_set(uint32_t ticks_active_to_start)
{
	_radio.ticks_active_to_start = ticks_active_to_start;
}

struct radio_adv_data *radio_adv_data_get(void)
{
	return &_radio.advertiser.adv_data;
}

struct radio_adv_data *radio_scan_data_get(void)
{
	return &_radio.advertiser.scan_data;
}

void radio_filter_clear(void)
{
	_radio.filter_enable_bitmask = 0;
	_radio.filter_addr_type_bitmask = 0;
}

uint32_t radio_filter_add(uint8_t addr_type, uint8_t *addr)
{
	if (_radio.filter_enable_bitmask != 0xFF) {
		uint8_t index;

		for (index = 0;
		     (_radio.filter_enable_bitmask & (1 << index));
		     index++) {
		}
		_radio.filter_enable_bitmask |= (1 << index);
		_radio.filter_addr_type_bitmask |=
			((addr_type & 0x01) << index);
		memcpy(&_radio.filter_bdaddr[index][0], addr, BDADDR_SIZE);

		return 0;
	}

	return 1;
}

uint32_t radio_filter_remove(uint8_t addr_type, uint8_t *addr)
{
	uint8_t index;

	if (!_radio.filter_enable_bitmask) {
		return 1;
	}

	index = 8;
	while (index--) {
		if ((_radio.filter_enable_bitmask & BIT(index)) &&
		    (((_radio.filter_addr_type_bitmask >> index) & 0x01) ==
		     (addr_type & 0x01)) &&
		    !memcmp(_radio.filter_bdaddr[index], addr, BDADDR_SIZE)) {
			_radio.filter_enable_bitmask &= ~BIT(index);
			_radio.filter_addr_type_bitmask &= ~BIT(index);

			return 0;
		}
	}

	return 1;
}

void radio_irk_clear(void)
{
	_radio.nirk = 0;
}

uint32_t radio_irk_add(uint8_t *irk)
{
	if (_radio.nirk >= RADIO_IRK_COUNT_MAX) {
		return 1;
	}

	memcpy(&_radio.irk[_radio.nirk][0], irk, 16);
	_radio.nirk++;

	return 0;
}

static struct connection *connection_get(uint16_t handle)
{
	struct connection *conn;

	if (handle < _radio.connection_count) {
		conn = mem_get(_radio.conn_pool, CONNECTION_T_SIZE, handle);
		if ((conn) && (conn->handle == handle)) {
			return conn;
		}
	}

	return 0;
}

static inline void role_active_disable(uint8_t ticker_id_stop,
				       uint32_t ticks_xtal_to_start,
				       uint32_t ticks_active_to_start)
{
	static struct work s_work_radio_inactive = { 0, 0, 0,
		WORK_TICKER_WORKER0_IRQ, (work_fp) work_radio_inactive, 0 };
	uint32_t volatile ticker_status_event;

	/* Step 2: Is caller before Event? Stop Event */
	ticker_status_event =
		ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
			    RADIO_TICKER_USER_ID_APP, RADIO_TICKER_ID_EVENT,
			    ticker_if_done, (void *)&ticker_status_event);

	if (ticker_status_event == TICKER_STATUS_BUSY) {
		work_enable(WORK_TICKER_JOB0_IRQ);

		LL_ASSERT(ticker_status_event != TICKER_STATUS_BUSY);
	}

	if (ticker_status_event == TICKER_STATUS_SUCCESS) {
		static struct work s_work_xtal_stop = { 0, 0, 0,
			WORK_TICKER_WORKER0_IRQ, (work_fp) work_xtal_stop, 0 };
		uint32_t volatile ticker_status_pre_event;

		/* Step 2.1: Is caller between Primary and Marker0?
		 * Stop the Marker0 event
		 */
		ticker_status_pre_event =
			ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_APP,
				    RADIO_TICKER_ID_MARKER_0,
				    ticker_if_done,
				    (void *)&ticker_status_pre_event);

		if (ticker_status_pre_event == TICKER_STATUS_BUSY) {
			work_enable(WORK_TICKER_JOB0_IRQ);

			LL_ASSERT(ticker_status_event != TICKER_STATUS_BUSY);
		}

		if (ticker_status_pre_event == TICKER_STATUS_SUCCESS) {
			/* Step 2.1.1: Check and deassert Radio Active or XTAL
			 * start
			 */
			if (ticks_active_to_start > ticks_xtal_to_start) {
				uint32_t retval;

				/* radio active asserted, handle deasserting
				 * here
				 */
				retval = work_schedule(&s_work_radio_inactive,
						0);
				LL_ASSERT(!retval);
			} else {
				uint32_t retval;

				/* XTAL started, handle XTAL stop here */
				retval = work_schedule(&s_work_xtal_stop, 0);
				LL_ASSERT(!retval);
			}
		} else if (ticker_status_pre_event == TICKER_STATUS_FAILURE) {
			uint32_t retval;

			/* Step 2.1.2: Deassert Radio Active and XTAL start */

			/* radio active asserted, handle deasserting here */
			retval = work_schedule(&s_work_radio_inactive, 0);
			LL_ASSERT(!retval);

			/* XTAL started, handle XTAL stop here */
			retval = work_schedule(&s_work_xtal_stop, 0);
			LL_ASSERT(!retval);
		} else {
			LL_ASSERT(0);
		}
	} else if (ticker_status_event == TICKER_STATUS_FAILURE) {
		uint32_t volatile ticker_status_stop;

		/* Step 3: Caller inside Event, handle graceful stop of Event
		 * (role dependent)
		 */
		/* Stop ticker "may" be in use for direct adv or observer,
		 * hence stop may fail if ticker not used.
		 */
		ticker_status_stop =
			ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
				    RADIO_TICKER_USER_ID_APP, ticker_id_stop,
				    ticker_if_done,
				    (void *)&ticker_status_stop);

		if (ticker_status_stop == TICKER_STATUS_BUSY) {
			work_enable(WORK_TICKER_JOB0_IRQ);

			LL_ASSERT(ticker_status_event != TICKER_STATUS_BUSY);
		}

		LL_ASSERT((ticker_status_stop == TICKER_STATUS_SUCCESS) ||
			  (ticker_status_stop == TICKER_STATUS_FAILURE));

		if (_radio.role != ROLE_NONE) {
			static struct work s_work_radio_stop = { 0, 0, 0,
				WORK_TICKER_WORKER0_IRQ,
				(work_fp) work_radio_stop, 0 };
			uint32_t retval;

			/* Radio state STOP is supplied in params */
			s_work_radio_stop.params = (void *)STATE_STOP;

			/* Stop Radio Tx/Rx */
			retval = work_schedule(&s_work_radio_stop, 0);
			LL_ASSERT(!retval);

			/* wait for radio ISR to exit */
			while (_radio.role != ROLE_NONE) {
				cpu_sleep();
			}
		}
	} else {
		LL_ASSERT(0);
	}

}

static uint32_t role_disable(uint8_t ticker_id_primary,
			     uint8_t ticker_id_stop)
{
	uint32_t volatile ticker_status;
	uint32_t ticks_xtal_to_start = 0;
	uint32_t ticks_active_to_start = 0;

	switch (ticker_id_primary) {
	case RADIO_TICKER_ID_ADV:
		ticks_xtal_to_start =
			_radio.advertiser.hdr.ticks_xtal_to_start;
		ticks_active_to_start =
			_radio.advertiser.hdr.ticks_active_to_start;
		break;

	case RADIO_TICKER_ID_OBS:
		ticks_xtal_to_start =
			_radio.observer.hdr.ticks_xtal_to_start;
		ticks_active_to_start =
			_radio.observer.hdr.ticks_active_to_start;
		break;
	default:
		if (ticker_id_primary >= RADIO_TICKER_ID_FIRST_CONNECTION) {
			struct connection *conn;
			uint16_t conn_handle;

			conn_handle = ticker_id_primary -
				      RADIO_TICKER_ID_FIRST_CONNECTION;
			conn = connection_get(conn_handle);
			if (!conn) {
				return 1;
			}

			ticks_xtal_to_start =
				conn->hdr.ticks_xtal_to_start;
			ticks_active_to_start =
				conn->hdr.ticks_active_to_start;
		} else {
			BT_ASSERT(0);
		}
		break;
	}

	/* Step 1: Is Primary started? Stop the Primary ticker */
	ticker_status =
		ticker_stop(RADIO_TICKER_INSTANCE_ID_RADIO,
			    RADIO_TICKER_USER_ID_APP,
			    ticker_id_primary, ticker_if_done,
			    (void *)&ticker_status);

	if (ticker_status == TICKER_STATUS_BUSY) {
		/* if inside our event, enable Job. */
		if (_radio.ticker_id_event == ticker_id_primary) {
			work_enable(WORK_TICKER_JOB0_IRQ);
		}

		/** @todo design to avoid this wait */
		while (ticker_status == TICKER_STATUS_BUSY) {
			cpu_sleep();
		}
	}

	if (ticker_status != TICKER_STATUS_SUCCESS) {
		return 1;
	}

	/* Inside our event, gracefully handle XTAL and Radio actives */
	if ((_radio.ticker_id_prepare == ticker_id_primary)
	    || (_radio.ticker_id_event == ticker_id_primary)) {

		role_active_disable(ticker_id_stop,
				    ticks_xtal_to_start, ticks_active_to_start);
	}

	return 0;
}

uint32_t radio_adv_enable(uint16_t interval, uint8_t chl_map,
			  uint8_t filter_policy)
{
	struct connection *conn;
	uint32_t volatile ticker_status;
	uint32_t ticks_slot_offset;
	struct pdu_adv *pdu_adv;

	pdu_adv = (struct pdu_adv *)
		&_radio.advertiser.adv_data.data[_radio.advertiser.adv_data.last][0];

	if ((pdu_adv->type == PDU_ADV_TYPE_ADV_IND) ||
	    (pdu_adv->type == PDU_ADV_TYPE_DIRECT_IND)) {
		void *link;

		if (_radio.advertiser.conn) {
			return 1;
		}

		link = mem_acquire(&_radio.link_rx_free);
		if (!link) {
			return 1;
		}

		conn = mem_acquire(&_radio.conn_free);
		if (!conn) {
			mem_release(link, &_radio.link_rx_free);

			return 1;
		}

		conn->handle = 0xFFFF;
		conn->llcp_features = RADIO_BLE_FEATURES;
		conn->data_channel_use = 0;
		conn->event_counter = 0;
		conn->latency_prepare = 0;
		conn->latency_event = 0;
		conn->default_tx_octets = _radio.default_tx_octets;
		conn->max_tx_octets = RADIO_LL_LENGTH_OCTETS_RX_MIN;
		conn->max_rx_octets = RADIO_LL_LENGTH_OCTETS_RX_MIN;
		conn->role.slave.role = 1;
		conn->role.slave.latency_cancel = 0;
		conn->role.slave.window_widening_prepare_us = 0;
		conn->role.slave.window_widening_event_us = 0;
		conn->role.slave.ticks_to_offset = 0;
		conn->supervision_expire = 6;
		conn->procedure_expire = 0;
		conn->apto_expire = 0;
		conn->appto_expire = 0;
		conn->llcp_req = 0;
		conn->llcp_ack = 0;
		conn->llcp_version.tx = 0;
		conn->llcp_version.rx = 0;
		conn->llcp_terminate.req = 0;
		conn->llcp_terminate.ack = 0;
		conn->llcp_terminate.reason_peer = 0;
		conn->llcp_terminate.radio_pdu_node_rx.hdr.onion.link = link;
		conn->llcp_length.req = 0;
		conn->llcp_length.ack = 0;
		conn->sn = 0;
		conn->nesn = 0;
		conn->pause_rx = 0;
		conn->pause_tx = 0;
		conn->enc_rx = 0;
		conn->enc_tx = 0;
		conn->refresh = 0;
		conn->empty = 0;
		conn->pkt_tx_head = NULL;
		conn->pkt_tx_ctrl = NULL;
		conn->pkt_tx_data = NULL;
		conn->pkt_tx_last = NULL;
		conn->packet_tx_head_len = 0;
		conn->packet_tx_head_offset = 0;
		conn->rssi_latest = 0x7F;
		conn->rssi_reported = 0x7F;
		conn->rssi_sample_count = 0;

		_radio.advertiser.conn = conn;
	} else {
		conn = NULL;
	}

	_radio.advertiser.chl_map = chl_map;
	_radio.advertiser.filter_policy = filter_policy;
	if (filter_policy) {
		_radio.advertiser.filter_addr_type_bitmask =
			_radio.filter_addr_type_bitmask;
		memcpy(&_radio.advertiser.filter_bdaddr[0][0],
		       &_radio.filter_bdaddr[0][0],
		       sizeof(_radio.advertiser.filter_bdaddr));
		_radio.advertiser.filter_enable_bitmask =
			_radio.filter_enable_bitmask;
	}

	_radio.advertiser.hdr.ticks_active_to_start =
		_radio.ticks_active_to_start;
	_radio.advertiser.hdr.ticks_xtal_to_start =
		TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US);
	_radio.advertiser.hdr.ticks_preempt_to_start =
		TICKER_US_TO_TICKS(RADIO_TICKER_PREEMPT_PART_MIN_US);
	_radio.advertiser.hdr.ticks_slot =
		TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US +
		/* Max. chain is ADV_IND + SCAN_REQ + SCAN_RESP */
		((376 + 150 + 176 + 150 + 376) * 3));

	ticks_slot_offset =
		(_radio.advertiser.hdr.ticks_active_to_start <
		 _radio.advertiser.hdr.ticks_xtal_to_start) ?
		_radio.advertiser.hdr.ticks_xtal_to_start :
		_radio.advertiser.hdr.ticks_active_to_start;

	if (pdu_adv->type == PDU_ADV_TYPE_DIRECT_IND) {
		uint32_t ticks_now = ticker_ticks_now_get();

		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_APP,
				     RADIO_TICKER_ID_ADV, ticks_now, 0,
				     (ticks_slot_offset +
				      _radio.advertiser.hdr.ticks_slot),
				     TICKER_NULL_REMAINDER, TICKER_NULL_LAZY,
				     (ticks_slot_offset +
				      _radio.advertiser.hdr.ticks_slot),
				     radio_event_adv_prepare, 0,
				     ticker_if_done, (void *)&ticker_status);

		/** @todo design to avoid this wait */
		while (ticker_status == TICKER_STATUS_BUSY) {
			cpu_sleep();
		}

		if (ticker_status != TICKER_STATUS_SUCCESS) {
			goto failure_cleanup;
		}

		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_APP,
				     RADIO_TICKER_ID_ADV_STOP, ticks_now,
				     TICKER_US_TO_TICKS((uint64_t) (1280 * 1000) +
							RADIO_TICKER_XTAL_OFFSET_US),
				     TICKER_NULL_PERIOD, TICKER_NULL_REMAINDER,
				     TICKER_NULL_LAZY, TICKER_NULL_SLOT,
				     event_adv_stop, 0, ticker_if_done,
				     (void *)&ticker_status);
	} else {
		ticker_status =
			ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
				     RADIO_TICKER_USER_ID_APP,
				     RADIO_TICKER_ID_ADV,
				     ticker_ticks_now_get(), 0,
				     TICKER_US_TO_TICKS((uint64_t) interval * 625),
				     TICKER_NULL_REMAINDER, TICKER_NULL_LAZY,
				     (ticks_slot_offset +
				      _radio.advertiser.hdr.ticks_slot),
				     radio_event_adv_prepare, 0, ticker_if_done,
				     (void *)&ticker_status);
	}

	/** @todo design to avoid this wait */
	while (ticker_status == TICKER_STATUS_BUSY) {
		cpu_sleep();
	}

	if (ticker_status == TICKER_STATUS_SUCCESS) {
		return 0;
	}

failure_cleanup:

	if (conn) {
		mem_release(conn->llcp_terminate.radio_pdu_node_rx.hdr.
			    onion.link, &_radio.link_rx_free);
		mem_release(conn, &_radio.conn_free);
	}

	return 1;
}

uint32_t radio_adv_disable(void)
{
	uint32_t status;

	status = role_disable(RADIO_TICKER_ID_ADV,
			      RADIO_TICKER_ID_ADV_STOP);
	if (!status) {
		struct connection *conn;

		conn = _radio.advertiser.conn;
		if (conn) {
			_radio.advertiser.conn = NULL;

			mem_release(conn->llcp_terminate.radio_pdu_node_rx.hdr.onion.link,
				    &_radio.link_rx_free);
			mem_release(conn, &_radio.conn_free);
		}
	}

	return status;
}

uint32_t radio_scan_enable(uint8_t scan_type, uint8_t init_addr_type,
			   uint8_t *init_addr, uint16_t interval,
			   uint16_t window, uint8_t filter_policy)
{
	uint32_t volatile ticker_status;
	uint32_t ticks_anchor;
	uint32_t us_offset;
	uint32_t ticks_interval;
	uint32_t ticks_slot_offset;

	_radio.observer.scan_type = scan_type;
	_radio.observer.init_addr_type = init_addr_type;
	memcpy(&_radio.observer.init_addr[0], init_addr, BDADDR_SIZE);
	_radio.observer.ticks_window =
		TICKER_US_TO_TICKS((uint64_t) window * 625);
	_radio.observer.filter_policy = filter_policy;
	if (filter_policy) {
		_radio.observer.filter_addr_type_bitmask =
			_radio.filter_addr_type_bitmask;
		memcpy(&_radio.observer.filter_bdaddr[0][0],
		       &_radio.filter_bdaddr[0][0],
		       sizeof(_radio.advertiser.filter_bdaddr));
		_radio.observer.filter_enable_bitmask =
			_radio.filter_enable_bitmask;
	}

	_radio.observer.hdr.ticks_active_to_start =
		_radio.ticks_active_to_start;
	_radio.observer.hdr.ticks_xtal_to_start =
		TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US);
	_radio.observer.hdr.ticks_preempt_to_start =
		TICKER_US_TO_TICKS(RADIO_TICKER_PREEMPT_PART_MIN_US);
	_radio.observer.hdr.ticks_slot = _radio.observer.ticks_window;

	ticks_interval = TICKER_US_TO_TICKS((uint64_t) interval * 625);
	if (_radio.observer.hdr.ticks_slot >
	    (ticks_interval -
	     TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US))) {
		_radio.observer.hdr.ticks_slot =
			(ticks_interval -
			 TICKER_US_TO_TICKS(RADIO_TICKER_XTAL_OFFSET_US));
	}

	ticks_slot_offset =
		(_radio.observer.hdr.ticks_active_to_start <
		 _radio.observer.hdr.ticks_xtal_to_start) ?
		_radio.observer.hdr.ticks_xtal_to_start :
		_radio.observer.hdr.ticks_active_to_start;

	ticks_anchor = ticker_ticks_now_get();

	if ((_radio.observer.conn) || !SCHED_ADVANCED) {
		us_offset = 0;
	}
#if SCHED_ADVANCED
	else {
		sched_after_master_free_slot_get(RADIO_TICKER_USER_ID_APP,
						 (ticks_slot_offset +
						  _radio.observer.hdr.ticks_slot),
						 &ticks_anchor, &us_offset);
	}
#endif

	ticker_status =
		ticker_start(RADIO_TICKER_INSTANCE_ID_RADIO,
			     RADIO_TICKER_USER_ID_APP, RADIO_TICKER_ID_OBS,
			     (ticks_anchor + TICKER_US_TO_TICKS(us_offset)), 0,
			     ticks_interval,
			     TICKER_REMAINDER((uint64_t) interval * 625),
			     TICKER_NULL_LAZY,
			     (ticks_slot_offset +
			      _radio.observer.hdr.ticks_slot),
			     event_obs_prepare, 0, ticker_if_done,
			     (void *)&ticker_status);

	/** @todo design to avoid this wait */
	while (ticker_status == TICKER_STATUS_BUSY) {
		cpu_sleep();
	}

	return ((ticker_status == TICKER_STATUS_SUCCESS) ? 0 : 1);
}

uint32_t radio_scan_disable(void)
{
	uint32_t status;

	status = role_disable(RADIO_TICKER_ID_OBS,
			      RADIO_TICKER_ID_OBS_STOP);
	if (!status) {
		struct connection *conn;

		conn = _radio.observer.conn;
		if (conn) {
			_radio.observer.conn = NULL;

			mem_release(conn->llcp_terminate.
				    radio_pdu_node_rx.hdr.onion.link,
				    &_radio.link_rx_free);
			mem_release(conn, &_radio.conn_free);
		}
	}

	return status;
}

uint32_t radio_connect_enable(uint8_t adv_addr_type, uint8_t *adv_addr,
			      uint16_t interval, uint16_t latency,
			      uint16_t timeout)
{
	void *link;
	struct connection *conn;
	uint32_t access_addr;
	uint32_t conn_interval_us;

	if (_radio.observer.conn) {
		return 1;
	}

	link = mem_acquire(&_radio.link_rx_free);
	if (!link) {
		return 1;
	}

	conn = mem_acquire(&_radio.conn_free);
	if (!conn) {
		mem_release(link, &_radio.link_rx_free);

		return 1;
	}

	radio_scan_disable();

	_radio.observer.adv_addr_type = adv_addr_type;
	memcpy(&_radio.observer.adv_addr[0], adv_addr, BDADDR_SIZE);
	_radio.observer.conn_interval = interval;
	_radio.observer.conn_latency = latency;
	_radio.observer.conn_timeout = timeout;
	_radio.observer.ticks_conn_slot =
		TICKER_US_TO_TICKS(RADIO_TICKER_START_PART_US +
				   RADIO_TX_READY_DELAY_US + 328 + 328 + 150);

	conn->handle = 0xFFFF;
	conn->llcp_features = RADIO_BLE_FEATURES;
	access_addr = access_addr_get();
	memcpy(&conn->access_addr[0], &access_addr, sizeof(conn->access_addr));
	memcpy(&conn->crc_init[0], &conn, 3);
	memcpy(&conn->data_channel_map[0], &_radio.data_channel_map[0],
	       sizeof(conn->data_channel_map));
	conn->data_channel_count = _radio.data_channel_count;
	conn->data_channel_hop = 6;
	conn->data_channel_use = 0;
	conn->event_counter = 0;
	conn->conn_interval = _radio.observer.conn_interval;
	conn->latency_prepare = 0;
	conn->latency_event = 0;
	conn->latency = _radio.observer.conn_latency;
	conn->default_tx_octets = _radio.default_tx_octets;
	conn->max_tx_octets = RADIO_LL_LENGTH_OCTETS_RX_MIN;
	conn->max_rx_octets = RADIO_LL_LENGTH_OCTETS_RX_MIN;
	conn->role.master.role = 0;
	conn->role.master.connect_expire = 6;
	conn_interval_us =
		(uint32_t)_radio.observer.conn_interval * 1250;
	conn->supervision_reload =
		RADIO_CONN_EVENTS((_radio.observer.conn_timeout * 10 * 1000),
				  conn_interval_us);
	conn->supervision_expire = 0;
	conn->procedure_reload =
		RADIO_CONN_EVENTS((40 * 1000 * 1000), conn_interval_us);
	conn->procedure_expire = 0;
	conn->apto_reload =
		RADIO_CONN_EVENTS((30 * 1000 * 1000), conn_interval_us);
	conn->apto_expire = 0;
	conn->appto_reload = (conn->apto_reload > (conn->latency + 2)) ?
		(conn->apto_reload - (conn->latency + 2)) : conn->apto_reload;
	conn->appto_expire = 0;
	conn->llcp_req = 0;
	conn->llcp_ack = 0;
	conn->llcp_version.tx = 0;
	conn->llcp_version.rx = 0;
	conn->llcp_terminate.req = 0;
	conn->llcp_terminate.ack = 0;
	conn->llcp_terminate.reason_peer = 0;
	conn->llcp_terminate.radio_pdu_node_rx.hdr.onion.link = link;
	conn->llcp_length.req = 0;
	conn->llcp_length.ack = 0;
	conn->sn = 0;
	conn->nesn = 0;
	conn->pause_rx = 0;
	conn->pause_tx = 0;
	conn->enc_rx = 0;
	conn->enc_tx = 0;
	conn->refresh = 0;
	conn->empty = 0;
	conn->pkt_tx_head = NULL;
	conn->pkt_tx_ctrl = NULL;
	conn->pkt_tx_data = NULL;
	conn->pkt_tx_last = NULL;
	conn->packet_tx_head_len = 0;
	conn->packet_tx_head_offset = 0;
	conn->rssi_latest = 0x7F;
	conn->rssi_reported = 0x7F;
	conn->rssi_sample_count = 0;

	_radio.observer.conn = conn;

	return 0;
}

uint32_t radio_connect_disable(void)
{
	uint32_t status;

	if (_radio.observer.conn == 0) {
		return 1;
	}

	status = radio_scan_disable();

	return status;
}

uint32_t radio_conn_update(uint16_t handle, uint8_t cmd, uint8_t status,
			   uint16_t interval, uint16_t latency,
			   uint16_t timeout)
{
	struct connection *conn;

	ARG_UNUSED(status);

	conn = connection_get(handle);
	if ((!conn) ||
	    ((conn->llcp_req != conn->llcp_ack) &&
	     ((conn->llcp_type != LLCP_CONNECTION_UPDATE) ||
	      (conn->llcp.connection_update.state !=
	       LLCP_CONN_STATE_APP_WAIT)))) {
		if ((conn) && (conn->llcp_type == LLCP_CONNECTION_UPDATE)) {
			/* controller busy (mockup requirement) */
			return 2;
		}

		return 1;
	}

	conn->llcp.connection_update.win_size = 1;
	conn->llcp.connection_update.win_offset_us = 0;
	conn->llcp.connection_update.interval = interval;
	conn->llcp.connection_update.latency = latency;
	conn->llcp.connection_update.timeout = timeout;
	/* conn->llcp.connection_update.instant     = 0; */
	conn->llcp.connection_update.state = cmd + 1;
	conn->llcp.connection_update.is_internal = 0;

	conn->llcp_type = LLCP_CONNECTION_UPDATE;
	conn->llcp_req++;

	return 0;
}

uint32_t radio_chm_update(uint8_t *chm)
{
	uint8_t instance;

	memcpy(&_radio.data_channel_map[0], chm,
	       sizeof(_radio.data_channel_map));
	_radio.data_channel_count =
		util_ones_count_get(&_radio.data_channel_map[0],
				    sizeof(_radio.data_channel_map));

	instance = _radio.connection_count;
	while (instance--) {
		struct connection *conn;

		conn = connection_get(instance);
		if (!conn || conn->role.slave.role) {
			continue;
		}

		if (conn->llcp_req != conn->llcp_ack) {
			return 1;
		}

		memcpy(&conn->llcp.channel_map.chm[0], chm,
		       sizeof(conn->llcp.channel_map.chm));
		/* conn->llcp.channel_map.instant     = 0; */
		conn->llcp.channel_map.initiate = 1;

		conn->llcp_type = LLCP_CHANNEL_MAP;
		conn->llcp_req++;
	}

	return 0;
}

uint32_t radio_chm_get(uint16_t handle, uint8_t *chm)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn) {
		return 1;
	}

	/** @todo make reading context-safe */
	memcpy(chm, conn->data_channel_map, sizeof(conn->data_channel_map));

	return 0;
}

uint32_t radio_enc_req_send(uint16_t handle, uint8_t *rand, uint8_t *ediv,
			    uint8_t *ltk)
{
	struct connection *conn;
	struct radio_pdu_node_tx *node_tx;

	conn = connection_get(handle);
	if (!conn) {
		return 1;
	}

	node_tx = radio_tx_mem_acquire();
	if (node_tx) {
		struct pdu_data *pdu_data_tx;

		pdu_data_tx = (struct pdu_data *)node_tx->pdu_data;

		memcpy(&conn->llcp.encryption.ltk[0], ltk,
		       sizeof(conn->llcp.encryption.ltk));

		if ((conn->enc_rx == 0) && (conn->enc_tx == 0)) {
			pdu_data_tx->ll_id = PDU_DATA_LLID_CTRL;
			pdu_data_tx->len = offsetof(struct pdu_data_llctrl,
						    ctrldata) +
				sizeof(struct pdu_data_llctrl_enc_req);
			pdu_data_tx->payload.llctrl.opcode =
				PDU_DATA_LLCTRL_TYPE_ENC_REQ;
			memcpy(&pdu_data_tx->payload.llctrl.ctrldata.
			       enc_req.rand[0], rand,
			       sizeof(pdu_data_tx->payload.llctrl.ctrldata.
				      enc_req.rand));
			pdu_data_tx->payload.llctrl.ctrldata.enc_req.ediv[0] =
				ediv[0];
			pdu_data_tx->payload.llctrl.ctrldata.enc_req.ediv[1] =
				ediv[1];
			memset(&pdu_data_tx->payload.llctrl.ctrldata.enc_req.
				skdm[0], 0xcc,
				/** @todo */
				sizeof(pdu_data_tx->payload.llctrl.ctrldata.
				       enc_req.skdm));
			memset(&pdu_data_tx->payload.llctrl.ctrldata.enc_req.
				ivm[0], 0xdd,
				/** @todo */
				sizeof(pdu_data_tx->payload.llctrl.ctrldata.
				       enc_req.ivm));
		} else if ((conn->enc_rx != 0) && (conn->enc_tx != 0)) {
			memcpy(&conn->llcp.encryption.rand[0], rand,
			       sizeof(conn->llcp.encryption.rand));

			conn->llcp.encryption.ediv[0] = ediv[0];
			conn->llcp.encryption.ediv[1] = ediv[1];

			pdu_data_tx->ll_id = PDU_DATA_LLID_CTRL;
			pdu_data_tx->len =
				offsetof(struct pdu_data_llctrl, ctrldata);
			pdu_data_tx->payload.llctrl.opcode =
			    PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_REQ;
		} else {
			radio_tx_mem_release(node_tx);

			return 1;
		}

		if (radio_tx_mem_enqueue(handle, node_tx)) {
			radio_tx_mem_release(node_tx);

			return 1;
		}

		return 0;
	}

	return 1;
}

uint32_t radio_start_enc_req_send(uint16_t handle, uint8_t error_code,
				  uint8_t const *const ltk)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn) {
		return 1;
	}

	if (error_code) {
		if (conn->refresh == 0) {
			if (conn->llcp_req != conn->llcp_ack) {
				return 1;
			}

			conn->llcp.encryption.error_code = error_code;

			conn->llcp_type = LLCP_ENCRYPTION;
			conn->llcp_req++;
		} else {
			if (conn->llcp_terminate.ack !=
			    conn->llcp_terminate.req) {
				return 1;
			}

			conn->llcp_terminate.reason_own = error_code;

			conn->llcp_terminate.req++;
		}
	} else {
		memcpy(&conn->llcp.encryption.ltk[0], ltk,
		       sizeof(conn->llcp.encryption.ltk));

		if (conn->llcp_req != conn->llcp_ack) {
			return 1;
		}

		conn->llcp.encryption.error_code = 0;

		conn->llcp_type = LLCP_ENCRYPTION;
		conn->llcp_req++;
	}

	return 0;
}

uint32_t radio_feature_req_send(uint16_t handle)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn || (conn->llcp_req != conn->llcp_ack)) {
		return 1;
	}

	conn->llcp_type = LLCP_FEATURE_EXCHANGE;
	conn->llcp_req++;

	return 0;
}

uint32_t radio_version_ind_send(uint16_t handle)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn || (conn->llcp_req != conn->llcp_ack)) {
		return 1;
	}

	conn->llcp_type = LLCP_VERSION_EXCHANGE;
	conn->llcp_req++;

	return 0;
}

uint32_t radio_terminate_ind_send(uint16_t handle, uint8_t reason)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn || (conn->llcp_terminate.ack != conn->llcp_terminate.req)) {
		return 1;
	}

	conn->llcp_terminate.reason_own = reason;

	conn->llcp_terminate.req++;

	return 0;
}

uint32_t radio_length_req_send(uint16_t handle, uint16_t tx_octets)
{
	struct connection *conn;

	conn = connection_get(handle);
	if (!conn || (conn->llcp_req != conn->llcp_ack) ||
	    (conn->llcp_length.req != conn->llcp_length.ack)) {
		return 1;
	}

	conn->llcp_length.state = LLCP_LENGTH_STATE_REQ;
	conn->llcp_length.tx_octets = tx_octets;
	conn->llcp_length.req++;

	return 0;
}

void radio_length_default_get(uint16_t *max_tx_octets, uint16_t *max_tx_time)
{
	*max_tx_octets = _radio.default_tx_octets;
	*max_tx_time = _radio.default_tx_time;
}

uint32_t radio_length_default_set(uint16_t max_tx_octets, uint16_t max_tx_time)
{
	if (max_tx_octets > RADIO_LL_LENGTH_OCTETS_RX_MAX ||
	    max_tx_time > RADIO_LL_LENGTH_TIME_RX_MAX) {

		return 1;
	}

	_radio.default_tx_octets = max_tx_octets;
	_radio.default_tx_time = max_tx_time;

	return 0;
}

void radio_length_max_get(uint16_t *max_tx_octets, uint16_t *max_tx_time,
			  uint16_t *max_rx_octets, uint16_t *max_rx_time)
{
	*max_tx_octets = RADIO_LL_LENGTH_OCTETS_RX_MAX;
	*max_tx_time = RADIO_LL_LENGTH_TIME_RX_MAX;
	*max_rx_octets = RADIO_LL_LENGTH_OCTETS_RX_MAX;
	*max_rx_time = RADIO_LL_LENGTH_TIME_RX_MAX;
}

static uint8_t tx_cmplt_get(uint16_t *handle, uint8_t *first, uint8_t last)
{
	uint8_t _first;
	uint8_t cmplt;

	_first = *first;
	if (_first == last) {
		return 0;
	}

	cmplt = 0;
	*handle = _radio.pkt_release[_first].handle;
	do {
		struct radio_pdu_node_tx *node_tx;
		struct pdu_data *pdu_data_tx;

		if (*handle != _radio.pkt_release[_first].handle) {
			break;
		}

		node_tx = _radio.pkt_release[_first].node_tx;
		/*@FIXME: assign before first 3 if conditions */
		pdu_data_tx = (struct pdu_data *)node_tx->pdu_data;
		if ((!node_tx) || (node_tx == (struct radio_pdu_node_tx *)1) ||
		    ((((uint32_t)node_tx & ~(0x00000003)) != 0) &&
		     (pdu_data_tx) && (pdu_data_tx->len != 0) &&
		     ((pdu_data_tx->ll_id == PDU_DATA_LLID_DATA_START) ||
		      (pdu_data_tx->ll_id == PDU_DATA_LLID_DATA_CONTINUE)))) {

			/* data packet, hence count num cmplt */
			_radio.pkt_release[_first].node_tx =
				(struct radio_pdu_node_tx *)1;

			cmplt++;
		} else {
			/* ctrl packet, hence not num cmplt */
			_radio.pkt_release[_first].node_tx =
				(struct radio_pdu_node_tx *)2;
		}

		if (((uint32_t)node_tx & ~(0x00000003)) != 0) {
			mem_release(node_tx, &_radio.pkt_tx_data_free);
		}

		_first = _first + 1;
		if (_first == _radio.packet_tx_count) {
			_first = 0;
		}

	} while (_first != last);

	*first = _first;

	return cmplt;
}

uint8_t radio_rx_get(struct radio_pdu_node_rx **radio_pdu_node_rx,
		     uint16_t *handle)
{
	uint8_t cmplt;

	cmplt = 0;
	if (_radio.link_rx_head != _radio.link_rx_tail) {
		struct radio_pdu_node_rx *_radio_pdu_node_rx;

		_radio_pdu_node_rx = *((void **)_radio.link_rx_head + 1);

		cmplt = tx_cmplt_get(handle,
				&_radio.packet_release_first,
				_radio_pdu_node_rx->hdr.onion.
				packet_release_last);
		if (!cmplt) {
			uint16_t handle;
			uint8_t first, cmplt_prev, cmplt_curr;

			first = _radio.packet_release_first;
			cmplt_curr = 0;
			do {
				cmplt_prev = cmplt_curr;
				cmplt_curr = tx_cmplt_get(&handle, &first,
						_radio.packet_release_last);
			} while ((cmplt_prev != 0) ||
				 (cmplt_prev != cmplt_curr));

			*radio_pdu_node_rx = _radio_pdu_node_rx;
		} else {
			*radio_pdu_node_rx = NULL;
		}
	} else {
		cmplt = tx_cmplt_get(handle, &_radio.packet_release_first,
				     _radio.packet_release_last);

		*radio_pdu_node_rx = NULL;
	}

	return cmplt;
}

void radio_rx_dequeue(void)
{
	struct radio_pdu_node_rx *radio_pdu_node_rx = NULL;
	void *link;

	link = memq_dequeue(_radio.link_rx_tail, &_radio.link_rx_head,
			    (void **)&radio_pdu_node_rx);
	LL_ASSERT(link);

	mem_release(link, &_radio.link_rx_free);

	switch (radio_pdu_node_rx->hdr.type) {
	case NODE_RX_TYPE_DC_PDU:
	case NODE_RX_TYPE_PROFILE:
	case NODE_RX_TYPE_REPORT:
	case NODE_RX_TYPE_CONNECTION:
	case NODE_RX_TYPE_CONN_UPDATE:
	case NODE_RX_TYPE_ENC_REFRESH:
	case NODE_RX_TYPE_APTO:
	case NODE_RX_TYPE_RSSI:
		/* release data link credit quota */
		LL_ASSERT(_radio.link_rx_data_quota <
			  (_radio.packet_rx_count - 1));

		_radio.link_rx_data_quota++;
		break;

	case NODE_RX_TYPE_TERMINATE:
		/* did not use data link quota */
		break;

	default:
		LL_ASSERT(0);
		break;
	}
}

void radio_rx_mem_release(struct radio_pdu_node_rx **radio_pdu_node_rx)
{
	struct radio_pdu_node_rx *_radio_pdu_node_rx;
	struct connection *conn;

	_radio_pdu_node_rx = *radio_pdu_node_rx;
	while (_radio_pdu_node_rx) {
		struct radio_pdu_node_rx *_radio_pdu_node_rx_free;

		_radio_pdu_node_rx_free = _radio_pdu_node_rx;
		_radio_pdu_node_rx = _radio_pdu_node_rx->hdr.onion.next;

		switch (_radio_pdu_node_rx_free->hdr.type) {
		case NODE_RX_TYPE_DC_PDU:
		case NODE_RX_TYPE_PROFILE:
		case NODE_RX_TYPE_REPORT:
		case NODE_RX_TYPE_CONNECTION:
		case NODE_RX_TYPE_CONN_UPDATE:
		case NODE_RX_TYPE_ENC_REFRESH:
		case NODE_RX_TYPE_APTO:
		case NODE_RX_TYPE_RSSI:
			mem_release(_radio_pdu_node_rx_free,
				    &_radio.pkt_rx_data_free);
			break;

		case NODE_RX_TYPE_TERMINATE:
			conn = mem_get(_radio.conn_pool, CONNECTION_T_SIZE,
				       _radio_pdu_node_rx_free->hdr.handle);

			mem_release(conn, &_radio.conn_free);
			break;

		default:
			LL_ASSERT(0);
			break;
		}
	}

	*radio_pdu_node_rx = _radio_pdu_node_rx;

	packet_rx_allocate(0xff);
}

static void rx_fc_lock(uint16_t handle)
{
	if (_radio.fc_req == _radio.fc_ack) {
		uint8_t req;

		_radio.fc_handle[_radio.fc_req] = handle;
		req = _radio.fc_req + 1;
		if (req == TRIPLE_BUFFER_SIZE) {
			req = 0;
		}
		_radio.fc_req = req;
	}
}

uint8_t do_radio_rx_fc_set(uint16_t handle, uint8_t req,
				uint8_t ack)
{
	if (req == ack) {
		if (_radio.link_rx_head == _radio.link_rx_tail) {
			uint8_t ack1 = ack;

			if (ack1 == 0) {
				ack1 = TRIPLE_BUFFER_SIZE;
			}
			_radio.fc_handle[--ack1] = handle;
			_radio.fc_ack = ack1;

			/* check if ISR updated FC by changing fc_req */
			if (req != _radio.fc_req) {
				_radio.fc_ack = ack;

				return 1;
			}
		} else {
			return 1;
		}
	} else if (((req == 0) &&
		    (_radio.fc_handle[TRIPLE_BUFFER_SIZE - 1] != handle)) ||
		   ((req != 0) && (_radio.fc_handle[req - 1] != handle))) {
		return 1;
	}

	return 0;
}

uint8_t radio_rx_fc_set(uint16_t handle, uint8_t fc)
{
	if (_radio.fc_ena) {
		uint8_t req = _radio.fc_req;
		uint8_t ack = _radio.fc_ack;

		if (fc) {
			if (handle != 0xffff) {
				return do_radio_rx_fc_set(handle, req, ack);
			}
		} else if ((_radio.link_rx_head == _radio.link_rx_tail) &&
			   (req != ack)
		    ) {
			_radio.fc_ack = req;

			if ((_radio.link_rx_head != _radio.link_rx_tail) &&
			    (req == _radio.fc_req)) {
				_radio.fc_ack = ack;
			}
		}
	}

	return 0;
}

uint8_t radio_rx_fc_get(uint16_t *handle)
{
	uint8_t req = _radio.fc_req;
	uint8_t ack = _radio.fc_ack;

	if (req != ack) {
		if (handle) {
			*handle = _radio.fc_handle[ack];
		}
		return 1;
	}

	return 0;
}

struct radio_pdu_node_tx *radio_tx_mem_acquire(void)
{
	return mem_acquire(&_radio.pkt_tx_data_free);
}

void radio_tx_mem_release(struct radio_pdu_node_tx *node_tx)
{
	mem_release(node_tx, &_radio.pkt_tx_data_free);
}

static void ticker_op_latency_cancelled(uint32_t ticker_status,
					   void *params)
{
	struct connection *conn;

	LL_ASSERT(ticker_status == TICKER_STATUS_SUCCESS);

	conn = (struct connection *)params;
	conn->role.slave.latency_cancel = 0;
}

uint32_t radio_tx_mem_enqueue(uint16_t handle,
			      struct radio_pdu_node_tx *node_tx)
{
	uint8_t last;
	struct connection *conn;
	struct pdu_data *pdu_data;

	last = _radio.packet_tx_last + 1;
	if (last == _radio.packet_tx_count) {
		last = 0;
	}

	pdu_data = (struct pdu_data *)node_tx->pdu_data;
	conn = connection_get(handle);
	if ((last == _radio.packet_tx_first) || (conn == 0) ||
	    (pdu_data->len > _radio.packet_data_octets_max)) {
		return 1;
	}

	_radio.pkt_tx[_radio.packet_tx_last].handle = handle;
	_radio.pkt_tx[_radio.packet_tx_last].  node_tx = node_tx;
	_radio.packet_tx_last = last;

	/* break slave latency */
	if ((conn->role.slave.role != 0) && (conn->latency_event != 0) &&
	    (conn->role.slave.latency_cancel == 0)) {
		uint32_t ticker_status;

		conn->role.slave.latency_cancel = 1;

		ticker_status = ticker_update(RADIO_TICKER_INSTANCE_ID_RADIO,
				 RADIO_TICKER_USER_ID_APP,
				 RADIO_TICKER_ID_FIRST_CONNECTION +
				 conn->handle, 0, 0, 0, 0, 1, 0,
				 ticker_op_latency_cancelled,
				 (void *)conn);
		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}

	return 0;
}
