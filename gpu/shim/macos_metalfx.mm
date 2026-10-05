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
#include <map>

namespace BbMetalFX {
struct Presentation::Impl {
    struct Texture {
        VkImage image{};
        VkDeviceMemory memory{};
        id<MTLTexture> metal;
        id<MTLHeap> heap;
    };
    struct Slot {
        Images images;
        Texture input, output;
        id<MTLFXSpatialScaler> scaler;
        id<MTLTexture> scaled;
    };
    VkPhysicalDevice physical;
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory_props{};
    PFN_vkGetPhysicalDeviceImageFormatProperties2 format_props;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_memory;
    PFN_vkGetMemoryMetalHandleEXT export_texture;
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    id<MTLRenderPipelineState> present_pipeline;
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
    MTLPixelFormat metal_format{MTLPixelFormatRGBA8Unorm};
    const char* format_name{"RGBA8"};
    std::map<uint32_t, Slot> slots;
    bool available = true, announced = false, completed = false;
    bool spatial = true, presented = false;

    Impl(VkInstance instance, VkPhysicalDevice p, VkDevice d,
         PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp) : physical(p), device(d) {
        if (const char* choice = std::getenv("BB_METALFX_FORMAT")) {
            if (std::strcmp(choice, "rgba16f") == 0) {
                format = VK_FORMAT_R16G16B16A16_SFLOAT;
                metal_format = MTLPixelFormatRGBA16Float; format_name = "RGBA16F";
            } else if (std::strcmp(choice, "rgba8") != 0) {
                std::fprintf(stderr, "MetalFX: unknown scratch format '%s'; using RGBA8.\n", choice);
            }
        }
#define DEVICE_PROC(member, name) member = reinterpret_cast<PFN_##name>(dp(d, #name))
        DEVICE_PROC(create_image, vkCreateImage);
        DEVICE_PROC(destroy_image, vkDestroyImage);
        DEVICE_PROC(memory_requirements, vkGetImageMemoryRequirements);
        DEVICE_PROC(allocate_memory, vkAllocateMemory);
        DEVICE_PROC(free_memory, vkFreeMemory);
        DEVICE_PROC(bind_memory, vkBindImageMemory);
        DEVICE_PROC(export_texture, vkGetMemoryMetalHandleEXT);
#undef DEVICE_PROC
        format_props = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            ip(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
        auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            ip(instance, "vkGetPhysicalDeviceMemoryProperties"));
        available = create_image && destroy_image && memory_requirements && allocate_memory &&
                    free_memory && bind_memory && export_texture && format_props && get_memory;
        if (available) get_memory(p, &memory_props);
    }
    void Release(Texture& t) {
        t.metal = nil;
        if (t.image) destroy_image(device, t.image, nullptr);
        if (t.memory) free_memory(device, t.memory, nullptr);
        t.heap = nil;
        t.image = {}; t.memory = {};
    }
    void Release(Slot& s) {
        s.scaler = nil; s.scaled = nil;
        Release(s.input); Release(s.output);
        s.images = {};
    }
    ~Impl() { for (auto& [key, slot] : slots) Release(slot); }

