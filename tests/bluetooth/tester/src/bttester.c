/* bttester.c - Bluetooth Tester */

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

#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <toolchain.h>
#include <bluetooth/bluetooth.h>
#include <misc/byteorder.h>
#include <console/uart_pipe.h>

#include "bttester.h"

#define STACKSIZE 2048
static char __stack stack[STACKSIZE];

#define CMD_QUEUED 2
static uint8_t cmd_buf[CMD_QUEUED * BTP_MTU];

static struct k_fifo cmds_queue;
static struct k_fifo avail_queue;

static void supported_commands(uint8_t *data, uint16_t len)
{
	uint8_t buf[1];
	struct core_read_supported_commands_rp *rp = (void *) buf;

	memset(buf, 0, sizeof(buf));

	tester_set_bit(buf, CORE_READ_SUPPORTED_COMMANDS);
	tester_set_bit(buf, CORE_READ_SUPPORTED_SERVICES);
	tester_set_bit(buf, CORE_REGISTER_SERVICE);

	tester_send(BTP_SERVICE_ID_CORE, CORE_READ_SUPPORTED_COMMANDS,
		    BTP_INDEX_NONE, (uint8_t *) rp, sizeof(buf));
}

static void supported_services(uint8_t *data, uint16_t len)
{
	uint8_t buf[1];
	struct core_read_supported_services_rp *rp = (void *) buf;

	memset(buf, 0, sizeof(buf));

	tester_set_bit(buf, BTP_SERVICE_ID_CORE);
	tester_set_bit(buf, BTP_SERVICE_ID_GAP);
	tester_set_bit(buf, BTP_SERVICE_ID_GATT);
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	tester_set_bit(buf, BTP_SERVICE_ID_L2CAP);
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */

	tester_send(BTP_SERVICE_ID_CORE, CORE_READ_SUPPORTED_SERVICES,
		    BTP_INDEX_NONE, (uint8_t *) rp, sizeof(buf));
}

static void register_service(uint8_t *data, uint16_t len)
{
	struct core_register_service_cmd *cmd = (void *) data;
	uint8_t status;

	switch (cmd->id) {
	case BTP_SERVICE_ID_GAP:
		status = tester_init_gap();
		/* Rsp with success status will be handled by bt enable cb */
		if (status == BTP_STATUS_FAILED) {
			goto rsp;
		}
		return;
	case BTP_SERVICE_ID_GATT:
		status = tester_init_gatt();
		break;
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
	case BTP_SERVICE_ID_L2CAP:
		status = tester_init_l2cap();
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */
		break;
	default:
		status = BTP_STATUS_FAILED;
		break;
	}

rsp:
	tester_rsp(BTP_SERVICE_ID_CORE, CORE_REGISTER_SERVICE, BTP_INDEX_NONE,
		   status);
}

static void handle_core(uint8_t opcode, uint8_t index, uint8_t *data,
			uint16_t len)
{
	if (index != BTP_INDEX_NONE) {
		tester_rsp(BTP_SERVICE_ID_CORE, opcode, index, BTP_STATUS_FAILED);
		return;
	}

	switch (opcode) {
	case CORE_READ_SUPPORTED_COMMANDS:
		supported_commands(data, len);
		return;
	case CORE_READ_SUPPORTED_SERVICES:
		supported_services(data, len);
		return;
	case CORE_REGISTER_SERVICE:
		register_service(data, len);
		return;
	default:
		tester_rsp(BTP_SERVICE_ID_CORE, opcode, BTP_INDEX_NONE,
			   BTP_STATUS_UNKNOWN_CMD);
		return;
	}
}

static void cmd_handler(void *p1, void *p2, void *p3)
{
	while (1) {
		struct btp_hdr *cmd;
		uint16_t len;

		cmd = k_fifo_get(&cmds_queue, K_FOREVER);

		len = sys_le16_to_cpu(cmd->len);

		/* TODO
		 * verify if service is registered before calling handler
		 */

		switch (cmd->service) {
		case BTP_SERVICE_ID_CORE:
			handle_core(cmd->opcode, cmd->index, cmd->data, len);
			break;
		case BTP_SERVICE_ID_GAP:
			tester_handle_gap(cmd->opcode, cmd->index, cmd->data,
					  len);
			break;
		case BTP_SERVICE_ID_GATT:
			tester_handle_gatt(cmd->opcode, cmd->index, cmd->data,
					    len);
			break;
#if defined(CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL)
		case BTP_SERVICE_ID_L2CAP:
			tester_handle_l2cap(cmd->opcode, cmd->index, cmd->data,
					    len);
#endif /* CONFIG_BLUETOOTH_L2CAP_DYNAMIC_CHANNEL */
			break;
		default:
			tester_rsp(cmd->service, cmd->opcode, cmd->index,
				   BTP_STATUS_FAILED);
			break;
		}

		k_fifo_put(&avail_queue, cmd);
	}
}

static uint8_t *recv_cb(uint8_t *buf, size_t *off)
{
	struct btp_hdr *cmd = (void *) buf;
	uint8_t *new_buf;
	uint16_t len;

	if (*off < sizeof(*cmd)) {
		return buf;
	}

	len = sys_le16_to_cpu(cmd->len);
	if (len > BTP_MTU - sizeof(*cmd)) {
		SYS_LOG_ERR("BT tester: invalid packet length");
		*off = 0;
		return buf;
	}

	if (*off < sizeof(*cmd) + len) {
		return buf;
	}

	new_buf =  k_fifo_get(&avail_queue, K_NO_WAIT);
	if (!new_buf) {
		SYS_LOG_ERR("BT tester: RX overflow");
		*off = 0;
		return buf;
	}

	k_fifo_put(&cmds_queue, buf);

	*off = 0;
	return new_buf;
}

void tester_init(void)
{
	int i;

	k_fifo_init(&cmds_queue);
	k_fifo_init(&avail_queue);

	for (i = 0; i < CMD_QUEUED; i++) {
		k_fifo_put(&avail_queue, &cmd_buf[i * BTP_MTU]);
	}

	k_thread_spawn(stack, STACKSIZE, cmd_handler, NULL, NULL, NULL,
		       K_PRIO_COOP(7), 0, K_NO_WAIT);

	uart_pipe_register(k_fifo_get(&avail_queue, K_NO_WAIT),
			   BTP_MTU, recv_cb);

	tester_send(BTP_SERVICE_ID_CORE, CORE_EV_IUT_READY, BTP_INDEX_NONE,
		    NULL, 0);
}

void tester_send(uint8_t service, uint8_t opcode, uint8_t index, uint8_t *data,
		 size_t len)
{
	struct btp_hdr msg;

	msg.service = service;
	msg.opcode = opcode;
	msg.index = index;
	msg.len = len;

	uart_pipe_send((uint8_t *)&msg, sizeof(msg));
	if (data && len) {
		uart_pipe_send(data, len);
	}
}

void tester_rsp(uint8_t service, uint8_t opcode, uint8_t index, uint8_t status)
{
	struct btp_status s;

	if (status == BTP_STATUS_SUCCESS) {
		tester_send(service, opcode, index, NULL, 0);
		return;
	}

	s.code = status;
	tester_send(service, BTP_STATUS, index, (uint8_t *) &s, sizeof(s));
}
