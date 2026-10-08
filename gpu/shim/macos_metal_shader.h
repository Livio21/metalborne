// Native compilation of bbport's translated guest shaders. No game shaders are embedded.
#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <functional>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan { struct RenderState; struct DynamicState; }

namespace BbMetalFX {

enum class ShaderResourceKind { Buffer, Texture, Sampler };
struct ShaderResource {
    ShaderResourceKind kind;
    uint32_t binding;
    uint32_t argument;
    uint32_t count;
    uint32_t texture_type = UINT32_MAX;
    uint32_t scalar = 0; // float, unsigned integer, signed integer
    bool depth = false;
};
struct ShaderBinding {
    ShaderResourceKind kind;
    uint32_t binding;
    uint32_t element;
    void* native;
    uint64_t offset;
    uint64_t size;
    bool written;
};
enum class CommandResult { Unavailable, Prepared, Complete, Failed };

class ShaderFunction {
public:
    explicit ShaderFunction(std::span<const uint32_t> spirv, void* device = nullptr, bool fix_clip = false);
    ~ShaderFunction();
    bool Available() const;
    uint32_t Stage() const;
    void* Device() const;
    void* NativeHandle() const;
    uint32_t ArgumentSlot() const;
    uint32_t PushSlot() const;
    std::span<const ShaderResource> Resources() const;
    std::string_view Error() const;
    friend class ComputeKernel;
    friend class RenderPipeline;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class ComputeKernel {
public:
    explicit ComputeKernel(std::span<const uint32_t> spirv, void* device = nullptr);
    ~ComputeKernel();
    bool Available() const;
    std::string_view Error() const;
    std::array<uint32_t, 3> WorkgroupSize() const;
    std::span<const ShaderResource> Resources() const;
    // Caller releases all Vulkan resources and completes earlier work before dispatch.
    // Complete/Failed mean a command was submitted; never replay Failed through Vulkan.
    CommandResult Dispatch(std::span<const ShaderBinding> bindings, std::span<const uint8_t> push,
                           std::array<uint32_t, 3> groups, float* gpu_ms = nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct VertexBufferBinding {
    VkVertexInputBindingDescription2EXT input;
    void* native;
    uint64_t offset, size;
};
struct DrawCommand {
    uint32_t count, instances, first_instance;
    int32_t first_vertex;
    void* index_buffer = nullptr;
    uint64_t index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
};

class RenderPipeline {
public:
    RenderPipeline(std::span<const uint32_t> vertex, std::span<const uint32_t> fragment,
                   const VkGraphicsPipelineCreateInfo& info, void* device = nullptr);
    ~RenderPipeline();
    bool Available() const;
    std::string_view Error() const;
    std::span<const ShaderResource> Resources(uint32_t stage) const;
    // State and resources are the exact ones prepared for the Vulkan draw.
    CommandResult Draw(const Vulkan::RenderState&, const Vulkan::DynamicState&,
                       std::span<const VkVertexInputAttributeDescription2EXT>,
                       std::span<const VertexBufferBinding>, std::span<const ShaderBinding>,
                       std::span<const uint8_t> push, const DrawCommand&, float* gpu_ms = nullptr,
                       std::function<bool(float*)>* deferred = nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace BbMetalFX
