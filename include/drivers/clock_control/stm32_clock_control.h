/*
 * Copyright (c) 2016 Open-RnD Sp. z o.o.
 * Copyright (c) 2016 BayLibre, SAS
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
#ifndef _STM32_CLOCK_CONTROL_H_
#define _STM32_CLOCK_CONTROL_H_

#include <clock_control.h>

/* common clock control device name for all STM32 chips */
#define STM32_CLOCK_CONTROL_NAME "stm32-cc"

#ifdef CONFIG_SOC_SERIES_STM32F1X
#include "stm32f1_clock_control.h"
#elif CONFIG_SOC_SERIES_STM32F4X
#include "stm32f4_clock_control.h"
#endif

#ifdef CONFIG_SOC_SERIES_STM32L4X
#include "stm32l4x_clock_control.h"
#endif

#endif /* _STM32_CLOCK_CONTROL_H_ */
