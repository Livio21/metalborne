#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#include "macos_metalfx.h"
#include "bbport_overlay.h"
#include "imgui_impl_metal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <vector>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cmath>

namespace BbMetalFX {
struct SharedBuffer::Impl {
    VkDevice device;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    uint64_t address{};
    id<MTLHeap> heap;
    id<MTLBuffer> metal;
    PFN_vkDestroyBuffer destroy;
    PFN_vkFreeMemory free;
    Impl(VkDevice d, const VkPhysicalDeviceMemoryProperties& properties, uint64_t size,
         PFN_vkGetDeviceProcAddr proc, VkBufferUsageFlags usage) : device(d) {
#define BUFFER_PROC(name) auto name = reinterpret_cast<PFN_##name>(proc(d, #name))
        BUFFER_PROC(vkCreateBuffer); BUFFER_PROC(vkGetBufferMemoryRequirements);
        BUFFER_PROC(vkAllocateMemory); BUFFER_PROC(vkBindBufferMemory);
        BUFFER_PROC(vkGetMemoryMetalHandleEXT);
        BUFFER_PROC(vkGetBufferDeviceAddress);
        destroy = reinterpret_cast<PFN_vkDestroyBuffer>(proc(d, "vkDestroyBuffer"));
        free = reinterpret_cast<PFN_vkFreeMemory>(proc(d, "vkFreeMemory"));
#undef BUFFER_PROC
        if (!vkGetMemoryMetalHandleEXT || !size || size > 512 * 1024 * 1024) return;
        const auto fail = [](const char* where, VkResult result) {
            std::fprintf(stderr, "Native Metal shared buffer unavailable: %s (VkResult=%d).\n", where, result);
        };
        VkExternalMemoryBufferCreateInfo external{.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
            .size = size, .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkResult result = vkCreateBuffer(d, &info, nullptr, &buffer);
        if (result != VK_SUCCESS) { fail("vkCreateBuffer", result); return; }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(d, buffer, &req);
        uint32_t type = properties.memoryTypeCount;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            const auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((req.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i; break;
            }
        }
        if (type == properties.memoryTypeCount) { fail("coherent shared memory required", VK_ERROR_FEATURE_NOT_PRESENT); return; }
        VkExportMemoryAllocateInfo exported{.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .pNext = &exported, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryDedicatedAllocateInfo dedicated{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ?
                static_cast<void*>(&flags) : static_cast<void*>(&exported), .buffer = buffer};
        VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated, .allocationSize = req.size, .memoryTypeIndex = type};
        result = vkAllocateMemory(d, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) { fail("vkAllocateMemory", result); return; }
        result = vkBindBufferMemory(d, buffer, memory, 0);
        if (result != VK_SUCCESS) { fail("vkBindBufferMemory", result); return; }
        VkMemoryGetMetalHandleInfoEXT query{.sType = VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT,
            .memory = memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        void* handle = nullptr;
        result = vkGetMemoryMetalHandleEXT(d, &query, &handle);
        if (result != VK_SUCCESS || !handle) { fail("vkGetMemoryMetalHandleEXT", result); return; }
        heap = (__bridge id<MTLHeap>)handle;
        if (heap.type != MTLHeapTypePlacement || heap.storageMode != MTLStorageModeShared)
            { fail("shared placement heap required", VK_ERROR_FEATURE_NOT_PRESENT); return; }
        const MTLResourceOptions options = MTLResourceStorageModeShared |
            (heap.cpuCacheMode << MTLResourceCPUCacheModeShift) |
            (heap.hazardTrackingMode << MTLResourceHazardTrackingModeShift);
        const auto layout = [heap.device heapBufferSizeAndAlignWithLength:size options:options];
        if (layout.size > req.size || layout.size > heap.size || layout.align > req.alignment)
            { fail("Metal/Vulkan buffer layout differs", VK_ERROR_FEATURE_NOT_PRESENT); return; }
        metal = [heap newBufferWithLength:size options:options offset:0];
        if (!metal) fail("newBufferWithLength:offset:", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        if (metal && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
            const VkBufferDeviceAddressInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
            address = vkGetBufferDeviceAddress(d, &info);
            if (!address) { metal = nil; fail("buffer device address unavailable", VK_ERROR_FEATURE_NOT_PRESENT); }
        }
    }
    ~Impl() {
        metal = nil;
        if (buffer) destroy(device, buffer, nullptr);
        if (memory) free(device, memory, nullptr);
        heap = nil;
    }
};
SharedBuffer::SharedBuffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties,
                           uint64_t size, PFN_vkGetDeviceProcAddr proc, VkBufferUsageFlags usage)
    : impl(std::make_unique<Impl>(device, properties, size, proc, usage)) {}
SharedBuffer::~SharedBuffer() = default;
VkBuffer SharedBuffer::Handle() const { return impl->metal ? impl->buffer : VK_NULL_HANDLE; }
void* SharedBuffer::NativeHandle() const { return (__bridge void*)impl->metal; }
uint8_t* SharedBuffer::MappedData() const { return static_cast<uint8_t*>(impl->metal.contents); }
uint64_t SharedBuffer::DeviceAddress() const { return impl->address; }

static bool RunCommands(id<MTLDevice> device,
                        const std::function<bool(id<MTLCommandBuffer>, id<MTLFence>)>& encode) {
    @autoreleasepool {
        // ponytail: one device/queue and synchronous completion; batch with guest submissions later.
        static std::mutex mutex;
        std::lock_guard lock{mutex};
        static id<MTLCommandQueue> queue;
        if (!queue || queue.device != device) queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLFence> fence = [device newFence];
        id<MTLBlitCommandEncoder> release = [command blitCommandEncoder];
        if (!command || !fence || !release) return false;
        [release updateFence:fence];
        [release endEncoding];
        if (!encode(command, fence)) return false;
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            std::fprintf(stderr, "Native Metal command failed: %s; falling back to Vulkan.\n",
                         (command.error.localizedDescription ?: @"command failed").UTF8String);
            return false;
        }
        return true;
    }
}

static bool RunBlit(id<MTLDevice> device, const std::function<void(id<MTLBlitCommandEncoder>)>& encode) {
    return RunCommands(device, [&](id<MTLCommandBuffer> command, id<MTLFence> fence) {
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (!encoder) return false;
        [encoder waitForFence:fence];
        encode(encoder);
        [encoder endEncoding];
        return true;
    });
}

bool CopyBuffers(void* source, void* destination, std::span<const VkBufferCopy> copies) {
    @autoreleasepool {
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)source, dst = (__bridge id<MTLBuffer>)destination;
        if (!src || !dst || src == dst || src.device != dst.device || copies.empty()) return false;
        for (size_t i = 0; i < copies.size(); ++i) {
            const auto& c = copies[i];
            if (!c.size || ((c.srcOffset | c.dstOffset | c.size) & 3) ||
                c.srcOffset > src.length || c.size > src.length - c.srcOffset ||
                c.dstOffset > dst.length || c.size > dst.length - c.dstOffset) return false;
            for (size_t j = 0; j < i; ++j)
                if (c.dstOffset < copies[j].dstOffset + copies[j].size &&
                    copies[j].dstOffset < c.dstOffset + c.size) return false;
        }
        return RunBlit(src.device, [&](id<MTLBlitCommandEncoder> encoder) {
            for (const auto& c : copies)
                [encoder copyFromBuffer:src sourceOffset:c.srcOffset toBuffer:dst
                        destinationOffset:c.dstOffset size:c.size];
        });
    }
}

static MTLPixelFormat ImageFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case VK_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
    case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
    default: return MTLPixelFormatInvalid;
    }
}

