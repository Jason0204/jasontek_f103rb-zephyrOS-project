/*
 * Copyright (c) 2016 Intel Corporation.
 * Copyright (c) 2013-2015 Wind River Systems, Inc.
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

/**
 * @brief Driver for UART on Atmel SAM3 family processor.
 *
 * Note that there is only one UART controller on the SoC.
 * It has two wires for RX and TX, and does not have other such as
 * CTS or RTS. Also, the RX and TX are connected directly to
 * bit shifters and there is no FIFO.
 *
 * For full serial function, use the USART controller.
 *
 * (used uart_stellaris.c as template)
 */

#include <kernel.h>
#include <arch/cpu.h>
#include <misc/__assert.h>
#include <board.h>
#include <init.h>
#include <uart.h>
#include <sections.h>

/* UART registers struct */
struct _uart {
	/* UART registers */
	uint32_t	cr;	/* 0x00 Control Register */
	uint32_t	mr;	/* 0x04 Mode Register */
	uint32_t	ier;	/* 0x08 Interrupt Enable Register */
	uint32_t	idr;	/* 0x0C Interrupt Disable Register */
	uint32_t	imr;	/* 0x10 Interrupt Mask Register */
	uint32_t	sr;	/* 0x14 Status Register */
	uint32_t	rhr;	/* 0x18 Receive Holding Register */
	uint32_t	thr;	/* 0x1C Transmit Holding Register */
	uint32_t	brgr;	/* 0x20 Baud Rate Generator Register */

	uint32_t	reserved[55];	/* 0x24 - 0xFF */

	/* PDC related registers */
	uint32_t	pdc_rpr;	/* 0x100 Receive Pointer Reg */
	uint32_t	pdc_rcr;	/* 0x104 Receive Counter Reg */
	uint32_t	pdc_tpr;	/* 0x108 Transmit Pointer Reg */
	uint32_t	pdc_tcr;	/* 0x10C Transmit Counter Reg */
	uint32_t	pdc_rnpr;	/* 0x110 Receive Next Pointer */
	uint32_t	pdc_rncr;	/* 0x114 Receive Next Counter */
	uint32_t	pdc_tnpr;	/* 0x118 Transmit Next Pointer */
	uint32_t	pdc_tncr;	/* 0x11C Transmit Next Counter */
	uint32_t	pdc_ptcr;	/* 0x120 Transfer Control Reg */
	uint32_t	pdc_ptsr;	/* 0x124 Transfer Status Reg */
};

/* Device data structure */
struct uart_sam3_dev_data_t {
	uint32_t baud_rate;	/* Baud rate */
};

/* convenience defines */
#define DEV_CFG(dev) \
	((const struct uart_device_config * const)(dev)->config->config_info)
#define DEV_DATA(dev) \
	((struct uart_sam3_dev_data_t * const)(dev)->driver_data)
#define UART_STRUCT(dev) \
	((volatile struct _uart *)(DEV_CFG(dev))->base)

/* Registers */
#define UART_CR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x00)))
#define UART_MR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x04)))
#define UART_IER(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x08)))
#define UART_IDR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x0C)))
#define UART_IMR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x10)))
#define UART_SR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x14)))
#define UART_RHR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x18)))
#define UART_THR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x1C)))
#define UART_BRGR(dev)	(*((volatile uint32_t *)(DEV_CFG(dev)->base + 0x20)))

/* bits */
#define UART_CR_RSTRX	(1 << 2)
#define UART_CR_RSTTX	(1 << 3)
#define UART_CR_RXEN	(1 << 4)
#define UART_CR_RXDIS	(1 << 5)
#define UART_CR_TXEN	(1 << 6)
#define UART_CR_TXDIS	(1 << 7)
#define UART_CR_RSTSTA	(1 << 8)

#define UART_MR_PARTIY_MASK	(0x0E00)
#define UART_MR_PARITY_EVEN	(0 << 9)
#define UART_MR_PARITY_ODD	(1 << 9)
#define UART_MR_PARITY_SPACE	(2 << 9)
#define UART_MR_PARITY_MARK	(3 << 9)
#define UART_MR_PARITY_NO	(4 << 9)

#define UART_MR_CHMODE_MASK		(0xC000)
#define UART_MR_CHMODE_NORMAL		(0 << 14)
#define UART_MR_CHMODE_AUTOMATIC	(1 << 14)
#define UART_MR_CHMODE_LOCAL_LOOPBACK	(2 << 14)
#define UART_MR_CHMODE_REMOTE_LOOPBACK	(3 << 14)

#define UART_INT_RXRDY		(1 << 0)
#define UART_INT_TXRDY		(1 << 1)
#define UART_INT_ENDRX		(1 << 3)
#define UART_INT_ENDTX		(1 << 4)
#define UART_INT_OVRE		(1 << 5)
#define UART_INT_FRAME		(1 << 6)
#define UART_INT_PARE		(1 << 7)
#define UART_INT_TXEMPTY	(1 << 9)
#define UART_INT_TXBUFE		(1 << 11)
#define UART_INT_RXBUFF		(1 << 12)

