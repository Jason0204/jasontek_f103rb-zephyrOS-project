/*
 * Copyright (c) 2011-2016 Wind River Systems, Inc.
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
 *
 * Dining philosophers demo for unified kernel.
 *
 * The demo can be configured to use different object types for its
 * synchronization: SEMAPHORES, MUTEXES, STACKS, FIFOS and LIFOS. To configure
 * a specific object, set the value of FORKS to one of these.
 *
 * By default, the demo uses MUTEXES.
 *
 * The demo can also be configured to work with static objects or dynamic
 * objects. The behaviour will change depending if STATIC_OBJS is set to 0 or
 * 1.
 *
 * By default, the demo uses dynamic objects.
 *
 * The demo can be configured to work with threads of the same priority or
 * not. If using different priorities, two threads will be cooperative
 * threads, and the other four will be preemtible threads; if using one
 * priority, there will be six preemtible threads of priority 0. This is
 * changed via SAME_PRIO.
 *
 * By default, the demo uses different priorities.
 *
 * The number of threads is set via NUM_PHIL. The demo has only been tested
 * with six threads. In theory it should work with any number of threads, but
 * not without making changes to the forks[] array in the phil_obj_abstract.h
 * header file.
 */

extern void phildemo_main(void);

