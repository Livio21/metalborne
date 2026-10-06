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

int main(int argc, char** argv) {
    const bool native = argc == 1 || std::strcmp(argv[1], "--vulkan");
    setenv("BB_METAL_BUFFER_CACHE", native ? "1" : "0", 1);
    setenv("BB_METAL_BUFFER_COPY", native ? "1" : "0", 1);
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
    std::puts(native ? "PASS: shared cache ownership, BDA, moves, 128 MiB stream, native copies, untouched bytes, preserved staging tick and callbacks" :
                      "PASS: original Vulkan allocation and copy fallback");
}
