/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/virtual_source.h"
#include <assert.h>
#include <stdio.h>
static void single_image_reuse(void) {
    gfg_virtual_source v;assert(gfg_virtual_init(&v,1));
    unsigned slot=99;uint64_t ticket=0;
    assert(gfg_virtual_acquire(&v,&slot) && slot==0);
    assert(!gfg_virtual_acquire(&v,&slot) && v.blocked==1);
    assert(gfg_virtual_begin_snapshot(&v,0,&ticket) && ticket==1);
    assert(!gfg_virtual_acquire(&v,&slot));
    assert(!gfg_virtual_release(&v,0,ticket,0));
    assert(!gfg_virtual_release(&v,0,ticket+1,1));
    assert(gfg_virtual_release(&v,0,ticket,1));
    assert(!gfg_virtual_release(&v,0,ticket,1));
    assert(gfg_virtual_acquire(&v,&slot) && slot==0);
    assert(gfg_virtual_cancel_acquire(&v,slot));
    assert(gfg_virtual_all_idle(&v));
    assert(v.acquired==2 && v.retired==1);
    gfg_virtual_close(&v);
    assert(!gfg_virtual_acquire(&v,&slot));
    puts("virtual-source single image: blocked until GPU snapshot retired PASS");
}
static void concurrent_image_release(void) {
    gfg_virtual_source v;assert(gfg_virtual_init(&v,2));
    unsigned a,b,c;uint64_t ta,tb,tc;
    assert(gfg_virtual_acquire(&v,&a) && a==1);
    assert(gfg_virtual_acquire(&v,&b) && b==0);
    assert(!gfg_virtual_acquire(&v,&c));
    assert(gfg_virtual_begin_snapshot(&v,a,&ta));
    assert(gfg_virtual_begin_snapshot(&v,b,&tb));
    assert(!gfg_virtual_release(&v,a,ta,0));
    assert(!gfg_virtual_acquire(&v,&c));
    assert(gfg_virtual_release(&v,b,tb,1));
    assert(gfg_virtual_acquire(&v,&c) && c==b);
    assert(gfg_virtual_begin_snapshot(&v,c,&tc));
    assert(gfg_virtual_release(&v,a,ta,1));
    assert(gfg_virtual_release(&v,c,tc,1));
    assert(gfg_virtual_all_idle(&v) && v.retired==3);
    puts("virtual-source two images: out-of-order snapshot release PASS");
}
static void stress(void) {
    for(unsigned images=1;images<=GFG_VIRTUAL_MAX_IMAGES;++images) {
        gfg_virtual_source v;assert(gfg_virtual_init(&v,images));
        for(unsigned i=0;i<10000;++i) {
            unsigned idx;uint64_t id;
            assert(gfg_virtual_acquire(&v,&idx));
            assert(gfg_virtual_begin_snapshot(&v,idx,&id));
            assert(gfg_virtual_release(&v,idx,id,1));
        }
        assert(v.retired==10000 && v.acquired==10000);
        assert(gfg_virtual_all_idle(&v) && !v.invalid);
    }
    assert(!gfg_virtual_init(NULL,2));
    gfg_virtual_source bad;
    assert(!gfg_virtual_init(&bad,0));
    assert(!gfg_virtual_init(&bad,GFG_VIRTUAL_MAX_IMAGES+1));
    puts("virtual-source 1..8 images 10000 cycles each PASS");
}
int main(void) {
    single_image_reuse();concurrent_image_release();stress();
    return 0;
}
