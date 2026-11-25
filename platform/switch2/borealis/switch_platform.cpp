/*
    Copyright 2021 natinusala

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/*
 * Switch 2 platform: console lifecycle, display changes and presentation pacing.
 *
 * Replaces library/borealis/library/lib/platforms/switch/switch_platform.cpp
 * when PLATFORM_SWITCH2=ON (see library/CMakeLists.txt), so library/borealis
 * stays at its pinned commit. Differences from the upstream file:
 *
 * - Every applet hook is handled, not only focus and exit, and none of them is
 *   behind BOREALIS_USE_DEKO3D: the Switch 2 build is GLES over OunceVK, where
 *   the deko3d-only OnOperationMode handler used to be dead code. Dock/undock
 *   (OnOperationMode) re-reads the display, OnResume re-reads it and re-probes
 *   audio and network, OnPerformanceMode is recorded.
 * - SDL events are pumped every frame. Nothing else drains that queue on this
 *   platform (there is no SDLPlatform), so it used to grow for the whole session.
 * - The applet state that libnx only updates while appletMainLoop() runs
 *   (operation mode, focus) is read after it, never before.
 * - Frame scheduling follows the panel: with a swap interval of 1 the loop
 *   already runs at 60 or 120 Hz, and a quiet, non-playing screen is paced down
 *   to 30 Hz instead of presenting 120 wasted frames a second.
 * - The destructor releases the platform in wili::TEARDOWN_ORDER: the input
 *   manager goes before the video context (its destructor deletes the cursor
 *   image through the NVG context), and the applet hook is unhooked before
 *   anything it can reach is destroyed.
 */

#include <switch.h>

#include <SDL2/SDL.h>

#include <atomic>
#include <cstring>

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/time.hpp>
#include <borealis/platforms/sdl/sdl_video.hpp>
#include <borealis/platforms/switch/switch_platform.hpp>

#include <fmt/format.h>

#include "utils/boot_api.h"
#include "utils/platform_api.h"

#include "lifecycle_probe.hpp"
#include "switch2_display.hpp"
#include "switch2_pointer_device.hpp"

extern "C" u32 __nx_applet_exit_mode;

namespace brls {

namespace {

// The hooks run on libnx's applet thread, so they only record what happened; the
// main loop reads these flags and does the work (firing events, touching GL).
AppletHookCookie applet_hook_cookie;

std::atomic<bool> exitRequested{false};
std::atomic<bool> focusReported{false};
std::atomic<bool> focused{false};
std::atomic<bool> displayRecheck{false};
std::atomic<bool> resumeRequested{false};
std::atomic<bool> performanceModeChanged{false};

// Main thread state.
//
// mediaPlaybackActive is written from the playback core's event thread (it asks
// for the screen to stay on exactly while a video is playing) and read here, so
// it is atomic. Everything else below is only touched by the main loop.
std::atomic<bool> mediaPlaybackActive{false};
std::atomic<bool> screenDimmingDisabled{false};
wili::FramePacer framePacer;
wili::ProbeSchedule probeSchedule(30 * 1000 * 1000);
wili::NetworkWatch networkWatch;
int appliedFrameCap  = 0;
bool frameCapApplied = false;
ControllerState lastControllerState{};

void on_applet_hook(AppletHookType hook, void* arg) {
    switch (hook) {
        case AppletHookType_OnExitRequest:
            exitRequested = true;
            break;
        case AppletHookType_OnFocusState:
            // appletGetFocusState() is the state the hook is reporting; read it
            // here and let the main loop fire the event.
            focused       = appletGetFocusState() == AppletFocusState_InFocus;
            focusReported = true;
            break;
        case AppletHookType_OnOperationMode:
            displayRecheck = true;
            break;
        case AppletHookType_OnResume:
            resumeRequested = true;
            displayRecheck  = true;
            break;
        case AppletHookType_OnPerformanceMode:
            performanceModeChanged = true;
            break;
        default:
            break;
    }
}

// Controller input this frame: a button is held, or the state changed since the
// last frame (a release is input too). hasActiveEvent() cannot be used for this:
// it returns true unconditionally on __SWITCH__.
bool inputActive() {
    const ControllerState& state = Application::getControllerState();

    bool anyPressed = false;
    for (bool pressed : state.buttons) anyPressed = anyPressed || pressed;

    // A release is input too, and it is the last frame's press that shows up in
    // the state at this point of the loop.
    const bool changed = std::memcmp(state.buttons, lastControllerState.buttons, sizeof(state.buttons)) != 0;
    std::memcpy(lastControllerState.buttons, state.buttons, sizeof(state.buttons));
    return anyPressed || changed || wili::switch2::pointerFrameActive();
}

}  // namespace

SwitchPlatform::SwitchPlatform() {
    // Cache theme variant before video context init
    // The background color is created once in the "static" command list
    // executed every frame, so we need to know the background color
    // to add the clear command to that list.
    ColorSetId colorSetId;
    setsysGetColorSetId(&colorSetId);

    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);

