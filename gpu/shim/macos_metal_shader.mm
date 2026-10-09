#import <Metal/Metal.h>
#include "macos_metal_shader.h"
#include "macos_metalfx.h"
#include <spirv_cross/spirv_msl.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <map>
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace BbMetalFX {
static_assert(sizeof(MTLDrawPrimitivesIndirectArguments) == sizeof(VkDrawIndirectCommand));
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, vertexCount) == offsetof(VkDrawIndirectCommand, vertexCount));
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, instanceCount) == offsetof(VkDrawIndirectCommand, instanceCount));
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, vertexStart) == offsetof(VkDrawIndirectCommand, firstVertex));
static_assert(offsetof(MTLDrawPrimitivesIndirectArguments, baseInstance) == offsetof(VkDrawIndirectCommand, firstInstance));
static_assert(sizeof(MTLDrawIndexedPrimitivesIndirectArguments) == sizeof(VkDrawIndexedIndirectCommand));
static_assert(offsetof(MTLDrawIndexedPrimitivesIndirectArguments, indexCount) == offsetof(VkDrawIndexedIndirectCommand, indexCount));
static_assert(offsetof(MTLDrawIndexedPrimitivesIndirectArguments, instanceCount) == offsetof(VkDrawIndexedIndirectCommand, instanceCount));
static_assert(offsetof(MTLDrawIndexedPrimitivesIndirectArguments, indexStart) == offsetof(VkDrawIndexedIndirectCommand, firstIndex));
static_assert(offsetof(MTLDrawIndexedPrimitivesIndirectArguments, baseVertex) == offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
static_assert(offsetof(MTLDrawIndexedPrimitivesIndirectArguments, baseInstance) == offsetof(VkDrawIndexedIndirectCommand, firstInstance));
static uint32_t TextureScalar(MTLPixelFormat format) {
    switch (format) {
#define U(f) case MTLPixelFormat##f:
        U(R8Uint) U(RG8Uint) U(RGBA8Uint) U(R16Uint) U(RG16Uint) U(RGBA16Uint)
        U(R32Uint) U(RG32Uint) U(RGBA32Uint) U(RGB10A2Uint) U(Stencil8) U(X32_Stencil8) return 1;
#undef U
#define S(f) case MTLPixelFormat##f:
        S(R8Sint) S(RG8Sint) S(RGBA8Sint) S(R16Sint) S(RG16Sint) S(RGBA16Sint)
        S(R32Sint) S(RG32Sint) S(RGBA32Sint) return 2;
#undef S
    default: return 0;
    }
}
struct ShaderFunction::Impl {
    id<MTLDevice> device;
    id<MTLLibrary> library;
    id<MTLFunction> function;
    uint32_t stage = UINT32_MAX;
    uint32_t argument_slot = UINT32_MAX;
    uint32_t push_slot = UINT32_MAX;
    size_t push_size{};
    std::array<uint32_t, 3> workgroup{};
    std::vector<ShaderResource> resources;
    std::string error;
    bool flat_inputs = false;
    mutable id<MTLArgumentEncoder> arguments;
    mutable std::mutex argument_mutex;

    Impl(std::span<const uint32_t> spirv, void* native_device, bool fix_clip) {
        @autoreleasepool {
            device = native_device ? (__bridge id<MTLDevice>)native_device : MTLCreateSystemDefaultDevice();
            if (!device || spirv.size() < 5 || spirv[0] != 0x07230203 || spirv.size() > 4 * 1024 * 1024) {
                error = "Invalid SPIR-V module or missing Metal device";
                return;
            }
            try {
                spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());
                if (compiler.get_ir().addressing_model != spv::AddressingModelLogical)
                    throw std::runtime_error("Physical guest pointers require a native page table");
                const auto entries = compiler.get_entry_points_and_stages();
                if (entries.size() != 1) throw std::runtime_error("Expected one guest entry point");
                stage = entries[0].execution_model;
                if (stage == spv::ExecutionModelGLCompute) {
                    for (uint32_t i = 0; i < 3; ++i)
                        workgroup[i] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
                    if (!workgroup[0] || !workgroup[1] || !workgroup[2])
                        throw std::runtime_error("Compute workgroup size is not fixed");
                }
                auto options = compiler.get_msl_options();
                options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(3, 1);
                options.argument_buffers = true;
                options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
                options.texture_buffer_native = true;
                options.texture_1D_as_2D = true;
                options.pad_fragment_output_components = true;
                options.agx_manual_cube_grad_fixup = true;
                compiler.set_msl_options(options);
                auto common = compiler.get_common_options();
                common.vertex.fixup_clipspace = fix_clip;
                compiler.set_common_options(common);
                compiler.add_msl_resource_binding({.stage = entries[0].execution_model,
                    .desc_set = 0, .binding = spirv_cross::kArgumentBufferBinding, .msl_buffer = 0});
                compiler.add_msl_resource_binding({.stage = entries[0].execution_model,
                    .desc_set = spirv_cross::kPushConstDescSet, .binding = spirv_cross::kPushConstBinding,
                    .msl_buffer = 1});
                const auto reflection = compiler.get_shader_resources();
                const auto source = compiler.compile();
                const auto add = [&](const auto& list, ShaderResourceKind kind) {
                    for (const auto& resource : list) {
                        const auto argument = compiler.get_automatic_msl_resource_binding(resource.id);
                        if (argument == UINT32_MAX) continue;
                        if (compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) != 0)
                            throw std::runtime_error("Guest resource uses an unexpected descriptor set");
                        const auto& type = compiler.get_type(resource.type_id);
                        uint64_t count = 1;
                        for (size_t i = 0; i < type.array.size(); ++i) {
                            if (!type.array_size_literal[i] || !type.array[i] || count > 65536 / type.array[i])
                                throw std::runtime_error("Unbounded guest descriptor array");
                            count *= type.array[i];
                        }
                        ShaderResource native{kind,
                            compiler.get_decoration(resource.id, spv::DecorationBinding),
                            argument, uint32_t(count)};
                        if (kind == ShaderResourceKind::Texture) {
                            const auto& image = type.image;
                            switch (image.dim) {
                            case spv::Dim1D:
                            case spv::Dim2D:
                                native.texture_type = image.ms ? (image.arrayed ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample) :
                                    image.arrayed ? MTLTextureType2DArray : MTLTextureType2D;
                                break;
                            case spv::Dim3D: native.texture_type = MTLTextureType3D; break;
                            case spv::DimCube: native.texture_type = image.arrayed ? MTLTextureTypeCubeArray : MTLTextureTypeCube; break;
                            case spv::DimBuffer: native.texture_type = MTLTextureTypeTextureBuffer; break;
                            default: throw std::runtime_error("Unsupported native texture dimension");
                            }
                            const auto scalar = compiler.get_type(image.type).basetype;
                            native.scalar = scalar == spirv_cross::SPIRType::UInt ? 1 : scalar == spirv_cross::SPIRType::Int ? 2 : 0;
                            native.depth = image.depth;
                        }
                        resources.push_back(native);
                    }
                };
                add(reflection.storage_buffers, ShaderResourceKind::Buffer);
                add(reflection.uniform_buffers, ShaderResourceKind::Buffer);
                add(reflection.separate_images, ShaderResourceKind::Texture);
                add(reflection.storage_images, ShaderResourceKind::Texture);
                add(reflection.separate_samplers, ShaderResourceKind::Sampler);
                for (const auto& input : reflection.stage_inputs) {
                    flat_inputs |= compiler.has_decoration(input.id, spv::DecorationFlat);
                    const auto& type = compiler.get_type(input.base_type_id);
                    for (uint32_t member = 0; member < type.member_types.size(); ++member)
                        flat_inputs |= compiler.has_member_decoration(input.base_type_id, member, spv::DecorationFlat);
                }
                if (!reflection.sampled_images.empty() || !reflection.subpass_inputs.empty())
                    throw std::runtime_error("Combined sampler/input attachment requires native binding support");
                if (!resources.empty()) argument_slot = 0;
                if (reflection.push_constant_buffers.size() > 1)
                    throw std::runtime_error("Multiple guest push-constant blocks");
                if (!reflection.push_constant_buffers.empty()) {
                    push_slot = compiler.get_automatic_msl_resource_binding(reflection.push_constant_buffers[0].id);
                    push_size = compiler.get_declared_struct_size(compiler.get_type(
                        reflection.push_constant_buffers[0].base_type_id));
                }
                MTLCompileOptions* native_options = [MTLCompileOptions new];
                native_options.languageVersion = MTLLanguageVersion3_1;
                native_options.fastMathEnabled = NO;
                NSError* failure = nil;
                library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                               options:native_options error:&failure];
                if (!library) {
                    error = (failure.localizedDescription ?: @"Metal shader compilation failed").UTF8String;
                    return;
                }
                const auto& entry = compiler.get_cleansed_entry_point_name(entries[0].name, entries[0].execution_model);
                function = [library newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
                if (!function) error = "Compiled Metal entry point missing";
            } catch (const std::exception& failure) {
                error = failure.what();
            }
        }
    }
    struct Arguments { id<MTLBuffer> encoded; std::vector<ShaderBinding> used; };
    bool Encode(std::span<const ShaderBinding> bindings, std::span<const uint8_t> push, Arguments& result) const {
        std::lock_guard lock{argument_mutex};
        if (push_slot != UINT32_MAX && (push.size() < push_size || push.size() > 4096))
            return false;
        if (!arguments && argument_slot != UINT32_MAX)
            arguments = [function newArgumentEncoderWithBufferIndex:argument_slot];
        result.encoded = arguments ? [device newBufferWithLength:arguments.encodedLength
                                                                    options:MTLResourceStorageModeShared] : nil;
        if (argument_slot != UINT32_MAX && (!arguments || !result.encoded)) return false;
        if (result.encoded) {
            std::memset(result.encoded.contents, 0, result.encoded.length);
            [arguments setArgumentBuffer:result.encoded offset:0];
        }
        for (const auto& resource : resources) {
            for (uint32_t i = 0; i < resource.count; ++i) {
                const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
                    return binding.kind == resource.kind && binding.binding == resource.binding && binding.element == i;
                });
                if (found == bindings.end() || !found->native) return false;
                const auto& binding = *found;
                switch (resource.kind) {
                case ShaderResourceKind::Buffer: {
                    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)binding.native;
                    if (buffer.device != device || !binding.size || binding.offset > buffer.length ||
                        binding.size > buffer.length - binding.offset) return false;
                    [arguments setBuffer:buffer offset:binding.offset atIndex:resource.argument + i];
                    break;
                }
                case ShaderResourceKind::Texture: {
                    id<MTLTexture> texture = (__bridge id<MTLTexture>)binding.native;
                    const bool depth = texture.pixelFormat == MTLPixelFormatDepth16Unorm ||
                        texture.pixelFormat == MTLPixelFormatDepth32Float || texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
                    if (texture.device != device || uint32_t(texture.textureType) != resource.texture_type ||
                        TextureScalar(texture.pixelFormat) != resource.scalar || depth != resource.depth) return false;
                    [arguments setTexture:texture atIndex:resource.argument + i];
                    break;
                }
                case ShaderResourceKind::Sampler: {
                    id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)binding.native;
                    if (sampler.device != device) return false;
                    [arguments setSamplerState:sampler atIndex:resource.argument + i];
                    break;
                }
                }
                result.used.push_back(binding);
            }
        }
        return true;
    }

};

