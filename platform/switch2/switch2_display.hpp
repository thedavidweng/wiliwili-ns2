//
// Switch 2 display facts and presentation pacing.
//
// The parts in this header are pure: no <switch.h>, no SDL, no GL. They hold the
// decisions that the console cannot test offline (how a reported output mode
// becomes a framebuffer, what a refresh word means, when to stop presenting at
// the panel rate) and are checked by tests/switch2_lifecycle_test.cpp.
//
// switch2_display.cpp is the libnx/SDL side: it asks applet and vi2 what the
// console is actually driving and applies the result to the window.
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace wili {

// The output the console says it is driving: appletGetDefaultDisplayResolution
// plus appletGetOperationMode. Switch 2 reports 1920x1080 in handheld; a docked
// television can report something else entirely, and nothing here assumes it.
struct OutputMode {
    int width    = 0;
    int height   = 0;
    bool docked  = false;

    bool valid() const { return width > 0 && height > 0; }
};

// The framebuffer the UI renders into. Never larger than the requested budget:
// on this console the UI is a 1280x720 layout scaled up, so a larger buffer
// costs battery and bandwidth without adding legibility.
struct Framebuffer {
    int width  = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
    bool operator==(const Framebuffer& other) const { return width == other.width && height == other.height; }
    bool operator!=(const Framebuffer& other) const { return !(*this == other); }
};

// Fits the reported output into the budget, keeping the aspect ratio: the
// smaller of the two scale factors, rounded down to even pixel counts (odd
// framebuffer dimensions break the blit path), and never below 640x360.
// An invalid report returns an invalid framebuffer: the caller falls back to
// the SDL drawable size instead of inventing a resolution.
Framebuffer fitFramebuffer(const OutputMode& output, int maxWidth, int maxHeight);

// The smallest framebuffer the UI is drawn into; a smaller report is scaled up
// to it, because the 1280x720 layout needs the pixels.
inline constexpr int FRAMEBUFFER_MIN_WIDTH  = 640;
inline constexpr int FRAMEBUFFER_MIN_HEIGHT = 360;

inline Framebuffer fitFramebuffer(const OutputMode& output, int maxWidth, int maxHeight) {
    if (!output.valid() || maxWidth <= 0 || maxHeight <= 0) return {};

    // Shrink only: a report that already fits is passed through, so a 720p
    // docked mode is not silently upscaled to the panel size.
    double scale = 1.0;
    if (output.width > maxWidth) scale = double(maxWidth) / output.width;
    if (output.height * scale > maxHeight) scale = double(maxHeight) / output.height;

    int width  = int(output.width * scale);
    int height = int(output.height * scale);

    if (width < FRAMEBUFFER_MIN_WIDTH || height < FRAMEBUFFER_MIN_HEIGHT) {
        const double up = width > 0 && height > 0
                ? std::max(double(FRAMEBUFFER_MIN_WIDTH) / width, double(FRAMEBUFFER_MIN_HEIGHT) / height)
                : 1.0;
        width  = int(width * up);
        height = int(height * up);
    }

    return {width - width % 2, height - height % 2};
}

// The nominal refresh rate the display is running now, from
// vi2QueryDisplayRefreshRate. Nominal, not a measured scanout rate; VRR means
// bit 63 of the same word is set.
struct Refresh {
    int hz   = 0;
    bool vrr = false;
};

// The modes the display offers, from vi2QueryDisplayModeCapability: bit 0 is
// 60 Hz, bit 1 is 120 Hz, bit 63 is VRR. The HDR and unknown bits say nothing
// about refresh.
struct RefreshCapability {
    bool supports60  = false;
    bool supports120 = false;
    bool vrr         = false;
};

inline RefreshCapability decodeRefreshCapability(uint64_t refreshRates) {
    RefreshCapability capability;
    capability.supports60  = (refreshRates & (1ull << 0)) != 0;
    capability.supports120 = (refreshRates & (1ull << 1)) != 0;
    capability.vrr         = (refreshRates & (1ull << 63)) != 0;
    return capability;
}

inline std::string describeRefresh(const Refresh& refresh) {
    if (refresh.hz <= 0) return "unknown";
    return std::to_string(refresh.hz) + " Hz" + (refresh.vrr ? " (VRR)" : "");
}

// One boot.log line for the display state. Render size, refresh rate and the
// physical output are three separate facts: a 1080p framebuffer on a 4K docked
// output is a decision, not a claim about what the television is doing.
inline std::string describeDisplay(const OutputMode& output, const Framebuffer& framebuffer,
                                   const Refresh& refresh) {
    const std::string render = framebuffer.valid()
            ? std::to_string(framebuffer.width) + "x" + std::to_string(framebuffer.height)
            : "unknown";
    std::string physical = "unknown";
    if (output.valid()) {
        physical = std::to_string(output.width) + "x" + std::to_string(output.height) +
                (output.docked ? " docked" : " handheld");
    }
    return "render " + render + ", refresh " + describeRefresh(refresh) + ", output " + physical;
}

// Presentation-timed frame scheduling.
//
// The main loop presents once per iteration, so with a swap interval of 1 it
// already runs at the panel rate: 60 Hz handheld, 120 Hz when the console grants
// the request. That is what "adapts to the active refresh rate" means here:
// nothing forces a rate, the panel decides, and 120 Hz UI support costs no code.
// What is left to decide is a screen nobody is touching: presenting 120 frames
// of an unchanged picture a second is battery for nothing, so after `quietUs`
// without input, animation, a display change or playback the platform caps the
// loop at `idleFps` (Application::setLimitedFPS, which sleeps after the present)
// and any activity restores every refresh on the same frame.
//
// The cap is never applied while the user has pinned a frame rate themselves
// (LIMITED_FPS, which sets a swap interval of 0): that is someone else's
// decision, so update() then answers LEAVE_ALONE.
class FramePacer {
  public:
    static inline constexpr int LEAVE_ALONE = -1;

    struct Config {
        int idleFps      = 30;
        int64_t quietUs  = 1000000;
    };

    explicit FramePacer(const Config& config) : config(config) {}
    FramePacer() = default;

    void setAdaptive(bool value) { this->adaptivePacing = value; }

    bool idle() const { return this->idling; }

    // `busy` is the caller's activity verdict for this frame; returns the frame
    // cap to apply, 0 for every refresh.
    int update(bool busy, int64_t nowUs) {
        if (!this->adaptivePacing) {
            this->started = false;
            this->idling  = false;
            return LEAVE_ALONE;
        }
        if (!this->started) {
            // The first frame counts as activity: the app just started.
            this->started  = true;
            this->lastBusy = nowUs;
        }
        if (busy) this->lastBusy = nowUs;
        this->idling = !busy && nowUs - this->lastBusy >= this->config.quietUs;
        return this->idling ? this->config.idleFps : 0;
    }

  private:
    Config config;
    bool adaptivePacing = true;
    bool started        = false;
    bool idling         = false;
    int64_t lastBusy    = 0;
};

// --- The Switch 2 side, implemented in switch2_display.cpp ---------------
//
// Every function here talks to applet, vi2, SDL or GL, so none of them is
// tested on the host; the decisions they feed are the pure functions above.

// Size to open the window with, before SDL exists. Falls back to the requested
// size when the console will not answer.
Framebuffer preferredWindowSize(int fallbackWidth, int fallbackHeight);

// Called once the GL context exists: opens the display change event, asks applet
// and vi2 what the console drives, and records the result in boot.log.
void displayInitialize(void* sdlWindow);

// Once per frame. Returns true when the output mode or the refresh rate changed
// and was applied to the window, the GL viewport and Borealis' logical size.
// `force` re-reads the display even when the console announced nothing (the
// applet hooks do this after a dock or a resume).
bool displayPoll(bool force = false);

double displayScaleFactor();
Framebuffer displayFramebuffer();

// Closes the display change event and vi2 handles. Called last during teardown.
void displayShutdown();

}  // namespace wili
