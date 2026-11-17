// SPDX-License-Identifier: GPL-2.0-or-later

#include <string.h>

#include "av1_bits.h"

void
av1_bw_init(struct av1_bw *bw, void *buf, size_t cap)
{
	memset(bw, 0, sizeof(*bw));
	bw->buf = buf;
	bw->cap = cap;
}

void
av1_bw_bit(struct av1_bw *bw, unsigned int bit)
{
	if (bw->error)
		return;
	if (bw->bit == 0) {
		if (bw->pos >= bw->cap) {
			bw->error = 1;
			return;
		}
		bw->buf[bw->pos] = 0;
	}
	/* AV1 f(n) is most significant bit first. */
	bw->buf[bw->pos] = (uint8_t)((bw->buf[bw->pos] << 1) | (bit & 1));
	if (++bw->bit == 8) {
		bw->bit = 0;
		bw->pos++;
	}
}

void
av1_bw_bits(struct av1_bw *bw, uint32_t value, unsigned int nbits)
{
	unsigned int i;

	if (nbits > 32)
		nbits = 32;
	for (i = nbits; i > 0; i--)
		av1_bw_bit(bw, (value >> (i - 1)) & 1);
}

void
av1_bw_su(struct av1_bw *bw, int32_t value, unsigned int nbits)
{
	if (nbits > 32)
		nbits = 32;
	if (nbits == 0)
		return;
	/* su(n) is the low n bits of the two's complement representation. */
	av1_bw_bits(bw, (uint32_t)value & ((nbits >= 32) ? 0xffffffffu :
					   ((1u << nbits) - 1)), nbits);
}

void
av1_bw_ns(struct av1_bw *bw, unsigned int value, unsigned int n)
{
	unsigned int w, m, v;

	if (n <= 1)
		return;
	w = av1_floor_log2(n) + 1;
	m = (1u << w) - n;
	/* w >= 1 here, and w - 1 bits are always available. */
	v = (value + m) >> 1;
	if (value < m) {
		av1_bw_bits(bw, value, w - 1);
	} else {
		av1_bw_bits(bw, v, w - 1);
		av1_bw_bit(bw, value - ((v << 1) - m));
	}
}

void
av1_bw_align(struct av1_bw *bw)
{
	while (bw->bit)
		av1_bw_bit(bw, 0);
}

void
av1_bw_trailing_bits(struct av1_bw *bw)
{
	av1_bw_bit(bw, 1);
	av1_bw_align(bw);
}

size_t
av1_bw_size(const struct av1_bw *bw)
{
	return bw->pos + (bw->bit ? 1 : 0);
}

size_t
av1_leb128_size(uint64_t value)
{
	size_t n = 1;

	while (value >= 0x80) {
		value >>= 7;
		n++;
	}
	return n;
}

size_t
av1_leb128_write(uint8_t *out, uint64_t value)
{
	size_t n = 0;

	do {
		uint8_t byte = (uint8_t)(value & 0x7f);

		value >>= 7;
		if (value)
			byte |= 0x80;
		out[n++] = byte;
	} while (value);
	return n;
}

unsigned int
av1_bit_width(uint32_t value)
{
	unsigned int n = 1;

	while (n < 32 && (value >> n))
		n++;
	return n;
}

unsigned int
av1_floor_log2(uint32_t value)
{
	unsigned int n = 0;

	while (value > 1) {
		value >>= 1;
		n++;
	}
	return n;
}
