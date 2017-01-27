/*  test_hmac_prng.c - TinyCrypt implementation of some HMAC-PRNG tests */

/*
 *  Copyright (C) 2015 by Intel Corporation, All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *    - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *    - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 *    - Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * DESCRIPTION
 * This module tests the following PRNG routines:
 *
 * Scenarios tested include:
 * - HMAC-PRNG init
 * - HMAC-PRNG reseed
 * - HMAC-PRNG generate)
 */

#include <tinycrypt/hmac_prng.h>
#include <tinycrypt/constants.h>

#include <stdio.h>
#include <tc_util.h>
#include <drivers/system_timer.h>

/*
 * Main task to test AES
 */

void main(void)
{
	/* Fake personalization and additional_input
	 * (replace by appropriate values)
	 * e.g.: hostname+timestamp
	 */
	uint8_t additional_input[] = "additional input";
	uint8_t personalization[] = "HOSTNAME";
	uint32_t size = (1 << 15);
	uint32_t result = TC_PASS;
	struct tc_hmac_prng_struct h;
	uint8_t random[size];
	uint8_t seed[128];
	uint32_t i;

	TC_START("Performing HMAC-PRNG tests:");
	TC_PRINT("HMAC-PRNG test#1 (init, reseed, generate):\n");

	/* Fake seed (replace by a a truly random seed): */
	for (i = 0; i < (uint32_t)sizeof(seed); ++i) {
		seed[i] = i;
	}

	TC_PRINT("HMAC-PRNG test#1 (init):\n");
	if (tc_hmac_prng_init(&h, personalization,
			      sizeof(personalization)) == 0) {
		TC_ERROR("HMAC-PRNG initialization failed.\n");
		result = TC_FAIL;
		goto exitTest;
	}
	TC_END_RESULT(result);

	TC_PRINT("HMAC-PRNG test#1 (reseed):\n");
	if (tc_hmac_prng_reseed(&h, seed, sizeof(seed), additional_input,
				sizeof(additional_input)) == 0) {
		TC_ERROR("HMAC-PRNG reseed failed.\n");
		result = TC_FAIL;
		goto exitTest;
	}
	TC_END_RESULT(result);

	TC_PRINT("HMAC-PRNG test#1 (generate):\n");
	if (tc_hmac_prng_generate(random, size, &h) < 1) {
		TC_ERROR("HMAC-PRNG generate failed.\n");
		result = TC_FAIL;
		goto exitTest;
	}
	TC_END_RESULT(result);

	TC_PRINT("All HMAC tests succeeded!\n");

exitTest:
	TC_END_RESULT(result);
	TC_END_REPORT(result);
}
