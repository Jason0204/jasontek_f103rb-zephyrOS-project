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

/**
 * @file
 * @brief Designware USB device controller driver private definitions
 *
 * This file contains the Designware USB device controller driver private
 * definitions.
 */

#ifndef __USB_DW_REGISTERS_H__
#define __USB_DW_REGISTERS_H__

#include <misc/util.h>
#include <board.h>

#if defined(CONFIG_SOC_QUARK_SE_C1000)
#include "qm_soc_regs.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Number of USB controllers */
enum USB_DW { USB_DW_0 = 0, USB_DW_NUM };

/* USB IN EP index */
enum usb_dw_in_ep_idx {
	USB_DW_IN_EP_0 = 0,
	USB_DW_IN_EP_1,
	USB_DW_IN_EP_2,
	USB_DW_IN_EP_3,
	USB_DW_IN_EP_4,
	USB_DW_IN_EP_5,
	USB_DW_IN_EP_NUM
};

/* USB OUT EP index */
enum usb_dw_out_ep_idx {
	USB_DW_OUT_EP_0 = 0,
	USB_DW_OUT_EP_1,
	USB_DW_OUT_EP_2,
	USB_DW_OUT_EP_3,
	USB_DW_OUT_EP_NUM
};

/* USB IN EP Register block type */
struct usb_dw_in_ep_reg {
	volatile uint32_t diepctl;
	uint32_t reserved;
	volatile uint32_t diepint;
	uint32_t reserved1;
	volatile uint32_t dieptsiz;
	volatile uint32_t diepdma;
	volatile uint32_t dtxfsts;
	uint32_t reserved2;
};

/* USB OUT EP Register block type */
struct usb_dw_out_ep_reg {
	volatile uint32_t doepctl;
	uint32_t reserved;
	volatile uint32_t doepint;
	uint32_t reserved1;
	volatile uint32_t doeptsiz;
	volatile uint32_t doepdma;
	uint32_t reserved2;
	uint32_t reserved3;
};

/* USB Register block type */
struct usb_dw_reg {
	volatile uint32_t gotgctl;
	volatile uint32_t gotgint;
	volatile uint32_t gahbcfg;
	volatile uint32_t gusbcfg;
	volatile uint32_t grstctl;
	volatile uint32_t gintsts;
	volatile uint32_t gintmsk;
	volatile uint32_t grxstsr;
	volatile uint32_t grxstsp;
	volatile uint32_t grxfsiz;
	volatile uint32_t gnptxfsiz;
	uint32_t reserved[5];
	volatile uint32_t gsnpsid;
	volatile uint32_t ghwcfg1;
	volatile uint32_t ghwcfg2;
	volatile uint32_t ghwcfg3;
	volatile uint32_t ghwcfg4;
	volatile uint32_t gdfifocfg;
	uint32_t reserved1[43];
	volatile uint32_t dieptxf1;
	volatile uint32_t dieptxf2;
	volatile uint32_t dieptxf3;
	volatile uint32_t dieptxf4;
	volatile uint32_t dieptxf5;
	uint32_t reserved2[442];
	volatile uint32_t dcfg;
	volatile uint32_t dctl;
	volatile uint32_t dsts;
	uint32_t reserved3;
	volatile uint32_t diepmsk;
	volatile uint32_t doepmsk;
	volatile uint32_t daint;
	volatile uint32_t daintmsk;
	uint32_t reserved4[2];
	volatile uint32_t dvbusdis;
	volatile uint32_t dvbuspulse;
	volatile uint32_t dthrctl;
	volatile uint32_t diepempmsk;
	uint32_t reserved5[50];
	struct usb_dw_in_ep_reg in_ep_reg[USB_DW_IN_EP_NUM];
	uint32_t reserved6[80];
	struct usb_dw_out_ep_reg out_ep_reg[USB_DW_OUT_EP_NUM];
};