ShaderFunction::ShaderFunction(std::span<const uint32_t> spirv, void* device, bool fix_clip)
    : impl(std::make_unique<Impl>(spirv, device, fix_clip)) {}
ShaderFunction::~ShaderFunction() = default;
bool ShaderFunction::Available() const { return impl->function != nil; }
uint32_t ShaderFunction::Stage() const { return impl->stage; }
void* ShaderFunction::Device() const { return (__bridge void*)impl->device; }
void* ShaderFunction::NativeHandle() const { return (__bridge void*)impl->function; }
uint32_t ShaderFunction::ArgumentSlot() const { return impl->argument_slot; }
uint32_t ShaderFunction::PushSlot() const { return impl->push_slot; }
std::span<const ShaderResource> ShaderFunction::Resources() const { return impl->resources; }
std::string_view ShaderFunction::Error() const { return impl->error; }

struct ComputeKernel::Impl {
    ShaderFunction shader;
    id<MTLComputePipelineState> pipeline;
    std::string error;
    Impl(std::span<const uint32_t> spirv, void* device) : shader(spirv, device) {
        @autoreleasepool {
            if (!shader.Available()) { error = shader.Error(); return; }
            if (shader.Stage() != spv::ExecutionModelGLCompute) {
                error = "Expected a compute entry point"; return;
            }
            const auto& group = shader.impl->workgroup;
            const uint64_t count = uint64_t(group[0]) * group[1] * group[2];
            const auto limit = shader.impl->device.maxThreadsPerThreadgroup;
            if (group[0] > limit.width || group[1] > limit.height || group[2] > limit.depth) {
                error = "Workgroup exceeds Metal device limits"; return;
            }
            NSError* failure = nil;
            pipeline = [shader.impl->device newComputePipelineStateWithFunction:shader.impl->function
                                                                         error:&failure];
            if (!pipeline) error = (failure.localizedDescription ?: @"Metal compute pipeline failed").UTF8String;
            else if (count > pipeline.maxTotalThreadsPerThreadgroup) {
                pipeline = nil; error = "Workgroup exceeds Metal kernel limits";
            }
        }
    }
};
ComputeKernel::ComputeKernel(std::span<const uint32_t> spirv, void* device)
    : impl(std::make_unique<Impl>(spirv, device)) {}
