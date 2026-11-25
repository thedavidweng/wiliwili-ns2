//
// Created by fang on 2023/4/1.
//

#include <borealis/core/application.hpp>
#include <borealis/views/label.hpp>

#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "utils/perf_diagnostics.hpp"

VideoProfile::VideoProfile() {
    this->inflateFromXMLRes("xml/views/video_profile.xml");
    brls::Logger::debug("View VideoProfile: create");
}

void VideoProfile::update() {
    auto mpvCore = &MPVCore::instance();

    // file
    if (mpvCore->filepath != labelUrl->getFullText()) labelUrl->setText(mpvCore->filepath);
    labelSize->setText(fmt::format("{:.2f}MB", mpvCore->getInt("file-size") / 1048576.0));
    labelFormat->setText(mpvCore->getString("file-format"));
    auto cache                 = mpvCore->getNodeMap("demuxer-cache-state");
    const double cacheDuration = mpvCore->getDouble("demuxer-cache-duration");
    const double cacheRateMBps = cache["raw-input-rate"].u.int64 / 1048576.0;
    labelCache->setText(fmt::format("{:.2f}MB ({:.1f} sec)", cache["fw-bytes"].u.int64 / 1048576.0, cacheDuration));
    brls::Logger::debug(
        "total-bytes: {:.2f}MB; cache-duration: "
        "{:.2f}; "
        "underrun: {}; fw-bytes: {:.2f}MB; bof-cached: "
        "{}; eof-cached: {}; file-cache-bytes: {}; "
        "raw-input-rate: {:.2f};",
        cache["total-bytes"].u.int64 / 1048576.0, cache["cache-duration"].u.double_, cache["underrun"].u.flag,
        cache["fw-bytes"].u.int64 / 1048576.0, cache["bof-cached"].u.flag, cache["eof-cached"].u.flag,
        cache["file-cache-bytes"].u.int64 / 1048576.0, cacheRateMBps);

    const int decodedW       = static_cast<int>(mpvCore->getInt("video-params/w"));
    const int decodedH       = static_cast<int>(mpvCore->getInt("video-params/h"));
    const int fps            = static_cast<int>(mpvCore->getInt("container-fps"));
    const int renderW        = static_cast<int>(brls::Application::windowWidth);
    const int renderH        = static_cast<int>(brls::Application::windowHeight);
    const std::string hwdec  = mpvCore->getString("hwdec-current");
    const int64_t decDrops   = mpvCore->getInt("decoder-frame-drop-count");
    const int64_t outDrops   = mpvCore->getInt("frame-drop-count");
    const double avsync      = mpvCore->getDouble("avsync");

    auto& perf = wili::PerfDiagnostics::instance();
    if (perf.isEnabled()) {
        const wili::PerfSnapshot current = perf.snapshot();
        const int outW = current.outputWidth > 0 ? current.outputWidth : renderW;
        const int outH = current.outputHeight > 0 ? current.outputHeight : renderH;
#if defined(MPV_SW_RENDER)
        const std::string transferMode = "SW";
#elif defined(MPV_NO_FB)
        const std::string transferMode = "NO_FB";
#elif defined(USE_GLES3)
        const std::string transferMode = "GLES3 FBO";
#else
        const std::string transferMode = "GL FBO";
#endif
        perf.updatePlaybackMetadata(current.sourceQn, decodedW, decodedH, renderW, renderH, outW, outH, current.docked,
                                    hwdec.empty() ? "software" : hwdec, transferMode, avsync, cacheDuration,
                                    cacheRateMBps, decDrops, outDrops);
    }

    const wili::PerfSnapshot snap = perf.snapshot();
    // video: keep source QN, decoded size, render size, and physical output distinct.
    labelVideoRes->setText(fmt::format(
        "QN:{} dec:{}x{}@{} ren:{}x{} out:{}x{}{} {}Hz (UI {}x{})", snap.sourceQn, decodedW, decodedH, fps, renderW,
        renderH, snap.outputWidth > 0 ? snap.outputWidth : renderW, snap.outputHeight > 0 ? snap.outputHeight : renderH,
        snap.docked ? "D" : "H", snap.nominalRefreshHz, brls::Application::contentWidth,
        brls::Application::contentHeight));
    labelVideoCodec->setText(mpvCore->getString("video-codec"));
    labelVideoPixel->setText(mpvCore->getString("video-params/pixelformat"));
    labelVideoHW->setText(fmt::format("{} ({})", hwdec.empty() ? "no" : hwdec, snap.transferMode));
    labelVideoBitrate->setText(std::to_string(mpvCore->getInt("video-bitrate") / 1024) + "kbps");
    if (perf.isEnabled() && snap.ui.count > 0) {
        labelVideoDrop->setText(fmt::format("{} (dec) {} (out) | p50/p95/p99 {:.1f}/{:.1f}/{:.1f}ms miss {}",
                                            decDrops, outDrops, snap.ui.p50Us / 1000.0, snap.ui.p95Us / 1000.0,
                                            snap.ui.p99Us / 1000.0, snap.ui.missedFrames));
    } else {
        labelVideoDrop->setText(fmt::format("{} (decoder) {} (output)", decDrops, outDrops));
    }
    labelVideoSync->setText(fmt::format("{:.5f}", avsync));

    // audio
    labelAudioCodec->setText(mpvCore->getString("audio-codec"));
    labelAudioChannel->setText(mpvCore->getString("audio-params/channel-count"));
    labelAudioSampleRate->setText(std::to_string(mpvCore->getInt("audio-params/samplerate") / 1000) + "kHz");
    labelAudioBitrate->setText(std::to_string(mpvCore->getInt("audio-bitrate") / 1024) + "kbps");
}

void VideoProfile::draw(NVGcontext *vg, float x, float y, float width, float height, brls::Style style,
                        brls::FrameContext *ctx) {
    static brls::Time time = 0;
    brls::Time now         = brls::getCPUTimeUsec();
    if ((now - time) > 1000000) {
        this->update();
        time = now;
    }

    Box::draw(vg, x, y, width, height, style, ctx);
}

VideoProfile::~VideoProfile() { brls::Logger::debug("View VideoProfile: delete"); }

brls::View *VideoProfile::create() { return new VideoProfile(); }