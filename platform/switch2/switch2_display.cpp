/*
 * Switch 2 display facts: what the console is driving, and how the UI follows it.
 *
 * Issue #5 treats Switch 2 display and output modes as runtime capabilities, not
 * as Switch 1 constants. Everything here is best effort and non-fatal: the
 * sources are appletGetDefaultDisplayResolution (the resolution the console
 * outputs, 1080p native in handheld), appletGetOperationMode (handheld or
 * docked), and the vi2 refresh queries. vi2 in particular may answer with
 * "unsupported" while OunceVK owns the window, and that only costs the refresh
 * rate in the diagnostics, never the picture.
 *
 * The SDL window size, the GL viewport and Borealis' logical size all follow the
 * same value (the framebuffer from switch2_display.hpp), because those three
 * disagreeing is what produces cropped or stretched output.
 */

#include "switch2_display.hpp"

#include <switch.h>

#include <SDL2/SDL.h>
#include <glad/glad.h>

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>

#include <fmt/format.h>

#include "utils/boot_api.h"

namespace {

// What the software baseline can afford to render. The 1280x720 logical layout
// gains no legibility above 1080p, and a bigger buffer costs memory bandwidth
// on a console running Zink on NVK. This is a budget, not a claim about the
// Switch 1 panel (720p) or about unverified 4K output: whatever the console
// reports is logged on its own line.
constexpr int MAX_FRAMEBUFFER_WIDTH  = 1920;
constexpr int MAX_FRAMEBUFFER_HEIGHT = 1080;

// The display change event is the signal; this poll covers a mode change the
// system did not announce. Once a second is cheap (one IPC) and bounded.
constexpr int POLL_INTERVAL_FRAMES = 60;

struct DisplayState {
    SDL_Window* window   = nullptr;
    bool initialized     = false;

    // appletGetDefaultDisplayResolutionChangeEvent, closed in displayShutdown().
    Event changeEvent{};
    bool changeEventOpen = false;

    // vi2 is ref counted: initialize once, close the display, then exit.
    Vi2Display vi2Display{};
    bool vi2Open        = false;
    bool vi2DisplayOpen = false;
    bool requested120   = false;
    wili::RefreshCapability capability{};

    wili::OutputMode output{};
    wili::Framebuffer framebuffer{};
    wili::Refresh refresh{};
    int appliedWidth     = 0;
    int appliedHeight    = 0;
    int framesSincePoll  = 0;
    double scaleFactor   = 1.0;
};

DisplayState state;

void note(const std::string& detail) {
    brls::Logger::info("switch2 display: {}", detail);
    wiliwili_boot_note("display", detail.c_str());
}

bool queryOutput(wili::OutputMode& output) {
    s32 width = 0, height = 0;
    const Result rc = appletGetDefaultDisplayResolution(&width, &height);
    if (R_FAILED(rc) || width <= 0 || height <= 0) return false;

    output.width  = width;
    output.height = height;
    output.docked = appletGetOperationMode() == AppletOperationMode_Console;
    return true;
}

// Best effort, once: a failing vi2 call is recorded and then ignored. The
// renderer does not present through VI2, OunceVK owns the window, so the picture
// does not depend on any of this.
void openVi2() {
    if (!state.vi2Open) {
        const Result rc = vi2Initialize();
        if (R_FAILED(rc)) {
            note(fmt::format("vi2 unavailable (0x{:08x}), refresh rate unknown", rc));
            return;
        }
        state.vi2Open = true;
    }

    if (!state.vi2DisplayOpen) {
        const Result rc = vi2OpenDefaultDisplay(&state.vi2Display);
        if (R_FAILED(rc)) {
            note(fmt::format("vi2 open display failed (0x{:08x})", rc));
            return;
        }
        state.vi2DisplayOpen = true;
    }

    Vi2DisplayModeCapability caps = {};
    if (R_FAILED(vi2QueryDisplayModeCapability(&state.vi2Display, &caps))) return;

    state.capability = wili::decodeRefreshCapability(caps.refresh_rates);
    note(fmt::format("vi2 modes: {}{}{}", state.capability.supports60 ? "60 Hz " : "",
                     state.capability.supports120 ? "120 Hz " : "",
                     state.capability.vrr ? "VRR" : ""));

    // Ask once: a request that VI2's display manager weighs at its next mode
    // selection, not a mode change, and the refresh query reports what it
    // actually chose. Asking does not mean presenting 120 frames a second of an
    // unchanged screen: the platform paces an idle, non-playing UI down.
    if (state.capability.supports120) {
        const Result rc   = vi2SetDisplay120HzCapability(&state.vi2Display, true);
        state.requested120 = R_SUCCEEDED(rc);
        note(fmt::format("vi2 120 Hz request {}", state.requested120 ? "accepted" : "failed"));
    }
}

// The nominal rate can change after the 120 Hz request was weighed, so it is
// re-read on every display poll and recorded when it moves.
void queryRefresh() {
    if (!state.vi2DisplayOpen) return;

    u32 hz         = 0;
    bool vrr       = false;
    const wili::Refresh previous = state.refresh;
    if (R_SUCCEEDED(vi2QueryDisplayRefreshRate(&state.vi2Display, &hz, &vrr)) && (hz == 60 || hz == 120)) {
        state.refresh = {int(hz), vrr};
    } else {
        // 0x1472 means "not a 60 or 120 Hz word"; the nominal rate is unknown.
        state.refresh = {0, false};
    }

    if (state.refresh.hz != previous.hz || state.refresh.vrr != previous.vrr)
        note("refresh " + wili::describeRefresh(state.refresh));
}

// Brings the window, the GL viewport and Borealis' logical size to the current
// framebuffer. Only called when the value changed: Application::onWindowResized
// debounces for 100 ms, so calling it every frame would postpone it forever.
void applyFramebuffer() {
    if (!state.window || !state.framebuffer.valid()) return;

    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSize(state.window, &windowWidth, &windowHeight);
    if (windowWidth != state.framebuffer.width || windowHeight != state.framebuffer.height) {
        // Best effort: SDL's Switch driver reports the size it was given, and the
        // drawable size below is what the GL viewport actually uses.
        SDL_SetWindowSize(state.window, state.framebuffer.width, state.framebuffer.height);
        SDL_GetWindowSize(state.window, &windowWidth, &windowHeight);
    }

    // SDL's drawable size is the authoritative physical framebuffer when it
    // answers; the fitted size is what was asked for.
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GL_GetDrawableSize(state.window, &drawableWidth, &drawableHeight);
    const int framebufferWidth  = drawableWidth > 0 ? drawableWidth : state.framebuffer.width;
    const int framebufferHeight = drawableHeight > 0 ? drawableHeight : state.framebuffer.height;

    state.scaleFactor = windowWidth > 0 ? double(framebufferWidth) / windowWidth : 1.0;
    glViewport(0, 0, framebufferWidth, framebufferHeight);

    if (framebufferWidth != state.appliedWidth || framebufferHeight != state.appliedHeight) {
        state.appliedWidth  = framebufferWidth;
        state.appliedHeight = framebufferHeight;
        brls::Application::onWindowResized(framebufferWidth, framebufferHeight);
    }

    note(wili::describeDisplay(state.output, state.framebuffer, state.refresh));
}

// Re-reads the output mode and the refresh rate, and applies a change.
bool refreshDisplay(bool force) {
    bool changed = false;

    wili::OutputMode output;
    if (queryOutput(output) &&
        (output.width != state.output.width || output.height != state.output.height ||
         output.docked != state.output.docked)) {
        state.output      = output;
        state.framebuffer = wili::fitFramebuffer(output, MAX_FRAMEBUFFER_WIDTH, MAX_FRAMEBUFFER_HEIGHT);
        changed           = true;
    }

    const wili::Refresh previousRefresh = state.refresh;
    queryRefresh();
    const bool refreshChanged = state.refresh.hz != previousRefresh.hz || state.refresh.vrr != previousRefresh.vrr;

    if (changed || refreshChanged || force) applyFramebuffer();
    return changed;
}

}  // namespace

