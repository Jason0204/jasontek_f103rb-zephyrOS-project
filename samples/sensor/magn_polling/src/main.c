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

#include <zephyr.h>
#include <device.h>
#include <sensor.h>
#include <misc/printk.h>
#include <stdio.h>

static void do_main(struct device *dev)
{
	int ret;
	struct sensor_value value_x, value_y, value_z;

	while (1) {
		ret = sensor_sample_fetch(dev);
		if (ret) {
			printk("sensor_sample_fetch failed ret %d\n", ret);
			return;
		}

		ret = sensor_channel_get(dev, SENSOR_CHAN_MAGN_X, &value_x);
		ret = sensor_channel_get(dev, SENSOR_CHAN_MAGN_Y, &value_y);
		ret = sensor_channel_get(dev, SENSOR_CHAN_MAGN_Z, &value_z);
		printf("( x y z ) = ( %f  %f  %f )\n", value_x.dval, value_y.dval, value_z.dval);

		k_sleep(500);
	}
}

struct device *sensor_search_for_magnetometer()
{
	static char *magn_sensors[] = {"bmc150_magn", NULL};
	struct device *dev;
	int i;

	i = 0;
	while (magn_sensors[i]) {
		dev = device_get_binding(magn_sensors[i]);
		if (dev) {
			return dev;
		}
		++i;
	}

	return NULL;
}

void main(void)
{
	struct device *dev;

	dev = sensor_search_for_magnetometer();
	if (dev) {
		printk("Found device is %p, name is %s\n", dev, dev->config->name);
		do_main(dev);
	} else {
		printk("There is no available magnetometer device.\n");
	}
}
