//
// Switch 2 lifecycle: the pre-playback audio/socket probe and the teardown
// contract.
//
// The parts in this header are pure: no <switch.h>, no SDL. lifecycle_probe.cpp
// is the libnx side that opens aud:u and asks nifm, and switch_platform.cpp runs
// both from the applet hooks. tests/switch2_lifecycle_test.cpp checks the
// decisions here without a console.
//

#pragma once

#include <cstdint>
#include <string>

namespace wili {

// What one probe found. The two halves are reported separately because they
// fail separately: audio is the aud:u route the playback core will use, the
// network half is nifm's view of the console's connection after a sleep or a
// Wi-Fi drop. No address is ever stored, so the log line stays sanitized.
struct ProbeResult {
    bool ran          = false;
    bool audioInitOk  = false;
    bool audioOk      = false;
    bool audioSkipped = false;  // aud:u is in use; the audio half was not run
    int audioOutputs  = 0;
    bool connected    = false;
    bool addressOk    = false;
    bool dnsOk        = false;

    bool healthy() const {
        return (this->audioOk || this->audioSkipped) && this->connected && this->addressOk && this->dnsOk;
    }

    // "<half>: <state>" for each half, one boot.log line, no addresses.
    std::string summary() const {
        if (!this->ran) return "not run";

        std::string audio = "init failed";
        if (this->audioSkipped)
            audio = "owned by playback";
        else if (this->audioInitOk)
            audio = this->audioOk ? std::to_string(this->audioOutputs) + " outputs" : "no outputs";

        std::string socket = "not connected";
        if (this->connected) socket = this->addressOk ? "connected" : "connected without address";

        return "audio " + audio + ", socket " + socket + ", dns " + (this->dnsOk ? "configured" : "unresolved");
    }
};

// The audio half of a probe opens aud:u. MPV holds that route from the moment it
// initializes audio, and opening it underneath a player closes the handle mpv is
// using, so a pre-playback probe skips it exactly then.
inline bool probeAudioRoute(bool playback_holds_audio) { return !playback_holds_audio; }

// nifm's view of the console's connection, sampled on a timer. A Wi-Fi drop and
// reconnect never suspends the console, so no applet hook reports it and the
// platform has to look: the connection that comes back is not the one the open
// sockets and DNS answers were made for.
struct NetworkState {
    bool connected   = false;
    uint32_t address = 0;

    bool operator==(const NetworkState& other) const {
        return this->connected == other.connected && this->address == other.address;
    }
    bool operator!=(const NetworkState& other) const { return !(*this == other); }
};

class NetworkWatch {
public:
    // Two seconds: one nifm query, not a poll per frame.
    static constexpr int64_t POLL_INTERVAL_US = 2 * 1000 * 1000;

    explicit NetworkWatch(int64_t intervalUs = POLL_INTERVAL_US) : intervalUs(intervalUs) {}

    bool due(int64_t nowUs) const { return !this->polled || nowUs - this->lastPollUs >= this->intervalUs; }

    void markPolled(int64_t nowUs) {
        this->polled     = true;
        this->lastPollUs = nowUs;
    }

    // True when the socket state changed in a way that invalidates the DNS
    // answers and pooled connections opened before it: a reconnect, or a new
    // address. The first sample only records the baseline, and a drop alone
    // needs no reset because there is nothing to reuse yet.
    bool observe(const NetworkState& state) {
        if (!this->seen) {
            this->seen = true;
            this->last = state;
            return false;
        }

        const bool changed = state != this->last;
        this->last         = state;
        return changed && state.connected;
    }

private:
    int64_t intervalUs = POLL_INTERVAL_US;
    int64_t lastPollUs = 0;
    bool polled        = false;
    bool seen          = false;
    NetworkState last{};
};

// Rate limit for the probe. Opening aud:u and nifm state is not something to do
// every frame after a sleep/wake, and the first ask is always due.
class ProbeSchedule {
public:
    explicit ProbeSchedule(int64_t minIntervalUs) : minIntervalUs(minIntervalUs) {}

    bool due(int64_t nowUs) const { return !this->ran || nowUs - this->lastRunUs >= this->minIntervalUs; }

    void markRun(int64_t nowUs) {
        this->ran       = true;
        this->lastRunUs = nowUs;
    }

private:
    int64_t minIntervalUs;
    int64_t lastRunUs = 0;
    bool ran          = false;
};

// The platform destructor runs these in this order. The input manager has to be
// destroyed before the video context because it deletes the cursor image
// through the NVG context (#4's destructor), the applet hook has to be unhooked
// before anything it can reach is gone, and the display change event is closed
// last, once nothing can ask for a resize any more.
enum class TeardownStep {
    Unhook,
    Input,
    Video,
    Audio,
    Ime,
    Font,
    Display,
};

inline constexpr TeardownStep TEARDOWN_ORDER[] = {
    TeardownStep::Unhook, TeardownStep::Input, TeardownStep::Video,   TeardownStep::Audio,
    TeardownStep::Ime,    TeardownStep::Font,  TeardownStep::Display,
};

// --- The Switch 2 side, implemented in lifecycle_probe.cpp ----------------

// Asks the console: audio route (aud:u) and network state. `probeAudioRoute` is
// false while playback owns the audio route, because opening aud:u then could
// close the handle the playback core is using.
ProbeResult runLifecycleProbe(bool probeAudioRoute);

// Writes one sanitized boot.log line: "<when>: <summary>".
void lifecycleProbeNote(const ProbeResult& result, const char* when);

}  // namespace wili
