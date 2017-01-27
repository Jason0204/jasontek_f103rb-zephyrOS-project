/**
 * @file sensor.h
 *
 * @brief Public APIs for the sensor driver.
 */

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
#ifndef __SENSOR_H__
#define __SENSOR_H__

/**
 * @brief Sensor Interface
 * @defgroup sensor_interface Sensor Interface
 * @ingroup io_interfaces
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <device.h>
#include <errno.h>

/** @brief Sensor value types. */
enum sensor_value_type {
	/** val1 contains an integer value, val2 is unused. */
	SENSOR_VALUE_TYPE_INT,
	/**
	 * val1 contains an integer value, val2 is the fractional value.
	 * To obtain the final value, use the formula: val1 + val2 *
	 * 10^(-6).
	 */
	SENSOR_VALUE_TYPE_INT_PLUS_MICRO,
	/**
	 * @brief val1 contains a Q16.16 representation, val2 is
	 * unused.
	 */
	SENSOR_VALUE_TYPE_Q16_16,
	/** @brief dval contains a floating point value. */
	SENSOR_VALUE_TYPE_DOUBLE,
};

/**
 * @brief Representation of a sensor readout value.
 *
 * The meaning of the fields is dictated by the type field.
 */
struct sensor_value {
	enum sensor_value_type type;
	union  {
		struct {
			int32_t val1;
			int32_t val2;
		};
		double dval;
	};
};

/**
 * @brief Sensor channels.
 */
enum sensor_channel {
	/** Acceleration on the X axis, in m/s^2. */
	SENSOR_CHAN_ACCEL_X,
	/** Acceleration on the Y axis, in m/s^2. */
	SENSOR_CHAN_ACCEL_Y,
	/** Acceleration on the Z axis, in m/s^2. */
	SENSOR_CHAN_ACCEL_Z,
	/** Acceleration on any axis. */
	SENSOR_CHAN_ACCEL_ANY,
	/** Angular velocity around the X axis, in radians/s. */
	SENSOR_CHAN_GYRO_X,
	/** Angular velocity around the Y axis, in radians/s. */
	SENSOR_CHAN_GYRO_Y,
	/** Angular velocity around the Z axis, in radians/s. */
	SENSOR_CHAN_GYRO_Z,
	/** Angular velocity on any axis. */
	SENSOR_CHAN_GYRO_ANY,
	/** Magnetic field on the X axis, in Gauss. */
	SENSOR_CHAN_MAGN_X,
	/** Magnetic field on the Y axis, in Gauss. */
	SENSOR_CHAN_MAGN_Y,
	/** Magnetic field on the Z axis, in Gauss. */
	SENSOR_CHAN_MAGN_Z,
	/** Magnetic field on any axis. */
	SENSOR_CHAN_MAGN_ANY,
	/** Temperature in degrees Celsius. */
	SENSOR_CHAN_TEMP,
	/** Pressure in kilopascal. */
	SENSOR_CHAN_PRESS,
	/**
	 * Proximity.  Adimensional.  A value of 1 indicates that an
	 * object is close.
	 */
	SENSOR_CHAN_PROX,
	/** Humidity, in milli percent. */
	SENSOR_CHAN_HUMIDITY,
	/** Illuminance in visible spectrum, in lux. */
	SENSOR_CHAN_LIGHT,
	/** Illuminance in infra-red spectrum, in lux. */
	SENSOR_CHAN_IR,
	/** Altitude, in meters */
	SENSOR_CHAN_ALTITUDE,
	/** All channels. */
	SENSOR_CHAN_ALL,
};

/**
 * @brief Sensor trigger types.
 */
enum sensor_trigger_type {
	/**
	 * Timer-based trigger, useful when the sensor does not have an
	 * interrupt line.
	 */
	SENSOR_TRIG_TIMER,
	/** Trigger fires whenever new data is ready. */
	SENSOR_TRIG_DATA_READY,
	/**
	 * Trigger fires when the selected channel varies significantly.
	 * This includes any-motion detection when the channel is
	 * acceleration or gyro. If detection is based on slope between
	 * successive channel readings, the slope threshold is configured
	 * via the @ref SENSOR_ATTR_SLOPE_TH and @ref SENSOR_ATTR_SLOPE_DUR
	 * attributes.
	 */
	SENSOR_TRIG_DELTA,
	/** Trigger fires when a near/far event is detected. */
	SENSOR_TRIG_NEAR_FAR,
	/**
	 * Trigger fires when channel reading transitions configured
	 * thresholds.  The thresholds are configured via the @ref
	 * SENSOR_ATTR_LOWER_THRESH and @ref SENSOR_ATTR_UPPER_THRESH
	 * attributes.
	 */
	SENSOR_TRIG_THRESHOLD,
};

