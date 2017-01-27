/*
 * Copyright (c) 2011-2012, 2014-2015 Wind River Systems, Inc.
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
 * @brief UART-driven console
 *
 *
 * Serial console driver.
 * Hooks into the printk and fputc (for printf) modules. Poll driven.
 */

#include <kernel.h>
#include <arch/cpu.h>

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>

#include <device.h>
#include <init.h>

#include <board.h>
#include <uart.h>
#include <console/uart_console.h>
#include <toolchain.h>
#include <sections.h>
#include <atomic.h>
#include <misc/printk.h>

static struct device *uart_console_dev;

#ifdef CONFIG_UART_CONSOLE_DEBUG_SERVER_HOOKS

static uart_console_in_debug_hook_t debug_hook_in;
void uart_console_in_debug_hook_install(uart_console_in_debug_hook_t hook)
{
	debug_hook_in = hook;
}

static UART_CONSOLE_OUT_DEBUG_HOOK_SIG(debug_hook_out_nop)
{
	ARG_UNUSED(c);
	return !UART_CONSOLE_DEBUG_HOOK_HANDLED;
}

static uart_console_out_debug_hook_t *debug_hook_out = debug_hook_out_nop;
void uart_console_out_debug_hook_install(uart_console_out_debug_hook_t *hook)
{
	debug_hook_out = hook;
}
#define HANDLE_DEBUG_HOOK_OUT(c) \
	(debug_hook_out(c) == UART_CONSOLE_DEBUG_HOOK_HANDLED)

#endif /* CONFIG_UART_CONSOLE_DEBUG_SERVER_HOOKS */

#if 0 /* NOTUSED */
/**
 *
 * @brief Get a character from UART
 *
 * @return the character or EOF if nothing present
 */

static int console_in(void)
{
	unsigned char c;

	if (uart_poll_in(uart_console_dev, &c) < 0)
		return EOF;
	else
		return (int)c;
}
#endif

#if defined(CONFIG_PRINTK) || defined(CONFIG_STDOUT_CONSOLE)
/**
 *
 * @brief Output one character to UART
 *
 * Outputs both line feed and carriage return in the case of a '\n'.
 *
 * @param c Character to output
 *
 * @return The character passed as input.
 */

static int console_out(int c)
{
#ifdef CONFIG_UART_CONSOLE_DEBUG_SERVER_HOOKS

	int handled_by_debug_server = HANDLE_DEBUG_HOOK_OUT(c);

	if (handled_by_debug_server) {
		return c;
	}

#endif /* CONFIG_UART_CONSOLE_DEBUG_SERVER_HOOKS */

	if ('\n' == c) {
		uart_poll_out(uart_console_dev, '\r');
	}
	uart_poll_out(uart_console_dev, c);

	return c;
}

#endif

#if defined(CONFIG_STDOUT_CONSOLE)
extern void __stdout_hook_install(int (*hook)(int));
#else
#define __stdout_hook_install(x)		\
	do {/* nothing */			\
	} while ((0))
#endif

#if defined(CONFIG_PRINTK)
extern void __printk_hook_install(int (*fn)(int));
#else
#define __printk_hook_install(x)		\
	do {/* nothing */			\
	} while ((0))
#endif

#if defined(CONFIG_CONSOLE_HANDLER)
static struct k_fifo *avail_queue;
static struct k_fifo *lines_queue;
static uint8_t (*completion_cb)(char *line, uint8_t len);

/* Control characters */
#define ESC                0x1b
#define DEL                0x7f

/* ANSI escape sequences */
#define ANSI_ESC           '['
#define ANSI_UP            'A'
#define ANSI_DOWN          'B'
#define ANSI_FORWARD       'C'
#define ANSI_BACKWARD      'D'
#define ANSI_END           'F'
#define ANSI_HOME          'H'
#define ANSI_DEL           '~'

static int read_uart(struct device *uart, uint8_t *buf, unsigned int size)
{
	int rx;

	rx = uart_fifo_read(uart, buf, size);
	if (rx < 0) {
		/* Overrun issue. Stop the UART */
		uart_irq_rx_disable(uart);

		return -EIO;
	}

	return rx;
}

