// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal AV1 bitstream writer.
 *
 * VA-API hands AV1 to the driver as per-tile CABAC payloads plus fully parsed
 * picture parameters, but the stateful Iris firmware wants a complete OBU
 * access unit.  Only the uncompressed headers have to be produced here: the
 * bit fields defined by the AV1 specification (f(n), su(n), ns(n), uvlc and
 * leb128).  The tile payload is already entropy coded and is copied verbatim.
 */

#ifndef IRIS_VAAPI_AV1_BITS_H
#define IRIS_VAAPI_AV1_BITS_H

#include <stddef.h>
#include <stdint.h>

/* MSB-first bit sink over a caller-supplied byte buffer. */
struct av1_bw {
	uint8_t *buf;
	size_t cap;		/* capacity in bytes */
	size_t pos;		/* complete bytes written */
	unsigned int bit;	/* bits used in the byte at buf[pos], 0..7 */
	int error;		/* set on overflow; writes become no-ops */
};

void av1_bw_init(struct av1_bw *bw, void *buf, size_t cap);
void av1_bw_bit(struct av1_bw *bw, unsigned int bit);
/* f(n): n bits, most significant first. n <= 32. */
void av1_bw_bits(struct av1_bw *bw, uint32_t value, unsigned int nbits);
/* su(n): n-bit two's-complement-ish signed value. n <= 32. */
void av1_bw_su(struct av1_bw *bw, int32_t value, unsigned int nbits);
/* ns(n): non-symmetric unsigned value in [0, n). */
void av1_bw_ns(struct av1_bw *bw, unsigned int value, unsigned int n);
/* Pad with zero bits to the next byte boundary. */
void av1_bw_align(struct av1_bw *bw);
/* trailing_bits(): one bit set to 1, then zeros to the next byte boundary. */
void av1_bw_trailing_bits(struct av1_bw *bw);
/* Bytes written so far, rounding a partial byte up. */
size_t av1_bw_size(const struct av1_bw *bw);

/* Byte length of the leb128 encoding of @value. */
size_t av1_leb128_size(uint64_t value);
/* Write the leb128 encoding of @value; returns the byte count. */
size_t av1_leb128_write(uint8_t *out, uint64_t value);

/* Number of bits needed to represent @value (at least 1). */
unsigned int av1_bit_width(uint32_t value);
/* FloorLog2 as defined by the specification (0 for 0). */
unsigned int av1_floor_log2(uint32_t value);

#endif /* IRIS_VAAPI_AV1_BITS_H */
