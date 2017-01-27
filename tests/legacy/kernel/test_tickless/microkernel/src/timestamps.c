/* timestamps.c - timestamp support for tickless idle testing */

/*
 * Copyright (c) 2014 Wind River Systems, Inc.
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

/*
DESCRIPTION
Platform-specific timestamp support for the tickless idle test.
 */

#include <tc_util.h>
#include <stddef.h>
#include <misc/__assert.h>

#if defined(CONFIG_SOC_TI_LM3S6965_QEMU)
/*
 * @brief Use a General Purpose Timer in
 * 32-bit periodic timer mode (down-counter)
 * (RTC mode's resolution of 1 second is insufficient.)
 */

#define _TIMESTAMP_NUM 0  /* set to timer # for use by timestamp (0-3) */

#define _CLKGATECTRL *((volatile uint32_t *)0x400FE104)
#define _CLKGATECTRL_TIMESTAMP_EN (1 << (16 + _TIMESTAMP_NUM))

#define _TIMESTAMP_BASE 0x40030000
#define _TIMESTAMP_OFFSET (0x1000 * _TIMESTAMP_NUM)
#define _TIMESTAMP_ADDR (_TIMESTAMP_BASE + _TIMESTAMP_OFFSET)

#define _TIMESTAMP_CFG *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0))
#define _TIMESTAMP_CTRL *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0xC))
#define _TIMESTAMP_MODE *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x4))
#define _TIMESTAMP_LOAD *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x28))
#define _TIMESTAMP_IMASK *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x18))
#define _TIMESTAMP_ISTATUS *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x1C))
#define _TIMESTAMP_ICLEAR *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x24))
#define _TIMESTAMP_VAL *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x48))

/*
 * Set the rollover value such that it leaves the most significant bit of
 * the returned timestamp value unused.  This allows room for extended values
 * when handling rollovers when converting to an up-counter value.
 */
#define _TIMESTAMP_MAX ((uint32_t)0x7FFFFFFF)
#define _TIMESTAMP_EXT ((uint32_t)0x80000000)

/**
 *
 * @brief Timestamp initialization
 *
 * This routine initializes the timestamp timer.
 *
 * @return N/A
 */
void _TimestampOpen(void)
{
	/* QEMU does not currently support the 32-bit timer modes of the GPTM */
	printk("WARNING! Timestamp is not supported for this target!\n");

	/* enable timer access */
	_CLKGATECTRL |= _CLKGATECTRL_TIMESTAMP_EN;

	/* minimum 3 clk delay is required before timer register access */
	task_sleep(3);

	_TIMESTAMP_CTRL = 0x0;  /* disable/reset timer */
	_TIMESTAMP_CFG = 0x0;  /* 32-bit timer */
	_TIMESTAMP_MODE = 0x2;  /* periodic mode */
	_TIMESTAMP_LOAD = _TIMESTAMP_MAX; /* maximum interval to reduce rollovers */
	_TIMESTAMP_IMASK = 0x70F;  /* mask all timer interrupts */
	_TIMESTAMP_ICLEAR = 0x70F;  /* clear all interrupt status */

	_TIMESTAMP_CTRL = 0x1;  /* enable timer */
}

/**
 *
 * @brief Timestamp timer read
 *
 * This routine returns the timestamp value.
 *
 * @return timestamp value
 */
uint32_t _TimestampRead(void)
{
	static uint32_t lastTimerVal = 0;
	static uint32_t cnt = 0;
	uint32_t timerVal = _TIMESTAMP_VAL;

	/* handle rollover for every other read (end of sleep) */

	if ((cnt % 2) && (timerVal > lastTimerVal)) {
		lastTimerVal = timerVal;

		/* convert to extended up-counter value */
		timerVal = _TIMESTAMP_EXT + (_TIMESTAMP_MAX - timerVal);
	} else {
		lastTimerVal = timerVal;

		/* convert to up-counter value */
		timerVal = _TIMESTAMP_MAX - timerVal;
	}

	cnt++;

	return timerVal;
}

/**
 *
 * @brief Timestamp release
 *
 * This routine releases the timestamp timer.
 *
 * @return N/A
 */
void _TimestampClose(void)
{

	/* disable/reset timer */
	_TIMESTAMP_CTRL = 0x0;
	_TIMESTAMP_CFG = 0x0;

	/* disable timer access */
	_CLKGATECTRL &= ~_CLKGATECTRL_TIMESTAMP_EN;
}

#elif defined(CONFIG_SOC_MK64F12)
/* Freescale FRDM-K64F target - use RTC (prescale value) */

#define _COUNTDOWN_TIMER false

#define _CLKGATECTRL *((volatile uint32_t *)0x4004803C)
#define _CLKGATECTRL_TIMESTAMP_EN (1 << 29)

#define _SYSOPTCTRL2 *((volatile uint32_t *)0x40048004)
#define _SYSOPTCTRL2_32KHZRTCCLK (1 << 4)

#define _TIMESTAMP_ADDR (0x4003D000)

#define _TIMESTAMP_ICLEAR *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x24))

#define _TIMESTAMP_VAL *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0))
#define _TIMESTAMP_PRESCALE *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x4))
#define _TIMESTAMP_COMP *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0xC))
#define _TIMESTAMP_CTRL *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x10))
#define _TIMESTAMP_STATUS *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x14))
#define _TIMESTAMP_LOCK *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x18))
#define _TIMESTAMP_IMASK *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x1C))
#define _TIMESTAMP_RACCESS *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x800))
#define _TIMESTAMP_WACCESS *((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x804))

