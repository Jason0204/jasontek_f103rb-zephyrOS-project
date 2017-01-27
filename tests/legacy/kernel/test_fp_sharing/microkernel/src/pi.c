/* pi.c - pi computation portion of FPU sharing test */

/*
 * Copyright (c) 2011-2014 Wind River Systems, Inc.
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

/*
DESCRIPTION
This module is used for the microkernel version of the FPU sharing test,
and supplements the basic load/store test by incorporating two additional
threads that utilize the floating point unit.

Testing utilizes a pair of tasks that independently compute pi. The lower
priority task is regularly preempted by the higher priority task, thereby
testing whether floating point context information is properly preserved.

The following formula is used to compute pi:

    pi = 4 * (1 - 1/3 + 1/5 - 1/7 + 1/9 - ... )

This series converges to pi very slowly. For example, performing 50,000
iterations results in an accuracy of 3 decimal places.

A reference value of pi is computed once at the start of the test. All
subsequent computations must produce the same value, otherwise an error
has occurred.
 */

#include <zephyr.h>

#include <stdio.h>
#include <tc_util.h>

#include <float_context.h>

/*
 * PI_NUM_ITERATIONS: This macro is defined in the project's Makefile and
 * is configurable from the command line.
 */

static double reference_pi = 0.0f;

/*
 * Test counters are "volatile" because GCC wasn't properly updating
 * calc_pi_low_count properly when calculate_pi_low() contained a "return"
 * in its error handling logic -- the value was incremented in a register,
 * but never written back to memory. (Seems to be a compiler bug!)
 */

static volatile unsigned int calc_pi_low_count = 0;
static volatile unsigned int calc_pi_high_count = 0;

/**
 *
 * @brief Entry point for the low priority pi compute task
 *
 * @return N/A
 */

void calculate_pi_low(void)
{
	volatile double pi; /* volatile to avoid optimizing out of loop */
	double divisor = 3.0;
	double sign = -1.0;
	unsigned int ix;

	/* loop forever, unless an error is detected */

	while (1) {

		sign = -1.0;
		pi = 1.0;
		divisor = 3.0;

		for (ix = 0; ix < PI_NUM_ITERATIONS; ix++) {
			pi += sign / divisor;
			divisor += 2.0;
			sign *= -1.0;
		}

		pi *= 4;

		if (reference_pi == 0.0f) {
			reference_pi = pi;
		} else if (reference_pi != pi) {
			TC_ERROR("Computed pi %1.6f, reference pi %1.6f\n",
					pi, reference_pi);
			fpu_sharing_error = 1;
			return;
		}

		++calc_pi_low_count;
	}
}

/**
 *
 * @brief Entry point for the high priority pi compute task
 *
 * @return N/A
 */

void calculate_pi_high(void)
{
	volatile double pi; /* volatile to avoid optimizing out of loop */
	double divisor = 3.0;
	double sign = -1.0;
	unsigned int ix;

	/* loop forever, unless an error is detected */

	while (1) {

		sign = -1.0;
		pi = 1.0;
		divisor = 3.0;

		for (ix = 0; ix < PI_NUM_ITERATIONS; ix++) {
			pi += sign / divisor;
			divisor += 2.0;
			sign *= -1.0;
		}

		/*
		 * Relinquish the processor for the remainder of the current
		 * system clock tick, so that lower priority threads get a
		 * chance to run.
		 *
		 * This exercises the ability of the nanokernel to restore the
		 * FPU state of a low priority thread _and_ the ability of the
		 * nanokernel to provide a "clean" FPU state to this thread
		 * once the sleep ends.
		 */

		task_sleep(1);

		pi *= 4;

		if (reference_pi == 0.0f) {
			reference_pi = pi;
		} else if (reference_pi != pi) {
			TC_ERROR("Computed pi %1.6f, reference pi %1.6f\n",
					 pi, reference_pi);
			fpu_sharing_error = 1;
			return;
		}

		/* periodically issue progress report */

		if ((++calc_pi_high_count % 100) == 50) {
			printf("Pi calculation OK after %u (high) + %u (low) tests (computed %1.6f)\n",
				calc_pi_high_count, calc_pi_low_count, pi);
		}
	}
}
