/* phil_task.c - dining philosophers */

/*
 * Copyright (c) 2011-2016 Wind River Systems, Inc.
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
#include "phil.h"

#define STSIZE 1024

extern void phil_entry(void);
extern void object_monitor(void);

char __stack phil_stack[N_PHILOSOPHERS][STSIZE];
char __stack mon_stack[STSIZE];
struct nano_sem forks[N_PHILOSOPHERS];

/**
 *
 * @brief Nanokernel entry point
 *
 */

int main(void)
{
	int i;

	for (i = 0; i < N_PHILOSOPHERS; i++) {
		nano_sem_init(&forks[i]);
		nano_task_sem_give(&forks[i]);
	}

	/* create philosopher fibers */
	for (i = 0; i < N_PHILOSOPHERS; i++) {
		task_fiber_start(&phil_stack[i][0], STSIZE,
				 (nano_fiber_entry_t) phil_entry, 0, 0, 6, 0);
	}

	/* create object counter monitor */
	task_fiber_start(mon_stack, STSIZE,
			(nano_fiber_entry_t) object_monitor, 0, 0, 7, 0);

	return 0;
}
