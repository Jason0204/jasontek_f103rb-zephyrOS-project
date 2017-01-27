/*******************************************************************************
 *
 * Copyright(c) 2015,2016 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in
 * the documentation and/or other materials provided with the
 * distribution.
 * * Neither the name of Intel Corporation nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

/**
 * @file
 * @brief WebUSB enabled custom class driver
 *
 * This is a modified version of CDC ACM class driver
 * to support the WebUSB.
 */

#include <zephyr.h>
#include <init.h>
#include <uart.h>
#include <string.h>
#include <misc/byteorder.h>
#include <misc/sys_log.h>
#include "cdc_acm.h"
#include "usb_device.h"
#include "usb_common.h"
#include "webusb_serial.h"

#ifndef CONFIG_UART_INTERRUPT_DRIVEN
#error "CONFIG_UART_INTERRUPT_DRIVEN must be set for WebUSB custom driver"
#endif

#define DEV_DATA(dev) \
	((struct webusb_serial_dev_data_t * const)(dev)->driver_data)

static const struct uart_driver_api webusb_serial_driver_api;

/* 115200bps, no parity, 1 stop bit, 8bit char */
#define DEFAUL_BAUDRATE {sys_cpu_to_le32(115200), 0, 0, 8}

/* Size of the internal buffer used for storing received data */
#define WEBUSB_BUFFER_SIZE (2 * CDC_BULK_EP_MPS)

/* Misc. macros */
#define LOW_BYTE(x)  ((x) & 0xFF)
#define HIGH_BYTE(x) ((x) >> 8)

static struct device *webusb_serial_dev;
static struct webusb_req_handlers *req_handlers;

/* Device data structure */
struct webusb_serial_dev_data_t {
	/* USB device status code */
	enum usb_dc_status_code usb_status;
	/* Callback function pointer */
	uart_irq_callback_t	cb;
	/* Tx ready status. Signals when */
	uint8_t tx_ready;
	uint8_t rx_ready;                 /* Rx ready status */
	uint8_t tx_irq_ena;               /* Tx interrupt enable status */
	uint8_t rx_irq_ena;               /* Rx interrupt enable status */
	uint8_t rx_buf[WEBUSB_BUFFER_SIZE];/* Internal Rx buffer */
	uint32_t rx_buf_head;             /* Head of the internal Rx buffer */
	uint32_t rx_buf_tail;             /* Tail of the internal Rx buffer */
	/* Interface data buffer */
	uint8_t interface_data[CDC_CLASS_REQ_MAX_DATA_SIZE];
	/* CDC ACM line coding properties. LE order */
	struct cdc_acm_line_coding line_coding;
	/* CDC ACM line state bitmap, DTE side */
	uint8_t line_state;
	/* CDC ACM serial state bitmap, DCE side */
	uint8_t serial_state;
	/* CDC ACM notification sent status */
	uint8_t notification_sent;
};