ComputeKernel::~ComputeKernel() = default;
bool ComputeKernel::Available() const { return impl->pipeline != nil; }
std::string_view ComputeKernel::Error() const { return impl->error; }
std::array<uint32_t, 3> ComputeKernel::WorkgroupSize() const { return impl->shader.impl->workgroup; }
std::span<const ShaderResource> ComputeKernel::Resources() const { return impl->shader.Resources(); }

CommandResult ComputeKernel::Dispatch(std::span<const ShaderBinding> bindings, std::span<const uint8_t> push,
                                    std::array<uint32_t, 3> groups, float* gpu_ms) const {
    @autoreleasepool {
        if (!Available()) return CommandResult::Unavailable;
        auto& shader = *impl->shader.impl;
        ShaderFunction::Impl::Arguments resources;
        if (!shader.Encode(bindings, push, resources)) return CommandResult::Unavailable;
        if (!groups[0] || !groups[1] || !groups[2]) return CommandResult::Complete;
        bool submitted = false;
        const bool complete = RunCommands(shader.device, [&](id<MTLCommandBuffer> command, id<MTLFence> fence) {
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) return false;
            [encoder waitForFence:fence];
            [encoder setComputePipelineState:impl->pipeline];
            if (resources.encoded) [encoder setBuffer:resources.encoded offset:0 atIndex:shader.argument_slot];
            if (shader.push_slot != UINT32_MAX)
                [encoder setBytes:push.data() length:push.size() atIndex:shader.push_slot];
            for (const auto& binding : resources.used) {
                if (binding.kind != ShaderResourceKind::Sampler) {
                    [encoder useResource:(__bridge id<MTLResource>)binding.native
                                   usage:MTLResourceUsageRead | (binding.written ? MTLResourceUsageWrite : 0)];
                }
            }
            const auto threads = shader.workgroup;
            [encoder dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2])
                    threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
            [encoder endEncoding];
            return true;
        }, gpu_ms, &submitted);
        return complete ? CommandResult::Complete : submitted ? CommandResult::Failed : CommandResult::Unavailable;
    }
}