struct SharedImage::Impl {
    VkDevice device;
    VkImage image{};
    VkDeviceMemory memory{};
    uint64_t size{};
    id<MTLTexture> metal;
    id<MTLHeap> heap;
    PFN_vkDestroyImage destroy;
    PFN_vkFreeMemory free;
    Impl(VkInstance instance, VkPhysicalDevice physical, VkDevice d, const VkImageCreateInfo& info,
         PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp) : device(d) {
#define IMAGE_PROC(name) auto name = reinterpret_cast<PFN_##name>(dp(d, #name))
        IMAGE_PROC(vkCreateImage); IMAGE_PROC(vkGetImageMemoryRequirements);
        IMAGE_PROC(vkAllocateMemory); IMAGE_PROC(vkBindImageMemory); IMAGE_PROC(vkGetMemoryMetalHandleEXT);
#undef IMAGE_PROC
        destroy = reinterpret_cast<PFN_vkDestroyImage>(dp(d, "vkDestroyImage"));
        free = reinterpret_cast<PFN_vkFreeMemory>(dp(d, "vkFreeMemory"));
        auto format_props = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            ip(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
        auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            ip(instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto format = ImageFormat(info.format);
        constexpr VkImageCreateFlags allowed = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        constexpr VkImageUsageFlags allowed_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        if (!vkGetMemoryMetalHandleEXT || !format_props || !get_memory || format == MTLPixelFormatInvalid ||
            info.pNext || info.imageType != VK_IMAGE_TYPE_2D || info.tiling != VK_IMAGE_TILING_OPTIMAL ||
            info.samples != VK_SAMPLE_COUNT_1_BIT || info.extent.depth != 1 || (info.flags & ~allowed) ||
            (info.usage & ~allowed_usage) ||
            info.sharingMode != VK_SHARING_MODE_EXCLUSIVE) return;
        const auto fail = [&](const char* stage, VkResult result = VK_ERROR_FEATURE_NOT_PRESENT) {
            static std::atomic<unsigned> failures{0};
            if (failures.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr, "Native Metal shared image unavailable: format %u, %ux%u; %s (VkResult=%d).\n",
                    unsigned(info.format), info.extent.width, info.extent.height, stage, int(result));
        };
        VkPhysicalDeviceExternalImageFormatInfo external_query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external_query, .format = info.format, .type = info.imageType,
            .tiling = info.tiling, .usage = info.usage, .flags = info.flags};
        VkExternalImageFormatProperties external_props{.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props};
        VkResult result = format_props(physical, &query, &props);
        const auto& limits = props.imageFormatProperties;
        if (result != VK_SUCCESS || !(external_props.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) || info.extent.width > limits.maxExtent.width ||
            info.extent.height > limits.maxExtent.height || info.mipLevels > limits.maxMipLevels ||
            info.arrayLayers > limits.maxArrayLayers) { fail("external image format query/limits", result); return; }
        VkExternalMemoryImageCreateInfo external{.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        auto image_info = info;
        image_info.pNext = &external;
        result = vkCreateImage(d, &image_info, nullptr, &image);
        if (result != VK_SUCCESS) { fail("vkCreateImage", result); return; }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(d, image, &req);
        VkPhysicalDeviceMemoryProperties properties{};
        get_memory(physical, &properties);
        uint32_t type = properties.memoryTypeCount;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i; break;
            }
        if (type == properties.memoryTypeCount) { fail("no device-local memory type"); return; }
        VkExportMemoryAllocateInfo exported{.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkMemoryDedicatedAllocateInfo dedicated{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &exported, .image = image};
        VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated, .allocationSize = req.size, .memoryTypeIndex = type};
        result = vkAllocateMemory(d, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) { fail("vkAllocateMemory", result); return; }
        result = vkBindImageMemory(d, image, memory, 0);
        if (result != VK_SUCCESS) { fail("vkBindImageMemory", result); return; }
        VkMemoryGetMetalHandleInfoEXT export_query{.sType = VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT,
            .memory = memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        void* handle = nullptr;
        result = vkGetMemoryMetalHandleEXT(d, &export_query, &handle);
        if (result != VK_SUCCESS || !handle) { fail("vkGetMemoryMetalHandleEXT", result); return; }
        heap = (__bridge id<MTLHeap>)handle;
        if (heap.type != MTLHeapTypePlacement) { fail("requires a placement heap"); return; }
        MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
        desc.textureType = info.arrayLayers > 1 || (info.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ?
            MTLTextureType2DArray : MTLTextureType2D;
        desc.pixelFormat = format;
        desc.width = info.extent.width; desc.height = info.extent.height;
        desc.depth = 1; desc.mipmapLevelCount = info.mipLevels; desc.arrayLength = info.arrayLayers;
        desc.storageMode = heap.storageMode; desc.cpuCacheMode = heap.cpuCacheMode;
        desc.hazardTrackingMode = heap.hazardTrackingMode;
        desc.usage = MTLTextureUsageUnknown;
        // Match KosmicKrisp's color image descriptor, including compression eligibility.
        desc.allowGPUOptimizedContents = !(info.usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
        if (info.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
            desc.usage |= MTLTextureUsageShaderRead;
        if (info.usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) desc.usage |= MTLTextureUsageShaderWrite;
        if (info.usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) desc.usage |= MTLTextureUsageRenderTarget;
        if (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) desc.usage |= MTLTextureUsagePixelFormatView;
        const auto layout = [heap.device heapTextureSizeAndAlignWithDescriptor:desc];
        if (!layout.size || layout.size > req.size || layout.size > heap.size || layout.align > req.alignment)
            { fail("Metal/Vulkan texture layout requirements differ"); return; }
        metal = [heap newTextureWithDescriptor:desc offset:0];
        if (!metal) { fail("newTextureWithDescriptor:offset:", VK_ERROR_OUT_OF_DEVICE_MEMORY); return; }
        size = req.size;
    }
    ~Impl() {
        metal = nil;
        if (image) destroy(device, image, nullptr);
        if (memory) free(device, memory, nullptr);
        heap = nil;
    }
};
SharedImage::SharedImage(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                         const VkImageCreateInfo& info, PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp)
    : impl(std::make_unique<Impl>(instance, physical, device, info, ip, dp)) {}
SharedImage::~SharedImage() = default;
VkImage SharedImage::Handle() const { return impl->metal ? impl->image : VK_NULL_HANDLE; }
void* SharedImage::NativeHandle() const { return (__bridge void*)impl->metal; }
uint64_t SharedImage::SizeBytes() const { return impl->size; }

bool ClearImage(void* image, const VkImageSubresourceRange& range, const VkClearColorValue& color) {
    @autoreleasepool {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)image;
        if (!texture || (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray) ||
            texture.sampleCount != 1 || !(texture.usage & MTLTextureUsageRenderTarget) ||
            range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || !range.levelCount || !range.layerCount ||
            range.baseMipLevel >= texture.mipmapLevelCount || range.levelCount > texture.mipmapLevelCount - range.baseMipLevel ||
            range.baseArrayLayer >= texture.arrayLength || range.layerCount > texture.arrayLength - range.baseArrayLayer)
            return false;
        for (float component : color.float32) if (!std::isfinite(component)) return false;
        return RunCommands(texture.device, [&](id<MTLCommandBuffer> command, id<MTLFence> fence) {
            for (uint32_t level = range.baseMipLevel; level < range.baseMipLevel + range.levelCount; ++level)
                for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer) {
                    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    auto attachment = pass.colorAttachments[0];
                    attachment.texture = texture;
                    attachment.level = level;
                    attachment.slice = layer;
                    attachment.loadAction = MTLLoadActionClear;
                    attachment.storeAction = MTLStoreActionStore;
                    attachment.clearColor = MTLClearColorMake(color.float32[0], color.float32[1],
                                                              color.float32[2], color.float32[3]);
                    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
                    if (!encoder) return false;
                    [encoder waitForFence:fence beforeStages:MTLRenderStageVertex];
                    [encoder updateFence:fence afterStages:MTLRenderStageFragment];
                    [encoder endEncoding];
                }
            return true;
        });
    }
}

static bool ValidImageRegion(id<MTLTexture> t, const VkImageSubresourceLayers& sub,
                             const VkOffset3D& o, const VkExtent3D& e) {
    if ((t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) || t.sampleCount != 1 ||
        sub.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || sub.mipLevel >= t.mipmapLevelCount || !sub.layerCount ||
        sub.baseArrayLayer >= t.arrayLength || sub.layerCount > t.arrayLength - sub.baseArrayLayer ||
        o.x < 0 || o.y < 0 || o.z || !e.width || !e.height || e.depth != 1) return false;
    const auto width = std::max<NSUInteger>(t.width >> sub.mipLevel, 1);
    const auto height = std::max<NSUInteger>(t.height >> sub.mipLevel, 1);
    return NSUInteger(o.x) < width && e.width <= width - o.x && NSUInteger(o.y) < height && e.height <= height - o.y;
}

static bool ImageRegionsOverlap(const VkImageSubresourceLayers& a, const VkOffset3D& ao, const VkExtent3D& ae,
                                const VkImageSubresourceLayers& b, const VkOffset3D& bo, const VkExtent3D& be) {
    return a.mipLevel == b.mipLevel && a.baseArrayLayer < b.baseArrayLayer + b.layerCount &&
        b.baseArrayLayer < a.baseArrayLayer + a.layerCount &&
        uint64_t(ao.x) < uint64_t(bo.x) + be.width && uint64_t(bo.x) < uint64_t(ao.x) + ae.width &&
        uint64_t(ao.y) < uint64_t(bo.y) + be.height && uint64_t(bo.y) < uint64_t(ao.y) + ae.height;
}

bool CopyImages(void* source, void* destination, std::span<const VkImageCopy> copies) {
    @autoreleasepool {
        id<MTLTexture> src = (__bridge id<MTLTexture>)source, dst = (__bridge id<MTLTexture>)destination;
        const auto is_2d = [](id<MTLTexture> t) { return t.textureType == MTLTextureType2D || t.textureType == MTLTextureType2DArray; };
        if (!src || !dst || src == dst || src.device != dst.device || src.pixelFormat != dst.pixelFormat ||
            !is_2d(src) || !is_2d(dst) || src.sampleCount != 1 || dst.sampleCount != 1 || copies.empty()) return false;
        for (size_t i = 0; i < copies.size(); ++i) {
            const auto& c = copies[i];
            if (c.srcSubresource.layerCount != c.dstSubresource.layerCount ||
                !ValidImageRegion(src, c.srcSubresource, c.srcOffset, c.extent) || !ValidImageRegion(dst, c.dstSubresource, c.dstOffset, c.extent)) return false;
            for (size_t j = 0; j < i; ++j) {
                const auto& p = copies[j];
                if (ImageRegionsOverlap(c.dstSubresource, c.dstOffset, c.extent,
                                        p.dstSubresource, p.dstOffset, p.extent)) return false;
            }
        }
        return RunBlit(src.device, [&](id<MTLBlitCommandEncoder> encoder) {
            for (const auto& c : copies)
                for (uint32_t layer = 0; layer < c.srcSubresource.layerCount; ++layer)
                    [encoder copyFromTexture:src sourceSlice:c.srcSubresource.baseArrayLayer + layer
                        sourceLevel:c.srcSubresource.mipLevel sourceOrigin:MTLOriginMake(c.srcOffset.x, c.srcOffset.y, 0)
                        sourceSize:MTLSizeMake(c.extent.width, c.extent.height, 1) toTexture:dst
                        destinationSlice:c.dstSubresource.baseArrayLayer + layer destinationLevel:c.dstSubresource.mipLevel
                        destinationOrigin:MTLOriginMake(c.dstOffset.x, c.dstOffset.y, 0)];
        });
    }
}

bool CopyBufferImage(void* buffer, void* image, std::span<const VkBufferImageCopy> copies, bool upload) {
    @autoreleasepool {
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)buffer;
        id<MTLTexture> t = (__bridge id<MTLTexture>)image;
        if (!b || !t || b.device != t.device || copies.empty()) return false;
        uint64_t pixel_bytes;
        switch (t.pixelFormat) {
        case MTLPixelFormatR8Unorm: pixel_bytes = 1; break;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatR16Float: pixel_bytes = 2; break;
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRG16Float: pixel_bytes = 4; break;
        case MTLPixelFormatRGBA16Float: pixel_bytes = 8; break;
        default: return false;
        }
        struct Layout { uint64_t row, slice, end; };
        std::vector<Layout> layouts;
        for (size_t i = 0; i < copies.size(); ++i) {
            const auto& c = copies[i];
            if (!ValidImageRegion(t, c.imageSubresource, c.imageOffset, c.imageExtent) ||
                c.bufferOffset % pixel_bytes || c.bufferOffset > b.length) return false;
            const uint64_t width = c.bufferRowLength ? c.bufferRowLength : c.imageExtent.width;
            const uint64_t height = c.bufferImageHeight ? c.bufferImageHeight : c.imageExtent.height;
            // ponytail: Apple-Silicon 2D row limit; wider padding falls back to Vulkan.
            if (width < c.imageExtent.width || width > 16384 || height < c.imageExtent.height) return false;
            const uint64_t row = width * pixel_bytes, slice = row * height;
            const uint64_t tail = (c.imageExtent.height - 1) * row + c.imageExtent.width * pixel_bytes;
            const uint64_t remaining = b.length - c.bufferOffset;
            if (tail > remaining || c.imageSubresource.layerCount - 1 > (remaining - tail) / slice) return false;
            const uint64_t end = c.bufferOffset + (c.imageSubresource.layerCount - 1) * slice + tail;
            for (size_t j = 0; j < i; ++j) {
                const auto& p = copies[j];
                if (upload ? ImageRegionsOverlap(c.imageSubresource, c.imageOffset, c.imageExtent,
                                                p.imageSubresource, p.imageOffset, p.imageExtent) :
                    c.bufferOffset < layouts[j].end && p.bufferOffset < end) return false;
            }
            layouts.push_back({row, slice, end});
        }
        return RunBlit(b.device, [&](id<MTLBlitCommandEncoder> encoder) {
            for (size_t i = 0; i < copies.size(); ++i) {
                const auto& c = copies[i];
                for (uint32_t layer = 0; layer < c.imageSubresource.layerCount; ++layer) {
                    const auto offset = c.bufferOffset + layer * layouts[i].slice;
                    const auto origin = MTLOriginMake(c.imageOffset.x, c.imageOffset.y, 0);
                    const auto size = MTLSizeMake(c.imageExtent.width, c.imageExtent.height, 1);
                    if (upload)
                        [encoder copyFromBuffer:b sourceOffset:offset sourceBytesPerRow:layouts[i].row sourceBytesPerImage:0
                            sourceSize:size toTexture:t destinationSlice:c.imageSubresource.baseArrayLayer + layer
                            destinationLevel:c.imageSubresource.mipLevel destinationOrigin:origin];
                    else
                        [encoder copyFromTexture:t sourceSlice:c.imageSubresource.baseArrayLayer + layer
                            sourceLevel:c.imageSubresource.mipLevel sourceOrigin:origin sourceSize:size toBuffer:b
                            destinationOffset:offset destinationBytesPerRow:layouts[i].row destinationBytesPerImage:0];
                }
            }
        });
    }
}

bool CheckCompute(std::span<const ComputeBuffer> buffers, std::span<const uint8_t> push,
                  uint32_t groups, uint32_t threads) {
    @autoreleasepool {
        const auto fail = [](NSString* error) {
            std::fprintf(stderr, "Native Metal compute proof: FAILED: %s\n", error.UTF8String);
            return false;
        };
        const char* path = std::getenv("BB_METAL_COMPUTE_MSL");
        if (!path || buffers.empty() || buffers.size() > 2 || push.empty() || !groups || !threads)
            return fail(@"missing source or invalid dispatch");
        NSError* error = nil;
        NSString* source = [NSString stringWithContentsOfFile:@(path)
                            encoding:NSUTF8StringEncoding error:&error];
        if (!source) return fail(error.localizedDescription);
        id<MTLDevice> device = buffers[0].native ? ((__bridge id<MTLBuffer>)buffers[0].native).device : MTLCreateSystemDefaultDevice();
        if (!device) return fail(@"Metal device unavailable");
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) return fail(error.localizedDescription);
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        if (!function) return fail(@"main0 entry point unavailable");
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) return fail(error.localizedDescription);
        if (threads > pipeline.maxTotalThreadsPerThreadgroup) return fail(@"threadgroup too large");
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
        if (!arguments) return fail(@"argument buffer unavailable");
        id<MTLBuffer> argument_buffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        if (!argument_buffer) return fail(@"argument allocation failed");
        [arguments setArgumentBuffer:argument_buffer offset:0];
        std::vector<id<MTLBuffer>> native;
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto& b = buffers[i];
            if (b.before.empty() || b.before.size() != b.reference.size() || b.before.size() > 16 * 1024 * 1024)
                return fail(@"invalid buffer snapshot");
            id<MTLBuffer> buffer = b.native ? (__bridge id<MTLBuffer>)b.native :
                [device newBufferWithBytes:b.before.data() length:b.before.size() options:MTLResourceStorageModeShared];
            if (!buffer) return fail(@"buffer allocation failed");
            if (buffer.device != device || buffer.length < b.before.size() || !buffer.contents)
                return fail(@"incompatible shared buffer");
            native.push_back(buffer);
            [arguments setBuffer:buffer offset:0 atIndex:i];
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLFence> fence = nil;
        if (buffers[0].native) {
            fence = [device newFence];
            id<MTLBlitCommandEncoder> release = [command blitCommandEncoder];
            if (!fence || !release) return fail(@"shared buffer fence unavailable");
            [release updateFence:fence];
            [release endEncoding];
        }
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!encoder) return fail(@"command encoder unavailable");
        if (fence) [encoder waitForFence:fence];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:argument_buffer offset:0 atIndex:0];
        [encoder setBytes:push.data() length:push.size() atIndex:1];
        for (id<MTLBuffer> buffer : native) [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) return fail(command.error.localizedDescription ?: @"command failed");
        size_t compared = 0, changed = 0, mismatches = 0;
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto* result = static_cast<const uint8_t*>(native[i].contents);
            const auto& b = buffers[i];
            for (size_t j = 0; j < b.reference.size(); ++j) {
                changed += b.before[j] != b.reference[j];
                mismatches += result[j] != b.reference[j];
            }
            compared += b.reference.size();
        }
        char timing[40] = "unavailable";
        const double start = command.GPUStartTime, duration = command.GPUEndTime - start;
        if (start > 0 && duration >= 0 && duration < 60)
            std::snprintf(timing, sizeof(timing), "%.3f ms", duration * 1000);
        std::fprintf(stderr, "Native Metal compute proof: %s; %zu buffers, %zu bytes compared, %zu bytes changed, %zu mismatches; %ux1x1 groups, %ux1x1 threads; GPU %s.\n",
                     mismatches ? "MISMATCH" : "PASS", buffers.size(), compared, changed, mismatches,
                     groups, threads, timing);
        return mismatches == 0;
    }
}