/* Structure representing the global USB description */
static const uint8_t webusb_serial_usb_description[] = {
	/* Device descriptor */
	USB_DEVICE_DESC_SIZE,           /* Descriptor size */
	USB_DEVICE_DESC,                /* Descriptor type */
	LOW_BYTE(USB_2_1),
	HIGH_BYTE(USB_2_1),             /* USB version in BCD format */
	0x00,                           /* Class - Interface specific */
	0x00,                           /* SubClass - Interface specific */
	0x00,                           /* Protocol - Interface specific */
	MAX_PACKET_SIZE0,               /* Max Packet Size */
	LOW_BYTE(CDC_VENDOR_ID),
	HIGH_BYTE(CDC_VENDOR_ID),       /* Vendor Id */
	LOW_BYTE(CDC_PRODUCT_ID),
	HIGH_BYTE(CDC_PRODUCT_ID),      /* Product Id */
	LOW_BYTE(BCDDEVICE_RELNUM),
	HIGH_BYTE(BCDDEVICE_RELNUM),    /* Device Release Number */
	/* Index of Manufacturer String Descriptor */
	0x01,
	/* Index of Product String Descriptor */
	0x02,
	/* Index of Serial Number String Descriptor */
	0x03,
	CDC_NUM_CONF,                   /* Number of Possible Configuration */

	/* Configuration descriptor */
	USB_CONFIGURATION_DESC_SIZE,    /* Descriptor size */
	USB_CONFIGURATION_DESC,         /* Descriptor type */
	/* Total length in bytes of data returned */
	LOW_BYTE(WEBUSB_SERIAL_CONF_SIZE),
	HIGH_BYTE(WEBUSB_SERIAL_CONF_SIZE),
	WEBUSB_NUM_ITF,                 /* Number of interfaces */
	0x01,                           /* Configuration value */
	0x00,                           /* Index of the Configuration string */
	USB_CONFIGURATION_ATTRIBUTES,   /* Attributes */
	MAX_LOW_POWER,                  /* Max power consumption */

	/* Interface descriptor */
	USB_INTERFACE_DESC_SIZE,        /* Descriptor size */
	USB_INTERFACE_DESC,             /* Descriptor type */
	0x00,                           /* Interface index */
	0x00,                           /* Alternate setting */
	CDC1_NUM_EP,                    /* Number of Endpoints */
	COMMUNICATION_DEVICE_CLASS,     /* Class */
	ACM_SUBCLASS,                   /* SubClass */
	V25TER_PROTOCOL,                /* Protocol */
	/* Index of the Interface String Descriptor */
	0x00,

	/* Header Functional Descriptor */
	USB_HFUNC_DESC_SIZE,            /* Descriptor size */
	CS_INTERFACE,                   /* Descriptor type */
	USB_HFUNC_SUBDESC,              /* Descriptor SubType */
	LOW_BYTE(USB_1_1),
	HIGH_BYTE(USB_1_1),             /* CDC Device Release Number */

	/* Call Management Functional Descriptor */
	USB_CMFUNC_DESC_SIZE,           /* Descriptor size */
	CS_INTERFACE,                   /* Descriptor type */
	USB_CMFUNC_SUBDESC,             /* Descriptor SubType */
	0x00,                           /* Capabilities */
	0x01,                           /* Data Interface */

	/* ACM Functional Descriptor */
	USB_ACMFUNC_DESC_SIZE,          /* Descriptor size */
	CS_INTERFACE,                   /* Descriptor type */
	USB_ACMFUNC_SUBDESC,            /* Descriptor SubType */
	/* Capabilities - Device supports the request combination of:
	 * Set_Line_Coding,
	 * Set_Control_Line_State,
	 * Get_Line_Coding
	 * and the notification Serial_State
	 */
	0x02,

	/* Union Functional Descriptor */
	USB_UFUNC_DESC_SIZE,            /* Descriptor size */
	CS_INTERFACE,                   /* Descriptor type */
	USB_UFUNC_SUBDESC,              /* Descriptor SubType */
	0x00,                           /* Master Interface */
	0x01,                           /* Slave Interface */

	/* Endpoint INT */
	USB_ENDPOINT_DESC_SIZE,         /* Descriptor size */
	USB_ENDPOINT_DESC,              /* Descriptor type */
	CDC_ENDP_INT,                   /* Endpoint address */
	USB_DC_EP_INTERRUPT,            /* Attributes */
	LOW_BYTE(CDC_INTERRUPT_EP_MPS),
	HIGH_BYTE(CDC_INTERRUPT_EP_MPS),/* Max packet size */
	0x0A,                           /* Interval */

	/* Interface descriptor */
	USB_INTERFACE_DESC_SIZE,        /* Descriptor size */
	USB_INTERFACE_DESC,             /* Descriptor type */
	0x01,                           /* Interface index */
	0x00,                           /* Alternate setting */
	CDC2_NUM_EP,                    /* Number of Endpoints */
	COMMUNICATION_DEVICE_CLASS_DATA,/* Class */
	0x00,                           /* SubClass */
	0x00,                           /* Protocol */
	/* Index of the Interface String Descriptor */
	0x00,

	/* First Endpoint IN */
	USB_ENDPOINT_DESC_SIZE,         /* Descriptor size */
	USB_ENDPOINT_DESC,              /* Descriptor type */
	CDC_ENDP_IN,                    /* Endpoint address */
	USB_DC_EP_BULK,                 /* Attributes */
	LOW_BYTE(CDC_BULK_EP_MPS),
	HIGH_BYTE(CDC_BULK_EP_MPS),     /* Max packet size */
	0x00,                           /* Interval */

	/* Second Endpoint OUT */
	USB_ENDPOINT_DESC_SIZE,         /* Descriptor size */
	USB_ENDPOINT_DESC,              /* Descriptor type */
	CDC_ENDP_OUT,                   /* Endpoint address */
	USB_DC_EP_BULK,                 /* Attributes */
	LOW_BYTE(CDC_BULK_EP_MPS),
	HIGH_BYTE(CDC_BULK_EP_MPS),     /* Max packet size */
	0x00,                           /* Interval */

	/* Interface descriptor */
	USB_INTERFACE_DESC_SIZE,        /* Descriptor size */
	USB_INTERFACE_DESC,             /* Descriptor type */
	0x02,                           /* Interface index */
	0x00,                           /* Alternate setting */
	WEBUSB_NUM_EP,                  /* Number of Endpoints */
	CUSTOM_CLASS,                   /* Custom Class */
	0x00,                           /* SubClass */
	0x00,                           /* Protocol */
	/* Index of the Interface String Descriptor */
	0x00,                           /* Interval */

	/* First Endpoint IN */
	USB_ENDPOINT_DESC_SIZE,         /* Descriptor size */
	USB_ENDPOINT_DESC,              /* Descriptor type */
	WEBUSB_ENDP_IN,                 /* Endpoint address */
	USB_DC_EP_BULK,                 /* Attributes */
	LOW_BYTE(CDC_BULK_EP_MPS),
	HIGH_BYTE(CDC_BULK_EP_MPS),     /* Max packet size */
	0x00,                           /* Interval */

	/* Second Endpoint OUT */
	USB_ENDPOINT_DESC_SIZE,         /* Descriptor size */
	USB_ENDPOINT_DESC,              /* Descriptor type */
	WEBUSB_ENDP_OUT,                /* Endpoint address */
	USB_DC_EP_BULK,                 /* Attributes */
	LOW_BYTE(CDC_BULK_EP_MPS),
	HIGH_BYTE(CDC_BULK_EP_MPS),     /* Max packet size */
	0x00,                           /* Interval */

	/* String descriptor language, only one, so min size 4 bytes.
	 * 0x0409 English(US) language code used
	 */
	USB_STRING_DESC_SIZE,           /* Descriptor size */
	USB_STRING_DESC,                /* Descriptor type */
	0x09,
	0x04,

	/* Manufacturer String Descriptor "Intel" */
	0x0C,
	USB_STRING_DESC,
	'I', 0, 'n', 0, 't', 0, 'e', 0, 'l', 0,

	/* Product String Descriptor "WebUSB" */
	0x0E,
	USB_STRING_DESC,
	'W', 0, 'e', 0, 'b', 0, 'U', 0, 'S', 0, 'B', 0,

	/* Serial Number String Descriptor "00.01" */
	0x0C,
	USB_STRING_DESC,
	'0', 0, '0', 0, '.', 0, '0', 0, '1', 0,
};