static MTLVertexFormat VertexFormat(VkFormat format) {
    switch (format) {
#define V(v, m) case VK_FORMAT_##v: return MTLVertexFormat##m
        V(R32_SFLOAT, Float); V(R32G32_SFLOAT, Float2); V(R32G32B32_SFLOAT, Float3); V(R32G32B32A32_SFLOAT, Float4);
        V(R32_UINT, UInt); V(R32G32_UINT, UInt2); V(R32G32B32_UINT, UInt3); V(R32G32B32A32_UINT, UInt4);
        V(R32_SINT, Int); V(R32G32_SINT, Int2); V(R32G32B32_SINT, Int3); V(R32G32B32A32_SINT, Int4);
        V(R16_SFLOAT, Half); V(R16G16_SFLOAT, Half2); V(R16G16B16_SFLOAT, Half3); V(R16G16B16A16_SFLOAT, Half4);
        V(R16_UNORM, UShortNormalized); V(R16G16_UNORM, UShort2Normalized); V(R16G16B16_UNORM, UShort3Normalized); V(R16G16B16A16_UNORM, UShort4Normalized);
        V(R16_SNORM, ShortNormalized); V(R16G16_SNORM, Short2Normalized); V(R16G16B16_SNORM, Short3Normalized); V(R16G16B16A16_SNORM, Short4Normalized);
        V(R16_UINT, UShort); V(R16G16_UINT, UShort2); V(R16G16B16_UINT, UShort3); V(R16G16B16A16_UINT, UShort4);
        V(R16_SINT, Short); V(R16G16_SINT, Short2); V(R16G16B16_SINT, Short3); V(R16G16B16A16_SINT, Short4);
        V(R8_UNORM, UCharNormalized); V(R8G8_UNORM, UChar2Normalized); V(R8G8B8_UNORM, UChar3Normalized); V(R8G8B8A8_UNORM, UChar4Normalized);
        V(R8_SNORM, CharNormalized); V(R8G8_SNORM, Char2Normalized); V(R8G8B8_SNORM, Char3Normalized); V(R8G8B8A8_SNORM, Char4Normalized);
        V(R8_UINT, UChar); V(R8G8_UINT, UChar2); V(R8G8B8_UINT, UChar3); V(R8G8B8A8_UINT, UChar4);
        V(R8_SINT, Char); V(R8G8_SINT, Char2); V(R8G8B8_SINT, Char3); V(R8G8B8A8_SINT, Char4);
        V(A2B10G10R10_UNORM_PACK32, UInt1010102Normalized); V(A2B10G10R10_SNORM_PACK32, Int1010102Normalized);
        V(B8G8R8A8_UNORM, UChar4Normalized_BGRA); V(B10G11R11_UFLOAT_PACK32, FloatRG11B10); V(E5B9G9R9_UFLOAT_PACK32, FloatRGB9E5);
#undef V
    default: return MTLVertexFormatInvalid;
    }
}
static MTLBlendFactor BlendFactor(VkBlendFactor factor) {
    switch (factor) {
#define B(v, m) case VK_BLEND_FACTOR_##v: return MTLBlendFactor##m
        B(ZERO, Zero); B(ONE, One); B(SRC_COLOR, SourceColor); B(ONE_MINUS_SRC_COLOR, OneMinusSourceColor);
        B(DST_COLOR, DestinationColor); B(ONE_MINUS_DST_COLOR, OneMinusDestinationColor);
        B(SRC_ALPHA, SourceAlpha); B(ONE_MINUS_SRC_ALPHA, OneMinusSourceAlpha);
        B(DST_ALPHA, DestinationAlpha); B(ONE_MINUS_DST_ALPHA, OneMinusDestinationAlpha);
        B(CONSTANT_COLOR, BlendColor); B(ONE_MINUS_CONSTANT_COLOR, OneMinusBlendColor);
        B(CONSTANT_ALPHA, BlendAlpha); B(ONE_MINUS_CONSTANT_ALPHA, OneMinusBlendAlpha);
        B(SRC_ALPHA_SATURATE, SourceAlphaSaturated); B(SRC1_COLOR, Source1Color);
        B(ONE_MINUS_SRC1_COLOR, OneMinusSource1Color); B(SRC1_ALPHA, Source1Alpha); B(ONE_MINUS_SRC1_ALPHA, OneMinusSource1Alpha);
#undef B
    default: return MTLBlendFactor(NSUIntegerMax);
    }
}
static MTLColorWriteMask ColorMask(uint32_t mask) {
    return MTLColorWriteMask((mask & VK_COLOR_COMPONENT_R_BIT ? MTLColorWriteMaskRed : 0) |
        (mask & VK_COLOR_COMPONENT_G_BIT ? MTLColorWriteMaskGreen : 0) |
        (mask & VK_COLOR_COMPONENT_B_BIT ? MTLColorWriteMaskBlue : 0) |
        (mask & VK_COLOR_COMPONENT_A_BIT ? MTLColorWriteMaskAlpha : 0));
}
static const VkBaseInStructure* FindChain(const void* chain, VkStructureType type) {
    auto* link = static_cast<const VkBaseInStructure*>(chain);
    while (link && link->sType != type) link = link->pNext;
    return link;
}

