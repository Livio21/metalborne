// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#ifdef __APPLE__
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>
#endif

namespace Vulkan {
#ifdef __APPLE__
// Compare the actual module bytes, including cache reloads and shader overrides.
// ponytail: one local reference per process; expand only when multiple proofs are needed.
static std::mutex reference_mutex;
static std::vector<vk::ShaderModule> reference_modules;
static const std::vector<u32>& MetalComputeReference() {
    static const auto reference = [] {
        std::vector<u32> words;
        const char* path = std::getenv("BB_METAL_COMPUTE_SPV");
        if (!path) return words;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size < 20 || size > 16 * 1024 * 1024 || size % 4) return words;
        words.resize(static_cast<size_t>(size) / 4);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(words.data()), size) || words[0] != 0x07230203)
            words.clear();
        return words;
    }();
    return reference;
}
bool IsMetalComputeReference(vk::ShaderModule module) {
    if (MetalComputeReference().empty()) return false;
    std::lock_guard lock(reference_mutex);
    return std::find(reference_modules.begin(), reference_modules.end(), module) != reference_modules.end();
}
#endif

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
#ifdef __APPLE__
    const auto& reference = MetalComputeReference();
    if (!reference.empty()) {
        std::lock_guard lock(reference_mutex);
        std::erase(reference_modules, module); // A recompiled module may reuse an old handle.
        if (std::ranges::equal(code, reference)) reference_modules.push_back(module);
    }
#endif
    return module;
}

} // namespace Vulkan
