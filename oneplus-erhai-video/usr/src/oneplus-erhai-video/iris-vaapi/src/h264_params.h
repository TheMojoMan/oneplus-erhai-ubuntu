// SPDX-License-Identifier: GPL-2.0-or-later
/* Re-serialize H.264 SPS/PPS NAL units from VAPictureParameterBufferH264 so a
 * whole-bitstream stateful decoder (iris) can be fed reconstructed access
 * units. */

#ifndef IRIS_VAAPI_H264_PARAMS_H
#define IRIS_VAAPI_H264_PARAMS_H

#include <stdint.h>
#include <va/va.h>

/* Build an SPS NAL unit (without start code) from the picture parameter
 * buffer.  profile_idc is taken from the VA profile.  Returns the NAL size. */
int h264_build_sps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic, VAProfile profile,
		   unsigned int display_width, unsigned int display_height);

/* Build a PPS NAL unit (without start code).  Returns the NAL size.
 * num_ref_idx_l0/l1_default_active_minus1 come from the slice parameters. */
int h264_build_pps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic,
		   const VAIQMatrixBufferH264 *iq,
		   int num_ref_idx_l0_default_active_minus1,
		   int num_ref_idx_l1_default_active_minus1);

/* Return a bit mask describing which PPS defaults the slice actually uses:
 * bit 0 is list 0 and bit 1 is list 1.  A zero mask means that the slice
 * carries num_ref_idx_active_override_flag=1 (or is an intra slice). */
int h264_slice_default_ref_mask(const uint8_t *nal, size_t nal_size,
				const VAPictureParameterBufferH264 *pic);

#endif