    if (colorSetId == ColorSetId_Dark)
        this->themeVariant = ThemeVariant::DARK;
    else
        this->themeVariant = ThemeVariant::LIGHT;

    Logger::info("switch system theme: {}", colorSetId ? "Dark" : "Light");

    // Get locale
    if (Platform::APP_LOCALE_DEFAULT == LOCALE_AUTO) {
        uint64_t languageCode = 0;
        Result rc             = setGetSystemLanguage(&languageCode);
        if (R_SUCCEEDED(rc)) {
            char* languageName = (char*)&languageCode;
            this->locale       = std::string(languageName);
        } else {
            Logger::error("switch: unable to get system language (error {:#x}), using the default one: {}", rc,
                          LOCALE_DEFAULT);
            this->locale = LOCALE_DEFAULT;
        }
    } else {
        this->locale = Platform::APP_LOCALE_DEFAULT;
    }

    // Init platform impls
    this->audioPlayer = new SwitchAudioPlayer();
    this->fontLoader  = new SwitchFontLoader();
    this->imeManager  = new SwitchImeManager();

    appletHook(&applet_hook_cookie, on_applet_hook, this);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);

    // The console is awake and idle: probe audio and network before any stream
    // is opened, and record it next to the boot stages.
    const int64_t now = getCPUTimeUsec();
    probeSchedule.markRun(now);
    wili::lifecycleProbeNote(wili::runLifecycleProbe(true), "boot");
}

void SwitchPlatform::createWindow(std::string windowTitle, uint32_t windowWidth, uint32_t windowHeight,
                                  float windowXPos, float windowYPos) {
#ifdef BOREALIS_USE_DEKO3D
    this->videoContext = new SwitchVideoContext();
#elif defined(__SDL2__)
    this->videoContext = new SDLVideoContext(windowTitle, windowWidth, windowHeight, 0, 0);
#else
    this->videoContext = new GLFWVideoContext(windowTitle, windowWidth, windowHeight);
#endif

    // Reinitialise controllers with settings from borealis, not by GLFW or SDL
    this->inputManager = new SwitchInputManager();
}

bool SwitchPlatform::canShowBatteryLevel() { return true; }

int SwitchPlatform::getBatteryLevel() {
    u32 charge;
    psmGetBatteryChargePercentage(&charge);
    return (int)charge;
}

bool SwitchPlatform::isBatteryCharging() {
    PsmChargerType type;
    psmGetChargerType(&type);
    return type == PsmChargerType_EnoughPower || type == PsmChargerType_LowPower;
}

bool SwitchPlatform::hasWirelessConnection() {
    bool res = false;
    nifmIsWirelessCommunicationEnabled(&res);
    return res;
}

bool SwitchPlatform::hasEthernetConnection() {
    bool res = false;
    nifmIsEthernetCommunicationEnabled(&res);
    return res;
}

int SwitchPlatform::getWirelessLevel() {
    NifmInternetConnectionType type;
    u32 wifiSignal;
    NifmInternetConnectionStatus status;
    nifmGetInternetConnectionStatus(&type, &wifiSignal, &status);
    return wifiSignal;
}

// wiliwili asks for this exactly while the playback core is playing, which is
// also the only time the applet must not start its idle countdown.
void SwitchPlatform::disableScreenDimming(bool disable, const std::string& reason, const std::string& app) {
    screenDimmingDisabled = disable;
    mediaPlaybackActive   = disable;
    appletSetMediaPlaybackState(disable);
}

bool SwitchPlatform::isScreenDimmingDisabled() { return screenDimmingDisabled; }

