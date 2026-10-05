// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

#ifdef __APPLE__
    const char* backend = std::getenv("BB_PRESENT_BACKEND");
    const bool native = backend && std::strcmp(backend, "metal") == 0;
    const char* mode = std::getenv("BB_METALFX");
    const bool spatial = mode && std::strcmp(mode, "spatial") == 0;
    if (backend && !native && std::strcmp(backend, "vulkan") != 0) {
        std::fprintf(stderr, "Unknown presentation backend '%s'; using Vulkan.\n", backend);
    }
    if (mode && !spatial && std::strcmp(mode, "off") != 0) {
        std::fprintf(stderr, "MetalFX: unsupported mode '%s'; use spatial or off.\n", mode);
    }
    if (native || spatial) {
        if (!instance.HasExternalMemoryMetal()) {
            std::fprintf(stderr, "Metal: driver lacks VK_EXT_external_memory_metal; using Vulkan.\n");
        } else {
            metalfx = std::make_unique<BbMetalFX::Presentation>(instance.GetInstance(),
                instance.GetPhysicalDevice(), device, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
            if (native) metalfx->SetNativeLayer(window.GetWindowInfo().render_surface);
            if (!metalfx->Available()) {
                std::fprintf(stderr, "Metal: texture export entry points unavailable; using Vulkan.\n");
                metalfx.reset();
            } else {
                const char* resolution = std::getenv("BB_METALFX_INPUT_RES");
                if (!resolution) resolution = std::getenv("BB_RENDER_RES");
                unsigned w = 0, h = 0; char extra = 0;
                if (resolution && std::sscanf(resolution, "%ux%u%c", &w, &h, &extra) == 2 &&
                    w >= 320 && h >= 180 && w <= 7680 && h <= 4320) {
                    metalfx_input_width = w; metalfx_input_height = h;
                }
                expected_frame_width = metalfx_input_width;
                expected_frame_height = metalfx_input_height;
                std::fprintf(stderr, native ? "Native Metal presentation requested (experimental).\n"
                    : "MetalFX spatial requested; activates when the window exceeds the input size.\n");
            }
        }
    }
#endif
    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    frame_format = swapchain.GetSurfaceFormat().format;
#ifdef __APPLE__
    if (metalfx && metalfx->NativePresentation()) frame_format = static_cast<vk::Format>(metalfx->InputFormat());
#endif
    pp_pass.Create(device, frame_format);
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        device.destroyImageView(frame.image_view);
        if (!frame.external_image) vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyFence(frame.present_done);
    }
#ifdef __APPLE__
    metalfx.reset(); // Frame views and scheduled reads finished before releasing exported images.
#endif
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image && !frame->external_image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }
    frame->image = nullptr; frame->allocation = {}; frame->external_image = false;

    const vk::Format format = frame_format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

#ifdef __APPLE__
    if (metalfx && metalfx->NativePresentation()) {
        frame->image = vk::Image{metalfx->CreateFrameImage(frame->id, width, height)};
        frame->external_image = bool(frame->image);
    }
#endif
    if (!frame->image) {
        VkImage unsafe_image{};
        VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
        VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                         &unsafe_image, &frame->allocation, nullptr);
        if (result != VK_SUCCESS) [[unlikely]] {
            LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                         vk::to_string(vk::Result{result}));
            UNREACHABLE();
        }
        frame->image = vk::Image{unsafe_image};
    }
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eGeneral,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);



    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    static const bool timing = EmulatorSettingsImpl::Flag("BB_PRESENT_STATS",
        EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false));
    const auto wait_start = timing ? BbPresentStats::Clock::now() : BbPresentStats::Clock::time_point{};
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    if (timing) backpressure_stats.Observe({BbPresentStats::Milliseconds(BbPresentStats::Clock::now() - wait_start)});
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

