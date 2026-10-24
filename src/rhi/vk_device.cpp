// Vulkan 1.3 backend for the Ukiyo RHI.
//   * Dynamic rendering (no VkRenderPass objects): pipelines only need formats.
//   * Descriptor sets are typed by SetType and allocated from one pool.
//   * Memory via Vulkan Memory Allocator (VMA, Debian's libvulkan-memory-allocator-dev).
//   * Images are tracked by layout; every pass transitions its targets and leaves
//     them in a sampled layout, so descriptors can always reference them.
//   * One frame in flight: beginFrame waits on the previous frame's fence.
#include "rhi/internal.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
// The presentation surface comes from the platform: a GLFW window on the desktop, the activity's ANativeWindow on
// Android. Everything else in this file is the same code on both.
#if defined(__ANDROID__)
#include <android/native_window.h>
#define UKIYO_VK_ANDROID 1
#define UKIYO_VK_GLFW 0
#else
#include <GLFW/glfw3.h>
#define UKIYO_VK_ANDROID 0
#define UKIYO_VK_GLFW 1
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef UKIYO_SHADER_DIR
#error "UKIYO_SHADER_DIR must point at the generated shader directory"
#endif

namespace rhi {
namespace {

constexpr uint32_t kPushConstantBytes = 128;
constexpr uint32_t kVertexStride = 96;

[[noreturn]] void vkFail(const char* what, VkResult r) {
    throw std::runtime_error(std::string(what) + " failed with VkResult " + std::to_string(static_cast<int>(r)));
}
#define VK_CHECK(call, what) do { VkResult _r = (call); if (_r != VK_SUCCESS) vkFail(what, _r); } while (0)

// State shared by every object that needs the device.
struct VKCore {
    bool bcSupported = true;   // textureCompressionBC from the physical device (batch 7)
    bool astcSupported = false;   // textureCompressionASTC_LDR from the physical device (batch 7)
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    VmaAllocator vma = nullptr;
    VkCommandPool transientPool = VK_NULL_HANDLE;
    std::string deviceName;
};

// Runs fn in a one-shot command buffer and waits for it.
void immediate(VKCore* core, const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = core->transientPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(core->device, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    fn(cb);
    VK_CHECK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(core->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    vkQueueWaitIdle(core->queue);
    vkFreeCommandBuffers(core->device, core->transientPool, 1, &cb);
}

// Conservative full barrier between layout changes (correct for every usage).
void barrierLayout(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                   uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mipCount, 0, VK_REMAINING_ARRAY_LAYERS};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}

// Bytes in one 4x4 block of a compressed format (0 = not compressed); the upload maths keeps this beside the format
// mapping so the sizes cannot drift.
int vkBlockBytes(Format f) {
    switch (f) {
    case Format::BC1:
    case Format::BC1_SRGB: return 8;
    case Format::BC3:
    case Format::BC3_SRGB:
    case Format::BC5:
    case Format::ASTC_4x4:
    case Format::ASTC_4x4_SRGB: return 16;
    default: return 0;
    }
}

VkFormat toVkFormat(Format f, VkFormat backbuffer) {
    switch (f) {
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::D32F: return VK_FORMAT_D32_SFLOAT;
    case Format::BC1: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case Format::BC1_SRGB: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC3_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case Format::ASTC_4x4_SRGB: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case Format::Backbuffer: return backbuffer;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

class VKTexture;

class VKBuffer final : public Buffer {
public:
    VKBuffer(VKCore* core, BufferUsage usage, size_t size, bool readback = false) : core_(core), size_(size) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.usage |= usage == BufferUsage::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                  : usage == BufferUsage::Index  ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                  : usage == BufferUsage::Storage ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                  : usage == BufferUsage::Indirect
                        ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                                 : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(core_->vma, &bi, &ai, &buffer_, &alloc_, &info), "vmaCreateBuffer");
        mapped_ = static_cast<uint8_t*>(info.pMappedData);
    }
    ~VKBuffer() override {
        if (buffer_) vmaDestroyBuffer(core_->vma, buffer_, alloc_);
    }
    void upload(const void* data, size_t size, size_t offset) override {
        if (offset + size > size_) throw std::runtime_error("buffer upload out of range");
        std::memcpy(mapped_ + offset, data, size);
    }
    size_t size() const override { return size_; }
    VkBuffer handle() const { return buffer_; }
    void* mapped() const { return mapped_; }

private:
    VKCore* core_;
    size_t size_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = nullptr;
    uint8_t* mapped_ = nullptr;
};

class VKTexture final : public Texture {
public:
    // Owned image created from a description (optionally with initial RGBA8 data).
    VKTexture(VKCore* core, const TextureDesc& d, VkFormat vkFmt, bool depth, const void* rgba, const MipChain* chain)
        : core_(core), desc_(d), vkFormat_(vkFmt), depth_(depth) {
        aspect_ = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        levels_ = d.mipmaps ? static_cast<uint32_t>(std::floor(std::log2(std::max(d.width, d.height)))) + 1 : 1;
        // Block-compressed formats (batch 7): 4x4 blocks; the upload is a chain of block bytes, the mip count is the
        // same formula as always, and the image is sample-only (no attachment usage). A device without the BC
        // feature refuses here rather than sampling garbage.
        const int blockB = vkBlockBytes(d.format);
        if (blockB > 0) {
            const bool astc = d.format == Format::ASTC_4x4 || d.format == Format::ASTC_4x4_SRGB;
            const bool ok = astc ? core_->astcSupported : core_->bcSupported;
            if (!ok)
                throw std::runtime_error("device lacks the compression feature for this format; "
                                         "the loader should have asked supportsBlockCompression/supportsAstc");
        }

        const uint32_t layerCount = d.faces == 6 ? 6u : 1u;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.flags = d.faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = vkFmt;
        ii.extent = {d.width, d.height, 1};
        ii.mipLevels = levels_;
        ii.arrayLayers = layerCount;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // Compressed images cannot be attachment targets; only uncompressed colour gets the attachment usage.
        ii.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                          : (blockB > 0 ? VkImageUsageFlags(0) : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_CHECK(vmaCreateImage(core_->vma, &ii, &ai, &image_, &alloc_, nullptr), "vmaCreateImage");

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image_;
        vi.viewType = d.faces == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vkFmt;
        vi.subresourceRange = {aspect_, 0, levels_, 0, layerCount};
        VK_CHECK(vkCreateImageView(core_->device, &vi, nullptr, &view_), "vkCreateImageView");

        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        VkFilter f = d.filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.magFilter = f;
        si.minFilter = f;
        si.mipmapMode = (levels_ > 1 && d.filter == Filter::Linear) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        VkSamplerAddressMode am = d.wrap == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeU = si.addressModeV = si.addressModeW = am;
        si.minLod = 0.0f;
        si.maxLod = static_cast<float>(levels_ - 1);   // last valid mip level
        VK_CHECK(vkCreateSampler(core_->device, &si, nullptr, &sampler_), "vkCreateSampler");

        // Initial contents: optional RGBA8 upload (+ mip chain), then a sampled layout.
        if (chain && !chain->empty()) {
            uploadChain(*chain);
        } else if (rgba) {
            upload(rgba);
        } else {
            immediate(core_, [&](VkCommandBuffer cb) {
                barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_UNDEFINED, sampledLayout());
            });
            layout_ = sampledLayout();
        }
    }

    // Wraps a swapchain image (not owned).
    VKTexture(VKCore* core, VkImage image, VkImageView view, uint32_t w, uint32_t h, VkFormat vkFmt)
        : core_(core), image_(image), view_(view), vkFormat_(vkFmt), external_(true) {
        desc_.width = w;
        desc_.height = h;
        desc_.format = Format::Backbuffer;
        aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    ~VKTexture() override {
        if (sampler_) vkDestroySampler(core_->device, sampler_, nullptr);
        if (view_) vkDestroyImageView(core_->device, view_, nullptr);
        if (!external_ && image_) vmaDestroyImage(core_->vma, image_, alloc_);
    }

    uint32_t width() const override { return desc_.width; }
    uint32_t height() const override { return desc_.height; }
    Format format() const override { return desc_.format; }

    // Partial upload: sampled -> transfer dst -> one copy -> sampled. The staging buffer is freed by immediate()'s
    // wait, so the caller sees a finished texture on return.
    void update(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* rgba8) override {
        if (depth_ || external_) throw std::runtime_error("texture update needs an owned colour texture");
        if (vkBlockBytes(desc_.format) > 0) throw std::runtime_error("compressed textures do not take partial updates");
        if (rgba8 == nullptr || w == 0 || h == 0) return;
        if (x + w > desc_.width || y + h > desc_.height) throw std::runtime_error("texture update out of range");
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation salloc = nullptr;
        VmaAllocationInfo sinfo{};
        VK_CHECK(vmaCreateBuffer(core_->vma, &bi, &ai, &staging, &salloc, &sinfo), "staging buffer (texture update)");
        std::memcpy(sinfo.pMappedData, rgba8, static_cast<size_t>(bytes));
        immediate(core_, [&](VkCommandBuffer cb) {
            barrierLayout(cb, image_, aspect_, sampledLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
            region.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sampledLayout());
        });
        layout_ = sampledLayout();
        vmaDestroyBuffer(core_->vma, staging, salloc);
    }

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    VkImageAspectFlags aspect() const { return aspect_; }
    VkImageLayout layout() const { return layout_; }
    void setLayout(VkImageLayout l) { layout_ = l; }
    VkImageLayout sampledLayout() const {
        return depth_ ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    bool isDepth() const { return depth_; }

private:
    // Uploads a CPU-built chain, one buffer-to-image copy per level. Same staging buffer, sized for the whole chain.
    void uploadChain(const MipChain& chain) {
        size_t total = 0;
        for (const auto& level : chain) total += level.size();
        if (total == 0) return;

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = static_cast<VkDeviceSize>(total);
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation salloc = nullptr;
        VmaAllocationInfo sinfo{};
        VK_CHECK(vmaCreateBuffer(core_->vma, &bi, &ai, &staging, &salloc, &sinfo), "staging buffer");
        uint8_t* dst = static_cast<uint8_t*>(sinfo.pMappedData);
        for (const auto& level : chain) {
            std::memcpy(dst, level.data(), level.size());
            dst += level.size();
        }

        immediate(core_, [&](VkCommandBuffer cb) {
            barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkDeviceSize offset = 0;
            const uint32_t layerCount = desc_.faces == 6 ? 6u : 1u;
            const uint32_t n = std::min(levels_, static_cast<uint32_t>(chain.size()) / layerCount);
            const int blockB = vkBlockBytes(desc_.format);
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t lw = std::max(1u, desc_.width >> i);
                    const uint32_t lh = std::max(1u, desc_.height >> i);
                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, layer, 1};
                    // The extent is the level's virtual size; a whole-mip copy from the origin is valid at any size,
                    // edge blocks included. Tightly packed (bufferRowLength 0) works for compressed formats too.
                    region.imageExtent = {lw, lh, 1};
                    vkCmdCopyBufferToImage(cb, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    offset += blockB > 0 ? static_cast<VkDeviceSize>(((lw + 3) / 4) * ((lh + 3) / 4)) * blockB
                                         : static_cast<VkDeviceSize>(lw) * lh * 4u;
                }
            }
            barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sampledLayout(), 0, levels_);
        });
        layout_ = sampledLayout();
        vmaDestroyBuffer(core_->vma, staging, salloc);
    }

    void upload(const void* rgba) {
        if (vkBlockBytes(desc_.format) > 0)
            throw std::runtime_error("a compressed texture is created from a chain of block bytes, not from pixels");
        const uint32_t w = desc_.width, h = desc_.height;
        const uint32_t layerCount = desc_.faces == 6 ? 6u : 1u;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4 * layerCount;
        // Staging buffer.
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation salloc = nullptr;
        VmaAllocationInfo sinfo{};
        VK_CHECK(vmaCreateBuffer(core_->vma, &bi, &ai, &staging, &salloc, &sinfo), "staging buffer");
        std::memcpy(sinfo.pMappedData, rgba, static_cast<size_t>(w) * h * 4);
        // A cubemap without a CPU chain (a 1x1 placeholder) gets the same face six times.
        for (uint32_t f = 1; f < layerCount; ++f)
            std::memcpy(static_cast<uint8_t*>(sinfo.pMappedData) + static_cast<size_t>(f) * w * h * 4, rgba,
                        static_cast<size_t>(w) * h * 4);

        immediate(core_, [&](VkCommandBuffer cb) {
            barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            // The staging buffer is face-major, so one region covers every layer.
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount};
            region.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Mip chain by repeated linear blits, per face.
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                int32_t mw = static_cast<int32_t>(w), mh = static_cast<int32_t>(h);
                for (uint32_t i = 1; i < levels_; ++i) {
                    barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i - 1, 1);
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, layer, 1};
                    blit.srcOffsets[0] = {0, 0, 0};
                    blit.srcOffsets[1] = {mw, mh, 1};
                    int32_t nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, layer, 1};
                    blit.dstOffsets[0] = {0, 0, 0};
                    blit.dstOffsets[1] = {nw, nh, 1};
                    vkCmdBlitImage(cb, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                    barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  sampledLayout(), i - 1, 1);
                    mw = nw;
                    mh = nh;
                }
            }
            barrierLayout(cb, image_, aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sampledLayout(), levels_ - 1, 1);
        });
        layout_ = sampledLayout();
        vmaDestroyBuffer(core_->vma, staging, salloc);
    }

    VKCore* core_;
    TextureDesc desc_;
    VkFormat vkFormat_;
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
    bool depth_ = false;
    bool external_ = false;
    uint32_t levels_ = 1;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