/**
 * @brief Handler called for Class requests not handled by the USB stack.
 *
 * @param pSetup    Information about the request to execute.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
int webusb_serial_class_handle_req(struct usb_setup_packet *pSetup,
		int32_t *len, uint8_t **data)
{
	struct webusb_serial_dev_data_t * const dev_data =
	    DEV_DATA(webusb_serial_dev);

	switch (pSetup->bRequest) {
	case CDC_SET_LINE_CODING:
		memcpy(&dev_data->line_coding,
		    *data, sizeof(dev_data->line_coding));
		SYS_LOG_DBG("\nCDC_SET_LINE_CODING %d %d %d %d",
		    sys_le32_to_cpu(dev_data->line_coding.dwDTERate),
		    dev_data->line_coding.bCharFormat,
		    dev_data->line_coding.bParityType,
		    dev_data->line_coding.bDataBits);
		break;

	case CDC_SET_CONTROL_LINE_STATE:
		dev_data->line_state = (uint8_t)sys_le16_to_cpu(pSetup->wValue);
		SYS_LOG_DBG("CDC_SET_CONTROL_LINE_STATE 0x%x",
			    dev_data->line_state);
		break;

	case CDC_GET_LINE_CODING:
		*data = (uint8_t *)(&dev_data->line_coding);
		*len = sizeof(dev_data->line_coding);
		SYS_LOG_DBG("\nCDC_GET_LINE_CODING %d %d %d %d",
		    sys_le32_to_cpu(dev_data->line_coding.dwDTERate),
		    dev_data->line_coding.bCharFormat,
		    dev_data->line_coding.bParityType,
		    dev_data->line_coding.bDataBits);
		break;

	default:
		SYS_LOG_DBG("WebUSB request 0x%x, value 0x%x",
		    pSetup->bRequest, pSetup->wValue);
		return -EINVAL;
	}

	return 0;
}

/**
 * @brief Custom handler for standard requests in order to
 *        catch the request and return the WebUSB Platform
 *        Capability Descriptor.
 *
 * @param pSetup    Information about the request to execute.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
int webusb_serial_custom_handle_req(struct usb_setup_packet *pSetup,
		int32_t *len, uint8_t **data)
{
	/* Call the callback */
	if ((req_handlers && req_handlers->custom_handler) &&
		(!req_handlers->custom_handler(pSetup, len, data)))
		return 0;

	return -ENOTSUP;
}

