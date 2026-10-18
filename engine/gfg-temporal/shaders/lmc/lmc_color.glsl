// ST 2084, RGB in the unchanged swapchain primaries. Cache absolute nits in
// FP16: normalized 0..1 FP16 would discard very dark PQ values as subnormals.
// Motion/confidence stay in perceptual code values; reconstruction/filtering
// use linear-light samples. No tone map or gamut conversion is applied.
#ifdef GFG_LINEAR_PQ
layout(set=0,binding=36) uniform sampler2D linearA;
layout(set=0,binding=37) uniform sampler2D linearB;

vec3 pq_decode_nits(vec3 code) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    vec3 v=pow(clamp(code,0.0,1.0),vec3(1.0/m2));
    return 10000.0*pow(max(v-c1,0.0)/(c2-c3*v),vec3(1.0/m1));
}
vec3 composition_encode(vec3 nits) {
    const float m1=2610.0/16384.0,m2=2523.0/32.0;
    const float c1=3424.0/4096.0,c2=2413.0/128.0,c3=2392.0/128.0;
    vec3 v=pow(clamp(nits/10000.0,0.0,1.0),vec3(m1));
    return pow((c1+c2*v)/(1.0+c3*v),vec3(m2));
}
vec3 composition_rgb(int which,vec2 q) {
    vec2 s=vec2(P.size.xy),uv=(clamp(q,vec2(0.0),s-1.0)+0.5)/s;
    return which==0?textureLod(linearA,uv,0.0).rgb:textureLod(linearB,uv,0.0).rgb;
}
#else
vec3 composition_encode(vec3 code) { return code; }
vec3 composition_rgb(int which,vec2 q) { return full_rgb(which,q); }
#endif
