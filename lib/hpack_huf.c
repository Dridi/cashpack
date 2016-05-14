/*-
 * Copyright (c) 2016 Dridi Boukelmoune
 * All rights reserved.
 *
 * Author: Dridi Boukelmoune <dridi.boukelmoune@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * HPACK Huffman Code (RFC 7541 Appendix B)
 */

#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hpack.h"
#include "hpack_priv.h"
#include "hpack_huf_idx.h"
#include "hpack_huf_tbl.h"

/**********************************************************************
 * Decode
 */

int
HPH_decode(HPACK_CTX, enum hpack_evt_e evt, size_t len)
{
	const struct hph_entry *he;
	struct hpack_state *hs;
	hpack_validate_f *val;
	uint32_t cod;
	char buf[256];
	unsigned eos, l;

	hs = &ctx->hp->state;

	if (hs->first) {
		CALLBACK(ctx, evt, NULL, len);
		hs->blen = 0;
		hs->bits = 0;
	}

	val = evt == HPACK_EVT_NAME ? HPV_token : HPV_value;

	eos = 0;
	l = 0;

	while (len > 0 || hs->blen > 0) {
		he = &hph_tbl[9]; /* the last 5-bit code */
		cod = UINT16_MAX;
		eos = 1;

		while (he != hph_tbl) {
			if (hs->blen < he->len) {
				if (len == 0)
					break;
				hs->bits = (hs->bits << 8) | *ctx->buf;
				hs->blen += 8;
				ctx->buf++;
				ctx->len--;
				len--;
			}
			cod = hs->bits >> (hs->blen - he->len);
			if (cod <= he->cod) {
				eos = 0;
				break;
			}
			he = &hph_tbl[he->nxt];
		}

		if (eos)
			break;

		assert(he->cod >= cod);
		he -= he->cod - cod;
		assert(he->cod == cod);

		assert(l < sizeof buf);
		buf[l] = he->chr;
		if (++l == sizeof buf) {
			CALL(val, ctx, (char *)buf, l, hs->first);
			CALLBACK(ctx, HPACK_EVT_DATA, buf, l);
			l = 0;
			hs->first = 0;
		}

		assert(hs->blen >= he->len);
		hs->blen -= he->len;
		hs->bits &= (1 << hs->blen) - 1;
	}

	if (l > 0) {
		CALL(val, ctx, (char *)buf, l, hs->first);
		CALLBACK(ctx, HPACK_EVT_DATA, buf, l);
	}

	EXPECT(ctx, HUF, len == 0); /* premature EOS */

	if (eos) {
		/* check padding */
		assert(hs->blen > 0);
		EXPECT(ctx, HUF, hs->blen < 8);
		EXPECT(ctx, HUF, hs->bits + 1 == (uint64_t)(1 << hs->blen));
	}
	else {
		/* no padding */
		assert(hs->bits == 0);
		assert(hs->blen == 0);
	}

	return (0);
}

void
HPH_encode(HPACK_CTX, const char *str)
{
	uint64_t bits;
	uint8_t buf[256], *cur;
	size_t sz, len, i;

	bits = 0;
	cur = buf;
	sz = 0;
	len = 0;

	while (*str != '\0') {
		i = hph_idx[(uint8_t)*str];
		bits = (bits << hph_tbl[i].len) | hph_tbl[i].cod;
		sz += hph_tbl[i].len;

		while (sz >= 8) {
			sz -= 8;
			*cur = (uint8_t)(bits >> sz);
			cur++;
			if (++len == sizeof buf) {
				HPE_push(ctx, buf, sizeof buf);
				cur = buf;
				len = 0;
			}
		}

		str++;
	}

	assert(sz < 8);
	if (sz > 0) {
		sz = 8 - sz; /* padding bits */
		bits <<= sz;
		bits |= (1 << sz) - 1;
		*cur = (uint8_t)bits;
		len++;
	}

	HPE_push(ctx, buf, len);
}

void
HPH_size(const char *str, size_t *res)
{
	uint16_t i;
	size_t sz;

	sz = 0;

	while (*str != '\0') {
		i = hph_idx[(uint16_t)*str];
		sz += hph_tbl[i].len;
		str++;
	}

	*res = sz >> 3;
	if (sz & 0x07)
		*res += 1;
}
