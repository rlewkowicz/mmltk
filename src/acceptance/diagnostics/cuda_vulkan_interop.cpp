// Standalone CUDA/Vulkan linear-image external-memory comparison.
// No application, Firefox, window system, or graphics shader dependencies.
// argv: width height allocation-pairs transfers-per-image validation-layer context-mode memory-owner
#include <cuda.h>
#include <vulkan/vulkan.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "cuda_vulkan_fill.h"

static void check(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* text = "unknown";
    cuGetErrorString(result, &text);
    throw std::runtime_error(std::string(operation) + ": " + text);
}
static void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
static void check(cudaError_t result, const char* operation) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(result));
}
#define CHECK(call) check((call), #call)

// Vulkan extensible structures share this header; zero every remaining field.
template <class T>
static T vk_structure(VkStructureType type, void* next = nullptr) {
    T value{};
    value.sType = type;
    value.pNext = next;
    return value;
}

struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

// Exercise dup/SCM_RIGHTS with an actual receiving descriptor.
static void transfer_fd(int original, Fd& received) {
    Fd sender, receiver, duplicate;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
        throw std::runtime_error("socketpair");
    sender.value = sockets[0];
    receiver.value = sockets[1];
    duplicate.value = dup(original);
    if (duplicate.value < 0) throw std::runtime_error("dup");
    char byte = 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &duplicate.value, sizeof(int));
    if (sendmsg(sender.value, &message, MSG_NOSIGNAL) != 1)
        throw std::runtime_error("sendmsg");
    control.fill(0);
    if (recvmsg(receiver.value, &message, MSG_CMSG_CLOEXEC) != 1)
        throw std::runtime_error("recvmsg");
    header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int)) || message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
        throw std::runtime_error("invalid SCM_RIGHTS receipt");
    std::memcpy(&received.value, CMSG_DATA(header), sizeof(int));
}

