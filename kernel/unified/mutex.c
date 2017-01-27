/*
 * Copyright (c) 2016 Wind River Systems, Inc.
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
 * @file @brief mutex kernel services
 *
 * This module contains routines for handling mutex locking and unlocking.
 *
 * Mutexes implement a priority inheritance algorithm that boosts the priority
 * level of the owning thread to match the priority level of the highest
 * priority thread waiting on the mutex.
 *
 * Each mutex that contributes to priority inheritance must be released in the
 * reverse order in which is was acquired.  Furthermore each subsequent mutex
 * that contributes to raising the owning thread's priority level must be
 * acquired at a point after the most recent "bumping" of the priority level.
 *
 * For example, if thread A has two mutexes contributing to the raising of its
 * priority level, the second mutex M2 must be acquired by thread A after
 * thread A's priority level was bumped due to owning the first mutex M1.
 * When releasing the mutex, thread A must release M2 before it releases M1.
 * Failure to follow this nested model may result in threads running at
 * unexpected priority levels (too high, or too low).
 */

#include <kernel.h>
#include <kernel_structs.h>
#include <toolchain.h>
#include <sections.h>
#include <wait_q.h>
#include <misc/dlist.h>
#include <misc/debug/object_tracing_common.h>
#include <errno.h>
#include <init.h>

#ifdef CONFIG_OBJECT_MONITOR
#define RECORD_STATE_CHANGE(mutex) \
	do { (mutex)->num_lock_state_changes++; } while ((0))
#define RECORD_CONFLICT(mutex) \
	do { (mutex)->num_conflicts++; } while ((0))
#else
#define RECORD_STATE_CHANGE(mutex) do { } while ((0))
#define RECORD_CONFLICT(mutex) do { } while ((0))
#endif

#ifdef CONFIG_OBJECT_MONITOR
#define INIT_OBJECT_MONITOR(mutex) do { \
	mutex->num_lock_state_changes = 0; \
	mutex->num_conflicts = 0; \
	} while ((0))
#else
#define INIT_OBJECT_MONITOR(mutex) do { } while ((0))
#endif

extern struct k_mutex _k_mutex_list_start[];
extern struct k_mutex _k_mutex_list_end[];

struct k_mutex *_trace_list_k_mutex;

#ifdef CONFIG_DEBUG_TRACING_KERNEL_OBJECTS

/*
 * Complete initialization of statically defined mutexes.
 */
static int init_mutex_module(struct device *dev)
{
	ARG_UNUSED(dev);

	struct k_mutex *mutex;

	for (mutex = _k_mutex_list_start; mutex < _k_mutex_list_end; mutex++) {
		SYS_TRACING_OBJ_INIT(k_mutex, mutex);
	}
	return 0;
}

SYS_INIT(init_mutex_module, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);

#endif /* CONFIG_DEBUG_TRACING_KERNEL_OBJECTS */

void k_mutex_init(struct k_mutex *mutex)
{
	mutex->owner = NULL;
	mutex->lock_count = 0;

	/* initialized upon first use */
	/* mutex->owner_orig_prio = 0; */

	sys_dlist_init(&mutex->wait_q);

	SYS_TRACING_OBJ_INIT(k_mutex, mutex);
	INIT_OBJECT_MONITOR(mutex);
}

static int new_prio_for_inheritance(int target, int limit)
{
	int new_prio = _is_prio_higher(target, limit) ? target : limit;

	new_prio = _get_new_prio_with_ceiling(new_prio);

	return new_prio;
}

static void adjust_owner_prio(struct k_mutex *mutex, int new_prio)
{
	if (mutex->owner->base.prio != new_prio) {

		K_DEBUG("%p (ready (y/n): %c) prio changed to %d (was %d)\n",
			mutex->owner, _is_thread_ready(mutex->owner) ?
			'y' : 'n',
			new_prio, mutex->owner->base.prio);

		_thread_priority_set(mutex->owner, new_prio);
	}
}

int k_mutex_lock(struct k_mutex *mutex, int32_t timeout)
{
	int new_prio, key;

	_sched_lock();

	if (likely(mutex->lock_count == 0 || mutex->owner == _current)) {

		RECORD_STATE_CHANGE();

		mutex->owner_orig_prio = mutex->lock_count == 0 ?
					_current->base.prio :
					mutex->owner_orig_prio;

		mutex->lock_count++;
		mutex->owner = _current;

		K_DEBUG("%p took mutex %p, count: %d, orig prio: %d\n",
			_current, mutex, mutex->lock_count,
			mutex->owner_orig_prio);

		k_sched_unlock();

		return 0;
	}

	RECORD_CONFLICT();

	if (unlikely(timeout == K_NO_WAIT)) {
		k_sched_unlock();
		return -EBUSY;
	}

#if 0
	if (_is_prio_higher(_current->prio, mutex->owner->prio)) {
		new_prio = _current->prio;
	}
	new_prio = _get_new_prio_with_ceiling(new_prio);
#endif
	new_prio = new_prio_for_inheritance(_current->base.prio,
					    mutex->owner->base.prio);

	key = irq_lock();

	K_DEBUG("adjusting prio up on mutex %p\n", mutex);

	adjust_owner_prio(mutex, new_prio);

	_pend_current_thread(&mutex->wait_q, timeout);

	int got_mutex = _Swap(key);

	K_DEBUG("on mutex %p got_mutex value: %d\n", mutex, got_mutex);

	K_DEBUG("%p got mutex %p (y/n): %c\n", _current, mutex,
		got_mutex ? 'y' : 'n');

	if (got_mutex == 0) {
		k_sched_unlock();
		return 0;
	}

	/* timed out */

	K_DEBUG("%p timeout on mutex %p\n", _current, mutex);

	struct k_thread *waiter =
		(struct k_thread *)sys_dlist_peek_head(&mutex->wait_q);

	new_prio = mutex->owner_orig_prio;
	new_prio = waiter ? new_prio_for_inheritance(waiter->base.prio,
						     new_prio) : new_prio;

	K_DEBUG("adjusting prio down on mutex %p\n", mutex);

	key = irq_lock();
	adjust_owner_prio(mutex, new_prio);
	irq_unlock(key);

	k_sched_unlock();

	return -EAGAIN;
}

void k_mutex_unlock(struct k_mutex *mutex)
{
	int key;

	__ASSERT(mutex->lock_count > 0, "");
	__ASSERT(mutex->owner == _current, "");

	_sched_lock();

	RECORD_STATE_CHANGE();

	mutex->lock_count--;

	K_DEBUG("mutex %p lock_count: %d\n", mutex, mutex->lock_count);

	if (mutex->lock_count != 0) {
		k_sched_unlock();
		return;
	}

	key = irq_lock();

	adjust_owner_prio(mutex, mutex->owner_orig_prio);

	struct k_thread *new_owner = _unpend_first_thread(&mutex->wait_q);

	K_DEBUG("new owner of mutex %p: %p (prio: %d)\n",
		mutex, new_owner, new_owner ? new_owner->base.prio : -1000);

	if (new_owner) {
		_abort_thread_timeout(new_owner);
		_ready_thread(new_owner);

		irq_unlock(key);

		_set_thread_return_value(new_owner, 0);

		/*
		 * new owner is already of higher or equal prio than first
		 * waiter since the wait queue is priority-based: no need to
		 * ajust its priority
		 */
		mutex->owner = new_owner;
		mutex->lock_count++;
		mutex->owner_orig_prio = new_owner->base.prio;
	} else {
		irq_unlock(key);
		mutex->owner = NULL;
	}

	k_sched_unlock();
}
