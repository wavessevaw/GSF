/* SPDX-License-Identifier: GPL-3.0-or-later
 * ×3 Output Owner binds exactly TWO physical targets per work slot. The
 * third storage binding is a valid descriptor alias but never written by
 * AFMF_BATCHED when count=2. This checks Vulkan descriptor writes, not
 * visual quality or a physical scanout pipeline.
 */
#include "framegen_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H(type,x) ((type)(uintptr_t)(x))
static uintptr_t next_descriptor=0x1000u;
static struct {VkDescriptorSet set;VkImageView lane[3];unsigned seen;} bindings[4];
static unsigned captured,layouts_allocated;
static VkResult VKAPI_PTR create_pool(VkDevice device,
    const VkDescriptorPoolCreateInfo *ci,const VkAllocationCallbacks *alloc,
    VkDescriptorPool *pool) {
    (void)device;(void)alloc;
    assert(ci && ci->maxSets>=4u && ci->poolSizeCount>=3u);
    *pool=H(VkDescriptorPool,0x998u);return VK_SUCCESS;
}
static VkResult VKAPI_PTR alloc_sets(VkDevice device,
    const VkDescriptorSetAllocateInfo *ci,VkDescriptorSet *out) {
    (void)device;
    assert(ci->descriptorPool==H(VkDescriptorPool,0x998u));
    for(uint32_t i=0;i<ci->descriptorSetCount;i++)
        out[i]=H(VkDescriptorSet,next_descriptor++);
    layouts_allocated+=ci->descriptorSetCount;
    return VK_SUCCESS;
}
static void VKAPI_PTR update(VkDevice device,uint32_t n,
    const VkWriteDescriptorSet *writes,uint32_t copies,
    const VkCopyDescriptorSet *copy) {
    (void)device;(void)copies;(void)copy;
    if(n!=7u)return;
    assert(captured<4);
    bindings[captured].set=writes[0].dstSet;
    for(uint32_t i=0;i<n;i++) {
        assert(writes[i].dstSet==bindings[captured].set);
        if(writes[i].dstBinding>=4u&&writes[i].dstBinding<=6u) {
            unsigned lane=writes[i].dstBinding-4u;
            assert(writes[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            bindings[captured].lane[lane]=writes[i].pImageInfo->imageView;
            bindings[captured].seen++;
        }
    }
    assert(bindings[captured].seen==3u);
    captured++;
}
int main(void) {
    struct afmf_device dev={0};
    struct afmf_framegen fg={0};
    struct afmf_framegen_pipelines pl={0};
    dev.handle=H(VkDevice,0x10);
    dev.framegen_pipelines=&pl;
    dev.fns.create_descriptor_pool=create_pool;
    dev.fns.allocate_descriptor_sets=alloc_sets;
    dev.fns.update_descriptor_sets=update;
    fg.direct=true;fg.batch=true;fg.batch_stride=2u;
    fg.slots=2u;fg.target_count=4u;
    fg.sampler=H(VkSampler,0x12u);
    fg.cb=H(VkBuffer,0x13u);
    for(uint32_t k=0;k<PASS_COUNT;k++)
        pl.set_layouts[k]=H(VkDescriptorSetLayout,0x100u+k);
    VkImageView v[4];
    for(uint32_t j=0;j<4;j++)v[j]=H(VkImageView,0x210u+j);
    fg.target_views=v;
    for(uint32_t parity=0;parity<2u;parity++) {
        fg.color[parity].view=H(VkImageView,0x300u+parity);
        for(uint32_t k=0;k<AFMF_LEVELS;k++) {
            fg.luma[parity][k].view=H(VkImageView,0x400u+parity*AFMF_LEVELS+k);
            fg.flow[parity][k].view=H(VkImageView,0x500u+parity*AFMF_LEVELS+k);
        }
    }
    fg.flow_out.view=H(VkImageView,0x601u);
    fg.scd_histogram.view=H(VkImageView,0x602u);
    fg.scd_previous_histogram.view=H(VkImageView,0x603u);
    fg.scd_temp.view=H(VkImageView,0x604u);
    fg.scd_output.view=H(VkImageView,0x605u);
    assert(afmf_framegen_descriptor_sets_create(&dev,&fg)==VK_SUCCESS);
    assert(layouts_allocated>4u && captured==4u);
    for(uint32_t parity=0;parity<2u;parity++) {
        for(uint32_t slot=0;slot<2u;slot++) {
            const unsigned index=parity*2u+slot;
            assert(bindings[index].set==fg.batch_sets[index]);
            assert(bindings[index].lane[0]==v[slot*2u]);
            assert(bindings[index].lane[1]==v[slot*2u+1u]);
            assert(bindings[index].lane[2]==v[slot*2u+1u]);
            assert(bindings[index].lane[0]!=bindings[index].lane[1]);
        }
    }
    free(fg.direct_sets);
    free(fg.batch_sets);
    printf("GSF_ALPHA_X3_BATCH descriptors=PASS two_physical_targets_per_slot=1"
           " third_binding_alias_only=1 parities=2 slots=2\n");
    return 0;
}
