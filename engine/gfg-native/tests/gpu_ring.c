/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the actual bridge reservation with deterministic fence states.
 * A skipped real frame must not hide a free slot, reset a pending fence or
 * disguise a device error as ordinary GPU backpressure. No live ICD needed.
 */
#include "../src/gpu_bridge.c"
#include <assert.h>

static VkResult states[RING_MAX];
static unsigned polls[RING_MAX];
static VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice device, VkFence fence) {
    (void)device;
    unsigned i = (unsigned)((uintptr_t)fence - 1);
    assert(i < RING_MAX);
    ++polls[i];
    return states[i];
}

static gfg_gpu_fg fixture(unsigned n) {
    gfg_gpu_fg fg = {.ring_size = n, .GetFenceStatus = fence_status};
    for (unsigned i = 0; i < n; ++i) {
        fg.ring[i].fence = (VkFence)(uintptr_t)(i + 1);
        fg.ring[i].busy = 1;
        states[i] = VK_NOT_READY;
        polls[i] = 0;
    }
    return fg;
}

int main(void) {
    /* Actual TE packet preparation: expiration must remove GPU work, keep
     * history synchronization, and compact G2-only outputs without confusing
     * its 2/3 timestamp or enabling a deferred nonexistent G1. */
    gfg_gpu_fg plan={.same_device=1,.multiplier=3,.shared_flow=1,.display_timing=2,
        .te_slot=1,.te_frames=5,.clock={.period_ns=40,.mask=3,.target_ns={100,200,300}}};
    te_submit s={0};
    assert(prepare_generation(&plan,1,10,89,&s)==3);
    assert(s.count==2 && s.defer_second && s.wait_value==10 && s.signal_first==11);
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,90,&s)==2);
    assert(s.count==1 && !s.defer_second && s.timestamps[0]==2.0f/3.0f && s.signal_first==12);
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,190,&s)==0);
    assert(!s.count && !s.defer_second && s.wait_value==10 && s.signal_first==12 && s.slot==1 && s.nonblocking);
    plan.multiplier=2;plan.clock.mask=1;
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,90,&s)==0 && !s.count);
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,89,&s)==1 && s.count==1);
    assert(s.timestamps[0]==0.5f && s.signal_first==12);
    s=(te_submit){0};prepare_generation(&plan,0,10,89,&s);assert(!s.count);
    plan.clock.period_ns=0;
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,UINT64_MAX,&s)==1 && s.count==1);
    gfg_gpu_fg_set_output_pressure(&plan,1);
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,0,&s)==0 && !s.count && s.signal_first==12);
    gfg_gpu_fg_set_output_pressure(&plan,0);
    const uint64_t now=10000000000ull;
    plan.clock=(gfg_frame_clock){.period_ns=11111111,.mask=1,.target_ns={now+11000000,now+22000000,now+33000000}};
    plan.last_generation_dispatch_ns=now;plan.gpu_budget_ns[0]=10000000;
    s=(te_submit){0};assert(!prepare_generation(&plan,1,10,now,&s) && !s.count);
    plan.gpu_budget_ns[0]=6000000;
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,now,&s)==1 && s.count==1);
    plan.gpu_budget_ns[0]=10000000;
    s=(te_submit){0};assert(!prepare_generation(&plan,1,10,now+2000000000ull,&s)); /* expired: no probe */
    plan.clock.target_ns[0]=now+2011000000ull;
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,now+2000000000ull,&s)==1 && s.count==1);
    plan.last_generation_dispatch_ns=now+2000000000ull;
    s=(te_submit){0};assert(!prepare_generation(&plan,1,10,now+2000000001ull,&s));
    plan.multiplier=3;plan.gpu_budget_ns[0]=12000000;plan.clock.mask=3;
    plan.clock.target_ns[1]=now+2022000000ull;
    s=(te_submit){0};assert(prepare_generation(&plan,1,10,now+2000000001ull,&s)==2);
    assert(s.count==1 && s.timestamps[0]==2.0f/3.0f && !s.defer_second);
    double samples[2]={50,10},median,p95;percentile(samples,2,&median,&p95);
    assert(median==30 && p95==48 && p95>=median);
    gfg_gpu_fg fg = fixture(2);
    fg.frame = 100; /* app progress does not grant a reservation */
    assert(reserve_ring(&fg) == NULL);
    assert(fg.ring_next == 0 && !fg.disabled);
    assert(polls[0] == 1 && polls[1] == 1);
    assert(fg.ring[0].busy && fg.ring[1].busy);

    /* App modulo would pick slot 0 again. Only slot 1 is reusable. */
    states[1] = VK_SUCCESS;
    assert(reserve_ring(&fg) == &fg.ring[1]);
    assert(fg.ring[0].busy && !fg.ring[1].busy);
    assert(fg.ring_reclaimed == 1 && fg.ring_next == 0);

    /* No fence poll is needed for a never-submitted/free slot. */
    fg = fixture(4);
    fg.ring[2].busy = 0;
    assert(reserve_ring(&fg) == &fg.ring[2]);
    assert(polls[0] == 1 && polls[1] == 1 && polls[2] == 0 && polls[3] == 0);
    assert(fg.ring_next == 3);
    states[3] = VK_SUCCESS;
    assert(reserve_ring(&fg) == &fg.ring[3]);
    assert(fg.ring_next == 0);

    fg = fixture(2);
    states[0] = VK_ERROR_DEVICE_LOST;
    fg.ring[1].busy = 0;
    assert(reserve_ring(&fg) == NULL);
    assert(fg.disabled && polls[1] == 0);
    assert(fg.ring[0].busy);

    fg = fixture(1);
    assert(reserve_ring(&fg) == NULL);
    states[0] = VK_SUCCESS;
    assert(reserve_ring(&fg) == &fg.ring[0] && fg.ring_next == 0);
    puts("GFG native: free-slot reservation, bounded busy polls and device-error handling PASS");
    return 0;
}
