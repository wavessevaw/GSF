/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/memory_policy.h"
int main(void) {
    VkPhysicalDeviceMemoryProperties p={0};
    uint32_t index=99;
    int coh=0,cached=0;
    p.memoryTypeCount=4;
    p.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; /* old bad selection */
    p.memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;   /* preferred GPU readback */
    p.memoryTypes[3].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT|
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(gfg_native_readback_memory_type(&p,0xf,&index,&coh,&cached));
    assert(index==2 && cached==1 && coh==0);
    assert(gfg_native_readback_memory_type(&p,0xa,&index,&coh,&cached));
    assert(index==3 && cached==1 && coh==1);
    assert(gfg_native_readback_memory_type(&p,0x2,&index,&coh,&cached));
    assert(index==1 && cached==0 && coh==1);
    assert(!gfg_native_readback_memory_type(&p,0x1,&index,&coh,&cached));
    puts("GFG native: cached Vulkan readback memory policy PASS");
    return 0;
}
