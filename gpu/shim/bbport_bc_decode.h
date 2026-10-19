// SacredFire Android: BC1-BC7 textures on GPUs without them (Mali). Such images are created in a
// plain format (GetSupportedFormat) and their uploads go through a compute shader that decodes
// the blocks (host_shaders/bc_decode.comp) into a scratch buffer, copied into the image.
#pragma once
#include <span>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace BbBcDecode {
/// The plain format a BC format is decoded into (the format itself when it is not BC).
vk::Format Substitute(vk::Format format);
/// The shader's format id when `guest` (BC) is stored as `host` (decoded), else 0.
u32 FormatId(vk::Format guest, vk::Format host);
/// Records the decoding of `copies` (regions of BC blocks in `src`) and returns the buffer
/// holding the texels; `out` gets the same regions in that buffer, for vkCmdCopyBufferToImage.
vk::Buffer Record(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, vk::Buffer src,
                  u32 format_id, std::span<const vk::BufferImageCopy> copies,
                  std::vector<vk::BufferImageCopy>& out);
} // namespace BbBcDecode
