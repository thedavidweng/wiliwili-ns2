//
// Boot diagnostics for the console ports.
//
// The Switch 2 port starts on a driver stack it cannot test offline (SDL2 over
// EGL/GLES from OunceVK, which is Zink on top of Mesa NVK). When that stack
// fails on hardware there is no terminal and no window, so everything that can
// fail has to leave a line in {configDir}/boot.log instead.
//

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wili {

// Where each console port keeps its settings. Switch 2 has its own directory so
// a Switch 1 install on the same SD card is never read or rewritten.
inline constexpr const char* CONFIG_DIR_SWITCH  = "/config/wiliwili";
inline constexpr const char* CONFIG_DIR_SWITCH2 = "/config/wiliwili-ns2";
inline constexpr const char* CONFIG_DIR_PSV     = "ux0:/data/wiliwili";
inline constexpr const char* CONFIG_DIR_PS4     = "/data/wiliwili";

// The port this binary was built for.
constexpr const char* PLATFORM_ID =
#if defined(__SWITCH2__)
        "switch2";
#elif defined(__SWITCH__)
        "switch";
#elif defined(__PSV__)
        "psv";
#elif defined(PS4)
        "ps4";
#elif defined(IOS)
        "ios";
#else
        "desktop";
#endif

// The fixed config directory of a console port, empty for the desktop ports
// (they derive it from $HOME or the current directory instead).
std::string configDirForPlatform(const char* platformId);

// Second line of defence behind the compile time NO_GA switch: the Switch 2
// port ships without third-party telemetry.
bool telemetryEnabledForPlatform(const char* platformId);

struct GpuDiagnosticField {
    std::string name;
    std::string value;
};

// One driver's capability report: GL strings for the Zink context, Vulkan
// properties for the NVK device underneath it.
struct GpuDiagnostics {
    std::string api;
    std::vector<GpuDiagnosticField> fields;
};

// Turns a driver supplied string into one log line: control characters become
// spaces, runs of spaces collapse, leading and trailing spaces go, and the
// result is capped at maxLength bytes without splitting a UTF-8 sequence.
std::string sanitizeLogValue(const std::string& value, size_t maxLength = 200);

// "gpu api: <api>" followed by one "name: value" line per field.
std::string formatGpuDiagnostics(const GpuDiagnostics& diagnostics);

// A capability list (the GLES extension string, the Vulkan instance or device
// extensions) as log lines: "<name> (<count>): first second ..." on the first
// line, continued with ".. next ..." while the list lasts. Lists are long and
// truncating them would drop the extensions worth reading, so they wrap at
// maxLength instead; only an entry too long for a line is cut, and marked
// with an ellipsis when it is.
std::string formatExtensionList(const std::string& name, const std::vector<std::string>& extensions,
                                size_t maxLength = 120);

// {configDir}/boot.log for the console ports, empty when the port keeps none.
std::string bootLogPath(const char* platformId);

// Appends one "[stage] detail" line to bootLogPath(PLATFORM_ID). Never throws:
// a failing boot log must not take the boot down with it.
void bootNote(const std::string& stage, const std::string& detail);

}  // namespace wili
