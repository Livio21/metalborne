// SPDX-License-Identifier: GPL-2.0-or-later
// One headless check of the production cache/copy path; no game data required.
#include <cassert>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
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
    setenv("BB_METAL_IMAGE_TRANSFER", native ? "1" : "0", 1);
    setenv("BB_METAL_IMAGE_CLEAR", native ? "1" : "0", 1);
    unsetenv("BB_VK_RECORD_THREAD");
    Vulkan::Instance instance(0, false);
    // Match the existing scene check: executable headers need their own dispatcher.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
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
        VideoCore::StreamBuffer held(instance, scheduler, MemoryType::HostUncached, 4096);
        assert(held.Reserve(4096));
        bool image_retired = false;
        scheduler.DeferOperation([&] { image_retired = true; });
        const auto image_tick = scheduler.CurrentTick();
        runtime.UploadImage(&small, &upload, small_regions);
        runtime.UploadImage(&large, &upload, large_regions);
        assert(scheduler.CurrentTick() == image_tick && !image_retired && !held.Reserve(4096, 0, false));
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
            const auto good = static_cast<VkBufferImageCopy>(small_regions[0]);
            auto bad = good;
            bad.bufferOffset = upload.SizeBytes();
            void* buffer = upload.buffer.metal->NativeHandle();
            assert(!BbMetalFX::CopyBufferImage(buffer, src, {&bad, 1}, true));
            assert(!BbMetalFX::CopyBufferImage(buffer, src, {&bad, 1}, false));
            bad = good; bad.bufferRowLength = good.imageExtent.width - 1;
            assert(!BbMetalFX::CopyBufferImage(buffer, src, {&bad, 1}, true));
            bad = good; bad.imageSubresource.mipLevel = 99;
            const VkBufferImageCopy invalid_late[]{good, bad};
            assert(!BbMetalFX::CopyBufferImage(buffer, src, invalid_late, true));
            const VkBufferImageCopy duplicate[]{good, good};
            assert(!BbMetalFX::CopyBufferImage(buffer, src, duplicate, true));
            assert(!BbMetalFX::CopyBufferImage(buffer, src, duplicate, false));
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
        // Read independently through Vulkan: a matching pair of wrong native
        // upload/download mappings must not cancel out in this round trip.
        std::memset(pixels.mapped_data.data(), 0x6d, large_size);
        pixels.Flush(0, large_size);
        runtime.Transit(&large, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        scheduler.Record([image = large.GetImage(), buffer = pixels.Handle(),
                          regions = scheduler.RecordData(std::span<const vk::BufferImageCopy>{large_regions}), &dispatch](vk::CommandBuffer cmd) {
            cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, regions.size(), regions.data(), dispatch);
        });
        runtime.AccessBuffer(&pixels, 0, large_size, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        scheduler.Finish();
        pixels.Invalidate(0, large_size);
        assert(std::memcmp(expected_pixels.data(), pixels.mapped_data.data(), large_size) == 0);
        if (format == vk::Format::eR8G8B8A8Unorm) {
            Buffer padded(instance, 0, 4096, MemoryType::HostCached);
            std::memset(padded.mapped_data.data(), 0x6d, 4096);
            padded.Flush(0, 4096);
            const vk::BufferImageCopy partial{.bufferOffset = 20, .bufferRowLength = 19, .bufferImageHeight = 11,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 1, 0, 2},
                .imageOffset = {3, 2, 0}, .imageExtent = {7, 4, 1}};
            std::vector<uint8_t> expected_pad(4096, 0x6d);
            for (uint32_t layer = 0; layer < 2; ++layer)
                for (uint32_t y = 0; y < 4; ++y) {
                    const auto from = large_regions[1].bufferOffset + (layer * 16 + y + 2) * 32 * 4 + 3 * 4;
                    const auto to = 20 + (layer * 11 + y) * 19 * 4;
                    std::memcpy(expected_pad.data() + to, expected_pixels.data() + from, 7 * 4);
                }
            runtime.DownloadImage(&large, &padded, {&partial, 1});
            scheduler.Finish();
            padded.Invalidate(0, 4096);
            assert(std::memcmp(expected_pad.data(), padded.mapped_data.data(), 4096) == 0);
            runtime.UploadImage(&large, &padded, {&partial, 1});
            runtime.DownloadImage(&large, &pixels, large_regions);
            scheduler.Finish();
            pixels.Invalidate(0, large_size);
            assert(std::memcmp(expected_pixels.data(), pixels.mapped_data.data(), large_size) == 0);
            // Disjoint writes in padded rows have overlapping bounding spans:
            // the conservative native guard rejects them, so test reacquired Vulkan fallback.
            auto split = std::array{partial, partial};
            for (auto& c : split) { c.imageSubresource.baseArrayLayer = 1; c.imageSubresource.layerCount = 1; c.imageExtent.width = 3; }
            split[1].bufferOffset += 16;
            split[1].imageOffset.x += 3;
            std::fill(expected_pad.begin(), expected_pad.end(), 0x6d);
            std::memset(padded.mapped_data.data(), 0x6d, 4096);
            padded.Flush(0, 4096);
            for (const auto& c : split)
                for (uint32_t y = 0; y < 4; ++y) {
                    const auto from = large_regions[1].bufferOffset + (16 + y + 2) * 32 * 4 + c.imageOffset.x * 4;
                    std::memcpy(expected_pad.data() + c.bufferOffset + y * 19 * 4, expected_pixels.data() + from, 12);
                }
            runtime.DownloadImage(&large, &padded, split);
            scheduler.Finish();
            padded.Invalidate(0, 4096);
            assert(std::memcmp(expected_pad.data(), padded.mapped_data.data(), 4096) == 0);
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
    // Clear production shared attachments and compare every byte with Vulkan's clear.
    for (const auto format : {vk::Format::eR8Unorm, vk::Format::eR8G8Unorm,
            vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
            vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
            vk::Format::eR16Sfloat, vk::Format::eR16G16Sfloat, vk::Format::eR16G16B16A16Sfloat}) {
        VideoCore::ImageInfo info{};
        info.type = AmdGpu::ImageType::Color2DArray;
        info.pixel_format = format;
        info.size = {32, 16, 1};
        info.resources = {.levels = 3, .layers = 2};
        VideoCore::Image target(instance, runtime, views, info), reference(instance, runtime, views, info);
        assert(bool(target.backing->image.metal) == native && bool(reference.backing->image.metal) == native);
        const VideoCore::SubresourceRange all{.base = {0, 0}, .extent = info.resources};
        const VideoCore::SubresourceRange part{.base = {1, 1}, .extent = {.levels = 2, .layers = 1}};
        const vk::ClearValue base{.color = {.float32 = std::array{0.0f, 1.0f, 0.0f, 1.0f}}};
        const vk::ClearValue color{.color = {.float32 = std::array{0.125f, 0.5f, 1.5f, 0.25f}}};
        const auto vulkan_clear = [&](const VideoCore::SubresourceRange& range, const vk::ClearValue& value) {
            runtime.Transit(&reference, vk::ImageLayout::eTransferDstOptimal,
                            vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite, range);
            runtime.FlushBarriers();
            scheduler.Record([image = reference.GetImage(), range, value, &dispatch](vk::CommandBuffer cmd) {
                const vk::ImageSubresourceRange sub{vk::ImageAspectFlagBits::eColor,
                    range.base.level, range.extent.levels, range.base.layer, range.extent.layers};
                cmd.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, value.color, sub, dispatch);
            });
        };
        runtime.ClearImage(&target, all, base);
        vulkan_clear(all, base);
        VideoCore::StreamBuffer held(instance, scheduler, MemoryType::HostUncached, 4096);
        assert(held.Reserve(4096));
        bool retired = false;
        scheduler.DeferOperation([&] { retired = true; });
        const auto tick = scheduler.CurrentTick();
        runtime.ClearImage(&target, part, color);
        assert(scheduler.CurrentTick() == tick && !retired && !held.Reserve(4096, 0, false));
        vulkan_clear(part, color);
        if (native) {
            void* texture = target.backing->image.metal->NativeHandle();
            const VkImageSubresourceRange good{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            auto bad = good; bad.levelCount = 0;
            assert(!BbMetalFX::ClearImage(texture, bad, static_cast<VkClearColorValue>(color.color)));
            bad = good; bad.baseMipLevel = UINT32_MAX;
            assert(!BbMetalFX::ClearImage(texture, bad, static_cast<VkClearColorValue>(color.color)));
            bad = good; bad.layerCount = 3;
            assert(!BbMetalFX::ClearImage(texture, bad, static_cast<VkClearColorValue>(color.color)));
            bad = good; bad.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            assert(!BbMetalFX::ClearImage(texture, bad, static_cast<VkClearColorValue>(color.color)));
            auto nonfinite = static_cast<VkClearColorValue>(color.color);
            nonfinite.float32[0] = std::numeric_limits<float>::quiet_NaN();
            assert(!BbMetalFX::ClearImage(texture, good, nonfinite));
        }
        std::vector<vk::BufferImageCopy> regions;
        uint64_t size = 0;
        for (uint32_t mip = 0; mip < 3; ++mip) {
            regions.push_back({.bufferOffset = size, .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 2},
                              .imageExtent = {32u >> mip, 16u >> mip, 1}});
            size += (32u >> mip) * (16u >> mip) * vk::blockSize(format) * 2;
        }
        Buffer actual(instance, 0, size, MemoryType::HostCached), expected(instance, 0, size, MemoryType::HostCached);
        // Independent Vulkan readbacks avoid cancelling out a native texture mapping bug.
        for (const auto pair : {std::pair{&target, &actual}, std::pair{&reference, &expected}}) {
            runtime.Transit(pair.first, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            scheduler.Record([image = pair.first->GetImage(), buffer = pair.second->Handle(), regions, &dispatch](vk::CommandBuffer cmd) {
                cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, regions, dispatch);
            });
            runtime.AccessBuffer(pair.second, 0, size, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        }
        scheduler.Finish();
        scheduler.PopPendingOperations();
        assert(retired);
        actual.Invalidate(0, size); expected.Invalidate(0, size);
        assert(std::memcmp(actual.mapped_data.data(), expected.mapped_data.data(), size) == 0);
    }
    std::puts("PASS: nine-format render-pass clears match Vulkan bytes, selected mips/layers, untouched subresources and preserved staging/callbacks");
    VideoCore::ImageInfo depth_info{};
    depth_info.type = AmdGpu::ImageType::Color2D;
    depth_info.pixel_format = vk::Format::eD32Sfloat;
    depth_info.props.is_depth = 1;
    VideoCore::Image depth(instance, runtime, views, depth_info);
    assert(depth.GetImage() && !depth.backing->image.metal); // Unsupported format keeps VMA ownership.
    std::puts("PASS: nine color formats, native uploads/downloads, Vulkan reference, padded rows/layers, untouched bytes/pixels, preserved staging tick, rejected copies and depth fallback");
    std::puts(native ? "PASS: shared cache ownership, BDA, moves, 128 MiB stream, native copies, untouched bytes, preserved staging tick and callbacks" :
                      "PASS: original Vulkan allocation and copy fallback");
}
