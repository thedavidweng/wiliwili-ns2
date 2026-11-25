#include "utils/perf_diagnostics.hpp"

#include <fmt/format.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <malloc.h>
#elif defined(__SWITCH__) || defined(__SWITCH2__)
#include <malloc.h>
#endif

namespace wili {

PerfDiagnostics& PerfDiagnostics::instance() {
    static PerfDiagnostics instance;
    return instance;
}

void PerfDiagnostics::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled == enabled_.load(std::memory_order_relaxed)) return;
    enabled_.store(enabled, std::memory_order_relaxed);
    lastUiFrameUs_ = 0;
    if (enabled) {
        uiFrames_.clear();
        videoRenderTimes_.clear();
    }
}

void PerfDiagnostics::setNominalRefreshHz(int hz, bool vrr) {
    std::lock_guard<std::mutex> lock(mutex_);
    nominalRefreshHz_ = hz > 0 ? hz : 60;
    vrr_              = vrr;
}

void PerfDiagnostics::setSourceQn(int qn) {
    std::lock_guard<std::mutex> lock(mutex_);
    sourceQn_ = qn;
}

void PerfDiagnostics::setDisplayMetrics(int refreshHz, bool vrr, int outputW, int outputH, int renderW, int renderH,
                                        bool docked) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (refreshHz > 0) nominalRefreshHz_ = refreshHz;
    vrr_          = vrr;
    outputWidth_  = outputW;
    outputHeight_ = outputH;
    renderWidth_  = renderW;
    renderHeight_ = renderH;
    docked_       = docked;
}

void PerfDiagnostics::markStartup(uint64_t durationMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    startupMs_ = durationMs;
}

void PerfDiagnostics::markPlayerOpen(uint64_t durationMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    playerOpenMs_ = durationMs;
}

void PerfDiagnostics::recordUiFrameTimestamp(int64_t nowUs) {
    if (!isEnabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastUiFrameUs_ > 0 && nowUs > lastUiFrameUs_) {
        uiFrames_.push(static_cast<uint64_t>(nowUs - lastUiFrameUs_));
    }
    lastUiFrameUs_ = nowUs;
}

void PerfDiagnostics::recordVideoRenderDuration(uint64_t durationUs) {
    if (!isEnabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    videoRenderTimes_.push(durationUs);
}

void PerfDiagnostics::updatePlaybackMetadata(int sourceQn, int decodedWidth, int decodedHeight, int renderWidth,
                                             int renderHeight, int outputWidth, int outputHeight, bool docked,
                                             const std::string& decoderName, const std::string& transferMode,
                                             double avsyncSec, double cacheDurationSec, double cacheInputRateMBps,
                                             int64_t decoderDrops, int64_t outputDrops) {
    if (!isEnabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sourceQn_      = sourceQn;
    decodedWidth_  = decodedWidth;
    decodedHeight_ = decodedHeight;
    renderWidth_   = renderWidth;
    renderHeight_  = renderHeight;
    outputWidth_   = outputWidth;
    outputHeight_  = outputHeight;
    docked_        = docked;
    decoderName_   = decoderName.empty() ? "software" : decoderName;
    transferMode_  = transferMode.empty() ? "GLES3 FBO" : transferMode;
    avsyncSec_     = avsyncSec;
    cacheDurationSec_ = cacheDurationSec;
    cacheRateMBps_    = cacheInputRateMBps;
    decoderDrops_     = decoderDrops;
    outputDrops_      = outputDrops;
}

void PerfDiagnostics::resetSamples() {
    std::lock_guard<std::mutex> lock(mutex_);
    uiFrames_.clear();
    videoRenderTimes_.clear();
    lastUiFrameUs_ = 0;
}

uint64_t PerfDiagnostics::budgetForRefreshHz(int hz) {
    if (hz >= 120) return 8333;
    if (hz > 0) return static_cast<uint64_t>((1000000 + hz / 2) / hz);
    return 16667;
}

uint64_t PerfDiagnostics::queryProcessMemoryKb() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        return static_cast<uint64_t>(info.resident_size / 1024);
    }
    return 0;
#elif defined(__SWITCH__) || defined(__SWITCH2__) || defined(__linux__)
    struct mallinfo mi = mallinfo();
    if (mi.uordblks > 0) {
        return static_cast<uint64_t>(mi.uordblks / 1024);
    }
    return 0;
#else
    return 0;
#endif
}

