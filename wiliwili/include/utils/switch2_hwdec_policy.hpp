#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

namespace wili {

enum class HwdecTransferPath {
    SoftwareDecode,
    HwDecodeCpuCopyBack,
    HwDecodeRefAvFramePitchLinear,
    HwDecodeGpuDirectShared,
};

inline const char* hwdecTransferPathName(HwdecTransferPath path) {
    switch (path) {
        case HwdecTransferPath::SoftwareDecode:
            return "sw-decode";
        case HwdecTransferPath::HwDecodeCpuCopyBack:
            return "hw-decode-cpu-copy-back";
        case HwdecTransferPath::HwDecodeRefAvFramePitchLinear:
            return "hw-decode-ref-avframe-pitch-linear";
        case HwdecTransferPath::HwDecodeGpuDirectShared:
            return "hw-decode-gpu-direct-shared";
    }
    return "sw-decode";
}

struct OunceInitTelemetry {
    bool seen               = false;
    bool initOk             = false;
    uint32_t rc             = 0;
    int profile             = 0;
    int level               = 0;
    int visibleWidth        = 0;
    int visibleHeight       = 0;
    int codedWidth          = 0;
    int codedHeight         = 0;
    bool tier1088Triggered  = false;
    int fps                 = 0;
    int tilingMode          = 0;
    uint32_t colorPolicy    = 0;
    uint32_t imageCount     = 0;
    uint64_t workSizeBytes  = 0;
    uint64_t imageBackingBytes = 0;
    size_t codecConfigBytes = 0;
    std::string rejectReason;
    std::string errorStage;
    uint32_t errorRc = 0;
};

namespace detail {

inline std::string toLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

inline int align16(int v) { return (v > 0) ? ((v + 15) & ~15) : 0; }

inline bool extractKV(const std::string& line, const char* key, std::string& outValue) {
    const std::string needle = std::string(key) + "=";
    const size_t pos         = line.find(needle);
    if (pos == std::string::npos) return false;
    if (pos > 0 && line[pos - 1] != ' ' && line[pos - 1] != ':') return false;
    const size_t start = pos + needle.size();
    size_t end         = line.find_first_of(" \r\n\t", start);
    if (end == std::string::npos) end = line.size();
    outValue = line.substr(start, end - start);
    return true;
}

inline uint64_t parseU64(const std::string& s, int base = 10) {
    char* end = nullptr;
    return static_cast<uint64_t>(std::strtoull(s.c_str(), &end, base));
}

inline bool parseDims(const std::string& s, int& w, int& h) {
    const size_t x = s.find('x');
    if (x == std::string::npos) return false;
    w = std::atoi(s.substr(0, x).c_str());
    h = std::atoi(s.substr(x + 1).c_str());
    return w > 0 && h > 0;
}

}  // namespace detail

inline bool parseOunceLogLine(const std::string& line, OunceInitTelemetry& telemetry, bool& isRuntimeError,
                              bool& isIneligible) {
    isRuntimeError = false;
    isIneligible   = false;
    if (line.find("[ounce_dec]") == std::string::npos) return false;

    std::string val;
    if (line.find("[ounce_dec] init ok:") != std::string::npos) {
        telemetry        = OunceInitTelemetry{};
        telemetry.seen   = true;
        telemetry.initOk = true;
        telemetry.rc     = 0;
        if (detail::extractKV(line, "profile", val)) telemetry.profile = std::atoi(val.c_str());
        if (detail::extractKV(line, "level", val)) telemetry.level = std::atoi(val.c_str());
        if (detail::extractKV(line, "vis", val)) detail::parseDims(val, telemetry.visibleWidth, telemetry.visibleHeight);
        if (detail::extractKV(line, "coded", val)) detail::parseDims(val, telemetry.codedWidth, telemetry.codedHeight);
        if (detail::extractKV(line, "tier_1088", val)) telemetry.tier1088Triggered = (std::atoi(val.c_str()) != 0);
        if (detail::extractKV(line, "fps", val)) telemetry.fps = std::atoi(val.c_str());
        if (detail::extractKV(line, "tiling", val)) telemetry.tilingMode = std::atoi(val.c_str());
        if (detail::extractKV(line, "color_policy", val)) telemetry.colorPolicy = static_cast<uint32_t>(detail::parseU64(val));
        if (detail::extractKV(line, "images", val)) telemetry.imageCount = static_cast<uint32_t>(detail::parseU64(val));
        if (detail::extractKV(line, "work_size", val)) telemetry.workSizeBytes = detail::parseU64(val);
        if (detail::extractKV(line, "backing_size", val)) telemetry.imageBackingBytes = detail::parseU64(val);
        if (detail::extractKV(line, "codec_config", val)) telemetry.codecConfigBytes = static_cast<size_t>(detail::parseU64(val));
        return true;
    }

    if (line.find("[ounce_dec] init fail:") != std::string::npos) {
        telemetry.seen   = true;
        telemetry.initOk = false;
        if (detail::extractKV(line, "rc", val)) telemetry.rc = static_cast<uint32_t>(detail::parseU64(val, 0));
        if (detail::extractKV(line, "profile", val)) telemetry.profile = std::atoi(val.c_str());
        if (detail::extractKV(line, "level", val)) telemetry.level = std::atoi(val.c_str());
        if (detail::extractKV(line, "vis", val)) detail::parseDims(val, telemetry.visibleWidth, telemetry.visibleHeight);
        if (detail::extractKV(line, "coded", val)) detail::parseDims(val, telemetry.codedWidth, telemetry.codedHeight);
        if (detail::extractKV(line, "tier_1088", val)) telemetry.tier1088Triggered = (std::atoi(val.c_str()) != 0);
        if (detail::extractKV(line, "fps", val)) telemetry.fps = std::atoi(val.c_str());
        if (detail::extractKV(line, "tiling", val)) telemetry.tilingMode = std::atoi(val.c_str());
        if (detail::extractKV(line, "work_size", val)) telemetry.workSizeBytes = detail::parseU64(val);
        if (detail::extractKV(line, "codec_config", val)) telemetry.codecConfigBytes = static_cast<size_t>(detail::parseU64(val));
        return true;
    }

    if (line.find("[ounce_dec] ineligible:") != std::string::npos) {
        isIneligible = true;
        telemetry.seen = true;
        telemetry.initOk = false;
        if (detail::extractKV(line, "reason", val)) telemetry.rejectReason = val;
        return true;
    }

    if (line.find("[ounce_dec] runtime error:") != std::string::npos) {
        isRuntimeError = true;
        if (detail::extractKV(line, "stage", val)) telemetry.errorStage = val;
        if (detail::extractKV(line, "rc", val)) telemetry.errorRc = static_cast<uint32_t>(detail::parseU64(val, 0));
        return true;
    }

    return false;
}

struct StreamEligibilityQuery {
    std::string codecName;
    int width       = 0;
    int height      = 0;
    int codedWidth  = 0;
    int codedHeight = 0;
    int bitDepth    = 0;  // 0 = unknown
    std::string profile;
    std::string pixelFormat;
    double fps = 0.0;
};

struct EligibilityDecision {
    bool useHardware = false;
    std::string mpvHwdecOption = "no";
    std::string reason;
};

inline bool isH264ProfileUnsupported(const std::string& profileRaw) {
    const std::string p = detail::toLowerAscii(profileRaw);
    if (p.empty()) return false;
    return p.find("high 10") != std::string::npos || p.find("high10") != std::string::npos ||
           p.find("4:2:2") != std::string::npos || p.find("4:4:4") != std::string::npos ||
           p.find("422") != std::string::npos || p.find("444") != std::string::npos ||
           p == "110" || p == "122" || p == "144" || p == "244";
}

inline bool is8Bit420PixFmt(const std::string& fmtRaw) {
    const std::string f = detail::toLowerAscii(fmtRaw);
    return f == "yuv420p" || f == "yuvj420p" || f == "nv12";
}

inline EligibilityDecision evaluateOunceEligibility(const StreamEligibilityQuery& q,
                                                    bool userHwdecEnabled,
                                                    bool sessionSwLatched,
                                                    bool vp9Qualified = false,
                                                    bool vp8Qualified = false,
                                                    bool av1Qualified = false) {
    if (!userHwdecEnabled) return {false, "no", "user_disabled"};
    if (sessionSwLatched) return {false, "no", "session_fallback_latched"};

    const std::string codec = detail::toLowerAscii(q.codecName);
    if (codec.empty()) return {false, "no", "unknown_codec"};

    const bool isH264 = (codec == "h264" || codec == "avc" || codec == "avc1" || codec == "h264_ounce");
    const bool isVp9  = (codec == "vp9" || codec == "vp09" || codec == "vp9_ounce");
    const bool isVp8  = (codec == "vp8" || codec == "vp8_ounce");
    const bool isAv1  = (codec == "av1" || codec == "av01" || codec == "libdav1d" || codec == "av1_ounce");

    if (isVp9 && !vp9Qualified) return {false, "no", "codec_unqualified"};
    if (isVp8 && !vp8Qualified) return {false, "no", "codec_unqualified"};
    if (isAv1 && !av1Qualified) return {false, "no", "codec_unqualified"};
    if (!isH264 && !isVp9 && !isVp8 && !isAv1) return {false, "no", "codec_unqualified"};

    if (!q.pixelFormat.empty() && !is8Bit420PixFmt(q.pixelFormat)) {
        return {false, "no", "pixfmt_unsupported"};
    }
    if (q.bitDepth > 0 && q.bitDepth != 8) {
        return {false, "no", "bit_depth_unsupported"};
    }
    if (isH264 && isH264ProfileUnsupported(q.profile)) {
        return {false, "no", "profile_unsupported"};
    }

    // Require established 8-bit metadata before qualifying hardware decoding.
    const bool depthEstablished = (q.bitDepth == 8) || (!q.pixelFormat.empty() && is8Bit420PixFmt(q.pixelFormat));
    if (!depthEstablished || q.width <= 0 || q.height <= 0) {
        return {false, "no", "unknown_metadata"};
    }

    const int cw = q.codedWidth > 0 ? detail::align16(q.codedWidth) : detail::align16(q.width);
    const int ch = q.codedHeight > 0 ? detail::align16(q.codedHeight) : detail::align16(q.height);

    if (isAv1) {
        if (q.fps > 60.5) return {false, "no", "fps_exceeds_60"};
        const double evalFps = q.fps > 0.0 ? q.fps : 30.0;
        const double rate    = static_cast<double>(q.width) * static_cast<double>(q.height) * evalFps;
        if (rate > 248832000.0 * 1.01) return {false, "no", "pixel_rate_exceeded"};
        return {true, "ounce-copy", "eligible"};
    }

    if (static_cast<int64_t>(cw) * static_cast<int64_t>(ch) > static_cast<int64_t>(1920) * 1088 ||
        std::max(cw, ch) > 1920 || std::min(cw, ch) > 1088) {
        return {false, "no", "resolution_exceeds_1080p"};
    }
    if (q.fps > 60.5) return {false, "no", "fps_exceeds_60"};

    return {true, "ounce-copy", "eligible"};
}

inline HwdecTransferPath classifyTransferPath(const std::string& hwdecCurrent) {
    if (hwdecCurrent.empty() || hwdecCurrent == "no") {
        return HwdecTransferPath::SoftwareDecode;
    }
    if (hwdecCurrent == "ounce-copy" || hwdecCurrent == "h264_ounce-copy") {
        return HwdecTransferPath::HwDecodeCpuCopyBack;
    }
    return HwdecTransferPath::SoftwareDecode;
}

class OunceSessionWatchdog {
public:
    static constexpr int64_t DEFAULT_STALL_TIMEOUT_US = 4000000;  // 4 seconds of active playback with no progress

