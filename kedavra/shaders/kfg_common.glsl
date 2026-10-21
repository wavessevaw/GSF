// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation: shared declarations.
//
// The descriptor interface is a subset of Gamescope's compute layout
// (src/shaders/descriptor_set.h): binding 0 = uniform constants (scalar),
// binding 1 = storage target, binding 3 = sampled textures. The same SPIR-V
// therefore runs inside Gamescope and in the standalone Vulkan test harness.

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_scalar_block_layout : require

#define KFG_SAMPLER_SLOTS 16

layout(binding = 1) writeonly uniform image2D dst;
layout(binding = 3) uniform sampler2D s_samplers[KFG_SAMPLER_SLOTS];

// One constant block for every pass; unused fields are ignored.
layout(binding = 0, scalar) uniform kfg_params_t {
	ivec2 u_dstSize;   // dispatch domain (texels of dst)
	ivec2 u_srcSize;   // size of slot-0 input in texels
	ivec2 u_fullSize;  // full-resolution source size in pixels
	float u_t;         // generated phase in (0, 1)
	int u_level;       // pyramid level of this pass (texel = 2^level pixels)
	int u_radius;      // search radius in texels at this level
	int u_flags;
	float u_lambda;    // motion smoothness weight
	float u_cutThreshold;
	float u_pad0;
	float u_pad1;
};

#define KFG_FLAG_COARSEST 1
#define KFG_FLAG_DEBUG_MARKER 2
#define KFG_FLAG_TEMPORAL 16
#define KFG_FLAG_GLOBAL 32

// Pair statistics image (4x1): texel 0 = cut and match statistics; texels 1 to 3 = camera
// motion as affine fields over normalised image coordinates: texel 1 = motion at the image
// centre (xy forward, zw backward, pixels), texels 2 and 3 = forward and backward gradients
// (x: xy, y: zw, pixels per half image).
#define KFG_GLOBAL_TEXEL ivec2(1, 0)

float kfg_luma(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Sample a texture at a texel-space position with the sampler's (linear) filter.
vec4 kfg_tap(int slot, vec2 texel_pos, ivec2 size)
{
	vec2 uv = (texel_pos + 0.5) / vec2(size);
	return textureLod(s_samplers[slot], uv, 0.0);
}

// Camera motion at a full-resolution pixel position: xy forward, zw backward, pixels.
vec4 kfg_global_at(int slot, vec2 pix)
{
	vec2 h = 0.5 * vec2(u_fullSize);
	vec2 u = (pix - h) / h;
	vec4 c = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL, 0);
	vec4 mf = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL + ivec2(1, 0), 0);
	vec4 mb = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL + ivec2(2, 0), 0);
	return c + vec4(dot(mf.xy, u), dot(mf.zw, u), dot(mb.xy, u), dot(mb.zw, u));
}
