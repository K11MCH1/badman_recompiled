#include "badman_gpu_device.hpp"
#include "psprecomp/common.hpp"
#if defined(_WIN32) || defined(__ANDROID__)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace badman {
namespace {
using psprecomp::Error;
#define BADMAN_VK_INSTANCE_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
 X(GetPhysicalDeviceFeatures) X(CreateDevice) X(GetDeviceProcAddr)
#define BADMAN_VK_DEVICE_FUNCTIONS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) \
 X(CreateFence) X(DestroyFence) X(ResetFences) X(QueueSubmit) X(WaitForFences) X(DeviceWaitIdle) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
 X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges)

class VulkanCompute final : public GpuDevice {
#ifdef _WIN32
    HMODULE loader_{};
#else
    void *loader_{};
#endif
    PFN_vkGetInstanceProcAddr get_instance_{};
    PFN_vkCreateInstance create_instance_{};
#define DECLARE(name) PFN_vk##name name##_{};
    BADMAN_VK_INSTANCE_FUNCTIONS(DECLARE)
    BADMAN_VK_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t family_{};
    VkShaderModule shader_{};
    VkDescriptorSetLayout descriptor_layout_{};
    VkPipelineLayout pipeline_layout_{};
    VkPipeline pipeline_{};
    VkDescriptorPool descriptor_pool_{};
    VkDescriptorSet descriptor_{};
    VkCommandPool command_pool_{};
    VkCommandBuffer command_{};
    VkFence fence_{};
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void *mapped{};
        VkDeviceSize capacity{};
        bool coherent{};
    } input_, state_;
    bool lost_{};
    std::mutex mutex_;
    std::vector<std::uint32_t> readback_; // Reused; holds only the latest completed transaction.

    void check(VkResult result, const char *operation) {
        if (result == VK_SUCCESS) return;
        if (result == VK_ERROR_DEVICE_LOST) lost_=true;
        throw Error(std::string("Vulkan ")+operation+" failed (VkResult "+std::to_string(result)+")");
    }
    template<class T> T instance_function(const char *name) {
        const auto result=get_instance_(instance_,name);
        if(!result)throw Error(std::string("Vulkan instance function missing: ")+name);
        return reinterpret_cast<T>(result);
    }
    template<class T> T device_function(const char *name) {
        const auto result=GetDeviceProcAddr_(device_,name);
        if(!result)throw Error(std::string("Vulkan device function missing: ")+name);
        return reinterpret_cast<T>(result);
    }
    void release(Buffer &buffer) noexcept {
        if(buffer.mapped && UnmapMemory_)UnmapMemory_(device_,buffer.memory);
        if(buffer.buffer && DestroyBuffer_)DestroyBuffer_(device_,buffer.buffer,nullptr);
        if(buffer.memory && FreeMemory_)FreeMemory_(device_,buffer.memory,nullptr);
        buffer={};
    }
    void cleanup() noexcept {
        if(device_ && DeviceWaitIdle_)DeviceWaitIdle_(device_);
        release(input_);release(state_);
        if(fence_ && DestroyFence_)DestroyFence_(device_,fence_,nullptr);
        if(command_pool_ && DestroyCommandPool_)DestroyCommandPool_(device_,command_pool_,nullptr);
        if(descriptor_pool_ && DestroyDescriptorPool_)DestroyDescriptorPool_(device_,descriptor_pool_,nullptr);
        if(pipeline_ && DestroyPipeline_)DestroyPipeline_(device_,pipeline_,nullptr);
        if(pipeline_layout_ && DestroyPipelineLayout_)DestroyPipelineLayout_(device_,pipeline_layout_,nullptr);
        if(descriptor_layout_ && DestroyDescriptorSetLayout_)DestroyDescriptorSetLayout_(device_,descriptor_layout_,nullptr);
        if(shader_ && DestroyShaderModule_)DestroyShaderModule_(device_,shader_,nullptr);
        if(device_ && DestroyDevice_)DestroyDevice_(device_,nullptr);
        if(instance_ && DestroyInstance_)DestroyInstance_(instance_,nullptr);
#ifdef _WIN32
        if(loader_)FreeLibrary(loader_);
#else
        if(loader_)dlclose(loader_);
#endif
        device_=VK_NULL_HANDLE;instance_=VK_NULL_HANDLE;loader_=nullptr;
    }
    void ensure(Buffer &buffer, VkDeviceSize bytes) {
        if(buffer.capacity>=bytes)return;
        Buffer next{};
        next.capacity=bytes;
        try {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size=bytes;info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(CreateBuffer_(device_,&info,nullptr,&next.buffer),"create storage buffer");
            VkMemoryRequirements requirements{};GetBufferMemoryRequirements_(device_,next.buffer,&requirements);
            unsigned index=memory_.memoryTypeCount,score=0;
            for(unsigned i=0;i<memory_.memoryTypeCount;++i) {
                const auto flags=memory_.memoryTypes[i].propertyFlags;
                if(!(requirements.memoryTypeBits&(1u<<i)) || !(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
                const auto candidate=1u+((flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)?2u:0u)+((flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?1u:0u);
                if(candidate>score){score=candidate;index=i;}
            }
            if(index==memory_.memoryTypeCount)throw Error("Vulkan storage buffer has no host-visible memory type");
            next.coherent=(memory_.memoryTypes[index].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=index;
            check(AllocateMemory_(device_,&allocation,nullptr,&next.memory),"allocate storage memory");
            check(BindBufferMemory_(device_,next.buffer,next.memory,0),"bind storage memory");
            check(MapMemory_(device_,next.memory,0,VK_WHOLE_SIZE,0,&next.mapped),"map storage memory");
        } catch(...) {release(next);throw;}
        release(buffer);buffer=next;
    }
    void flush(const Buffer &buffer) {
        if(buffer.coherent)return;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer.memory;range.size=VK_WHOLE_SIZE;
        check(FlushMappedMemoryRanges_(device_,1,&range),"flush host writes");
    }
    void initialize(std::span<const std::uint32_t> spirv) {
        // Validate the container before handing untrusted malformed words to an ICD.
        if(spirv.size()<5 || spirv[0]!=0x07230203u || spirv[4]!=0 || spirv[3]==0)
            throw Error("Vulkan compute shader has an invalid SPIR-V header");
        for(std::size_t i=5;i<spirv.size();) {
            const auto words=spirv[i]>>16u;
            if(!words || words>spirv.size()-i)throw Error("Vulkan compute shader has a truncated SPIR-V instruction");
            i+=words;
        }
#ifdef _WIN32
        loader_=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!loader_)throw Error("Vulkan loader is unavailable in Windows System32 (error "+std::to_string(GetLastError())+")");
        get_instance_=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader_,"vkGetInstanceProcAddr"));
#else
        // Android's system Vulkan loader.
        loader_=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
        if(!loader_)throw Error(std::string("Vulkan loader libvulkan.so is unavailable: ")+dlerror());
        get_instance_=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader_,"vkGetInstanceProcAddr"));
