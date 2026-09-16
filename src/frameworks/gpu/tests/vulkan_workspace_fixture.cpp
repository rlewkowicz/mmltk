#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include <vulkan/vulkan.h>
#include <cuda.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace mmltk::frameworks::gpu::test_support {
namespace {
void Check(VkResult status) {
    if (status != VK_SUCCESS) throw std::runtime_error("Vulkan workspace fixture operation failed");
}
}  // namespace
struct VulkanWorkspaceFixture::State final {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    ImageWorkspaceLayout layout;
    ~State() {
        if (device) {
            // All fixture submissions finish before construction returns.
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
VulkanWorkspaceFixture::VulkanWorkspaceFixture(int ordinal, std::uint32_t width, std::uint32_t height) : state_(std::make_unique<State>()) {
    auto& state = *state_;
    CUuuid uuid{};
    if (cuDeviceGetUuid(&uuid, ordinal) != CUDA_SUCCESS) throw std::runtime_error("CUDA fixture UUID unavailable");
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    Check(vkCreateInstance(&instance_info, nullptr, &state.instance));
    std::uint32_t count = 0U;
    Check(vkEnumeratePhysicalDevices(state.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(state.instance, &count, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (auto candidate : devices) {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        if (std::memcmp(id.deviceUUID, uuid.bytes, VK_UUID_SIZE) == 0) {
            physical = candidate;
            break;
        }
    }
    if (!physical) throw std::runtime_error("matching Vulkan fixture device unavailable");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0U;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) throw std::runtime_error("Vulkan fixture graphics queue unavailable");
    const float priority = 1.0F;
    const char* extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1U;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1U;
    device_info.ppEnabledExtensionNames = &extension;
    Check(vkCreateDevice(physical, &device_info, nullptr, &state.device));
    VkPhysicalDeviceExternalImageFormatInfo external_query{};
    external_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext = &external_query;
    query.format = VK_FORMAT_R8G8B8A8_UNORM;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_LINEAR;
    query.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties external_properties{};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &external_properties;
    Check(vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &properties));
    if (!(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        throw std::runtime_error("Vulkan fixture image cannot be exported");
    VkExternalMemoryImageCreateInfo external_image{};
    external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_image;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(state.device, &image_info, nullptr, &state.image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(state.device, state.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    std::uint32_t type = 0U;
    while (type < memory_properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1U << type)) || !(memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
        ++type;
    if (type == memory_properties.memoryTypeCount) throw std::runtime_error("Vulkan fixture memory type unavailable");
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = state.image;
    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext = &dedicated;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &export_info;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    Check(vkAllocateMemory(state.device, &allocation, nullptr, &state.memory));
    Check(vkBindImageMemory(state.device, state.image, state.memory, 0U));
    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout sublayout{};
    vkGetImageSubresourceLayout(state.device, state.image, &subresource, &sublayout);
    state.layout = {.device_incarnation = 7U + static_cast<std::uint64_t>(ordinal),
                    .device = ordinal,
                    .width = width,
                    .height = height,
                    .pitch_bytes = sublayout.rowPitch,
                    .offset_bytes = sublayout.offset,
                    .required_allocation_bytes = requirements.size,
                    .alignment_bytes = requirements.alignment,
                    .dedicated = true};
    std::memcpy(state.layout.device_uuid.data(), uuid.bytes, VK_UUID_SIZE);
    if (!state.layout.valid()) throw std::runtime_error("Vulkan fixture pitched layout unavailable");
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;
    Check(vkCreateCommandPool(state.device, &pool_info, nullptr, &state.pool));
    VkCommandBufferAllocateInfo commands_info{};
    commands_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands_info.commandPool = state.pool;
    commands_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands_info.commandBufferCount = 1U;
    VkCommandBuffer command;
    Check(vkAllocateCommandBuffers(state.device, &commands_info, &command));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Check(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = state.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
    barrier.srcAccessMask = barrier.dstAccessMask;
    barrier.dstAccessMask = 0U;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
    Check(vkEndCommandBuffer(command));
    VkQueue queue;
    vkGetDeviceQueue(state.device, family, 0U, &queue);
    VkSubmitInfo submission{};
    submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submission.commandBufferCount = 1U;
    submission.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue, 1U, &submission, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(queue));
}
VulkanWorkspaceFixture::~VulkanWorkspaceFixture() = default;
const ImageWorkspaceLayout& VulkanWorkspaceFixture::layout() const noexcept { return state_->layout; }
mmltk::common::io::ScopedFd VulkanWorkspaceFixture::Export() const {
    const auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(state_->device, "vkGetMemoryFdKHR"));
    if (!get_fd) throw std::runtime_error("Vulkan fixture memory export unavailable");
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = state_->memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    Check(get_fd(state_->device, &info, &fd));
    return mmltk::common::io::ScopedFd(fd);
}
}  // namespace mmltk::frameworks::gpu::test_support
