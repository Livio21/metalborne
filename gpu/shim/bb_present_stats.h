// Opt-in host API timing. These measurements do not measure display scanout.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

class BbPresentStats {
public:
    using Clock = std::chrono::steady_clock;
    static double Milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    void Observe(Clock::time_point now, double queue_ms, double work_ms, size_t depth) {
        if (last != Clock::time_point{}) intervals.push_back(Milliseconds(now - last));
        if (start == Clock::time_point{}) start = now;
        last = now;
        queue.push_back(queue_ms);
        work.push_back(work_ms);
        max_depth = std::max(max_depth, depth);
        if (now - start < std::chrono::seconds(5)) return;
        std::printf("Host present calls: interval p50 %.2f / p95 %.2f / p99 %.2f ms; "
                    "queue p50 %.2f / p99 %.2f ms; work p50 %.2f / p99 %.2f ms; "
                    "max queued %zu (API timing, not scanout)\n",
                    Percentile(intervals, .5), Percentile(intervals, .95), Percentile(intervals, .99),
                    Percentile(queue, .5), Percentile(queue, .99),
                    Percentile(work, .5), Percentile(work, .99), max_depth);
        intervals.clear(); queue.clear(); work.clear(); max_depth = 0; start = now;
    }
private:
    static double Percentile(std::vector<double>& samples, double fraction) {
        if (samples.empty()) return 0;
        std::sort(samples.begin(), samples.end());
        return samples[size_t(fraction * (samples.size() - 1))];
    }
    Clock::time_point start{}, last{};
    std::vector<double> intervals, queue, work;
    size_t max_depth{};
};

// CPU wall times around API stages, including any intentional backpressure.
template <size_t N>
class BbStageStats {
public:
    BbStageStats(const char* label_, std::array<const char*, N> names_)
        : label(label_), names(names_) {}
    void Observe(const std::array<double, N>& milliseconds) {
        const auto now = BbPresentStats::Clock::now();
        if (start == BbPresentStats::Clock::time_point{}) start = now;
        ++count;
        for (size_t i = 0; i < N; ++i) {
            sum[i] += milliseconds[i]; worst[i] = std::max(worst[i], milliseconds[i]);
        }
        if (now - start < std::chrono::seconds(5)) return;
        std::printf("%s (CPU wall time, mean / max ms):", label);
        for (size_t i = 0; i < N; ++i) std::printf(" %s %.2f / %.2f;", names[i], sum[i] / count, worst[i]);
        std::printf(" %u samples\n", count);
        sum.fill(0); worst.fill(0); count = 0; start = now;
    }
private:
    const char* label;
    std::array<const char*, N> names;
    std::array<double, N> sum{}, worst{};
    unsigned count{};
    BbPresentStats::Clock::time_point start{};
};