#endif
        if(!get_instance_)throw Error("Vulkan loader is missing vkGetInstanceProcAddr");
        create_instance_=instance_function<PFN_vkCreateInstance>("vkCreateInstance");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="PSPRecomp compute";app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance_info.pApplicationInfo=&app;
        check(create_instance_(&instance_info,nullptr,&instance_),"create instance");
#define LOAD_INSTANCE(name) name##_=instance_function<PFN_vk##name>("vk" #name);
        BADMAN_VK_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        std::uint32_t count{};check(EnumeratePhysicalDevices_(instance_,&count,nullptr),"enumerate physical devices");
        if(!count)throw Error("Vulkan has no physical compute device");
        std::vector<VkPhysicalDevice> devices(count);
        check(EnumeratePhysicalDevices_(instance_,&count,devices.data()),"enumerate physical device handles");
        unsigned best{};
        for(auto physical:devices) {
            VkPhysicalDeviceProperties properties{};GetPhysicalDeviceProperties_(physical,&properties);
            if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
            if(properties.limits.maxComputeWorkGroupInvocations<64 || properties.limits.maxComputeWorkGroupSize[0]<64)continue;
            std::uint32_t families{};GetPhysicalDeviceQueueFamilyProperties_(physical,&families,nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);GetPhysicalDeviceQueueFamilyProperties_(physical,&families,queues.data());
            for(unsigned i=0;i<families;++i)if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
                const auto rank=properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?3u:
                    properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?2u:1u;
                if(rank>best){best=rank;physical_=physical;properties_=properties;family_=i;}
                break;
            }
        }
        if(!physical_)throw Error("Vulkan has no hardware device supporting 64-thread compute");
        GetPhysicalDeviceMemoryProperties_(physical_,&memory_);
        VkPhysicalDeviceFeatures supported{};GetPhysicalDeviceFeatures_(physical_,&supported);
        VkPhysicalDeviceFeatures enabled{};enabled.robustBufferAccess=supported.robustBufferAccess;
        const float priority=1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex=family_;queue_info.queueCount=1;queue_info.pQueuePriorities=&priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue_info;device_info.pEnabledFeatures=&enabled;
        check(CreateDevice_(physical_,&device_info,nullptr,&device_),"create hardware device");