#ifdef __APPLE__
void Presenter::ApplyMetalFX(Frame* frame, vk::Image& source, u32& width, u32& height) {
    const bool native = metalfx->NativePresentation();
    const u32 target_width = native ? std::max(0, window.GetWidth()) : swapchain.GetWidth();
    const u32 target_height = native ? std::max(0, window.GetHeight()) : swapchain.GetHeight();
    if (!target_width || !target_height) return;
    const auto fit = FitImage(frame->width, frame->height, target_width, target_height);
    if (!native && (fit.extent.width < frame->width || fit.extent.height < frame->height ||
        (fit.extent.width == frame->width && fit.extent.height == frame->height))) return;
    if (const auto* previous = metalfx->Find(frame->id); previous && previous->input &&
        (previous->input_width != frame->width || previous->input_height != frame->height ||
         previous->output_width != fit.extent.width || previous->output_height != fit.extent.height)) {
        // Idle redraws may still reference a cached output. Finish those reads before
        // destroying a slot's images during resize.
        present_scheduler.Finish();
    }
    auto* images = metalfx->Prepare(frame->id, frame->width, frame->height,
                                    fit.extent.width, fit.extent.height);
    if (!images) return;
    const auto begin = std::chrono::steady_clock::now();
    auto& scheduler = present_scheduler;
    const auto command = scheduler.CommandBuffer();
    const auto family = instance.GetGraphicsQueueFamilyIndex();
    const vk::Image input{images->input}, output{images->output};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const bool direct = frame->image == input;
    if (!direct) {
        const std::array to_copy{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image, .subresourceRange = range},
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = images->initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = input, .subresourceRange = range},
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                                {}, {}, {}, to_copy);
        command.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, input,
                          vk::ImageLayout::eTransferDstOptimal,
                          MakeImageBlitStretch(frame->width, frame->height, frame->width, frame->height),
                          vk::Filter::eNearest);
        const std::array release{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image, .subresourceRange = range},
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = input, .subresourceRange = range},
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = images->initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = output, .subresourceRange = range},
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                                {}, {}, {}, vk::ArrayProxy<const vk::ImageMemoryBarrier>{output ? 3u : 2u, release.data()});
    }
    // Keep layout transitions within Vulkan; external handoffs use GENERAL on
    // both sides, so release/acquire barriers describe the same layout pair.
    const std::array external_release{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite, .dstAccessMask = {},
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = family, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = input, .subresourceRange = range},
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = {},
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = family, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = output, .subresourceRange = range},
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
        vk::ArrayProxy<const vk::ImageMemoryBarrier>{output ? 2u : 1u, external_release.data()});
    const auto tick = scheduler.CurrentTick();
    SubmitInfo ready{};
    ready.AddWait(frame->ready_semaphore, frame->ready_tick);
    scheduler.Flush(ready);
    // ponytail: KosmicKrisp has no public MTLSharedEvent bridge. Keep both
    // APIs' ownership and completion explicit until GPU-side synchronization is available.
    scheduler.Wait(tick);
    const auto copied = std::chrono::steady_clock::now();
    const bool success = metalfx->Encode(frame->id, native ? target_width : 0, native ? target_height : 0);
    const auto scaled = std::chrono::steady_clock::now();
    const std::array acquire{
        vk::ImageMemoryBarrier{
            .srcAccessMask = {}, .dstAccessMask = direct
                ? vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                : vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL, .dstQueueFamilyIndex = family,
            .image = input, .subresourceRange = range},
        vk::ImageMemoryBarrier{
            .srcAccessMask = {}, .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL, .dstQueueFamilyIndex = family,
            .image = output, .subresourceRange = range},
    };
    scheduler.CommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
        vk::ArrayProxy<const vk::ImageMemoryBarrier>{output ? 2u : 1u, acquire.data()});
    images->initialized = true;
    if (success && output) {
        source = output; width = images->output_width; height = images->output_height;
        last_metalfx_frame = frame; last_metalfx_image = output;
        last_metalfx_width = width; last_metalfx_height = height;
    }
    static const bool stats = EmulatorSettingsImpl::Flag("BB_PRESENT_STATS",
        EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false));
    if (stats) {
        static auto window = begin;
        static double copy_ms = 0, scale_ms = 0; static u32 count = 0;
        copy_ms += std::chrono::duration<double, std::milli>(copied - begin).count();
        scale_ms += std::chrono::duration<double, std::milli>(scaled - copied).count();
        ++count;
        if (scaled - window >= std::chrono::seconds(5)) {
            std::printf("%s bridge: %u frames; Vulkan %s/completion %.2f ms/frame; "
                        "Metal encode/completion %.2f ms/frame (CPU wall time)\n",
                        native ? "Native Metal" : "MetalFX", count,
                        direct ? "render (no input copy)" : "copy", copy_ms / count, scale_ms / count);
            count = 0; copy_ms = scale_ms = 0; window = scaled;
        }
    }
}
#endif

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    static const bool timing = EmulatorSettingsImpl::Flag("BB_PRESENT_STATS",
        EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false));
    const auto clock = [&] { return timing ? BbPresentStats::Clock::now() : BbPresentStats::Clock::time_point{}; };
    const auto started = clock();
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

#ifdef __APPLE__
    if (metalfx && metalfx->NativePresentation() && !frame->is_hdr) {
        vk::Image source = frame->image;
        u32 width = frame->width, height = frame->height;
        ApplyMetalFX(frame, source, width, height);
        if (metalfx->NativePresentation()) {
            // Metal has finished reading the exported input. Signal the existing frame
            // fence after its Vulkan ownership acquire, before allowing frame reuse.
            Check(instance.GetDevice().resetFences(frame->present_done));
            SubmitInfo ready{};
            ready.AddWait(frame->ready_semaphore, frame->ready_tick);
            ready.AddSignal(frame->present_done);
            present_scheduler.Flush(ready);
            if (timing && !is_reusing_frame && is_game_frame) {
                api_stats.Observe({0, BbPresentStats::Milliseconds(clock() - started), 0, 0});
            }
            free_frame();
            if (!is_reusing_frame && is_game_frame) DebugState.IncFlipFrameNum();
            return;
        }
        // A failed native command/allocation falls back to a newly configured swapchain.
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
#endif

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    const auto acquired = clock();
    vk::Image source_image = frame->image;
    u32 source_width = frame->width, source_height = frame->height;
#ifdef __APPLE__
    if (is_reusing_frame && last_metalfx_frame == frame && last_metalfx_image && !frame->is_hdr) {
        source_image = last_metalfx_image;
        source_width = last_metalfx_width; source_height = last_metalfx_height;
    } else if (!is_reusing_frame) {
        last_metalfx_image = nullptr;
        last_metalfx_frame = nullptr;
        if (metalfx && metalfx->Available() && !frame->is_hdr && is_game_frame) {
            ApplyMetalFX(frame, source_image, source_width, source_height);
        }
    }
#endif
    const auto upscaled = clock();
    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(source_image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(source_width, source_height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    const auto submitted = clock();
    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    if (timing && !is_reusing_frame && is_game_frame) {
        api_stats.Observe({BbPresentStats::Milliseconds(acquired - started),
            BbPresentStats::Milliseconds(upscaled - acquired),
            BbPresentStats::Milliseconds(submitted - upscaled),
            BbPresentStats::Milliseconds(clock() - submitted)});
    }
    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
#ifdef __APPLE__
    if (metalfx && metalfx->Available()) {
        expected_frame_width = metalfx_input_width;
        expected_frame_height = metalfx_input_height;
        return;
    }
#endif
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
