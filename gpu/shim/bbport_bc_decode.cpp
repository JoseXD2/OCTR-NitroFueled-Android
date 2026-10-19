// SacredFire Android: see bbport_bc_decode.h.
#include "bbport_bc_decode.h"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/bc_decode_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace BbBcDecode {
namespace {
// Shader format ids (bc_decode.comp).
enum : u32 { BC1 = 1, BC2 = 2, BC3 = 3, BC4U = 4, BC4S = 5, BC5U = 6, BC5S = 7, BC6U = 8, BC6S = 9,
             BC7 = 10, BC1_OPAQUE = 11 };

u32 Id(vk::Format f) {
    switch (f) {
    case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbaSrgbBlock: return BC1;
    case vk::Format::eBc1RgbUnormBlock: case vk::Format::eBc1RgbSrgbBlock: return BC1_OPAQUE;
    case vk::Format::eBc2UnormBlock: case vk::Format::eBc2SrgbBlock: return BC2;
    case vk::Format::eBc3UnormBlock: case vk::Format::eBc3SrgbBlock: return BC3;
    case vk::Format::eBc4UnormBlock: return BC4U;
    case vk::Format::eBc4SnormBlock: return BC4S;
    case vk::Format::eBc5UnormBlock: return BC5U;
    case vk::Format::eBc5SnormBlock: return BC5S;
    case vk::Format::eBc6HUfloatBlock: return BC6U;
    case vk::Format::eBc6HSfloatBlock: return BC6S;
    case vk::Format::eBc7UnormBlock: case vk::Format::eBc7SrgbBlock: return BC7;
    default: return 0;
    }
}
u32 BlockBytes(u32 id) {
    return (id == BC1 || id == BC1_OPAQUE || id == BC4U || id == BC4S) ? 8 : 16;
}
u32 TexelBytes(u32 id) {
    switch (id) {
    case BC4U: case BC4S: return 1;
    case BC5U: case BC5S: return 2;
    case BC6U: case BC6S: return 8;
    default: return 4;
    }
}

struct Decoder {
    vk::Device device;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    u64 storage_align = 16;
    u64 max_range = 0;
    std::vector<std::unique_ptr<VideoCore::Buffer>> rings; ///< the last one is current
    u64 ring_head = 0;
};
std::mutex g_mutex;
std::unique_ptr<Decoder> g_decoder;

Decoder& Get(const Vulkan::Instance& instance) {
    if (g_decoder) {
        return *g_decoder;
    }
    auto d = std::make_unique<Decoder>();
    d->device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    d->set_layout = Vulkan::Check(d->device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 32};
    const vk::DescriptorSetLayout set_layout = *d->set_layout;
    d->layout = Vulkan::Check(d->device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = Vulkan::CompileSPV(BC_DECODE_COMP, d->device);
    d->pipeline = Vulkan::Check(d->device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                .layout = *d->layout,
            }));
    d->device.destroyShaderModule(module);
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    d->storage_align = std::max<u64>(limits.minStorageBufferOffsetAlignment, 4);
    d->max_range = limits.maxStorageBufferRange;
    LOG_WARNING(Render_Vulkan, "No BC textures on this device: BC1-BC7 are decoded in a compute shader");
    g_decoder = std::move(d);
    return *g_decoder;
}

/// `bytes` of the scratch ring, in GPU order after everything recorded before (the barrier
/// before each dispatch orders its writes after earlier copies out of the ring).
std::pair<vk::Buffer, u64> Allocate(const Vulkan::Instance& instance, Decoder& d, u64 bytes) {
    bytes = (bytes + 255) & ~u64{255};
    const u64 capacity = d.rings.empty() ? 0 : d.rings.back()->size_bytes;
    if (bytes > capacity) {
        const u64 size = std::max<u64>(std::bit_ceil(bytes), u64{64} << 20);
        d.rings.push_back(std::make_unique<VideoCore::Buffer>(instance, 0, size,
                                                              VideoCore::MemoryType::DeviceLocal,
                                                              "BC decode ring"));
        d.ring_head = 0;
    } else if (d.ring_head + bytes > capacity) {
        d.ring_head = 0;
    }
    const u64 offset = d.ring_head;
    d.ring_head += bytes;
    return {d.rings.back()->Handle(), offset};
}
} // namespace

