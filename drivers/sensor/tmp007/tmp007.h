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

#ifndef _SENSOR_TMP007
#define _SENSOR_TMP007

#include <device.h>
#include <gpio.h>
#include <misc/util.h>

#define TMP007_I2C_ADDRESS		CONFIG_TMP007_I2C_ADDR

#define TMP007_REG_CONFIG		0x02
#define TMP007_ALERT_EN_BIT		BIT(8)

#define TMP007_REG_TOBJ			0x03
#define TMP007_DATA_INVALID_BIT		BIT(0)

#define TMP007_REG_STATUS		0x04
#define TMP007_DATA_READY_INT_BIT	BIT(14)
#define TMP007_TOBJ_TH_HIGH_INT_BIT	BIT(13)
#define TMP007_TOBJ_TH_LOW_INT_BIT	BIT(12)
#define TMP007_TOBJ_TH_INT_BITS		\
	(TMP007_TOBJ_TH_HIGH_INT_BIT | TMP007_TOBJ_TH_LOW_INT_BIT)

#define TMP007_REG_TOBJ_TH_HIGH		0x06
#define TMP007_REG_TOBJ_TH_LOW		0x07

/* scale in micro degrees Celsius */
#define TMP007_TEMP_SCALE		31250
#define TMP007_TEMP_TH_SCALE		500000

struct tmp007_data {
	struct device *i2c;
	int16_t sample;

#ifdef CONFIG_TMP007_TRIGGER
	struct device *gpio;
	struct gpio_callback gpio_cb;

	sensor_trigger_handler_t drdy_handler;
	struct sensor_trigger drdy_trigger;

	sensor_trigger_handler_t th_handler;
	struct sensor_trigger th_trigger;

#if defined(CONFIG_TMP007_TRIGGER_OWN_THREAD)
	char __stack thread_stack[CONFIG_TMP007_THREAD_STACK_SIZE];
	struct k_sem gpio_sem;
#elif defined(CONFIG_TMP007_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
	struct device *dev;
#endif

#endif /* CONFIG_TMP007_TRIGGER */
};

#ifdef CONFIG_TMP007_TRIGGER
int tmp007_reg_read(struct tmp007_data *drv_data, uint8_t reg, uint16_t *val);

int tmp007_reg_write(struct tmp007_data *drv_data, uint8_t reg, uint16_t val);

int tmp007_reg_update(struct tmp007_data *drv_data, uint8_t reg,
		      uint16_t mask, uint16_t val);

int tmp007_attr_set(struct device *dev,
		    enum sensor_channel chan,
		    enum sensor_attribute attr,
		    const struct sensor_value *val);

int tmp007_trigger_set(struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler);

int tmp007_init_interrupt(struct device *dev);
#endif

#define SYS_LOG_DOMAIN "TMP007"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_SENSOR_LEVEL
#include <misc/sys_log.h>
#endif /* _SENSOR_TMP007_ */
