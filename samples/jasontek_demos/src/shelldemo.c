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
#include <device.h>
#include <gpio.h>
#include <misc/printk.h>
#include <misc/shell.h>
#include <stdlib.h>

//#include <../lib/libc/minimal/include/stdlib.h>

/**
 * the demo assumes use of nucleo_f103rb board, adjust defines below
 * to fit your board
 */
static int shell_cmd_ping(int argc, char *argv[])
{
        printk("pong\n");

        return 0;
}

static int shell_cmd_params(int argc, char *argv[])
{
        printk("argc = %d, argv[0] = %s\n", argc, argv[0]);

        return 0;
}

//extern int atoi(const char *s);
	
static int shell_cmd_blink(int argc, char *argv[])
{
	/* we're going to use PA5 */
	#define PORT "GPIOA"
	/* PB5 */
	#define LED1 5
	#define SLEEP_TIME	500

	if(argc != 2) {
		printk("blink must have one parameter!\n");
		return 0;
	}
	int cnt = 0;
	struct device *gpioa;
	int mode = atoi(argv[1]);
	
	gpioa = device_get_binding(PORT);
	gpio_pin_configure(gpioa, LED1, GPIO_DIR_OUT);
	
	switch(mode)
	{
		case 1: /*on*/
			gpio_pin_write(gpioa, LED1, 1);
		break;
		case 2: /*off*/
		    gpio_pin_write(gpioa, LED1, 0);
		break;
		case 3: /*blinking*/
			while (cnt<51) {
			gpio_pin_write(gpioa, LED1, cnt % 2);
			k_sleep(SLEEP_TIME);
			cnt++;
			}
		break;
		default:break;
	}
	return 0;
}

static struct shell_cmd commands[] = {
        { "ping", shell_cmd_ping },
        { "params", shell_cmd_params, "print argc" },
		{ "blink", shell_cmd_blink, "led blink" },
        { NULL, NULL }
};

void shelldemo_main(void)
{
	SHELL_REGISTER("shell_demo", commands);
}
