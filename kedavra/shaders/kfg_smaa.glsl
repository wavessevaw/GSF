// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA: SMAA 1x (Jimenez et al., MIT, smaa/SMAA.hlsl) run as three compute passes.
// Settings follow vkBasalt's defaults: luma edges, threshold 0.05, 32 orthogonal and
// 16 diagonal search steps, corner rounding 25.

#extension GL_GOOGLE_include_directive : require
#include "kfg_common.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

#define SMAA_GLSL_4 1
#define SMAA_RT_METRICS vec4(1.0 / vec2(u_dstSize), vec2(u_dstSize))
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 32
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#include "smaa/SMAA.hlsl"

// Texture coordinate and pixel coordinate of this invocation; false outside the image.
bool kfg_smaa_coords(out vec2 texcoord, out vec2 pixcoord)
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	texcoord = (vec2(p) + 0.5) / vec2(u_dstSize);
	pixcoord = texcoord * vec2(u_dstSize);
	return all(lessThan(p, u_dstSize));
}
