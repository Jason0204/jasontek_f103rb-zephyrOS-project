/*
 * Copyright (c) 2016 Intel Corporation
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

#include <arch/cpu.h>
#include <misc/__assert.h>


/**
 * Flush the entire instruction cache and pipeline.
 *
 * You will need to call this function if the application writes new program
 * text to memory, such as a boot copier or runtime synthesis of code.  If the
 * new text was written with instructions that do not bypass cache memories,
 * this should immediately be followed by an invocation of
 * _nios2_dcache_flush_all() so that cached instruction data is committed to
 * RAM.
 *
 * See Chapter 9 of the Nios II Gen 2 Software Developer's Handbook for more
 * information on cache considerations.
 */
#if ALT_CPU_ICACHE_SIZE > 0
void _nios2_icache_flush_all(void)
{
	uint32_t i;

	for (i = 0; i < ALT_CPU_ICACHE_SIZE; i += ALT_CPU_ICACHE_LINE_SIZE) {
		_nios2_icache_flush(i);
	}

	/* Get rid of any stale instructions in the pipeline */
	_nios2_pipeline_flush();
}
#endif

/**
 * Flush the entire data cache.
 *
 * This will be typically needed after writing new program text to memory
 * after flushing the instruction cache.
 *
 * The Nios II does not support hardware cache coherency for multi-master
 * or multi-processor systems and software coherency must be implemented
 * when communicating with shared memory. If support for this is introduced
 * in Zephyr additional APIs for flushing ranges of the data cache will need
 * to be implemented.
 *
 * See Chapter 9 of the Nios II Gen 2 Software Developer's Handbook for more
 * information on cache considerations.
 */
#if ALT_CPU_DCACHE_SIZE > 0
void _nios2_dcache_flush_all(void)
{
	uint32_t i;

	for (i = 0; i < ALT_CPU_DCACHE_SIZE; i += ALT_CPU_DCACHE_LINE_SIZE) {
		_nios2_dcache_flush(i);
	}
}
#endif
