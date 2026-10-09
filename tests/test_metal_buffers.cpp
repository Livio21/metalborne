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
#include <filesystem>
#include <fstream>
#include <map>
#include "macos_metal_shader.h"
#include "bbport_copy.h"
#include "shader_recompiler/resource.h"
#include <vector>
#include <limits>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "metal_draw_vert.h"
#include "metal_draw_frag.h"
#include "metal_flat_frag.h"
#include <vulkan/vulkan_format_traits.hpp>

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--shaders")) {
        std::map<uint32_t, std::pair<unsigned, unsigned>> counts;
        for (const auto& file : std::filesystem::directory_iterator(argv[2])) {
            if (file.path().extension() != ".spv") continue;
            std::ifstream input(file.path(), std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            assert(size >= 20 && size <= 16 * 1024 * 1024 && size % 4 == 0);
            std::vector<uint32_t> code(size / 4);
            input.seekg(0);
            assert(input.read(reinterpret_cast<char*>(code.data()), size));
            BbMetalFX::ShaderFunction shader(code);
            auto& count = counts[shader.Stage()];
            bool ready = shader.Available();
            if (!shader.Available()) std::fprintf(stderr, "Metal shader rejected %s: %.*s\n",
                file.path().filename().c_str(), int(shader.Error().size()), shader.Error().data());
            if (ready && shader.Stage() == 5) {
                BbMetalFX::ComputeKernel kernel(code);
                ready = kernel.Available();
                if (!ready) std::fprintf(stderr, "Metal kernel rejected %s: %.*s\n",
                    file.path().filename().c_str(), int(kernel.Error().size()), kernel.Error().data());
            }
            ++(ready ? count.first : count.second);
        }
        unsigned total = 0, failed = 0;
        for (const auto& [stage, count] : counts) {
            std::printf("Metal shader stage %u: %u compiled, %u rejected\n", stage, count.first, count.second);
            total += count.first + count.second; failed += count.second;
        }
        return total && !failed ? 0 : 1;
    }
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
    {
        unsigned idle_copy_flushes = 0;
        scheduler.SetPendingExternal([&] { ++idle_copy_flushes; });
        scheduler.WaitHostCopies();
        assert(idle_copy_flushes == 0);
        assert(!BbCopy::HasPending());
        scheduler.FlushPendingExternal();
        assert(idle_copy_flushes == 1);
        if (BbCopy::Enabled()) {
            BbCopy::QueueCopy({.run = [](const BbCopy::Item&) {}, .size = 1});
            assert(BbCopy::HasPending());
            unsigned queued_copy_flushes = 0;
            scheduler.SetPendingExternal([&] { ++queued_copy_flushes; });
            scheduler.WaitHostCopies();
            assert(queued_copy_flushes == 1 && !BbCopy::HasPending());
            std::puts("PASS: a queued copy-pool item flushes the native batch before WaitHostCopies waits");

            std::promise<void> started, release;
            auto release_copy = release.get_future().share();
            BbCopy::Async([&] { started.set_value(); release_copy.wait(); });
            started.get_future().wait();
            unsigned active_copy_flushes = 0;
            scheduler.SetPendingExternal([&] { ++active_copy_flushes; release.set_value(); });
            scheduler.WaitHostCopies();
            assert(active_copy_flushes == 1 && !BbCopy::HasPending());
            std::puts("PASS: an active copy-pool task flushes the native batch before WaitHostCopies waits");
        }
        unsigned pending_flushes = 0;
        const auto pending = [&] {
            scheduler.SetPendingExternal([&] {
                ++pending_flushes;
                scheduler.FinishForExternal(); // Re-enters recording and submission without the hook.
            });
        };
        const auto pending_tick = scheduler.CurrentTick();
        pending();
        scheduler.RecordHostCopy([] {}); // The flush must precede host_copies_issued.
        scheduler.WaitHostCopies();
        assert(pending_flushes == 1);
        pending();
        const std::array<u32, 4> payload{3, 5, 7, 11};
        const auto captured = scheduler.RecordData(std::span<const u32>(payload));
        assert(pending_flushes == 2); // Flush before allocating capture storage, not inside Record.
        scheduler.Record([captured](vk::CommandBuffer) { assert(captured[3] == 11); });
        scheduler.KickRecording(true);
        scheduler.SyncRecording();
        u64 nested_value = 0;
        scheduler.SetPendingExternal([&] { nested_value = scheduler.NextExternalValue(); });
        const auto following_value = scheduler.NextExternalValue();
        assert(nested_value && following_value > nested_value);
        pending(); scheduler.EndRendering(); assert(pending_flushes == 3);
        pending(); scheduler.CommandBuffer(); assert(pending_flushes == 4);
        pending(); scheduler.WaitDeferredSignals(); assert(pending_flushes == 5);
        pending(); scheduler.FinishForExternal(); assert(pending_flushes == 6);
        assert(scheduler.CurrentTick() == pending_tick);
        std::puts("PASS: idle host-copy waits retain native batches; pending work flushes before recording storage, copy counters, guest signals and external timeline allocation without retiring the guest tick");
    }
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
    {
        Buffer arena(instance, 0x100000000, 131072, MemoryType::Sparse);
        Buffer alias(instance, arena.cpu_addr, arena.SizeBytes(), MemoryType::Sparse);
        const auto generation = runtime.SparseBufferGeneration();
        runtime.AccessBuffer(&arena, 0, 4096, vk::PipelineStageFlagBits2::eVertexShader, vk::AccessFlagBits2::eShaderRead);
        runtime.AccessBuffer(&destination, 0, 4096, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        assert(runtime.SparseBufferGeneration() == generation);
        const auto untouched = runtime.SparseBufferGeneration(arena.cpu_addr + 65536, 4096);
        for (const auto write : {vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderWrite,
                                vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransformFeedbackWriteEXT}) {
            const auto before = runtime.SparseBufferGeneration();
            runtime.AccessBuffer(&arena, 16, 16, vk::PipelineStageFlagBits2::eAllCommands, write);
            assert(runtime.SparseBufferGeneration() == before + 1);
            // Repeated writes and a merged arena alias must also invalidate cached bytes.
            runtime.AccessBuffer(&alias, 16, 16, vk::PipelineStageFlagBits2::eAllCommands, write);
            assert(runtime.SparseBufferGeneration() == before + 2);
            assert(runtime.SparseBufferGeneration(arena.cpu_addr, 4096) == before + 2);
            assert(runtime.SparseBufferGeneration(arena.cpu_addr + 65536, 4096) == untouched);
        }
        auto before = runtime.SparseBufferGeneration();
        runtime.InvalidateSparseBuffers(arena.cpu_addr + 65532, 8);
        assert(runtime.SparseBufferGeneration(arena.cpu_addr, 4) == before + 1);
        assert(runtime.SparseBufferGeneration(arena.cpu_addr + 65536, 4) == before + 1);
        runtime.InvalidateSparseBuffers(arena.cpu_addr + 4096 * 65536, 4);
        assert(runtime.SparseBufferGeneration(arena.cpu_addr, 4) == before + 2); // A hash collision safely refreshes extra data.
        runtime.InvalidateSparseBuffers(arena.cpu_addr, 4097ULL * 65536);
        assert(runtime.SparseBufferGeneration(arena.cpu_addr + 4095 * 65536, 65536) == before + 3);
        runtime.InvalidateSparseBuffers();
        assert(runtime.SparseBufferGeneration() == before + 4);
        assert(runtime.SparseBufferGeneration(arena.cpu_addr + 12345ULL * 65536, 4) == before + 4);
        runtime.FlushBarriers(); scheduler.FinishForExternal();
        std::puts("PASS: sparse mirror page versions, read/disjoint-range preservation, upload/shader/memory/transform-feedback writes, repeated writes, arena aliases, page crossings, hash collisions, full-table writes and unbounded DMA invalidation");
    }
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
    if (argc == 3 && !std::strcmp(argv[1], "--compute")) {
        std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
        const auto bytes = input.tellg();
        assert(bytes >= 20 && bytes <= 16 * 1024 * 1024 && bytes % 4 == 0);
        std::vector<uint32_t> code(bytes / 4);
        input.seekg(0);
        assert(input.read(reinterpret_cast<char*>(code.data()), bytes));
        BbMetalFX::ComputeKernel kernel(code);
        assert(kernel.Available());
        assert((kernel.WorkgroupSize() == std::array<uint32_t, 3>{64, 1, 1}));
        assert(kernel.Resources().size() == 2);
        for (uint32_t i = 0; i < 1024; ++i) {
            const float value = float(i) + 0.25f;
            std::memcpy(source.mapped_data.data() + i * 4, &value, 4);
        }
        Shader::PushData push{};
        push.ud_regs[0] = 500;
        push.buf_offsets[0] = 16;
        push.buf_offsets[1] = 20;
        const std::array<BbMetalFX::ShaderBinding, 2> bindings{{
            {BbMetalFX::ShaderResourceKind::Buffer, 0, 0, source.buffer.metal->NativeHandle(), 256, 3000, false},
            {BbMetalFX::ShaderResourceKind::Buffer, 1, 0, destination.buffer.metal->NativeHandle(), 512, 3000, true}}};
        const auto push_bytes = std::span{reinterpret_cast<const uint8_t*>(&push), sizeof(push)};
        auto invalid = bindings;
        invalid[1].offset = 4096;
        assert(kernel.Dispatch(invalid, push_bytes, {8, 1, 1}) == BbMetalFX::CommandResult::Unavailable);
        assert(kernel.Dispatch(bindings, {}, {8, 1, 1}) == BbMetalFX::CommandResult::Unavailable);
        runtime.FillBuffer(&destination, 0, 4096, 0xa5a5a5a5);
        const std::array<vk::Buffer, 2> handles{source.Handle(), destination.Handle()};
        const auto ownership = [&](bool release) {
            scheduler.Record([handles, family = instance.GetGraphicsQueueFamilyIndex(), release, dispatch](vk::CommandBuffer command) {
                std::array<vk::BufferMemoryBarrier2, 2> barriers;
                for (size_t i = 0; i < handles.size(); ++i) barriers[i] = {
                    .srcStageMask = release ? vk::PipelineStageFlagBits2::eAllCommands : vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask = release ? vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite : vk::AccessFlags2{},
                    .dstStageMask = release ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask = release ? vk::AccessFlags2{} : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .srcQueueFamilyIndex = release ? family : VK_QUEUE_FAMILY_EXTERNAL,
                    .dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : family,
                    .buffer = handles[i], .offset = 0, .size = VK_WHOLE_SIZE};
                command.pipelineBarrier2(vk::DependencyInfo{
                    .bufferMemoryBarrierCount = 2, .pBufferMemoryBarriers = barriers.data()}, dispatch);
            });
        };
        bool retired = false;
        scheduler.DeferOperation([&] { retired = true; });
        const auto tick = scheduler.CurrentTick();
        ownership(true);
        scheduler.FinishForExternal();
        assert(!retired && scheduler.CurrentTick() == tick);
        assert(kernel.Dispatch(bindings, push_bytes, {8, 1, 1}) == BbMetalFX::CommandResult::Complete);
        ownership(false);
        const vk::BufferCopy full{0, 0, 4096};
        runtime.CopyBuffer(&destination, &readback, {&full, 1});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        assert(retired);
        readback.Invalidate(0, 4096);
        for (size_t i = 0; i < 4096; ++i)
            assert(readback.mapped_data[i] == (i >= 532 && i < 2532 ? source.mapped_data[i - 260] : 0xa5));
        std::printf("Native guest kernel: push/argument offsets, bounded writes, guard bytes and guest completion passed\n");
        return 0;
    }
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
    assert(depth.GetImage() && bool(depth.backing->image.metal) == native);
    if (native) {
        VideoCore::ImageInfo texture_info{};
        texture_info.type = AmdGpu::ImageType::Color2D;
        texture_info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        texture_info.size = {8, 8, 1}; texture_info.resources = {3, 2};
        VideoCore::Image texture(instance, runtime, views, texture_info);
        VideoCore::ImageInfo output_info{};
        output_info.type = AmdGpu::ImageType::Color2D;
        output_info.pixel_format = vk::Format::eR8G8B8A8Unorm; output_info.size = {32, 32, 1};
        VideoCore::Image output(instance, runtime, views, output_info), reference(instance, runtime, views, output_info);
        depth_info.size = {32, 32, 1};
        VideoCore::Image target_depth(instance, runtime, views, depth_info);
        VideoCore::ImageViewInfo sampled_info;
        sampled_info.range = {{1, 1}, {2, 1}};
        sampled_info.mapping = {vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eOne};
        VideoCore::ImageView sampled_view(instance, sampled_info, texture);
        VideoCore::ImageViewInfo target_info;
        target_info.range = {{0, 0}, {1, 1}};
        VideoCore::ImageView output_view(instance, target_info, output), reference_view(instance, target_info, reference);
        target_info.format = vk::Format::eD32Sfloat;
        VideoCore::ImageView depth_view(instance, target_info, target_depth);
        assert(BbMetalFX::FindNativeImageView(*sampled_view.image_view).sampled);
        assert(BbMetalFX::FindNativeImageView(*output_view.image_view).attachment);
        assert(BbMetalFX::FindNativeImageView(*depth_view.image_view).attachment);
        const vk::SamplerCreateInfo sampler_ci{.magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest, .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge, .addressModeW = vk::SamplerAddressMode::eClampToEdge,
            .mipLodBias = 0.25f, .maxLod = 1};
        auto sampler = Vulkan::Check(instance.GetDevice().createSamplerUnique(sampler_ci, nullptr, dispatch));
        BbMetalFX::Sampler native_sampler(*sampler, static_cast<VkSamplerCreateInfo>(sampler_ci));
        assert(BbMetalFX::FindNativeSampler(*sampler));
        Buffer upload(instance, 0, 1024, MemoryType::HostUncached), actual(instance, 0, 4096, MemoryType::HostCached),
            expected(instance, 0, 4096, MemoryType::HostCached), vertex_data(instance, 0, 512, MemoryType::HostUncached),
            index_data(instance, 0, 64, MemoryType::HostUncached);
        Buffer vertex_mirror(instance, 0, 512, MemoryType::DeviceLocal), native_written(instance, 0, 512, MemoryType::DeviceLocal),
            mirror_readback(instance, 0, 512, MemoryType::HostCached);
        const auto refresh_mirror = [&] {
            scheduler.Record([source = vertex_data.Handle(), mirror = vertex_mirror.Handle(), &dispatch](vk::CommandBuffer command) {
                const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite, .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
                command.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before}, dispatch);
                command.fillBuffer(mirror, 0, 512, 0xcccccccc, dispatch);
                command.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before}, dispatch);
                command.copyBuffer(source, mirror, vk::BufferCopy{0, 0, 512}, dispatch);
            });
        };
        std::vector<vk::BufferImageCopy> regions;
        u64 offset = 0;
        for (u32 mip = 0; mip < 3; ++mip) {
            const u32 side = 8 >> mip;
            regions.push_back({.bufferOffset = offset, .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 2},
                .imageExtent = {side, side, 1}});
            for (u32 layer = 0; layer < 2; ++layer) for (u32 y = 0; y < side; ++y) for (u32 x = 0; x < side; ++x) {
                auto* pixel = upload.mapped_data.data() + offset + ((layer * side + y) * side + x) * 4;
                pixel[0] = 35 + mip * 40 + x * 6; pixel[1] = 70 + layer * 50 + y * 7; pixel[2] = 220 - x * 10; pixel[3] = 255;
            }
            offset += side * side * 4 * 2;
        }
        upload.Flush(0, offset);
        runtime.UploadImage(&texture, &upload, regions);
        const std::array<std::array<float, 4>, 5> points{{{99, 99, .5f, 1}, {-1, -1, .5f, 1}, {1, -1, .5f, 1},
            {1, 1, .5f, 1}, {-1, 1, .5f, 1}}};
        std::memcpy(vertex_data.mapped_data.data(), points.data(), sizeof(points)); vertex_data.Flush(0, 512);
        const u16 indices[]{0xffff, 0xffff, 0, 1, 2, 2, 3, 0};
        std::memcpy(index_data.mapped_data.data(), indices, sizeof(indices)); index_data.Flush(0, 64);
        const vk::DescriptorSetLayoutBinding descriptor_bindings[]{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment},
            {.binding = 1, .descriptorType = vk::DescriptorType::eSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment}};
        auto descriptor_layout = Vulkan::Check(instance.GetDevice().createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR, .bindingCount = 2, .pBindings = descriptor_bindings}, nullptr, dispatch));
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eFragment, 0, 16};
        const vk::DescriptorSetLayout set_layout = *descriptor_layout;
        auto layout = Vulkan::Check(instance.GetDevice().createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &set_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range}, nullptr, dispatch));
        const auto vs = Vulkan::CompileSPV(METAL_DRAW_VERT, instance.GetDevice()), fs = Vulkan::CompileSPV(METAL_DRAW_FRAG, instance.GetDevice());
        const vk::PipelineShaderStageCreateInfo stages[]{
            {.stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"},
            {.stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main"}};
        const vk::Format color_format = output_info.pixel_format;
        const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
            .depthAttachmentFormat = vk::Format::eD32Sfloat};
        const vk::VertexInputBindingDescription vertex_binding{0, 16, vk::VertexInputRate::eVertex};
        const vk::VertexInputAttributeDescription attribute{0, 0, vk::Format::eR32G32B32Sfloat, 0};
        const vk::PipelineVertexInputStateCreateInfo vertex_input{.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vertex_binding,
            .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
        const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill, .frontFace = vk::FrontFace::eCounterClockwise, .lineWidth = 1};
        const vk::PipelineViewportStateCreateInfo viewport_info{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineMultisampleStateCreateInfo samples{.rasterizationSamples = vk::SampleCountFlagBits::e1};
        const auto mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        const vk::PipelineColorBlendAttachmentState blend{.blendEnable = true, .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha, .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne, .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd, .colorWriteMask = mask};
        const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount = 1, .pAttachments = &blend};
        const vk::PipelineDepthStencilStateCreateInfo depth_state{.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vk::CompareOp::eLess};
        const vk::DynamicState dynamic_items[]{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic_ci{.dynamicStateCount = 2, .pDynamicStates = dynamic_items};
        const vk::GraphicsPipelineCreateInfo pipeline_info{.pNext = &rendering, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &viewport_info,
            .pRasterizationState = &raster, .pMultisampleState = &samples, .pDepthStencilState = &depth_state,
            .pColorBlendState = &blending, .pDynamicState = &dynamic_ci, .layout = *layout};
        auto vk_pipeline = Vulkan::Check(instance.GetDevice().createGraphicsPipelineUnique({}, pipeline_info, nullptr, dispatch));
        BbMetalFX::RenderPipeline metal_pipeline(METAL_DRAW_VERT, METAL_DRAW_FRAG, static_cast<VkGraphicsPipelineCreateInfo>(pipeline_info));
        assert(metal_pipeline.Available());
        Vulkan::DynamicState dynamic;
        dynamic.viewports.push_back({4, 5, 16, 14, 0, 1}); dynamic.scissors.push_back({{7, 8}, {10, 9}});
        dynamic.line_width = 1; dynamic.front_face = raster.frontFace; dynamic.color_write_masks[0] = mask;
        dynamic.depth_test_enabled = true; dynamic.depth_write_enabled = true; dynamic.depth_compare_op = vk::CompareOp::eLess;
        Vulkan::RenderState state{};
        state.width = state.height = 32; state.num_layers = state.num_color_attachments = 1;
        state.color_attachments[0].image_view = *output_view.image_view;
        state.color_attachments[0].image_layout = vk::ImageLayout::eGeneral; state.color_attachments[0].is_clear = true;
        const std::array<float, 4> clear{.05f, .1f, .2f, 1};
        std::memcpy(state.color_attachments[0].clear_value.data(), clear.data(), sizeof(clear));
        state.depth_stencil_attachment.image_view = *depth_view.image_view; state.depth_stencil_attachment.image_layout = vk::ImageLayout::eGeneral;
        state.depth_stencil_attachment.has_depth = state.depth_stencil_attachment.depth_clear = true;
        state.depth_stencil_attachment.clear_value[0] = std::bit_cast<u32>(1.f);
        const std::array<float, 4> tint{1, .5f, .25f, .75f};
        const std::span<const u8> push{reinterpret_cast<const u8*>(tint.data()), sizeof(tint)};
        const BbMetalFX::ShaderBinding shader_bindings[]{
            {BbMetalFX::ShaderResourceKind::Texture, 0, 0, BbMetalFX::FindNativeImageView(*sampled_view.image_view).sampled, 0, 0, false},
            {BbMetalFX::ShaderResourceKind::Sampler, 1, 0, BbMetalFX::FindNativeSampler(*sampler), 0, 0, false}};
        const VkVertexInputAttributeDescription2EXT native_attribute{.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
        BbMetalFX::VertexBufferBinding native_vertex{{.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1},
            vertex_mirror.buffer.metal->NativeHandle(), 0, 512};
        const BbMetalFX::DrawCommand draw{6, 1, 0, 1, index_data.buffer.metal->NativeHandle(), 4, VK_INDEX_TYPE_UINT16};
        assert(metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings, {}, draw) == BbMetalFX::CommandResult::Unavailable);
        {
            auto info = static_cast<VkGraphicsPipelineCreateInfo>(pipeline_info);
            auto unsupported_samples = static_cast<VkPipelineMultisampleStateCreateInfo>(samples);
            info.pMultisampleState = &unsupported_samples;
            uint32_t sample_mask = 0;
            unsupported_samples.pSampleMask = &sample_mask;
            assert(!BbMetalFX::RenderPipeline(METAL_DRAW_VERT, METAL_DRAW_FRAG, info).Available());
            sample_mask = 1; // Only the active sample bit matters.
            assert(BbMetalFX::RenderPipeline(METAL_DRAW_VERT, METAL_DRAW_FRAG, info).Available());
            unsupported_samples.sampleShadingEnable = true;
            assert(!BbMetalFX::RenderPipeline(METAL_DRAW_VERT, METAL_DRAW_FRAG, info).Available());
            auto strip = static_cast<VkPipelineInputAssemblyStateCreateInfo>(assembly);
            strip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            unsupported_samples.sampleShadingEnable = false;
            info.pInputAssemblyState = &strip;
            BbMetalFX::RenderPipeline strip_pipeline(METAL_DRAW_VERT, METAL_DRAW_FRAG, info);
            assert(strip_pipeline.Available());
            assert(strip_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings, push, draw) == BbMetalFX::CommandResult::Unavailable);
            const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
                .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
            auto flat_raster = static_cast<VkPipelineRasterizationStateCreateInfo>(raster);
            flat_raster.pNext = &provoking;
            info.pRasterizationState = &flat_raster;
            BbMetalFX::RenderPipeline flat_pipeline(METAL_DRAW_VERT, METAL_FLAT_FRAG, info);
            assert(!flat_pipeline.Available());
            assert(flat_pipeline.Error() == "Flat inputs require last-vertex index expansion");
            std::puts("PASS: unsupported sample mask/minimum sample shading, indexed strip restart-disabled and flat struct-member last-vertex states rejected before submission");
        }
        const auto ownership = [&](bool release, Vulkan::SubmitInfo* external = nullptr, u64 value = 0) {
            std::vector<vk::ImageMemoryBarrier2> images;
            for (auto* image : {&texture, &output, &target_depth}) {
                if (release) runtime.Transit(image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
                images.push_back({.srcStageMask = release ? vk::PipelineStageFlagBits2::eAllCommands : vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask = release ? vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite : vk::AccessFlags2{},
                    .dstStageMask = release ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask = release ? vk::AccessFlags2{} : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = release ? instance.GetGraphicsQueueFamilyIndex() : VK_QUEUE_FAMILY_EXTERNAL,
                    .dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : instance.GetGraphicsQueueFamilyIndex(),
                    .image = image->GetImage(), .subresourceRange = {image->aspect_mask, 0, image->info.resources.levels, 0, image->info.resources.layers}});
            }
            runtime.FlushBarriers();
            scheduler.Record([images, &dispatch](vk::CommandBuffer command) {
                command.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = u32(images.size()), .pImageMemoryBarriers = images.data()}, dispatch);
            });
            const std::array handles{vertex_mirror.Handle(), index_data.Handle(), native_written.Handle()};
            scheduler.Record([handles, release, family = instance.GetGraphicsQueueFamilyIndex(), &dispatch](vk::CommandBuffer command) {
                std::array<vk::BufferMemoryBarrier2, 3> barriers;
                for (size_t i = 0; i < handles.size(); ++i) barriers[i] = {
                    .srcStageMask = release ? vk::PipelineStageFlagBits2::eAllCommands : vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask = release ? vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite : vk::AccessFlags2{},
                    .dstStageMask = release ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask = release ? vk::AccessFlags2{} : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .srcQueueFamilyIndex = release ? family : VK_QUEUE_FAMILY_EXTERNAL,
                    .dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : family,
                    .buffer = handles[i], .offset = 0, .size = VK_WHOLE_SIZE};
                command.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 3, .pBufferMemoryBarriers = barriers.data()}, dispatch);
            });
            if (external && release) scheduler.FlushForExternal(*external, value);
            else if (!external) scheduler.FinishForExternal();
        };
        refresh_mirror(); ownership(true);
        const auto drawn = metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings, push, draw);
        if (drawn != BbMetalFX::CommandResult::Complete) std::fprintf(stderr, "native graphics unavailable: %.*s\n", int(metal_pipeline.Error().size()), metal_pipeline.Error().data());
        assert(drawn == BbMetalFX::CommandResult::Complete);
        ownership(false);
        const vk::BufferImageCopy whole{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, .imageExtent = {32, 32, 1}};
        runtime.DownloadImage(&output, &actual, {&whole, 1});
        runtime.Transit(&reference, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        runtime.Transit(&target_depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        runtime.FlushBarriers();
        state.color_attachments[0].image_view = *reference_view.image_view;
        scheduler.BeginRendering(state);
        scheduler.Record([&](vk::CommandBuffer command) {
            command.bindPipeline(vk::PipelineBindPoint::eGraphics, *vk_pipeline, dispatch);
            command.setViewport(0, dynamic.viewports, dispatch); command.setScissor(0, dynamic.scissors, dispatch);
            const vk::Buffer vertex = vertex_data.Handle(); const vk::DeviceSize offset = 0;
            command.bindVertexBuffers(0, 1, &vertex, &offset, dispatch);
            command.bindIndexBuffer(index_data.Handle(), 4, vk::IndexType::eUint16, dispatch);
            const vk::DescriptorImageInfo image_info{{}, *sampled_view.image_view, vk::ImageLayout::eGeneral}, sampler_info{*sampler, {}, vk::ImageLayout::eUndefined};
            const vk::WriteDescriptorSet writes[]{
                {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &image_info},
                {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &sampler_info}};
            command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *layout, 0, writes, dispatch);
            command.pushConstants(*layout, vk::ShaderStageFlagBits::eFragment, 0, 16, tint.data(), dispatch);
            command.drawIndexed(6, 1, 0, 1, 0, dispatch);
        });
        scheduler.EndRendering();
        runtime.DownloadImage(&reference, &expected, {&whole, 1}); scheduler.Finish();
        actual.Invalidate(0, 4096); expected.Invalidate(0, 4096);
        unsigned differences = 0;
        for (u32 i = 0; i < 4096; ++i) {
            const int delta = std::abs(int(actual.mapped_data[i])-int(expected.mapped_data[i]));
            if (delta > 1 && differences++ < 8) std::fprintf(stderr, "native graphics mismatch byte %u: %u versus %u\n", i, actual.mapped_data[i], expected.mapped_data[i]);
        }
        assert(!differences);
        assert(actual.mapped_data[10 * 32 * 4 + 10 * 4] != actual.mapped_data[0]); // The scissored draw actually wrote pixels.
        for (size_t i = 0; i < points.size(); ++i) std::memcpy(vertex_data.mapped_data.data() + i * 32, points[i].data(), 16);
        vertex_data.Flush(0, 512); native_vertex.input.stride = 32;
        state.color_attachments[0].image_view = *output_view.image_view;
        refresh_mirror(); ownership(true);
        assert(metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings, push, draw) == BbMetalFX::CommandResult::Complete);
        ownership(false);
        runtime.DownloadImage(&output, &actual, {&whole, 1}); scheduler.Finish(); actual.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i) assert(std::abs(int(actual.mapped_data[i])-int(expected.mapped_data[i])) <= 1);
        std::atomic<unsigned> completed{0};
        bool retired = false;
        const auto tick = scheduler.CurrentTick();
        scheduler.DeferOperation([&] { retired = true; });
        scheduler.ExternalSemaphore();
        u64 previous_value = 0;
        for (unsigned i = 0; i < 2; ++i) {
            std::function<bool(float*)> work;
            assert(metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings,
                push, draw, nullptr, &work) == BbMetalFX::CommandResult::Prepared);
            assert(work);
            const auto value = scheduler.NextExternalValue();
            assert(value > previous_value);
            previous_value = value;
            auto fence = std::make_shared<vk::UniqueFence>(Vulkan::Check(instance.GetDevice().createFenceUnique({}, nullptr, dispatch)));
            Vulkan::SubmitInfo release{};
            release.fence = **fence;
            refresh_mirror(); ownership(true, &release, value);
            scheduler.EnqueueExternal(std::move(fence), value, [work = std::move(work), &completed,
                    input = vertex_mirror.buffer.metal->NativeHandle(), output = native_written.buffer.metal->NativeHandle()] {
                if (!work(nullptr)) return false;
                const VkBufferCopy bytes{0, 0, 512};
                if (!BbMetalFX::CopyBuffers(input, output, {&bytes, 1})) return false;
                completed.fetch_add(1, std::memory_order_release);
                return true;
            });
            ownership(false, &release, value); // Only record the acquire; Vulkan waits on the native completion.
            scheduler.Record([source = native_written.Handle(), destination = mirror_readback.Handle(), &dispatch](vk::CommandBuffer command) {
                command.copyBuffer(source, destination, vk::BufferCopy{0, 0, 512}, dispatch);
            });
            assert(scheduler.CurrentTick() == tick);
            assert(!retired);
        }
        runtime.DownloadImage(&output, &actual, {&whole, 1}); scheduler.Finish(); scheduler.PopPendingOperations();
        assert(completed.load(std::memory_order_acquire) == 2 && retired);
        mirror_readback.Invalidate(0, 512);
        assert(std::memcmp(mirror_readback.mapped_data.data(), vertex_data.mapped_data.data(), 512) == 0);
        actual.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i) assert(std::abs(int(actual.mapped_data[i])-int(expected.mapped_data[i])) <= 1);
        std::array<std::function<bool(float*)>, 2> batch;
        std::shared_ptr<BbMetalFX::GraphicsBatch> metal_batch;
        for (auto& work : batch)
            assert(metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings,
                push, draw, nullptr, &work, &metal_batch) == BbMetalFX::CommandResult::Prepared);
        assert(metal_batch && metal_batch->EncodedDrawCount() == batch.size());
        refresh_mirror();
        completed.store(0);
        retired = false;
        const auto batch_tick = scheduler.CurrentTick();
        scheduler.DeferOperation([&] { retired = true; });
        unsigned releases = 0;
        scheduler.SetPendingExternal([&, batch = std::move(batch)] {
            ++releases;
            const auto value = scheduler.NextExternalValue();
            auto fence = std::make_shared<vk::UniqueFence>(Vulkan::Check(instance.GetDevice().createFenceUnique({}, nullptr, dispatch)));
            Vulkan::SubmitInfo release{};
            release.fence = **fence;
            ownership(true, &release, value);
            scheduler.EnqueueExternal(std::move(fence), value, [batch, &completed] {
                if (!batch.back()(nullptr)) return false;
                completed.fetch_add(batch.size(), std::memory_order_release);
                return true;
            });
            ownership(false, &release, value);
        });
        assert(releases == 0 && !retired);
        runtime.DownloadImage(&output, &actual, {&whole, 1});
        assert(releases == 1 && scheduler.CurrentTick() == batch_tick && !retired);
        scheduler.Finish(); scheduler.PopPendingOperations();
        assert(completed.load(std::memory_order_acquire) == 2 && retired);
        actual.Invalidate(0, 4096);
        for (u32 i = 0; i < 4096; ++i) assert(std::abs(int(actual.mapped_data[i])-int(expected.mapped_data[i])) <= 1);
        std::puts("PASS: two native draws encode into one Metal command buffer and one ownership release; dependent Vulkan download matches pixels and guest retirement waits for completion");
        ownership(true);
        std::array<std::function<bool(float*)>, 64> prepared;
        // Apple's default queue allows 64 uncompleted commands. Helpers must progress while it is full.
        for (auto& work : prepared)
            assert(metal_pipeline.Draw(state, dynamic, {&native_attribute, 1}, {&native_vertex, 1}, shader_bindings,
                push, draw, nullptr, &work) == BbMetalFX::CommandResult::Prepared);
        auto helper = std::async(std::launch::async, [&] {
            const VkBufferCopy bytes{0, 0, 512};
            return BbMetalFX::CopyBuffers(vertex_mirror.buffer.metal->NativeHandle(),
                native_written.buffer.metal->NativeHandle(), {&bytes, 1});
        });
        const bool helper_ready = helper.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        // Drain even on timeout so a shared-queue regression can unwind instead of hanging the check.
        for (auto& work : prepared) assert(work(nullptr));
        assert(helper.get());
        ownership(false);
        assert(helper_ready);
        std::puts("PASS: synchronous native helper completes while 64 deferred graphics commands fill their queue");
        instance.GetDevice().destroyShaderModule(vs, nullptr, dispatch); instance.GetDevice().destroyShaderModule(fs, nullptr, dispatch);
        std::puts("PASS: native indexed textured graphics versus Vulkan, mip/layer/swizzle views, sampler LOD bias, base vertex/index offset, dynamic stride, blend/channel mask, depth, viewport/scissor, untouched pixels, queued mirror refresh/native writeback and ordered async completion preserving the guest tick");
    }
    std::puts("PASS: nine color formats, native uploads/downloads, Vulkan reference, padded rows/layers, untouched bytes/pixels, preserved staging tick, rejected copies and shared depth allocation");
    std::puts(native ? "PASS: shared cache ownership, BDA, moves, 128 MiB stream, native copies, untouched bytes, preserved staging tick and callbacks" :
                      "PASS: original Vulkan allocation and copy fallback");
}