void SwitchPlatform::setBacklightBrightness(float brightness) {
    if (brightness < 0) brightness = 0.0f;
    if (brightness > 1) brightness = 1.0f;
    lblSetCurrentBrightnessSetting(brightness);
}

float SwitchPlatform::getBacklightBrightness() {
    float brightness = 0.0f;
    lblGetCurrentBrightnessSetting(&brightness);
    return brightness;
}

bool SwitchPlatform::canSetBacklightBrightness() { return true; }

std::string SwitchPlatform::getIpAddress() {
    u32 ip;
    nifmGetCurrentIpAddress(&ip);
    return fmt::format("{}.{}.{}.{}", ip & 0xff, (ip & 0xff00) >> 8, (ip & 0xff0000) >> 16, (ip & 0xff000000) >> 24);
}

std::string SwitchPlatform::getDnsServer() {
    u32 ip, mask, gateway, dns1, dns2;
    nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2);
    std::string dns1_str = fmt::format("{}.{}.{}.{}", dns1 & 0xff, (dns1 & 0xff00) >> 8, (dns1 & 0xff0000) >> 16,
                                       (dns1 & 0xff000000) >> 24);
    std::string dns2_str = fmt::format("{}.{}.{}.{}", dns2 & 0xff, (dns2 & 0xff00) >> 8, (dns2 & 0xff0000) >> 16,
                                       (dns2 & 0xff000000) >> 24);
    return dns1_str + "\n" + dns2_str;
}

bool SwitchPlatform::isApplicationMode() {
    AppletType at = appletGetAppletType();
    return at == AppletType_Application || at == AppletType_SystemApplication;
}

void SwitchPlatform::exitToHomeMode(bool value) { __nx_applet_exit_mode = wili::switch2AppletExitMode(value); }

void SwitchPlatform::forceEnableGamePlayRecording() { appletInitializeGamePlayRecording(); }

void SwitchPlatform::openBrowser(std::string url) {
    WebCommonConfig config;

    Result rc = webPageCreate(&config, url.c_str());
    if (R_SUCCEEDED(rc)) {
        rc = webConfigSetWhitelist(&config, "^http*");
        if (R_SUCCEEDED(rc)) {
            rc = webConfigShow(&config, NULL);
        }
    }
}

std::string SwitchPlatform::getName() { return "Switch 2"; }

bool SwitchPlatform::mainLoopIteration() {
    bool active = false;

    // SDL owns the window and the GL surface here, and nobody else drains its
    // queue: without this loop the events pile up for the whole session.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        active = true;
        if (event.type == SDL_QUIT) exitRequested = true;
    }

    // libnx updates the applet state (operation mode, focus) in here, so every
    // applet query below has to happen after it and never before.
    if (!appletMainLoop()) return false;

    if (exitRequested) {
        Logger::info("switch2: applet exit requested");
        Application::quit();
    }

    if (resumeRequested.exchange(false)) {
        // The console may have come back on a different network: drop the stale
        // DNS answers, then report the audio and network state before a stream is
        // opened again. The resume itself is always recorded; the probe is rate
        // limited, because it opens aud:u.
        Logger::info("switch2: resumed");
        wiliwili_boot_note("lifecycle", "resumed");
        wiliwili_network_reset();
        const int64_t now = getCPUTimeUsec();
        if (probeSchedule.due(now)) {
            probeSchedule.markRun(now);
            wili::lifecycleProbeNote(wili::runLifecycleProbe(wili::probeAudioRoute(mediaPlaybackActive.load())),
                                     "resume");
        }
        active = true;
    }

    if (performanceModeChanged.exchange(false)) {
        Logger::info("switch2: performance mode {}",
                     appletGetPerformanceMode() == ApmPerformanceMode_Boost ? "boost" : "normal");
        active = true;
    }

    if (focusReported.exchange(false)) {
        // The input manager and the playback core both react to this: held
        // mouse buttons and pointer claims are dropped on focus loss, mpv
        // pauses, and both come back here.
        Application::getWindowFocusChangedEvent()->fire(focused);
        active = true;
    }

    const bool displayChanged = wili::displayPoll(displayRecheck.exchange(false));
    active                    = active || displayChanged;

    // nifm is polled while the console stays awake: a Wi-Fi drop and reconnect
    // never suspends it, so no applet hook announces that the connection the open
    // sockets and DNS answers were made for is gone.
    const int64_t networkNow = getCPUTimeUsec();
    if (networkWatch.due(networkNow)) {
        networkWatch.markPolled(networkNow);

        wili::NetworkState state;
        NifmInternetConnectionType type     = NifmInternetConnectionType_WiFi;
        u32 signal                          = 0;
        NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &signal, &status)))
            state.connected = status == NifmInternetConnectionStatus_Connected;
        if (state.connected) nifmGetCurrentIpAddress(&state.address);

        if (networkWatch.observe(state)) {
            Logger::info("switch2: network changed, dropping stale sockets and DNS answers");
            wiliwili_boot_note("lifecycle", "network changed");
            wiliwili_network_reset();
            if (probeSchedule.due(networkNow)) {
                probeSchedule.markRun(networkNow);
                wili::lifecycleProbeNote(wili::runLifecycleProbe(wili::probeAudioRoute(mediaPlaybackActive.load())),
                                         "network");
            }
            active = true;
        }
    }

    // Presentation pacing. Swap interval 0 means the user pinned a frame rate in
    // the settings, which the pacer leaves alone. The panel rate itself needs no
    // code: a swap interval of 1 presents once per iteration, so the loop is
    // already at 60 or at the 120 Hz the console granted.
    framePacer.setAdaptive(VideoContext::swapInterval != 0);
    const bool busy = active || mediaPlaybackActive || !Ticking::runningTickings.empty() || inputActive();
    const int cap   = framePacer.update(busy, getCPUTimeUsec());
    if (cap == wili::FramePacer::LEAVE_ALONE) {
        frameCapApplied = false;
    } else if (!frameCapApplied || cap != appliedFrameCap) {
        Application::setLimitedFPS(size_t(cap));
        appliedFrameCap = cap;
        frameCapApplied = true;
        Logger::verbose("switch2: frame cap {}", cap == 0 ? std::string("every refresh") : fmt::format("{}/s", cap));
    }

    return true;
}

