/**
 * @file
 *
 * @brief Generic low-level inter-processor mailbox communication API.
 */

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

#ifndef __INCipmh
#define __INCipmh

/**
 * @brief IPM Interface
 * @defgroup ipm_interface IPM Interface
 * @ingroup io_interfaces
 * @{
 */

#include <nanokernel.h>
#include <device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef ipm_callback_t
 * @brief Callback API for incoming IPM messages
 *
 * These callbacks execute in interrupt context. Therefore, use only
 * interrupt-safe APIS. Registration of callbacks is done via
 * @a ipm_register_callback
 *
 * @param "void *context" Arbitrary context pointer provided at
 *        registration time.
 * @param "uint32_t id" Message type identifier.
 * @param "volatile void *data" Message data pointer. The correct
 *        amount of data to read out
 * must be inferred using the message id/upper level protocol.
 */
typedef void (*ipm_callback_t)(void *context, uint32_t id, volatile void *data);

/**
 * @typedef ipm_send_t
 * @brief Callback API to send IPM messages
 *
 * See @a ipm_send() for argument definitions.
 */
typedef int (*ipm_send_t)(struct device *ipmdev, int wait, uint32_t id,
			  const void *data, int size);
/**
 * @typedef ipm_max_data_size_get_t
 * @brief Callback API to get maximum data size
 *
 * See @a ipm_max_data_size_get() for argument definitions.
 */
typedef int (*ipm_max_data_size_get_t)(struct device *ipmdev);

/**
 * @typedef ipm_max_id_val_get_t
 * @brief Callback API to get the ID's maximum value
 *
 * See @a ipm_max_id_val_get() for argument definitions.
 */
typedef uint32_t (*ipm_max_id_val_get_t)(struct device *ipmdev);

/**
 * @typedef ipm_register_callback_t
 * @brief Callback API upon registration
 *
 * See @a ipm_register_callback() for argument definitions.
 */
typedef void (*ipm_register_callback_t)(struct device *port, ipm_callback_t cb,
					void *cb_context);

/**
 * @typedef ipm_set_enabled_t
 * @brief Callback API upon emablement of interrupts
 *
 * See @a ipm_set_enabled() for argument definitions.
 */
typedef int (*ipm_set_enabled_t)(struct device *ipmdev, int enable);

struct ipm_driver_api {
	ipm_send_t send;
	ipm_register_callback_t register_callback;
	ipm_max_data_size_get_t max_data_size_get;
	ipm_max_id_val_get_t max_id_val_get;
	ipm_set_enabled_t set_enabled;
};

/**
 * @brief Try to send a message over the IPM device.
 *
 * A message is considered consumed once the remote interrupt handler
 * finishes. If there is deferred processing on the remote side,
 * or if outgoing messages must be queued and wait on an
 * event/semaphore, a high-level driver can implement that.
 *
 * There are constraints on how much data can be sent or the maximum value
 * of id. Use the @a ipm_max_data_size_get and @a ipm_max_id_val_get routines
 * to determine them.
 *
 * The @a size parameter is used only on the sending side to determine
 * the amount of data to put in the message registers. It is not passed along
 * to the receiving side. The upper-level protocol dictates the amount of
 * data read back.
 *
 * @param ipmdev Driver instance
 * @param wait If nonzero, busy-wait for remote to consume the message. The
 *	       message is considered consumed once the remote interrupt handler
 *	       finishes. If there is deferred processing on the remote side,
 *	       or you would like to queue outgoing messages and wait on an
 *	       event/semaphore, you can implement that in a high-level driver
 * @param id Message identifier. Values are constrained by
 *        @a ipm_max_data_size_get since many boards only allow for a
 *        subset of bits in a 32-bit register to store the ID.
 * @param data Pointer to the data sent in the message.
 * @param size Size of the data.
 *
 * @retval EBUSY    If the remote hasn't yet read the last data sent.
 * @retval EMSGSIZE If the supplied data size is unsupported by the driver.
 * @retval EINVAL   If there was a bad parameter, such as: too-large id value.
 *                  or the device isn't an outbound IPM channel.
 * @retval 0        On success.
 */
static inline int ipm_send(struct device *ipmdev, int wait, uint32_t id,
			   const void *data, int size)
{
	const struct ipm_driver_api *api = ipmdev->driver_api;

	return api->send(ipmdev, wait, id, data, size);
}

/**
 * @brief Register a callback function for incoming messages.
 *
 * @param ipmdev Driver instance pointer.
 * @param cb Callback function to execute on incoming message interrupts.
 * @param context Application-specific context pointer which will be passed
 *        to the callback function when executed.
 */
static inline void ipm_register_callback(struct device *ipmdev,
					 ipm_callback_t cb, void *context)
{
	const struct ipm_driver_api *api = ipmdev->driver_api;

	api->register_callback(ipmdev, cb, context);
}

/**
 * @brief Return the maximum number of bytes possible in an outbound message.
 *
 * IPM implementations vary on the amount of data that can be sent in a
 * single message since the data payload is typically stored in registers.
 *
 * @param ipmdev Driver instance pointer.
 *
 * @return Maximum possible size of a message in bytes.
 */
static inline int ipm_max_data_size_get(struct device *ipmdev)
{
	const struct ipm_driver_api *api = ipmdev->driver_api;

	return api->max_data_size_get(ipmdev);
}


/**
 * @brief Return the maximum id value possible in an outbound message.
 *
 * Many IPM implementations store the message's ID in a register with
 * some bits reserved for other uses.
 *
 * @param ipmdev Driver instance pointer.
 *
 * @return Maximum possible value of a message ID.
 */
static inline uint32_t ipm_max_id_val_get(struct device *ipmdev)
{
	const struct ipm_driver_api *api = ipmdev->driver_api;

	return api->max_id_val_get(ipmdev);
}

/**
 * @brief Enable interrupts and callbacks for inbound channels.
 *
 * @param ipmdev Driver instance pointer.
 * @param enable Set to 0 to disable and to nonzero to enable.
 *
 * @retval 0      On success.
 * @retval EINVAL If it isn't an inbound channel.
 */
static inline int ipm_set_enabled(struct device *ipmdev, int enable)
{
	const struct ipm_driver_api *api = ipmdev->driver_api;

	return api->set_enabled(ipmdev, enable);
}

#ifdef __cplusplus
}
#endif

/**
 * @}
 */
#endif /* __INCipmh */