#define LOAD_DEVICE(name) name##_=device_function<PFN_vk##name>("vk" #name);
        BADMAN_VK_DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        GetDeviceQueue_(device_,family_,0,&queue_);
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=spirv.size_bytes();shader_info.pCode=spirv.data();
        check(CreateShaderModule_(device_,&shader_info,nullptr,&shader_),"create SPIR-V shader module");
        VkDescriptorSetLayoutBinding bindings[2]{};
        for(unsigned i=0;i<2;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptor_info.bindingCount=2;descriptor_info.pBindings=bindings;
        check(CreateDescriptorSetLayout_(device_,&descriptor_info,nullptr,&descriptor_layout_),"create descriptor layout");
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout_info.setLayoutCount=1;layout_info.pSetLayouts=&descriptor_layout_;
        check(CreatePipelineLayout_(device_,&layout_info,nullptr,&pipeline_layout_),"create pipeline layout");
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.layout=pipeline_layout_;
        pipeline_info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline_info.stage.module=shader_;pipeline_info.stage.pName="main";
        check(CreateComputePipelines_(device_,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&pipeline_),"create compute pipeline");
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool_info.maxSets=1;pool_info.poolSizeCount=1;pool_info.pPoolSizes=&pool_size;
        check(CreateDescriptorPool_(device_,&pool_info,nullptr,&descriptor_pool_),"create descriptor pool");
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set_info.descriptorPool=descriptor_pool_;set_info.descriptorSetCount=1;set_info.pSetLayouts=&descriptor_layout_;
        check(AllocateDescriptorSets_(device_,&set_info,&descriptor_),"allocate descriptor set");
        VkCommandPoolCreateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};command_info.queueFamilyIndex=family_;
        check(CreateCommandPool_(device_,&command_info,nullptr,&command_pool_),"create command pool");
        VkCommandBufferAllocateInfo command_allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_allocate.commandPool=command_pool_;command_allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_allocate.commandBufferCount=1;
        check(AllocateCommandBuffers_(device_,&command_allocate,&command_),"allocate command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(CreateFence_(device_,&fence_info,nullptr,&fence_),"create completion fence");
    }
