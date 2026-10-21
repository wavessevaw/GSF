// SPDX-License-Identifier: GPL-3.0-or-later
//
// SMAA's precomputed lookup textures (smaa/AreaTex.h and smaa/SearchTex.h, MIT, from
// iryoku/smaa 71c806a). The area texture is RG8; hosts that keep every filter image RGBA8
// expand it with kfg_smaa_area_rgba8.
#ifndef KFG_SMAA_TEX_H
#define KFG_SMAA_TEX_H

#include <stdint.h>

#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

#define KFG_SMAA_AREA_W AREATEX_WIDTH
#define KFG_SMAA_AREA_H AREATEX_HEIGHT
#define KFG_SMAA_SEARCH_W SEARCHTEX_WIDTH
#define KFG_SMAA_SEARCH_H SEARCHTEX_HEIGHT

// out: AREATEX_WIDTH * AREATEX_HEIGHT * 4 bytes.
static inline void kfg_smaa_area_rgba8(uint8_t *out)
{
	for (uint32_t i = 0; i < (uint32_t)(AREATEX_WIDTH * AREATEX_HEIGHT); i++) {
		out[4 * i + 0] = areaTexBytes[2 * i + 0];
		out[4 * i + 1] = areaTexBytes[2 * i + 1];
		out[4 * i + 2] = 0;
		out[4 * i + 3] = 255;
	}
}

static inline const uint8_t *kfg_smaa_search_r8(void)
{
	return searchTexBytes;
}

#endif