static inline void cursor_forward(unsigned int count)
{
	printk("\x1b[%uC", count);
}

static inline void cursor_backward(unsigned int count)
{
	printk("\x1b[%uD", count);
}

static inline void cursor_save(void)
{
	printk("\x1b[s");
}

static inline void cursor_restore(void)
{
	printk("\x1b[u");
}

static void insert_char(char *pos, char c, uint8_t end)
{
	char tmp;

	/* Echo back to console */
	uart_poll_out(uart_console_dev, c);

	if (end == 0) {
		*pos = c;
		return;
	}

	tmp = *pos;
	*(pos++) = c;

	cursor_save();

	while (end-- > 0) {
		uart_poll_out(uart_console_dev, tmp);
		c = *pos;
		*(pos++) = tmp;
		tmp = c;
	}

	/* Move cursor back to right place */
	cursor_restore();
}

static void del_char(char *pos, uint8_t end)
{
	uart_poll_out(uart_console_dev, '\b');

	if (end == 0) {
		uart_poll_out(uart_console_dev, ' ');
		uart_poll_out(uart_console_dev, '\b');
		return;
	}

	cursor_save();

	while (end-- > 0) {
		*pos = *(pos + 1);
		uart_poll_out(uart_console_dev, *(pos++));
	}

	uart_poll_out(uart_console_dev, ' ');

	/* Move cursor back to right place */
	cursor_restore();
}

enum {
	ESC_ESC,
	ESC_ANSI,
	ESC_ANSI_FIRST,
	ESC_ANSI_VAL,
	ESC_ANSI_VAL_2
};

static atomic_t esc_state;
static unsigned int ansi_val, ansi_val_2;
static uint8_t cur, end;

static void handle_ansi(uint8_t byte, char *line)
{
	if (atomic_test_and_clear_bit(&esc_state, ESC_ANSI_FIRST)) {
		if (!isdigit(byte)) {
			ansi_val = 1;
			goto ansi_cmd;
		}

		atomic_set_bit(&esc_state, ESC_ANSI_VAL);
		ansi_val = byte - '0';
		ansi_val_2 = 0;
		return;
	}

	if (atomic_test_bit(&esc_state, ESC_ANSI_VAL)) {
		if (isdigit(byte)) {
			if (atomic_test_bit(&esc_state, ESC_ANSI_VAL_2)) {
				ansi_val_2 *= 10;
				ansi_val_2 += byte - '0';
			} else {
				ansi_val *= 10;
				ansi_val += byte - '0';
			}
			return;
		}

		/* Multi value sequence, e.g. Esc[Line;ColumnH */
		if (byte == ';' &&
		    !atomic_test_and_set_bit(&esc_state, ESC_ANSI_VAL_2)) {
			return;
		}

		atomic_clear_bit(&esc_state, ESC_ANSI_VAL);
		atomic_clear_bit(&esc_state, ESC_ANSI_VAL_2);
	}

ansi_cmd:
	switch (byte) {
	case ANSI_BACKWARD:
		if (ansi_val > cur) {
			break;
		}

		end += ansi_val;
		cur -= ansi_val;
		cursor_backward(ansi_val);
		break;
	case ANSI_FORWARD:
		if (ansi_val > end) {
			break;
		}

		end -= ansi_val;
		cur += ansi_val;
		cursor_forward(ansi_val);
		break;
	case ANSI_HOME:
		if (!cur) {
			break;
		}

		cursor_backward(cur);
		end += cur;
		cur = 0;
		break;
	case ANSI_END:
		if (!end) {
			break;
		}

		cursor_forward(end);
		cur += end;
		end = 0;
		break;
	case ANSI_DEL:
		if (!end) {
			break;
		}

		cursor_forward(1);
		del_char(&line[cur], --end);
		break;
	default:
		break;
	}

	atomic_clear_bit(&esc_state, ESC_ANSI);
}

