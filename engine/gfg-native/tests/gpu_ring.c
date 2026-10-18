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