/**
 *
 * @brief Timestamp initialization
 *
 * This routine initializes the timestamp timer.
 *
 * @return N/A
 */
void _TimestampOpen(void)
{
	/* enable timer access */
	_CLKGATECTRL |= _CLKGATECTRL_TIMESTAMP_EN;

	/* set 32 KHz RTC clk */
	_SYSOPTCTRL2 |= _SYSOPTCTRL2_32KHZRTCCLK;

	_TIMESTAMP_STATUS = 0x0;  /* disable counter */
	_TIMESTAMP_CTRL = 0x100;  /* enable oscillator */

	_TIMESTAMP_LOCK = 0xFF;  /* unlock registers */
	_TIMESTAMP_PRESCALE = 0x0;  /* reset prescale value */
	_TIMESTAMP_COMP = 0x0;  /* reset compensation values */
	_TIMESTAMP_RACCESS = 0xFF;  /* allow register read access */
	_TIMESTAMP_WACCESS = 0xFF;  /* allow register write access */
	_TIMESTAMP_IMASK = 0x0;  /* mask all timer interrupts */

	/* minimum 0.3 sec delay required for oscillator stabilization */
	task_sleep(300000/sys_clock_us_per_tick);

	_TIMESTAMP_VAL = 0x0;  /* clear invalid time flag in status register */

	_TIMESTAMP_STATUS = 0x10;  /* enable counter */
}

/**
 *
 * @brief Timestamp timer read
 *
 * This routine returns the timestamp value.
 *
 * @return timestamp value
 */
uint32_t _TimestampRead(void)
{
	static uint32_t lastPrescale = 0;
	static uint32_t cnt = 0;
	uint32_t prescale1 = _TIMESTAMP_PRESCALE;
	uint32_t prescale2 = _TIMESTAMP_PRESCALE;

	/* ensure a valid reading */

	while (prescale1 != prescale2) {
		prescale1 = _TIMESTAMP_PRESCALE;
		prescale2 = _TIMESTAMP_PRESCALE;
	}

	/* handle prescale rollover @ 0x8000 for every other read (end of sleep) */

	if ((cnt % 2) && (prescale1 < lastPrescale)) {
		prescale1 += 0x8000;
	}

	lastPrescale = prescale2;
	cnt++;

	return prescale1;
}

/**
 *
 * @brief Timestamp release
 *
 * This routine releases the timestamp timer.
 *
 * @return N/A
 */
void _TimestampClose(void)
{
	_TIMESTAMP_STATUS = 0x0;  /* disable counter */
	_TIMESTAMP_CTRL = 0x0;  /* disable oscillator */
}

#elif defined(CONFIG_SOC_ATMEL_SAM3)
/* Atmel SAM3 family processor - use RTT (Real-time Timer) */

#include <soc.h>

#define _TIMESTAMP_ADDR (0x400E1A30)

#define _TIMESTAMP_MODE (*((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x00)))
#define _TIMESTAMP_VAL (*((volatile uint32_t *)(_TIMESTAMP_ADDR + 0x08)))

/**
 *
 * @brief Timestamp initialization
 *
 * This routine initializes the timestamp timer.
 *
 * @return N/A
 */
void _TimestampOpen(void)
{
	/* enable RTT clock from PMC */
	__PMC->pcer0 = (1 << PID_RTT);

	/* Reset RTT and set prescaler to 1 */
	_TIMESTAMP_MODE = (1 << 18) | (1 << 0);
}

/**
 *
 * @brief Timestamp timer read
 *
 * This routine returns the timestamp value.
 *
 * @return timestamp value
 */
uint32_t _TimestampRead(void)
{
	static uint32_t last_val;
	uint32_t tmr_val = _TIMESTAMP_VAL;
	uint32_t ticks;

	/* handle rollover */
	if (tmr_val < last_val) {
		ticks =  ((0xFFFFFFFF - last_val)) + 1 + tmr_val;
	} else {
		ticks = tmr_val - last_val;
	}

	last_val = tmr_val;

	return ticks;
}

/**
 *
 * @brief Timestamp release
 *
 * This routine releases the timestamp timer.
 *
 * @return N/A
 */
void _TimestampClose(void)
{
	/* disable RTT clock from PMC */
	__PMC->pcdr0 = (1 << PID_RTT);
}

#elif defined(CONFIG_SOC_QUARK_SE_C1000_SS)
#include <device.h>
#include <rtc.h>

static struct device *rtc_device;

void timestamp_open(void)
{
	struct rtc_config cfg = {
		.init_val = 0xfff,
		.alarm_enable = 0,
		.alarm_val = 0,
		.cb_fn = NULL
	};
	rtc_device = device_get_binding(CONFIG_RTC_0_NAME);
	__ASSERT(rtc_device != NULL, "QMSI RTC device not found");
	rtc_enable(rtc_device);
	rtc_set_config(rtc_device, &cfg);
}

uint32_t timestamp_read(void)
{
	return rtc_read(rtc_device);
}

void timestamp_close(void)
{
	rtc_disable(rtc_device);
}

#else
#error "Unknown platform"
#endif /* CONFIG_SOC_xxx */
