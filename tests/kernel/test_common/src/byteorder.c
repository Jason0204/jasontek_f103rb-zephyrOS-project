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


#include <ztest.h>

#include <misc/byteorder.h>

void byteorder_test_memcpy_swap(void)
{
	uint8_t buf_orig[8] = { 0x00, 0x01, 0x02, 0x03,
				0x04, 0x05, 0x06, 0x07 };
	uint8_t buf_chk[8] = { 0x07, 0x06, 0x05, 0x04,
			       0x03, 0x02, 0x01, 0x00 };
	uint8_t buf_dst[8] = { 0 };

	sys_memcpy_swap(buf_dst, buf_orig, 8);
	assert_true((memcmp(buf_dst, buf_chk, 8) == 0),
		    "Swap memcpy failed");

	sys_memcpy_swap(buf_dst, buf_chk, 8);
	assert_true((memcmp(buf_dst, buf_orig, 8) == 0),
		    "Swap memcpy failed");
}

void byteorder_test_mem_swap(void)
{
	uint8_t buf_orig_1[8] = { 0x00, 0x01, 0x02, 0x03,
				  0x04, 0x05, 0x06, 0x07 };
	uint8_t buf_orig_2[11] = { 0x00, 0x01, 0x02, 0x03,
				   0x04, 0x05, 0x06, 0x07,
				   0x08, 0x09, 0xa0 };
	uint8_t buf_chk_1[8] = { 0x07, 0x06, 0x05, 0x04,
				 0x03, 0x02, 0x01, 0x00 };
	uint8_t buf_chk_2[11] = { 0xa0, 0x09, 0x08, 0x07,
				  0x06, 0x05, 0x04, 0x03,
				  0x02, 0x01, 0x00 };

	sys_mem_swap(buf_orig_1, 8);
	assert_true((memcmp(buf_orig_1, buf_chk_1, 8) == 0),
		    "Swapping buffer failed");

	sys_mem_swap(buf_orig_2, 11);
	assert_true((memcmp(buf_orig_2, buf_chk_2, 11) == 0),
		    "Swapping buffer failed");
}

