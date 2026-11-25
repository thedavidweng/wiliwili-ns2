#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace wili {

// Fixed-capacity frame interval ring buffer. Zero heap allocations on record().
template <size_t Capacity = 1024>
class FrameIntervalRing {
public:
    struct Stats {
        size_t count         = 0;
        uint64_t minUs       = 0;
        uint64_t maxUs       = 0;
        uint64_t p50Us       = 0;
        uint64_t p95Us       = 0;
        uint64_t p99Us       = 0;
        double meanUs        = 0.0;
        double fps           = 0.0;
        size_t missedFrames  = 0;
        size_t delayedFrames = 0;
    };

    void clear() {
        head_  = 0;
        count_ = 0;
    }

    void push(uint64_t durationUs) {
        samples_[head_] = durationUs;
        head_           = (head_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    size_t size() const { return count_; }

    Stats compute(uint64_t budgetUs) const {
        Stats s;
        if (count_ == 0) return s;
        s.count = count_;

        std::array<uint64_t, Capacity> sorted{};
        uint64_t sum = 0;
        const size_t start = (count_ == Capacity) ? head_ : 0;
        for (size_t i = 0; i < count_; ++i) {
            const uint64_t v = samples_[(start + i) % Capacity];
            sorted[i]        = v;
            sum += v;
            if (budgetUs > 0) {
                if (v > budgetUs) ++s.missedFrames;
                if (v > (budgetUs * 3) / 2) ++s.delayedFrames;
            }
        }
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));

        s.minUs  = sorted[0];
        s.maxUs  = sorted[count_ - 1];
        s.p50Us  = percentile(sorted, count_, 50);
        s.p95Us  = percentile(sorted, count_, 95);
        s.p99Us  = percentile(sorted, count_, 99);
        s.meanUs = static_cast<double>(sum) / static_cast<double>(count_);
        if (s.meanUs > 0.0) s.fps = 1000000.0 / s.meanUs;
        return s;
    }

private:
    static uint64_t percentile(const std::array<uint64_t, Capacity>& sorted, size_t n, int pct) {
        if (n == 0) return 0;
        const size_t rank = (static_cast<size_t>(pct) * (n - 1) + 99) / 100;
        return sorted[std::min(rank, n - 1)];
    }

    std::array<uint64_t, Capacity> samples_{};
    size_t head_  = 0;
    size_t count_ = 0;
};

struct PerfSnapshot {
    FrameIntervalRing<>::Stats ui;
    FrameIntervalRing<>::Stats videoRender;
    int nominalRefreshHz       = 60;
    uint64_t budgetUs          = 16667;
    int sourceQn               = 0;
    int decodedWidth           = 0;
    int decodedHeight          = 0;
    int renderWidth            = 0;
    int renderHeight           = 0;
    int outputWidth            = 0;
    int outputHeight           = 0;
    bool docked                = false;
    bool vrr                   = false;
    std::string decoderName    = "software";
    std::string transferMode   = "GLES3 FBO";
    double avsyncSec           = 0.0;
    double cacheDurationSec    = 0.0;
    double cacheInputRateMBps  = 0.0;
    int64_t decoderDrops       = 0;
    int64_t outputDrops        = 0;
    uint64_t startupMs         = 0;
    uint64_t playerOpenMs      = 0;
    uint64_t currentMemoryKb   = 0;
    uint64_t peakMemoryKb      = 0;
};

// Lightweight, zero-credential opt-in performance diagnostics inspired by nx_prof.
// Disabled by default (single atomic load fast path). Never logs cookies,
// tokens, URLs, or personal identifiers.
class PerfDiagnostics {
public:
    static PerfDiagnostics& instance();

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    void setNominalRefreshHz(int hz, bool vrr = false);
    void setSourceQn(int qn);
    void setDisplayMetrics(int refreshHz, bool vrr, int outputW, int outputH, int renderW, int renderH, bool docked);
    void markStartup(uint64_t durationMs);
    void markPlayerOpen(uint64_t durationMs);

    // Records UI frame-to-frame interval from monotonic microsecond clock.
    void recordUiFrameTimestamp(int64_t nowUs);
    // Records an explicit video render/compose duration in microseconds.
    void recordVideoRenderDuration(uint64_t durationUs);

    void updatePlaybackMetadata(int sourceQn, int decodedWidth, int decodedHeight, int renderWidth, int renderHeight,
                                int outputWidth, int outputHeight, bool docked, const std::string& decoderName,
                                const std::string& transferMode, double avsyncSec, double cacheDurationSec,
                                double cacheInputRateMBps, int64_t decoderDrops, int64_t outputDrops);

    void resetSamples();
    PerfSnapshot snapshot();

    static uint64_t budgetForRefreshHz(int hz);
    static uint64_t queryProcessMemoryKb();
    static std::string formatSanitizedSummary(const PerfSnapshot& snap);

private:
    PerfDiagnostics() = default;

    std::atomic<bool> enabled_{false};
    std::mutex mutex_;
    FrameIntervalRing<> uiFrames_;
    FrameIntervalRing<> videoRenderTimes_;
    int64_t lastUiFrameUs_    = 0;
    int nominalRefreshHz_     = 60;
    bool vrr_                 = false;
    int sourceQn_             = 0;
    int decodedWidth_         = 0;
    int decodedHeight_        = 0;
    int renderWidth_          = 0;
    int renderHeight_         = 0;
    int outputWidth_          = 0;
    int outputHeight_         = 0;
    bool docked_              = false;
    std::string decoderName_  = "software";
    std::string transferMode_ = "GLES3 FBO";
    double avsyncSec_         = 0.0;
    double cacheDurationSec_  = 0.0;
    double cacheRateMBps_     = 0.0;
    int64_t decoderDrops_     = 0;
    int64_t outputDrops_      = 0;
    uint64_t startupMs_       = 0;
    uint64_t playerOpenMs_    = 0;
    uint64_t peakMemoryKb_    = 0;
};

}  // namespace wili
