// SacredFire Android: stand-ins for null descriptors on devices without VK_EXT_robustness2's
// nullDescriptor (Mali). Unbound buffers and images are bound to small zeroed resources instead
// of VK_NULL_HANDLE.
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbNullResources {
// Creates the resources when the device lacks null descriptors (no-op otherwise).
void Init(const Vulkan::Instance& instance);
// True when null handles must be replaced (the device lacks nullDescriptor).
bool Needed();
// A zeroed 64 KiB buffer (uniform, storage, vertex, index, texel usages).
vk::Buffer Buffer();
// A 1x1 RGBA8 view (contents undefined) of the given type (e2D, e2DArray, e3D).
vk::ImageView ImageView(vk::ImageViewType type);
// The buffer when `buffer` is null and null descriptors are unsupported, else `buffer`.
inline vk::Buffer OrNull(vk::Buffer buffer) {
    return (!buffer && Needed()) ? Buffer() : buffer;
}
} // namespace BbNullResources