/* USB register offsets and masks */
#define USB_DW_GRSTCTL_AHB_IDLE BIT(31)
#define USB_DW_GRSTCTL_TX_FNUM_OFFSET (6)
#define USB_DW_GRSTCTL_TX_FFLSH BIT(5)
#define USB_DW_GRSTCTL_C_SFT_RST BIT(0)
#define USB_DW_GAHBCFG_DMA_EN BIT(5)
#define USB_DW_GAHBCFG_GLB_INTR_MASK BIT(0)
#define USB_DW_DCTL_SFT_DISCON BIT(1)
#define USB_DW_GINTSTS_WK_UP_INT BIT(31)
#define USB_DW_GINTSTS_OEP_INT BIT(19)
#define USB_DW_GINTSTS_IEP_INT BIT(18)
#define USB_DW_GINTSTS_ENUM_DONE BIT(13)
#define USB_DW_GINTSTS_USB_RST BIT(12)
#define USB_DW_GINTSTS_USB_SUSP BIT(11)
#define USB_DW_GINTSTS_RX_FLVL BIT(4)
#define USB_DW_GINTSTS_OTG_INT BIT(2)
#define USB_DW_DCFG_DEV_SPD_LS (0x2)
#define USB_DW_DCFG_DEV_SPD_FS (0x3)
#define USB_DW_DCFG_DEV_ADDR_MASK (0x7F << 4)
#define USB_DW_DCFG_DEV_ADDR_OFFSET (4)
#define USB_DW_DAINT_IN_EP_INT(ep) (1 << (ep))
#define USB_DW_DAINT_OUT_EP_INT(ep) (0x10000 << (ep))
#define USB_DW_DEPCTL_EP_ENA BIT(31)
#define USB_DW_DEPCTL_EP_DIS BIT(30)
#define USB_DW_DEPCTL_SETDOPID BIT(28)
#define USB_DW_DEPCTL_SNAK BIT(27)
#define USB_DW_DEPCTL_CNAK BIT(26)
#define USB_DW_DEPCTL_STALL BIT(21)
#define USB_DW_DEPCTL_EP_TYPE_MASK (0x3 << 18)
#define USB_DW_DEPCTL_EP_TYPE_OFFSET (18)
#define USB_DW_DEPCTL_EP_TYPE_CONTROL (0)
#define USB_DW_DEPCTL_EP_TYPE_ISO (0x1)
#define USB_DW_DEPCTL_EP_TYPE_BULK (0x2)
#define USB_DW_DEPCTL_EP_TYPE_INTERRUPT (0x3)
#define USB_DW_DEPCTL_USB_ACT_EP BIT(15)
#define USB_DW_DEPCTL0_MSP_MASK (0x3)
#define USB_DW_DEPCTL0_MSP_8 (0x3)
#define USB_DW_DEPCTL0_MSP_16 (0x2)
#define USB_DW_DEPCTL0_MSP_32 (0x1)
#define USB_DW_DEPCTL0_MSP_64 (0)
#define USB_DW_DEPCTLn_MSP_MASK (0x3FF)
#define USB_DW_DEPCTL_MSP_OFFSET (0)
#define USB_DW_DOEPTSIZ_SUP_CNT_MASK (0x3 << 29)
#define USB_DW_DOEPTSIZ_SUP_CNT_OFFSET (29)
#define USB_DW_DOEPTSIZ0_PKT_CNT_MASK (0x1 << 19)
#define USB_DW_DOEPTSIZn_PKT_CNT_MASK (0x3FF << 19)
#define USB_DW_DIEPTSIZ0_PKT_CNT_MASK (0x3 << 19)
#define USB_DW_DIEPTSIZn_PKT_CNT_MASK (0x3FF << 19)
#define USB_DW_DEPTSIZ_PKT_CNT_OFFSET (19)
#define USB_DW_DEPTSIZ0_XFER_SIZE_MASK (0x7F)
#define USB_DW_DEPTSIZn_XFER_SIZE_MASK (0x7FFFF)
#define USB_DW_DEPTSIZ_XFER_SIZE_OFFSET (0)
#define USB_DW_DIEPINT_XFER_COMPL BIT(0)
#define USB_DW_DIEPINT_TX_FEMP BIT(7)
#define USB_DW_DIEPINT_XFER_COMPL BIT(0)
#define USB_DW_DOEPINT_SET_UP BIT(3)
#define USB_DW_DOEPINT_XFER_COMPL BIT(0)
#define USB_DW_DSTS_ENUM_SPD_MASK (0x3)
#define USB_DW_DSTS_ENUM_SPD_OFFSET (1)
#define USB_DW_DSTS_ENUM_LS (2)
#define USB_DW_DSTS_ENUM_FS (3)
#define USB_DW_GRXSTSR_EP_NUM_MASK (0xF << 0)
#define USB_DW_GRXSTSR_PKT_STS_MASK (0xF << 17)
#define USB_DW_GRXSTSR_PKT_STS_OFFSET (17)
#define USB_DW_GRXSTSR_PKT_CNT_MASK (0x7FF << 4)
#define USB_DW_GRXSTSR_PKT_CNT_OFFSET (4)
#define USB_DW_GRXSTSR_PKT_STS_OUT_DATA (2)
#define USB_DW_GRXSTSR_PKT_STS_OUT_DATA_DONE (3)
#define USB_DW_GRXSTSR_PKT_STS_SETUP_DONE (4)
#define USB_DW_GRXSTSR_PKT_STS_SETUP (6)
#define USB_DW_DTXFSTS_TXF_SPC_AVAIL_MASK (0xFFFF)

#define USB_DW_CORE_RST_TIMEOUT_US 10000
#define USB_DW_PLL_TIMEOUT_US 100

#if defined(CONFIG_SOC_QUARK_SE_C1000)
#define USB_DW_BASE QM_USB_0_BASE
#define USB_DW_IRQ QM_IRQ_USB_0_INT
#else
#error "Unsupported board"
#endif

#define USB_DW_EP_FIFO(ep) (*(uint32_t *)(USB_DW_BASE + 0x1000 * (ep + 1)))
/* USB register block base address */
#define USB_DW ((struct usb_dw_reg *)USB_DW_BASE)

#ifdef __cplusplus
}
#endif

#endif /* __USB_DW_REGISTERS_H__ */
