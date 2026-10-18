// Storage views written by the per-pair passes (newest frame B only).
layout(set = 0, binding = 13, r32f) uniform image2D storeL0;
layout(set = 0, binding = 14, r32f) uniform image2D storeL1;
layout(set = 0, binding = 15, r32f) uniform image2D storeL2;
layout(set = 0, binding = 16, r32f) uniform image2D storeL3;
layout(set = 0, binding = 17, r32f) uniform image2D storeL4;
layout(set = 0, binding = 20, rgba16f) uniform image2D storeHalf;
layout(set = 0, binding = 21, r32ui) uniform uimage2D hudCount;

float load_l(int l, ivec2 p) {
    if (l == 0) return imageLoad(storeL0, p).r;
    if (l == 1) return imageLoad(storeL1, p).r;
    if (l == 2) return imageLoad(storeL2, p).r;
    if (l == 3) return imageLoad(storeL3, p).r;
    return imageLoad(storeL4, p).r;
}

void store_l(int l, ivec2 p, float v) {
    if (l == 0) imageStore(storeL0, p, vec4(v));
    else if (l == 1) imageStore(storeL1, p, vec4(v));
    else if (l == 2) imageStore(storeL2, p, vec4(v));
    else if (l == 3) imageStore(storeL3, p, vec4(v));
    else imageStore(storeL4, p, vec4(v));
}
layout(set = 0, binding = 34, rgba16f) uniform image2D storeFieldB;
layout(set = 0, binding = 35, rgba16f) uniform image2D storeFieldA;
