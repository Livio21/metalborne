// Experimental shared Metal resources, presentation and MetalFX.
#pragma once
#include <memory>
#include <span>
#include <vulkan/vulkan_core.h>

namespace BbMetalFX {
struct ComputeBuffer {
    std::span<const uint8_t> before, reference;
    void* native{}; // Optional GPU-populated shared clone; retained by SharedBuffer.
};
class SharedBuffer {
public:
    SharedBuffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties,
                 uint64_t size, PFN_vkGetDeviceProcAddr proc,
                 VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ~SharedBuffer();
    VkBuffer Handle() const;
    void* NativeHandle() const;
    uint8_t* MappedData() const;
    uint64_t DeviceAddress() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Caller completes Vulkan release before entering, then acquires after completion.
bool CopyBuffers(void* source, void* destination, std::span<const VkBufferCopy> copies);
class SharedImage {
public:
    SharedImage(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                const VkImageCreateInfo& info, PFN_vkGetInstanceProcAddr instance_proc,
                PFN_vkGetDeviceProcAddr device_proc);
    ~SharedImage();
    VkImage Handle() const;
    void* NativeHandle() const;
    uint64_t SizeBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Same release/completion/acquire contract as CopyBuffers; no format conversion.
bool CopyImages(void* source, void* destination, std::span<const VkImageCopy> copies);
// Shared uncompressed color storage; buffer pitches follow VkBufferImageCopy.
bool CopyBufferImage(void* buffer, void* image, std::span<const VkBufferImageCopy> copies,
                     bool upload);
// Same ownership contract; clears complete mip/layer subresources through render passes.
bool ClearImage(void* image, const VkImageSubresourceRange& range, const VkClearColorValue& color);
// One-shot shadow dispatch: clones buffers and leaves the live Vulkan resources untouched.
bool CheckCompute(std::span<const ComputeBuffer> buffers, std::span<const uint8_t> push,
                  uint32_t groups, uint32_t threads);
struct Images {
    VkImage input{}, output{};
    uint32_t input_width{}, input_height{}, output_width{}, output_height{};
    bool initialized{};
};
class Presentation {
public:
    Presentation(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                 PFN_vkGetInstanceProcAddr instance_proc, PFN_vkGetDeviceProcAddr device_proc);
    ~Presentation();
    bool Available() const;
    void SetNativeLayer(void* layer);
    bool NativePresentation() const;
    VkFormat InputFormat() const;
    // Frame fence must be complete and its old image view destroyed before replacement.
    VkImage CreateFrameImage(uint8_t slot, uint32_t width, uint32_t height);
    const Images* Find(uint32_t slot) const;
    // Caller must complete earlier Vulkan reads before replacing these resources.
    Images* Prepare(uint32_t slot, uint32_t input_width, uint32_t input_height,
                    uint32_t output_width, uint32_t output_height);
    // Caller releases external ownership and waits for Vulkan before entering.
    // Returns only after Metal completes; never copies texture pixels to the CPU.
    bool Encode(uint32_t slot, uint32_t window_width = 0, uint32_t window_height = 0);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Objective-C bridge for the existing overlay context; caller holds its mutex.
bool OverlayInit(void* device);
void OverlayShutdown();
void OverlayNewFrame(void* pass);
void OverlayRender(void* data, void* command, void* encoder);
}
