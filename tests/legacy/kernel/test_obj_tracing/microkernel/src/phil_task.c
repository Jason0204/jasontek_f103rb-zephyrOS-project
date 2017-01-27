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

/**
 *
 * @brief Routine to start dining philosopher demo
 *
 */

void phil_demo(void)
{
	task_group_start(PHI);
	task_group_start(MON);
}
