#include "lmc_common.glsl"
#include "lmc_store.glsl"
#include "lmc_color.glsl"
// One half-resolution warp per output. Pair motion/confidence are shared.
#ifdef BATCH_RECONSTRUCTION
layout(set=3,binding=0,rgba16f) uniform writeonly image2D output0;
layout(set=3,binding=1,rgba16f) uniform writeonly image2D output1;
layout(set=3,binding=2,rgba16f) uniform writeonly image2D output2;
layout(set=3,binding=3,rgba16f) uniform writeonly image2D output3;
float time_value(){return PC.batch_times[gl_GlobalInvocationID.z];}
void store_result(ivec2 p,vec4 c) {
    uint i=gl_GlobalInvocationID.z;
    if(i==0u)imageStore(output0,p,c);
    else if(i==1u)imageStore(output1,p,c);
    else if(i==2u)imageStore(output2,p,c);
    else imageStore(output3,p,c);
}
#else
layout(set=2,binding=0,rgba16f) uniform writeonly image2D reconstruction;
float time_value(){return PC.t;}
void store_result(ivec2 p,vec4 c){imageStore(reconstruction,p,c);}
#endif
layout(local_size_x=8,local_size_y=8) in;
vec4 flow(vec2 p) {
    ivec2 n=lblocks(1),b=clamp(ivec2(p/8.0),ivec2(0),n-1);
    return ORB.ob[b.y*n.x+b.x];
}
void main() {
    ivec2 q=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(q,P.size.zw)))return;
    vec2 p=vec2(q)*2.0+0.5;
    if((PC.flags&1)!=0 || G.pal_count!=0u) {
        store_result(q,vec4(composition_rgb(time_value()<0.5?0:1,p),0));return;
    }
    vec4 f=flow(p);vec2 v=f.xy*2.0;
    // Limit false coarse outliers. No full-resolution search.
    bool valid=all(lessThan(abs(v),vec2(min(256.0,float(min(P.size.x,P.size.y))/8.0)))) && !any(isnan(v)) && !any(isinf(v));
    if(!valid)v=vec2(0);
    vec2 a=p-time_value()*v,b=p+(1.0-time_value())*v;
    bool ia=inside(a,P.size.xy),ib=inside(b,P.size.xy);
    float consistency=length(flow(b).xy-f.xy);
    float confidence=valid?clamp(1.0-f.z*6.0-consistency*0.03,0.0,1.0):0.0;
    vec3 ca=composition_rgb(0,ia?a:p),cb=composition_rgb(1,ib?b:p);
    // Per-time visibility: only blend mutually visible, reliable trajectories.
    vec3 c=ia&&ib?mix(ca,cb,time_value()):ia?ca:cb;
    if(ia&&ib && (confidence<0.3 || f.z>0.01)) {
        // A bad vector must not warp both endpoints to unrelated surfaces.
        // Conservative unwarped temporal blending limits contour tearing;
        // moderate ghosts are preferable to a confident false large motion.
        vec3 ua=composition_rgb(0,p),ub=composition_rgb(1,p);
        c=mix(ua,ub,time_value());confidence=0.0;
    }
    store_result(q,vec4(c,confidence));
}
