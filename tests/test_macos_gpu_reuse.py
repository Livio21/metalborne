#!/usr/bin/env python3
"""Compile and check the production memory selector, warmup policy and job loop.

No game/driver required. GPU compilation and gameplay need separate checks.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fsr = (root / "gpu/third_party/fsr-vulkan/upstream/ffx-1.1.4/sdk/src/backends/vk/ffx_vk.cpp").read_text()
warmup = (root / "gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp").read_text()
memory = fsr[fsr.index("uint32_t findMemoryTypeIndex("):fsr.index("VkBufferUsageFlags ffxGetVKBufferUsageFlags")]
policy = warmup.split("const u32 preload_threads = [] {", 1)[1].split("}();", 1)[0]
jobs = warmup.split("    if (!jobs.empty()) {", 1)[1].split("    // bbport: always reported", 1)[0]
code = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#define FFX_ASSERT assert
#define LOG_WARNING(...) ((void)0)
using u32 = uint32_t;
using VkPhysicalDevice = void*;
using VkBool32 = bool;
using VkMemoryPropertyFlags = uint32_t;
constexpr uint32_t VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2;
constexpr uint32_t VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4;
constexpr uint32_t VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD = 8;
struct VkMemoryRequirements { uint32_t memoryTypeBits; };
struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags; } memoryTypes[32];
} props;
void vkGetPhysicalDeviceMemoryProperties(void*, VkPhysicalDeviceMemoryProperties* p) { *p = props; }
""" + memory + "\nu32 Workers() {" + policy + "}\n" + r"""
struct Job { std::function<void()> build, finish; };
void RunJobs(std::vector<Job>& jobs, u32 preload_threads) {
    if (!jobs.empty()) {
""" + jobs + r"""
}
int main() {
    for (const char* invalid : {"", "0", "-1", "17", "4x", "999999999999999999999"}) {
        unsetenv("BB_PRELOAD_THREADS"); const auto expected = Workers();
        setenv("BB_PRELOAD_THREADS", invalid, 1); assert(Workers() == expected);
    }
    for (const char* valid : {"1", "4", "16"}) {
        setenv("BB_PRELOAD_THREADS", valid, 1); assert(Workers() == std::atoi(valid));
    }
    uint32_t selected = 0;
    const auto pick = [&](uint32_t bits, uint32_t requested = 1, bool coherent = false) {
        return findMemoryTypeIndex(&props, {bits}, requested, coherent, selected);
    };
    props = {1, {{3}}}; assert(pick(1) == 0 && selected == 3); // unified memory
    props = {2, {{3}, {1}}}; assert(pick(3) == 1 && selected == 1); // discrete preference
    assert(pick(1) == 0 && selected == 3); assert(pick(0) == UINT32_MAX);
    props = {2, {{11}, {3}}}; assert(pick(3) == 1 && selected == 3); // AMD guard
    assert(pick(1) == UINT32_MAX); assert(pick(1, 1, true) == 0 && selected == 11);
    props = {2, {{3}, {7}}}; assert(pick(3, 2) == 1 && selected == 7); // host coherency
    const auto main_thread = std::this_thread::get_id();
    for (u32 threads : {1, 4, 16}) for (size_t size : {0, 1, 37}) {
        std::vector<std::atomic<int>> built(size);
        std::vector<int> finished(size);
        std::vector<Job> work;
        for (size_t i = 0; i < size; ++i) work.push_back({
            [&, i] { assert(built[i].fetch_add(1) == 0); },
            [&, i] {
                assert(std::this_thread::get_id() == main_thread);
                for (const auto& b : built) assert(b.load() == 1);
                assert(finished[i]++ == 0);
            }});
        RunJobs(work, threads);
        for (int count : finished) assert(count == 1);
    }
}
"""
with tempfile.TemporaryDirectory(prefix="metalborne-gpu-reuse-") as temporary:
    source = Path(temporary) / "check.cpp"
    executable = Path(temporary) / "check"
    source.write_text(code)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++23", "-pthread", "-UNDEBUG",
                    str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("GPU reuse: memory selection, worker limits and completion ordering PASS")