public:
    explicit VulkanCompute(std::span<const std::uint32_t> spirv) {
        try{initialize(spirv);}catch(...){cleanup();throw;}
    }
    ~VulkanCompute() override {cleanup();}
    std::string description() const override {return std::string("Vulkan compute: ")+properties_.deviceName;}
    void dispatch(std::span<const std::uint32_t> input,std::span<std::uint32_t> state,std::uint32_t groups) override {
        const std::lock_guard lock(mutex_);
        if(lost_)throw Error("Vulkan compute device is unusable after a failed device transaction; recreate it before dispatch");
        if(!groups)return;
        if(input.empty() || state.empty())throw Error("Vulkan compute requires nonempty input and state buffers");
        if(groups>properties_.limits.maxComputeWorkGroupCount[0])throw Error("Vulkan compute group count exceeds the hardware limit");
        if(input.size_bytes()>properties_.limits.maxStorageBufferRange || state.size_bytes()>properties_.limits.maxStorageBufferRange)
            throw Error("Vulkan compute storage buffer exceeds the hardware range limit");
        // Allocate the complete readback before starting the GPU transaction.
        // Caller state is committed only after a successful fence and invalidate.
        auto &output=readback_;
        output.resize(state.size());
        ensure(input_,input.size_bytes());ensure(state_,state.size_bytes());
        std::memcpy(input_.mapped,input.data(),input.size_bytes());std::memcpy(state_.mapped,state.data(),state.size_bytes());
        flush(input_);flush(state_);
        const VkDescriptorBufferInfo buffers[2]={{input_.buffer,0,input.size_bytes()},{state_.buffer,0,state.size_bytes()}};
        VkWriteDescriptorSet writes[2]{};
        for(unsigned i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=descriptor_;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];}
        UpdateDescriptorSets_(device_,2,writes,0,nullptr);
        check(ResetCommandPool_(device_,command_pool_,0),"reset command pool");
        check(ResetFences_(device_,1,&fence_),"reset completion fence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer_(command_,&begin),"begin compute commands");
        VkBufferMemoryBarrier barriers[2]{};
        for(unsigned i=0;i<2;++i){auto &b=barriers[i];b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|(i?VK_ACCESS_SHADER_WRITE_BIT:0);b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buffers[i].buffer;b.size=VK_WHOLE_SIZE;}
        CmdPipelineBarrier_(command_,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers,0,nullptr);
        CmdBindPipeline_(command_,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
        CmdBindDescriptorSets_(command_,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout_,0,1,&descriptor_,0,nullptr);
        CmdDispatch_(command_,groups,1,1);
        VkBufferMemoryBarrier readback{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;readback.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;readback.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;readback.buffer=state_.buffer;readback.size=VK_WHOLE_SIZE;
        CmdPipelineBarrier_(command_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&readback,0,nullptr);
        check(EndCommandBuffer_(command_),"end compute commands");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command_;
        const auto submitted=QueueSubmit_(queue_,1,&submit,fence_);
        if(submitted!=VK_SUCCESS)lost_=true;
        check(submitted,"submit compute work");
        const auto completed=WaitForFences_(device_,1,&fence_,VK_TRUE,std::numeric_limits<std::uint64_t>::max());
        // A failed wait need not prove the queue is idle. Never overwrite or
        // recycle potentially in-flight buffers on a later dispatch.
        if(completed!=VK_SUCCESS)lost_=true;
        check(completed,"wait for compute completion");
        if(!state_.coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=state_.memory;range.size=VK_WHOLE_SIZE;check(InvalidateMappedMemoryRanges_(device_,1,&range),"invalidate compute readback");}
        std::memcpy(output.data(),state_.mapped,state.size_bytes());
        std::copy(output.begin(),output.end(),state.begin());
    }
};
#undef BADMAN_VK_INSTANCE_FUNCTIONS
#undef BADMAN_VK_DEVICE_FUNCTIONS
} // namespace
std::unique_ptr<GpuDevice> make_vulkan_compute(std::span<const std::uint32_t> spirv) {return std::make_unique<VulkanCompute>(spirv);}
} // namespace badman
#else
namespace badman {
std::unique_ptr<GpuDevice> make_vulkan_compute(std::span<const std::uint32_t>) {
    throw psprecomp::Error("Vulkan compute device requires Windows or Android in this profile");
}
}
#endif
