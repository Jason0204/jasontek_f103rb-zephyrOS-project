/*
 * Copyright (c) 2015 Intel Corporation.
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

#ifndef _RTC_H_
#define _RTC_H_
#include <stdint.h>
#include <device.h>
#include <misc/util.h>

#ifdef __cplusplus
extern "C" {
#endif

enum clk_rtc_div {
	RTC_CLK_DIV_1,
	RTC_CLK_DIV_2,
	RTC_CLK_DIV_4,
	RTC_CLK_DIV_8,
	RTC_CLK_DIV_16,
	RTC_CLK_DIV_32,
	RTC_CLK_DIV_64,
	RTC_CLK_DIV_128,
	RTC_CLK_DIV_256,
	RTC_CLK_DIV_512,
	RTC_CLK_DIV_1024,
	RTC_CLK_DIV_2048,
	RTC_CLK_DIV_4096,
	RTC_CLK_DIV_8192,
	RTC_CLK_DIV_16384,
	RTC_CLK_DIV_32768
};

#define RTC_DIVIDER RTC_CLK_DIV_1

/** Number of RTC ticks in a second */
#define RTC_ALARM_SECOND (32768 / (1UL << RTC_DIVIDER))

/** Number of RTC ticks in a minute */
#define RTC_ALARM_MINUTE (RTC_ALARM_SECOND * 60)

/** Number of RTC ticks in an hour */
#define RTC_ALARM_HOUR (RTC_ALARM_MINUTE * 60)

/** Number of RTC ticks in a day */
#define RTC_ALARM_DAY (RTC_ALARM_HOUR * 24)



struct rtc_config {
	uint32_t init_val;
	/*!< enable/disable alarm  */
	uint8_t alarm_enable;
	/*!< initial configuration value for the 32bit RTC alarm value  */
	uint32_t alarm_val;
	/*!< Pointer to function to call when alarm value
	 * matches current RTC value */
	void (*cb_fn)(struct device *dev);
};

typedef void (*rtc_api_enable)(struct device *dev);
typedef void (*rtc_api_disable)(struct device *dev);
typedef int (*rtc_api_set_config)(struct device *dev,
				  struct rtc_config *config);
typedef int (*rtc_api_set_alarm)(struct device *dev,
				 const uint32_t alarm_val);
typedef uint32_t (*rtc_api_read)(struct device *dev);
typedef uint32_t (*rtc_api_get_pending_int)(struct device *dev);

struct rtc_driver_api {
	rtc_api_enable enable;
	rtc_api_disable disable;
	rtc_api_read read;
	rtc_api_set_config set_config;
	rtc_api_set_alarm set_alarm;
	rtc_api_get_pending_int get_pending_int;
};

static inline uint32_t rtc_read(struct device *dev)
{
	const struct rtc_driver_api *api = dev->driver_api;

	return api->read(dev);
}

static inline void rtc_enable(struct device *dev)
{
	const struct rtc_driver_api *api = dev->driver_api;

	api->enable(dev);
}


static inline void rtc_disable(struct device *dev)
{
	const struct rtc_driver_api *api = dev->driver_api;

	api->disable(dev);
}

static inline int rtc_set_config(struct device *dev,
				 struct rtc_config *cfg)
{
	const struct rtc_driver_api *api = dev->driver_api;

	return api->set_config(dev, cfg);
}

static inline int rtc_set_alarm(struct device *dev,
				const uint32_t alarm_val)
{
	const struct rtc_driver_api *api = dev->driver_api;

	return api->set_alarm(dev, alarm_val);
}

/**
 * @brief Function to get pending interrupts
 *
 * The purpose of this function is to return the interrupt
 * status register for the device.
 * This is especially useful when waking up from
 * low power states to check the wake up source.
 *
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 1 if the rtc interrupt is pending.
 * @retval 0 if no rtc interrupt is pending.
 */
static inline int rtc_get_pending_int(struct device *dev)
{
	struct rtc_driver_api *api;

	api = (struct rtc_driver_api *)dev->driver_api;
	return api->get_pending_int(dev);
}

#ifdef __cplusplus
}
#endif

#endif
