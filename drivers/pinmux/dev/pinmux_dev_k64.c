/* pinmux_dev_k64.c - Pinmux dev driver for Freescale K64 */

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

#include <device.h>
#include <init.h>
#include <kernel.h>
#include <pinmux.h>
#include <soc.h>
#include <sys_io.h>
#include <pinmux/k64/pinmux.h>

static int fsl_k64_dev_set(struct device *dev, uint32_t pin,
				uint32_t func)
{
	ARG_UNUSED(dev);

	return _fsl_k64_set_pin(pin, func);
}

static int fsl_k64_dev_get(struct device *dev, uint32_t pin,
				uint32_t *func)
{
	ARG_UNUSED(dev);

	return _fsl_k64_get_pin(pin, func);
}

static const struct pinmux_driver_api api_funcs = {
	.set = fsl_k64_dev_set,
	.get = fsl_k64_dev_get
};

int pinmux_fsl_k64_initialize(struct device *port)
{
	ARG_UNUSED(port);

	return 0;
}

/* must be initialized after GPIO */
DEVICE_AND_API_INIT(pmux, CONFIG_PINMUX_DEV_NAME, &pinmux_fsl_k64_initialize,
		    NULL, NULL,
		    POST_KERNEL, CONFIG_PINMUX_INIT_PRIORITY,
		    &api_funcs);