struct Presentation::Impl {
    struct Texture {
        VkImage image{};
        std::unique_ptr<SharedImage> shared;
        id<MTLTexture> metal;
    };
    struct Slot {
        Images images;
        Texture input, output;
        id<MTLFXSpatialScaler> scaler;
        id<MTLTexture> scaled;
    };
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    PFN_vkGetInstanceProcAddr instance_proc;
    PFN_vkGetDeviceProcAddr device_proc;
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    id<MTLRenderPipelineState> present_pipeline;
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
    const char* format_name{"RGBA8"};
    // Frame IDs are u8. Fixed slots allow draw/present threads to access distinct frames;
    // the existing per-frame fence protects each slot's textures.
    std::array<Slot, 256> slots;
    std::atomic<bool> available{true};
    bool announced = false, completed = false;
    bool spatial = true, presented = false;

    Impl(VkInstance i, VkPhysicalDevice p, VkDevice d,
         PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp)
        : instance(i), physical(p), device(d), instance_proc(ip), device_proc(dp) {
        if (const char* choice = std::getenv("BB_METALFX_FORMAT")) {
            if (std::strcmp(choice, "rgba16f") == 0) {
                format = VK_FORMAT_R16G16B16A16_SFLOAT;
                format_name = "RGBA16F";
            } else if (std::strcmp(choice, "rgba8") != 0) {
                std::fprintf(stderr, "MetalFX: unknown scratch format '%s'; using RGBA8.\n", choice);
            }
        }
        available = dp(d, "vkGetMemoryMetalHandleEXT") &&
                    ip(i, "vkGetPhysicalDeviceImageFormatProperties2") &&
                    ip(i, "vkGetPhysicalDeviceMemoryProperties");
    }
    void Release(Texture& t) {
        t.metal = nil;
        t.shared.reset();
        t.image = {};
    }
    void Release(Slot& s) {
        s.scaler = nil; s.scaled = nil;
        Release(s.input); Release(s.output);
        s.images = {};
    }
    ~Impl() { for (auto& slot : slots) Release(slot); }

