// SacredFire Android: see bbport_null_resources.h.
#include "bbport_null_resources.h"

#include <cstring>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace BbNullResources {
namespace {
bool g_needed = false;
vk::Buffer g_buffer;
vk::Image g_image_2d, g_image_3d;
vk::ImageView g_view_2d, g_view_2d_array, g_view_3d;

u32 FindMemoryType(const Vulkan::Instance& instance, u32 bits, vk::MemoryPropertyFlags want) {
    const auto props = instance.GetPhysicalDevice().getMemoryProperties();
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return ~0u;
}

vk::DeviceMemory AllocateZeroed(const Vulkan::Instance& instance, const vk::MemoryRequirements& req) {
    const vk::Device device = instance.GetDevice();
    u32 type = FindMemoryType(instance, req.memoryTypeBits,
                              vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    const bool host = type != ~0u;
    if (!host) {
        type = FindMemoryType(instance, req.memoryTypeBits, {});
    }
    ASSERT_MSG(type != ~0u, "No memory type for the null resources");
    const auto [result, memory] = device.allocateMemory({.allocationSize = req.size, .memoryTypeIndex = type});
    ASSERT_MSG(result == vk::Result::eSuccess, "Null resources: allocateMemory failed");
    if (host) {
        const auto [map_result, data] = device.mapMemory(memory, 0, req.size);
        if (map_result == vk::Result::eSuccess) {
            std::memset(data, 0, req.size);
            device.unmapMemory(memory);
        }
    }
    return memory;
}

vk::Image MakeImage(const Vulkan::Instance& instance, vk::ImageType type, u32 layers) {
    const vk::Device device = instance.GetDevice();
    const vk::ImageCreateInfo info{
        .imageType = type,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    const auto [result, image] = device.createImage(info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Null resources: createImage failed");
    const auto memory = AllocateZeroed(instance, device.getImageMemoryRequirements(image));
    (void)device.bindImageMemory(image, memory, 0);
    return image;
}

vk::ImageView MakeView(const Vulkan::Instance& instance, vk::Image image, vk::ImageViewType type, u32 layers) {
    const auto [result, view] = instance.GetDevice().createImageView({
        .image = image,
        .viewType = type,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
    });
    ASSERT_MSG(result == vk::Result::eSuccess, "Null resources: createImageView failed");
    return view;
}
} // namespace

void Init(const Vulkan::Instance& instance) {
    if (instance.IsNullDescriptorSupported() || g_buffer) {
        return;
    }
    g_needed = true;
    const vk::Device device = instance.GetDevice();
    const auto [result, buffer] = device.createBuffer({
        .size = 64 * 1024,
        .usage = vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                 vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer |
                 vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
    });
    ASSERT_MSG(result == vk::Result::eSuccess, "Null resources: createBuffer failed");
    const auto memory = AllocateZeroed(instance, device.getBufferMemoryRequirements(buffer));
    (void)device.bindBufferMemory(buffer, memory, 0);
    g_buffer = buffer;
    g_image_2d = MakeImage(instance, vk::ImageType::e2D, 6);
    g_image_3d = MakeImage(instance, vk::ImageType::e3D, 1);
    g_view_2d = MakeView(instance, g_image_2d, vk::ImageViewType::e2D, 1);
    g_view_2d_array = MakeView(instance, g_image_2d, vk::ImageViewType::e2DArray, 6);
    g_view_3d = MakeView(instance, g_image_3d, vk::ImageViewType::e3D, 1);
    LOG_WARNING(Render_Vulkan, "No nullDescriptor (VK_EXT_robustness2): unbound resources use stand-ins");
}

bool Needed() {
    return g_needed;
}

vk::Buffer Buffer() {
    return g_buffer;
}

vk::ImageView ImageView(vk::ImageViewType type) {
    switch (type) {
    case vk::ImageViewType::e3D:
        return g_view_3d;
    case vk::ImageViewType::e2DArray:
    case vk::ImageViewType::e1DArray:
    case vk::ImageViewType::eCubeArray:
    case vk::ImageViewType::eCube:
        return g_view_2d_array;
    default:
        return g_view_2d;
    }
}
} // namespace BbNullResources
