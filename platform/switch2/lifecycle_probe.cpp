/*
 * Switch 2 pre-playback probe: audio route and network state.
 *
 * Both halves of playback break silently after a suspend or a Wi-Fi drop, and
 * both are cheap to ask about before a stream is opened. The audio half goes
 * through libounce's audoutInitialize (which routes to aud:u on this console);
 * the network half asks nifm for the connection, the address and the DNS server.
 * Nothing here is fatal: a failed probe is a line in boot.log.
 *
 * Addresses are never logged, only whether they exist.
 */

#include "lifecycle_probe.hpp"

#include <switch.h>

#include <fmt/format.h>

#include <borealis/core/logger.hpp>

#include "utils/boot_api.h"

namespace {

// audoutListAudioOuts writes fixed 0x100 byte names, one per output.
constexpr int MAX_AUDIO_OUTPUTS         = 4;
constexpr size_t AUDIO_OUTPUT_NAME_SIZE = 0x100;

bool probeAudio(int& outputs) {
    outputs         = 0;
    const Result rc = audoutInitialize();
    if (R_FAILED(rc)) return false;

    char names[MAX_AUDIO_OUTPUTS * AUDIO_OUTPUT_NAME_SIZE] = {};
    u32 count                                              = 0;
    const Result listRc                                    = audoutListAudioOuts(names, MAX_AUDIO_OUTPUTS, &count);
    audoutExit();

    if (R_FAILED(listRc)) return false;
    outputs = int(count);
    return true;
}

}  // namespace

namespace wili {

ProbeResult runLifecycleProbe(bool probeAudioRoute) {
    ProbeResult result;
    result.ran = true;

    if (probeAudioRoute) {
        result.audioInitOk = probeAudio(result.audioOutputs);
        result.audioOk     = result.audioInitOk && result.audioOutputs > 0;
    } else {
        // Playback owns the audio route right now: opening aud:u underneath it
        // could close the handle mpv is using. The socket half still runs.
        result.audioSkipped = true;
    }

    NifmInternetConnectionType type     = NifmInternetConnectionType_WiFi;
    u32 signalStrength                  = 0;
    NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &signalStrength, &status)))
        result.connected = status == NifmInternetConnectionStatus_Connected;

    u32 address = 0;
    if (result.connected && R_SUCCEEDED(nifmGetCurrentIpAddress(&address))) result.addressOk = address != 0;

    u32 ip = 0, mask = 0, gateway = 0, primaryDns = 0, secondaryDns = 0;
    if (result.addressOk && R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &primaryDns, &secondaryDns)))
        result.dnsOk = primaryDns != 0 || secondaryDns != 0;

    return result;
}

void lifecycleProbeNote(const ProbeResult& result, const char* when) {
    const std::string line = fmt::format("{}: {}", when, result.summary());
    brls::Logger::info("switch2 probe {}", line);
    wiliwili_boot_note("probe", line.c_str());
}

}  // namespace wili
