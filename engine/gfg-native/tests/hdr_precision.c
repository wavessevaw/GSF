/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual packed10 -> TE FP16 -> packed10 GPU roundtrip. Adjacent 10-bit
 * levels catch an accidental rgba8 intermediate; no native runtime readback.
 */
#include "te_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"HDR precision failed line %d: %s\n",__LINE__,#x);fprintf(stderr,"TE: %s\n",te_last_error());exit(1);} } while(0)
#define OK(x) CHECK((x)==VK_SUCCESS)
enum { W=64,H=64,N=W*H };
static unsigned errors;
static double pq_decode(double code) {
    double v=pow(code,32.0/2523.0);
    return 10000.0*pow(fmax(v-3424.0/4096.0,0)/(2413.0/128.0-2392.0/128.0*v),16384.0/2610.0);
}
static unsigned pq_encode(double nits) {
    double v=pow(fmax(nits,0)/10000.0,2610.0/16384.0);
    return (unsigned)llround(1023.0*pow((3424.0/4096.0+2413.0/128.0*v)/(1+2392.0/128.0*v),2523.0/32.0));
}
static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){++errors;fprintf(stderr,"VALIDATION ERROR: %s\n",data->pMessage);}
    return VK_FALSE;
}
static uint32_t memory_type(VkPhysicalDevice p,uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;vkGetPhysicalDeviceMemoryProperties(p,&props);
    for(uint32_t i=0;i<props.memoryTypeCount;++i)
        if((bits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags)return i;
    CHECK(0);return 0;
}
static VkImageMemoryBarrier barrier(VkImage image,VkAccessFlags src,VkAccessFlags dst,VkImageLayout old,VkImageLayout new_layout) {
    return (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=src,.dstAccessMask=dst,
        .oldLayout=old,.newLayout=new_layout,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}
static void begin(VkCommandBuffer cb) {
    OK(vkResetCommandBuffer(cb,0));VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(cb,&b));
}
static void finish(VkDevice d,VkQueue q,VkCommandBuffer cb) {
    OK(vkEndCommandBuffer(cb));VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    OK(vkQueueSubmit(q,1,&s,VK_NULL_HANDLE));OK(vkDeviceWaitIdle(d));
}
int main(void) {
    const int linear=getenv("GFG_TEST_PQ_LINEAR")!=NULL;
    const int pq=linear || getenv("GFG_TEST_PQ_DUPLICATE")!=NULL;
    const unsigned outputs=getenv("GFG_TEST_X2")?1u:2u;
    unsigned source_code[2][3];
    for(unsigned s=0;s<2;++s)for(unsigned c=0;c<3;++c)
        source_code[s][c]=pq_encode(c==0?(s?0.01:0.0):c==1?100.0*(s+1):5000.0*(s+1));
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    const char *extensions[]={VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation={.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,.enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&sync};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.pNext=&validation,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=extensions};
    VkInstance instance;OK(vkCreateInstance(&ici,NULL,&instance));
    PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,.messageType=7,.pfnUserCallback=message};
    VkDebugUtilsMessengerEXT messenger;OK(create(instance,&debug,NULL,&messenger));
    uint32_t count=1;VkPhysicalDevice p;OK(vkEnumeratePhysicalDevices(instance,&count,&p));
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&families,NULL);
    VkQueueFamilyProperties *props=calloc(families,sizeof(*props));CHECK(props);
    vkGetPhysicalDeviceQueueFamilyProperties(p,&families,props);uint32_t family=0;
    while(family<families && !(props[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;
    free(props);CHECK(family<families);
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice d;OK(vkCreateDevice(p,&dci,NULL,&d));VkQueue q;vkGetDeviceQueue(d,family,0,&q);
    te_device *engine;OK(te_device_borrow(instance,vkGetInstanceProcAddr,p,d,vkGetDeviceProcAddr,NULL,q,family,&engine));
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=N*4*3,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;OK(vkCreateBuffer(d,&bci,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(d,buffer,&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(p,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory memory;OK(vkAllocateMemory(d,&alloc,NULL,&memory));OK(vkBindBufferMemory(d,buffer,memory,0));
    uint32_t *pixels;OK(vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,(void**)&pixels));
    for(unsigned i=0;i<N;++i)pixels[i]=((i+119)%1024)|(((i*23+321)%1024)<<10)|(((i*31+727)%1024)<<20)|(3u<<30);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;OK(vkCreateCommandPool(d,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo cba={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb;OK(vkAllocateCommandBuffers(d,&cba,&cb));
    unsigned formats_tested=0;
    for(unsigned fmt=0;fmt<2;++fmt) {
    VkFormat format=fmt?VK_FORMAT_A2R10G10B10_UNORM_PACK32:VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    VkFormatProperties capability;vkGetPhysicalDeviceFormatProperties(p,format,&capability);
    if(!(capability.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        fprintf(stderr,"HDR precision: format=%d BLIT_DST unsupported, skipped (runtime also refuses it)\n",(int)format);continue;
    }
    ++formats_tested;
    VkImage target;VkDeviceMemory target_memory;
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={W,H,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    OK(vkCreateImage(d,&image,NULL,&target));vkGetImageMemoryRequirements(d,target,&req);
    alloc.allocationSize=req.size;alloc.memoryTypeIndex=memory_type(p,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    OK(vkAllocateMemory(d,&alloc,NULL,&target_memory));OK(vkBindImageMemory(d,target,target_memory,0));
    te_context_desc desc;te_context_desc_default(&desc,W,H,outputs);desc.host_io=0;
    desc.source_format=format;desc.output_fp16=1;desc.params.cut_thr=2;
    if(getenv("GFG_TEST_FORCE_CLASS")){desc.params.adaptive=0;desc.params.force_class=atoi(getenv("GFG_TEST_FORCE_CLASS"));}
    if(getenv("GFG_TEST_GPU_LMC"))desc.params.algorithm=TE_ALGO_LMC_GPU;
    if(getenv("GFG_TEST_GSF")){desc.params.algorithm=TE_ALGO_SHARED_FLOW;desc.shared_flow_dense=4;}
    if(pq)desc.color_transfer=TE_TRANSFER_PQ;
    te_context *ctx;OK(te_context_create(engine,&desc,&ctx));
    for(unsigned slot=0;slot<2;++slot) {
        if(linear)for(unsigned i=0;i<N;++i)pixels[i]=(3u<<30)|source_code[slot][0]|(source_code[slot][1]<<10)|(source_code[slot][2]<<20);
        begin(cb);VkImage source=te_source_image(ctx,slot);
        VkImageMemoryBarrier b=barrier(source,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
        vkCmdCopyBufferToImage(cb,buffer,source,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&b);finish(d,q,cb);
        te_submit submit={.slot=slot,.count=slot?outputs:0,.timestamps={outputs==1?0.5f:1.f/3,2.f/3}};OK(te_submit_frame(ctx,&submit));OK(te_wait_idle(ctx));
    }
    for(unsigned out=0;out<outputs;++out) {
        begin(cb);VkImage output=te_output_image(ctx,out);
        VkImageMemoryBarrier b[2]={barrier(output,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL),
            barrier(target,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,b);
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .srcOffsets={{0,0,0},{W,H,1}},.dstOffsets={{0,0,0},{W,H,1}}};
        vkCmdBlitImage(cb,output,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
        b[0]=barrier(target,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,b);
        VkBufferImageCopy copy={.bufferOffset=N*4*(out+1),.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
        vkCmdCopyImageToBuffer(cb,target,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);finish(d,q,cb);
        unsigned mismatches=0;
        unsigned expected[3]={0},coded[3]={0};
        if(linear)for(unsigned c=0;c<3;++c) {
            const double t=outputs==1?0.5:(double)(out+1)/3.0;
            expected[c]=pq_encode((1-t)*pq_decode((double)source_code[0][c]/1023)+t*pq_decode((double)source_code[1][c]/1023));
            coded[c]=(unsigned)llround((1-t)*source_code[0][c]+t*source_code[1][c]);
            CHECK(abs((int)expected[c]-(int)coded[c])>=2);
        }
        for(unsigned i=0;i<N;++i) {
            uint32_t actual=pixels[N*(out+1)+i];
            if(linear) {
                for(unsigned c=0;c<3;++c)if(abs((int)((actual>>(10*c))&1023)-(int)expected[c])>1) {
                    if(mismatches<5)fprintf(stderr,"PQ mismatch output=%u channel=%u expected-linear=%u wrong-coded=%u actual=%u\n",out,c,expected[c],coded[c],(actual>>(10*c))&1023);
                    ++mismatches;
                }
            } else if(actual!=pixels[i]) {
                if(mismatches<5)fprintf(stderr,"HDR mismatch output=%u i=%u expected=%08x got=%08x\n",out,i,pixels[i],actual);
                ++mismatches;
            }
        }
        CHECK(!mismatches);
        if(linear)fprintf(stderr,"PQ linear output=%u codes=%u,%u,%u coded-average-rejected=%u,%u,%u PASS\n",out,expected[0],expected[1],expected[2],coded[0],coded[1],coded[2]);
    }
    fprintf(stderr,"HDR precision: format=%d outputs=%u %s PASS\n",(int)format,outputs,linear?"linear-light PQ reconstruction":"all 1024 packed levels preserved exactly");
    te_context_destroy(ctx);vkDestroyImage(d,target,NULL);vkFreeMemory(d,target_memory,NULL);
    }
    te_device_destroy(engine);vkDestroyCommandPool(d,pool,NULL);vkUnmapMemory(d,memory);vkDestroyBuffer(d,buffer,NULL);vkFreeMemory(d,memory,NULL);
    vkDestroyDevice(d,NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");
    destroy(instance,messenger,NULL);vkDestroyInstance(instance,NULL);CHECK(errors==0 && formats_tested>0);
    puts(linear?"HDR PQ: packed10, cached linear nits, x2/x3 CPU-oracle comparison PASS; scanout unobserved":"HDR precision: supported packed10 formats, FP16 synthesis, 1024 distinct levels, two outputs bit-exact PASS; scanout unobserved");
    return 0;
}