/**
 * @brief Handler called for WebUSB vendor specific commands.
 *
 * @param pSetup    Information about the request to execute.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
int webusb_serial_vendor_handle_req(struct usb_setup_packet *pSetup,
		int32_t *len, uint8_t **data)
{
	/* Call the callback */
	if ((req_handlers && req_handlers->vendor_handler) &&
		(!req_handlers->vendor_handler(pSetup, len, data)))
		return 0;

	return -ENOTSUP;
}

/**
 * @brief Register Custom and Vendor request callbacks
 *
 * This function registers Custom and Vendor request callbacks
 * for handling the device requests.
 *
 * @param [in] handlers Pointer to WebUSB request handlers structure
 *
 * @return N/A
 */
void webusb_register_request_handlers(struct webusb_req_handlers *handlers)
{
	req_handlers = handlers;
}

/**
 * @brief EP Bulk IN handler, used to send data to the Host
 *
 * @param ep        Endpoint address.
 * @param ep_status Endpoint status code.
 *
 * @return  N/A.
 */
static void webusb_serial_bulk_in(uint8_t ep,
		enum usb_dc_ep_cb_status_code ep_status)
{
	struct webusb_serial_dev_data_t * const dev_data =
	    DEV_DATA(webusb_serial_dev);

	dev_data->tx_ready = 1;
	/* Call callback only if tx irq ena */
	if (dev_data->cb && dev_data->tx_irq_ena) {
		dev_data->cb(webusb_serial_dev);
	}
}

/**
 * @brief EP Bulk OUT handler, used to read the data received from the Host
 *
 * @param ep        Endpoint address.
 * @param ep_status Endpoint status code.
 *
 * @return  N/A.
 */
static void webusb_serial_bulk_out(uint8_t ep,
		enum usb_dc_ep_cb_status_code ep_status)
{
	struct webusb_serial_dev_data_t * const dev_data =
	    DEV_DATA(webusb_serial_dev);
	uint32_t bytes_to_read, i, j, buf_head;
	uint8_t tmp_buf[4];