struct RenderPipeline::Impl {
    std::unique_ptr<ShaderFunction> vertex, fragment;
    MTLRenderPipelineDescriptor* descriptor;
    VkPrimitiveTopology topology;
    VkPolygonMode polygon;
    bool clamp;
    std::map<std::vector<uint32_t>, id<MTLRenderPipelineState>> pipelines;
    std::map<std::array<uint32_t, 16>, id<MTLDepthStencilState>> depth_states;
    std::string error;
    Impl(std::span<const uint32_t> vs, std::span<const uint32_t> fs, const VkGraphicsPipelineCreateInfo& info, void* device) {
        @autoreleasepool {
            const auto* render = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(FindChain(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO));
            const auto* clip = info.pViewportState ? reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(
                FindChain(info.pViewportState->pNext, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT)) : nullptr;
            if (!render || !info.pInputAssemblyState || !info.pRasterizationState || !info.pMultisampleState ||
                !info.pColorBlendState || render->colorAttachmentCount > 8 ||
                render->colorAttachmentCount != info.pColorBlendState->attachmentCount || info.pColorBlendState->logicOpEnable) {
                error = "Unsupported native rendering/blend configuration"; return;
            }
            for (uint32_t i = 0; i < info.stageCount; ++i)
                if (info.pStages[i].stage != VK_SHADER_STAGE_VERTEX_BIT && info.pStages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
                    error = "Native geometry/tessellation lowering required"; return;
                }
            const auto& samples = *info.pMultisampleState;
            if (samples.sampleShadingEnable) { error = "Native minimum sample shading is unsupported"; return; }
            if (samples.pSampleMask) {
                const uint32_t count = samples.rasterizationSamples;
                for (uint32_t first = 0; first < count; first += 32) {
                    const uint32_t bits = std::min(32u, count - first);
                    const uint32_t enabled = UINT32_MAX >> (32 - bits);
                    if ((samples.pSampleMask[first / 32] & enabled) != enabled) {
                        error = "Native fixed sample masks are unsupported"; return;
                    }
                }
            }
            topology = info.pInputAssemblyState->topology;
            polygon = info.pRasterizationState->polygonMode;
            clamp = info.pRasterizationState->depthClampEnable;
            if (topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || polygon == VK_POLYGON_MODE_POINT) {
                error = "Native primitive expansion required"; return;
            }
            vertex = std::make_unique<ShaderFunction>(vs, device, clip && clip->negativeOneToOne);
            if (!vertex->Available() || vertex->Stage() != spv::ExecutionModelVertex) { error = vertex->Error(); return; }
            if (!fs.empty()) {
                fragment = std::make_unique<ShaderFunction>(fs, vertex->Device());
                if (!fragment->Available() || fragment->Stage() != spv::ExecutionModelFragment) { error = fragment->Error(); return; }
            }
            const auto* provoking = reinterpret_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(
                FindChain(info.pRasterizationState->pNext, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT));
            if (provoking && provoking->provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT &&
                fragment && fragment->impl->flat_inputs) { error = "Flat inputs require last-vertex index expansion"; return; }
            descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = (__bridge id<MTLFunction>)vertex->NativeHandle();
            descriptor.fragmentFunction = fragment ? (__bridge id<MTLFunction>)fragment->NativeHandle() : nil;
            descriptor.rasterSampleCount = info.pMultisampleState->rasterizationSamples;
            descriptor.alphaToCoverageEnabled = info.pMultisampleState->alphaToCoverageEnable;
            descriptor.alphaToOneEnabled = info.pMultisampleState->alphaToOneEnable;
            descriptor.depthAttachmentPixelFormat = ImageFormat(render->depthAttachmentFormat);
            descriptor.stencilAttachmentPixelFormat = ImageFormat(render->stencilAttachmentFormat);
            // Undefined Vulkan attachments correspond to Metal's invalid format.
            for (uint32_t i = 0; i < render->colorAttachmentCount; ++i) {
                auto* attachment = descriptor.colorAttachments[i];
                const auto& blend = info.pColorBlendState->pAttachments[i];
                attachment.pixelFormat = ImageFormat(render->pColorAttachmentFormats[i]);
                if (attachment.pixelFormat == MTLPixelFormatInvalid && render->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED) {
                    descriptor = nil; error = "Unsupported native color format"; return;
                }
                attachment.blendingEnabled = blend.blendEnable;
                attachment.sourceRGBBlendFactor = BlendFactor(blend.srcColorBlendFactor);
                attachment.destinationRGBBlendFactor = BlendFactor(blend.dstColorBlendFactor);
                attachment.sourceAlphaBlendFactor = BlendFactor(blend.srcAlphaBlendFactor);
                attachment.destinationAlphaBlendFactor = BlendFactor(blend.dstAlphaBlendFactor);
                attachment.rgbBlendOperation = MTLBlendOperation(blend.colorBlendOp);
                attachment.alphaBlendOperation = MTLBlendOperation(blend.alphaBlendOp);
                attachment.writeMask = ColorMask(blend.colorWriteMask);
            }
        }
    }
};
RenderPipeline::RenderPipeline(std::span<const uint32_t> vertex, std::span<const uint32_t> fragment,
                               const VkGraphicsPipelineCreateInfo& info, void* device)
    : impl(std::make_unique<Impl>(vertex, fragment, info, device)) {}
