/*
 * Copyright (c) 2014 Wind River Systems, Inc.
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
 * @brief Populated exception vector table
 *
 * Vector table with exceptions filled in. The reset vector is the system entry
 * point, ie. the first instruction executed.
 *
 * The table is populated with all the system exception handlers. No exception
 * should not be triggered until the kernel is ready to handle them.
 *
 * We are using a C file instead of an assembly file (like the ARM vector table)
 * to work around an issue with the assembler where:
 *
 *   .word <function>
 *
 * statements would end up with the two half-words of the functions' addresses
 * swapped.
 */

#include <stdint.h>
#include <toolchain.h>
#include "vector_table.h"

struct vector_table {
	uint32_t reset;
	uint32_t memory_error;
	uint32_t instruction_error;
	uint32_t ev_machine_check;
	uint32_t ev_tlb_miss_i;
	uint32_t ev_tlb_miss_d;
	uint32_t ev_prot_v;
	uint32_t ev_privilege_v;
	uint32_t ev_swi;
	uint32_t ev_trap;
	uint32_t ev_extension;
	uint32_t ev_div_zero;
	uint32_t ev_dc_error;
	uint32_t ev_maligned;
	uint32_t unused_1;
	uint32_t unused_2;
};

struct vector_table _VectorTable _GENERIC_SECTION(.exc_vector_table) = {
	(uint32_t)__reset,
	(uint32_t)__memory_error,
	(uint32_t)__instruction_error,
	(uint32_t)__ev_machine_check,
	(uint32_t)__ev_tlb_miss_i,
	(uint32_t)__ev_tlb_miss_d,
	(uint32_t)__ev_prot_v,
	(uint32_t)__ev_privilege_v,
	(uint32_t)__ev_swi,
	(uint32_t)__ev_trap,
	(uint32_t)__ev_extension,
	(uint32_t)__ev_div_zero,
	(uint32_t)__ev_dc_error,
	(uint32_t)__ev_maligned,
	0,
	0
};