struct Device {
    CUdevice cuda_device = 0;
    CUcontext context = nullptr;
    bool isolated_context = false;
    CUstream stream = nullptr;
    CUcontext producer_context = nullptr;
    CUstream producer_stream = nullptr;
    CUevent producer_complete = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice vk = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    std::atomic<unsigned> validation_errors{0};
    VkPhysicalDeviceMemoryProperties memory{};
    ~Device() {
        if (producer_context) {
            cuCtxSetCurrent(producer_context);
            if (producer_stream) cuStreamSynchronize(producer_stream);
        }
        if (context) cuCtxSetCurrent(context);
        if (stream) cuStreamSynchronize(stream);
        if (vk) vkDeviceWaitIdle(vk);
        if (fence) vkDestroyFence(vk, fence, nullptr);
        if (pool) vkDestroyCommandPool(vk, pool, nullptr);
        if (vk) vkDestroyDevice(vk, nullptr);
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        if (stream) cuStreamDestroy(stream);
        if (producer_context) {
            cuCtxSetCurrent(producer_context);
            if (producer_complete) cuEventDestroy(producer_complete);
            if (producer_stream) cuStreamDestroy(producer_stream);
            cuCtxDestroy(producer_context);
            if (context) cuCtxSetCurrent(context);
        }
        if (context) {
            if (isolated_context) cuCtxDestroy(context);
            else cuDevicePrimaryCtxRelease(cuda_device);
        }
    }
    void initialize(bool validation, unsigned mode) {
        CHECK(cuInit(0));
        CHECK(cuDeviceGet(&cuda_device, 0));
        isolated_context = mode != 0;
        if (isolated_context) CHECK(cuCtxCreate(&context, nullptr, CU_CTX_SCHED_AUTO, cuda_device));
        else CHECK(cuDevicePrimaryCtxRetain(&context, cuda_device));
        CHECK(cuCtxSetCurrent(context));
        CHECK(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
        CUuuid uuid{};
        CHECK(cuDeviceGetUuid(&uuid, cuda_device));
        if (mode == 2) {
            CHECK(cuCtxCreate(&producer_context, nullptr, CU_CTX_SCHED_AUTO, cuda_device));
            CHECK(cuStreamCreate(&producer_stream, CU_STREAM_NON_BLOCKING));
            CHECK(cuEventCreate(&producer_complete, CU_EVENT_DISABLE_TIMING));
            CHECK(cuCtxSetCurrent(context));
        }
        std::printf("cuda_context=%s\n", mode == 2 ? "separate_producer_and_semaphore" :
                    isolated_context ? "isolated" : "primary");
        auto app = vk_structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "standalone-cuda-vulkan";
        app.apiVersion = VK_API_VERSION_1_2;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        auto info = vk_structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &app;
        info.enabledLayerCount = validation ? 1 : 0;
        info.ppEnabledLayerNames = &layer;
        const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        info.enabledExtensionCount = validation ? 1 : 0;
        info.ppEnabledExtensionNames = &debug_extension;
        auto debug = vk_structure<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pUserData = &validation_errors;
        debug.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                  VkDebugUtilsMessageTypeFlagsEXT,
                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) -> VkBool32 {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                static_cast<std::atomic<unsigned>*>(user)->fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "VULKAN: %s\n", data->pMessage);
            return VK_FALSE;
        };
        if (validation) info.pNext = &debug;
        CHECK(vkCreateInstance(&info, nullptr, &instance));
        if (validation) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create) throw std::runtime_error("missing vkCreateDebugUtilsMessengerEXT");
            CHECK(create(instance, &debug, nullptr, &messenger));
        }
        uint32_t count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            auto ids = vk_structure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
            auto properties = vk_structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids);
            vkGetPhysicalDeviceProperties2(candidate, &properties);
            if (std::memcmp(uuid.bytes, ids.deviceUUID, VK_UUID_SIZE) == 0) {
                physical = candidate;
                std::printf("device=%s driver=%u uuid=", properties.properties.deviceName,
                            properties.properties.driverVersion);
                for (unsigned char value : ids.deviceUUID) std::printf("%02x", value);
                std::puts("");
                break;
            }
        }
        if (!physical) throw std::runtime_error("no UUID-matched CUDA/Vulkan device");
        CHECK(vkEnumeratePhysicalDeviceGroups(instance, &count, nullptr));
        std::vector<VkPhysicalDeviceGroupProperties> groups(
            count, vk_structure<VkPhysicalDeviceGroupProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES));
        CHECK(vkEnumeratePhysicalDeviceGroups(instance, &count, groups.data()));
        for (const auto& group : groups)
            for (uint32_t i = 0; i < group.physicalDeviceCount; ++i)
                if (group.physicalDevices[i] == physical && group.physicalDeviceCount != 1)
                    throw std::runtime_error("CUDA interop requires a single-device Vulkan group");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (; family < count; ++family)
            if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        if (family == count) throw std::runtime_error("no graphics queue");
        auto timeline = vk_structure<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        auto features = vk_structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline);
        vkGetPhysicalDeviceFeatures2(physical, &features);
        if (!timeline.timelineSemaphore) throw std::runtime_error("no timeline semaphore support");
        auto external = vk_structure<VkPhysicalDeviceExternalSemaphoreInfo>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO);
        external.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        auto type = vk_structure<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        external.pNext = &type;
        auto semaphore_properties = vk_structure<VkExternalSemaphoreProperties>(VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES);
        vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &external, &semaphore_properties);
        if (!(semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT))
            throw std::runtime_error("timeline opaque FD export unavailable");
        float priority = 1;
        auto queue_info = vk_structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* extensions[]{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
        auto device_info = vk_structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &timeline);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 2;
        device_info.ppEnabledExtensionNames = extensions;
        CHECK(vkCreateDevice(physical, &device_info, nullptr, &vk));
        vkGetDeviceQueue(vk, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        auto pool_info = vk_structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        CHECK(vkCreateCommandPool(vk, &pool_info, nullptr, &pool));
        auto allocate = vk_structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(vk, &allocate, &command));
        auto fence_info = vk_structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        CHECK(vkCreateFence(vk, &fence_info, nullptr, &fence));
    }
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1U << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no compatible memory type");
    }
    void begin() {
        CHECK(vkResetCommandBuffer(command, 0));
        auto info = vk_structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(command, &info));
    }
    void submit(VkSemaphore semaphore = VK_NULL_HANDLE, uint64_t ready = 0) {
        CHECK(vkEndCommandBuffer(command));
        uint64_t release = ready + 1;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        auto timeline = vk_structure<VkTimelineSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &ready;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &release;
        auto info = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        if (semaphore) {
            info.pNext = &timeline;
            info.waitSemaphoreCount = info.signalSemaphoreCount = 1;
            info.pWaitSemaphores = info.pSignalSemaphores = &semaphore;
            info.pWaitDstStageMask = &stage;
        }
        CHECK(vkResetFences(vk, 1, &fence));
        CHECK(vkQueueSubmit(queue, 1, &info, fence));
        CHECK(vkWaitForFences(vk, 1, &fence, VK_TRUE, 10'000'000'000ULL));
    }
};

enum class MemoryOwner { Cuda, Vulkan, VulkanDedicated };

struct Image {
    Device& device;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE, host_memory = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    cudaExternalSemaphore_t cuda_semaphore = nullptr;
    CUexternalMemory external_memory = nullptr;
    CUmemGenericAllocationHandle allocation = 0;
    CUdeviceptr address = 0;
    bool mapped_cuda = false;
    void* host = nullptr;
    void* native = nullptr;
    size_t bytes = 0;
    VkSubresourceLayout layout{};
    uint32_t width, height;
    uint64_t sequence = 0;
    MemoryOwner memory_owner;
    Image(Device& owner, uint32_t w, uint32_t h, MemoryOwner memory)
        : device(owner), width(w), height(h), memory_owner(memory) {}
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image() {
        if (device.producer_context) {
            cuCtxSetCurrent(device.producer_context);
            cuStreamSynchronize(device.producer_stream);
        }
        cuCtxSetCurrent(device.context);
        cuStreamSynchronize(device.stream);
        vkDeviceWaitIdle(device.vk);
        if (cuda_semaphore) cudaDestroyExternalSemaphore(cuda_semaphore);
        if (semaphore) vkDestroySemaphore(device.vk, semaphore, nullptr);
        cuCtxSetCurrent(device.producer_context ? device.producer_context : device.context);
        if (external_memory) {
            if (address) cuMemFree(address);
            cuDestroyExternalMemory(external_memory);
        } else {
            if (mapped_cuda) cuMemUnmap(address, bytes);
            if (address) cuMemAddressFree(address, bytes);
            if (allocation) cuMemRelease(allocation);
        }
        if (image) vkDestroyImage(device.vk, image, nullptr);
        if (image_memory) vkFreeMemory(device.vk, image_memory, nullptr);
        if (host) vkUnmapMemory(device.vk, host_memory);
        if (readback) vkDestroyBuffer(device.vk, readback, nullptr);
        if (host_memory) vkFreeMemory(device.vk, host_memory, nullptr);
        cuCtxSetCurrent(device.context);
        if (native) cuMemFreeHost(native);
    }
    void barrier(uint32_t from, uint32_t to, VkImageLayout old_layout,
                 VkAccessFlags source, VkAccessFlags destination) {
        auto barrier = vk_structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        barrier.oldLayout = old_layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = from;
        barrier.dstQueueFamilyIndex = to;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(device.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    void initialize() {
        constexpr auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(device.physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
        constexpr auto sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((format_properties.linearTilingFeatures & sampled) != sampled)
            throw std::runtime_error("linear RGBA filtered sampling unavailable");
        auto external = vk_structure<VkPhysicalDeviceExternalImageFormatInfo>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        auto format = vk_structure<VkPhysicalDeviceImageFormatInfo2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external);
        format.format = VK_FORMAT_R8G8B8A8_UNORM;
        format.type = VK_IMAGE_TYPE_2D;
        format.tiling = VK_IMAGE_TILING_LINEAR;
        format.usage = usage;
        auto external_properties = vk_structure<VkExternalImageFormatProperties>(VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
        auto properties = vk_structure<VkImageFormatProperties2>(VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_properties);
        CHECK(vkGetPhysicalDeviceImageFormatProperties2(device.physical, &format, &properties));
        auto features = external_properties.externalMemoryProperties.externalMemoryFeatures;
        const auto required_feature = memory_owner == MemoryOwner::Cuda ?
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
        if (!(features & required_feature) ||
            !(external_properties.externalMemoryProperties.compatibleHandleTypes & external.handleType))
            throw std::runtime_error("requested linear RGBA opaque FD sharing direction unavailable");
        if (width > properties.imageFormatProperties.maxExtent.width ||
            height > properties.imageFormatProperties.maxExtent.height ||
            !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            throw std::runtime_error("requested image exceeds external image limits");
        auto external_image = vk_structure<VkExternalMemoryImageCreateInfo>(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
        external_image.handleTypes = external.handleType;
        auto create = vk_structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external_image);
        create.imageType = format.type;
        create.format = format.format;
        create.extent = {width, height, 1};
        create.mipLevels = create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = format.tiling;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        CHECK(vkCreateImage(device.vk, &create, nullptr, &image));
        VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        vkGetImageSubresourceLayout(device.vk, image, &subresource, &layout);
        auto dedicated = vk_structure<VkMemoryDedicatedRequirements>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
        auto requirements = vk_structure<VkMemoryRequirements2>(VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated);
        auto image_info = vk_structure<VkImageMemoryRequirementsInfo2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2);
        image_info.image = image;
        vkGetImageMemoryRequirements2(device.vk, &image_info, &requirements);
        CHECK(cuCtxSetCurrent(device.producer_context ? device.producer_context : device.context));
        bytes = std::max<uint64_t>(requirements.memoryRequirements.size, layout.offset + layout.rowPitch * height);
        const bool needs_dedicated = dedicated.requiresDedicatedAllocation ||
                                    (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ||
                                    memory_owner == MemoryOwner::VulkanDedicated;
        auto dedicate = vk_structure<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
        dedicate.image = image;
        auto allocate = vk_structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocate.memoryTypeIndex = device.memory_type(requirements.memoryRequirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Fd exported, received;
        if (memory_owner == MemoryOwner::Cuda) {
            CUmemAllocationProp property{};
            property.type = CU_MEM_ALLOCATION_TYPE_PINNED;
            property.location = {CU_MEM_LOCATION_TYPE_DEVICE, device.cuda_device};
            property.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
            size_t granularity = 0;
            CHECK(cuMemGetAllocationGranularity(&granularity, &property, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED));
            bytes = (bytes + granularity - 1) / granularity * granularity;
            CHECK(cuMemCreate(&allocation, bytes, &property, 0));
            CHECK(cuMemAddressReserve(&address, bytes, granularity, 0, 0));
            CHECK(cuMemMap(address, bytes, 0, allocation, 0));
            mapped_cuda = true;
            CUmemAccessDesc access{property.location, CU_MEM_ACCESS_FLAGS_PROT_READWRITE};
            CHECK(cuMemSetAccess(address, bytes, &access, 1));
            CHECK(cuMemExportToShareableHandle(&exported.value, allocation, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
            transfer_fd(exported.value, received);
            auto import = vk_structure<VkImportMemoryFdInfoKHR>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
            import.handleType = external.handleType;
            import.fd = received.value;
            if (needs_dedicated) import.pNext = &dedicate;
            allocate.pNext = &import;
            allocate.allocationSize = bytes;
            CHECK(vkAllocateMemory(device.vk, &allocate, nullptr, &image_memory));
            received.value = -1; // Vulkan owns the imported FD after success.
        } else {
            auto export_info = vk_structure<VkExportMemoryAllocateInfo>(VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
            export_info.handleTypes = external.handleType;
            if (needs_dedicated) export_info.pNext = &dedicate;
            allocate.pNext = &export_info;
            allocate.allocationSize = bytes;
            CHECK(vkAllocateMemory(device.vk, &allocate, nullptr, &image_memory));
        }
        allocate.pNext = nullptr;
        CHECK(vkBindImageMemory(device.vk, image, image_memory, 0));
        if (memory_owner != MemoryOwner::Cuda) {
            // Vulkan owns the allocation; CUDA receives its exact exported size.
            auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device.vk, "vkGetMemoryFdKHR"));
            if (!get_fd) throw std::runtime_error("missing vkGetMemoryFdKHR");
            auto get = vk_structure<VkMemoryGetFdInfoKHR>(VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);
            get.memory = image_memory;
            get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            CHECK(get_fd(device.vk, &get, &exported.value));
            transfer_fd(exported.value, received);
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC import{};
            import.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
            import.handle.fd = received.value;
            import.size = bytes;
            if (needs_dedicated) import.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
            CHECK(cuImportExternalMemory(&external_memory, &import));
            received.value = -1; // CUDA owns the imported FD after success.
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapping{};
            mapping.size = bytes;
            CHECK(cuExternalMemoryGetMappedBuffer(&address, external_memory, &mapping));
        }
        std::printf("image=%ux%u pitch=%lu offset=%lu bytes=%zu memory_type=%u dedicated=%d cuda_va=%llu memory_owner=%s\n",
                    width, height, layout.rowPitch, layout.offset, bytes, allocate.memoryTypeIndex,
                    needs_dedicated, address, memory_owner == MemoryOwner::Cuda ? "cuda" : "vulkan");
        auto timeline = vk_structure<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        auto export_semaphore = vk_structure<VkExportSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, &timeline);
        export_semaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        auto semaphore_info = vk_structure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &export_semaphore);
        CHECK(vkCreateSemaphore(device.vk, &semaphore_info, nullptr, &semaphore));
        auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device.vk, "vkGetSemaphoreFdKHR"));
        if (!get_fd) throw std::runtime_error("missing vkGetSemaphoreFdKHR");
        auto get = vk_structure<VkSemaphoreGetFdInfoKHR>(VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR);
        get.semaphore = semaphore;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        Fd semaphore_fd;
        CHECK(get_fd(device.vk, &get, &semaphore_fd.value));
        cudaExternalSemaphoreHandleDesc cuda_import{};
        cuda_import.type = cudaExternalSemaphoreHandleTypeTimelineSemaphoreFd;
        cuda_import.handle.fd = semaphore_fd.value;
        CHECK(cuCtxSetCurrent(device.context));
        CHECK(cudaImportExternalSemaphore(&cuda_semaphore, &cuda_import));
        semaphore_fd.value = -1;
        // Complete initialization before any CUDA pixel write.
        device.begin();
        barrier(VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_IMAGE_LAYOUT_UNDEFINED, 0, 0);
        barrier(device.family, VK_QUEUE_FAMILY_EXTERNAL, VK_IMAGE_LAYOUT_GENERAL, 0, 0);
        if (memory_owner == MemoryOwner::Cuda) device.submit();
        else device.submit(semaphore, 0); // Vulkan signals 1 before the first CUDA access.
        auto buffer = vk_structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer.size = size_t(width) * height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        CHECK(vkCreateBuffer(device.vk, &buffer, nullptr, &readback));
        VkMemoryRequirements buffer_requirements{};
        vkGetBufferMemoryRequirements(device.vk, readback, &buffer_requirements);
        allocate.pNext = nullptr;
        allocate.allocationSize = buffer_requirements.size;
        allocate.memoryTypeIndex = device.memory_type(buffer_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CHECK(vkAllocateMemory(device.vk, &allocate, nullptr, &host_memory));
        CHECK(vkBindBufferMemory(device.vk, readback, host_memory, 0));
        CHECK(vkMapMemory(device.vk, host_memory, 0, VK_WHOLE_SIZE, 0, &host));
        CHECK(cuMemAllocHost(&native, buffer.size));
    }
    void transfer(uint32_t expected) {
        CHECK(cuCtxSetCurrent(device.context));
        const uint64_t timeline_base = memory_owner == MemoryOwner::Cuda ? 0 : 1;
        if (sequence || timeline_base) {
            cudaExternalSemaphoreWaitParams wait{};
            wait.params.fence.value = sequence * 2 + timeline_base;
            CHECK(cudaWaitExternalSemaphoresAsync(&cuda_semaphore, &wait, 1, device.stream));
            // The product's physical gate opens only after its CUDA release wait settles.
            CHECK(cuStreamSynchronize(device.stream));
        }
        CHECK(cuCtxSetCurrent(device.producer_context ? device.producer_context : device.context));
        const auto writing_stream = device.producer_context ? device.producer_stream : device.stream;
        CHECK(cuda_vulkan_fill(reinterpret_cast<void*>(address + layout.offset), layout.rowPitch,
                              width, height, expected, writing_stream));
        if (device.producer_context) {
            CHECK(cuEventRecord(device.producer_complete, device.producer_stream));
            CHECK(cuCtxSetCurrent(device.context));
            CHECK(cuStreamWaitEvent(device.stream, device.producer_complete, 0));
        }
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = address + layout.offset;
        copy.srcPitch = layout.rowPitch;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = native;
        copy.dstPitch = size_t(width) * 4;
        copy.WidthInBytes = copy.dstPitch;
        copy.Height = height;
        CHECK(cuMemcpy2DAsync(&copy, device.stream));
        uint64_t ready = ++sequence * 2 - 1 + timeline_base;
        cudaExternalSemaphoreSignalParams signal{};
        signal.params.fence.value = ready;
        CHECK(cudaSignalExternalSemaphoresAsync(&cuda_semaphore, &signal, 1, device.stream));
        CHECK(cuStreamSynchronize(device.stream));
        device.begin();
        barrier(VK_QUEUE_FAMILY_EXTERNAL, device.family, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(device.command, image, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &region);
        barrier(device.family, VK_QUEUE_FAMILY_EXTERNAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
        auto host_barrier = vk_structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(device.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        device.submit(semaphore, ready);
        size_t native_bad = 0, vulkan_bad = 0, zero = 0;
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            native_bad += static_cast<uint32_t*>(native)[i] != expected;
            vulkan_bad += static_cast<uint32_t*>(host)[i] != expected;
            zero += static_cast<uint32_t*>(host)[i] == 0;
        }
        if (native_bad || vulkan_bad) {
            std::printf("FAIL transfer=%lu expected=%08x native_first=%08x vulkan_first=%08x native_bad=%zu vulkan_bad=%zu vulkan_zero=%zu\n",
                        sequence, expected, static_cast<uint32_t*>(native)[0], static_cast<uint32_t*>(host)[0],
                        native_bad, vulkan_bad, zero);
            throw std::runtime_error("pixel mismatch");
        }
    }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
            std::puts("Usage: ./mmltk --test cuda-vulkan -- [width height allocation-pairs transfers validation context-mode memory-owner]");
            std::puts("Defaults: 894 512 32 8 0 2 0. validation is 0/1; context-mode is 0=primary, 1=isolated, 2=separate producer/semaphore.");
            std::puts("memory-owner: 0=CUDA VMM, 1=Vulkan, 2=Vulkan dedicated. Vulkan-owned memory starts with Vulkan signal 1.");
            std::puts("Two alternating images per pair; a CUDA kernel writes every pixel before each timeline handoff and comparison.");
            std::puts("CUDA and Vulkan share one process; FD transfer uses a local SCM_RIGHTS socket pair.");
            return 0;
        }
        std::array<unsigned, 7> options{894, 512, 32, 8, 0, 2, 0};
        if (argc > 8) throw std::runtime_error("too many arguments; use --help");
        for (int i = 1; i < argc; ++i) {
            const auto [end, error] = std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), options[i - 1]);
            if (error != std::errc{} || *end) throw std::runtime_error("invalid numeric argument");
        }
        auto [width, height, pairs, transfers, validation, context_mode, memory_owner] = options;
        if (!width || !height || width > 4096 || height > 4096 || !pairs || pairs > 256 ||
            !transfers || transfers > 256 || validation > 1 || context_mode > 2 || memory_owner > 2)
            throw std::runtime_error("argument out of range");
        Device device;
        device.initialize(validation, context_mode);
        for (unsigned pair = 0; pair < pairs; ++pair) {
            const auto owner = static_cast<MemoryOwner>(memory_owner);
            Image first(device, width, height, owner), second(device, width, height, owner);
            first.initialize();
            second.initialize();
            for (unsigned iteration = 0; iteration < transfers; ++iteration) {
                first.transfer(0xff000001U | (pair << 16) | (iteration << 8));
                second.transfer(0xff800002U | (pair << 16) | (iteration << 8));
            }
        }
        if (device.validation_errors.load(std::memory_order_relaxed))
            throw std::runtime_error("Vulkan validation reported errors");
        std::printf("PASS images=%u transfers=%u pixels_per_transfer=%lu\n",
                    pairs * 2, pairs * 2 * transfers, size_t(width) * height);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 1;
    }
}
