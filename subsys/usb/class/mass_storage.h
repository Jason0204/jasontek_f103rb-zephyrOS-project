/*
 * Certain structures and defines in this file are from  mbed's implementation.
 *
 * Copyright (c) 2010-2011 mbed.org, MIT License
 * Copyright (c) 2016 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file
 * @brief Mass Storage device class driver header file
 *
 * Header file for USB Mass Storage device class driver
 */

#ifndef __MASS_STORAGE_H__
#define __MASS_STORAGE_H__

/* Bulk-only Command Block Wrapper (CBW) */
struct CBW {
	uint32_t Signature;
	uint32_t Tag;
	uint32_t DataLength;
	uint8_t  Flags;
	uint8_t  LUN;
	uint8_t  CBLength;
	uint8_t  CB[16];
} __packed;

/* Bulk-only Command Status Wrapper (CSW) */
struct CSW {
	uint32_t Signature;
	uint32_t Tag;
	uint32_t DataResidue;
	uint8_t  Status;
} __packed;

/* Intel vendor ID */
#define MASS_STORAGE_VENDOR_ID	0x8086

/* Product Id, random value */
#define MASS_STORAGE_PRODUCT_ID	0xF8A1

/* Max packet size for Bulk endpoints */
#define MASS_STORAGE_BULK_EP_MPS		64

/* Number of configurations for the USB Device */
#define MASS_NUM_CONF	0x01
/* Number of interfaces in the configuration */
#define MASS_NUM_ITF		0x01

#define EPBULK_OUT	0x03
#define EPBULK_IN		0x84

/* Size in bytes of the configuration sent to
 * the Host on GetConfiguration() request
 * For Mass Storage Device: CONF + (1 x ITF) + (2 x EP)
 */
#define MASS_CONF_SIZE   (USB_CONFIGURATION_DESC_SIZE + \
	(1 * USB_INTERFACE_DESC_SIZE) + (2 * USB_ENDPOINT_DESC_SIZE))

#define CBW_Signature   0x43425355
#define CSW_Signature   0x53425355

/*SCSI Commands*/
#define TEST_UNIT_READY            0x00
#define REQUEST_SENSE              0x03
#define FORMAT_UNIT                0x04
#define INQUIRY                    0x12
#define MODE_SELECT6               0x15
#define MODE_SENSE6                0x1A
#define START_STOP_UNIT            0x1B
#define MEDIA_REMOVAL              0x1E
#define READ_FORMAT_CAPACITIES     0x23
#define READ_CAPACITY              0x25
#define READ10                     0x28
#define WRITE10                    0x2A
#define VERIFY10                   0x2F
#define READ12                     0xA8
#define WRITE12                    0xAA
#define MODE_SELECT10              0x55
#define MODE_SENSE10               0x5A

/*MSC class specific requests*/
#define MSC_REQUEST_RESET          0xFF
#define MSC_REQUEST_GET_MAX_LUN    0xFE

#define THREAD_OP_READ_QUEUED		1
#define THREAD_OP_WRITE_QUEUED		3
#define THREAD_OP_WRITE_DONE		4

#endif /* __MASS_STORAGE_H__ */
