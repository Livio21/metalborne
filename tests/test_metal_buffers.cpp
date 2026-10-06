// SPDX-License-Identifier: GPL-2.0-or-later
// One headless check of the production cache/copy path; no game data required.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include <vulkan/vulkan_format_traits.hpp>

int main(int argc, char** argv) {
    const bool native = argc == 1 || std::strcmp(argv[1], "--vulkan");
    setenv("BB_METAL_BUFFER_CACHE", native ? "1" : "0", 1);
    setenv("BB_METAL_BUFFER_COPY", native ? "1" : "0", 1);
    setenv("BB_METAL_IMAGE_CACHE", native ? "1" : "0", 1);
    setenv("BB_METAL_IMAGE_COPY", native ? "1" : "0", 1);
    unsetenv("BB_VK_RECORD_THREAD");
    Vulkan::Instance instance(0, false);
    const bool threaded = argc > 1 && !std::strcmp(argv[1], "--threaded");
    Vulkan::Scheduler scheduler(instance, threaded);
    assert(scheduler.IsRecordingDeferred() == threaded);
    Vulkan::Runtime runtime(instance, scheduler);
    using VideoCore::Buffer;
    using VideoCore::MemoryType;
    Buffer original(instance, 0, 4096, MemoryType::HostUncached);
    Buffer destination(instance, 0, 4096, MemoryType::DeviceLocal);
    Buffer readback(instance, 0, 4096, MemoryType::HostCached);
    const auto handle = original.Handle();
    const auto address = original.BufferDeviceAddress();
    Buffer source(std::move(original));
    assert(!original.Handle() && source.Handle() == handle && source.BufferDeviceAddress() == address);
    assert(source.mapped_data.size() == 4096);
    if (native) {
        assert(source.buffer.metal && destination.buffer.metal && readback.buffer.metal);
        assert(source.buffer.metal->MappedData() == source.mapped_data.data());
        // The stream buffer is larger than the old diagnostic allocation ceiling.
        Buffer stream(instance, 0, 128 * 1024 * 1024, MemoryType::Stream);
        assert(stream.buffer.metal && stream.mapped_data.size() == stream.SizeBytes());
        stream.mapped_data.back() = 17;
        assert(stream.buffer.metal->MappedData()[stream.SizeBytes() - 1] == 17);
        void* src = source.buffer.metal->NativeHandle();
        void* dst = destination.buffer.metal->NativeHandle();
        const VkBufferCopy same{0, 0, 4}, bounds{0, 4092, 8}, alignment{1, 0, 4};
        const VkBufferCopy overlap[]{{0, 0, 8}, {8, 4, 8}};
        assert(!BbMetalFX::CopyBuffers(src, src, {&same, 1}));
        assert(!BbMetalFX::CopyBuffers(src, dst, {&bounds, 1}));
        assert(!BbMetalFX::CopyBuffers(src, dst, {&alignment, 1}));
        assert(!BbMetalFX::CopyBuffers(src, dst, overlap));
    } else {
        assert(!source.buffer.metal && !destination.buffer.metal && !readback.buffer.metal);
    }
    for (size_t i = 0; i < source.SizeBytes(); ++i) source.mapped_data[i] = (i * 37 + 11) & 255;
    source.Flush(0, source.SizeBytes());
    std::vector<uint8_t> expected(4096, 0xa5);
    const vk::BufferCopy regions[]{{16, 64, 256}, {512, 1024, 512}, {2048, 3072, 1024}};
    for (const auto& c : regions)
        std::memcpy(expected.data() + c.dstOffset, source.mapped_data.data() + c.srcOffset, c.size);
    runtime.FillBuffer(&destination, 0, 4096, 0xa5a5a5a5);
    VideoCore::StreamBuffer pending(instance, scheduler, MemoryType::HostUncached, 4096);
    assert(pending.Reserve(4096));
    bool retired = false;
    scheduler.DeferOperation([&] { retired = true; });
    const auto tick = scheduler.CurrentTick();
    runtime.CopyBuffer(&source, &destination, regions);
    assert(scheduler.CurrentTick() == tick && !retired && !scheduler.IsFree(tick));
    assert(!pending.Reserve(4096, 0, false));
    const vk::BufferCopy full{0, 0, 4096};
    runtime.CopyBuffer(&destination, &readback, {&full, 1});
    scheduler.Finish();
    scheduler.PopPendingOperations();
    assert(retired);
    readback.Invalidate(0, 4096);
    assert(std::memcmp(expected.data(), readback.mapped_data.data(), 4096) == 0);
    // A completed fault callback can make more pages resident and upload their table.
    bool callback_copy = false;
    scheduler.DeferOperation([&] {
        runtime.CopyBuffer(&source, &destination, regions);
        callback_copy = true;
    });
    scheduler.Finish();
    scheduler.PopPendingOperations();
    assert(callback_copy);
    // Reverse the transfer direction after the native write has been acquired by Vulkan.
    runtime.FillBuffer(&destination, 0, 4096, 0x12345678);
    runtime.CopyBuffer(&destination, &source, {&full, 1});
    scheduler.Finish();
    source.Invalidate(0, 4096);
    const uint32_t value = 0x12345678;
    for (size_t i = 0; i < 4096; i += 4)
        assert(std::memcmp(source.mapped_data.data() + i, &value, 4) == 0);
    if (native) {
        // Native rejects byte alignment that Vulkan accepts. Immediately wrap/reuse the
        // staging ring after fallback; an unfinished fallback would read the new bytes.
        VideoCore::StreamBuffer staging(instance, scheduler, MemoryType::HostUncached, 4096);
        const auto [data, offset] = staging.Map(4096);
        std::memset(data, 0x3c, 4096);
        staging.Commit();
        runtime.FillBuffer(&destination, 0, 4096, 0xa5a5a5a5);
        const vk::BufferCopy byte_copy{offset + 1, 128, 15};
        runtime.CopyBuffer(&staging, &destination, {&byte_copy, 1});
        const auto [reused, next] = staging.Map(4096);
        assert(next == 0);
        std::memset(reused, 0xe7, 4096);
        staging.Commit();
        runtime.CopyBuffer(&destination, &readback, {&full, 1});
        scheduler.Finish();
        readback.Invalidate(0, 4096);
        for (size_t i = 0; i < 4096; ++i)
            assert(readback.mapped_data[i] == (i >= 128 && i < 143 ? 0x3c : 0xa5));
    }
    Common::SlotVector<VideoCore::ImageView> views;
    for (const auto format : {vk::Format::eR8Unorm, vk::Format::eR8G8Unorm,
            vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
            vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
            vk::Format::eR16Sfloat, vk::Format::eR16G16Sfloat, vk::Format::eR16G16B16A16Sfloat}) {
        VideoCore::ImageInfo info{};
        info.type = AmdGpu::ImageType::Color2DArray;
        info.pixel_format = format;
        info.size = {32, 16, 1};
        info.resources = {.levels = 3, .layers = 2};
        VideoCore::Image small(instance, runtime, views, info);
        info.size = {64, 32, 1};
        VideoCore::Image large(instance, runtime, views, info);
        assert(bool(small.backing->image.metal) == native && bool(large.backing->image.metal) == native);
        const auto image_handle = small.GetImage();
        const auto image_size = small.GetHostImageSize();
        VideoCore::UniqueImage moved(std::move(small.backing->image));
        assert(!small.GetImage() && moved.image == image_handle && moved.size_bytes == image_size);
        small.backing->image = std::move(moved);
        assert(!moved.image && small.GetImage() == image_handle && small.GetHostImageSize() == image_size);
        const auto bytes = vk::blockSize(format);
        std::vector<vk::BufferImageCopy> small_regions, large_regions;
        uint64_t small_size = 0, large_size = 0;
        for (uint32_t mip = 0; mip < 3; ++mip) {
            small_regions.push_back({.bufferOffset = small_size,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 2}, .imageExtent = {32u >> mip, 16u >> mip, 1}});
            large_regions.push_back({.bufferOffset = large_size,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 2}, .imageExtent = {64u >> mip, 32u >> mip, 1}});
            small_size += (32u >> mip) * (16u >> mip) * bytes * 2;
            large_size += (64u >> mip) * (32u >> mip) * bytes * 2;
        }
        Buffer upload(instance, 0, small_size + large_size, MemoryType::HostUncached);
        Buffer pixels(instance, 0, large_size, MemoryType::HostCached);
        for (size_t i = 0; i < small_size; ++i) upload.mapped_data[i] = (i * 37 + 11) & 255;
        std::memset(upload.mapped_data.data() + small_size, 0xa5, large_size);
        upload.Flush(0, upload.SizeBytes());
        for (auto& region : large_regions) region.bufferOffset += small_size;
        runtime.UploadImage(&small, &upload, small_regions);
        runtime.UploadImage(&large, &upload, large_regions);
        VideoCore::StreamBuffer held(instance, scheduler, MemoryType::HostUncached, 4096);
        assert(held.Reserve(4096));
        bool image_retired = false;
        scheduler.DeferOperation([&] { image_retired = true; });
        const auto image_tick = scheduler.CurrentTick();
        if (native) {
            const VkImageCopy valid{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {},
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {8, 8, 1}};
            auto invalid = valid;
            invalid.dstOffset.x = 64;
            auto* src = small.backing->image.metal->NativeHandle();
            auto* dst = large.backing->image.metal->NativeHandle();
            assert(!BbMetalFX::CopyImages(src, src, {&valid, 1}));
            assert(!BbMetalFX::CopyImages(src, dst, {&invalid, 1}));
            const VkImageCopy overlap[]{valid, valid};
            assert(!BbMetalFX::CopyImages(src, dst, overlap));
        }
        runtime.CopyImage(&small, &large);
        assert(scheduler.CurrentTick() == image_tick && !image_retired && !held.Reserve(4096, 0, false));
        for (auto& region : large_regions) region.bufferOffset -= small_size;
        runtime.DownloadImage(&large, &pixels, large_regions);
        scheduler.Finish();
        scheduler.PopPendingOperations();
        assert(image_retired);
        pixels.Invalidate(0, pixels.SizeBytes());
        std::vector<uint8_t> expected_pixels(large_size, 0xa5);
        for (uint32_t mip = 0; mip < 3; ++mip)
            for (uint32_t layer = 0; layer < 2; ++layer)
                for (uint32_t y = 0; y < (16u >> mip); ++y) {
                    const auto from = small_regions[mip].bufferOffset + (layer * (16u >> mip) + y) * (32u >> mip) * bytes;
                    const auto to = large_regions[mip].bufferOffset + (layer * (32u >> mip) + y) * (64u >> mip) * bytes;
                    std::memcpy(expected_pixels.data() + to, upload.mapped_data.data() + from, (32u >> mip) * bytes);
                }
        assert(std::memcmp(expected_pixels.data(), pixels.mapped_data.data(), large_size) == 0);
        if (format == vk::Format::eR8G8B8A8Unorm) {
            // Both images are shared, but Metal rejects a different pixel format.
            // The Vulkan bitwise-copy fallback must reacquire the right layouts.
            info.pixel_format = vk::Format::eR8G8B8A8Srgb;
            VideoCore::Image other_format(instance, runtime, views, info);
            assert(bool(other_format.backing->image.metal) == native);
            auto initialized = large_regions;
            for (auto& region : initialized) region.bufferOffset += small_size;
            runtime.UploadImage(&other_format, &upload, initialized);
            runtime.CopyImage(&small, &other_format);
            runtime.DownloadImage(&other_format, &pixels, large_regions);
            scheduler.Finish();
            pixels.Invalidate(0, pixels.SizeBytes());
            assert(std::memcmp(expected_pixels.data(), pixels.mapped_data.data(), large_size) == 0);
        }
    }
    VideoCore::ImageInfo depth_info{};
    depth_info.type = AmdGpu::ImageType::Color2D;
    depth_info.pixel_format = vk::Format::eD32Sfloat;
    depth_info.props.is_depth = 1;
    VideoCore::Image depth(instance, runtime, views, depth_info);
    assert(depth.GetImage() && !depth.backing->image.metal); // Unsupported format keeps VMA ownership.
    std::puts("PASS: nine color formats, image moves, three mips/two layers, untouched pixels, preserved staging tick, rejected copies, format-copy and depth fallback");
    std::puts(native ? "PASS: shared cache ownership, BDA, moves, 128 MiB stream, native copies, untouched bytes, preserved staging tick and callbacks" :
                      "PASS: original Vulkan allocation and copy fallback");
}
