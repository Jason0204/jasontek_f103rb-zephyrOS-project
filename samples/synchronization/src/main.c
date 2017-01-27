/* main.c - Hello World demo */

/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
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
#include <misc/printk.h>

/*
 * The hello world demo has two threads that utilize semaphores and sleeping
 * to take turns printing a greeting message at a controlled rate. The demo
 * shows both the static and dynamic approaches for spawning a thread; a real
 * world application would likely use the static approach for both threads.
 */


/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

/* delay between greetings (in ms) */
#define SLEEPTIME 500


/*
 * @param my_name      thread identification string
 * @param my_sem       thread's own semaphore
 * @param other_sem    other thread's semaphore
 */
void helloLoop(const char *my_name,
	       struct k_sem *my_sem, struct k_sem *other_sem)
{
	while (1) {
		/* take my semaphore */
		k_sem_take(my_sem, K_FOREVER);

		/* say "hello" */
		printk("%s: Hello World from %s!\n", my_name, CONFIG_ARCH);

		/* wait a while, then let other thread have a turn */
		k_sleep(SLEEPTIME);
		k_sem_give(other_sem);
	}
}

/* define semaphores */

K_SEM_DEFINE(threadA_sem, 1, 1);	/* starts off "available" */
K_SEM_DEFINE(threadB_sem, 0, 1);	/* starts off "not available" */


/* threadB is a dynamic thread that is spawned by threadA */

void threadB(void *dummy1, void *dummy2, void *dummy3)
{
	/* invoke routine to ping-pong hello messages with threadA */
	helloLoop(__func__, &threadB_sem, &threadA_sem);
}

char __noinit __stack threadB_stack_area[STACKSIZE];


/* threadA is a static thread that is spawned automatically */

void threadA(void *dummy1, void *dummy2, void *dummy3)
{
	/* spawn threadB */
	k_thread_spawn(threadB_stack_area, STACKSIZE, threadB, NULL, NULL, NULL,
		       PRIORITY, 0, K_NO_WAIT);

	/* invoke routine to ping-pong hello messages with threadB */
	helloLoop(__func__, &threadA_sem, &threadB_sem);
}

K_THREAD_DEFINE(threadA_id, STACKSIZE, threadA, NULL, NULL, NULL,
		PRIORITY, 0, K_NO_WAIT);