	/* Check how many bytes were received */
	if (usb_read(ep, NULL, 0, &bytes_to_read) != 0) {
		SYS_LOG_ERR("USB read failed");
		return;
	}

	buf_head = dev_data->rx_buf_head;

	/*
	 * Quark SE USB controller is always storing data
	 * in the FIFOs per 32-bit words.
	 */
	for (i = 0; i < bytes_to_read; i += 4) {
		usb_read(ep, tmp_buf, 4, NULL);
		for (j = 0; j < 4; j++) {
			if (i + j == bytes_to_read) {
				/* We read all the data */
				break;
			}

			if (((buf_head + 1) % WEBUSB_BUFFER_SIZE) ==
			    dev_data->rx_buf_tail) {
				/* FIFO full, discard data */
				SYS_LOG_ERR("WebUSB buffer full!");
			} else {
				dev_data->rx_buf[buf_head] = tmp_buf[j];
				buf_head = (buf_head + 1) % WEBUSB_BUFFER_SIZE;
			}
		}
	}

	dev_data->rx_buf_head = buf_head;
	dev_data->rx_ready = 1;
	/* Call callback only if rx irq ena */
	if (dev_data->cb && dev_data->rx_irq_ena) {
		dev_data->cb(webusb_serial_dev);
	}
}

/**
 * @brief EP Interrupt handler
 *
 * @param ep        Endpoint address.
 * @param ep_status Endpoint status code.
 *
 * @return  N/A.
 */
static void webusb_serial_int_in(uint8_t ep,
	enum usb_dc_ep_cb_status_code ep_status)
{
	struct webusb_serial_dev_data_t * const dev_data =
	    DEV_DATA(webusb_serial_dev);

	dev_data->notification_sent = 1;
	SYS_LOG_DBG("CDC_IntIN EP[%x]\r", ep);
}

/**
 * @brief Callback used to know the USB connection status
 *
 * @param status USB device status code.
 *
 * @return  N/A.
 */
static void webusb_serial_dev_status_cb(enum usb_dc_status_code status)
{
	struct webusb_serial_dev_data_t * const dev_data =
	    DEV_DATA(webusb_serial_dev);

	/* Store the new status */
	dev_data->usb_status = status;

	/* Check the USB status and do needed action if required */
	switch (status) {
	case USB_DC_ERROR:
		SYS_LOG_DBG("USB device error");
		break;
	case USB_DC_RESET:
		SYS_LOG_DBG("USB device reset detected");
		break;
	case USB_DC_CONNECTED:
		SYS_LOG_DBG("USB device connected");
		break;
	case USB_DC_CONFIGURED:
		SYS_LOG_DBG("USB device configured");
		break;
	case USB_DC_DISCONNECTED:
		SYS_LOG_DBG("USB device disconnected");
		break;
	case USB_DC_SUSPEND:
		SYS_LOG_DBG("USB device supended");
		break;
	case USB_DC_RESUME:
		SYS_LOG_DBG("USB device resumed");
		break;
	case USB_DC_UNKNOWN:
	default:
		SYS_LOG_DBG("USB unknown state");
		break;
	}
}

/* Describe EndPoints configuration */
static struct usb_ep_cfg_data webusb_serial_ep_data[] = {
	{
		.ep_cb	= webusb_serial_int_in,
		.ep_addr = CDC_ENDP_INT
	},
	{
		.ep_cb	= webusb_serial_bulk_out,
		.ep_addr = CDC_ENDP_OUT
	},
	{
		.ep_cb = webusb_serial_bulk_in,
		.ep_addr = CDC_ENDP_IN
	},
	{
		.ep_cb	= webusb_serial_bulk_out,
		.ep_addr = WEBUSB_ENDP_OUT
	},
	{
		.ep_cb = webusb_serial_bulk_in,
		.ep_addr = WEBUSB_ENDP_IN
	}
};

