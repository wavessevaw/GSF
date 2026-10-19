/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real Vulkan commands, application device with NO optional features enabled.
 * Pixel readback is confined to this test, never the GPU transport runtime.
 */
#include "te_engine.h"
#include "wrapped_physical_dispatch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"borrowed GPU: %s line%d\n",#x,__LINE__);return 1;} }while(0)
#define OK(x) REQUIRE((x)==VK_SUCCESS)
enum { W=256, H=160 };
static PFN_vkQueueSubmit real_submit;
static int fail_next_submit;
static VKAPI_ATTR VkResult VKAPI_CALL fail_submit(VkQueue queue,uint32_t count,const VkSubmitInfo *info,VkFence fence) {
    if(fail_next_submit){fail_next_submit=0;return VK_ERROR_OUT_OF_HOST_MEMORY;}
    return real_submit(queue,count,info,fence);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL test_dispatch(VkDevice device,const char *name) {
    return !strcmp(name,"vkQueueSubmit")?(PFN_vkVoidFunction)fail_submit:vkGetDeviceProcAddr(device,name);
}
static void moving_pattern(unsigned char *rgb,int offset_x,int offset_y) {
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        double u=x-offset_x,v=y-offset_y;
        double values[3]={128+60*sin(u*.21)*cos(v*.17)+40*sin((u+v)*.09),
            128+55*sin(v*.23)+45*cos(u*.13+v*.05),128+70*cos(u*.07-v*.19)};
        for(int c=0;c<3;++c)rgb[3*(y*W+x)+c]=(unsigned char)(values[c]+.5);
    }
}
static double interior_psnr(const unsigned char *a,const unsigned char *b) {
    double error=0;unsigned n=0;
    for(int y=16;y<H-16;++y)for(int x=16;x<W-16;++x)for(int c=0;c<3;++c){
        double d=(double)a[3*(y*W+x)+c]-b[3*(y*W+x)+c];error+=d*d;++n;
    }
    return error?10*log10(255.*255.*n/error):99;
}
int main(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="gfg-borrowed-gpu-test",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;
    OK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=0;
    OK(vkEnumeratePhysicalDevices(instance,&count,NULL));REQUIRE(count>0);
    VkPhysicalDevice *devices=calloc(count,sizeof(*devices));REQUIRE(devices);
    OK(vkEnumeratePhysicalDevices(instance,&count,devices));
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count && !physical;++i){
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,NULL);
        VkQueueFamilyProperties *props=calloc(n,sizeof(*props));REQUIRE(props);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,props);
        for(uint32_t j=0;j<n;++j)if(props[j].queueFlags&VK_QUEUE_COMPUTE_BIT){
            physical=devices[i];family=j;break;
        }
        free(props);
    }
    free(devices);REQUIRE(physical);
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;OK(vkCreateDevice(physical,&dci,NULL,&device));
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    te_device *engine=NULL;
    VkPhysicalDevice borrowed=physical;PFN_vkGetInstanceProcAddr borrowed_gipa=vkGetInstanceProcAddr;
    const int wrapped=gsf_test_wrap_physical(physical,&borrowed,&borrowed_gipa);
    OK(te_device_borrow(instance,borrowed_gipa,borrowed,device,vkGetDeviceProcAddr,NULL,queue,family,&engine));
    if(wrapped)fprintf(stderr,"WRAPPED_PHYSICAL_BORROW_SETUP complete\n");
    te_context_desc desc;
    te_context_desc_default(&desc,96,64,2);
    desc.host_io=1;desc.output_bgra=1;
    desc.params.adaptive=0;desc.params.force_class=1;desc.params.cut_thr=2;
    te_context *context=NULL;
    OK(te_context_create(engine,&desc,&context));
    REQUIRE(te_source_image(context,0));REQUIRE(te_output_image(context,1));
    REQUIRE(!te_source_image(context,2));REQUIRE(!te_output_image(context,2));
    unsigned char *rgb=malloc(96*64*3);REQUIRE(rgb);
    for(unsigned i=0;i<96*64;++i){rgb[3*i]=90;rgb[3*i+1]=120;rgb[3*i+2]=160;}
    OK(te_upload_rgb(context,0,rgb));
    te_submit first={.slot=0};OK(te_submit_frame(context,&first));OK(te_wait_idle(context));
    te_submit invalid={.slot=1,.signal_first=1};
    REQUIRE(te_submit_frame(context,&invalid)==VK_ERROR_FEATURE_NOT_PRESENT);
    for(unsigned i=0;i<96*64;++i){rgb[3*i]=102;rgb[3*i+1]=132;rgb[3*i+2]=172;}
    OK(te_upload_rgb(context,1,rgb));
    te_submit work={.slot=1,.count=2,.timestamps={1.f/3.f,2.f/3.f},.download=1};
    OK(te_submit_frame(context,&work));OK(te_wait_idle(context));
    for(unsigned output=0;output<2;++output){
        OK(te_download_rgb(context,output,rgb));
        const int expected[3]={94+4*(int)output,124+4*(int)output,164+4*(int)output};
        for(unsigned i=0;i<96*64;++i)for(unsigned c=0;c<3;++c)
            REQUIRE(abs((int)rgb[3*i+c]-expected[c])<=2);
    }
    te_pair_stats stats;OK(te_last_stats(context,&stats));
    REQUIRE(stats.outputs==2 && !stats.held && stats.outputs_completed_total==2);
    free(rgb);
    te_context_destroy(context);
    /* Run the runtime's adaptive GPU algorithm on textured motion, not just
     * uniform blends. Two references at distinct times catch output aliasing. */
    te_context_desc_default(&desc,W,H,2);
    desc.host_io=1;desc.output_bgra=1;desc.params.full_density=2;
    if(getenv("GFG_TEST_GPU_LMC")) desc.params.algorithm=TE_ALGO_LMC_GPU;
    if(getenv("GFG_TEST_GSF")){desc.params.algorithm=TE_ALGO_SHARED_FLOW;desc.shared_flow_dense=4;}
    OK(te_context_create(engine,&desc,&context));
    REQUIRE(te_timeline_fd_probe(context,1,2)==VK_ERROR_FEATURE_NOT_PRESENT);
    unsigned char *a=malloc(W*H*3),*b=malloc(W*H*3),*expected=malloc(W*H*3),*out=malloc(W*H*3);
    REQUIRE(a && b && expected && out);
    moving_pattern(a,0,0);moving_pattern(b,6,3);
    OK(te_upload_rgb(context,0,a));OK(te_submit_frame(context,&first));OK(te_wait_idle(context));
    OK(te_upload_rgb(context,1,b));OK(te_submit_frame(context,&work));OK(te_wait_idle(context));
    for(unsigned output=0;output<2;++output){
        moving_pattern(expected,2*(output+1),output+1);
        OK(te_download_rgb(context,output,out));
        double quality=interior_psnr(out,expected);
        double repeat=interior_psnr(output?a:b,expected);
        fprintf(stderr,"borrowed motion: t=%u/3 psnr_db=%.3f endpoint_db=%.3f\n",output+1,quality,repeat);
        REQUIRE(quality>34 && quality>repeat+8);
    }
    /* Static images must remain bit exact through BGRA output packing. */
    OK(te_upload_rgb(context,0,b));work.slot=0;
    OK(te_submit_frame(context,&work));OK(te_wait_idle(context));
    for(unsigned output=0;output<2;++output){
        OK(te_download_rgb(context,output,out));REQUIRE(!memcmp(out,b,W*H*3));
    }
    free(a);free(b);free(expected);free(out);te_context_destroy(context);
    desc.params.algorithm=TE_ALGO_LMC;
    REQUIRE(te_context_create(engine,&desc,&context)==VK_ERROR_FEATURE_NOT_PRESENT);
    te_device_destroy(engine);
    /* Borrowing must never destroy the game's device. */
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;OK(vkCreateFence(device,&fci,NULL,&fence));vkDestroyFence(device,fence,NULL);
    vkDestroyDevice(device,NULL);
    /* Caller-owned timeline: the bridge enables the feature, submits source
     * readiness before TE waits, and retains semaphore ownership after TE. */
    VkPhysicalDeviceTimelineSemaphoreFeatures enabled={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.timelineSemaphore=VK_TRUE};
    dci.pNext=&enabled;OK(vkCreateDevice(physical,&dci,NULL,&device));
    vkGetDeviceQueue(device,family,0,&queue);
    real_submit=(PFN_vkQueueSubmit)vkGetDeviceProcAddr(device,"vkQueueSubmit");
    OK(te_device_borrow(instance,borrowed_gipa,borrowed,device,test_dispatch,NULL,queue,family,&engine));
    VkSemaphoreTypeCreateInfo type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE,.initialValue=0};
    VkSemaphoreCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};
    VkSemaphore timeline;OK(vkCreateSemaphore(device,&sci,NULL,&timeline));
    te_context_desc_default(&desc,96,64,2);desc.application_timeline=timeline;
    if(getenv("GFG_TEST_GSF"))desc.host_io=1;
    if(getenv("GFG_TEST_GPU_LMC")) desc.params.algorithm=TE_ALGO_LMC_GPU;
    if(getenv("GFG_TEST_GSF")){desc.params.algorithm=TE_ALGO_SHARED_FLOW;desc.shared_flow_dense=4;}
    OK(te_context_create(engine,&desc,&context));
    uint64_t source_ready=1;
    if(getenv("GFG_TEST_GSF")) {
        unsigned char black[96*64*3]={0};
        OK(te_upload_rgb(context,0,black));OK(te_upload_rgb(context,1,black));
    }
    VkTimelineSemaphoreSubmitInfo ready={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&source_ready};
    VkSubmitInfo signal={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&ready,
        .signalSemaphoreCount=1,.pSignalSemaphores=&timeline};
    OK(vkQueueSubmit(queue,1,&signal,VK_NULL_HANDLE));
    te_submit ordered={.slot=0,.count=2,.timestamps={1.f/3,2.f/3},.wait_value=1,.signal_first=2};
    OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,3,UINT64_MAX));
    OK(te_wait_idle(context));
    /* Native initial/gap frames should rebuild history without synthesizing
     * two discarded full-size holds, while still signalling completion. */
    te_submit history={.slot=1,.count=0,.wait_value=3,.signal_first=4,.discontinuity=1};
    OK(te_submit_frame(context,&history));OK(te_timeline_wait(context,4,5000000000ull));
    OK(te_wait_idle(context));OK(te_last_stats(context,&stats));
    REQUIRE(stats.outputs_completed_total==2);
    ordered.slot=0;ordered.wait_value=4;ordered.signal_first=5;
    OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,6,5000000000ull));
    OK(te_wait_idle(context));OK(te_last_stats(context,&stats));
    REQUIRE(!stats.held && stats.outputs==2 && stats.outputs_completed_total==4);
    uint64_t final_value=6;
    if(getenv("GFG_TEST_GSF")) {
        unsigned char samples[96*64*3];
        for(unsigned i=0;i<96*64;++i){samples[3*i]=12;samples[3*i+1]=24;samples[3*i+2]=36;}
        OK(te_upload_rgb(context,1,samples));
        ordered.slot=1;ordered.wait_value=6;ordered.signal_first=7;ordered.defer_second=1;ordered.download=1;
        OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,7,5000000000ull));
        te_poll(context);REQUIRE(!te_context_idle(context,0));
        uint64_t partial=0;OK(vkGetSemaphoreCounterValue(device,timeline,&partial));REQUIRE(partial==7);
        REQUIRE(te_timeline_wait(context,8,0)==VK_TIMEOUT);
        OK(te_download_rgb(context,0,samples));
        for(unsigned i=0;i<96*64;++i)for(unsigned c=0;c<3;++c)
            REQUIRE(abs((int)samples[3*i+c]-4*(int)(c+1))<=1);
        OK(te_resume_outputs(context));OK(te_timeline_wait(context,8,5000000000ull));
        OK(te_wait_idle(context));OK(te_download_rgb(context,1,samples));
        for(unsigned i=0;i<96*64;++i)for(unsigned c=0;c<3;++c)
            REQUIRE(abs((int)samples[3*i+c]-8*(int)(c+1))<=1);
        ordered.slot=0;ordered.wait_value=8;ordered.signal_first=9;
        OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,9,5000000000ull));
        OK(te_cancel_deferred_output(context));OK(te_timeline_wait(context,10,5000000000ull));
        OK(te_wait_idle(context));OK(te_last_stats(context,&stats));
        REQUIRE(stats.outputs==1 && stats.outputs_completed_total==7);
        REQUIRE(te_output_control(context,1,10,NULL,NULL,NULL)==VK_ERROR_FEATURE_NOT_PRESENT);
        ordered.slot=1;ordered.wait_value=10;ordered.signal_first=11;
        OK(te_submit_frame(context,&ordered));OK(te_wait_idle(context));
        OK(vkGetSemaphoreCounterValue(device,timeline,&partial));REQUIRE(partial==12);
        ordered.slot=0;ordered.wait_value=12;ordered.signal_first=13;
        OK(te_submit_frame(context,&ordered));REQUIRE(te_context_idle(context,5000000000ull));
        OK(vkGetSemaphoreCounterValue(device,timeline,&partial));REQUIRE(partial==14);
        ordered.slot=1;ordered.wait_value=14;ordered.signal_first=15;
        OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,15,5000000000ull));
        fail_next_submit=1;
        REQUIRE(te_resume_outputs(context)==VK_ERROR_OUT_OF_HOST_MEMORY);
        REQUIRE(te_wait_idle(context)==VK_ERROR_OUT_OF_HOST_MEMORY);
        OK(vkGetSemaphoreCounterValue(device,timeline,&partial));REQUIRE(partial==15);
        te_context_destroy(context);
        OK(te_context_create(engine,&desc,&context));
        ordered.slot=0;ordered.wait_value=15;ordered.signal_first=16;
        OK(te_submit_frame(context,&ordered));OK(te_timeline_wait(context,16,5000000000ull));
        /* Destroy must flush deferred G2 rather than waiting an unsent fence. */
        final_value=17;
        puts("borrowed deferred GSF: G1 pixels ready while G2 unsent; cancel/explicit/idle/destroy flush and failed-submit cleanup PASS");
    }
    te_context_destroy(context);te_device_destroy(engine);
    uint64_t value=0;OK(vkGetSemaphoreCounterValue(device,timeline,&value));REQUIRE(value==final_value);
    vkDestroySemaphore(device,timeline,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    gsf_test_wrapped_result(wrapped,0);
    puts("borrowed GPU: exact 1/3,2/3 pixels, BGRA packing, no optional features, device lifetime PASS");
    return 0;
}
