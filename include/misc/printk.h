/* printk.h - low-level debug output */

/*
 * Copyright (c) 2010-2012, 2014 Wind River Systems, Inc.
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
#ifndef _PRINTK_H_
#define _PRINTK_H_

#include <toolchain.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 *
 * @brief Print kernel debugging message.
 *
 * This routine prints a kernel debugging message to the system console.
 * Output is send immediately, without any mutual exclusion or buffering.
 *
 * A basic set of conversion specifier characters are supported:
 *   - signed decimal: \%d, \%i
 *   - unsigned decimal: \%u
 *   - unsigned hexadecimal: \%x (\%X is treated as \%x)
 *   - pointer: \%p
 *   - string: \%s
 *   - character: \%c
 *   - percent: \%\%
 *
 * No other conversion specification capabilities are supported, such as flags,
 * field width, precision, or length attributes.
 *
 * @param fmt Format string.
 * @param ... Optional list of format arguments.
 *
 * @return N/A
 */
#ifdef CONFIG_PRINTK
extern __printf_like(1, 2) int printk(const char *fmt, ...);
extern __printf_like(3, 4) int snprintk(char *str, size_t size,
					const char *fmt, ...);
extern int vsnprintk(char *str, size_t size, const char *fmt, va_list ap);

void _vprintk(int (*out)(int, void *), void *ctx, const char *fmt, va_list ap);
#else
static inline __printf_like(1, 2) int printk(const char *fmt, ...)
{
	ARG_UNUSED(fmt);
	return 0;
}

static inline __printf_like(3, 4) int snprintk(char *str, size_t size,
					       const char *fmt, ...)
{
	ARG_UNUSED(str);
	ARG_UNUSED(size);
	ARG_UNUSED(fmt);
	return 0;
}

static inline int vsnprintk(char *str, size_t size, const char *fmt,
			    va_list ap)
{
	ARG_UNUSED(str);
	ARG_UNUSED(size);
	ARG_UNUSED(fmt);
	return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