/* Configuration of the CDC-ACM Device send to the USB Driver */
static struct usb_cfg_data webusb_serial_config = {
	.usb_device_description = webusb_serial_usb_description,
	.cb_usb_status = webusb_serial_dev_status_cb,
	.interface = {
		.class_handler = webusb_serial_class_handle_req,
		.custom_handler = webusb_serial_custom_handle_req,
		.vendor_handler = webusb_serial_vendor_handle_req,
		.payload_data = NULL,
	},
	.num_endpoints = CDC1_NUM_EP + CDC2_NUM_EP + WEBUSB_NUM_EP,
	.endpoint = webusb_serial_ep_data
};

/**
 * @brief Set the baud rate
 *
 * This routine set the given baud rate for the UART.
 *
 * @param dev             WebUSB Serial device struct.
 * @param baudrate        Baud rate.
 *
 * @return N/A.
 */
static void webusb_serial_baudrate_set(struct device *dev, uint32_t baudrate)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->line_coding.dwDTERate = sys_cpu_to_le32(baudrate);
}

/**
 * @brief Initialize UART channel
 *
 * This routine is called to reset the chip in a quiescent state.
 * It is assumed that this function is called only once per UART.
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return 0 on success, negative errno code on fail
 */
static int webusb_serial_init(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);
	int ret;

	webusb_serial_config.interface.payload_data = dev_data->interface_data;
	webusb_serial_dev = dev;

	/* Initialize the WebUSB driver with the right configuration */
	ret = usb_set_config(&webusb_serial_config);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to config USB");
		return ret;
	}

	/* Enable WebUSB driver */
	ret = usb_enable(&webusb_serial_config);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to enable USB");
		return ret;
	}

	dev->driver_api = &webusb_serial_driver_api;

	return 0;
}

/**
 * @brief Fill FIFO with data
 *
 * @param dev     WebUSB Serial device struct.
 * @param tx_data Data to transmit.
 * @param len     Number of bytes to send.
 *
 * @return Number of bytes sent.
 */
static int webusb_serial_fifo_fill(struct device *dev,
		const uint8_t *tx_data, int len)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);
	uint32_t bytes_written = 0;

	if (dev_data->usb_status != USB_DC_CONFIGURED) {
		return 0;
	}

	dev_data->tx_ready = 0;
	if (usb_write(WEBUSB_ENDP_IN, tx_data, len, &bytes_written) != 0) {
		SYS_LOG_ERR("USB write failed\n");
	}

	return bytes_written;
}

/**
 * @brief Read data from FIFO
 *
 * @param dev     WebUSB Serial device struct.
 * @param rx_data Pointer to data container.
 * @param size    Container size.
 *
 * @return Number of bytes read.
 */
static int webusb_serial_fifo_read(struct device *dev, uint8_t *rx_data,
		const int size)
{
	uint32_t avail_data, bytes_read, i;
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	avail_data = (WEBUSB_BUFFER_SIZE + dev_data->rx_buf_head -
	    dev_data->rx_buf_tail) % WEBUSB_BUFFER_SIZE;
	if (avail_data > size) {
		bytes_read = size;
	} else {
		bytes_read = avail_data;
	}

	for (i = 0; i < bytes_read; i++) {
		rx_data[i] = dev_data->rx_buf[(dev_data->rx_buf_tail + i) %
		    WEBUSB_BUFFER_SIZE];
	}

	dev_data->rx_buf_tail = (dev_data->rx_buf_tail + bytes_read) %
	    WEBUSB_BUFFER_SIZE;

	if (dev_data->rx_buf_tail == dev_data->rx_buf_head) {
		/* Buffer empty */
		dev_data->rx_ready = 0;
	}

	return bytes_read;
}

/**
 * @brief Enable TX interrupt
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return N/A.
 */
static void webusb_serial_irq_tx_enable(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->tx_irq_ena = 1;
}