RenderPipeline::~RenderPipeline() = default;
bool RenderPipeline::Available() const { return impl->descriptor != nil; }
std::string_view RenderPipeline::Error() const { return impl->error; }
std::span<const ShaderResource> RenderPipeline::Resources(uint32_t stage) const {
    auto* shader = stage == spv::ExecutionModelVertex ? impl->vertex.get() : impl->fragment.get();
    return shader ? shader->Resources() : std::span<const ShaderResource>{};
}

CommandResult RenderPipeline::Draw(const Vulkan::RenderState& state, const Vulkan::DynamicState& dynamic,
        std::span<const VkVertexInputAttributeDescription2EXT> attributes, std::span<const VertexBufferBinding> vertices,
        std::span<const ShaderBinding> bindings, std::span<const uint8_t> push, const DrawCommand& draw, float* gpu_ms,
        std::function<bool(float*)>* deferred, std::shared_ptr<GraphicsBatch>* graphics_batch) const {
    @autoreleasepool {
        if (!Available() || dynamic.viewports.empty() || dynamic.scissors.empty() || dynamic.depth_bounds_test_enabled ||
            dynamic.feedback_loop_enabled || dynamic.line_width != 1 || state.num_layers != 1 ||
            !state.width || !state.height || state.num_color_attachments > 8 ||
            (dynamic.primitive_restart_enable && impl->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
             impl->topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)) return CommandResult::Unavailable;
        if (draw.index_buffer && !dynamic.primitive_restart_enable &&
            (impl->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || impl->topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP))
            return CommandResult::Unavailable; // Metal always treats the strip's sentinel index as restart.
        auto& vs = *impl->vertex->impl;
        ShaderFunction::Impl* fs = impl->fragment ? impl->fragment->impl.get() : nullptr;
        ShaderFunction::Impl::Arguments vertex_args, fragment_args;
        if (!vs.Encode(bindings, push, vertex_args) || (fs && !fs->Encode(bindings, push, fragment_args))) return CommandResult::Unavailable;
        MTLVertexDescriptor* vertex_desc = [MTLVertexDescriptor new];
        std::vector<uint32_t> key;
        for (const auto& vertex : vertices) {
            const auto& input = vertex.input;
            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)vertex.native;
            if (input.binding >= 29 || !buffer || buffer.device != vs.device || !vertex.size ||
                vertex.offset > buffer.length || vertex.size > buffer.length - vertex.offset) return CommandResult::Unavailable;
            auto* layout = vertex_desc.layouts[input.binding + 2];
            const bool constant = !input.stride || (input.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && !input.divisor);
            layout.stride = constant ? 0 : MTLBufferLayoutStrideDynamic;
            layout.stepFunction = constant ? MTLVertexStepFunctionConstant : input.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ?
                MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            layout.stepRate = constant ? 0 : input.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? input.divisor : 1;
            key.insert(key.end(), {input.binding, uint32_t(layout.stepFunction), uint32_t(layout.stepRate)});
        }
        for (const auto& attribute : attributes) {
            if (attribute.location >= 31 || attribute.binding >= 29) return CommandResult::Unavailable;
            const auto format = VertexFormat(attribute.format);
            if (format == MTLVertexFormatInvalid || std::none_of(vertices.begin(), vertices.end(), [&](const auto& vertex) {
                    return vertex.input.binding == attribute.binding && attribute.offset < vertex.size; })) return CommandResult::Unavailable;
            auto* native = vertex_desc.attributes[attribute.location];
            native.format = format; native.bufferIndex = attribute.binding + 2; native.offset = attribute.offset;
            key.insert(key.end(), {attribute.location, attribute.binding, uint32_t(attribute.format), attribute.offset});
        }
        key.push_back(UINT32_MAX);
        for (const auto mask : dynamic.color_write_masks) key.push_back(uint32_t(mask));
        auto found = impl->pipelines.find(key);
        if (found == impl->pipelines.end()) {
            MTLRenderPipelineDescriptor* desc = [impl->descriptor copy];
            desc.vertexDescriptor = vertex_desc;
            for (uint32_t i = 0; i < state.num_color_attachments; ++i)
                desc.colorAttachments[i].writeMask = ColorMask(uint32_t(dynamic.color_write_masks[i]));
            NSError* failure = nil;
            auto pipeline = [vs.device newRenderPipelineStateWithDescriptor:desc error:&failure];
            if (!pipeline) {
                impl->error = (failure.localizedDescription ?: @"Metal graphics pipeline failed").UTF8String;
                return CommandResult::Unavailable;
            }
            found = impl->pipelines.emplace(std::move(key), pipeline).first;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
        pass.renderTargetWidth = state.width; pass.renderTargetHeight = state.height;
        const auto attachment_texture = [&](VkImageView view, MTLPixelFormat format) -> id<MTLTexture> {
            id<MTLTexture> texture = (__bridge id<MTLTexture>)FindNativeImageView(view).attachment;
            if (!texture || texture.device != vs.device || texture.pixelFormat != format ||
                texture.width < state.width || texture.height < state.height || texture.sampleCount != impl->descriptor.rasterSampleCount)
                return nil;
            return texture;
        };
        for (uint32_t i = 0; i < state.num_color_attachments; ++i) {
            const auto& attachment = state.color_attachments[i];
            const auto format = impl->descriptor.colorAttachments[i].pixelFormat;
            if (format == MTLPixelFormatInvalid && !attachment.image_view) continue;
            auto* native = pass.colorAttachments[i];
            native.texture = attachment_texture(attachment.image_view, format);
            if (!native.texture) return CommandResult::Unavailable;
            native.loadAction = attachment.is_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            native.storeAction = MTLStoreActionStore;
            double color[4];
            for (uint32_t c = 0; c < 4; ++c) {
                const auto bits = attachment.clear_value[c];
                float value; std::memcpy(&value, &bits, sizeof(value));
                color[c] = TextureScalar(format) == 1 ? double(bits) : TextureScalar(format) == 2 ? double(int32_t(bits)) : value;
            }
            native.clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
        }
        const auto& ds = state.depth_stencil_attachment;
        if (ds.has_depth) {
            pass.depthAttachment.texture = attachment_texture(ds.image_view, impl->descriptor.depthAttachmentPixelFormat);
            if (!pass.depthAttachment.texture) return CommandResult::Unavailable;
            pass.depthAttachment.loadAction = ds.depth_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            float clear; std::memcpy(&clear, ds.clear_value.data(), sizeof(clear));
            pass.depthAttachment.clearDepth = clear;
        }
        if (ds.has_stencil) {
            pass.stencilAttachment.texture = attachment_texture(ds.image_view, impl->descriptor.stencilAttachmentPixelFormat);
            if (!pass.stencilAttachment.texture) return CommandResult::Unavailable;
            pass.stencilAttachment.loadAction = ds.stencil_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = ds.clear_value[1];
        }
        const auto& front = dynamic.stencil_front_ops;
        const auto& back = dynamic.stencil_back_ops;
        const std::array<uint32_t, 16> depth_key{uint32_t(dynamic.depth_test_enabled), uint32_t(dynamic.depth_write_enabled),
            uint32_t(dynamic.depth_compare_op), uint32_t(dynamic.stencil_test_enabled), uint32_t(front.fail_op), uint32_t(front.pass_op),
            uint32_t(front.depth_fail_op), uint32_t(front.compare_op), dynamic.stencil_front_compare_mask, dynamic.stencil_front_write_mask,
            uint32_t(back.fail_op), uint32_t(back.pass_op), uint32_t(back.depth_fail_op), uint32_t(back.compare_op),
            dynamic.stencil_back_compare_mask, dynamic.stencil_back_write_mask};
        auto depth = impl->depth_states.find(depth_key);
        if (depth == impl->depth_states.end()) {
            MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
            desc.depthCompareFunction = dynamic.depth_test_enabled ? MTLCompareFunction(dynamic.depth_compare_op) : MTLCompareFunctionAlways;
            desc.depthWriteEnabled = dynamic.depth_test_enabled && dynamic.depth_write_enabled;
            if (dynamic.stencil_test_enabled) {
                const auto stencil = [](const Vulkan::StencilOps& ops, uint32_t read, uint32_t write) {
                    MTLStencilDescriptor* desc = [MTLStencilDescriptor new];
                    desc.stencilFailureOperation = MTLStencilOperation(ops.fail_op);
                    desc.depthStencilPassOperation = MTLStencilOperation(ops.pass_op);
                    desc.depthFailureOperation = MTLStencilOperation(ops.depth_fail_op);
                    desc.stencilCompareFunction = MTLCompareFunction(ops.compare_op);
                    desc.readMask = read; desc.writeMask = write;
                    return desc;
                };
                desc.frontFaceStencil = stencil(front, dynamic.stencil_front_compare_mask, dynamic.stencil_front_write_mask);
                desc.backFaceStencil = stencil(back, dynamic.stencil_back_compare_mask, dynamic.stencil_back_write_mask);
            }
            auto native = [vs.device newDepthStencilStateWithDescriptor:desc];
            if (!native) return CommandResult::Unavailable;
            depth = impl->depth_states.emplace(depth_key, native).first;
        }
        id<MTLBuffer> indices = (__bridge id<MTLBuffer>)draw.index_buffer;
        if (indices && (indices.device != vs.device || draw.index_type > VK_INDEX_TYPE_UINT32 ||
                draw.index_offset % (draw.index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4) || draw.index_offset > indices.length ||
                uint64_t(draw.count) * (draw.index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4) > indices.length - draw.index_offset))
            return CommandResult::Unavailable;
        id<MTLBuffer> indirect = (__bridge id<MTLBuffer>)draw.indirect_buffer;
        if (draw.indirect_buffer || draw.indirect_count || draw.indirect_stride) {
            const uint64_t argument_size = indices ? sizeof(MTLDrawIndexedPrimitivesIndirectArguments)
                                                   : sizeof(MTLDrawPrimitivesIndirectArguments);
            if (!indirect || indirect.device != vs.device || !draw.indirect_count ||
                draw.indirect_offset % 4 || draw.indirect_stride < argument_size || draw.indirect_stride % 4 ||
                draw.indirect_count - 1 > (UINT64_MAX - draw.indirect_offset) / draw.indirect_stride)
                return CommandResult::Unavailable;
            const uint64_t last = draw.indirect_offset + uint64_t(draw.indirect_count - 1) * draw.indirect_stride;
            if (last > indirect.length || argument_size > indirect.length - last) return CommandResult::Unavailable;
        }
        const auto primitive = MTLPrimitiveType(impl->topology);
        if (graphics_batch && !*graphics_batch)
            *graphics_batch = CreateGraphicsBatch((__bridge void*)vs.device);
        if (graphics_batch && !*graphics_batch) return CommandResult::Unavailable;
        bool submitted = false;
        const bool complete = RunCommands(vs.device, [&](id<MTLCommandBuffer> command, id<MTLFence> fence) {
            auto encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return false;
            [encoder waitForFence:fence beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
            [encoder setRenderPipelineState:found->second];
            [encoder setDepthStencilState:depth->second];
            [encoder setStencilFrontReferenceValue:dynamic.stencil_front_reference backReferenceValue:dynamic.stencil_back_reference];
            const uint32_t cull = uint32_t(dynamic.cull_mode);
            [encoder setCullMode:cull == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront : cull == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone];
            [encoder setFrontFacingWinding:dynamic.front_face == vk::FrontFace::eClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise];
            [encoder setTriangleFillMode:impl->polygon == VK_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
            [encoder setDepthClipMode:impl->clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
            [encoder setDepthBias:dynamic.depth_bias_enabled ? dynamic.depth_bias_constant : 0
                      slopeScale:dynamic.depth_bias_enabled ? dynamic.depth_bias_slope : 0 clamp:dynamic.depth_bias_enabled ? dynamic.depth_bias_clamp : 0];
            const auto& blend = dynamic.blend_constants;
            [encoder setBlendColorRed:blend[0] green:blend[1] blue:blend[2] alpha:blend[3]];
            std::vector<MTLViewport> viewports;
            for (const auto& v : dynamic.viewports) viewports.push_back({v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth});
            [encoder setViewports:viewports.data() count:viewports.size()];
            std::vector<MTLScissorRect> scissors;
            for (const auto& s : dynamic.scissors) {
                const uint32_t x = std::clamp<int64_t>(s.offset.x, 0, state.width), y = std::clamp<int64_t>(s.offset.y, 0, state.height);
                const uint32_t right = std::clamp<int64_t>(int64_t(s.offset.x) + s.extent.width, x, state.width);
                const uint32_t bottom = std::clamp<int64_t>(int64_t(s.offset.y) + s.extent.height, y, state.height);
                scissors.push_back({x, y, dynamic.rasterizer_discard_enable || cull == VK_CULL_MODE_FRONT_AND_BACK ? 0 : right-x, bottom-y});
            }
            [encoder setScissorRects:scissors.data() count:scissors.size()];
            for (const auto& vertex : vertices)
                [encoder setVertexBuffer:(__bridge id<MTLBuffer>)vertex.native offset:vertex.offset
                         attributeStride:vertex.input.stride atIndex:vertex.input.binding + 2];
            const auto bind = [&](ShaderFunction::Impl& shader, const ShaderFunction::Impl::Arguments& args, bool vertex) {
                if (args.encoded) {
                    if (vertex) [encoder setVertexBuffer:args.encoded offset:0 atIndex:shader.argument_slot];
                    else [encoder setFragmentBuffer:args.encoded offset:0 atIndex:shader.argument_slot];
                }
                if (shader.push_slot != UINT32_MAX) {
                    if (vertex) [encoder setVertexBytes:push.data() length:push.size() atIndex:shader.push_slot];
                    else [encoder setFragmentBytes:push.data() length:push.size() atIndex:shader.push_slot];
                }
                for (const auto& binding : args.used) if (binding.kind != ShaderResourceKind::Sampler)
                    [encoder useResource:(__bridge id<MTLResource>)binding.native
                        usage:MTLResourceUsageRead | (binding.written ? MTLResourceUsageWrite : 0)
                        stages:vertex ? MTLRenderStageVertex : MTLRenderStageFragment];
            };
            bind(vs, vertex_args, true);
            if (fs) bind(*fs, fragment_args, false);
            if (indirect) {
                [encoder useResource:indirect usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
                for (uint32_t i = 0; i < draw.indirect_count; ++i) {
                    const uint64_t offset = draw.indirect_offset + uint64_t(i) * draw.indirect_stride;
                    if (indices) [encoder drawIndexedPrimitives:primitive
                        indexType:draw.index_type == VK_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                        indexBuffer:indices indexBufferOffset:draw.index_offset
                        indirectBuffer:indirect indirectBufferOffset:offset];
                    else [encoder drawPrimitives:primitive indirectBuffer:indirect indirectBufferOffset:offset];
                }
            } else if (indices) [encoder drawIndexedPrimitives:primitive indexCount:draw.count
                indexType:draw.index_type == VK_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                indexBuffer:indices indexBufferOffset:draw.index_offset instanceCount:draw.instances baseVertex:draw.first_vertex baseInstance:draw.first_instance];
            else [encoder drawPrimitives:primitive vertexStart:draw.first_vertex vertexCount:draw.count
                           instanceCount:draw.instances baseInstance:draw.first_instance];
            [encoder endEncoding];
            return true;
        }, gpu_ms, &submitted, deferred, graphics_batch ? *graphics_batch : nullptr);
        return complete ? deferred ? CommandResult::Prepared : CommandResult::Complete : submitted ? CommandResult::Failed : CommandResult::Unavailable;
    }
}

} // namespace BbMetalFX
