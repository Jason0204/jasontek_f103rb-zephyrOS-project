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

#ifndef __SENSOR_LIS3MDL_H__
#define __SENSOR_LIS3MDL_H__

#include <device.h>
#include <misc/util.h>
#include <stdint.h>
#include <gpio.h>

#define SYS_LOG_DOMAIN "LIS3MDL"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_SENSOR_LEVEL
#include <misc/sys_log.h>

#define LIS3MDL_I2C_ADDR_BASE		0x1C
#define LIS3MDL_I2C_ADDR_MASK		(~BIT(1))

/* guard against invalid CONFIG_I2C_ADDR values */
#if (CONFIG_LIS3MDL_I2C_ADDR & LIS3MDL_I2C_ADDR_MASK) != LIS3MDL_I2C_ADDR_BASE
#error "Invalid value for CONFIG_LIS3MDL_I2C_ADDR"
#endif

#define LIS3MDL_REG_WHO_AM_I		0x0F
#define LIS3MDL_CHIP_ID			0x3D

#define LIS3MDL_REG_CTRL1		0x20
#define LIS3MDL_OM_SHIFT		5
#define LIS3MDL_OM_MASK			(BIT_MASK(2) << LIS3MDL_OM_SHIFT)
#define LIS3MDL_DO_SHIFT		2
#define LIS3MDL_FAST_ODR_SHIFT		1
#define LIS3MDL_FAST_ODR_MASK		BIT(LIS3MDL_FAST_ODR_SHIFT)
#define LIS3MDL_TEMP_EN			BIT(7)

#define LIS3MDL_ODR_BITS(om_bits, do_bits, fast_odr)	\
	(((om_bits) << LIS3MDL_OM_SHIFT) |		\
	 ((do_bits) << LIS3MDL_DO_SHIFT) |		\
	 ((fast_odr) << LIS3MDL_FAST_ODR_SHIFT))

#define LIS3MDL_REG_CTRL2		0x21
#define LIS3MDL_FS_SHIFT		5
#define LIS3MDL_FS_IDX			((CONFIG_LIS3MDL_FS / 4) - 1)

/* guard against invalid CONFIG_LIS3MDL_FS values */
#if CONFIG_LIS3MDL_FS % 4 != 0 || LIS3MDL_FS_IDX < -1 || LIS3MDL_FS_IDX >= 4
#error "Invalid value for CONFIG_LIS3MDL_FS"
#endif

#define LIS3MDL_REG_CTRL3		0x22
#define LIS3MDL_MD_CONTINUOUS		0
#define LIS3MDL_MD_SINGLE		1

#define LIS3MDL_REG_CTRL4		0x23
#define LIS3MDL_OMZ_SHIFT		2

#define LIS3MDL_REG_CTRL5		0x024
#define LIS3MDL_BDU_EN			BIT(6)

#define LIS3MDL_REG_SAMPLE_START	0x28

#define LIS3MDL_REG_INT_CFG		0x30
#define LIS3MDL_INT_X_EN		BIT(7)
#define LIS3MDL_INT_Y_EN		BIT(6)
#define LIS3MDL_INT_Z_EN		BIT(5)
#define LIS3MDL_INT_XYZ_EN		\
	(LIS3MDL_INT_X_EN | LIS3MDL_INT_Y_EN | LIS3MDL_INT_Z_EN)

static const char * const lis3mdl_odr_strings[] = {
	"0.625", "1.25", "2.5", "5", "10", "20",
	"40", "80", "155", "300", "560", "1000"
};

static const uint8_t lis3mdl_odr_bits[] = {
	LIS3MDL_ODR_BITS(0, 0, 0), /* 0.625 Hz */
	LIS3MDL_ODR_BITS(0, 1, 0), /* 1.25 Hz */
	LIS3MDL_ODR_BITS(0, 2, 0), /* 2.5 Hz */
	LIS3MDL_ODR_BITS(0, 3, 0), /* 5 Hz */
	LIS3MDL_ODR_BITS(0, 4, 0), /* 10 Hz */
	LIS3MDL_ODR_BITS(0, 5, 0), /* 20 Hz */
	LIS3MDL_ODR_BITS(0, 6, 0), /* 40 Hz */
	LIS3MDL_ODR_BITS(0, 7, 0), /* 80 Hz */
	LIS3MDL_ODR_BITS(3, 0, 1), /* 155 Hz */
	LIS3MDL_ODR_BITS(2, 0, 1), /* 300 Hz */
	LIS3MDL_ODR_BITS(1, 0, 1), /* 560 Hz */
	LIS3MDL_ODR_BITS(0, 0, 1)  /* 1000 Hz */
};

static const uint16_t lis3mdl_magn_gain[] = {
	6842, 3421, 2281, 1711
};

struct lis3mdl_data {
	struct device *i2c;
	int16_t x_sample;
	int16_t y_sample;
	int16_t z_sample;
	int16_t temp_sample;

#ifdef CONFIG_LIS3MDL_TRIGGER
	struct device *gpio;
	struct gpio_callback gpio_cb;

	struct sensor_trigger data_ready_trigger;
	sensor_trigger_handler_t data_ready_handler;

#if defined(CONFIG_LIS3MDL_TRIGGER_OWN_THREAD)
	char __stack thread_stack[CONFIG_LIS3MDL_THREAD_STACK_SIZE];
	struct k_sem gpio_sem;
#elif defined(CONFIG_LIS3MDL_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
	struct device *dev;
#endif

#endif /* CONFIG_LIS3MDL_TRIGGER */
};

#ifdef CONFIG_LIS3MDL_TRIGGER
int lis3mdl_trigger_set(struct device *dev,
			const struct sensor_trigger *trig,
			sensor_trigger_handler_t handler);

int lis3mdl_sample_fetch(struct device *dev, enum sensor_channel chan);

int lis3mdl_init_interrupt(struct device *dev);
#endif

#endif /* __SENSOR_LIS3MDL__ */