#define UART_PDC_PTCR_RXTDIS	(1 << 1)
#define UART_PDC_PTCR_TXTDIS	(1 << 9)

static const struct uart_driver_api uart_sam3_driver_api;

/**
 * @brief Set the baud rate
 *
 * This routine set the given baud rate for the UART.
 *
 * @param dev UART device struct
 * @param baudrate Baud rate
 * @param sys_clk_freq_hz System clock frequency in Hz
 *
 * @return N/A
 */
static void baudrate_set(struct device *dev,
			 uint32_t baudrate, uint32_t sys_clk_freq_hz)
{
	volatile struct _uart *uart = UART_STRUCT(dev);
	const struct uart_device_config * const dev_cfg = DEV_CFG(dev);
	struct uart_sam3_dev_data_t * const dev_data = DEV_DATA(dev);
	uint32_t divisor; /* baud rate divisor */

	ARG_UNUSED(sys_clk_freq_hz);

	if ((baudrate != 0) && (dev_cfg->sys_clk_freq != 0)) {
		/* calculate baud rate divisor */
		divisor = (dev_cfg->sys_clk_freq / baudrate) >> 4;
		divisor &= 0xFFFF;

		uart->brgr = divisor;

		dev_data->baud_rate = baudrate;
	}
}

/**
 * @brief Initialize UART channel
 *
 * This routine is called to reset the chip in a quiescent state.
 * It is assumed that this function is called only once per UART.
 *
 * @param dev UART device struct
 *
 * @return 0
 */
static int uart_sam3_init(struct device *dev)
{
	volatile struct _uart *uart = UART_STRUCT(dev);

	/* Enable UART clock in PMC */
	__PMC->pcer0 = (1 << PID_UART);

	/* Detach pins PA8 and PA9 from PIO controller */
	__PIOA->pdr = (1 << 8) | (1 << 9);

	/* Disable PDC (DMA) */
	uart->pdc_ptcr = UART_PDC_PTCR_RXTDIS | UART_PDC_PTCR_TXTDIS;

	/* Reset and disable UART */
	uart->cr = UART_CR_RSTRX | UART_CR_RSTTX
		   | UART_CR_RXDIS | UART_CR_TXDIS | UART_CR_RSTSTA;

	/* No parity and normal mode */
	uart->mr = UART_MR_PARITY_NO | UART_MR_CHMODE_NORMAL;

	/* Set baud rate */
	baudrate_set(dev, DEV_DATA(dev)->baud_rate,
		     DEV_CFG(dev)->sys_clk_freq);

	/* Enable receiver and transmitter */
	uart->cr = UART_CR_RXEN | UART_CR_TXEN;

	return 0;
}

/**
 * @brief Poll the device for input.
 *
 * @param dev UART device struct
 * @param c Pointer to character
 *
 * @return 0 if a character arrived, -1 if the input buffer if empty.
 */

static int uart_sam3_poll_in(struct device *dev, unsigned char *c)
{
	volatile struct _uart *uart = UART_STRUCT(dev);

	if (!(uart->sr & UART_INT_RXRDY))
		return (-1);

	/* got a character */
	*c = (unsigned char)uart->rhr;

	return 0;
}

/**
 * @brief Output a character in polled mode.
 *
 * Checks if the transmitter is empty. If empty, a character is written to
 * the data register.
 *
 * @param dev UART device struct
 * @param c Character to send
 *
 * @return Sent character
 */
static unsigned char uart_sam3_poll_out(struct device *dev,
					     unsigned char c)
{
	volatile struct _uart *uart = UART_STRUCT(dev);

	/* Wait for transmitter to be ready */
	while (!(uart->sr & UART_INT_TXRDY))
		;

	/* send a character */
	uart->thr = (uint32_t)c;
	return c;
}

static const struct uart_driver_api uart_sam3_driver_api = {
	.poll_in = uart_sam3_poll_in,
	.poll_out = uart_sam3_poll_out,
};

static const struct uart_device_config uart_sam3_dev_cfg_0 = {
	.base = (uint8_t *)UART_ADDR,
	.sys_clk_freq = CONFIG_UART_ATMEL_SAM3_CLK_FREQ,
};

static struct uart_sam3_dev_data_t uart_sam3_dev_data_0 = {
	.baud_rate = CONFIG_UART_ATMEL_SAM3_BAUD_RATE,
};

DEVICE_AND_API_INIT(uart_sam3_0, CONFIG_UART_ATMEL_SAM3_NAME, &uart_sam3_init,
		    &uart_sam3_dev_data_0, &uart_sam3_dev_cfg_0,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_sam3_driver_api);