PerfSnapshot PerfDiagnostics::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    PerfSnapshot snap;
    snap.nominalRefreshHz  = nominalRefreshHz_;
    snap.vrr               = vrr_;
    snap.budgetUs          = budgetForRefreshHz(nominalRefreshHz_);
    snap.ui                = uiFrames_.compute(snap.budgetUs);
    snap.videoRender       = videoRenderTimes_.compute(snap.budgetUs);
    snap.sourceQn          = sourceQn_;
    snap.decodedWidth      = decodedWidth_;
    snap.decodedHeight     = decodedHeight_;
    snap.renderWidth       = renderWidth_;
    snap.renderHeight      = renderHeight_;
    snap.outputWidth       = outputWidth_;
    snap.outputHeight      = outputHeight_;
    snap.docked            = docked_;
    snap.decoderName       = decoderName_;
    snap.transferMode      = transferMode_;
    snap.avsyncSec         = avsyncSec_;
    snap.cacheDurationSec  = cacheDurationSec_;
    snap.cacheInputRateMBps = cacheRateMBps_;
    snap.decoderDrops      = decoderDrops_;
    snap.outputDrops       = outputDrops_;
    snap.startupMs         = startupMs_;
    snap.playerOpenMs      = playerOpenMs_;
    snap.currentMemoryKb   = queryProcessMemoryKb();
    peakMemoryKb_          = std::max(peakMemoryKb_, snap.currentMemoryKb);
    snap.peakMemoryKb      = peakMemoryKb_;
    return snap;
}

std::string PerfDiagnostics::formatSanitizedSummary(const PerfSnapshot& snap) {
    return fmt::format(
        "ui_fps={:.1f} ui_p50_ms={:.2f} ui_p95_ms={:.2f} ui_p99_ms={:.2f} missed={} delayed={} "
        "video_p50_ms={:.2f} video_p95_ms={:.2f} video_p99_ms={:.2f} "
        "source_qn={} decoded={}x{} render={}x{} output={}x{}{} refresh={}Hz{} "
        "decoder={} transfer={} avsync_ms={:.2f} cache_sec={:.1f} cache_rate_mbps={:.2f} "
        "drop_dec={} drop_out={} mem_mb={:.1f} peak_mem_mb={:.1f} startup_ms={} player_open_ms={}",
        snap.ui.fps, snap.ui.p50Us / 1000.0, snap.ui.p95Us / 1000.0, snap.ui.p99Us / 1000.0, snap.ui.missedFrames,
        snap.ui.delayedFrames, snap.videoRender.p50Us / 1000.0, snap.videoRender.p95Us / 1000.0,
        snap.videoRender.p99Us / 1000.0, snap.sourceQn, snap.decodedWidth, snap.decodedHeight, snap.renderWidth,
        snap.renderHeight, snap.outputWidth, snap.outputHeight, snap.docked ? "_docked" : "_handheld",
        snap.nominalRefreshHz, snap.vrr ? "_vrr" : "", snap.decoderName, snap.transferMode, snap.avsyncSec * 1000.0,
        snap.cacheDurationSec, snap.cacheInputRateMBps, snap.decoderDrops, snap.outputDrops,
        snap.currentMemoryKb / 1024.0, snap.peakMemoryKb / 1024.0, snap.startupMs, snap.playerOpenMs);
}

}  // namespace wili
