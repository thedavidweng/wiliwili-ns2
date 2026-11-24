// Host checks for the Switch 2 boot diagnostics and the config namespaces.
// Run by ctest as the boot_diagnostics case.
//
// The expected values are the device visible contracts of issue #3: Switch 2
// keeps its settings in /config/wiliwili-ns2, sends no telemetry, and a driver
// string can never break the one-line-per-note format of boot.log.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "utils/boot_diagnostics.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, int line) {
    if (ok) return;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
    ++failures;
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected)                                                       \
    check((actual) == (expected),                                                        \
          std::string(#actual) + " == " + std::string(#expected) + " (got \"" + (actual) + \
              "\", want \"" + (expected) + "\")",                                        \
          __LINE__)

std::string read_source(const std::string& relative) {
    std::ifstream file(std::string(WILIWILI_SOURCE_DIR) + "/" + relative);
    if (!file) {
        std::cerr << "FAIL: cannot read " << relative << '\n';
        ++failures;
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

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
    CHECK_EQ(wili::sanitizeLogValue("a\x01\x02" "b"), "a b");
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

}  // namespace

int main() {
    check_config_namespaces();
    check_telemetry();
    check_sanitizer();
    check_gpu_formatter();
    check_boot_stays_light();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all boot diagnostics checks passed\n";
    return 0;
}