    struct SavedPlaybackState {
        int64_t positionSec = 0;
        double speed        = 1.0;
        std::string aid;
        std::string sid;
        bool wasPaused = false;
    };

    enum class Action {
        None,
        FallbackToSoftware,
    };

    explicit OunceSessionWatchdog(int64_t stallTimeoutUs = DEFAULT_STALL_TIMEOUT_US)
        : stallTimeoutUs_(stallTimeoutUs > 0 ? stallTimeoutUs : DEFAULT_STALL_TIMEOUT_US) {}

    // Reset per-stream state when opening a brand new video/episode/channel.
    // Reevaluates hardware eligibility for subsequent streams.
    void onNewStream() {
        swLatched_             = false;
        hwAttempted_           = false;
        hwActive_              = false;
        fallbackTriggered_     = false;
        restorePending_        = false;
        lastProgressTimeSec_   = -1.0;
        lastDecodedFrames_     = -1;
        lastProgressWallUs_    = 0;
        hwErrorCount_          = 0;
        savedState_            = SavedPlaybackState{};
    }

    // Arm software fallback for the current playback session so we reopen
    // cleanly once in SW mode and never loop back to HW for this stream.
    void armFallbackReopen(const SavedPlaybackState& state) {
        swLatched_         = true;
        hwActive_          = false;
        fallbackTriggered_ = true;
        restorePending_    = true;
        savedState_        = state;
    }

