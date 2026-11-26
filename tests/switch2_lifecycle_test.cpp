// Host checks for the Switch 2 console lifecycle: display and framebuffer
// policy, refresh decoding, presentation pacing, the pre-playback probe state
// machine and the teardown order. Run by ctest as the switch2_lifecycle case.
//
// Everything asserted here is pure logic from platform/switch2/switch2_display.hpp
// and platform/switch2/lifecycle_probe.hpp: the parts of issue #5 that a
// physical console is not needed to get right, and that a regression would
// otherwise only show as a stretched docked image or a battery drain on device.

#include <iostream>
#include <string>

#include "lifecycle_probe.hpp"
#include "switch2_display.hpp"
#include "test_check.hpp"
#include "utils/boot_diagnostics.hpp"

namespace {

using test_check::check;
using test_check::failures;

void check_framebuffer_fit() {
    // Handheld is a native 1080p panel: the UI renders at the panel size, not at
    // the Switch 1 720p assumption and not at a fixed constant.
    const wili::OutputMode handheld{1920, 1080, false};
    const wili::Framebuffer native = wili::fitFramebuffer(handheld, 1920, 1080);
    CHECK(native.width == 1920);
    CHECK(native.height == 1080);

    // A docked television that reports 4K is fitted into the budget with the
    // 16:9 ratio kept, and the render size stays separate from the output size.
    const wili::OutputMode uhd{3840, 2160, true};
    const wili::Framebuffer fitted = wili::fitFramebuffer(uhd, 1920, 1080);
    CHECK(fitted.width == 1920);
    CHECK(fitted.height == 1080);

    // 1280x720 output passes through untouched rather than being upscaled to a
    // hardcoded panel size.
    const wili::Framebuffer hd = wili::fitFramebuffer({1280, 720, true}, 1920, 1080);
    CHECK(hd.width == 1280);
    CHECK(hd.height == 720);

    // An ultrawide docked mode keeps its aspect ratio instead of being
    // stretched to the budget: 2560x800 -> 1920x600.
    const wili::Framebuffer ultrawide = wili::fitFramebuffer({2560, 800, true}, 1920, 1080);
    CHECK(ultrawide.width == 1920);
    CHECK(ultrawide.height == 600);

    // Odd dimensions become even ones: a framebuffer blit needs even sizes.
    const wili::Framebuffer odd = wili::fitFramebuffer({1367, 769, false}, 1920, 1080);
    CHECK(odd.width % 2 == 0);
    CHECK(odd.height % 2 == 0);
    CHECK(odd.width <= 1366);
    CHECK(odd.height <= 768);

    // A report too small to be a display is clamped up to the floor rather than
    // producing a zero sized framebuffer.
    const wili::Framebuffer tiny = wili::fitFramebuffer({320, 180, false}, 1920, 1080);
    CHECK(tiny.width == 640);
    CHECK(tiny.height == 360);

    // No report at all is not a resolution: the caller uses the SDL drawable
    // size instead of guessing one.
    CHECK(!wili::fitFramebuffer({0, 0, false}, 1920, 1080).valid());
    CHECK(!wili::fitFramebuffer({-1, 1080, true}, 1920, 1080).valid());
}

void check_refresh_decoding() {
    // vi2 uses bit 0 for 60 Hz, bit 1 for 120 Hz and bit 63 for VRR in the
    // refresh word. Nothing else in the word is a refresh rate.
    const wili::RefreshCapability only60 = wili::decodeRefreshCapability(1ull);
    CHECK(only60.supports60);
    CHECK(!only60.supports120);
    CHECK(!only60.vrr);

    const wili::RefreshCapability only120 = wili::decodeRefreshCapability(2ull);
    CHECK(!only120.supports60);
    CHECK(only120.supports120);

    const wili::RefreshCapability both = wili::decodeRefreshCapability(3ull);
    CHECK(both.supports60);
    CHECK(both.supports120);

    const wili::RefreshCapability vrr = wili::decodeRefreshCapability((1ull << 63) | 3ull);
    CHECK(vrr.vrr);
    CHECK(vrr.supports120);

    // HDR bits and unknown wire bits are not refresh rates.
    CHECK(!wili::decodeRefreshCapability(0x4ull | 0x8ull).supports120);
    CHECK(!wili::decodeRefreshCapability(0).supports60);
    CHECK(!wili::decodeRefreshCapability(0).supports120);
}

void check_lifecycle_probe() {
    // The pre-playback probe opens aud:u and asks nifm about the connection. It
    // runs at boot and after sleep/wake, not every frame.
    wili::ProbeSchedule schedule(30000000);
    CHECK(schedule.due(5000000));
    schedule.markRun(5000000);
    CHECK(!schedule.due(5000001));
    CHECK(!schedule.due(34999999));
    CHECK(schedule.due(35000000));

    // A healthy console: audio outputs are open, the socket is connected with an
    // address, and nifm reports a DNS server.
    wili::ProbeResult healthy;
    healthy.ran          = true;
    healthy.audioInitOk  = true;
    healthy.audioOk      = true;
    healthy.audioOutputs = 2;
    healthy.connected    = true;
    healthy.addressOk    = true;
    healthy.dnsOk        = true;
    CHECK(healthy.healthy());
    CHECK_EQ(healthy.summary(), "audio 2 outputs, socket connected, dns configured");

    // Each failing half is reported on its own: audio can come back without a
    // network and vice versa, and the log has to say which one is broken.
    wili::ProbeResult noAudio = healthy;
    noAudio.audioInitOk       = false;
    noAudio.audioOk           = false;
    CHECK(!noAudio.healthy());
    CHECK_EQ(noAudio.summary(), "audio init failed, socket connected, dns configured");

    wili::ProbeResult muted = healthy;
    muted.audioOutputs      = 0;
    muted.audioOk           = false;
    CHECK(!muted.healthy());
    CHECK_EQ(muted.summary(), "audio no outputs, socket connected, dns configured");

    wili::ProbeResult offline = healthy;
    offline.connected         = false;
    offline.addressOk         = false;
    offline.dnsOk             = false;
    CHECK(!offline.healthy());
    CHECK_EQ(offline.summary(), "audio 2 outputs, socket not connected, dns unresolved");

    wili::ProbeResult noAddress = healthy;
    noAddress.addressOk         = false;
    CHECK_EQ(noAddress.summary(), "audio 2 outputs, socket connected without address, dns configured");

    // Nothing was probed at all (desktop build, or the probe was skipped).
    CHECK_EQ(wili::ProbeResult{}.summary(), "not run");
    CHECK(!wili::ProbeResult{}.healthy());
}

// A pre-playback probe asks about the audio route only while playback does not
// own it: opening aud:u under a playing mpv closes the handle it is using.
void check_pre_playback_probe() {
    CHECK(wili::probeAudioRoute(false));  // nothing plays yet: probe the route
    CHECK(!wili::probeAudioRoute(true));  // mpv holds it: skip

    // The skipped half says so instead of claiming outputs it never looked at.
    wili::ProbeResult skipped;
    skipped.ran          = true;
    skipped.audioSkipped = true;
    skipped.connected    = true;
    skipped.addressOk    = true;
    skipped.dnsOk        = true;
    CHECK_EQ(skipped.summary(), "audio owned by playback, socket connected, dns configured");
    CHECK(skipped.healthy());  // the player's own route is not a fault

    // The socket half is still the reason a stream can fail.
    wili::ProbeResult offline = skipped;
    offline.connected         = false;
    offline.addressOk         = false;
    offline.dnsOk             = false;
    CHECK(!offline.healthy());
    CHECK_EQ(offline.summary(), "audio owned by playback, socket not connected, dns unresolved");
}

// A Wi-Fi drop and reconnect never suspends the console, so the platform polls
// nifm instead of waiting for an applet hook. A reconnect or a new address is
// what makes the cached DNS answers and sockets stale.
void check_network_watch() {
    wili::NetworkWatch watch;
    const int64_t start = 10 * 1000 * 1000;

    // The first ask is always due, then one query per interval.
    CHECK(watch.due(start));
    watch.markPolled(start);
    CHECK(!watch.due(start + 1));
    CHECK(!watch.due(start + wili::NetworkWatch::POLL_INTERVAL_US - 1));
    CHECK(watch.due(start + wili::NetworkWatch::POLL_INTERVAL_US));
    CHECK_EQ((int)wili::NetworkWatch::POLL_INTERVAL_US, 2000000);

    // The first sample is the baseline: nothing is stale yet.
    CHECK(!watch.observe({true, 0xA000001}));
    CHECK(!watch.observe({true, 0xA000001}));  // unchanged

    // A transient drop is not a reset: the next request simply fails, and the
    // reconnect is what invalidates the old answers.
    CHECK(!watch.observe({false, 0}));
    CHECK(watch.observe({true, 0xA000001}));

    // A new address on the same Wi-Fi also invalidates them.
    CHECK(watch.observe({true, 0xA000002}));

    // Staying disconnected does not schedule anything.
    CHECK(!watch.observe({false, 0}));
    CHECK(!watch.observe({false, 0}));
}

void check_teardown_order() {
    // The platform destructor runs the steps in this order. The input manager
    // has to go before the video context (its destructor deletes a cursor image
    // through the NVG context), the applet hook has to be unhooked before
    // anything it can reach is destroyed, and the display change event is only
    // closed after the window is gone.
    int unhook = -1, input = -1, video = -1, display = -1;
    int index = 0;
    for (wili::TeardownStep step : wili::TEARDOWN_ORDER) {
        switch (step) {
            case wili::TeardownStep::Unhook:
                unhook = index;
                break;
            case wili::TeardownStep::Input:
                input = index;
                break;
            case wili::TeardownStep::Video:
                video = index;
                break;
            case wili::TeardownStep::Display:
                display = index;
                break;
            default:
                break;
        }
        ++index;
    }
    CHECK(unhook == 0);
    CHECK(input >= 0);
    CHECK(video > input);
    CHECK(display == index - 1);

    // In-app exit must return to kaiHEN (0) rather than killing the host process (1).
    CHECK_EQ(wili::switch2AppletExitMode(true), 0u);
    CHECK_EQ(wili::switch2AppletExitMode(false), 0u);
}

void check_system_tick_conversion() {
    // Switch 2's hardware counter runs at 31.25 MHz (32 ns/tick), whereas
    // Switch 1 ran at 19.2 MHz. Feeding 1 second of Switch 2 ticks into
    // Borealis' Switch 1 formula ((ticks * 10) / 192) inflated time to
    // 1,627,604 us (+62.8%), making danmaku scroll 1.63x too fast and jump on pause.
    constexpr uint64_t kSwitch2FreqHz = 31250000ULL;
    constexpr uint64_t kSwitch1FreqHz = 19200000ULL;

    const uint64_t legacySwitch1FormulaOnSwitch2 = (kSwitch2FreqHz * 10ULL) / 192ULL;
    CHECK_EQ(legacySwitch1FormulaOnSwitch2, 1627604ULL);
    CHECK_EQ(wili::ticksToUsec(kSwitch2FreqHz, kSwitch2FreqHz), 1000000);
    CHECK_EQ(wili::ticksToUsec(kSwitch1FreqHz, kSwitch1FreqHz), 1000000);

    // Sub-frame and frame-interval precision at 60 Hz (520,833 ticks = 16,666 us)
    // and 120 Hz (260,417 ticks = 8,333 us).
    CHECK_EQ(wili::ticksToUsec(520833ULL, kSwitch2FreqHz), 16666);
    CHECK_EQ(wili::ticksToUsec(260417ULL, kSwitch2FreqHz), 8333);
    CHECK_EQ(wili::ticksToUsec(125ULL, kSwitch2FreqHz), 4);
    CHECK_EQ(wili::ticksToUsec(0ULL, kSwitch2FreqHz), 0);
    CHECK_EQ(wili::ticksToUsec(kSwitch2FreqHz, 0ULL), 0);

    // Must not overflow uint64_t even after 365 days of continuous console uptime
    // (where ticks * 1,000,000 would overflow 2^64 by >50x).
    constexpr uint64_t kOneYearSeconds = 365ULL * 24ULL * 3600ULL;
    constexpr uint64_t kOneYearTicks   = kOneYearSeconds * kSwitch2FreqHz;
    CHECK_EQ(wili::ticksToUsec(kOneYearTicks, kSwitch2FreqHz),
             static_cast<int64_t>(kOneYearSeconds * 1000000ULL));
}

void check_tls_default() {
    // Switch 2 verifies certificates by default: the platform's libcurl is
    // built against libnx's ssl service, which does have peer and hostname
    // verification. Switch 1, PSV and PS4 keep the historical default.
    CHECK(wili::tlsVerifyDefaultForPlatform("switch2"));
    CHECK(!wili::tlsVerifyDefaultForPlatform("switch"));
    CHECK(!wili::tlsVerifyDefaultForPlatform("psv"));
    CHECK(!wili::tlsVerifyDefaultForPlatform("ps4"));
    CHECK(wili::tlsVerifyDefaultForPlatform("desktop"));
}

void check_frame_pacing() {
    // The panel rate needs no code: a swap interval of 1 presents once per
    // iteration, so the loop already runs at 60 or at the 120 Hz the console
    // granted. A quiet screen must stop paying for presents, and any activity
    // must bring the full rate back on the same frame, not one idle period
    // later.
    const int64_t start = 1000000;
    wili::FramePacer pacer{{30, 1000000}};

    // Nothing has been quiet long enough yet.
    CHECK(pacer.update(false, start) == 0);
    CHECK(!pacer.idle());
    CHECK(pacer.update(false, start + 500000) == 0);
    CHECK(!pacer.idle());

    // A second without input, animation, display change or playback: the loop
    // drops to the idle cap.
    CHECK(pacer.update(false, start + 1000000) == 30);
    CHECK(pacer.idle());
    CHECK(pacer.update(false, start + 2000000) == 30);

    // One busy frame restores every refresh immediately and restarts the quiet
    // period, so a drag or a button press is never throttled.
    CHECK(pacer.update(true, start + 2100000) == 0);
    CHECK(!pacer.idle());
    CHECK(pacer.update(false, start + 2500000) == 0);
    CHECK(pacer.update(false, start + 3100000) == 30);

    // A 60 Hz panel gets the same idle cap.
    wili::FramePacer hd{{30, 1000000}};
    CHECK(hd.update(false, start) == 0);
    CHECK(hd.update(false, start + 1000000) == 30);

    // A user pinned frame cap (LIMITED_FPS) is not the platform's to change.
    wili::FramePacer pinned{{30, 1000000}};
    pinned.setAdaptive(false);
    CHECK(pinned.update(false, start + 10000000) == wili::FramePacer::LEAVE_ALONE);
    CHECK(pinned.update(true, start + 10000000) == wili::FramePacer::LEAVE_ALONE);
    CHECK(!pinned.idle());

    // Going back to vsync hands pacing to the platform again.
    pinned.setAdaptive(true);
    CHECK(pinned.update(false, start + 10000000) == 0);
    CHECK(pinned.update(false, start + 11000000) == 30);

    // Active pacing at display refresh prevents 4-buffer Vulkan FIFO burst stalls.
    CHECK_EQ(wili::activeFrameCapForRefresh(0), 60);
    CHECK_EQ(wili::activeFrameCapForRefresh(60), 60);
    CHECK_EQ(wili::activeFrameCapForRefresh(120), 120);

    wili::FramePacer active60{{30, 1000000, wili::activeFrameCapForRefresh(60)}};
    CHECK_EQ(active60.update(true, start), 60);
    CHECK(!active60.idle());
    CHECK_EQ(active60.update(false, start + 1000000), 30);
    CHECK(active60.idle());
    active60.setActiveFps(wili::activeFrameCapForRefresh(120));
    CHECK_EQ(active60.update(true, start + 1100000), 120);
}

void check_display_diagnostics() {
    // boot.log has to keep render resolution, nominal refresh rate and the
    // physical output mode apart: they are three different numbers on this
    // console and only the output mode is what the television reports.
    CHECK_EQ(wili::describeRefresh({60, false}), "60 Hz");
    CHECK_EQ(wili::describeRefresh({120, false}), "120 Hz");
    CHECK_EQ(wili::describeRefresh({120, true}), "120 Hz (VRR)");
    CHECK_EQ(wili::describeRefresh({0, false}), "unknown");

    const wili::OutputMode uhd{3840, 2160, true};
    const wili::Framebuffer fitted{1920, 1080};
    CHECK_EQ(wili::describeDisplay(uhd, fitted, {120, false}),
             "render 1920x1080, refresh 120 Hz, output 3840x2160 docked");

    const wili::OutputMode handheld{1920, 1080, false};
    CHECK_EQ(wili::describeDisplay(handheld, {1920, 1080}, {60, false}),
             "render 1920x1080, refresh 60 Hz, output 1920x1080 handheld");

    // A failed query says so instead of claiming a resolution.
    CHECK_EQ(wili::describeDisplay({0, 0, false}, {}, {0, false}), "render unknown, refresh unknown, output unknown");
}

}  // namespace

int main() {
    check_framebuffer_fit();
    check_refresh_decoding();
    check_frame_pacing();
    check_display_diagnostics();
    check_lifecycle_probe();
    check_pre_playback_probe();
    check_network_watch();
    check_teardown_order();
    check_system_tick_conversion();
    check_tls_default();

    return test_check::report("switch2 lifecycle checks passed");
}