vk::Format Substitute(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc2UnormBlock: case vk::Format::eBc3UnormBlock: case vk::Format::eBc7UnormBlock:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eBc1RgbaSrgbBlock: case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc2SrgbBlock: case vk::Format::eBc3SrgbBlock: case vk::Format::eBc7SrgbBlock:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eBc4UnormBlock: return vk::Format::eR8Unorm;
    case vk::Format::eBc4SnormBlock: return vk::Format::eR8Snorm;
    case vk::Format::eBc5UnormBlock: return vk::Format::eR8G8Unorm;
    case vk::Format::eBc5SnormBlock: return vk::Format::eR8G8Snorm;
    case vk::Format::eBc6HUfloatBlock: case vk::Format::eBc6HSfloatBlock:
        return vk::Format::eR16G16B16A16Sfloat;
    default: return format;
    }
}

u32 FormatId(vk::Format guest, vk::Format host) {
    return guest != host ? Id(guest) : 0;
}

vk::Buffer Record(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, vk::Buffer src,
                  u32 format_id, std::span<const vk::BufferImageCopy> copies,
                  std::vector<vk::BufferImageCopy>& out) {
    std::scoped_lock lock{g_mutex};
    Decoder& d = Get(instance);
    const u32 block_bytes = BlockBytes(format_id), texel_bytes = TexelBytes(format_id);
    struct Job {
        vk::DescriptorBufferInfo src, dst;
        std::array<u32, 8> push;
        std::array<u32, 3> groups;
    };
    std::vector<Job> jobs;
    u64 total = 0;
    for (const auto& c : copies) {
        const u32 bx = (c.imageExtent.width + 3) / 4, by = (c.imageExtent.height + 3) / 4;
        total += u64{bx} * by * 16 * texel_bytes * c.imageExtent.depth * c.imageSubresource.layerCount;
    }
    const auto [ring, ring_base] = Allocate(instance, d, total);
    u64 dst_at = ring_base;
    out.clear();
    for (const auto& c : copies) {
        const u32 w = c.imageExtent.width, h = c.imageExtent.height;
        const u32 bx = (w + 3) / 4, by = (h + 3) / 4;
        const u32 nz = c.imageExtent.depth * c.imageSubresource.layerCount;
        const u32 row_len = c.bufferRowLength ? c.bufferRowLength : w;
        const u32 img_h = c.bufferImageHeight ? c.bufferImageHeight : h;
        const u64 row_bytes = u64{(row_len + 3) / 4} * block_bytes;
        const u64 slice_bytes = row_bytes * ((img_h + 3) / 4);
        const u64 src_bytes = (nz - 1) * slice_bytes + (by - 1) * row_bytes + u64{bx} * block_bytes;
        const u64 src_aligned = c.bufferOffset & ~(d.storage_align - 1);
        const u64 src_range = std::min<u64>(c.bufferOffset - src_aligned + src_bytes, d.max_range);
        const u64 dst_bytes = u64{bx} * by * 16 * texel_bytes * nz;
        Job job{};
        job.src = {src, src_aligned, src_range};
        job.dst = {ring, dst_at, dst_bytes};
        job.push = {u32((c.bufferOffset - src_aligned) / 4), 0, bx, by, nz, u32(row_bytes / 4),
                    u32(slice_bytes / 4), format_id};
        job.groups = {(bx + 7) / 8, (by + 7) / 8, nz};
        jobs.push_back(job);
        vk::BufferImageCopy decoded = c;
        decoded.bufferOffset = dst_at;
        decoded.bufferRowLength = bx * 4;
        decoded.bufferImageHeight = by * 4;
        out.push_back(decoded);
        dst_at += dst_bytes;
    }
    scheduler.Record([jobs = std::move(jobs), pipeline = *d.pipeline,
                      layout = *d.layout](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        for (const auto& job : jobs) {
            const std::array<vk::WriteDescriptorSet, 2> writes{{
                {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &job.src},
                {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &job.dst},
            }};
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(job.push),
                                 job.push.data());
            cmdbuf.dispatch(job.groups[0], job.groups[1], job.groups[2]);
        }
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    return ring;
}
} // namespace BbBcDecode
