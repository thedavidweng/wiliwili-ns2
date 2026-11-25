#include "test_check.hpp"

#include <nlohmann/json.hpp>

#include "utils/dash_selector.hpp"
#include "utils/perf_diagnostics.hpp"
#include "utils/playback_recovery.hpp"

namespace {

void check_playback_recovery() {
    wili::PlaybackRecovery recovery;

    // Natural EOF near end of stream (or within 5s of clipEnd) does not trigger recovery.
    CHECK(!recovery.onStreamFailure(598, 600, 0, true, 1000000));
    CHECK(!recovery.onStreamFailure(548, 600, 550, true, 1000000));
    CHECK(!recovery.pending());

    // Premature EOF mid-stream arms recovery at saved timestamp with 2s backoff.
    CHECK(recovery.onStreamFailure(123, 600, 0, true, 1000000));
    CHECK(recovery.pending());
    CHECK_EQ(recovery.savedPosition(), 123);
    CHECK(!recovery.shouldRetry(2000000));
    CHECK(recovery.shouldRetry(3000000));
    CHECK_EQ(recovery.attempts(), 1);
    CHECK(!recovery.pending());

    // Second failure uses 4s backoff, bypassed immediately on network recovery.
    CHECK(recovery.onStreamFailure(123, 600, 0, false, 4000000));
    CHECK(!recovery.shouldRetry(5000000));
    recovery.onNetworkRecovered(5000000);
    CHECK(recovery.shouldRetry(5000000));
    CHECK_EQ(recovery.attempts(), 2);

    // Third attempt succeeds and then fourth failure exhausts budget.
    CHECK(recovery.onStreamFailure(123, 600, 0, false, 6000000));
    CHECK(recovery.shouldRetry(6000000 + (2000000LL << 2)));
    CHECK_EQ(recovery.attempts(), 3);
    CHECK(!recovery.onStreamFailure(123, 600, 0, false, 20000000));
    CHECK(recovery.exhausted());

    // Loading a stream resets the retry budget.
    recovery.onStreamLoaded();
    CHECK(!recovery.exhausted());
    CHECK_EQ(recovery.attempts(), 0);
    CHECK(recovery.onStreamFailure(42, 600, 0, false, 21000000));
}

void check_perf_diagnostics() {
    auto& perf = wili::PerfDiagnostics::instance();
    perf.setEnabled(false);
    perf.resetSamples();

    // Disabled fast-path records nothing.
    perf.recordUiFrameTimestamp(1000);
    perf.recordUiFrameTimestamp(17667);
    CHECK_EQ(perf.snapshot().ui.count, size_t(0));

    perf.setEnabled(true);
    perf.setNominalRefreshHz(60, false);
    CHECK_EQ(wili::PerfDiagnostics::budgetForRefreshHz(60), uint64_t(16667));
    CHECK_EQ(wili::PerfDiagnostics::budgetForRefreshHz(120), uint64_t(8333));

    int64_t t = 1000000;
    perf.recordUiFrameTimestamp(t);
    for (int i = 1; i <= 100; ++i) {
        // 90 frames at 16.0 ms, 6 frames at 20.0 ms (> budget), 4 frames at 30.0 ms (> 1.5x budget).
        const int64_t delta = (i <= 90) ? 16000 : ((i <= 96) ? 20000 : 30000);
        t += delta;
        perf.recordUiFrameTimestamp(t);
        perf.recordVideoRenderDuration(static_cast<uint64_t>(delta / 10));
    }

    perf.setSourceQn(80);
    perf.setDisplayMetrics(60, false, 1920, 1080, 1920, 1080, false);
    perf.updatePlaybackMetadata(80, 1920, 1080, 1920, 1080, 1920, 1080, false, "software", "GLES3 FBO", 0.002, 12.5,
                                3.25, 0, 1);

    const wili::PerfSnapshot snap60 = perf.snapshot();
    CHECK_EQ(snap60.ui.count, size_t(100));
    CHECK_EQ(snap60.ui.p50Us, uint64_t(16000));
    CHECK_EQ(snap60.ui.p95Us, uint64_t(20000));
    CHECK_EQ(snap60.ui.p99Us, uint64_t(30000));
    CHECK_EQ(snap60.ui.missedFrames, size_t(10));
    CHECK_EQ(snap60.ui.delayedFrames, size_t(4));

    perf.setDisplayMetrics(120, true, 3840, 2160, 1920, 1080, true);
    const wili::PerfSnapshot snap = perf.snapshot();
    CHECK_EQ(snap.nominalRefreshHz, 120);
    CHECK(snap.vrr);
    CHECK_EQ(snap.budgetUs, uint64_t(8333));
    CHECK_EQ(snap.sourceQn, 80);
    CHECK_EQ(snap.outputWidth, 3840);
    CHECK_EQ(snap.renderWidth, 1920);

    const std::string summary = wili::PerfDiagnostics::formatSanitizedSummary(snap);
    CHECK(summary.find("source_qn=80") != std::string::npos);
    CHECK(summary.find("decoded=1920x1080") != std::string::npos);
    CHECK(summary.find("render=1920x1080") != std::string::npos);
    CHECK(summary.find("output=3840x2160_docked") != std::string::npos);
    CHECK(summary.find("refresh=120Hz_vrr") != std::string::npos);
    CHECK(summary.find("SESSDATA") == std::string::npos);
    CHECK(summary.find("bili_jct") == std::string::npos);

    perf.setEnabled(false);
}

void check_dash_selector() {
    bilibili::VideoUrlResult url;
    url.quality = 116;  // 1080P60 requested

    bilibili::DashMedia v116_avc{116, "https://cdn.example/116_avc.m4s", {"https://bak.example/116_avc.m4s"}, 6000000, 1920, 1080, 7};
    bilibili::DashMedia v116_hevc{116, "https://cdn.example/116_hevc.m4s", {}, 4500000, 1920, 1080, 12};
    bilibili::DashMedia v80_hevc{80, "https://cdn.example/80_hevc.m4s", {}, 2500000, 1920, 1080, 12};
    bilibili::DashMedia v80_avc{80, "https://cdn.example/80_avc.m4s", {"https://bak.example/80_avc.m4s"}, 3000000, 1920, 1080, 7};
    url.dash.video = {v116_avc, v116_hevc, v80_hevc, v80_avc};

    bilibili::DashMedia a_high{30280, "https://cdn.example/a30280.m4s", {"https://bak.example/a30280.m4s"}, 192000, 0, 0, 0};
    bilibili::DashMedia a_med{30232, "https://cdn.example/a30232.m4s", {}, 132000, 0, 0, 0};
    bilibili::DashMedia a_dolby{30250, "https://cdn.example/dolby.m4s", {}, 320000, 0, 0, 0};
    url.dash.audio       = {a_med, a_high};
    url.dash.dolby_audio = {a_dolby};
    url.dash.has_flac    = true;
    url.dash.flac_audio  = {30251, "https://cdn.example/flac.m4s", {}, 900000, 0, 0, 0};

    // Landscape capped at 80, preferring HEVC (12), audio preferring Dolby (30250).
    const auto s1 = wili::selectDashTracks(url, 80, 128, 12, 30250);
    CHECK_EQ(s1.selectedQuality, 80);
    CHECK(s1.hasVideo);
    CHECK_EQ(s1.video.codecid, 12);
    CHECK_EQ(s1.video.base_url, std::string("https://cdn.example/80_hevc.m4s"));
    CHECK(s1.hasAudio);
    CHECK_EQ(s1.audio.id, 30250);

    // Unlimited landscape, preferring AV1 (13) -> falls back to first codec at 116 (AVC), standard audio fallback.
    const auto s2 = wili::selectDashTracks(url, 128, 128, 13, 30216);
    CHECK_EQ(s2.selectedQuality, 116);
    CHECK_EQ(s2.video.codecid, 7);
    CHECK_EQ(s2.audio.id, 30280);
    CHECK_EQ(s2.audioUrls.size(), size_t(2));
}

void check_mpv_renderer_invariants() {
    const std::string mpv_hpp = test_check::readSource("wiliwili/include/view/mpv_core.hpp");
    const std::string mpv_cpp = test_check::readSource("wiliwili/source/view/mpv_core.cpp");

    // Uniform/attribute locations belong on GLShader so MPVCore::restart() never
    // reads stale cached locations from a destroyed GL program.
    CHECK(mpv_hpp.find("GLint alphaUniform = -1;") != std::string::npos);
    CHECK(mpv_cpp.find("static GLint alphaID") == std::string::npos);
    CHECK(mpv_cpp.find("shader.alphaUniform") != std::string::npos);

    // setFrameSize guards glTexImage2D against identical pixel dimensions.
    CHECK(mpv_cpp.find("if (drawWidth != this->mpv_fbo.w || drawHeight != this->mpv_fbo.h)") != std::string::npos);
}

}  // namespace

int main() {
    check_playback_recovery();
    check_perf_diagnostics();
    check_dash_selector();
    check_mpv_renderer_invariants();

    return test_check::report("all mpv_core and performance checks passed");
}
