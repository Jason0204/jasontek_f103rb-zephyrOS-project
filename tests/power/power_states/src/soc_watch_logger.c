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

#include "soc_watch_logger.h"

#define STSIZE 512
char __stack soc_watch_event_logger_stack[1][STSIZE];

/**
 * @brief soc_watch data collector thread
 *
 * @details Collect the kernel event messages and pass them to
 * soc_watch
 *
 * @return No return value.
 */
void soc_watch_data_collector(void)
{
#ifdef CONFIG_SOC_WATCH
	int res;
	uint32_t data[4];
	uint8_t dropped_count;
	uint16_t event_id;

	/* We register the thread as collector to avoid this thread generating a
	 * context switch event every time it collects the data
	 */
	sys_k_event_logger_register_as_collector();

	while (1) {
		/* collect the data */
		uint8_t data_length = SIZE32_OF(data);

		res = sys_k_event_logger_get_wait(&event_id, &dropped_count,
				data, &data_length);

		if (res > 0) {

			/* process the data */
			switch (event_id) {
#ifdef CONFIG_KERNEL_EVENT_LOGGER_CONTEXT_SWITCH
			case KERNEL_EVENT_LOGGER_CONTEXT_SWITCH_EVENT_ID:
				if (data_length != 2) {
					PRINTF("\x1b[13;1HError in context switch message. "
							"event_id = %d, Expected %d, received %d\n",
							event_id, 2, data_length);
				} else {
					/* Log context switch event for SoCWatch */
					SOC_WATCH_LOG_APP_EVENT(SOCW_EVENT_APP,
							event_id, data[1]);
				}
				break;
#endif
#ifdef CONFIG_KERNEL_EVENT_LOGGER_INTERRUPT
			case KERNEL_EVENT_LOGGER_INTERRUPT_EVENT_ID:
				if (data_length != 2) {
					PRINTF("\x1b[13;1HError in interrupt message. "
						"event_id = %d, Expected %d, received %d\n",
						event_id, 2, data_length);
				} else {
					/* Log interrupt event for SoCWatch */
					SOC_WATCH_LOG_EVENT(SOCW_EVENT_INTERRUPT, data[1]);
				}
				break;
#endif
			default:
				PRINTF("unrecognized event id %d", event_id);
			}
		} else {
			/* This error should never happen */
			if (res == -EMSGSIZE) {
				PRINTF("FATAL ERROR. The buffer provided to collect the "
						"profiling events is too small\n");
			}
		}
	}
#endif
}

/**
 * @brief Start the soc_watch logger thread
 *
 * @details Start the soc_watch data collector thread
 *
 * @return No return value.
 */
void soc_watch_logger_thread_start(void)
{
	PRINTF("\x1b[2J\x1b[15;1H");

	k_thread_spawn(&soc_watch_event_logger_stack[0][0], STSIZE,
			(k_thread_entry_t) soc_watch_data_collector, 0, 0, 0,
			6, 0, 0);
}
