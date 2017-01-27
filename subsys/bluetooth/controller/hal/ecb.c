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

#include <string.h>
#include <soc.h>

#include "mem.h"
#include "ecb.h"

#include "debug.h"

void ecb_encrypt(uint8_t const *const key_le,
		 uint8_t const *const clear_text_le,
		 uint8_t * const cipher_text_le,
		 uint8_t * const cipher_text_be)
{
	struct {
		uint8_t key[16];
		uint8_t clear_text[16];
		uint8_t cipher_text[16];
	} __packed ecb;

	mem_rcopy(&ecb.key[0], key_le, sizeof(ecb.key));
	mem_rcopy(&ecb.clear_text[0], clear_text_le, sizeof(ecb.clear_text));

	do {
		NRF_ECB->TASKS_STOPECB = 1;
		NRF_ECB->ECBDATAPTR = (uint32_t) &ecb;
		NRF_ECB->EVENTS_ENDECB = 0;
		NRF_ECB->EVENTS_ERRORECB = 0;
		NRF_ECB->TASKS_STARTECB = 1;
		while ((NRF_ECB->EVENTS_ENDECB == 0) &&
				(NRF_ECB->EVENTS_ERRORECB == 0) &&
				(NRF_ECB->ECBDATAPTR != 0)) {
			/*__WFE();*/
		}
		NRF_ECB->TASKS_STOPECB = 1;
	} while ((NRF_ECB->EVENTS_ERRORECB != 0) || (NRF_ECB->ECBDATAPTR == 0));

	NRF_ECB->ECBDATAPTR = 0;

	if (cipher_text_le) {
		mem_rcopy(cipher_text_le, &ecb.cipher_text[0],
			  sizeof(ecb.cipher_text));
	}

	if (cipher_text_be) {
		memcpy(cipher_text_be, &ecb.cipher_text[0],
			 sizeof(ecb.cipher_text));
	}
}

uint32_t ecb_encrypt_nonblocking(struct ecb *ecb)
{
	/* prepare to be used in a BE AES h/w */
	if (ecb->in_key_le) {
		mem_rcopy(&ecb->in_key_be[0], ecb->in_key_le,
			  sizeof(ecb->in_key_be));
	}
	if (ecb->in_clear_text_le) {
		mem_rcopy(&ecb->in_clear_text_be[0],
			  ecb->in_clear_text_le,
			  sizeof(ecb->in_clear_text_be));
	}

	/* setup the encryption h/w */
	NRF_ECB->ECBDATAPTR = (uint32_t)ecb;
	NRF_ECB->EVENTS_ENDECB = 0;
	NRF_ECB->EVENTS_ERRORECB = 0;
	NRF_ECB->INTENSET = ECB_INTENSET_ERRORECB_Msk | ECB_INTENSET_ENDECB_Msk;

	/* enable interrupt */
	_NvicIrqUnpend(ECB_IRQn);
	irq_enable(ECB_IRQn);

	/* start the encryption h/w */
	NRF_ECB->TASKS_STARTECB = 1;

	return 0;
}

static void ecb_cleanup(void)
{
	/* stop h/w */
	NRF_ECB->TASKS_STOPECB = 1;

	/* cleanup interrupt */
	irq_disable(ECB_IRQn);
}

void ecb_isr(void)
{
	if (NRF_ECB->EVENTS_ERRORECB) {
		struct ecb *ecb = (struct ecb *)NRF_ECB->ECBDATAPTR;

		ecb_cleanup();

		ecb->fp_ecb(1, 0, ecb->context);
	}

	else if (NRF_ECB->EVENTS_ENDECB) {
		struct ecb *ecb = (struct ecb *)NRF_ECB->ECBDATAPTR;

		ecb_cleanup();

		ecb->fp_ecb(0, &ecb->out_cipher_text_be[0],
			      ecb->context);
	}

	else {
		LL_ASSERT(0);
	}
}

struct ecb_ut_context {
	uint32_t volatile done;
	uint32_t status;
	uint8_t cipher_text[16];
};

static void ecb_cb(uint32_t status, uint8_t *cipher_be, void *context)
{
	struct ecb_ut_context *ecb_ut_context =
		(struct ecb_ut_context *)context;

	ecb_ut_context->done = 1;
	ecb_ut_context->status = status;
	if (!status) {
		mem_rcopy(ecb_ut_context->cipher_text, cipher_be,
			  sizeof(ecb_ut_context->cipher_text));
	}
}

uint32_t ecb_ut(void)
{
	uint8_t key[16] = {
	    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00,
	0x11, 0x22, 0x33, 0x44, 0x55 };
	uint8_t clear_text[16] = {
	    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00,
	0x11, 0x22, 0x33, 0x44, 0x55 };
	uint8_t cipher_text[16];
	uint32_t status = 0;
	struct ecb ecb;
	struct ecb_ut_context context;

	ecb_encrypt(key, clear_text, cipher_text, 0);

	context.done = 0;
	ecb.in_key_le = key;
	ecb.in_clear_text_le = clear_text;
	ecb.fp_ecb = ecb_cb;
	ecb.context = &context;
	status = ecb_encrypt_nonblocking(&ecb);
	do {
		__WFE();
		__SEV();
		__WFE();
	} while (!context.done);

	if (context.status != 0) {
		return context.status;
	}

	status = memcmp(cipher_text, context.cipher_text, sizeof(cipher_text));
	if (status) {
		return status;
	}

	return status;
}