/**
 * @brief Sensor trigger spec.
 */
struct sensor_trigger {
	/** Trigger type. */
	enum sensor_trigger_type type;
	/** Channel the trigger is set on. */
	enum sensor_channel chan;
};

/**
 * @brief Sensor attribute types.
 */
enum sensor_attribute {
	/**
	 * Sensor sampling frequency, i.e. how many times a second the
	 * sensor takes a measurement.
	 */
	SENSOR_ATTR_SAMPLING_FREQUENCY,
	/** Lower threshold for trigger. */
	SENSOR_ATTR_LOWER_THRESH,
	/** Upper threshold for trigger. */
	SENSOR_ATTR_UPPER_THRESH,
	/** Threshold for any-motion (slope) trigger. */
	SENSOR_ATTR_SLOPE_TH,
	/**
	 * Duration for which the slope values needs to be
	 * outside the threshold for the trigger to fire.
	 */
	SENSOR_ATTR_SLOPE_DUR,
	/** Oversampling factor */
	SENSOR_ATTR_OVERSAMPLING,
	/** Sensor range, in SI units. */
	SENSOR_ATTR_FULL_SCALE,
	/**
	 * The sensor value returned will be altered by the amount indicated by
	 * offset: final_value = sensor_value + offset.
	 */
	SENSOR_ATTR_OFFSET,
	/**
	 * Calibration target. This will be used by the internal chip's
	 * algorithms to calibrate itself on a certain axis, or all of them.
	 */
	SENSOR_ATTR_CALIB_TARGET,
};

/**
 * @typedef sensor_trigger_handler_t
 * @brief Callback API upon firing of a trigger
 *
 * @param "struct device *dev" Pointer to the sensor device
 * @param "struct sensor_trigger *trigger" The trigger
 */
typedef void (*sensor_trigger_handler_t)(struct device *dev,
					 struct sensor_trigger *trigger);

/**
 * @typedef sensor_attr_set_t
 * @brief Callback API upon setting a sensor's attributes
 *
 * See sensor_attr_set() for argument description
 */
typedef int (*sensor_attr_set_t)(struct device *dev,
				 enum sensor_channel chan,
				 enum sensor_attribute attr,
				 const struct sensor_value *val);
/**
 * @typedef sensor_trigger_set_t
 * @brief Callback API for setting a sensor's trigger and handler
 *
 * See sensor_trigger_set() for argument description
 */
typedef int (*sensor_trigger_set_t)(struct device *dev,
				    const struct sensor_trigger *trig,
				    sensor_trigger_handler_t handler);
/**
 * @typedef sensor_sample_fetch_t
 * @brief Callback API for fetching data from a sensor
 *
 * See sensor_sample_fetch() for argument descriptor
 */
typedef int (*sensor_sample_fetch_t)(struct device *dev,
				     enum sensor_channel chan);
/**
 * @typedef sensor_channel_get_t
 * @brief Callback API for getting a reading from a sensor
 *
 * See sensor_channel_get() for argument descriptor
 */
typedef int (*sensor_channel_get_t)(struct device *dev,
				    enum sensor_channel chan,
				    struct sensor_value *val);

struct sensor_driver_api {
	sensor_attr_set_t attr_set;
	sensor_trigger_set_t trigger_set;
	sensor_sample_fetch_t sample_fetch;
	sensor_channel_get_t channel_get;
};

/**
 * @brief Set an attribute for a sensor
 *
 * @param dev Pointer to the sensor device
 * @param chan The channel the attribute belongs to, if any.  Some
 * attributes may only be set for all channels of a device, depending on
 * device capabilities.
 * @param attr The attribute to set
 * @param val The value to set the attribute to
 *
 * @return 0 if successful, negative errno code if failure.
 */
static inline int sensor_attr_set(struct device *dev,
				  enum sensor_channel chan,
				  enum sensor_attribute attr,
				  const struct sensor_value *val)
{
	const struct sensor_driver_api *api = dev->driver_api;

	if (!api->attr_set) {
		return -ENOTSUP;
	}

	return api->attr_set(dev, chan, attr, val);
}

