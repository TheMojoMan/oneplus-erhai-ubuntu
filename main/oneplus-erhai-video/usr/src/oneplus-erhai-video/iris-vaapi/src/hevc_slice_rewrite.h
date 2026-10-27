// SPDX-License-Identifier: GPL-2.0-or-later
/* Rewrite HEVC slice headers so reconstructed SPS files do not need the
 * original SPS short-term reference-picture-set tables. */

#ifndef IRIS_VAAPI_HEVC_SLICE_REWRITE_H
#define IRIS_VAAPI_HEVC_SLICE_REWRITE_H

#include <stddef.h>
#include <stdint.h>
#include <va/va.h>

/* Rewrite one HEVC NAL unit (without an Annex-B start code).
 *
 * Returns the rewritten NAL size, or a negative errno-style value. The PPS id
 * parsed from the original slice is returned through pps_id. A positive return
 * does not imply that bytes changed: already-compatible and dependent/IDR
 * slices are copied verbatim.
 */
int hevc_rewrite_slice(uint8_t *out, size_t out_size,
		       const uint8_t *nal, size_t nal_size,
		       const VAPictureParameterBufferHEVC *pic,
		       const VASliceParameterBufferHEVC *slice,
		       unsigned int *pps_id);

#endif
