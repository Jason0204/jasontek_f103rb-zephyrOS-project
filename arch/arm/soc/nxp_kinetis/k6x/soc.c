/*
 * Copyright (c) 2014-2015 Wind River Systems, Inc.
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
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
 * @brief System/hardware module for fsl_frdm_k64f platform
 *
 * This module provides routines to initialize and support board-level
 * hardware for the fsl_frdm_k64f platform.
 */

#include <nanokernel.h>
#include <device.h>
#include <init.h>
#include <soc.h>
#include <uart.h>
#include <sections.h>
#include <fsl_common.h>
#include <fsl_clock.h>
#include <arch/cpu.h>

#define PLLFLLSEL_MCGFLLCLK	(0)
#define PLLFLLSEL_MCGPLLCLK	(1)
#define PLLFLLSEL_IRC48MHZ	(3)

#define ER32KSEL_OSC32KCLK	(0)
#define ER32KSEL_RTC		(2)
#define ER32KSEL_LPO1KHZ	(3)

#define TIMESRC_OSCERCLK        (2)

/*
 * K64F Flash configuration fields
 * These 16 bytes, which must be loaded to address 0x400, include default
 * protection and security settings.
 * They are loaded at reset to various Flash Memory module (FTFE) registers.
 *
 * The structure is:
 * -Backdoor Comparison Key for unsecuring the MCU - 8 bytes
 * -Program flash protection bytes, 4 bytes, written to FPROT0-3
 * -Flash security byte, 1 byte, written to FSEC
 * -Flash nonvolatile option byte, 1 byte, written to FOPT
 * -Reserved, 1 byte, (Data flash protection byte for FlexNVM)
 * -Reserved, 1 byte, (EEPROM protection byte for FlexNVM)
 *
 */
uint8_t __security_frdm_k64f_section __security_frdm_k64f[] = {
	/* Backdoor Comparison Key (unused) */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* Program flash protection; 1 bit/region - 0=protected, 1=unprotected
	 */
	0xFF, 0xFF, 0xFF, 0xFF,
	/*
	 * Flash security: Backdoor key disabled, Mass erase enabled,
	 *                 Factory access enabled, MCU is unsecure
	 */
	0xFE,
	/* Flash nonvolatile option: NMI enabled, EzPort enabled, Normal boot */
	0xFF,
	/* Reserved for FlexNVM feature (unsupported by this MCU) */
	0xFF, 0xFF};

static const osc_config_t oscConfig = {
	.freq = CONFIG_OSC_XTAL0_FREQ,
	.capLoad = 0,

#if defined(CONFIG_OSC_EXTERNAL)
	.workMode = kOSC_ModeExt,
#elif defined(CONFIG_OSC_LOW_POWER)
	.workMode = kOSC_ModeOscLowPower,
#elif defined(CONFIG_OSC_HIGH_GAIN)
	.workMode = kOSC_ModeOscHighGain,
#else
#error "An oscillator mode must be defined"
#endif

	.oscerConfig = {
		.enableMode = kOSC_ErClkEnable,
#if (defined(FSL_FEATURE_OSC_HAS_EXT_REF_CLOCK_DIVIDER) && \
	FSL_FEATURE_OSC_HAS_EXT_REF_CLOCK_DIVIDER)
		.erclkDiv = 0U,
#endif
	},
};

static const mcg_pll_config_t pll0Config = {
	.enableMode = 0U,
	.prdiv = CONFIG_MCG_PRDIV0,
	.vdiv = CONFIG_MCG_VDIV0,
};

static const sim_clock_config_t simConfig = {
	.pllFllSel = PLLFLLSEL_MCGPLLCLK, /* PLLFLLSEL select PLL. */
	.er32kSrc = ER32KSEL_RTC,         /* ERCLK32K selection, use RTC. */
	.clkdiv1 = SIM_CLKDIV1_OUTDIV1(CONFIG_K64_CORE_CLOCK_DIVIDER - 1) |
		   SIM_CLKDIV1_OUTDIV2(CONFIG_K64_BUS_CLOCK_DIVIDER - 1) |
		   SIM_CLKDIV1_OUTDIV3(CONFIG_K64_FLEXBUS_CLOCK_DIVIDER - 1) |
		   SIM_CLKDIV1_OUTDIV4(CONFIG_K64_FLASH_CLOCK_DIVIDER - 1),
};

/**
 *
 * @brief Initialize the system clock
 *
 * This routine will configure the multipurpose clock generator (MCG) to
 * set up the system clock.
 * The MCG has nine possible modes, including Stop mode.  This routine assumes
 * that the current MCG mode is FLL Engaged Internal (FEI), as from reset.
 * It transitions through the FLL Bypassed External (FBE) and
 * PLL Bypassed External (PBE) modes to get to the desired
 * PLL Engaged External (PEE) mode and generate the maximum 120 MHz system
 * clock.
 *
 * @return N/A
 *
 */
static ALWAYS_INLINE void clkInit(void)
{
	CLOCK_SetSimSafeDivs();

	CLOCK_InitOsc0(&oscConfig);
	CLOCK_SetXtal0Freq(CONFIG_OSC_XTAL0_FREQ);

	CLOCK_BootToPeeMode(kMCG_OscselOsc, kMCG_PllClkSelPll0, &pll0Config);

	CLOCK_SetInternalRefClkConfig(kMCG_IrclkEnable, kMCG_IrcSlow,
				      CONFIG_MCG_FCRDIV);

	CLOCK_SetSimConfig(&simConfig);

#if CONFIG_ETH_KSDK
	CLOCK_SetEnetTime0Clock(TIMESRC_OSCERCLK);
#endif
}

/**
 *
 * @brief Perform basic hardware initialization
 *
 * Initialize the interrupt controller device drivers and the
 * Kinetis UART device driver.
 * Also initialize the timer device driver, if required.
 *
 * @return 0
 */

static int fsl_frdm_k64f_init(struct device *arg)
{
	ARG_UNUSED(arg);

	int oldLevel; /* old interrupt lock level */
	uint32_t temp_reg;

	/* disable interrupts */
	oldLevel = irq_lock();

	/* enable the port clocks */
	SIM->SCGC5 |= (SIM_SCGC5_PORTA(1) | SIM_SCGC5_PORTB(1) |
			       SIM_SCGC5_PORTC(1) | SIM_SCGC5_PORTD(1) |
			       SIM_SCGC5_PORTE(1));

	/* release I/O power hold to allow normal run state */
	PMC->REGSC |= PMC_REGSC_ACKISO_MASK;

	/*
	 * Disable memory protection and clear slave port errors.
	 * Note that the K64F does not implement the optional ARMv7-M memory
	 * protection unit (MPU), specified by the architecture (PMSAv7), in the
	 * Cortex-M4 core.  Instead, the processor includes its own MPU module.
	 */
	temp_reg = MPU->CESR;
	temp_reg &= ~MPU_CESR_VLD_MASK;
	temp_reg |= MPU_CESR_SPERR_MASK;
	MPU->CESR = temp_reg;

	/* clear all faults */

	_ScbMemFaultAllFaultsReset();
	_ScbBusFaultAllFaultsReset();
	_ScbUsageFaultAllFaultsReset();

	_ScbHardFaultAllFaultsReset();

	/* Initialize PLL/system clock to 120 MHz */
	clkInit();

	/*
	 * install default handler that simply resets the CPU
	 * if configured in the kernel, NOP otherwise
	 */
	NMI_INIT();

	/* restore interrupt state */
	irq_unlock(oldLevel);
	return 0;
}

SYS_INIT(fsl_frdm_k64f_init, PRE_KERNEL_1, 0);