/**
 * @brief Activate a sensor's trigger and set the trigger handler
 *
 * The handler will be called from a fiber, so I2C or SPI operations are
 * safe.  However, the fiber's stack is limited and defined by the
 * driver.  It is currently up to the caller to ensure that the handler
 * does not overflow the stack.
 *
 * @param dev Pointer to the sensor device
 * @param trig The trigger to activate
 * @param handler The function that should be called when the trigger
 * fires
 *
 * @return 0 if successful, negative errno code if failure.
 */
static inline int sensor_trigger_set(struct device *dev,
				     struct sensor_trigger *trig,
				     sensor_trigger_handler_t handler)
{
	const struct sensor_driver_api *api = dev->driver_api;

	if (!api->trigger_set) {
		return -ENOTSUP;
	}

	return api->trigger_set(dev, trig, handler);
}

/**
 * @brief Fetch a sample from the sensor and store it in an internal
 * driver buffer
 *
 * Read all of a sensor's active channels and, if necessary, perform any
 * additional operations necessary to make the values useful.  The user
 * may then get individual channel values by calling @ref
 * sensor_channel_get.
 *
 * Since the function communicates with the sensor device, it is unsafe
 * to call it in an ISR if the device is connected via I2C or SPI.
 *
 * @param dev Pointer to the sensor device
 *
 * @return 0 if successful, negative errno code if failure.
 */
static inline int sensor_sample_fetch(struct device *dev)
{
	const struct sensor_driver_api *api = dev->driver_api;

	return api->sample_fetch(dev, SENSOR_CHAN_ALL);
}

/**
 * @brief Fetch a sample from the sensor and store it in an internal
 * driver buffer
 *
 * Read and compute compensation for one type of sensor data (magnetometer,
 * accelerometer, etc). The user may then get individual channel values by
 * calling @ref sensor_channel_get.
 *
 * This is mostly implemented by multi function devices enabling reading at
 * different sampling rates.
 *
 * Since the function communicates with the sensor device, it is unsafe
 * to call it in an ISR if the device is connected via I2C or SPI.
 *
 * @param dev Pointer to the sensor device
 * @param type The channel that needs updated
 *
 * @return 0 if successful, negative errno code if failure.
 */
static inline int sensor_sample_fetch_chan(struct device *dev,
					   enum sensor_channel type)
{
	const struct sensor_driver_api *api = dev->driver_api;

	return api->sample_fetch(dev, type);
}

/**
 * @brief Get a reading from a sensor device
 *
 * Return a useful value for a particular channel, from the driver's
 * internal data.  Before calling this function, a sample must be
 * obtained by calling @ref sensor_sample_fetch or
 * @ref sensor_sample_fetch_chan. It is guaranteed that two subsequent
 * calls of this function for the same channels will yield the same
 * value, if @ref sensor_sample_fetch or @ref sensor_sample_fetch_chan
 * has not been called in the meantime.
 *
 * For vectorial data samples you can request all axes in just one call
 * by passing the specific channel with _ANY suffix. The sample will be
 * returned at val[0], val[1] and val[2] (X, Y and Z in that order).
 *
 * @param dev Pointer to the sensor device
 * @param chan The channel to read
 * @param val Where to store the value
 *
 * @return 0 if successful, negative errno code if failure.
 */
static inline int sensor_channel_get(struct device *dev,
				     enum sensor_channel chan,
				     struct sensor_value *val)
{
	const struct sensor_driver_api *api = dev->driver_api;

	return api->channel_get(dev, chan, val);
}

/**
 * @brief The value of gravitational constant in micro m/s^2.
 */
#define SENSOR_G		9806650LL

/**
 * @brief The value of constant PI in micros.
 */
#define SENSOR_PI		3141592LL

/**
 * @brief Helper function to convert acceleration from m/s^2 to Gs
 *
 * @param ms2 A pointer to a sensor_value struct holding the acceleration,
 *            in m/s^2.
 *
 * @return The converted value, in Gs.
 */
static inline int32_t sensor_ms2_to_g(const struct sensor_value *ms2)
{
	int64_t micro_ms2 = ms2->val1 * 1000000LL + ms2->val2;

	if (micro_ms2 > 0) {
		return (micro_ms2 + SENSOR_G / 2) / SENSOR_G;
	} else {
		return (micro_ms2 - SENSOR_G / 2) / SENSOR_G;
	}
}

