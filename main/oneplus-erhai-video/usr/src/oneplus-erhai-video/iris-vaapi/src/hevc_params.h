// SPDX-License-Identifier: GPL-2.0-or-later
/* Re-serialize HEVC VPS/SPS/PPS NAL units from VAPictureParameterBufferHEVC
 * for the whole-bitstream stateful iris decoder. */

#ifndef IRIS_VAAPI_HEVC_PARAMS_H
#define IRIS_VAAPI_HEVC_PARAMS_H

#include <stdint.h>
#include <stddef.h>
#include <va/va.h>

/* Build a VPS NAL unit (without start code).  Returns the NAL size. */
int hevc_build_vps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferHEVC *pic);
/* Build an SPS NAL unit (without start code).  Returns the NAL size. */
int hevc_build_sps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferHEVC *pic,
		   const VAIQMatrixBufferHEVC *iq);
/* Build a PPS NAL unit (without start code).  Returns the NAL size. */
int hevc_build_pps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferHEVC *pic);

/* Preserve the PPS id parsed from the original slice while linking it to the
 * canonical SPS id zero. */
int hevc_build_pps_id(uint8_t *out, size_t out_size,
		      const VAPictureParameterBufferHEVC *pic,
		      unsigned int pps_id);

#endif