// The playback core asks this right before it opens a stream. MPV opens aud:u
// when it initializes audio, so the audio half of the probe only runs while
// nothing plays: opening the route underneath the player closes its handle.
extern "C" void wiliwili_pre_playback_probe(void) {
    wili::lifecycleProbeNote(wili::runLifecycleProbe(wili::probeAudioRoute(mediaPlaybackActive.load())), "playback");
}

VideoContext* SwitchPlatform::getVideoContext() { return this->videoContext; }

std::string SwitchPlatform::getLocale() { return this->locale; }

AudioPlayer* SwitchPlatform::getAudioPlayer() { return this->audioPlayer; }

InputManager* SwitchPlatform::getInputManager() { return this->inputManager; }

FontLoader* SwitchPlatform::getFontLoader() { return this->fontLoader; }

ImeManager* SwitchPlatform::getImeManager() { return this->imeManager; }

ThemeVariant SwitchPlatform::getThemeVariant() { return this->themeVariant; }

void SwitchPlatform::setThemeVariant(ThemeVariant theme) {
    this->themeVariant = theme;
#ifdef BOREALIS_USE_DEKO3D
    this->videoContext->recordStaticCommands();
#endif
}

SwitchPlatform::~SwitchPlatform() {
    // The order is the contract in lifecycle_probe.hpp, checked by the host
    // tests. The input manager has to go before the video context: its
    // destructor deletes the cursor image through the NVG context, and the NVG
    // context dies with the video context.
    for (wili::TeardownStep step : wili::TEARDOWN_ORDER) {
        switch (step) {
            case wili::TeardownStep::Unhook:
                appletUnhook(&applet_hook_cookie);
                break;
            case wili::TeardownStep::Input:
                delete this->inputManager;
                this->inputManager = nullptr;
                break;
            case wili::TeardownStep::Video:
                delete this->videoContext;
                this->videoContext = nullptr;
                break;
            case wili::TeardownStep::Audio:
                delete this->audioPlayer;
                this->audioPlayer = nullptr;
                break;
            case wili::TeardownStep::Ime:
                delete this->imeManager;
                this->imeManager = nullptr;
                break;
            case wili::TeardownStep::Font:
                delete this->fontLoader;
                this->fontLoader = nullptr;
                break;
            case wili::TeardownStep::Display:
                wili::displayShutdown();
                break;
        }
    }
}

}  // namespace brls
