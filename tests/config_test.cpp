// Host checks for the pinned upstream revisions and the Switch 2 build
// configuration. Run by ctest as the config_pins case.
//
// The expected values come from the repository decisions recorded in
// docs/adr/ and in issue #2, not from the files under test: a changed pin or a
// floating `latest` has to fail here before it reaches a device.

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
    ++failures;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

std::string read(const std::string& relative) {
    std::ifstream file(std::string(WILIWILI_SOURCE_DIR) + "/" + relative);
    if (!file) {
        std::cerr << "FAIL: cannot read " << relative << '\n';
        ++failures;
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

json parse(const std::string& relative) {
    try {
        return json::parse(read(relative));
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << relative << " is not valid JSON: " << error.what() << '\n';
        ++failures;
        return json::object();
    }
}

// at(document, "a/b/c") walks objects without throwing, so a missing key is a
// reported failure rather than an abort.
json at(const json& node, const std::string& path) {
    json current = node;
    std::string key;
    std::istringstream stream(path);
    while (std::getline(stream, key, '/')) {
        if (!current.is_object()) return json();
        const auto found = current.find(key);
        if (found == current.end()) return json();
        current = *found;
    }
    return current;
}

bool is_hex(const json& value, size_t length) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    return text.size() == length &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// A pin is a commit, a digest or a version; `latest`, a branch name or an
// unpinned image tag would make a build unreproducible.
void check_no_floating_pins(const json& node, const std::string& path) {
    if (node.is_object()) {
        for (auto entry = node.begin(); entry != node.end(); ++entry) {
            check_no_floating_pins(entry.value(), path + "/" + entry.key());
        }
    } else if (node.is_array()) {
        for (const auto& entry : node) check_no_floating_pins(entry, path);
    } else if (node.is_string()) {
        const std::string& text = node.get_ref<const std::string&>();
        const bool floating = text == "latest" || text == "head" ||
                              (text.size() > 7 && text.compare(text.size() - 7, 7, ":latest") == 0);
        if (floating) {
            std::cerr << "FAIL: floating pin at " << path << ": " << text << '\n';
            ++failures;
        }
    }
}

void check_upstream_pins() {
    const json upstream = parse("UPSTREAM.json");
    CHECK(at(upstream, "schema") == 1);
    CHECK(at(upstream, "upstream/commit") == "88e5876bea9502d06f46a8656e3530684d3aaf7d");
    CHECK(at(upstream, "upstream/version") == "v1.6.0");
    CHECK(at(upstream, "integrated_fork/commit") == "e5dd00d06f4b1ee3e38465a1e1cbd5223002faa6");
    CHECK(at(upstream, "references/openblops_nx/commit") ==
          "302b036471e8c8fea0918366218025418362dc81");

    // Submodule gitlinks are recorded, and borealis is still the upstream
    // subtree the UI framework comes from (ADR 0005).
    const json submodules = at(upstream, "submodules");
    CHECK(submodules.is_object());
    for (auto entry = submodules.begin(); entry != submodules.end(); ++entry) {
        check(is_hex(entry.value(), 40), entry.key().c_str(), __LINE__);
    }
    CHECK(submodules.value("library/borealis", std::string()) ==
          "5f08b286f3df737f3321d2247a6fe633fcead03c");

    // Each of the six dragonfly/dev commits is either integrated or skipped,
    // and the two sets do not overlap.
    const std::set<std::string> dragonfly_commits = {
            "031ddf97d967614aa2086963ed9caff96b65e031",
            "5dccf14651991f81581daed54796b8d03e4741e2",
            "2f583df3edeca2c67d4e48da715cc473d75aa15a",
            "64538394481a74de0981d2d0a5d1730e477e00ee",
            "62e6ec18ffb065779b1b1d60c12b40f1e61b9805",
            "e5dd00d06f4b1ee3e38465a1e1cbd5223002faa6",
    };
    std::set<std::string> accounted;
    for (const char* key : {"integrated", "skipped"}) {
        for (const auto& entry : at(upstream, std::string("integrated_fork/") + key)) {
            const std::string sha = entry.is_object() ? entry.value("commit", std::string())
                                                      : entry.get<std::string>();
            CHECK(is_hex(sha, 40));
            CHECK(accounted.insert(sha).second);
        }
    }
    CHECK(accounted == dragonfly_commits);

    const std::string base_image = at(upstream, "switch2_toolchain/base_image");
    CHECK(base_image == "docker.io/devkitpro/devkita64:20260219"
                        "@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282");
    CHECK(at(upstream, "switch2_toolchain/compiler") == "aarch64-none-elf-g++ (devkitA64) 15.2.0");
    CHECK(at(upstream, "switch2_toolchain/newlib") == "4.6.0.20260123");
    CHECK(at(upstream, "switch2_toolchain/ounce_repository") == "https://pkg.ouncebrew.org");
    CHECK(is_hex(at(upstream, "switch2_toolchain/ounce_signing_key_fingerprint"), 40));

    // The three [ounce] packages ship as one version and must not float.
    for (const char* package : {"libounce", "ouncevk", "libkaihen"}) {
        CHECK(at(upstream, std::string("switch2_toolchain/ounce_packages/") + package) ==
              "20261117.1-1");
    }

    // The software-decoding baseline of ADR 0001, by package and by digest.
    const json portlibs = at(upstream, "switch2_toolchain/portlibs");
    CHECK(at(portlibs, "switch-dav1d/version") == "1.5.5-1");
    CHECK(at(portlibs, "switch-ffmpeg/version") == "7.1.5-5");
    CHECK(at(portlibs, "switch-libmpv/version") == "0.36.0-2");
    for (auto entry = portlibs.begin(); entry != portlibs.end(); ++entry) {
        check(is_hex(at(entry.value(), "sha256"), 64), entry.key().c_str(), __LINE__);
    }

    check_no_floating_pins(upstream, "UPSTREAM.json");

    // The container installs exactly what UPSTREAM.json records.
    const std::string dockerfile = read("scripts/switch2/Dockerfile");
    CHECK(dockerfile.find(base_image) != std::string::npos);
    CHECK(dockerfile.find(at(upstream, "switch2_toolchain/ounce_signing_key_fingerprint")
                                  .get<std::string>()) != std::string::npos);
    for (auto entry = portlibs.begin(); entry != portlibs.end(); ++entry) {
        CHECK(dockerfile.find(entry.key()) != std::string::npos);
        CHECK(dockerfile.find(at(entry.value(), "sha256").get<std::string>()) != std::string::npos);
    }
}

json find_named(const json& list, const std::string& name) {
    if (!list.is_array()) return json();
    for (const auto& entry : list) {
        if (at(entry, "name") == name) return entry;
    }
    return json();
}

// The presets share one hidden "base" preset, so resolve a configure preset
// against its parent before checking the effective values.
json resolve_configure(const json& configure_presets, const std::string& name) {
    json preset = find_named(configure_presets, name);
    const std::string parent = preset.value("inherits", std::string());
    if (preset.is_null() || parent.empty()) return preset;

    json merged = find_named(configure_presets, parent);
    for (const auto& entry : preset.items()) {
        if (entry.key() == "cacheVariables" && merged.contains("cacheVariables")) {
            for (const auto& variable : entry.value().items()) {
                merged["cacheVariables"][variable.key()] = variable.value();
            }
        } else {
            merged[entry.key()] = entry.value();
        }
    }
    return merged;
}

void check_presets() {
    const json presets = parse("CMakePresets.json");
    CHECK(at(presets, "version") >= 3);

    struct ConfigureCase {
        const char* name;
        const char* platform;
    };
    const ConfigureCase configure_cases[] = {{"desktop-debug", "PLATFORM_DESKTOP"},
                                             {"desktop-release", "PLATFORM_DESKTOP"},
                                             {"switch2-release", "PLATFORM_SWITCH"}};
    for (const auto& expected : configure_cases) {
        const json preset = resolve_configure(at(presets, "configurePresets"), expected.name);
        if (preset.is_null()) {
            std::cerr << "FAIL: missing configure preset " << expected.name << '\n';
            ++failures;
            continue;
        }
        CHECK(at(preset, "generator") == "Ninja");
        CHECK(at(preset, "binaryDir").get<std::string>().rfind("${sourceDir}/build/", 0) == 0);
        CHECK(at(preset, std::string("cacheVariables/") + expected.platform) == "ON");
        CHECK(at(preset, "cacheVariables/BRLS_UNITY_BUILD") == "OFF");
    }

    const json switch2 = resolve_configure(at(presets, "configurePresets"), "switch2-release");
    if (!switch2.is_null()) {
        const std::string cache = "cacheVariables/";
        CHECK(at(switch2, cache + "PLATFORM_SWITCH2") == "ON");
        // Switch 1 pieces this port does not use.
        CHECK(at(switch2, cache + "BUILTIN_NSP") == "OFF");
        CHECK(at(switch2, cache + "USE_DEKO3D") == "OFF");
        // SDL2 + EGL on top of OunceVK.
        CHECK(at(switch2, cache + "USE_SDL2") == "ON");
        CHECK(at(switch2, cache + "USE_EGL") == "ON");
        CHECK(at(switch2, cache + "USE_LIBROMFS") == "ON");
        CHECK(at(switch2, cache + "CMAKE_BUILD_TYPE") == "Release");
        CHECK(at(switch2, "toolchainFile").get<std::string>().find(
                      "libounce/cmake/Switch2.cmake") != std::string::npos);
        // Release flags stay safe, and -g keeps the unstripped .elf usable.
        const json release_flags_json = at(switch2, cache + "CMAKE_CXX_FLAGS_RELEASE");
        const std::string release_flags =
                release_flags_json.is_string() ? release_flags_json.get<std::string>() : "";
        CHECK(release_flags.find("-g") != std::string::npos);
        CHECK(release_flags.find("ffast-math") == std::string::npos);
    }

    const std::string text = read("CMakePresets.json");
    CHECK(text.find("ffast-math") == std::string::npos);

    for (const char* name : {"desktop-debug", "desktop-release", "switch2-release"}) {
        CHECK(!find_named(at(presets, "buildPresets"), name).is_null());
    }
    const json test_preset = find_named(at(presets, "testPresets"), "desktop-release");
    if (test_preset.is_null()) {
        std::cerr << "FAIL: missing test preset desktop-release\n";
        ++failures;
    } else {
        CHECK(at(test_preset, "output/outputOnFailure") == true);
    }
}

void check_switch2_wiring() {
    const std::string cmake = read("CMakeLists.txt");
    CHECK(cmake.find("option(PLATFORM_SWITCH2") != std::string::npos);
    CHECK(cmake.find("option(WILIWILI_BUILD_TESTS") != std::string::npos);
    CHECK(cmake.find("cmake/Switch2Platform.cmake") != std::string::npos);

    const std::string platform = read("cmake/Switch2Platform.cmake");
    // NX_ROOT and the find prefixes point at the [ounce] devkit, and the host
    // libromfs-generator is handed to borealis instead of being cross-compiled.
    CHECK(platform.find("libounce") != std::string::npos);
    CHECK(platform.find("portlibs/switch2") != std::string::npos);
    CHECK(platform.find("__dkp_platform_prefix") != std::string::npos);
    CHECK(platform.find("LIBROMFS_PREBUILT_GENERATOR") != std::string::npos);
}

}  // namespace

int main() {
    check_upstream_pins();
    check_presets();
    check_switch2_wiring();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all configuration checks passed\n";
    return 0;
}
