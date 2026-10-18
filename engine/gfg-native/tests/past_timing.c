/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include "../src/past_timing.h"
#include <assert.h>
#include <stdio.h>

static uint32_t pending, next_id, queries, reads, added;
static VkResult query_result=VK_SUCCESS, read_result=VK_SUCCESS;
/* Model Gamescope 3.16.16: erase INPUT count, not returned count. A request
 * larger than pending would erase beyond end even if array copy was bounded.
 * Do not perform that undefined erase: fail at the API boundary instead.
 */
static VKAPI_ATTR VkResult VKAPI_CALL legacy_wsi(VkDevice d,VkSwapchainKHR s,
    uint32_t *count,VkPastPresentationTimingGOOGLE *out) {
    (void)d;(void)s;
    if(!out) {
        assert(*count==0);
        ++queries;*count=pending;return query_result;
    }
    ++reads;pending+=added;added=0;
    assert(*count>0 && *count<=pending);
    if(read_result!=VK_SUCCESS && read_result!=VK_INCOMPLETE)return read_result;
    uint32_t input=*count;
    for(uint32_t i=0;i<input;++i)out[i]=(VkPastPresentationTimingGOOGLE){.presentID=++next_id};
    pending-=input;
    return pending?VK_INCOMPLETE:read_result;
}
static uint32_t consume(void) {
    struct { uint64_t before; VkPastPresentationTimingGOOGLE past[16]; uint64_t after; }
        buffer={.before=0x12345678,.after=0x87654321};
    uint32_t n=999;
    VkResult result=gfg_past_timing_read(legacy_wsi,VK_NULL_HANDLE,VK_NULL_HANDLE,16,buffer.past,&n);
    if(result!=VK_SUCCESS && result!=VK_INCOMPLETE)assert(!n);
    assert(buffer.before==0x12345678 && buffer.after==0x87654321 && n<=16);
    for(uint32_t i=1;i<n;++i)assert(buffer.past[i].presentID==buffer.past[i-1].presentID+1);
    return n;
}
int main(void) {
    const uint32_t counts[]={0,1,2,15,16,17,64,65,128};
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        pending=counts[i];uint32_t total=0;
        do { total+=consume(); }while(pending);
        assert(total==counts[i]);
    }
    /* Feedback can arrive between the count and data calls. */
    pending=1;added=3;assert(consume()==1 && pending==3);assert(consume()==3);
    pending=0;unsigned before=reads;assert(!consume() && reads==before);
    query_result=VK_ERROR_SURFACE_LOST_KHR;pending=2;
    assert(!consume() && reads==before);query_result=VK_SUCCESS;
    read_result=VK_ERROR_OUT_OF_DATE_KHR;assert(!consume() && pending==2);
    read_result=VK_SUCCESS;
    uint32_t total=0;
    /* Cross the observed 64-present crash repeatedly, including empty polls. */
    pending=0;
    for(unsigned frame=0;frame<3000;++frame) {
        pending+=frame%3;total+=consume();
    }
    assert(total==3000 && !pending && queries>3000);
    puts("past-timing: legacy erase capacity, empty/sparse/backlog/growing feedback, errors and 3000 frames PASS");
}
