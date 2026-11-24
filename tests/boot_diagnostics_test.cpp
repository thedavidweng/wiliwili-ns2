// Host checks for the Switch 2 boot diagnostics and the config namespaces.
// Run by ctest as the boot_diagnostics case.
//
// The expected values are the device visible contracts of issue #3: Switch 2
// keeps its settings in /config/wiliwili-ns2, sends no telemetry, and a driver
// string can never break the one-line-per-note format of boot.log.

#include <string>
#include <vector>

#include "test_check.hpp"
#include "utils/boot_diagnostics.hpp"
#include "utils/number_helper.hpp"

namespace {

using test_check::check;
using test_check::failures;

std::string read_source(const std::string& relative) { return test_check::readSource(relative); }

void check_config_namespaces() {
    // Switch 2 must never share a directory with Switch 1: a Switch 1 install
    // on the same SD card stays untouched.
    CHECK_EQ(wili::configDirForPlatform("switch2"), "/config/wiliwili-ns2");
    CHECK_EQ(wili::configDirForPlatform("switch"), "/config/wiliwili");
    CHECK_EQ(wili::configDirForPlatform("psv"), "ux0:/data/wiliwili");
    CHECK_EQ(wili::configDirForPlatform("ps4"), "/data/wiliwili");
    CHECK(wili::configDirForPlatform("switch2") != wili::configDirForPlatform("switch"));
    CHECK(wili::configDirForPlatform("switch2").rfind("/config/", 0) == 0);

    // The desktop ports derive their directory from the environment instead.
    CHECK(wili::configDirForPlatform("desktop").empty());
    CHECK(wili::configDirForPlatform("ios").empty());
    CHECK(wili::configDirForPlatform(nullptr).empty());

    // boot.log lives next to the settings, in the same isolated directory.
    CHECK_EQ(wili::bootLogPath("switch2"), "/config/wiliwili-ns2/boot.log");
    CHECK_EQ(wili::bootLogPath("switch"), "/config/wiliwili/boot.log");
    CHECK(wili::bootLogPath("desktop").empty());
}

void check_telemetry() {
    CHECK(!wili::telemetryEnabledForPlatform("switch2"));
    CHECK(wili::telemetryEnabledForPlatform("switch"));
    CHECK(wili::telemetryEnabledForPlatform("ps4"));
    CHECK(wili::telemetryEnabledForPlatform("desktop"));
    CHECK(!wili::telemetryEnabledForPlatform(nullptr));
}

void check_sanitizer() {
    CHECK_EQ(wili::sanitizeLogValue("Mesa/X.org"), "Mesa/X.org");
    // One log line stays one log line.
    CHECK_EQ(wili::sanitizeLogValue("zink (Vulkan 1.4.318)\nZink"), "zink (Vulkan 1.4.318) Zink");
    CHECK_EQ(wili::sanitizeLogValue("  \t Mesa \r\n\n"), "Mesa");
    CHECK_EQ(wili::sanitizeLogValue("a\x01\x02"
                                    "b"),
             "a b");
    CHECK_EQ(wili::sanitizeLogValue("a  b"), "a b");
    CHECK(wili::sanitizeLogValue("").empty());
    CHECK(wili::sanitizeLogValue("\x7f\x01", 8).empty());

    // 10 bytes: seven characters and an ellipsis.
    CHECK_EQ(wili::sanitizeLogValue("0123456789abcdef", 10), "0123456...");
    // Exactly at the cap: no ellipsis.
    CHECK_EQ(wili::sanitizeLogValue("abcdefghij", 10), "abcdefghij");
    // Too small for an ellipsis: plain truncation.
    CHECK_EQ(wili::sanitizeLogValue("0123456789abc", 2), "01");
    CHECK(wili::sanitizeLogValue("abcdefghij", 0).empty());

    // A truncated value never ends inside a UTF-8 sequence: 中 is three bytes.
    CHECK_EQ(wili::sanitizeLogValue("中中中中", 8), "中...");
    CHECK_EQ(wili::sanitizeLogValue("中文渲染器", 100), "中文渲染器");
}

void check_gpu_formatter() {
    wili::GpuDiagnostics gl;
    gl.api    = "OpenGL ES 2.0";
    gl.fields = {
        {"GL_VENDOR", "Mesa"},
        {"GL_RENDERER", "zink (Vulkan 1.4.318, NVK)"},
        {"", "field without a name is dropped"},
        {"GL_VERSION", "OpenGL ES 2.0 Mesa 25.1.0"},
    };
    CHECK_EQ(wili::formatGpuDiagnostics(gl),
             "gpu api: OpenGL ES 2.0\n"
             "GL_VENDOR: Mesa\n"
             "GL_RENDERER: zink (Vulkan 1.4.318, NVK)\n"
             "GL_VERSION: OpenGL ES 2.0 Mesa 25.1.0\n");

    // Without an API name the report still says which block it is.
    wili::GpuDiagnostics vulkan;
    vulkan.fields = {{"vkDeviceName", "NVK (NVIDIA Switch 2 GPU)"}};
    CHECK_EQ(wili::formatGpuDiagnostics(vulkan), "gpu api: unknown\nvkDeviceName: NVK (NVIDIA Switch 2 GPU)\n");

    // Driver strings go through the sanitizer on the way out.
    wili::GpuDiagnostics dirty;
    dirty.api    = "Vulkan\t1.4";
    dirty.fields = {{"vkDriverInfo", "Mesa 25.1\r\nNVK"}};
    CHECK_EQ(wili::formatGpuDiagnostics(dirty), "gpu api: Vulkan 1.4\nvkDriverInfo: Mesa 25.1 NVK\n");

    // An empty report is still a report.
    CHECK_EQ(wili::formatGpuDiagnostics({}), "gpu api: unknown\n");
}

// Driver extension lists are long, and the 200 byte cap that keeps other driver
// strings on one readable line would cut their tail off. The extensions that
// say whether OunceVK can present at all (VK_NN_vi_surface) or hand out GPU
// memory are as likely to be at the end as at the start, so the list wraps
// instead of being truncated.
bool lines_within(const std::string& text, size_t maxLength, std::string& offender) {
    size_t start = 0;
    while (start < text.size()) {
        const size_t end       = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.size() > maxLength) {
            offender = line;
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

void check_extension_list() {
    // Nothing to report is still a report.
    CHECK_EQ(wili::formatExtensionList("GL_EXTENSIONS", {}), "GL_EXTENSIONS: none\n");

    // A list that fits stays on one line, with its count.
    CHECK_EQ(wili::formatExtensionList("GL_EXTENSIONS", {"GL_EXT_a", "GL_EXT_b", "GL_EXT_c"}),
             "GL_EXTENSIONS (3): GL_EXT_a GL_EXT_b GL_EXT_c\n");

    // Too long for one line: wrapped, and every line says what it is.
    CHECK_EQ(
        wili::formatExtensionList(
            "GL_EXTENSIONS", {"GL_EXT_aaaaaaaaaa", "GL_EXT_bbbbbbbbbb", "GL_EXT_cccccccccc", "GL_EXT_dddddddddd"}, 40),
        "GL_EXTENSIONS (4): GL_EXT_aaaaaaaaaa\n"
        ".. GL_EXT_bbbbbbbbbb GL_EXT_cccccccccc\n"
        ".. GL_EXT_dddddddddd\n");

    // An entry with no room left is marked as truncated instead of overflowing:
    // 40 bytes of line, 19 of header, the first 18 bytes of the name and "...".
    CHECK_EQ(wili::formatExtensionList("GL_EXTENSIONS", {"GL_EXT_" + std::string(40, 'z')}, 40),
             "GL_EXTENSIONS (1): GL_EXT_zzzzzzzzzzz...\n");

    // The real thing: 30 Vulkan extensions, all of them recorded, every line
    // inside the cap.
    std::vector<std::string> vulkan;
    for (int i = 0; i < 29; ++i) vulkan.push_back("VK_KHR_some_reasonably_long_extension_name_" + std::to_string(i));
    vulkan.push_back("VK_NN_vi_surface");

    const std::string report = wili::formatExtensionList("vkDeviceExtensions", vulkan);
    CHECK(report.rfind("vkDeviceExtensions (30): ", 0) == 0);
    for (const auto& name : vulkan) CHECK(report.find(name) != std::string::npos);
    std::string offender;
    check(lines_within(report, 120, offender),
          "every extension line stays inside the cap, offending line: \"" + offender + "\"", __LINE__);
}

// The Vulkan half of the GPU report has to list what the driver offers, not
// just its name: the surface and memory extensions are the difference between
// "Zink found a device" and "OunceVK can present on this console".
void check_gpu_report_sources() {
    const std::string video = read_source("platform/switch2/sdl_video.cpp");
    CHECK(video.find("vkEnumerateInstanceExtensionProperties") != std::string::npos);
    CHECK(video.find("vkEnumerateDeviceExtensionProperties") != std::string::npos);
    CHECK(video.find("GL_EXTENSIONS") != std::string::npos);
    // The extension lists go through the wrapper, not through a capped field.
    CHECK(video.find("wili::formatExtensionList") != std::string::npos);
    CHECK(video.find("vkGetPhysicalDeviceProperties") != std::string::npos);

    // And they are written to boot.log, one note per line, like the rest.
    CHECK(video.find("wiliwili_boot_note") != std::string::npos);
}

// The boot has to stay cheap and readable: no player instance before the user
// opens a video, and a failed start reported rather than silent.
void check_boot_stays_light() {
    const std::string main = read_source("wiliwili/source/main.cpp");
    // MPVCore is a singleton: instance() creates it, including the mpv context.
    CHECK(main.find("MPVCore::instance()") == std::string::npos);
    CHECK(main.find("wiliwili_boot_panic") != std::string::npos);
    CHECK(main.find("catch (const std::exception&") != std::string::npos);

    // Settings initialisation only writes MPVCore's static options.
    const std::string config = read_source("wiliwili/source/utils/config_helper.cpp");
    CHECK(config.find("MPVCore::instance()") == std::string::npos);
    CHECK(config.find("wili::configDirForPlatform(wili::PLATFORM_ID)") != std::string::npos);
}

// The API hands out numeric ids as strings. A malformed one is a normal answer,
// so it has to fail a check instead of throwing on the UI thread.
void check_api_ids() {
    uint64_t id = 7;
    CHECK(wiliwili::parseUid("2511565362", id));
    CHECK_EQ(id, (uint64_t)2511565362);

    // Anything that is not exactly one decimal id leaves the value alone.
    id = 7;
    CHECK(!wiliwili::parseUid("", id));
    CHECK(!wiliwili::parseUid("abc", id));
    CHECK(!wiliwili::parseUid("12a", id));
    CHECK(!wiliwili::parseUid("-5", id));
    CHECK(!wiliwili::parseUid(" 42", id));
    CHECK(!wiliwili::parseUid("99999999999999999999999999", id));
    CHECK_EQ(id, (uint64_t)7);

    // ... and the dynamic feed uses it instead of the throwing conversion.
    const std::string dynamic = read_source("wiliwili/source/fragment/dynamic_tab.cpp");
    CHECK(dynamic.find("std::stoull") == std::string::npos);
    CHECK(dynamic.find("parseUid(") != std::string::npos);
}

}  // namespace

int main() {
    check_config_namespaces();
    check_telemetry();
    check_sanitizer();
    check_gpu_formatter();
    check_extension_list();
    check_gpu_report_sources();
    check_boot_stays_light();
    check_api_ids();

    return test_check::report("all boot diagnostics checks passed");
}
