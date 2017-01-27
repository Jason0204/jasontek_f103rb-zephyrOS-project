/*
 * Copyright (c) 2016 Intel Corporation.
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

#include <zephyr.h>
#include <sys_io.h>
#include <misc/__assert.h>
#include <power.h>
#include <soc_power.h>
#include <init.h>
#include <kernel_structs.h>

#include "ss_power_states.h"

#define SLEEP_MODE_CORE_OFF (0x0)
#define SLEEP_MODE_CORE_TIMERS_RTC_OFF (0x60)
#define ENABLE_INTERRUPTS (BIT(4) | _ARC_V2_STATUS32_E(_ARC_V2_DEF_IRQ_LEVEL))

#define ARC_SS1 (SLEEP_MODE_CORE_OFF | ENABLE_INTERRUPTS)
#define ARC_SS2 (SLEEP_MODE_CORE_TIMERS_RTC_OFF | ENABLE_INTERRUPTS)

/* QMSI does not set the interrupt enable bit in the sleep operand.
 * For the time being, implement this in Zephyr.
 * This will be removed once QMSI is fixed.
 */
static void enter_arc_state(int mode)
{
	/* Enter SSx */
	__asm__ volatile("sleep %0"
			: /* No output operands. */
			: /* Input operands. */
			"r"(mode) : "memory");
}

void _sys_soc_set_power_state(enum power_states state)
{
	switch (state) {
	case SYS_POWER_STATE_CPU_LPS:
		ss_power_soc_lpss_enable();
		enter_arc_state(ARC_SS2);
		break;
	case SYS_POWER_STATE_CPU_LPS_1:
		enter_arc_state(ARC_SS2);
		break;
	case SYS_POWER_STATE_CPU_LPS_2:
		enter_arc_state(ARC_SS1);
		break;
	case SYS_POWER_STATE_DEEP_SLEEP:
	case SYS_POWER_STATE_DEEP_SLEEP_1:
		/* Sleep states are not yet supported for ARC. */
		break;
	default:
		break;
	}
}

void _sys_soc_power_state_post_ops(enum power_states state)
{
	uint32_t limit;

	switch (state) {
	case SYS_POWER_STATE_CPU_LPS:
		ss_power_soc_lpss_disable();
	case SYS_POWER_STATE_CPU_LPS_1:
		/* Expire the timer as it is disabled in SS2. */
		limit = _arc_v2_aux_reg_read(_ARC_V2_TMR0_LIMIT);
		_arc_v2_aux_reg_write(_ARC_V2_TMR0_COUNT, limit - 1);
		break;
	default:
		break;
	}
}