    bool Allocate(Texture& t, uint32_t width, uint32_t height) {
        // SDR pixels already carry the presenter's sRGB encode.
        VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {width, height, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        t.shared = std::make_unique<SharedImage>(instance, physical, device, info, instance_proc, device_proc);
        t.image = t.shared->Handle();
        t.metal = (__bridge id<MTLTexture>)t.shared->NativeHandle();
        return t.image != VK_NULL_HANDLE;
    }
    void Disable(const char* why) {
        available = false;
        if (layer) layer.framebufferOnly = NO;
        std::fprintf(stderr, "%s unavailable: %s; using Vulkan presentation.\n",
                     layer ? "Native Metal presentation" : "MetalFX spatial", why);
    }
};
Presentation::Presentation(VkInstance i, VkPhysicalDevice p, VkDevice d,
                 PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp)
    : impl(std::make_unique<Impl>(i, p, d, ip, dp)) {}
Presentation::~Presentation() = default;
bool Presentation::Available() const { return impl->available; }
void Presentation::SetNativeLayer(void* handle) {
    impl->layer = (__bridge CAMetalLayer*)handle;
    const char* mode = std::getenv("BB_METALFX");
    impl->spatial = mode && std::strcmp(mode, "spatial") == 0;
}
bool Presentation::NativePresentation() const { return impl->available && impl->layer; }
VkFormat Presentation::InputFormat() const { return impl->format; }
VkImage Presentation::CreateFrameImage(uint8_t key, uint32_t width, uint32_t height) {
    @autoreleasepool {
        if (!NativePresentation()) return {};
        auto& s = impl->slots.at(key);
        impl->Release(s);
        if (!impl->Allocate(s.input, width, height)) {
            impl->Release(s); impl->Disable("exportable frame allocation failed"); return {};
        }
        s.images = {s.input.image, {}, width, height, 0, 0, false};
        std::fprintf(stderr, "Native Metal frame #%u: post-processing renders directly into shared %s %ux%u.\n",
                     unsigned(key), impl->format_name, width, height);
        return s.input.image;
    }
}
const Images* Presentation::Find(uint32_t key) const {
    const auto& s = impl->slots.at(key);
    return s.images.input ? &s.images : nullptr;
}
Images* Presentation::Prepare(uint32_t key, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
    @autoreleasepool {
        if (!impl->available || !iw || !ih || !ow || !oh) return nullptr;
        const bool native = NativePresentation();
        if (!native && (ow < iw || oh < ih || (ow == iw && oh == ih))) return nullptr;
        auto& s = impl->slots.at(key);
        if (s.images.input_width == iw && s.images.input_height == ih &&
            s.images.output_width == ow && s.images.output_height == oh) return &s.images;
        // Window resize must retain the frame image/view used by Vulkan post-processing.
        const bool keep_input = native && s.input.image &&
            s.images.input_width == iw && s.images.input_height == ih;
        s.scaler = nil; s.scaled = nil;
        impl->Release(s.output);
        if (!keep_input) impl->Release(s.input);
        const auto fail = [&](const char* why) -> Images* {
            // A native frame still owns a Vulkan view of the shared input on fallback.
            if (!keep_input) impl->Release(s);
            else { s.scaler = nil; s.scaled = nil; impl->Release(s.output); }
            impl->Disable(why); return nullptr;
        };
        if ((!keep_input && !impl->Allocate(s.input, iw, ih)) ||
            (!native && !impl->Allocate(s.output, ow, oh))) {
            return fail("exportable placement heap textures could not be allocated");
        }
        id<MTLDevice> device = s.input.metal.device;
        const bool scale = impl->spatial && ow >= iw && oh >= ih && (ow != iw || oh != ih);
        if ((!native && s.output.metal.device != device) ||
            (scale && ![MTLFXSpatialScalerDescriptor supportsDevice:device])) {
            return fail("GPU does not support the spatial scaler");
        }
        if (!impl->queue) impl->queue = [device newCommandQueue];
        if (!impl->queue) {
            return fail("Metal command queue allocation failed");
        }
        if (scale) {
            MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
            desc.inputWidth = iw; desc.inputHeight = ih; desc.outputWidth = ow; desc.outputHeight = oh;
            desc.colorTextureFormat = s.input.metal.pixelFormat;
            desc.outputTextureFormat = s.input.metal.pixelFormat;
            desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            s.scaler = [desc newSpatialScalerWithDevice:device];
            MTLTextureDescriptor* scaled_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                s.input.metal.pixelFormat width:ow height:oh mipmapped:NO];
            scaled_desc.storageMode = MTLStorageModePrivate;
            scaled_desc.usage = s.scaler.outputTextureUsage | MTLTextureUsageShaderRead;
            s.scaled = [device newTextureWithDescriptor:scaled_desc];
            if (!impl->queue || !s.scaler || !s.scaled ||
                (s.input.metal.usage & s.scaler.colorTextureUsage) != s.scaler.colorTextureUsage ||
                (s.scaled.usage & s.scaler.outputTextureUsage) != s.scaler.outputTextureUsage) {
                return fail("scaler creation or texture usage requirements failed");
            }
            // Supply MetalFX's fence for the driver's untracked heap resources.
            if (s.input.metal.hazardTrackingMode == MTLHazardTrackingModeUntracked) {
                s.scaler.fence = [device newFence];
                if (!s.scaler.fence) {
                    return fail("untracked-resource fence allocation failed");
                }
            }
        }
        s.images = {s.input.image, s.output.image, iw, ih, ow, oh,
                    keep_input && s.images.initialized};
        if (!impl->announced) {
            impl->announced = true;
            std::fprintf(stderr, "%s prepared: %ux%u -> %ux%u %s; GPU textures, CPU completion bridge (experimental).\n",
                native ? "Native Metal presentation" : "MetalFX spatial", iw, ih, ow, oh, impl->format_name);
        }
        return &s.images;
    }
}
bool Presentation::Encode(uint32_t key, uint32_t window_width, uint32_t window_height) {
    @autoreleasepool {
        if (!impl->available) return false;
        const bool native = impl->layer != nil;
        auto& s = impl->slots.at(key);
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        if (!command) { impl->Disable("Metal command buffer allocation failed"); return false; }
        command.label = native ? @"Metalborne native presentation" : @"bbport MetalFX spatial";
        if (s.scaler) {
            s.scaler.colorTexture = s.input.metal; s.scaler.outputTexture = s.scaled;
            s.scaler.inputContentWidth = s.images.input_width;
            s.scaler.inputContentHeight = s.images.input_height;
            if (s.scaler.fence) {
                id<MTLBlitCommandEncoder> fence_encoder = [command blitCommandEncoder];
                if (!fence_encoder) { impl->Disable("Metal input fence encoder allocation failed"); return false; }
                [fence_encoder updateFence:s.scaler.fence];
                [fence_encoder endEncoding];
            }
            [s.scaler encodeToCommandBuffer:command];
        }
        if (native) {
            CAMetalLayer* layer = impl->layer;
            if (!window_width || !window_height) return false;
            if (layer.device && layer.device != s.input.metal.device) {
                impl->Disable("window and exported textures use different Metal devices"); return false;
            }
            layer.device = s.input.metal.device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = YES;
            layer.displaySyncEnabled = YES;
            layer.drawableSize = CGSizeMake(window_width, window_height);
            if (!impl->present_pipeline) {
                NSString* source = @R"MSL(
                    #include <metal_stdlib>
                    using namespace metal;
                    struct Varying { float4 position [[position]]; float2 uv; };
                    vertex Varying present_vertex(uint id [[vertex_id]]) {
                        const float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
                        return {float4(p[id],0,1), float2((p[id].x+1)*0.5,(1-p[id].y)*0.5)};
                    }
                    fragment half4 present_fragment(Varying v [[stage_in]], texture2d<half> image [[texture(0)]]) {
                        constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
                        return half4(image.sample(linear_sampler,v.uv).rgb,1);
                    }
                )MSL";
                NSError* error = nil;
                id<MTLLibrary> library = [layer.device newLibraryWithSource:source options:nil error:&error];
                MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
                desc.vertexFunction = [library newFunctionWithName:@"present_vertex"];
                desc.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
                desc.colorAttachments[0].pixelFormat = layer.pixelFormat;
                if (library) impl->present_pipeline = [layer.device newRenderPipelineStateWithDescriptor:desc error:&error];
                if (!impl->present_pipeline) {
                    std::fprintf(stderr, "Native Metal shader error: %s\n", error.localizedDescription.UTF8String);
                    impl->Disable("presentation pipeline creation failed"); return false;
                }
            }
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) return false; // Occlusion/resize can temporarily exhaust drawables.
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) { impl->Disable("presentation encoder allocation failed"); return false; }
            if (s.scaler.fence) [encoder waitForFence:s.scaler.fence beforeStages:MTLRenderStageFragment];
            [encoder setViewport:MTLViewport{double(window_width-s.images.output_width)/2,
                double(window_height-s.images.output_height)/2, double(s.images.output_width),
                double(s.images.output_height), 0, 1}];
            [encoder setRenderPipelineState:impl->present_pipeline];
            [encoder setFragmentTexture:s.scaler ? s.scaled : s.input.metal atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            const bool overlay = BbOverlay::RenderMetal((__bridge void*)layer.device,
                (__bridge void*)pass, (__bridge void*)command, (__bridge void*)encoder,
                window_width, window_height);
            [encoder endEncoding];
            [command presentDrawable:drawable];
            [command commit];
            [command waitUntilCompleted];
            if (!overlay || command.status != MTLCommandBufferStatusCompleted) {
                impl->Disable(!overlay ? "Metal overlay initialization failed" : "Metal presentation command failed");
                return false;
            }
            if (!impl->presented) {
                impl->presented = true;
                std::fprintf(stderr, "Native Metal presentation: first drawable completed successfully%s.\n",
                             s.scaler ? " with MetalFX spatial" : "");
            }
            return true;
        }
        // ponytail: the driver uses shared heap storage; MetalFX needs private output.
        // Keep the GPU blit until the driver supports private heap interop.
        id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
        if (!copy) { impl->Disable("Metal output blit allocation failed"); return false; }
        if (s.scaler.fence) [copy waitForFence:s.scaler.fence];
        [copy copyFromTexture:s.scaled sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(s.images.output_width, s.images.output_height, 1)
            toTexture:s.output.metal destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [copy endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            std::fprintf(stderr, "MetalFX command error: %s\n", command.error.localizedDescription.UTF8String);
            impl->Disable("Metal command did not complete successfully"); return false;
        }
        if (!impl->completed) {
            impl->completed = true;
            std::fprintf(stderr, "MetalFX spatial: first GPU upscale completed successfully.\n");
        }
        return true;
    }
}
bool OverlayInit(void* device) { return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device); }
void OverlayShutdown() { ImGui_ImplMetal_Shutdown(); }
void OverlayNewFrame(void* pass) { ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)pass); }
void OverlayRender(void* data, void* command, void* encoder) {
    ImGui_ImplMetal_RenderDrawData(static_cast<ImDrawData*>(data),
        (__bridge id<MTLCommandBuffer>)command, (__bridge id<MTLRenderCommandEncoder>)encoder);
}
}
