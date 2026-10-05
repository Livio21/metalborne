// Experimental MetalFX spatial pass. Vulkan and Metal share dedicated GPU textures.
#pragma once
#include <memory>
#include <vulkan/vulkan_core.h>

namespace BbMetalFX {
struct Images {
    VkImage input{}, output{};
    uint32_t input_width{}, input_height{}, output_width{}, output_height{};
    bool initialized{};
};
class Spatial {
public:
    Spatial(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            PFN_vkGetInstanceProcAddr instance_proc, PFN_vkGetDeviceProcAddr device_proc);
    ~Spatial();
    bool Available() const;
    const Images* Find(uint32_t slot) const;
    // Caller must complete earlier Vulkan reads before replacing these resources.
    Images* Prepare(uint32_t slot, uint32_t input_width, uint32_t input_height,
                    uint32_t output_width, uint32_t output_height);
    // Caller releases external ownership and waits for Vulkan before entering.
    // Returns only after Metal completes; never copies texture pixels to the CPU.
    bool Encode(uint32_t slot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