    bool Allocate(Texture& t, uint32_t width, uint32_t height) {
        const auto fail = [&](const char* stage, VkResult result = VK_SUCCESS) {
            std::fprintf(stderr, "MetalFX allocation failed: %s %ux%u; %s (VkResult=%d).\n",
                         format_name, width, height, stage, int(result));
            return false;
        };
        // The presenter stores SDR pixels in an UNORM frame after its sRGB encode.
        // Preserve that encoding in the scratch texture and use MetalFX perceptual processing.
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkPhysicalDeviceExternalImageFormatInfo external_query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external_query, .format = format,
            .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
        VkExternalImageFormatProperties external_props{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                      .pNext = &external_props};
        VkResult result = format_props(physical, &query, &props);
        if (result != VK_SUCCESS) return fail("external image format query", result);
        if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) return fail("image memory is not exportable");
        if (width > props.imageFormatProperties.maxExtent.width ||
            height > props.imageFormatProperties.maxExtent.height) return fail("image exceeds format limits");
        VkExternalMemoryImageCreateInfo external_image{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkImageCreateInfo image_info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external_image, .imageType = VK_IMAGE_TYPE_2D,
            .format = format, .extent = {width, height, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        result = create_image(device, &image_info, nullptr, &t.image);
        if (result != VK_SUCCESS) return fail("vkCreateImage", result);
        VkMemoryRequirements req{};
        memory_requirements(device, t.image, &req);
        uint32_t type = memory_props.memoryTypeCount;
        for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i; break;
            }
        }
        if (type == memory_props.memoryTypeCount) return fail("no device-local memory type");
        VkExportMemoryAllocateInfo export_info{.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        VkMemoryDedicatedAllocateInfo dedicated{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &export_info, .image = t.image};
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated, .allocationSize = req.size, .memoryTypeIndex = type};
        result = allocate_memory(device, &alloc, nullptr, &t.memory);
        if (result != VK_SUCCESS) return fail("vkAllocateMemory", result);
        result = bind_memory(device, t.image, t.memory, 0);
        if (result != VK_SUCCESS) return fail("vkBindImageMemory", result);
        VkMemoryGetMetalHandleInfoEXT export_query{.sType = VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT,
            .memory = t.memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT};
        void* handle = nullptr;
        result = export_texture(device, &export_query, &handle);
        if (result != VK_SUCCESS || !handle) return fail("vkGetMemoryMetalHandleEXT", result);
        // Borrowed export: retain the heap until both APIs finish using it.
        t.heap = (__bridge id<MTLHeap>)handle;
        if (t.heap.type != MTLHeapTypePlacement) return fail("requires a placement heap");
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            metal_format width:width height:height mipmapped:NO];
        desc.storageMode = t.heap.storageMode;
        desc.cpuCacheMode = t.heap.cpuCacheMode;
        desc.hazardTrackingMode = t.heap.hazardTrackingMode;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        const MTLSizeAndAlign size = [t.heap.device heapTextureSizeAndAlignWithDescriptor:desc];
        if (!size.size || size.size > req.size || size.size > t.heap.size ||
            size.align > req.alignment) {
            std::fprintf(stderr, "MetalFX layout: Metal size/alignment=%lu/%lu; Vulkan=%llu/%llu; heap=%lu.\n",
                (unsigned long)size.size, (unsigned long)size.align,
                (unsigned long long)req.size, (unsigned long long)req.alignment,
                (unsigned long)t.heap.size);
            return fail("Metal/Vulkan texture layout requirements differ");
        }
        t.metal = [t.heap newTextureWithDescriptor:desc offset:0];
        return t.metal != nil || fail("newTextureWithDescriptor:offset:");
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
const Images* Presentation::Find(uint32_t key) const {
    const auto found = impl->slots.find(key);
    return found == impl->slots.end() ? nullptr : &found->second.images;
}
Images* Presentation::Prepare(uint32_t key, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
    @autoreleasepool {
        if (!impl->available || !iw || !ih || !ow || !oh) return nullptr;
        const bool native = NativePresentation();
        if (!native && (ow < iw || oh < ih || (ow == iw && oh == ih))) return nullptr;
        auto& s = impl->slots[key];
        if (s.images.input_width == iw && s.images.input_height == ih &&
            s.images.output_width == ow && s.images.output_height == oh) return &s.images;
        impl->Release(s);
        if (!impl->Allocate(s.input, iw, ih) || (!native && !impl->Allocate(s.output, ow, oh))) {
            impl->Release(s); impl->Disable("exportable placement heap textures could not be allocated");
            return nullptr;
        }
        id<MTLDevice> device = s.input.metal.device;
        const bool scale = impl->spatial && ow >= iw && oh >= ih && (ow != iw || oh != ih);
        if ((!native && s.output.metal.device != device) ||
            (scale && ![MTLFXSpatialScalerDescriptor supportsDevice:device])) {
            impl->Release(s); impl->Disable("GPU does not support the spatial scaler"); return nullptr;
        }
        if (!impl->queue) impl->queue = [device newCommandQueue];
        if (!impl->queue) {
            impl->Release(s); impl->Disable("Metal command queue allocation failed"); return nullptr;
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
                impl->Release(s); impl->Disable("scaler creation or texture usage requirements failed");
                return nullptr;
            }
            // Supply MetalFX's fence for the driver's untracked heap resources.
            if (s.input.metal.hazardTrackingMode == MTLHazardTrackingModeUntracked) {
                s.scaler.fence = [device newFence];
                if (!s.scaler.fence) {
                    impl->Release(s); impl->Disable("untracked-resource fence allocation failed"); return nullptr;
                }
            }
        }
        s.images = {s.input.image, s.output.image, iw, ih, ow, oh, false};
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
        auto& s = impl->slots.at(key);
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        if (!command) { impl->Disable("Metal command buffer allocation failed"); return false; }
        command.label = NativePresentation() ? @"Metalborne native presentation" : @"bbport MetalFX spatial";
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
        if (NativePresentation()) {
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
