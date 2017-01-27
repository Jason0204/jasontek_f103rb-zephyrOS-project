/* ipm_quark_se.c - Quark SE mailbox driver */

/*
 * Copyright (c) 2015 Intel Corporation
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

#include <kernel.h>
#include <stdint.h>
#include <string.h>
#include <device.h>
#include <init.h>
#include <ipm.h>
#include <arch/cpu.h>
#include <misc/printk.h>
#include <misc/__assert.h>
#include <errno.h>
#include "ipm_quark_se.h"


/* We have a single ISR for all channels, so in order to properly handle
 * messages we need to figure out which device object corresponds to
 * in incoming channel
 */
static struct device *device_by_channel[QUARK_SE_IPM_CHANNELS];
static uint32_t inbound_channels;

static uint32_t quark_se_ipm_sts_get(void)
{
	return sys_read32(QUARK_SE_IPM_CHALL_STS) & inbound_channels;
}

static void set_channel_irq_state(int channel, int enable)
{
	mem_addr_t addr = QUARK_SE_IPM_MASK;
	int bit = channel + QUARK_SE_IPM_MASK_START_BIT;

	if (enable) {
		sys_clear_bit(addr, bit);
	} else {
		sys_set_bit(addr, bit);
	}
}


/* Interrupt handler, gets messages on all incoming enabled mailboxes */
void quark_se_ipm_isr(void *param)
{
	int channel;
	int sts, bit;
	struct device *d;
	const struct quark_se_ipm_config_info *config;
	struct quark_se_ipm_driver_data *driver_data;
	volatile struct quark_se_ipm *ipm;
	unsigned int key;

	ARG_UNUSED(param);
	sts = quark_se_ipm_sts_get();

	__ASSERT(sts, "spurious IPM interrupt");
	bit = find_msb_set(sts) - 1;
	channel = bit / 2;
	d = device_by_channel[channel];

	__ASSERT(d, "got IRQ on channel with no IPM device");
	config = d->config->config_info;
	driver_data = d->driver_data;
	ipm = config->ipm;

	__ASSERT(driver_data->callback, "enabled IPM channel with no callback");
	driver_data->callback(driver_data->callback_ctx, ipm->ctrl.ctrl,
			      &ipm->data);

	key = irq_lock();

	ipm->sts.irq = 1; /* Clear the interrupt bit */
	ipm->sts.sts = 1; /* Clear channel status bit */

	/* Wait for the above register writes to clear the channel
	 * to propagate to the global channel status register
	 */
	while (quark_se_ipm_sts_get() & (0x3 << (channel * 2))) {
		/* Busy-wait */
	}
	irq_unlock(key);
}


static int quark_se_ipm_send(struct device *d, int wait, uint32_t id,
			const void *data, int size)
{
	const struct quark_se_ipm_config_info *config = d->config->config_info;
	volatile struct quark_se_ipm *ipm = config->ipm;
	const uint8_t *data8;
	int i;
	int flags;

	if (id > QUARK_SE_IPM_MAX_ID_VAL) {
		return -EINVAL;
	}

	if (config->direction != QUARK_SE_IPM_OUTBOUND) {
		return -EINVAL;
	}

	if (size > QUARK_SE_IPM_DATA_BYTES) {
		return -EMSGSIZE;
	}

	flags = irq_lock();

	if (ipm->sts.sts != 0) {
		irq_unlock(flags);
		return -EBUSY;
	}

	/* Populate the data, memcpy doesn't take volatiles */
	data8 = (const uint8_t *)data;

	for (i = 0; i < size; ++i) {
		ipm->data[i] = data8[i];
	}
	ipm->ctrl.ctrl = id;

	/* Cause the interrupt to assert on the remote side */
	ipm->ctrl.irq = 1;

	/* Wait for HW to set the sts bit */
	while (ipm->sts.sts == 0) {
	}
	irq_unlock(flags);

	if (wait) {
		/* Loop until remote clears the status bit */
		while (ipm->sts.sts != 0) {
		}
	}
	return 0;
}


static int quark_se_ipm_max_data_size_get(struct device *d)
{
	ARG_UNUSED(d);

	return QUARK_SE_IPM_DATA_BYTES;
}


static uint32_t quark_se_ipm_max_id_val_get(struct device *d)
{
	ARG_UNUSED(d);

	return QUARK_SE_IPM_MAX_ID_VAL;
}

static void quark_se_ipm_register_callback(struct device *d, ipm_callback_t cb,
				       void *context)
{
	struct quark_se_ipm_driver_data *driver_data = d->driver_data;

	driver_data->callback = cb;
	driver_data->callback_ctx = context;
}


static int quark_se_ipm_set_enabled(struct device *d, int enable)
{
	const struct quark_se_ipm_config_info *config_info =
		d->config->config_info;

	if (config_info->direction != QUARK_SE_IPM_INBOUND) {
		return -EINVAL;
	}
	set_channel_irq_state(config_info->channel, enable);
	return 0;
}

const struct ipm_driver_api ipm_quark_se_api_funcs = {
	.send = quark_se_ipm_send,
	.register_callback = quark_se_ipm_register_callback,
	.max_data_size_get = quark_se_ipm_max_data_size_get,
	.max_id_val_get = quark_se_ipm_max_id_val_get,
	.set_enabled = quark_se_ipm_set_enabled
};

int quark_se_ipm_controller_initialize(struct device *d)
{
	const struct quark_se_ipm_controller_config_info *config =
		d->config->config_info;
#if CONFIG_IPM_QUARK_SE_MASTER
	int i;

	/* Mask all mailbox interrupts, we'll enable them
	 * individually later. Clear out any pending messages
	 */
	sys_write32(0xFFFFFFFF, QUARK_SE_IPM_MASK);
	for (i = 0; i < QUARK_SE_IPM_CHANNELS; ++i) {
		volatile struct quark_se_ipm *ipm = QUARK_SE_IPM(i);

		ipm->sts.sts = 0;
		ipm->sts.irq = 0;
	}
#endif

	if (config->controller_init) {
		return config->controller_init();
	}
	return 0;
}


int quark_se_ipm_initialize(struct device *d)
{
	const struct quark_se_ipm_config_info *config = d->config->config_info;

	device_by_channel[config->channel] = d;
	if (config->direction == QUARK_SE_IPM_INBOUND) {
		inbound_channels |= (0x3 << (config->channel * 2));
	}

	return 0;
}
