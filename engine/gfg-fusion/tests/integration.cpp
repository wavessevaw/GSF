/* SPDX-License-Identifier: GPL-3.0-or-later */
// Uses the pinned MAKO production scaler on a real Vulkan device (Lavapipe
// in CI). No mock pixel math and no licensed LSFG payload required.
#include "spatial_scaler.hpp"
#include "image_transfer.hpp"
#include "mako-common/vulkan/buffer.hpp"
#include "mako-common/vulkan/command_buffer.hpp"
#include "mako-common/vulkan/image.hpp"
#include "mako-common/vulkan/vulkan.hpp"
#include "mako-common/helpers/errors.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using mako::layer::SpatialScaler;
using mako::cli::images::imageBarrier;
using mako::cli::images::uploadImage;
static PFN_vkCmdBlitImage originalBlit{};
static uint64_t blits{};
static PFN_vkCmdCopyImage originalCopy{};
static uint64_t copies{};
static void VKAPI_CALL countedCopy(VkCommandBuffer command, VkImage source,
        VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout,
        uint32_t count, const VkImageCopy* regions) {
    ++copies;
    originalCopy(command, source, sourceLayout, destination, destinationLayout, count, regions);
}
static void VKAPI_CALL countedBlit(VkCommandBuffer command, VkImage source,
        VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout,
        uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    ++blits;
    originalBlit(command, source, sourceLayout, destination, destinationLayout,
        count, regions, filter);
}
static void require(bool good, const char* reason) {
    if (!good) throw std::runtime_error(reason);
}
// Private-image validation needs no external-memory/FD transport. Production
// still uses its normal application-device/LSFG transport; we explicitly test
// the same scaler on a headless device without pretending to exercise LSFG.
struct HeadlessDevice {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};uint32_t family{};
    HeadlessDevice() {
        const VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName="gfg-frame-fusion-proof",.apiVersion=VK_API_VERSION_1_1};
        const VkInstanceCreateInfo create{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
        require(vkCreateInstance(&create,nullptr,&instance)==VK_SUCCESS,"headless instance unavailable");
        uint32_t count{};vkEnumeratePhysicalDevices(instance,&count,nullptr);
        require(count>0,"no Vulkan device");std::vector<VkPhysicalDevice> devices(count);
        require(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS,"device enumeration failed");
        physical=devices.front();
        for(const auto candidate:devices) {VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate,&props);if(props.vendorID==0x1002){physical=candidate;break;}}
        uint32_t families{};vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
        bool found=false;for(uint32_t i=0;i<families;++i) if(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;found=true;break;}
        require(found,"no compute queue");const float priority=1.0F;
        const VkDeviceQueueCreateInfo queue{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
        VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(physical,&features);
        const VkDeviceCreateInfo info{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.pEnabledFeatures=&features};
        require(vkCreateDevice(physical,&info,nullptr,&device)==VK_SUCCESS,"headless device unavailable");
    }
    ~HeadlessDevice(){if(device)vkDestroyDevice(device,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}
    HeadlessDevice(const HeadlessDevice&)=delete;
};
template<class Function> static void rejected(Function f) {
    bool failed=false;
    try { f(); } catch (const std::exception&) { failed=true; }
    require(failed, "invalid input accepted");
}
static std::vector<uint8_t> pattern(VkExtent2D size, unsigned phase) {
    std::vector<uint8_t> pixels(size.width*size.height*4);
    for (uint32_t y=0; y<size.height; ++y) for (uint32_t x=0; x<size.width; ++x) {
        const auto p=(y*size.width+x)*4;
        pixels[p]=static_cast<uint8_t>((x*17+y*3+phase*43)%256);
        pixels[p+1]=static_cast<uint8_t>((y*31+x*7+phase*19)%256);
        pixels[p+2]=((x+phase*3)/5+y/7)%2 ? 255 : 0;
        pixels[p+3]=static_cast<uint8_t>((x+y+phase)%256);
    }
    return pixels;
}
static std::vector<uint8_t> download(const vk::Vulkan& vk, const vk::Image& image) {
    const std::vector<uint8_t> zeroes(image.getExtent().width*image.getExtent().height*4);
    const vk::Buffer staging(vk,zeroes.data(),zeroes.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const vk::CommandBuffer command(vk);
    command.begin(vk);
    const auto barrier=imageBarrier(image.handle(),VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk.df().CmdPipelineBarrier(command.handle(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    const VkBufferImageCopy region{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={image.getExtent().width,image.getExtent().height,1}};
    vk.df().CmdCopyImageToBuffer(command.handle(),image.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.handle(),1,&region);
    // Make transfer results available to the host, even on non-coherent heaps.
    const VkBufferMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=staging.handle(),.offset=0,.size=VK_WHOLE_SIZE};
    vk.df().CmdPipelineBarrier(command.handle(),VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    command.end(vk);command.submit(vk);
    return staging.read(vk);
}
static void record(const vk::Vulkan& vk, SpatialScaler& scaler, const vk::Image& input,
        const vk::Image& output, VkImageLayout inputLayout=VK_IMAGE_LAYOUT_GENERAL) {
    const vk::CommandBuffer command(vk);command.begin(vk);
    scaler.recordSourceToPresentation(vk,command,input.handle(),output.handle(),
        inputLayout,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    command.end(vk);command.submit(vk);
}

// TimestampQueryPool is inserted here by the source preparation helper.
// Timestamp utility adapted from MAKO CLI quality.cpp (GPL-3.0-or-later).
    template<typename Function>
    [[nodiscard]] Function deviceFunction(
            const vk::Vulkan& vk, const char* name) {
        const auto function = reinterpret_cast<Function>(
            vk.fi().GetDeviceProcAddr(vk.dev(), name)
        );
        if (!function)
            throw ls::vulkan_error(
                "failed to get device proc addr for " + std::string(name)
            );
        return function;
    }

    class TimestampQueryPool {
    public:
        TimestampQueryPool(const vk::Vulkan& vk, const uint32_t queryCount) :
            device(vk.dev()),
            create(deviceFunction<PFN_vkCreateQueryPool>(vk, "vkCreateQueryPool")),
            destroy(deviceFunction<PFN_vkDestroyQueryPool>(vk, "vkDestroyQueryPool")),
            resetFunction(deviceFunction<PFN_vkCmdResetQueryPool>(
                vk, "vkCmdResetQueryPool"
            )),
            writeFunction(deviceFunction<PFN_vkCmdWriteTimestamp>(
                vk, "vkCmdWriteTimestamp"
            )),
            resultsFunction(deviceFunction<PFN_vkGetQueryPoolResults>(
                vk, "vkGetQueryPoolResults"
            )),
            count(queryCount) {
            uint32_t familyCount{};
            vk.fi().GetPhysicalDeviceQueueFamilyProperties(
                vk.physdev(), &familyCount, nullptr
            );
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vk.fi().GetPhysicalDeviceQueueFamilyProperties(
                vk.physdev(), &familyCount, families.data()
            );
            if (vk.queueFamilyIndex() >= families.size())
                throw ls::vulkan_error("timestamp queue family is out of range");
            this->validBits = families.at(
                vk.queueFamilyIndex()
            ).timestampValidBits;
            if (this->validBits == 0)
                throw ls::vulkan_error(
                    "selected Vulkan queue does not support timestamps"
                );
            if (this->validBits > 64)
                throw ls::vulkan_error(
                    "selected Vulkan queue reports an invalid timestamp width"
                );

            VkPhysicalDeviceProperties properties{};
            vk.fi().GetPhysicalDeviceProperties(vk.physdev(), &properties);
            this->periodNanoseconds = properties.limits.timestampPeriod;
            if (!std::isfinite(this->periodNanoseconds) ||
                    this->periodNanoseconds <= 0.0F) {
                throw ls::vulkan_error(
                    "selected Vulkan device reports an invalid timestamp period"
                );
            }

            const VkQueryPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = queryCount,
            };
            const auto result = this->create(
                this->device, &info, nullptr, &this->pool
            );
            if (result != VK_SUCCESS)
                throw ls::vulkan_error(result, "vkCreateQueryPool() failed");
        }

        ~TimestampQueryPool() {
            if (this->pool != VK_NULL_HANDLE)
                this->destroy(this->device, this->pool, nullptr);
        }

        TimestampQueryPool(const TimestampQueryPool&) = delete;
        TimestampQueryPool& operator=(const TimestampQueryPool&) = delete;

        void reset(const VkCommandBuffer commandBuffer) const {
            this->resetFunction(commandBuffer, this->pool, 0, this->count);
        }

        void write(const VkCommandBuffer commandBuffer,
                const VkPipelineStageFlagBits stage,
                const uint32_t query) const {
            this->writeFunction(commandBuffer, stage, this->pool, query);
        }

        [[nodiscard]] std::vector<double> microseconds() const {
            std::vector<uint64_t> timestamps(this->count);
            const auto result = this->resultsFunction(
                this->device, this->pool, 0, this->count,
                timestamps.size() * sizeof(uint64_t), timestamps.data(),
                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
            );
            if (result != VK_SUCCESS)
                throw ls::vulkan_error(
                    result, "vkGetQueryPoolResults() failed"
                );

            const uint64_t mask = this->validBits == 64
                ? UINT64_MAX : (uint64_t{1} << this->validBits) - 1;
            std::vector<double> samples;
            samples.reserve(this->count / 2);
            for (uint32_t query = 0; query < this->count; query += 2) {
                const uint64_t elapsed =
                    (timestamps.at(query + 1) - timestamps.at(query)) & mask;
                samples.push_back(
                    static_cast<double>(elapsed) *
                    static_cast<double>(this->periodNanoseconds) / 1000.0
                );
            }
            return samples;
        }

        [[nodiscard]] uint32_t timestampValidBits() const {
            return this->validBits;
        }

        [[nodiscard]] float timestampPeriodNanoseconds() const {
            return this->periodNanoseconds;
        }

    private:
        VkDevice device{VK_NULL_HANDLE};
        PFN_vkCreateQueryPool create{};
        PFN_vkDestroyQueryPool destroy{};
        PFN_vkCmdResetQueryPool resetFunction{};
        PFN_vkCmdWriteTimestamp writeFunction{};
        PFN_vkGetQueryPoolResults resultsFunction{};
        VkQueryPool pool{VK_NULL_HANDLE};
        uint32_t count{};
        uint32_t validBits{};
        float periodNanoseconds{};
    };



static void integration(vk::Vulkan& vk) {
    unsigned comparisons=0;
    for (const auto outputSize : {VkExtent2D{320,200},VkExtent2D{317,193}})
    for (const float factor : {1.25F,1.5F,2.0F})
    for (const float sharpness : {0.0F,0.5F,1.0F}) {
        const VkExtent2D inputSize{static_cast<uint32_t>(outputSize.width/factor),
            static_cast<uint32_t>(outputSize.height/factor)};
        std::vector<vk::Image> inputs;
        for(unsigned i=0;i<3;++i) inputs.emplace_back(vk,inputSize,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        const vk::Image baselineOutput(vk,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        const vk::Image fusionOutput(vk,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        SpatialScaler baseline(vk,inputSize,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,sharpness,std::nullopt);
        SpatialScaler fusion(vk,inputSize,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,sharpness,std::nullopt);
        const std::array refs{std::cref(inputs[0]),std::cref(inputs[1]),std::cref(inputs[2])};
        fusion.configureDirectFrameGenerationInputs(vk,refs);
        require(fusion.directFrameGenerationInputCount()==3,"missing input bindings");
        const std::array duplicates{std::cref(inputs[0]),std::cref(inputs[0])};
        rejected([&]{fusion.configureDirectFrameGenerationInputs(vk,duplicates);});
        require(fusion.directFrameGenerationInputCount()==3,"failed replacement lost old bindings");
        for(unsigned phase=0;phase<6;++phase) {
            auto& input=inputs[phase%3];
            uploadImage(vk,input,pattern(inputSize,phase));
            blits=0;record(vk,baseline,input,baselineOutput);
            require(blits==2,"baseline copy count changed");
            blits=0;record(vk,fusion,input,fusionOutput,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            require(blits==1,"fusion did not remove exactly one input copy");
            require(download(vk,baselineOutput)==download(vk,fusionOutput),"fusion pixels differ");
            ++comparisons;
        }
        require(fusion.frameFusionRecordedFrames()==6,"recorded-frame counter incorrect");
        fusion.clearDirectFrameGenerationInputs();
        require(fusion.frameFusionRecordedFrames()==0,"clear retained stale evidence");
        blits=0;record(vk,fusion,inputs[0],fusionOutput,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        require(blits==2,"clearing did not restore copy path");
        const vk::Image wrongSize(vk,{inputSize.width+1,inputSize.height});
        const std::array wrongSizeRefs{std::cref(wrongSize)};
        rejected([&]{fusion.configureDirectFrameGenerationInputs(vk,wrongSizeRefs);});
        const vk::Image wrongFormat(vk,inputSize,VK_FORMAT_R16G16B16A16_SFLOAT);
        const std::array wrongFormatRefs{std::cref(wrongFormat)};
        rejected([&]{fusion.configureDirectFrameGenerationInputs(vk,wrongFormatRefs);});
        const vk::Image noSampling(vk,inputSize,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT);
        const std::array noSamplingRefs{std::cref(noSampling)};
        rejected([&]{fusion.configureDirectFrameGenerationInputs(vk,noSamplingRefs);});
        // An unregistered image must use the baseline; never reuse a descriptor
        // for another generated image merely because its dimensions match.
        fusion.configureDirectFrameGenerationInputs(vk,std::span(refs).first(1));
        blits=0;record(vk,fusion,inputs[1],fusionOutput,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        require(blits==2,"unregistered source was sampled through the wrong binding");
        SpatialScaler native(vk,inputSize,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Native,sharpness,std::nullopt);
        native.configureDirectFrameGenerationInputs(vk,refs);
        require(native.directFrameGenerationInputCount()==0,"native baseline advertised fusion");
    }
    std::cout << "Frame Fusion PASS: " << comparisons
        << " byte-identical image comparisons; input-copy elimination; rollback; invalid-input guards\n";
}
static void applicationIntegration(vk::Vulkan& vk) {
    unsigned comparisons=0;
    for(const auto format : {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM})
    for(const bool handoff : {false,true})
    for(const float sharpness : {0.0F,0.5F,1.0F}) {
        const VkExtent2D physical{317,193},source{211,128};
        const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
            VK_IMAGE_USAGE_SAMPLED_BIT;
        const vk::Image original(vk,physical,format,usage),direct(vk,physical,format,usage);
        const vk::Image baseFg(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage|VK_IMAGE_USAGE_STORAGE_BIT);
        const vk::Image fusedFg(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage|VK_IMAGE_USAGE_STORAGE_BIT);
        const vk::Image baseFgNext(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage|VK_IMAGE_USAGE_STORAGE_BIT);
        const vk::Image fusedFgNext(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage|VK_IMAGE_USAGE_STORAGE_BIT);
        SpatialScaler base(vk,source,physical,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,sharpness,std::nullopt);
        SpatialScaler fused(vk,source,physical,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,sharpness,std::nullopt);
        if(handoff) {
            const std::array a{std::cref(baseFg),std::cref(baseFgNext)},
                b{std::cref(fusedFg),std::cref(fusedFgNext)};
            base.configureDirectFrameGenerationOutputs(vk,a);
            fused.configureDirectFrameGenerationOutputs(vk,b);
        }
        const std::array handles{direct.handle()};
        fused.configureDirectApplicationInputs(vk,handles,physical,format,usage);
        require(fused.directFrameGenerationInputCount()==1,"application view missing");
        for(unsigned phase=0;phase<6;++phase) {
            auto pixels=pattern(physical,phase);
            // Pixels outside the logical source rectangle must never bleed in.
            for(uint32_t y=0;y<physical.height;++y) for(uint32_t x=0;x<physical.width;++x)
                if(x>=source.width || y>=source.height) {
                    const auto p=(y*physical.width+x)*4;
                    pixels[p]=255;pixels[p+1]=0;pixels[p+2]=255;pixels[p+3]=255;
                }
            uploadImage(vk,original,pixels);uploadImage(vk,direct,pixels);
            const auto execute=[&](SpatialScaler& scaler,const vk::Image& app,const vk::Image& target) {
                const vk::CommandBuffer command(vk);command.begin(vk);
                scaler.record(vk,command,app.handle(),handoff ? target.handle() : VK_NULL_HANDLE,
                    VK_IMAGE_LAYOUT_GENERAL);
                command.end(vk);command.submit(vk);
            };
            blits=0;execute(base,original,phase%2 ? baseFgNext : baseFg);require(blits==2,"pre-FG baseline count changed");
            blits=0;execute(fused,direct,phase%2 ? fusedFgNext : fusedFg);require(blits==1,"pre-FG source copy remains");
            const auto a=download(vk,original),b=download(vk,direct);
            for(size_t i=0;i<a.size();++i)
                require(std::abs(int(a[i])-int(b[i]))<=1,"cropped direct input differs by more than 1 LSB");
            // Discard/readback semantics cannot destroy the borrowed game image.
            ++comparisons;
        }
        fused.clearDirectFrameGenerationInputs();
        uploadImage(vk,direct,pattern(physical,1));
        const std::array badFormat{direct.handle()};
        rejected([&]{fused.configureDirectApplicationInputs(vk,badFormat,physical,
            VK_FORMAT_R8G8B8A8_SRGB,usage);});
        rejected([&]{fused.configureDirectApplicationInputs(vk,badFormat,physical,
            format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT);});
    }
    std::cout << "Frame Fusion PRE-FG PASS: " << comparisons
        << " cropped RGBA/BGRA comparisons within 1 LSB; guarded source region; FG handoff; borrowed lifetime\n";
}
// Exercise the production raw transfer primitive independently of WSI and the
// licensed model. Both histories remain private: no acquired game image is
// borrowed by another VkDevice. Sampler comparison verifies channel semantics.
static void sourceTransfer(const vk::Vulkan& vk, const vk::CommandBuffer& command,
        const vk::Image& app, const vk::Image& history, const bool native) {
    const std::array before{
        imageBarrier(app.handle(), 0, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        imageBarrier(history.handle(), 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    if (native) {
        require(app.getFormat() == history.getFormat(), "raw transfer must preserve format");
        command.copyImage(vk, before, {app.handle(), history.handle()}, app.getExtent(), {});
    } else {
        command.blitImage(vk, before, {app.handle(), history.handle()}, app.getExtent(), {});
    }
    const std::array after{
        imageBarrier(app.handle(), VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(history.handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)};
    vk.df().CmdPipelineBarrier(command.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(after.size()), after.data());
}
static void restoreReadImage(const vk::Vulkan& vk, const vk::Image& image) {
    const vk::CommandBuffer command(vk); command.begin(vk);
    const auto after = imageBarrier(image.handle(), VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk.df().CmdPipelineBarrier(command.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &after);
    command.end(vk); command.submit(vk);
}
static std::vector<uint8_t> readGeneral(const vk::Vulkan& vk, const vk::Image& image) {
    auto bytes = download(vk, image);
    restoreReadImage(vk, image);
    return bytes;
}
static void nativeSourceIntegration(vk::Vulkan& vk) {
    unsigned comparisons = 0;
    const auto usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
    for (const auto size : {VkExtent2D{320,200}, VkExtent2D{317,193}}) {
        const vk::Image app(vk, size, format, usage);
        const VkExtent2D sampleSize=size;
        std::vector<vk::Image> baseline, native;
        for (unsigned i=0; i<2; ++i) {
            baseline.emplace_back(vk, size, VK_FORMAT_R8G8B8A8_UNORM, usage);
            native.emplace_back(vk, size, format, usage);
        }
        const vk::Image oldSample(vk, sampleSize, VK_FORMAT_R8G8B8A8_UNORM, usage);
        const vk::Image newSample(vk, sampleSize, VK_FORMAT_R8G8B8A8_UNORM, usage);
        SpatialScaler oldSampler(vk, size, sampleSize, VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako, 0.5F, std::nullopt);
        SpatialScaler newSampler(vk, size, sampleSize, VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako, 0.5F, std::nullopt);
        const std::array oldHandles{baseline[0].handle(), baseline[1].handle()};
        const std::array newHandles{native[0].handle(), native[1].handle()};
        oldSampler.configureDirectApplicationInputs(vk, oldHandles, size, VK_FORMAT_R8G8B8A8_UNORM, usage);
        newSampler.configureDirectApplicationInputs(vk, newHandles, size, format, usage);
        std::array<std::vector<uint8_t>, 2> historyBytes;
        for (unsigned phase=0; phase<12; ++phase) {
            const unsigned slot=phase%2, previous=1-slot;
            const auto pixels=pattern(size, phase);
            uploadImage(vk, app, pixels);
            for (const bool direct : {false,true}) {
                const vk::CommandBuffer command(vk); command.begin(vk);
                blits=0; copies=0;
                sourceTransfer(vk, command, app, direct ? native[slot] : baseline[slot], direct);
                command.end(vk); command.submit(vk);
                require(blits == (direct ? 0 : 1) && copies == (direct ? 1 : 0),
                    "native source transport must replace one blit with one copy");
            }
            historyBytes[slot]=pixels;
            require(readGeneral(vk, native[slot]) == pixels, "native source bytes changed");
            if (phase) require(readGeneral(vk, native[previous]) == historyBytes[previous],
                "native source transfer overwrote the previous history frame");
            record(vk, oldSampler, baseline[slot], oldSample);
            record(vk, newSampler, native[slot], newSample);
            restoreReadImage(vk, baseline[slot]);
            restoreReadImage(vk, native[slot]);
            require(download(vk, oldSample) == download(vk, newSample),
                "native source sampler colours differ from legacy RGBA input");
            ++comparisons;
        }
        // Recreating a context destroys both private histories and views.
        // The next size/format starts a fresh ring rather than borrowing WSI.
    }
    std::cout << "Frame Fusion NATIVE PASS: " << comparisons
        << " byte-identical RGBA/BGRA sampler comparisons; source bytes and alpha preserved; two-history isolation; one copy replaces one blit\n";
}
static void nativeSourceProfile(vk::Vulkan& vk) {
    const VkExtent2D size{1280,800};
    const auto usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    const vk::Image app(vk, size, VK_FORMAT_B8G8R8A8_UNORM, usage);
    const vk::Image baseline(vk, size, VK_FORMAT_R8G8B8A8_UNORM, usage);
    const vk::Image native(vk, size, VK_FORMAT_B8G8R8A8_UNORM, usage);
    uploadImage(vk, app, pattern(size,0));
    for (const bool direct : {false,true}) {
        TimestampQueryPool queries(vk,2);
        std::vector<double> samples;
        for (unsigned i=0; i<48; ++i) {
            const vk::CommandBuffer command(vk); command.begin(vk); queries.reset(command.handle());
            queries.write(command.handle(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0);
            sourceTransfer(vk, command, app, direct ? native : baseline, direct);
            queries.write(command.handle(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,1);
            command.end(vk); command.submit(vk);
            if (i>=8) samples.push_back(queries.microseconds().front());
        }
        std::sort(samples.begin(), samples.end());
        std::cout << (direct ? "native-source-copy" : "legacy-source-conversion")
            << " samples=40 gpu_median_us=" << (samples[19]+samples[20])/2
            << " gpu_p95_us=" << samples[37] << " source_transfers_per_real_frame=1\n";
    }
}
static void profile(vk::Vulkan& vk) {
    const VkExtent2D sourceSize{852,532},outputSize{1280,800};
    const vk::Image input(vk,sourceSize,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    const vk::Image output(vk,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    for(const bool direct : {false,true}) {
        SpatialScaler scaler(vk,sourceSize,outputSize,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,0.5F,std::nullopt);
        if(direct) { const std::array refs{std::cref(input)};
            scaler.configureDirectFrameGenerationInputs(vk,refs); }
        uploadImage(vk,input,pattern(sourceSize,0));
        for(unsigned i=0;i<8;++i) record(vk,scaler,input,output,
            i==0 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        TimestampQueryPool queries(vk,80);
        const vk::CommandBuffer command(vk);command.begin(vk);queries.reset(command.handle());
        for(uint32_t i=0;i<40;++i) {
            queries.write(command.handle(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,2*i);
            scaler.recordSourceToPresentation(vk,command,input.handle(),output.handle(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
            queries.write(command.handle(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,2*i+1);
        }
        command.end(vk);command.submit(vk);
        auto samples=queries.microseconds();std::sort(samples.begin(),samples.end());
        std::cout << (direct ? "fusion" : "baseline") << " samples=40 gpu_median_us="
            << (samples[19]+samples[20])/2 << " gpu_p95_us=" << samples[37]
            << " blits_per_generated_frame=" << (direct ? 1 : 2) << '\n';
    }
    std::cout << "Timing describes this Vulkan device; it is not a Steam Deck energy measurement.\n";
}
static void applicationProfile(vk::Vulkan& vk) {
    const VkExtent2D source{852,532},physical{1280,800};
    const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    const vk::Image app(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage);
    const vk::Image target(vk,physical,VK_FORMAT_R8G8B8A8_UNORM,usage|VK_IMAGE_USAGE_STORAGE_BIT);
    const auto pixels=pattern(physical,0);
    const vk::Buffer restore(vk,pixels.data(),pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto reset=[&](const vk::CommandBuffer& command) {
        const auto before=imageBarrier(app.handle(),VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vk.df().CmdPipelineBarrier(command.handle(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&before);
        const VkBufferImageCopy region{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent={physical.width,physical.height,1}};
        vk.df().CmdCopyBufferToImage(command.handle(),restore.handle(),app.handle(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        const auto after=imageBarrier(app.handle(),VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        vk.df().CmdPipelineBarrier(command.handle(),VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&after);
    };
    for(const bool direct:{false,true}) {
        SpatialScaler scaler(vk,source,physical,VK_FORMAT_R8G8B8A8_UNORM,
            ls::ScalingMethod::Mako,0.5F,std::nullopt);
        const std::array refs{std::cref(target)};scaler.configureDirectFrameGenerationOutputs(vk,refs);
        if(direct) {const std::array handles{app.handle()};
            scaler.configureDirectApplicationInputs(vk,handles,physical,VK_FORMAT_R8G8B8A8_UNORM,usage);}
        for(unsigned i=0;i<8;++i) {
            const vk::CommandBuffer command(vk);command.begin(vk);reset(command);
            scaler.record(vk,command,app.handle(),target.handle(),VK_IMAGE_LAYOUT_GENERAL);
            command.end(vk);command.submit(vk);
        }
        TimestampQueryPool queries(vk,80);
        const vk::CommandBuffer command(vk);command.begin(vk);queries.reset(command.handle());
        for(uint32_t i=0;i<40;++i) {
            reset(command); // Fresh game pixels, deliberately outside the timed interval.
            queries.write(command.handle(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,2*i);
            scaler.record(vk,command,app.handle(),target.handle(),VK_IMAGE_LAYOUT_GENERAL);
            queries.write(command.handle(),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,2*i+1);
        }
        command.end(vk);command.submit(vk);
        auto samples=queries.microseconds();std::sort(samples.begin(),samples.end());
        std::cout << (direct ? "pre-fusion" : "pre-baseline") << " samples=40 gpu_median_us="
            << (samples[19]+samples[20])/2 << " gpu_p95_us=" << samples[37]
            << " blits_per_real_frame=" << (direct ? 1 : 2) << '\n';
    }
}
int main(int argc,char** argv) {
    try {
        HeadlessDevice native;
        const auto instanceFunctions=vk::initVulkanInstanceFuncs(native.instance,vkGetInstanceProcAddr,false);
        vk::Vulkan vk(native.instance,native.device,native.physical,instanceFunctions,
            vk::initVulkanDeviceFuncs(instanceFunctions,native.device,false,false),
            native.family,false,false);
        auto& functions=const_cast<vk::VulkanDeviceFuncs&>(vk.df());
        originalBlit=functions.CmdBlitImage;functions.CmdBlitImage=countedBlit;
        originalCopy=functions.CmdCopyImage;functions.CmdCopyImage=countedCopy;
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(native.physical,&properties);
        std::cout << "Vulkan proof device: " << properties.deviceName << '\n';
        if(argc==2 && std::string(argv[1])=="--profile") {profile(vk);applicationProfile(vk);nativeSourceProfile(vk);}
        else {integration(vk);applicationIntegration(vk);nativeSourceIntegration(vk);}
        functions.CmdBlitImage=originalBlit;
        functions.CmdCopyImage=originalCopy;
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "Frame Fusion FAIL: " << error.what() << '\n';return 1;
    }
}
