#include "utils/boot_diagnostics.hpp"

#include <string>

#include "utils/boot_api.h"

#if defined(__SWITCH2__)
#include <sys/stat.h>

#include <cstdio>
#include <mutex>
#endif

namespace wili {

namespace {

bool isContinuationByte(char value) { return (static_cast<unsigned char>(value) & 0xC0) == 0x80; }

#if defined(__SWITCH2__)
constexpr size_t BOOT_LOG_MAX_BYTES = 64 * 1024;

void createParentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    const std::string parent = path.substr(0, slash);
    // EEXIST is the normal case: the config directory outlives the boot.
    mkdir(parent.c_str(), 0777);
}
#endif

}  // namespace

std::string configDirForPlatform(const char* platformId) {
    if (platformId == nullptr) return {};
    const std::string id{platformId};
    if (id == "switch2") return CONFIG_DIR_SWITCH2;
    if (id == "switch") return CONFIG_DIR_SWITCH;
    if (id == "psv") return CONFIG_DIR_PSV;
    if (id == "ps4") return CONFIG_DIR_PS4;
    return {};
}

bool telemetryEnabledForPlatform(const char* platformId) {
    if (platformId == nullptr) return false;
    return std::string{platformId} != "switch2";
}

bool tlsVerifyDefaultForPlatform(const char* platformId) {
    if (platformId == nullptr) return false;
    const std::string id{platformId};
    // Switch 1 and the PlayStation ports hand TLS to a stack this repository
    // does not verify against, and their installs keep whatever the old default
    // was. Switch 2 is the first port whose default is verification.
    if (id == "switch" || id == "psv" || id == "ps4") return false;
    return true;
}

std::string sanitizeLogValue(const std::string& value, size_t maxLength) {
    std::string out;
    out.reserve(value.size());

    // Collapse whitespace while copying: a driver is free to return a renderer
    // string with tabs and newlines in it, but one log line has to stay one
    // line. lastIsSpace starts true so leading spaces are dropped as well.
    bool lastIsSpace = true;
    for (const char value_char : value) {
        const unsigned char byte = static_cast<unsigned char>(value_char);
        if (byte < 0x20 || byte == 0x7f) {
            if (!lastIsSpace) {
                out.push_back(' ');
                lastIsSpace = true;
            }
            continue;
        }
        if (byte == ' ') {
            if (lastIsSpace) continue;
            out.push_back(' ');
            lastIsSpace = true;
            continue;
        }
        out.push_back(value_char);
        lastIsSpace = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();

    if (out.size() <= maxLength) return out;

    size_t cut = maxLength < 3 ? maxLength : maxLength - 3;
    while (cut > 0 && isContinuationByte(out[cut])) --cut;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (maxLength >= 3) out += "...";
    return out;
}

std::string formatGpuDiagnostics(const GpuDiagnostics& diagnostics) {
    const std::string api = sanitizeLogValue(diagnostics.api, 64);
    std::string out        = "gpu api: " + (api.empty() ? std::string{"unknown"} : api) + "\n";
    for (const auto& field : diagnostics.fields) {
        const std::string name = sanitizeLogValue(field.name, 64);
        if (name.empty()) continue;
        out += name + ": " + sanitizeLogValue(field.value) + "\n";
    }
    return out;
}

std::string formatExtensionList(const std::string& name, const std::vector<std::string>& extensions,
                                size_t maxLength) {
    std::string label = sanitizeLogValue(name, 64);
    if (label.empty()) label = "extensions";

    // The per-entry budget comes from the widest header the list can produce, so
    // an entry that had to be cut still fits on whichever line it lands on.
    const size_t headerLength = label.size() + 5 + std::to_string(extensions.size()).size();
    const size_t entryBudget  = maxLength > headerLength ? maxLength - headerLength : 1;

    std::vector<std::string> entries;
    entries.reserve(extensions.size());
    for (const auto& extension : extensions) {
        std::string entry = sanitizeLogValue(extension, entryBudget);
        if (!entry.empty()) entries.push_back(std::move(entry));
    }
    if (entries.empty()) return label + ": none\n";

    const std::string continuation = ".. ";
    std::string line               = label + " (" + std::to_string(entries.size()) + "): ";
    std::string out;
    bool lineIsEmpty = true;
    for (const auto& entry : entries) {
        if (lineIsEmpty) {
            line += entry;
            lineIsEmpty = false;
            continue;
        }
        if (line.size() + 1 + entry.size() <= maxLength) {
            line += ' ';
            line += entry;
            continue;
        }
        out += line;
        out += '\n';
        line = continuation + entry;
    }
    out += line;
    out += '\n';
    return out;
}

std::string bootLogPath(const char* platformId) {
    const std::string dir = configDirForPlatform(platformId);
    if (dir.empty()) return {};
    return dir + "/boot.log";
}

void bootNote(const std::string& stage, const std::string& detail) {
#if defined(__SWITCH2__)
    // Only the Switch 2 port keeps a boot log: it is the port that can fail
    // before there is a window to read an error from.
    const std::string path = bootLogPath(PLATFORM_ID);
    if (path.empty()) return;
    createParentDirectory(path);

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    // A boot log that grows until the SD card is full is worse than a short
    // one: keep the history until it reaches the cap, then start over.
    static bool bootStarted = false;
    if (!bootStarted) {
        bootStarted = true;
        FILE* probe = fopen(path.c_str(), "ab");
        if (probe != nullptr) {
            fseek(probe, 0, SEEK_END);
            const long size = ftell(probe);
            fclose(probe);
            if (size > static_cast<long>(BOOT_LOG_MAX_BYTES)) {
                FILE* reset = fopen(path.c_str(), "wb");
                if (reset != nullptr) fclose(reset);
            }
        }
    }

    FILE* log = fopen(path.c_str(), "a");
    if (log == nullptr) return;
    fprintf(log, "[%s] %s\n", sanitizeLogValue(stage, 32).c_str(), sanitizeLogValue(detail, 512).c_str());
    fclose(log);
#else
    // The desktop ports have a terminal, and Switch 1 has its own wrapper.
    (void)stage;
    (void)detail;
#endif
}

}  // namespace wili

extern "C" void wiliwili_boot_note(const char* stage, const char* detail) {
    wili::bootNote(stage != nullptr ? stage : "boot", detail != nullptr ? detail : "");
}
