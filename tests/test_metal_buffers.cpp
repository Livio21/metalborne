// SPDX-License-Identifier: GPL-2.0-or-later
// One headless check of the production cache/copy path; no game data required.
#include <cassert>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <future>
#include <vector>
#include <limits>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
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
    if(native) {
        const auto extensions=Vulkan::Check(instance.GetPhysicalDevice().enumerateDeviceExtensionProperties(nullptr,dispatch));
        const bool metal_objects=std::any_of(extensions.begin(),extensions.end(),[](const auto& ext) {
            return !std::strcmp(ext.extensionName,"VK_EXT_metal_objects");
        });
        std::printf("Driver shared-event API: VK_EXT_metal_objects=%u; host-signaled timeline bridge checked below\n",unsigned(metal_objects));
    }
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
    if (native) {
        // Asymmetric constant quadrants expose orientation, channel and extra sRGB conversion.
        u64 external_value = 0;
        constexpr std::array<std::array<uint8_t,4>,4> colors{{
            {17,63,149,255}, {215,90,33,255}, {32,185,67,255}, {179,48,201,255}}};
        for (const auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
                                 vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb}) {
            VideoCore::ImageInfo info{};
            info.type = AmdGpu::ImageType::Color2D; info.pixel_format = format;
            info.size = {64,32,1}; info.resources = {.levels=1,.layers=1};
            VideoCore::Image input(instance,runtime,views,info);
            Buffer pixels(instance,0,64*32*4,MemoryType::HostUncached);
            for (const u32 width : {96u,128u}) {
                auto output_info = info; output_info.size = {width,width/2,1};
                // The drawable/presentation frame is UNORM; scene UI also exercises sRGB at the other size.
                if(width==96) output_info.pixel_format=(format==vk::Format::eR8G8B8A8Unorm || format==vk::Format::eR8G8B8A8Srgb)
                    ? vk::Format::eR8G8B8A8Unorm : vk::Format::eB8G8R8A8Unorm;
                VideoCore::Image target(instance,runtime,views,output_info);
                assert(input.backing->image.metal && target.backing->image.metal);
                for (const u32 phase : {0u,1u}) {
                    for (u32 y=0;y<32;++y) for(u32 x=0;x<64;++x) {
                        const auto& color=colors[((y>=16)*2+(x>=32)+phase)%4];
                        std::memcpy(pixels.mapped_data.data()+(y*64+x)*4,color.data(),4);
                    }
                    pixels.Flush(0,pixels.SizeBytes());
                    const vk::BufferImageCopy upload{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                                     .imageExtent={64,32,1}};
                    runtime.UploadImage(&input,&pixels,{&upload,1});
                    for (auto* image : {&input,&target})
                        runtime.Transit(image,vk::ImageLayout::eGeneral,
                                        vk::PipelineStageFlagBits2::eAllCommands,
                                        vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite);
                    runtime.FlushBarriers();
                    const std::array handles{input.GetImage(),target.GetImage()};
                    const auto ownership=[&](bool release) {
                        scheduler.Record([handles,release,family=instance.GetGraphicsQueueFamilyIndex(),&dispatch](vk::CommandBuffer cmd) {
                            std::array<vk::ImageMemoryBarrier2,2> barriers;
                            for(size_t i=0;i<2;++i) barriers[i]={
                                .srcStageMask=release?vk::PipelineStageFlagBits2::eAllCommands:vk::PipelineStageFlagBits2::eNone,
                                .srcAccessMask=release?vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite:vk::AccessFlags2{},
                                .dstStageMask=release?vk::PipelineStageFlagBits2::eNone:vk::PipelineStageFlagBits2::eAllCommands,
                                .dstAccessMask=release?vk::AccessFlags2{}:vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                                .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                                .srcQueueFamilyIndex=release?family:VK_QUEUE_FAMILY_EXTERNAL,
                                .dstQueueFamilyIndex=release?VK_QUEUE_FAMILY_EXTERNAL:family,
                                .image=handles[i],.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()},dispatch);
                        });
                    };
                    auto src=input.backing->image.metal->NativeHandle(), dst=target.backing->image.metal->NativeHandle();
                    assert(!BbMetalFX::PrepareScene(src,src));
                    const bool asynchronous=phase==0 || width==128;
                    const bool linear_only=phase==1 && width==128;
                    std::future<BbMetalFX::SceneResult> task;
                    std::array<vk::UniqueSemaphore,2> other_waits;
                    std::promise<void> queued;
                    bool retired=false;
                    const auto tick=scheduler.CurrentTick();
                    scheduler.DeferOperation([&] { retired=true; });
                    ownership(true);
                    if(asynchronous) {
                        const vk::SemaphoreTypeCreateInfo timeline{.semaphoreType=vk::SemaphoreType::eTimeline};
                        for(auto& wait : other_waits)
                            wait=Vulkan::Check(instance.GetDevice().createSemaphoreUnique({.pNext=&timeline},nullptr,dispatch));
                        auto work=BbMetalFX::PrepareScene(src,dst,linear_only);
                        assert(work);
                        const auto semaphore=scheduler.ExternalSemaphore();
                        const auto value=++external_value;
                        auto fence=Vulkan::Check(instance.GetDevice().createFenceUnique({},nullptr,dispatch));
                        Vulkan::SubmitInfo release{}; release.fence=*fence;
                        scheduler.FlushForExternal(release,value);
                        assert(scheduler.CurrentTick()==tick);
                        task=std::async(std::launch::async,[work,semaphore,value,fence=std::move(fence),
                            gate=queued.get_future(),device=instance.GetDevice(),&dispatch]() mutable {
                            assert(device.waitForFences(*fence,true,UINT64_MAX,dispatch)==vk::Result::eSuccess);
                            const bool submitted=gate.wait_for(std::chrono::seconds(1))==std::future_status::ready;
                            const auto result=work();
                            assert(result.ready);
                            assert(device.signalSemaphore({.semaphore=semaphore,.value=value},dispatch)==vk::Result::eSuccess);
                            assert(submitted && "Driver blocked queue submission on a future host signal");
                            return result;
                        });
                    } else scheduler.FinishForExternal();
                    scheduler.PopPendingOperations();
                    assert(!retired && scheduler.CurrentTick()==tick);
                    float gpu_ms=0;
                    assert(!BbMetalFX::UpscaleScene(src,src,&gpu_ms) && std::isnan(gpu_ms));
                    assert(!BbMetalFX::UpscaleScene(dst,src)); // Equal/downscaled input stays Vulkan.
                    if(!asynchronous) {
                        assert(BbMetalFX::UpscaleScene(src,dst,&gpu_ms));
                        assert(std::isfinite(gpu_ms) && gpu_ms>=0);
                    }
                    const bool boxed=width==96 && phase==1;
                    const VkRect2D region{{8,4},{width-16,width/2-8}};
                    if(boxed) {
                        const auto srgb=(format==vk::Format::eR8G8B8A8Unorm || format==vk::Format::eR8G8B8A8Srgb)
                            ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
                        assert(!BbMetalFX::PostProcess(src,srgb,dst,1,false,{{-1,0},{width,width/2}}));
                        assert(!BbMetalFX::PostProcess(src,srgb,dst,1,false,{{1,0},{width,width/2}}));
                        assert(!BbMetalFX::PostProcess(src,srgb,dst,1,false,{{0,0},{width,0}}));
                        assert(BbMetalFX::PostProcess(src,srgb,dst,1,false,region));
                    }
                    ownership(false);
                    Buffer readback(instance,0,width*(width/2)*4,MemoryType::HostCached);
                    runtime.Transit(&target,vk::ImageLayout::eTransferSrcOptimal,
                                    vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead);
                    runtime.FlushBarriers();
                    const vk::BufferImageCopy download{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                                       .imageExtent={width,width/2,1}};
                    scheduler.Record([image=target.GetImage(),buffer=readback.Handle(),download,&dispatch](vk::CommandBuffer cmd) {
                        cmd.copyImageToBuffer(image,vk::ImageLayout::eTransferSrcOptimal,buffer,download,dispatch);
                    });
                    if(asynchronous) {
                        Vulkan::SubmitInfo ready{};
                        for(const auto& wait : other_waits) ready.AddWait(*wait,0);
                        scheduler.Flush(ready); // External completion is the third ALL_COMMANDS wait.
                        assert(!retired);
                        queued.set_value();
                        scheduler.Wait(tick);
                        const auto result=task.get();
                        assert(result.scaled!=linear_only);
                        assert(linear_only ? std::isnan(result.gpu_ms) : std::isfinite(result.gpu_ms));
                    } else scheduler.Finish();
                    scheduler.PopPendingOperations();
                    assert(retired);
                    readback.Invalidate(0,readback.SizeBytes());
                    if(linear_only) {
                        VideoCore::Image reference(instance,runtime,views,output_info);
                        Buffer expected(instance,0,readback.SizeBytes(),MemoryType::HostCached);
                        runtime.Transit(&input,vk::ImageLayout::eTransferSrcOptimal,vk::PipelineStageFlagBits2::eBlit,vk::AccessFlagBits2::eTransferRead);
                        runtime.Transit(&reference,vk::ImageLayout::eTransferDstOptimal,vk::PipelineStageFlagBits2::eBlit,vk::AccessFlagBits2::eTransferWrite);
                        runtime.FlushBarriers();
                        scheduler.Record([source=input.GetImage(),target=reference.GetImage(),width,&dispatch](vk::CommandBuffer cmd) {
                            const vk::ImageBlit blit{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                .srcOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{64,32,1}},
                                .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                .dstOffsets=std::array{vk::Offset3D{0,0,0},vk::Offset3D{s32(width),s32(width/2),1}}};
                            cmd.blitImage(source,vk::ImageLayout::eTransferSrcOptimal,target,vk::ImageLayout::eTransferDstOptimal,blit,vk::Filter::eLinear,dispatch);
                        });
                        runtime.Transit(&reference,vk::ImageLayout::eTransferSrcOptimal,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead);
                        runtime.FlushBarriers();
                        scheduler.Record([image=reference.GetImage(),buffer=expected.Handle(),download,&dispatch](vk::CommandBuffer cmd) {
                            cmd.copyImageToBuffer(image,vk::ImageLayout::eTransferSrcOptimal,buffer,download,dispatch);
                        });
                        scheduler.Finish(); expected.Invalidate(0,expected.SizeBytes());
                        for(size_t i=0;i<readback.SizeBytes();++i)
                            assert(std::abs(int(readback.mapped_data[i])-int(expected.mapped_data[i]))<=1);
                    }
                    for (u32 q=0;q<4;++q) {
                        const u32 x=boxed?8+region.extent.width*(q%2?3:1)/4:width*(q%2?3:1)/4;
                        const u32 y=boxed?4+region.extent.height*(q/2?3:1)/4:(width/2)*(q/2?3:1)/4;
                        const auto& expected=colors[(q+phase)%4];
                        for(size_t c=0;c<4;++c) {
                            const auto actual=readback.mapped_data[(y*width+x)*4+c];
                            if(std::abs(int(actual)-int(expected[c]))>1)
                                std::fprintf(stderr,"scene upscale mismatch: format %u phase %u quadrant %u channel %zu actual %u expected %u\n",
                                    unsigned(format),phase,q,c,actual,expected[c]);
                            assert(std::abs(int(actual)-int(expected[c]))<=1);
                        }
                    }
                    if(boxed) for(u32 y=0;y<width/2;++y) for(u32 x=0;x<width;++x) {
                        if(x>=8 && x<width-8 && y>=4 && y<width/2-4) continue;
                        for(size_t c=0;c<4;++c)
                            assert(readback.mapped_data[(y*width+x)*4+c]==(c==3?255:0));
                    }
                }
            }
        }
        std::puts("PASS: pre-HUD MetalFX, async host-signal ordering, preserved reservations/callbacks, four RGBA/BGRA/sRGB formats, forced linear fallback versus Vulkan, resize/color/alpha/GPU timing and letterboxed post-process");
    }
    if (native) {
        size_t compared = 0, rounding = 0;
        for (const auto source_format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
                vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
                vk::Format::eA2B10G10R10UnormPack32, vk::Format::eA2R10G10B10UnormPack32}) {
            for (const bool swap : {false,true}) {
                auto view_format = source_format;
                if (swap) switch (source_format) {
                case vk::Format::eR8G8B8A8Unorm: view_format=vk::Format::eB8G8R8A8Srgb; break;
                case vk::Format::eR8G8B8A8Srgb: view_format=vk::Format::eB8G8R8A8Unorm; break;
                case vk::Format::eB8G8R8A8Unorm: view_format=vk::Format::eR8G8B8A8Srgb; break;
                case vk::Format::eB8G8R8A8Srgb: view_format=vk::Format::eR8G8B8A8Unorm; break;
                case vk::Format::eA2B10G10R10UnormPack32: view_format=vk::Format::eA2R10G10B10UnormPack32; break;
                case vk::Format::eA2R10G10B10UnormPack32: view_format=vk::Format::eA2B10G10R10UnormPack32; break;
                default: assert(false);
                }
                for (const auto output_format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Unorm}) {
                    VideoCore::ImageInfo src_info{};
                    src_info.type = AmdGpu::ImageType::Color2D;
                    src_info.pixel_format = source_format;
                    src_info.size = {32,16,1};
                    src_info.resources = {.levels = 1, .layers = 1};
                    VideoCore::Image input(instance, runtime, views, src_info);
                    auto dst_info = src_info; dst_info.pixel_format = output_format; dst_info.size = {24,12,1};
                    VideoCore::Image target(instance, runtime, views, dst_info), reference(instance, runtime, views, dst_info);
                    assert(input.backing->image.metal && target.backing->image.metal && reference.backing->image.metal);
                    Buffer pixels(instance, 0, 32*16*4, MemoryType::HostUncached);
                    const bool packed = source_format == vk::Format::eA2B10G10R10UnormPack32 ||
                                        source_format == vk::Format::eA2R10G10B10UnormPack32;
                    auto snapshot_info = src_info;
                    if (!packed) snapshot_info.pixel_format = output_format;
                    VideoCore::Image snapshot(instance, runtime, views, snapshot_info);
                    assert(snapshot.backing->image.metal);
                    for (size_t i = 0; i < 32*16; ++i) {
                        // Asymmetric rows/channels reveal Y reversal, channel swaps and alpha handling.
                        if (packed) {
                            const uint32_t value = uint32_t((i*11)%1024) | uint32_t((i*7)%1024)<<10 |
                                                   uint32_t((i*17)%1024)<<20 | uint32_t(i%4)<<30;
                            std::memcpy(pixels.mapped_data.data()+i*4, &value, 4);
                        } else {
                            for (size_t c=0;c<4;++c) pixels.mapped_data[i*4+c] = (i*(c*6+7)+c*31)%256;
                        }
                    }
                    pixels.Flush(0, pixels.SizeBytes());
                    const vk::BufferImageCopy upload{.imageSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
                                                     .imageExtent = {32,16,1}};
                    runtime.UploadImage(&input, &pixels, {&upload,1});
                    // The presentation thread reads a byte-preserving Vulkan snapshot, including
                    // RGBA/BGRA and sRGB/UNORM reinterpretation, never the mutable guest image.
                    runtime.Transit(&input, vk::ImageLayout::eGeneral,
                                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
                    runtime.Transit(&snapshot, vk::ImageLayout::eGeneral,
                                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
                    runtime.FlushBarriers();
                    scheduler.Record([src=input.GetImage(),dst=snapshot.GetImage(),&dispatch](vk::CommandBuffer cmd) {
                        const vk::ImageCopy copy{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                            .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.extent={32,16,1}};
                        cmd.copyImage(src,vk::ImageLayout::eGeneral,dst,vk::ImageLayout::eGeneral,copy,dispatch);
                    });
                    VideoCore::ImageViewInfo src_view_info{}; src_view_info.format = view_format;
                    src_view_info.mapping.a = vk::ComponentSwizzle::eOne;
                    const auto src_view = *input.FindView(src_view_info).image_view;
                    VideoCore::ImageViewInfo dst_view_info{}; dst_view_info.format = output_format;
                    Vulkan::Frame frame{}; frame.width=24; frame.height=12; frame.image=reference.GetImage();
                    frame.image_view=*reference.FindView(dst_view_info).image_view;
                    Vulkan::HostPasses::PostProcessingPass pass;
                    pass.Create(instance.GetDevice(), output_format);
                    for (const float gamma : {0.8f,1.0f,1.2f}) {
                        for (const bool srgb_input : {false,true}) {
                            runtime.Transit(&snapshot, vk::ImageLayout::eGeneral,
                                            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead);
                            runtime.Transit(&target, vk::ImageLayout::eGeneral,
                                            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite);
                            runtime.FlushBarriers();
                            const std::array handles{snapshot.GetImage(),target.GetImage()};
                            const auto ownership = [&](bool release) {
                                scheduler.Record([handles,release,family=instance.GetGraphicsQueueFamilyIndex(),&dispatch](vk::CommandBuffer cmd) {
                                    std::array<vk::ImageMemoryBarrier2,2> barriers;
                                    for(size_t i=0;i<2;++i) barriers[i]={
                                        .srcStageMask=release?vk::PipelineStageFlagBits2::eAllCommands:vk::PipelineStageFlagBits2::eNone,
                                        .srcAccessMask=release?vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite:vk::AccessFlags2{},
                                        .dstStageMask=release?vk::PipelineStageFlagBits2::eNone:vk::PipelineStageFlagBits2::eAllCommands,
                                        .dstAccessMask=release?vk::AccessFlags2{}:vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                                        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                                        .srcQueueFamilyIndex=release?family:VK_QUEUE_FAMILY_EXTERNAL,
                                        .dstQueueFamilyIndex=release?VK_QUEUE_FAMILY_EXTERNAL:family,
                                        .image=handles[i],.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                                    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()},dispatch);
                                });
                            };
                            ownership(true); scheduler.FinishForExternal();
                            assert(BbMetalFX::PostProcess(snapshot.backing->image.metal->NativeHandle(),static_cast<VkFormat>(view_format),
                                                         target.backing->image.metal->NativeHandle(),gamma,srgb_input));
                            assert(!BbMetalFX::PostProcess(snapshot.backing->image.metal->NativeHandle(),static_cast<VkFormat>(source_format),
                                                          target.backing->image.metal->NativeHandle(),0,srgb_input));
                            assert(!BbMetalFX::PostProcess(snapshot.backing->image.metal->NativeHandle(),VK_FORMAT_D32_SFLOAT,
                                                          target.backing->image.metal->NativeHandle(),gamma,srgb_input));
                            ownership(false);
                            runtime.Transit(&input,vk::ImageLayout::eShaderReadOnlyOptimal,
                                            vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderRead);
                            runtime.Transit(&reference,vk::ImageLayout::eColorAttachmentOptimal,
                                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentWrite);
                            runtime.FlushBarriers();
                            scheduler.Record([&](vk::CommandBuffer cmd) {
                                pass.Render(cmd,src_view,{32,16},frame,{gamma,0,uint32_t(srgb_input)});
                            });
                            // The pass ends in GENERAL; update the test image tracker to that state.
                            reference.backing->state.layout=vk::ImageLayout::eGeneral;
                            Buffer actual(instance,0,24*12*4,MemoryType::HostCached),expected(instance,0,24*12*4,MemoryType::HostCached);
                            const vk::BufferImageCopy download{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                                               .imageExtent={24,12,1}};
                            for(const auto pair:{std::pair{&target,&actual},std::pair{&reference,&expected}}) {
                                runtime.Transit(pair.first,vk::ImageLayout::eTransferSrcOptimal,
                                                vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead);
                                runtime.FlushBarriers();
                                scheduler.Record([image=pair.first->GetImage(),buffer=pair.second->Handle(),download,&dispatch](vk::CommandBuffer cmd) {
                                    cmd.copyImageToBuffer(image,vk::ImageLayout::eTransferSrcOptimal,buffer,download,dispatch);
                                });
                            }
                            scheduler.Finish(); actual.Invalidate(0,actual.SizeBytes()); expected.Invalidate(0,expected.SizeBytes());
                            for(size_t i=0;i<actual.SizeBytes();++i) {
                                const int delta=std::abs(int(actual.mapped_data[i])-int(expected.mapped_data[i]));
                                if(delta>1) std::fprintf(stderr,"post-process mismatch: src %u dst %u gamma %.1f decode %u byte %zu actual %u expected %u\n",
                                    unsigned(source_format),unsigned(output_format),gamma,unsigned(srgb_input),i,actual.mapped_data[i],expected.mapped_data[i]);
                                assert(delta<=1); rounding+=delta!=0; ++compared;
                                if(i%4==3) assert(actual.mapped_data[i]==255);
                            }
                        }
                    }
                }
            }
        }
        std::printf("PASS: native post-process from Vulkan snapshots, 144 format/view/gamma/decode combinations; %zu bytes compared, %zu one-LSB rounding differences\n",compared,rounding);
    }
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