/**
 * @brief Disable TX interrupt
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return N/A.
 */
static void webusb_serial_irq_tx_disable(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->tx_irq_ena = 0;
}

/**
 * @brief Check if Tx IRQ has been raised
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return 1 if a Tx IRQ is pending, 0 otherwise.
 */
static int webusb_serial_irq_tx_ready(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	if (dev_data->tx_ready) {
		dev_data->tx_ready = 0;
		return 1;
	}

	return 0;
}

/**
 * @brief Enable RX interrupt
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return N/A
 */
static void webusb_serial_irq_rx_enable(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->rx_irq_ena = 1;
}

/**
 * @brief Disable RX interrupt
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return N/A.
 */
static void webusb_serial_irq_rx_disable(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->rx_irq_ena = 0;
}

/**
 * @brief Check if Rx IRQ has been raised
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return 1 if an IRQ is ready, 0 otherwise.
 */
static int webusb_serial_irq_rx_ready(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	if (dev_data->rx_ready) {
		dev_data->rx_ready = 0;
		return 1;
	}

	return 0;
}

/**
 * @brief Check if Tx or Rx IRQ is pending
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return 1 if a Tx or Rx IRQ is pending, 0 otherwise.
 */
static int webusb_serial_irq_is_pending(struct device *dev)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	if (dev_data->tx_ready || dev_data->rx_ready) {
		return 1;
	}

	return 0;
}

/**
 * @brief Update IRQ status
 *
 * @param dev WebUSB Serial device struct.
 *
 * @return Always 1
 */
static int webusb_serial_irq_update(struct device *dev)
{
	return 1;
}

/**
 * @brief Set the callback function pointer for IRQ.
 *
 * @param dev WebUSB Serial device struct.
 * @param cb  Callback function pointer.
 *
 * @return N/A
 */
static void webusb_serial_irq_callback_set(struct device *dev,
		uart_irq_callback_t cb)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	dev_data->cb = cb;
}

#ifdef CONFIG_UART_LINE_CTRL

/**
 * @brief Send serial line state notification to the Host
 *
 * This routine sends asynchronous notification of UART status
 * on the interrupt endpoint
 *
 * @param dev WebUSB Serial device struct.
 * @param ep_status Endpoint status code.
 *
 * @return 0 if successful, failed otherwise.
 */
static int webusb_serial_send_notification(struct device *dev,
		uint16_t serial_state)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);
	struct cdc_acm_notification notification;
	uint32_t cnt = 0;

	notification.bmRequestType = 0xA1;
	notification.bNotificationType = 0x20;
	notification.wValue = 0;
	notification.wIndex = 0;
	notification.wLength = sys_cpu_to_le16(sizeof(serial_state));
	notification.data = sys_cpu_to_le16(serial_state);

	dev_data->notification_sent = 0;
	usb_write(CDC_ENDP_INT, (const uint8_t *)&notification,
	    sizeof(notification), NULL);

	/* Wait for notification to be sent */
	while (!((volatile uint8_t)dev_data->notification_sent)) {
		k_busy_wait(1);
		if (++cnt > CDC_CONTROL_SERIAL_STATE_TIMEOUT_US) {
			SYS_LOG_DBG("WebUSB notification timeout!");
			return -EIO;
		}
	}

	return 0;
}

/**
 * @brief Manipulate line control for UART.
 *
 * @param dev WebUSB Serial device struct
 * @param ctrl The line control to be manipulated
 * @param val Value to set the line control
 *
 * @return 0 if successful, failed otherwise.
 */
