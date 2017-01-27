/* pinmux_dev_qmsi.c -  QMSI pinmux dev driver */

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

#include <errno.h>
#include <pinmux.h>

#include "qm_pinmux.h"

#define MASK_2_BITS	0x3

static int pinmux_dev_set(struct device *dev, uint32_t pin,
			  uint32_t func)
{
	ARG_UNUSED(dev);

	return qm_pmux_select(pin, func) == 0 ? 0 : -EIO;
}

static int pinmux_dev_get(struct device *dev, uint32_t pin,
			  uint32_t *func)
{
	ARG_UNUSED(dev);

	/*
	 * pinmux control registers are 32-bit wide, but each pin requires
	 * 2 bits to set the mode (A, B, C, or D).  As such we only get 16
	 * pins per register.
	 */
	uint32_t reg_offset = pin >> 4;

	/* The pin offset within the register */
	uint32_t pin_no = pin % 16;

	/*
	 * Now figure out what is the full address for the register
	 * we are looking for.
	 */
	volatile uint32_t *mux_register = &QM_SCSS_PMUX->pmux_sel[reg_offset];

	/*
	 * MASK_2_BITS (the value of which is 3) is used because there are
	 * 2 bits for the mode of each pin.
	 */
	uint32_t pin_mask = MASK_2_BITS << (pin_no << 1);
	uint32_t mode_mask = *mux_register & pin_mask;
	uint32_t mode = mode_mask >> (pin_no << 1);

	*func = mode;

	return 0;
}

static int pinmux_dev_pullup(struct device *dev, uint32_t pin,
			     uint8_t func)
{
	ARG_UNUSED(dev);

	return qm_pmux_pullup_en(pin, func) == 0 ? 0 : -EIO;
}

static int pinmux_dev_input(struct device *dev, uint32_t pin,
			    uint8_t func)
{
	ARG_UNUSED(dev);

	return qm_pmux_input_en(pin, func) == 0 ? 0 : -EIO;
}

static struct pinmux_driver_api api_funcs = {
	.set = pinmux_dev_set,
	.get = pinmux_dev_get,
	.pullup = pinmux_dev_pullup,
	.input = pinmux_dev_input
};

static int pinmux_dev_initialize(struct device *port)
{
	return 0;
}

DEVICE_AND_API_INIT(pmux_dev, CONFIG_PINMUX_DEV_NAME,
		    &pinmux_dev_initialize, NULL, NULL,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		    &api_funcs);
