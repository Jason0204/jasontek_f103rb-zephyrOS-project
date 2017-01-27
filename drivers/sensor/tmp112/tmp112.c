/*
 * Copyright (c) 2016 Firmwave
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
#include <i2c.h>
#include <misc/byteorder.h>
#include <misc/util.h>
#include <kernel.h>
#include <sensor.h>
#include <misc/__assert.h>

#define SYS_LOG_DOMAIN "TMP112"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_SENSOR_LEVEL
#include <misc/sys_log.h>

#define TMP112_I2C_ADDRESS		CONFIG_TMP112_I2C_ADDR

#define TMP112_REG_TEMPERATURE		0x00
#define TMP112_D0_BIT			BIT(0)

#define TMP112_REG_CONFIG		0x01
#define TMP112_EM_BIT			BIT(4)
#define TMP112_CR0_BIT			BIT(6)
#define TMP112_CR1_BIT			BIT(7)

/* scale in micro degrees Celsius */
#define TMP112_TEMP_SCALE		62500

struct tmp112_data {
	struct device *i2c;
	int16_t sample;
};

static int tmp112_reg_read(struct tmp112_data *drv_data,
			   uint8_t reg, uint16_t *val)
{
	struct i2c_msg msgs[2] = {
		{
			.buf = &reg,
			.len = 1,
			.flags = I2C_MSG_WRITE | I2C_MSG_RESTART,
		},
		{
			.buf = (uint8_t *)val,
			.len = 2,
			.flags = I2C_MSG_READ | I2C_MSG_STOP,
		},
	};

	if (i2c_transfer(drv_data->i2c, msgs, 2, TMP112_I2C_ADDRESS) < 0) {
		return -EIO;
	}

	*val = sys_be16_to_cpu(*val);

	return 0;
}

static int tmp112_reg_write(struct tmp112_data *drv_data,
			    uint8_t reg, uint16_t val)
{
	uint8_t tx_buf[3] = {reg, val >> 8, val & 0xFF};

	return i2c_write(drv_data->i2c, tx_buf, sizeof(tx_buf),
			 TMP112_I2C_ADDRESS);
}

static int tmp112_reg_update(struct tmp112_data *drv_data, uint8_t reg,
			     uint16_t mask, uint16_t val)
{
	uint16_t old_val = 0;
	uint16_t new_val;

	if (tmp112_reg_read(drv_data, reg, &old_val) < 0) {
		return -EIO;
	}

	new_val = old_val & ~mask;
	new_val |= val & mask;

	return tmp112_reg_write(drv_data, reg, new_val);
}

static int tmp112_attr_set(struct device *dev,
			   enum sensor_channel chan,
			   enum sensor_attribute attr,
			   const struct sensor_value *val)
{
	struct tmp112_data *drv_data = dev->driver_data;
	int64_t value;
	uint16_t cr;

	if (chan != SENSOR_CHAN_TEMP) {
		return -ENOTSUP;
	}

	switch (attr) {
	case SENSOR_ATTR_FULL_SCALE:

		if (val->type != SENSOR_VALUE_TYPE_INT) {
			return -ENOTSUP;
		}

		/* the sensor supports two ranges -55 to 128 and -55 to 150 */
		/* the value contains the upper limit */
		if (val->val1 == 128) {
			value = 0x0000;
		} else if (val->val1 == 150) {
			value = TMP112_EM_BIT;
		} else {
			return -ENOTSUP;
		}

		if (tmp112_reg_update(drv_data, TMP112_REG_CONFIG,
				      TMP112_EM_BIT, value) < 0) {
			SYS_LOG_DBG("Failed to set attribute!");
			return -EIO;
		}

		return 0;

	case SENSOR_ATTR_SAMPLING_FREQUENCY:

		if (val->type != SENSOR_VALUE_TYPE_INT_PLUS_MICRO) {
			return -ENOTSUP;
		}

		/* conversion rate in mHz */
		cr = val->val1 * 1000 + val->val2 / 1000;

		/* the sensor supports 0.25Hz, 1Hz, 4Hz and 8Hz */
		/* conversion rate */
		switch (cr) {
		case 250:
			value = 0x0000;
			break;

		case 1000:
			value = TMP112_CR0_BIT;
			break;

		case 4000:
			value = TMP112_CR1_BIT;
			break;

		case 8000:
			value = TMP112_CR0_BIT | TMP112_CR1_BIT;
			break;

		default:
			return -ENOTSUP;
		}

		if (tmp112_reg_update(drv_data, TMP112_REG_CONFIG,
				      TMP112_CR0_BIT | TMP112_CR1_BIT,
				      value) < 0) {
			SYS_LOG_DBG("Failed to set attribute!");
			return -EIO;
		}

		return 0;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int tmp112_sample_fetch(struct device *dev, enum sensor_channel chan)
{
	struct tmp112_data *drv_data = dev->driver_data;
	uint16_t val;

	__ASSERT_NO_MSG(chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_TEMP);

	if (tmp112_reg_read(drv_data, TMP112_REG_TEMPERATURE, &val) < 0) {
		return -EIO;
	}

	if (val & TMP112_D0_BIT) {
		drv_data->sample = arithmetic_shift_right((int16_t)val, 3);
	} else {
		drv_data->sample = arithmetic_shift_right((int16_t)val, 4);
	}

	return 0;
}

static int tmp112_channel_get(struct device *dev,
		enum sensor_channel chan,
		struct sensor_value *val)
{
	struct tmp112_data *drv_data = dev->driver_data;
	int32_t uval;

	if (chan != SENSOR_CHAN_TEMP) {
		return -ENOTSUP;
	}

	uval = (int32_t)drv_data->sample * TMP112_TEMP_SCALE;
	val->type = SENSOR_VALUE_TYPE_INT_PLUS_MICRO;
	val->val1 = uval / 1000000;
	val->val2 = uval % 1000000;

	return 0;
}

static const struct sensor_driver_api tmp112_driver_api = {
	.attr_set = tmp112_attr_set,
	.sample_fetch = tmp112_sample_fetch,
	.channel_get = tmp112_channel_get,
};

int tmp112_init(struct device *dev)
{
	struct tmp112_data *drv_data = dev->driver_data;

	drv_data->i2c = device_get_binding(CONFIG_TMP112_I2C_MASTER_DEV_NAME);
	if (drv_data->i2c == NULL) {
		SYS_LOG_DBG("Failed to get pointer to %s device!",
			    CONFIG_TMP112_I2C_MASTER_DEV_NAME);
		return -EINVAL;
	}

	dev->driver_api = &tmp112_driver_api;

	return 0;
}

static struct tmp112_data tmp112_driver;

DEVICE_INIT(tmp112, CONFIG_TMP112_NAME, tmp112_init, &tmp112_driver,
	    NULL, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY);