static int webusb_serial_line_ctrl_set(struct device *dev,
		uint32_t ctrl, uint32_t val)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	switch (ctrl) {
	case LINE_CTRL_BAUD_RATE:
		webusb_serial_baudrate_set(dev, val);
		return 0;
	case LINE_CTRL_DCD:
		dev_data->serial_state &= ~CDC_CONTROL_SERIAL_STATE_DCD;
		if (val)
			dev_data->serial_state |= CDC_CONTROL_SERIAL_STATE_DCD;

		webusb_serial_send_notification(dev,
		    CDC_CONTROL_SERIAL_STATE_DCD);
		return 0;
	case LINE_CTRL_DSR:
		dev_data->serial_state &= ~CDC_CONTROL_SERIAL_STATE_DSR;
		if (val)
			dev_data->serial_state |= CDC_CONTROL_SERIAL_STATE_DSR;

		webusb_serial_send_notification(dev, dev_data->serial_state);
		return 0;
	default:
		return -ENODEV;
	}

	return -ENOTSUP;
}

/**
 * @brief Manipulate line control for UART.
 *
 * @param dev WebUSB Serial device struct
 * @param ctrl The line control to be manipulated
 * @param val Value to set the line control
 *
 * @return 0 if successful, failed otherwise.
 */
static int webusb_serial_line_ctrl_get(struct device *dev,
		uint32_t ctrl, uint32_t *val)
{
	struct webusb_serial_dev_data_t * const dev_data = DEV_DATA(dev);

	switch (ctrl) {
	case LINE_CTRL_BAUD_RATE:
		*val = sys_le32_to_cpu(dev_data->line_coding.dwDTERate);
		return 0;
	case LINE_CTRL_RTS:
		*val =
		    (dev_data->line_state & CDC_CONTROL_LINE_STATE_RTS) ? 1 : 0;
		return 0;
	case LINE_CTRL_DTR:
		*val =
		    (dev_data->line_state & CDC_CONTROL_LINE_STATE_DTR) ? 1 : 0;
		return 0;
	}

	return -ENOTSUP;
}

#endif /* CONFIG_UART_LINE_CTRL */

/*
 * @brief Poll the device for input.
 *
 * @return -ENOTSUP Since underlying USB device controller always uses
 * interrupts, polled mode UART APIs are not implemented for the UART interface
 * exported by CDC ACM driver. Apps should use fifo_read API instead.
 */

static int webusb_serial_poll_in(struct device *dev, unsigned char *c)
{
	return -ENOTSUP;
}

/*
 * @brief Output a character in polled mode.
 *
 * @return 0 Since it is not supported. See the comments of
 * webusb_serial_poll_in() for details. Apps should use fifo_fill API instead.
 */
static unsigned char webusb_serial_poll_out(struct device *dev,
		unsigned char c)
{
	return 0;
}

static const struct uart_driver_api webusb_serial_driver_api = {
	.poll_in = webusb_serial_poll_in,
	.poll_out = webusb_serial_poll_out,
	.fifo_fill = webusb_serial_fifo_fill,
	.fifo_read = webusb_serial_fifo_read,
	.irq_tx_enable = webusb_serial_irq_tx_enable,
	.irq_tx_disable = webusb_serial_irq_tx_disable,
	.irq_tx_ready = webusb_serial_irq_tx_ready,
	.irq_rx_enable = webusb_serial_irq_rx_enable,
	.irq_rx_disable = webusb_serial_irq_rx_disable,
	.irq_rx_ready = webusb_serial_irq_rx_ready,
	.irq_is_pending = webusb_serial_irq_is_pending,
	.irq_update = webusb_serial_irq_update,
	.irq_callback_set = webusb_serial_irq_callback_set,
#ifdef CONFIG_UART_LINE_CTRL
	.line_ctrl_set = webusb_serial_line_ctrl_set,
	.line_ctrl_get = webusb_serial_line_ctrl_get,
#endif /* CONFIG_UART_LINE_CTRL */
};

static struct webusb_serial_dev_data_t webusb_serial_dev_data = {
	.usb_status = USB_DC_UNKNOWN,
	.line_coding = DEFAUL_BAUDRATE,
};

DEVICE_INIT(webusb_serial, WEBUSB_SERIAL_PORT_NAME, &webusb_serial_init,
			&webusb_serial_dev_data, NULL,
			APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
