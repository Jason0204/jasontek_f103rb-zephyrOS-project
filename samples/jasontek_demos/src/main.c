/*
 * Copyright (c) 2016 Open-RnD Sp. z o.o.
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


#if CONFIG_JASONTEK_SYNCDEMO
	#include "syncdemo.h"
	K_THREAD_DEFINE(threadA_id, STACKSIZE, threadA, NULL, NULL, NULL,	PRIORITY, 0, K_NO_WAIT);
#else
	#if CONFIG_JASONTEK_SHELLDEMO
		#include "shelldemo.h"
		#define  demo_main  shelldemo_main
	#elif CONFIG_JASONTEK_BTNDEMO
		#include "btndemo.h"
		#define  demo_main  button_main
	#elif CONFIG_JASONTEK_PHILDEMO
		#include "phildemo.h"
		#define  demo_main  phildemo_main
	#elif CONFIG_JASONTEK_PWMDEMO
		#include "pwmdemo.h"
		#define  demo_main  fadedemo_main	
	#endif
void main(void)
{
	demo_main();
}
#endif