namespace wili {

Framebuffer preferredWindowSize(int fallbackWidth, int fallbackHeight) {
    OutputMode output;
    if (queryOutput(output)) {
        const Framebuffer framebuffer = fitFramebuffer(output, MAX_FRAMEBUFFER_WIDTH, MAX_FRAMEBUFFER_HEIGHT);
        if (framebuffer.valid()) return framebuffer;
    }
    return {fallbackWidth, fallbackHeight};
}

void displayInitialize(void* sdlWindow) {
    state.window = static_cast<SDL_Window*>(sdlWindow);

    const Result rc = appletGetDefaultDisplayResolutionChangeEvent(&state.changeEvent);
    if (R_SUCCEEDED(rc)) {
        state.changeEventOpen = true;
    } else {
        note(fmt::format("display resolution event unavailable (0x{:08x}), polling", rc));
    }

    OutputMode output;
    if (queryOutput(output)) state.output = output;
    state.framebuffer = fitFramebuffer(state.output, MAX_FRAMEBUFFER_WIDTH, MAX_FRAMEBUFFER_HEIGHT);
    if (!state.framebuffer.valid()) {
        // The console would not report a resolution; the window it accepted is
        // the only size left to render at.
        int width = 0, height = 0;
        SDL_GetWindowSize(state.window, &width, &height);
        state.framebuffer = {width, height};
    }

    openVi2();
    queryRefresh();
    state.initialized = true;

    applyFramebuffer();
    note(fmt::format("output mode {} ({})", state.output.docked ? "docked" : "handheld",
                     state.output.valid() ? "reported" : "not reported"));
}

bool displayPoll(bool force) {
    if (!state.initialized) return false;

    bool signalled = false;
    if (state.changeEventOpen) signalled = R_SUCCEEDED(eventWait(&state.changeEvent, 0));

    if (!signalled && !force) {
        if (++state.framesSincePoll < POLL_INTERVAL_FRAMES) return false;
    }
    state.framesSincePoll = 0;

    return refreshDisplay(force);
}

double displayScaleFactor() { return state.scaleFactor; }

Framebuffer displayFramebuffer() { return state.framebuffer; }

void displayShutdown() {
    if (state.changeEventOpen) {
        eventClose(&state.changeEvent);
        state.changeEventOpen = false;
    }
    if (state.vi2DisplayOpen) {
        vi2CloseDisplay(&state.vi2Display);
        state.vi2DisplayOpen = false;
    }
    if (state.vi2Open) {
        vi2Exit();
        state.vi2Open = false;
    }
    state.window      = nullptr;
    state.initialized = false;
}

}  // namespace wili