class VKPipeline final : public Pipeline {
public:
    VKCore* core = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    ~VKPipeline() override {
        if (pipeline) vkDestroyPipeline(core->device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(core->device, layout, nullptr);
    }
};

class VKDescriptorSet final : public DescriptorSet {
public:
    VKDescriptorSet(VKCore* core, VkDescriptorPool pool, VkDescriptorSetLayout layout, SetType type)
        : core_(core), pool_(pool), type_(type) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VK_CHECK(vkAllocateDescriptorSets(core_->device, &ai, &set_), "vkAllocateDescriptorSets");
    }
    ~VKDescriptorSet() override {
        if (set_) vkFreeDescriptorSets(core_->device, pool_, 1, &set_);
    }
    void setBuffer(uint32_t binding, Buffer* buffer) override {
        auto* b = static_cast<VKBuffer*>(buffer);
        VkDescriptorBufferInfo bi{b->handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        // The Compute set's bindings are storage buffers (layout 8); every other set's are uniform blocks. The
        // layout and the write have to agree or vkUpdateDescriptorSets validates the mismatch.
        w.descriptorType = type_ == SetType::Compute ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                      : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(core_->device, 1, &w, 0, nullptr);
    }
    void setTexture(uint32_t binding, Texture* texture) override {
        auto* t = static_cast<VKTexture*>(texture);
        VkDescriptorImageInfo ii{t->sampler(), t->view(), t->sampledLayout()};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(core_->device, 1, &w, 0, nullptr);
    }
    VkDescriptorSet handle() const { return set_; }

private:
    VKCore* core_;
    VkDescriptorPool pool_;
    SetType type_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

class VKDevice final : public Device {
public:
    VKDevice(const DeviceConfig& cfg, const NativeWindow& window)
        : headless_(cfg.headless || !window.valid()), width_(cfg.width), height_(cfg.height) {
        createInstance(cfg, window);
        if (window.valid() && !headless_) {
#if UKIYO_VK_ANDROID
            VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            ci.window = static_cast<ANativeWindow*>(window.android);
            VK_CHECK(vkCreateAndroidSurfaceKHR(core_.instance, &ci, nullptr, &core_.surface),
                     "vkCreateAndroidSurfaceKHR");
#else
            VK_CHECK(glfwCreateWindowSurface(core_.instance, window.glfw, nullptr, &core_.surface),
                     "glfwCreateWindowSurface");
#endif
        }
        pickDevice();
        createDevice(cfg);
        createVma();

        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = core_.qfamily;
        VK_CHECK(vkCreateCommandPool(core_.device, &pi, nullptr, &framePool_), "vkCreateCommandPool");
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_CHECK(vkCreateCommandPool(core_.device, &pi, nullptr, &core_.transientPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = framePool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(core_.device, &ai, &cb_), "vkAllocateCommandBuffers");

        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
        };
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = 1024;
        dp.poolSizeCount = 3;
        dp.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(core_.device, &dp, nullptr, &descPool_), "vkCreateDescriptorPool");
        createSetLayouts();

        // The profiler's GPU brackets: two timestamp queries per slot (docs/PROFILER.md). timestampPeriod is the
        // queue clock's nanoseconds per tick; a device that reports 0 has no clock, and the API then reports no
        // timings rather than zeros pretending to be measurements.
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(core_.phys, &props);
            if (props.limits.timestampPeriod > 0.0f) {
                VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qp.queryCount = kGpuTimeSlots * 2;
                if (vkCreateQueryPool(core_.device, &qp, nullptr, &timePool_) == VK_SUCCESS)
                    timePeriod_ = props.limits.timestampPeriod;
                else
                    timePool_ = VK_NULL_HANDLE;
                if (timePool_) gpuTimes_.assign(kGpuTimeSlots, 0.0f);
            }
        }

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(core_.device, &si, nullptr, &imageAvailable_), "semaphore");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(core_.device, &fi, nullptr, &inFlight_), "fence");

        if (!headless_) {
            createSwapchain();
        } else {
            backbufferFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
            TextureDesc d;
            d.width = cfg.width;
            d.height = cfg.height;
            d.format = Format::RGBA8;
            d.filter = Filter::Nearest;
            offscreen_ = std::make_unique<VKTexture>(&core_, d, VK_FORMAT_R8G8B8A8_UNORM, false, nullptr, nullptr);
        }
    }

    ~VKDevice() override {
        vkDeviceWaitIdle(core_.device);
        offscreen_.reset();
        readback_.reset();
        destroySwapchain();
        if (swapchain_) vkDestroySwapchainKHR(core_.device, swapchain_, nullptr);
        for (auto& l : setLayouts_)
            if (l) vkDestroyDescriptorSetLayout(core_.device, l, nullptr);
        for (auto s : renderFinished_) vkDestroySemaphore(core_.device, s, nullptr);
        vkDestroySemaphore(core_.device, imageAvailable_, nullptr);
        vkDestroyFence(core_.device, inFlight_, nullptr);
        if (timePool_) vkDestroyQueryPool(core_.device, timePool_, nullptr);
        vkDestroyDescriptorPool(core_.device, descPool_, nullptr);
        vkDestroyCommandPool(core_.device, framePool_, nullptr);
        vkDestroyCommandPool(core_.device, core_.transientPool, nullptr);
        if (core_.vma) vmaDestroyAllocator(core_.vma);
        vkDestroyDevice(core_.device, nullptr);
        if (core_.surface) vkDestroySurfaceKHR(core_.instance, core_.surface, nullptr);
        vkDestroyInstance(core_.instance, nullptr);
    }

    Backend backend() const override { return Backend::Vulkan; }
    std::string deviceName() const override { return "Vulkan " + core_.deviceName; }
    bool headless() const override { return headless_; }
    uint32_t backbufferWidth() const override { return width_; }
    uint32_t backbufferHeight() const override { return height_; }
    Format backbufferFormat() const override { return Format::Backbuffer; }
    bool presentFlipsY() const override { return !headless_; }

    bool supportsBlockCompression() const override { return core_.bcSupported; }
    bool supportsAstc() const override { return core_.astcSupported; }

    std::unique_ptr<Buffer> createBuffer(BufferUsage usage, size_t size) override {
        return std::make_unique<VKBuffer>(&core_, usage, std::max<size_t>(size, 16));
    }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc, const void* rgba8,
                                           const MipChain* mipChain) override {
        bool depth = desc.format == Format::D32F;
        return std::make_unique<VKTexture>(&core_, desc, toVkFormat(desc.format, backbufferFormat_), depth, rgba8,
                                           mipChain);
    }

    std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) override {
        auto p = std::make_unique<VKPipeline>();
        p->core = &core_;

        // Pipeline layout: one set layout per entry in desc.sets, plus push constants. A compute pipeline declares
        // its push range for the compute stage only — pushConstants() picks the same stage at record time.
        std::vector<VkDescriptorSetLayout> layouts;
        for (SetType t : desc.sets) layouts.push_back(setLayouts_[static_cast<size_t>(t)]);
        const VkShaderStageFlags pushStages = desc.computeShader.empty()
                                                  ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                                  : VK_SHADER_STAGE_COMPUTE_BIT;
        VkPushConstantRange pcr{pushStages, 0, kPushConstantBytes};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pl.pSetLayouts = layouts.data();
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(core_.device, &pl, nullptr, &p->layout), "vkCreatePipelineLayout");

        if (!desc.computeShader.empty()) {
            VkShaderModule cs = loadModule(shaderRoot() + "/spv/" + desc.computeShader + ".comp.spv");
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.layout = p->layout;
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                        cs, "main", nullptr};
            VK_CHECK(vkCreateComputePipelines(core_.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipeline),
                     "vkCreateComputePipelines");
            vkDestroyShaderModule(core_.device, cs, nullptr);
            p->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
            return p;
        }

        VkShaderModule vs = loadModule(shaderRoot() + "/spv/" + desc.vertexShader + ".vert.spv");
        VkShaderModule fs = VK_NULL_HANDLE;
        if (desc.fragmentSpirv.empty()) {
            fs = loadModule(shaderRoot() + "/spv/" + desc.fragmentShader + ".frag.spv");
        } else {
            VkShaderModuleCreateInfo fci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            fci.codeSize = desc.fragmentSpirv.size() * sizeof(uint32_t);
            fci.pCode = desc.fragmentSpirv.data();
            VK_CHECK(vkCreateShaderModule(core_.device, &fci, nullptr, &fs), "vkCreateShaderModule (generated fragment)");
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

        VkVertexInputBindingDescription binding{0, kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[8] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
            {3, 0, VK_FORMAT_R16G16B16A16_UINT, 32},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 40},
            {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56},   // colour
            {6, 0, VK_FORMAT_R32G32_SFLOAT, 72},         // uv1
            {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80},   // smooth normal (outline)
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (desc.vertexInput) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = 8;
            vi.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = desc.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.cullMode = desc.cull == Cull::None ? VK_CULL_MODE_NONE : desc.cull == Cull::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
        // GL-style projection with a positive viewport: Vulkan framebuffer space has
        // flipped winding relative to GL, so GL-counter-clockwise faces are clockwise here.
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        std::vector<VkPipelineColorBlendAttachmentState> blends;
        std::vector<VkFormat> colorFormats;
        for (Format f : desc.colorFormats) {
            VkPipelineColorBlendAttachmentState b{};
            b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            if (desc.blend == Blend::Alpha) {
                b.blendEnable = VK_TRUE;
                b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.colorBlendOp = VK_BLEND_OP_ADD;
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                b.alphaBlendOp = VK_BLEND_OP_ADD;
            } else if (desc.blend == Blend::Additive) {
                b.blendEnable = VK_TRUE;
                b.srcColorBlendFactor = b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.dstColorBlendFactor = b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            blends.push_back(b);
            colorFormats.push_back(toVkFormat(f, backbufferFormat_));
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = static_cast<uint32_t>(blends.size());
        cb.pAttachments = blends.data();

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2;
        dsi.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rend.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
        rend.pColorAttachmentFormats = colorFormats.data();
        rend.depthAttachmentFormat = desc.hasDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &rend;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsi;
        gp.layout = p->layout;
        gp.renderPass = VK_NULL_HANDLE;
        VkResult r = vkCreateGraphicsPipelines(core_.device, VK_NULL_HANDLE, 1, &gp, nullptr, &p->pipeline);
        vkDestroyShaderModule(core_.device, vs, nullptr);
        vkDestroyShaderModule(core_.device, fs, nullptr);
        if (r != VK_SUCCESS) vkFail(("vkCreateGraphicsPipelines " + desc.vertexShader + "/" + desc.fragmentShader).c_str(), r);
        return p;
    }

    std::unique_ptr<DescriptorSet> createDescriptorSet(SetType type) override {
        return std::make_unique<VKDescriptorSet>(&core_, descPool_, setLayouts_[static_cast<size_t>(type)], type);
    }

    // The drawable's size in pixels. Android reports it from the window, and it changes when the surface does; the
    // desktop asks GLFW. One function so the swapchain code below is platform-neutral.
    void drawableSize(int& w, int& h) const {
#if UKIYO_VK_ANDROID
        auto* win = static_cast<ANativeWindow*>(androidWindow_);
        w = win ? ANativeWindow_getWidth(win) : 0;
        h = win ? ANativeWindow_getHeight(win) : 0;
#else
        glfwGetFramebufferSize(window_, &w, &h);
#endif
    }

    bool beginFrame() override {
        if (!headless_) {
            int fw = 0, fh = 0;
            drawableSize(fw, fh);
            if (fw == 0 || fh == 0) return false;
            if (needRecreate_ || static_cast<uint32_t>(fw) != swapExtent_.width || static_cast<uint32_t>(fh) != swapExtent_.height) {
                vkDeviceWaitIdle(core_.device);
                createSwapchain();
            }
        }
        VK_CHECK(vkWaitForFences(core_.device, 1, &inFlight_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        if (!headless_) {
            VkResult r = vkAcquireNextImageKHR(core_.device, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex_);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) { needRecreate_ = true; return false; }
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vkFail("vkAcquireNextImageKHR", r);
            if (r == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
        }
        VK_CHECK(vkResetFences(core_.device, 1, &inFlight_), "vkResetFences");
        VK_CHECK(vkResetCommandBuffer(cb_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb_, &bi), "vkBeginCommandBuffer");
        // The fence above retired the previous frame, so its timestamps read without waiting; the reset is recorded
        // for this frame's submission and lands after the read. (One frame in flight, per the note at the top of
        // this file — that is what makes resolve-then-reset in the same place correct.)
        resolveGpuTimes();
        if (timePool_) vkCmdResetQueryPool(cb_, timePool_, 0, kGpuTimeSlots * 2);
        inFrame_ = true;
        return true;
    }

    // GPU timestamps, written outside every render pass (docs/PROFILER.md). The pair resolves a frame later, when
    // the fence has retired the frame that wrote it, so the profiler never waits; a slot this frame did not write
    // stays NOT_READY at resolve time and keeps its previous value.
    void beginGpuTime(uint32_t slot) override {
        if (!timePool_ || !inFrame_ || slot >= kGpuTimeSlots) return;
        vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timePool_, slot * 2);
    }
    void endGpuTime(uint32_t slot) override {
        if (!timePool_ || !inFrame_ || slot >= kGpuTimeSlots) return;
        vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timePool_, slot * 2 + 1);
    }
    std::vector<float> gpuTimesMs() override { return timePool_ ? gpuTimes_ : std::vector<float>{}; }

    void resolveGpuTimes() {
        if (!timePool_) return;
        for (uint32_t s = 0; s < kGpuTimeSlots; ++s) {
            uint64_t t[2]{0, 0};
            if (vkGetQueryPoolResults(core_.device, timePool_, s * 2, 2, sizeof(t), t, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                continue;
            const double ns = double(t[1] > t[0] ? t[1] - t[0] : 0) * double(timePeriod_);
            gpuTimes_[s] = float(ns / 1.0e6);
        }
    }

    void endFrame(bool capture) override {
        VkTexture_t* bb = backbufferTexture();
        if (capture) {
            ensureReadback();
            barrierTo(bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width_, height_, 1};
            if (!headless_) region.imageExtent = {swapExtent_.width, swapExtent_.height, 1};
            vkCmdCopyImageToBuffer(cb_, bb->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_->handle(), 1, &region);
        }
        if (!headless_) {
            barrierTo(bb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        VK_CHECK(vkEndCommandBuffer(cb_), "vkEndCommandBuffer");
        inFrame_ = false;

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb_;
        if (!headless_) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &imageAvailable_;
            si.pWaitDstStageMask = &waitStage;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &renderFinished_[imageIndex_];
        }
        VK_CHECK(vkQueueSubmit(core_.queue, 1, &si, inFlight_), "vkQueueSubmit");

        if (!headless_) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &renderFinished_[imageIndex_];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &imageIndex_;
            VkResult r = vkQueuePresentKHR(core_.queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
            else if (r != VK_SUCCESS) vkFail("vkQueuePresentKHR", r);
        }

        if (capture) {
            vkQueueWaitIdle(core_.queue);
            const uint8_t* src = static_cast<const uint8_t*>(readback_->mapped());
            capture_.assign(src, src + readbackBytes());
            if (headless_) {
                // Headless images are stored bottom row first (GL convention); flip to top-first.
                const size_t stride = static_cast<size_t>(width_) * 4;
                std::vector<uint8_t> flipped(capture_.size());
                for (uint32_t y = 0; y < height_; ++y)
                    std::memcpy(flipped.data() + y * stride, capture_.data() + (height_ - 1 - y) * stride, stride);
                capture_.swap(flipped);
            }
        }
    }

    std::vector<uint8_t> takeCapture() override {
        std::vector<uint8_t> out;
        out.swap(capture_);
        return out;
    }

    void waitIdle() override { vkDeviceWaitIdle(core_.device); }

    bool resizeBackbuffer(uint32_t w, uint32_t h) override {
        if (!headless_) return false;
        if (w == 0 || h == 0) return false;
        if (w == width_ && h == height_) return true;
        vkDeviceWaitIdle(core_.device);
        offscreen_.reset();
        width_ = w;
        height_ = h;
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.format = Format::RGBA8;
        d.filter = Filter::Nearest;
        offscreen_ = std::make_unique<VKTexture>(&core_, d, VK_FORMAT_R8G8B8A8_UNORM, false, nullptr, nullptr);
        return true;
    }

    void beginPass(const PassDesc& pass) override {
        std::vector<VkRenderingAttachmentInfo> colors;
        uint32_t w = width_, h = height_;
        activeTargets_.clear();
        if (pass.backbuffer) {
            VkTexture_t* bb = backbufferTexture();
            barrierTo(bb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            a.imageView = bb->view();
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.clearValue.color = {{pass.clearValue.r, pass.clearValue.g, pass.clearValue.b, pass.clearValue.a}};
            colors.push_back(a);
            w = bb->width();
            h = bb->height();
            activeTargets_.push_back(bb);
        } else {
            for (Texture* t : pass.colors) {
                auto* vt = static_cast<VKTexture*>(t);
                barrierTo(vt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                a.imageView = vt->view();
                a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                a.loadOp = pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                a.clearValue.color = {{pass.clearValue.r, pass.clearValue.g, pass.clearValue.b, pass.clearValue.a}};
                colors.push_back(a);
                w = vt->width();
                h = vt->height();
                activeTargets_.push_back(vt);
            }
        }
        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (pass.depth) {
            auto* dt = static_cast<VKTexture*>(pass.depth);
            barrierTo(dt, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            depthAtt.imageView = dt->view();
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = pass.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = {1.0f, 0};
            if (pass.colors.empty()) { w = dt->width(); h = dt->height(); }
            activeTargets_.push_back(dt);
        }

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
        ri.pColorAttachments = colors.empty() ? nullptr : colors.data();
        ri.pDepthAttachment = pass.depth ? &depthAtt : nullptr;
        vkCmdBeginRendering(cb_, &ri);
        passW_ = w;
        passH_ = h;
        setViewport(0, 0, static_cast<int>(w), static_cast<int>(h));
    }

    void endPass() override {
        vkCmdEndRendering(cb_);
        // Leave every target in a sampleable layout (the backbuffer is handled in endFrame).
        for (VKTexture* t : activeTargets_) {
            if (t == backbufferTexture()) continue;
            barrierTo(t, t->sampledLayout());
        }
        activeTargets_.clear();
    }

    void setViewport(int x, int y, int w, int h) override {
        VkViewport vp{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sc{{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cb_, 0, 1, &vp);
        vkCmdSetScissor(cb_, 0, 1, &sc);
    }

    void bindPipeline(Pipeline* pipeline) override {
        auto* p = static_cast<VKPipeline*>(pipeline);
        curLayout_ = p->layout;
        curBindPoint_ = p->bindPoint;
        vkCmdBindPipeline(cb_, curBindPoint_, p->pipeline);
    }

    void bindDescriptorSet(uint32_t index, DescriptorSet* set) override {
        VkDescriptorSet s = static_cast<VKDescriptorSet*>(set)->handle();
        vkCmdBindDescriptorSets(cb_, curBindPoint_, curLayout_, index, 1, &s, 0, nullptr);
    }

    void bindVertexBuffer(Buffer* buffer) override {
        VkBuffer b = static_cast<VKBuffer*>(buffer)->handle();
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cb_, 0, 1, &b, &off);
    }

    void bindIndexBuffer(Buffer* buffer) override {
        vkCmdBindIndexBuffer(cb_, static_cast<VKBuffer*>(buffer)->handle(), 0, VK_INDEX_TYPE_UINT32);
    }

    void pushConstants(const void* data, size_t size) override {
        // The stage must be one the bound pipeline layout declared: compute layouts carry COMPUTE, graphics VF.
        const VkShaderStageFlags stages = curBindPoint_ == VK_PIPELINE_BIND_POINT_COMPUTE
                                              ? VK_SHADER_STAGE_COMPUTE_BIT
                                              : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        vkCmdPushConstants(cb_, curLayout_, stages, 0, static_cast<uint32_t>(size), data);
    }

    void drawIndexed(uint32_t indexCount, uint32_t firstIndex) override {
        vkCmdDrawIndexed(cb_, indexCount, 1, firstIndex, 0, 0);
    }

    void drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount) override {
        vkCmdDrawIndexed(cb_, indexCount, instanceCount, 0, 0, 0);
    }

    void dispatch(uint32_t x, uint32_t y, uint32_t z) override {
        // Coarse barriers either side: everything before is visible to the compute stage, and everything the
        // dispatch wrote is visible to indirect commands, index/vertex reads and both shader stages after it.
        VkMemoryBarrier pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        pre.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &pre, 0, nullptr, 0, nullptr);
        vkCmdDispatch(cb_, x, y, z);
        VkMemoryBarrier post{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                            VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &post, 0, nullptr, 0, nullptr);
    }

    void drawIndexedIndirect(Buffer* buffer, size_t offset, uint32_t drawCount) override {
        vkCmdDrawIndexedIndirect(cb_, static_cast<VKBuffer*>(buffer)->handle(), offset, drawCount,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    void drawFullscreen() override {
        vkCmdDraw(cb_, 3, 1, 0, 0);
    }

    void drawInstanced(uint32_t vertexCount, uint32_t instanceCount) override {
        vkCmdDraw(cb_, vertexCount, instanceCount, 0, 0);
    }

private:
    using VkTexture_t = VKTexture;

    VKTexture* backbufferTexture() {
        return headless_ ? offscreen_.get() : swapTextures_[imageIndex_].get();
    }

    void barrierTo(VKTexture* t, VkImageLayout to) {
        if (t->layout() == to) return;
        barrierLayout(cb_, t->image(), t->aspect(), t->layout(), to);
        t->setLayout(to);
    }

    uint32_t readbackBytes() const { return width_ * height_ * 4; }

    void ensureReadback() {
        uint32_t w = headless_ ? width_ : swapExtent_.width;
        uint32_t h = headless_ ? height_ : swapExtent_.height;
        if (!readback_ || readback_->size() < static_cast<size_t>(w) * h * 4 || readbackW_ != w || readbackH_ != h) {
            readback_ = std::make_unique<VKBuffer>(&core_, BufferUsage::Uniform, static_cast<size_t>(w) * h * 4, true);
            readbackW_ = w;
            readbackH_ = h;
        }
    }

    void createInstance(const DeviceConfig& cfg, const NativeWindow& window) {
        window_ = window.glfw;
        androidWindow_ = window.android;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Ukiyo Engine";
        app.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> exts;
        if (window.valid() && !headless_) {
#if UKIYO_VK_ANDROID
            // What the Android loader needs to hand a VkSurfaceKHR back for an ANativeWindow.
            exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
            uint32_t n = 0;
            const char** req = glfwGetRequiredInstanceExtensions(&n);
            exts.assign(req, req + n);
#endif
        }
        std::vector<const char*> layers;
        if (cfg.validation) {
            uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> avail(n);
            vkEnumerateInstanceLayerProperties(&n, avail.data());
            for (auto& l : avail)
                if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
        ci.ppEnabledLayerNames = layers.data();
        VK_CHECK(vkCreateInstance(&ci, nullptr, &core_.instance), "vkCreateInstance");
    }

    void pickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(core_.instance, &n, nullptr);
        if (n == 0) throw std::runtime_error("no Vulkan physical devices found");
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(core_.instance, &n, devs.data());
        for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
            for (uint32_t i = 0; i < qn; ++i) {
                if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                if (core_.surface) {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(d, i, core_.surface, &present);
                    if (!present) continue;
                }
                core_.phys = d;
                core_.qfamily = i;
                core_.deviceName = props.deviceName;
                return;
            }
        }
        throw std::runtime_error("no Vulkan 1.3 device with graphics (and present) support");
    }

    void createDevice(const DeviceConfig&) {
        // Dynamic rendering is core in 1.3; check it is actually supported.
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(core_.phys, &f2);
        if (!f13.dynamicRendering) throw std::runtime_error("device lacks dynamicRendering");
        // Wireframe needs the non-solid fill mode. It is optional in Vulkan, though every desktop driver and lavapipe
        // have it; a device without it can still run everything else, so a wireframe pipeline is refused rather than
        // the device.
        fillModeNonSolid_ = f2.features.fillModeNonSolid == VK_TRUE;
        // Block-compressed textures (batch 7): optional in Vulkan, universal on desktop GPUs and lavapipe. A device
        // without it refuses the compressed formats at creation rather than sampling garbage. ASTC is the phone
        // counterpart: nearly universal there, rare on desktop — both are queried and the loaders pick.
        core_.bcSupported = f2.features.textureCompressionBC == VK_TRUE;
        core_.astcSupported = f2.features.textureCompressionASTC_LDR == VK_TRUE;

        std::vector<const char*> devExts;
        if (core_.surface) devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        VkPhysicalDeviceVulkan13Features enable13{};
        enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enable13.dynamicRendering = VK_TRUE;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = core_.qfamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures base{};
        base.fillModeNonSolid = fillModeNonSolid_ ? VK_TRUE : VK_FALSE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &enable13;
        dci.pEnabledFeatures = &base;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
        dci.ppEnabledExtensionNames = devExts.data();
        VK_CHECK(vkCreateDevice(core_.phys, &dci, nullptr, &core_.device), "vkCreateDevice");
        vkGetDeviceQueue(core_.device, core_.qfamily, 0, &core_.queue);
    }

    void createVma() {
        VmaAllocatorCreateInfo ai{};
        ai.physicalDevice = core_.phys;
        ai.device = core_.device;
        ai.instance = core_.instance;
        ai.vulkanApiVersion = VK_API_VERSION_1_3;
        VK_CHECK(vmaCreateAllocator(&ai, &core_.vma), "vmaCreateAllocator");
    }

    void createSetLayouts() {
        auto make = [&](std::vector<VkDescriptorSetLayoutBinding> b) {
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = static_cast<uint32_t>(b.size());
            ci.pBindings = b.data();
            VkDescriptorSetLayout l;
            VK_CHECK(vkCreateDescriptorSetLayout(core_.device, &ci, nullptr, &l), "vkCreateDescriptorSetLayout");
            return l;
        };
        const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkShaderStageFlags F = VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkShaderStageFlags V = VK_SHADER_STAGE_VERTEX_BIT;
        auto B = [](uint32_t binding, VkDescriptorType t, VkShaderStageFlags s) {
            return VkDescriptorSetLayoutBinding{binding, t, 1, s, nullptr};
        };
        const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
        // Index order must match SetType: Frame, Material, Skin, Post, ShaderMaterial, Motion, Particles, Hud, Compute.
        setLayouts_[0] = make({B(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VF), B(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VF),
                               B(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F)});
        setLayouts_[1] = make({B(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               B(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VF),
                               B(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               B(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               B(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F)});
        setLayouts_[2] = make({B(9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, V)});
        setLayouts_[3] = make({B(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               B(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               B(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F), B(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F),
                               // 7 is the effects pass's small parameter block (screen-space contact shadows). Textures
                               // stop at 6 on purpose: a Post pass that also has the Frame set binds the shadow atlas
                               // on GL unit 1, and the two sets would collide there.
                               B(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, F)});
        {
            std::vector<VkDescriptorSetLayoutBinding> sm;
            sm.push_back(B(15, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VF));   // vertex too: foliage reads instance rows (step 3)
            for (uint32_t b = 0; b < 8; ++b) sm.push_back(B(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F));
            setLayouts_[4] = make(sm);
        }
        setLayouts_[5] = make({B(10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, V)});
        setLayouts_[6] = make({B(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, V)});
        // Index 7 is SetType::Hud: the glyph atlas, at binding 0. Its own set so the HUD pass binds one texture and
        // nothing else, and does not disturb the Frame set's units.
        setLayouts_[7] = make({B(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, F)});
        // Index 8 is SetType::Compute: four storage buffers, visible to the compute stage only — the whole
        // descriptor surface a dispatch gets (docs/VGEOM.md's GPU path): 0 = cluster data, 1 = children,
        // 2 = the frame's indirect work buffer, 3 = the selection view. Storage, not UBO, for binding 3 so one
        // layout serves Vulkan and GL, whose descriptor binder maps every Compute buffer to an SSBO binding.
        setLayouts_[8] = make({B(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, C), B(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, C),
                               B(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, C), B(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, C)});
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(core_.phys, core_.surface, &caps);

        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(core_.phys, core_.surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(core_.phys, core_.surface, &fn, fmts.data());
        VkSurfaceFormatKHR chosen = fmts[0];
        for (auto& f : fmts) {
            // Non-sRGB: the tonemap pass already applies the sRGB curve.
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)) { chosen = f; break; }
        }

        int fw = 0, fh = 0;
        drawableSize(fw, fh);
        VkExtent2D ext{static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)};
        if (caps.currentExtent.width != 0xFFFFFFFFu) ext = caps.currentExtent;
        ext.width = std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);

        uint32_t imageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainKHR old = swapchain_;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = core_.surface;
        ci.minImageCount = imageCount;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(core_.device, &ci, nullptr, &fresh), "vkCreateSwapchainKHR");
        destroySwapchain();
        swapchain_ = fresh;
        if (old) vkDestroySwapchainKHR(core_.device, old, nullptr);

        swapExtent_ = ext;
        width_ = ext.width;
        height_ = ext.height;
        backbufferFormat_ = chosen.format;
        needRecreate_ = false;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(core_.device, swapchain_, &n, nullptr);
        std::vector<VkImage> images(n);
        vkGetSwapchainImagesKHR(core_.device, swapchain_, &n, images.data());
        for (VkImage img : images) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = chosen.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(core_.device, &vi, nullptr, &view), "vkCreateImageView(swapchain)");
            swapTextures_.push_back(std::make_unique<VKTexture>(&core_, img, view, ext.width, ext.height, chosen.format));
        }
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        renderFinished_.resize(n);
        for (auto& s : renderFinished_) VK_CHECK(vkCreateSemaphore(core_.device, &si, nullptr, &s), "semaphore");
    }

    void destroySwapchain() {
        vkDeviceWaitIdle(core_.device);
        swapTextures_.clear();
        for (auto s : renderFinished_) vkDestroySemaphore(core_.device, s, nullptr);
        renderFinished_.clear();
    }

    VkShaderModule loadModule(const std::string& path) {
        auto bytes = readFileBytes(path);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(core_.device, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }

public:
    // Instance-level members declared after methods for readability.
    GLFWwindow* window_ = nullptr;   // desktop, or null on Android
    void* androidWindow_ = nullptr;  // ANativeWindow* on Android, null on the desktop
    VKCore core_;
    bool fillModeNonSolid_ = false;   // the device supports VK_POLYGON_MODE_LINE, so wireframe pipelines are possible
    bool headless_ = false;
    uint32_t width_ = 0, height_ = 0;
    uint32_t passW_ = 0, passH_ = 0;
    VkCommandPool framePool_ = VK_NULL_HANDLE;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts_[9]{};
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence inFlight_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapExtent_{0, 0};
    VkFormat backbufferFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<std::unique_ptr<VKTexture>> swapTextures_;
    std::vector<VkSemaphore> renderFinished_;
    uint32_t imageIndex_ = 0;
    bool needRecreate_ = false;
    bool inFrame_ = false;
    std::unique_ptr<VKTexture> offscreen_;
    VkPipelineLayout curLayout_ = VK_NULL_HANDLE;
    VkPipelineBindPoint curBindPoint_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<VKTexture*> activeTargets_;
    std::unique_ptr<VKBuffer> readback_;
    uint32_t readbackW_ = 0, readbackH_ = 0;
    std::vector<uint8_t> capture_;
    VkQueryPool timePool_ = VK_NULL_HANDLE;
    float timePeriod_ = 1.0f;   // ns per timestamp tick (VkPhysicalDeviceProperties::limits.timestampPeriod)
    std::vector<float> gpuTimes_;
};

} // namespace

std::unique_ptr<Device> createVKDevice(const DeviceConfig& config, const NativeWindow& window) {
    return std::make_unique<VKDevice>(config, window);
}

std::unique_ptr<Device> createDevice(Backend backend, const DeviceConfig& config, GLFWwindow* window) {
    return createDevice(backend, config, NativeWindow{window, nullptr});
}

std::unique_ptr<Device> createDevice(Backend backend, const DeviceConfig& config, const NativeWindow& window) {
#if UKIYO_VK_ANDROID
    // There is no desktop GL on Android, and no GL backend is built there: Vulkan is the only one.
    if (backend == Backend::OpenGL) throw std::runtime_error("the OpenGL backend is not available on Android");
#else
    if (backend == Backend::OpenGL) return createGLDevice(config, window.glfw);
#endif
    return createVKDevice(config, window);
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(n));
    if (n > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short read on " + path);
    }
    std::fclose(f);
    return data;
}

} // namespace rhi