    bool consumeRestoreState(SavedPlaybackState& out) {
        if (!restorePending_) return false;
        out             = savedState_;
        restorePending_ = false;
        return true;
    }

    void onHwdecSelected(const std::string& hwdecCurrent) {
        if (hwdecCurrent == "ounce-copy" || hwdecCurrent == "h264_ounce-copy") {
            hwAttempted_ = true;
            hwActive_    = true;
        } else {
            hwActive_ = false;
        }
    }

    Action onHwRuntimeError() {
        ++hwErrorCount_;
        if ((hwActive_ || hwAttempted_) && !swLatched_ && !fallbackTriggered_) {
            swLatched_         = true;
            fallbackTriggered_ = true;
            return Action::FallbackToSoftware;
        }
        return Action::None;
    }

    Action onStreamFailure(bool isEof, int64_t positionSec, int64_t durationSec) {
        const bool nearEnd = isEof && durationSec > 0 && (durationSec - positionSec) <= 5;
        if (nearEnd) return Action::None;
        if ((hwActive_ || hwAttempted_) && !swLatched_ && !fallbackTriggered_) {
            swLatched_         = true;
            fallbackTriggered_ = true;
            return Action::FallbackToSoftware;
        }
        return Action::None;
    }

    Action onPlaybackSample(double playbackTimeSec, int64_t decodedFrames, bool paused, bool seeking,
                            bool pausedForCache, bool eof, int64_t nowUs) {
        const bool timeAdvanced =
            (lastProgressTimeSec_ < 0.0) ? (playbackTimeSec > 0.0) : (playbackTimeSec - lastProgressTimeSec_ > 1e-4);
        const bool frameAdvanced = (decodedFrames > lastDecodedFrames_ && decodedFrames > 0);

        if (timeAdvanced || frameAdvanced) {
            if (playbackTimeSec > lastProgressTimeSec_) lastProgressTimeSec_ = playbackTimeSec;
            if (decodedFrames > lastDecodedFrames_) lastDecodedFrames_ = decodedFrames;
            lastProgressWallUs_ = nowUs;
            return Action::None;
        }

        if (!hwActive_ || swLatched_ || fallbackTriggered_) {
            lastProgressWallUs_ = nowUs;
            return Action::None;
        }

        if (paused || seeking || pausedForCache || eof) {
            lastProgressWallUs_ = nowUs;
            return Action::None;
        }

        if (lastProgressWallUs_ <= 0) {
            lastProgressWallUs_ = nowUs;
            return Action::None;
        }

        if (nowUs - lastProgressWallUs_ >= stallTimeoutUs_) {
            swLatched_         = true;
            fallbackTriggered_ = true;
            return Action::FallbackToSoftware;
        }
        return Action::None;
    }

    bool isSwFallbackLatched() const { return swLatched_; }
    bool wasHwAttempted() const { return hwAttempted_; }
    bool isHwActive() const { return hwActive_; }
    bool wasFallbackTriggered() const { return fallbackTriggered_; }
    bool hasPendingRestore() const { return restorePending_; }
    int hwErrorCount() const { return hwErrorCount_; }

private:
    int64_t stallTimeoutUs_;
    bool swLatched_           = false;
    bool hwAttempted_         = false;
    bool hwActive_            = false;
    bool fallbackTriggered_   = false;
    bool restorePending_      = false;
    double lastProgressTimeSec_ = -1.0;
    int64_t lastDecodedFrames_  = -1;
    int64_t lastProgressWallUs_ = 0;
    int hwErrorCount_           = 0;
    SavedPlaybackState savedState_{};
};

}  // namespace wili