void uart_console_isr(struct device *unused)
{
	ARG_UNUSED(unused);

	while (uart_irq_update(uart_console_dev) &&
	       uart_irq_is_pending(uart_console_dev)) {
		static struct uart_console_input *cmd;
		uint8_t byte;
		int rx;

		if (!uart_irq_rx_ready(uart_console_dev)) {
			continue;
		}

		/* Character(s) have been received */

		rx = read_uart(uart_console_dev, &byte, 1);
		if (rx < 0) {
			return;
		}

#ifdef CONFIG_UART_CONSOLE_DEBUG_SERVER_HOOKS
		if (debug_hook_in != NULL && debug_hook_in(byte) != 0) {
			/*
			 * The input hook indicates that no further processing
			 * should be done by this handler.
			 */
			return;
		}
#endif

		if (!cmd) {
			cmd = k_fifo_get(avail_queue, K_NO_WAIT);
			if (!cmd)
				return;
		}

		/* Handle ANSI escape mode */
		if (atomic_test_bit(&esc_state, ESC_ANSI)) {
			handle_ansi(byte, cmd->line);
			continue;
		}

		/* Handle escape mode */
		if (atomic_test_and_clear_bit(&esc_state, ESC_ESC)) {
			switch (byte) {
			case ANSI_ESC:
				atomic_set_bit(&esc_state, ESC_ANSI);
				atomic_set_bit(&esc_state, ESC_ANSI_FIRST);
				break;
			default:
				break;
			}

			continue;
		}

		/* Handle special control characters */
		if (!isprint(byte)) {
			switch (byte) {
			case DEL:
				if (cur > 0) {
					del_char(&cmd->line[--cur], end);
				}
				break;
			case ESC:
				atomic_set_bit(&esc_state, ESC_ESC);
				break;
			case '\r':
				cmd->line[cur + end] = '\0';
				uart_poll_out(uart_console_dev, '\r');
				uart_poll_out(uart_console_dev, '\n');
				cur = 0;
				end = 0;
				k_fifo_put(lines_queue, cmd);
				cmd = NULL;
				break;
			case '\t':
				if (completion_cb && !end) {
					cur += completion_cb(cmd->line, cur);
				}
				break;
			default:
				break;
			}

			continue;
		}

		/* Ignore characters if there's no more buffer space */
		if (cur + end < sizeof(cmd->line) - 1) {
			insert_char(&cmd->line[cur++], byte, end);
		}
	}
}

static void console_input_init(void)
{
	uint8_t c;

	uart_irq_rx_disable(uart_console_dev);
	uart_irq_tx_disable(uart_console_dev);

	uart_irq_callback_set(uart_console_dev, uart_console_isr);

	/* Drain the fifo */
	while (uart_irq_rx_ready(uart_console_dev)) {
		uart_fifo_read(uart_console_dev, &c, 1);
	}

	uart_irq_rx_enable(uart_console_dev);
}

void uart_register_input(struct k_fifo *avail, struct k_fifo *lines,
			 uint8_t (*completion)(char *str, uint8_t len))
{
	avail_queue = avail;
	lines_queue = lines;
	completion_cb = completion;

	console_input_init();
}

#else
#define console_input_init(x)			\
	do {/* nothing */			\
	} while ((0))
#define uart_register_input(x)			\
	do {/* nothing */			\
	} while ((0))
#endif

/**
 *
 * @brief Install printk/stdout hook for UART console output
 *
 * @return N/A
 */

void uart_console_hook_install(void)
{
	__stdout_hook_install(console_out);
	__printk_hook_install(console_out);
}

/**
 *
 * @brief Initialize one UART as the console/debug port
 *
 * @return 0 if successful, otherwise failed.
 */
static int uart_console_init(struct device *arg)
{

	ARG_UNUSED(arg);

	uart_console_dev = device_get_binding(CONFIG_UART_CONSOLE_ON_DEV_NAME);

#ifdef CONFIG_USB_UART_CONSOLE
	while (1) {
		uint32_t dtr = 0;

		uart_line_ctrl_get(uart_console_dev, LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
	}
	k_busy_wait(1000000);
#endif

	uart_console_hook_install();

	return 0;
}

/* UART console initializes after the UART device itself */
SYS_INIT(uart_console_init,
#if defined(CONFIG_USB_UART_CONSOLE)
			APPLICATION,
#elif defined(CONFIG_EARLY_CONSOLE)
			PRE_KERNEL_1,
#else
			POST_KERNEL,
#endif
			CONFIG_UART_CONSOLE_INIT_PRIORITY);
