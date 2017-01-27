/* mailbox_b.c */

/*
 * Copyright (c) 1997-2010, 2013-2014 Wind River Systems, Inc.
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

#include "master.h"

#ifdef MAILBOX_BENCH

static struct k_msg Message;

#ifdef FLOAT
#define PRINT_HEADER()                                                       \
	PRINT_STRING                                                             \
	   ("|   size(B) |       time/packet (usec)       |          MB/sec" \
	    "                |\n", output_file);
#define PRINT_ONE_RESULT()                                                   \
	PRINT_F(output_file, "|%11lu|%32.3f|%32f|\n", putsize, puttime / 1000.0, \
	     (1000.0 * putsize) / puttime)

#define PRINT_OVERHEAD()                                                     \
	PRINT_F(output_file,						\
	     "| message overhead:  %10.3f     usec/packet                   "\
	     "            |\n", EmptyMsgPutTime / 1000.0)

#define PRINT_XFER_RATE()                                                     \
	double NettoTransferRate;                                                 \
	NettoTransferRate = 1000.0 * (putsize >> 1) / (puttime - EmptyMsgPutTime);\
	PRINT_F(output_file,						\
	     "| raw transfer rate:     %10.3f MB/sec (without"		\
	     " overhead)                 |\n", NettoTransferRate)

#else
#define PRINT_HEADER()                                                       \
	PRINT_STRING                                                             \
	   ("|   size(B) |       time/packet (nsec)       |          KB/sec" \
	    "                |\n", output_file);

#define PRINT_ONE_RESULT()                                                   \
	PRINT_F(output_file, "|%11lu|%32lu|%32lu|\n", putsize, puttime,	\
	     (uint32_t)((1000000 * (uint64_t)putsize) / puttime))

#define PRINT_OVERHEAD()                                                     \
	PRINT_F(output_file,						\
	     "| message overhead:  %10u     nsec/packet                     "\
	     "          |\n", EmptyMsgPutTime)

#define PRINT_XFER_RATE()                                                    \
	PRINT_F(output_file, "| raw transfer rate:     %10lu KB/sec (without" \
	     " overhead)                 |\n",                               \
	     (uint32_t)(1000000 * (uint64_t)(putsize >> 1)                   \
	     / (puttime - EmptyMsgPutTime)))

#endif


/*
 * Function prototypes.
 */
void mailbox_put(uint32_t size, int count, uint32_t *time);

/*
 * Function declarations.
 */

/**
 *
 * @brief Mailbox transfer speed test
 *
 * @return N/A
 */
void mailbox_test(void)
{
	uint32_t putsize;
	uint32_t puttime;
	int putcount;
	unsigned int EmptyMsgPutTime;
	GetInfo getinfo;

	PRINT_STRING(dashline, output_file);
	PRINT_STRING("|                "
				 "M A I L B O X   M E A S U R E M E N T S"
				 "                      |\n", output_file);
	PRINT_STRING(dashline, output_file);
	PRINT_STRING("| Send mailbox message to waiting high "
				 "priority task and wait                 |\n", output_file);
	PRINT_F(output_file, "| repeat for %4d times and take the "
			"average                                  |\n",
			NR_OF_MBOX_RUNS);
	PRINT_STRING(dashline, output_file);
	PRINT_HEADER();
	PRINT_STRING(dashline, output_file);
	task_sem_reset(SEM0);
	task_sem_give(STARTRCV);

	putcount = NR_OF_MBOX_RUNS;

	putsize = 0;
	mailbox_put(putsize, putcount, &puttime);
	/* waiting for ack */
	task_fifo_get(MB_COMM, &getinfo, TICKS_UNLIMITED);
	PRINT_ONE_RESULT();
	EmptyMsgPutTime = puttime;
	for (putsize = 8; putsize <= MESSAGE_SIZE; putsize <<= 1) {
		mailbox_put(putsize, putcount, &puttime);
		/* waiting for ack */
		task_fifo_get(MB_COMM, &getinfo, TICKS_UNLIMITED);
		PRINT_ONE_RESULT();
	}
	PRINT_STRING(dashline, output_file);
	PRINT_OVERHEAD();
	PRINT_XFER_RATE();
}


/**
 *
 * @brief Write the number of data chunks into the mailbox
 *
 * @param size    The size of the data chunk.
 * @param count   Number of data chunks.
 * @param time    The total time.
 *
 * @return N/A
 */
void mailbox_put(uint32_t size, int count, uint32_t *time)
{
	int i;
	unsigned int t;

	Message.rx_task = ANYTASK;
	Message.tx_data = data_bench;
	Message.size = size;

	/* first sync with the receiver */
	task_sem_give(SEM0);
	t = BENCH_START();
	for (i = 0; i < count; i++) {
		task_mbox_put(MAILB1, 1, &Message, TICKS_UNLIMITED);
	}
	t = TIME_STAMP_DELTA_GET(t);
	*time = SYS_CLOCK_HW_CYCLES_TO_NS_AVG(t, count);
	check_result();
}

#endif /* MAILBOX_BENCH */
