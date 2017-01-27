/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2016 Vinayak Kariappa Chettimada
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

#include <stdint.h>

void *memq_init(void *link, void **head, void **tail)
{
	/* head and tail pointer to the initial link node */
	*head = *tail = link;

	return link;
}

void *memq_enqueue(void *mem, void *link, void **tail)
{
	/* make the current tail link node point to new link node */
	*((void **)*tail) = link;

	/* assign mem to current tail link node */
	*((void **)*tail + 1) = mem;

	/* increment the tail! */
	*tail = link;

	return link;
}

void *memq_dequeue(void *tail, void **head, void **mem)
{
	void *link;

	/* if head and tail are equal, then queue empty */
	if (*head == tail) {
		return 0;
	}

	/* pick the head link node */
	link = *head;

	/* extract the element node */
	if (mem) {
		*mem = *((void **)link + 1);
	}

	/* increment the head to next link node */
	*head = *((void **)link);

	return link;
}

uint32_t memq_ut(void)
{
	void *head;
	void *tail;
	void *link;
	void *link_0[2];
	void *link_1[2];

	link = memq_init(&link_0[0], &head, &tail);
	if ((link != &link_0[0]) || (head != &link_0[0])
	    || (tail != &link_0[0])) {
		return 1;
	}

	link = memq_dequeue(tail, &head, 0);
	if ((link) || (head != &link_0[0])) {
		return 2;
	}

	link = memq_enqueue(0, &link_1[0], &tail);
	if ((link != &link_1[0]) || (tail != &link_1[0])) {
		return 3;
	}

	link = memq_dequeue(tail, &head, 0);
	if ((link != &link_0[0]) || (tail != &link_1[0])
	    || (head != &link_1[0])) {
		return 4;
	}

	return 0;
}
