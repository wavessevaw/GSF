// Storage views of the newest frame's pyramid.  LUMA_FMT is r16f or r32f,
// chosen at build time; the engine picks the variant the device supports.
layout(set = 0, binding = 17, LUMA_FMT) uniform image2D storeL0;
layout(set = 0, binding = 18, LUMA_FMT) uniform image2D storeL1;
layout(set = 0, binding = 19, LUMA_FMT) uniform image2D storeL2;
layout(set = 0, binding = 20, LUMA_FMT) uniform image2D storeL3;

float load_luma(int l, ivec2 p) {
    if (l == 0) return imageLoad(storeL0, p).r;
    if (l == 1) return imageLoad(storeL1, p).r;
    if (l == 2) return imageLoad(storeL2, p).r;
    return imageLoad(storeL3, p).r;
}

void store_luma(int l, ivec2 p, float v) {
    if (l == 0) imageStore(storeL0, p, vec4(v));
    else if (l == 1) imageStore(storeL1, p, vec4(v));
    else if (l == 2) imageStore(storeL2, p, vec4(v));
    else imageStore(storeL3, p, vec4(v));
}