/**
 * @brief Helper function to convert acceleration from Gs to m/s^2
 *
 * @param g The G value to be converted.
 * @param ms2 A pointer to a sensor_value struct, where the result is stored.
 */
static inline void sensor_g_to_ms2(int32_t g, struct sensor_value *ms2)
{
	ms2->type = SENSOR_VALUE_TYPE_INT_PLUS_MICRO;
	ms2->val1 = ((int64_t)g * SENSOR_G) / 1000000LL;
	ms2->val2 = ((int64_t)g * SENSOR_G) % 1000000LL;
}

/**
 * @brief Helper function for converting radians to degrees.
 *
 * @param rad A pointer to a sensor_value struct, holding the value in radians.
 *
 * @return The converted value, in degrees.
 */
static inline int32_t sensor_rad_to_degrees(const struct sensor_value *rad)
{
	int64_t micro_rad_s = rad->val1 * 1000000LL + rad->val2;

	if (micro_rad_s > 0) {
		return (micro_rad_s * 180LL + SENSOR_PI / 2) / SENSOR_PI;
	} else {
		return (micro_rad_s * 180LL - SENSOR_PI / 2) / SENSOR_PI;
	}
}

/**
 * @brief Helper function for converting degrees to radians.
 *
 * @param d The value (in degrees) to be converted.
 * @param rad A pointer to a sensor_value struct, where the result is stored.
 */
static inline void sensor_degrees_to_rad(int32_t d, struct sensor_value *rad)
{
	rad->type = SENSOR_VALUE_TYPE_INT_PLUS_MICRO;
	rad->val1 = ((int64_t)d * SENSOR_PI / 180LL) / 1000000LL;
	rad->val2 = ((int64_t)d * SENSOR_PI / 180LL) % 1000000LL;
}

/**
 * @brief configuration parameters for sensor triggers.
 */
enum sensor_trigger_mode {
	/** Do not use triggering. */
	SENSOR_TRIG_MODE_NONE,
	/**
	 * Driver should start a workqueue specifically for this
	 * device.  See @ref sensor_trig_or_wq_config for instruction on
	 * how to specify the parameters of the workqueue.
	 */
	SENSOR_TRIG_MODE_OWN_WQ,
	/** Use the system workqueue. */
	SENSOR_TRIG_MODE_GLOBAL_WQ,
};

/**
 * @brief configuration parameters for sensor triggers.
 */
struct sensor_trigger_config {
	/**
	 * This is always set to NULL when using a @ref
	 * sensor_trigger_config.  See the comment in @ref
	 * sensor_trig_or_wq_config.
	 */
	void *always_null;
	enum sensor_trigger_mode mode;
};

/**
 * @brief Structure used for sensor trigger configuration.
 *
 * If fiber_config.stack is non-NULL, the driver should start its
 * own fiber based on @ref fiber_config.  Otherwise, use
 * sensor_interface::sensor_trigger_mode to decide if and how to use
 * triggering.
 */
union sensor_trig_or_wq_config {
	struct fiber_config fiber_config;
	struct sensor_trigger_config trig_config;
};

#define SENSOR_DECLARE_TRIG_CONFIG		\
	union sensor_trig_or_wq_config trig_or_wq_config

#define SENSOR_TRIG_WQ_OWN(_stack, _prio)			\
	.trig_or_wq_config = {					\
		.fiber_config = {				\
			.stack = (_stack),			\
			.stack_size = sizeof(_stack),		\
			.prio = (_prio),			\
		}						\
	}

#define SENSOR_TRIG_WQ_GLOBAL					\
	.trig_or_wq_config = {					\
		.trig_config = {				\
			.always_null = NULL,			\
			.mode = SENSOR_TRIG_MODE_GLOBAL_WQ,	\
		}						\
	}

#define SENSOR_TRIG_NONE					\
	.trig_or_wq_config = {					\
		.trig_config = {				\
			.always_null = NULL,			\
			.mode = SENSOR_TRIG_MODE_NONE,		\
		}						\
	}

#define SENSOR_GET_TRIG_MODE(_conf)				\
	(!(_conf)->trig_or_wq_config.fiber_config.stack		\
	 ? SENSOR_TRIG_MODE_OWN_WQ :				\
	 (_conf)->trig_or_wq_config.trig_config.mode)
#define SENSOR_GET_WQ_CONFIG(_conf)				\
	((_conf)->trig_or_wq_config.fiber_config)

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* __SENSOR_H__ */
