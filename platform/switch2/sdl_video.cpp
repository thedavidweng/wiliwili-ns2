/*
 * Borealis' SDL video backend, for Switch 2.
 *
 * Replaces library/borealis/library/lib/platforms/sdl/sdl_video.cpp when
 * PLATFORM_SWITCH2=ON (see library/CMakeLists.txt). The Borealis submodule stays
 * at its pinned commit; the Switch 2 changes live here.
 *
 * Differences from the upstream file:
 * - The `#ifdef __SWITCH__` block that asks SDL for a desktop OpenGL 4.3 core
 *   context no longer applies here: Switch 2 asks for GLES 2.0, which is what
 *   OunceVK's EGL provides (GLES on Zink) and what nanovg is asked to draw with.
 * - SDL_Init also starts the timer subsystem, and every SDL/GL call that can
 *   fail is checked and reported instead of leaving a black screen.
 * - The window opens at the console's own handheld resolution, so the
 *   framebuffer is 1:1 with the panel and Nanovg scales the 1280x720 layout up.
 *   The size, the viewport and the device pixel ratio all come from
 *   switch2_display.cpp, which is also what follows a dock/undock; this file
 *   only owns the window and the GL context.
 * - The driver capabilities (GL strings, GLES extensions and the NVK Vulkan
 *   device underneath Zink) are written to boot.log.
 * - SDL events are not watched here: SwitchPlatform pumps the queue every frame,
 *   because nothing else on this platform drains it.
 */

#include <fmt/format.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/util.hpp>
#include <borealis/platforms/sdl/sdl_video.hpp>
#include <glad/glad.h>

#define NANOVG_GLES2_IMPLEMENTATION
#include <nanovg_gl.h>

#include "utils/boot_api.h"
#include "utils/boot_diagnostics.hpp"
#include "switch2_display.hpp"

namespace {

const char* glString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "(unavailable)";
}

// SDL reports its failure through SDL_GetError(), and a black screen is the one
// thing this port must not produce: the reason goes to the log and to boot.log,
// then the boot stops.
[[noreturn]] void failSdl(const std::string& what) {
    const std::string error = fmt::format("{}: {}", what, SDL_GetError());
    brls::Logger::error("sdl: {}", error);
    wiliwili_boot_note("sdl", error.c_str());
    brls::fatal(error);
}

std::string apiVersionString(uint32_t version) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

// glGetString(GL_EXTENSIONS) is one space separated line; "(unavailable)" is
// what glString() returns when there is no context to ask.
std::vector<std::string> splitExtensions(const char* list) {
    std::vector<std::string> extensions;
    if (!list || !list[0] || list[0] == '(') return extensions;
    for (const char* cursor = list; *cursor;) {
        const char* end = cursor;
        while (*end && *end != ' ') ++end;
        extensions.emplace_back(cursor, end);
        cursor = *end ? end + 1 : end;
    }
    return extensions;
}

std::vector<std::string> instanceExtensionNames() {
    std::vector<std::string> names;
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || !count) return names;
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) return names;
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) names.emplace_back(properties[i].extensionName);
    return names;
}

std::vector<std::string> deviceExtensionNames(VkPhysicalDevice device) {
    std::vector<std::string> names;
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || !count) return names;
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()) != VK_SUCCESS) return names;
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) names.emplace_back(properties[i].extensionName);
    return names;
}

// One boot.log line per report line, and the same text to the regular log.
void writeReport(const char* stage, const std::string& report) {
    size_t start = 0;
    while (start < report.size()) {
        const size_t end       = report.find('\n', start);
        const std::string line = report.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            brls::Logger::info("switch2: {}", line);
            wiliwili_boot_note(stage, line.c_str());
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

// Zink is a Vulkan driver as much as a GL one: the device it runs on is what
// tells us whether OunceVK got NVK, and the extensions it offers are what says
// whether the console's own display surface (VK_NN_vi_surface) and the GPU
// memory sharing behind it are there. Purely informational, never fatal.
void reportVulkanDevice() {
    wili::GpuDiagnostics vulkan;
    vulkan.api = "Vulkan (OunceVK)";

    // Enumerating instance extensions needs no instance, so this still reports
    // when creating one fails.
    const std::vector<std::string> instanceExtensions = instanceExtensionNames();
    std::vector<std::string> deviceExtensions;

    uint32_t instanceVersion = 0;
    if (vkEnumerateInstanceVersion(&instanceVersion) == VK_SUCCESS)
        vulkan.fields.push_back({"vkInstanceVersion", apiVersionString(instanceVersion)});

    VkApplicationInfo application  = {};
    application.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName   = "wiliwili-NS2";
    application.applicationVersion = VK_MAKE_VERSION(1, 6, 0);
    application.pEngineName        = "borealis";
    application.apiVersion         = instanceVersion ? instanceVersion : VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo     = &application;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        vulkan.fields.push_back({"vkInstance", "not available"});
    } else {
        uint32_t deviceCount = 0;
        if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0) {
            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) == VK_SUCCESS && deviceCount > 0) {
                VkPhysicalDeviceProperties properties = {};
                vkGetPhysicalDeviceProperties(devices[0], &properties);
                vulkan.fields.push_back({"vkDeviceName", properties.deviceName});
                vulkan.fields.push_back({"vkDeviceType", fmt::format("{}", static_cast<int>(properties.deviceType))});
                vulkan.fields.push_back({"vkDriverVersion", apiVersionString(properties.driverVersion)});
                vulkan.fields.push_back({"vkApiVersion", apiVersionString(properties.apiVersion)});
                vulkan.fields.push_back({"vkDeviceCount", fmt::format("{}", deviceCount)});
                deviceExtensions = deviceExtensionNames(devices[0]);
            }
        } else {
            vulkan.fields.push_back({"vkPhysicalDevices", "none"});
        }
        vkDestroyInstance(instance, nullptr);
    }

    std::string report = wili::formatGpuDiagnostics(vulkan);
    report += wili::formatExtensionList("vkInstanceExtensions", instanceExtensions);
    if (!deviceExtensions.empty()) report += wili::formatExtensionList("vkDeviceExtensions", deviceExtensions);
    writeReport("gpu", report);
}

void reportGraphicsCapabilities() {
    wili::GpuDiagnostics gl;
    gl.api    = "OpenGL ES (Zink)";
    gl.fields = {
        {"GL_VENDOR", glString(GL_VENDOR)},
        {"GL_RENDERER", glString(GL_RENDERER)},
        {"GL_VERSION", glString(GL_VERSION)},
        {"GL_SHADING_LANGUAGE_VERSION", glString(GL_SHADING_LANGUAGE_VERSION)},
    };

    std::string report = wili::formatGpuDiagnostics(gl);
    report += wili::formatExtensionList("GL_EXTENSIONS", splitExtensions(glString(GL_EXTENSIONS)));
    writeReport("gpu", report);

    reportVulkanDevice();
}

}  // namespace

namespace brls {

SDLVideoContext::SDLVideoContext(std::string windowTitle, uint32_t windowWidth, uint32_t windowHeight, float, float) {
    const wili::Framebuffer preferred = wili::preferredWindowSize(windowWidth, windowHeight);
    windowWidth                       = preferred.width;
    windowHeight                      = preferred.height;
    brls::Logger::info("switch2: requesting a {}x{} window", windowWidth, windowHeight);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) failSdl("SDL_Init failed");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    this->window = SDL_CreateWindow(windowTitle.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, windowWidth,
                                    windowHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);
    if (!this->window) failSdl("SDL_CreateWindow failed");
    brls::Logger::info("switch2: SDL video driver: {}", SDL_GetCurrentVideoDriver());

    SDL_GLContext context = SDL_GL_CreateContext(this->window);
    if (!context) failSdl("SDL_GL_CreateContext failed");
    if (SDL_GL_MakeCurrent(this->window, context) < 0) failSdl("SDL_GL_MakeCurrent failed");
    wiliwili_boot_note("sdl", "GLES context created");

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        wiliwili_boot_note("sdl", "gladLoadGLLoader failed");
        fatal("gladLoadGLLoader failed");
    }

    reportGraphicsCapabilities();

    this->nvgContext = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
    if (!this->nvgContext) {
        wiliwili_boot_note("sdl", "nvgCreateGLES2 failed");
        fatal("sdl: unable to init nanovg");
    }

    setSwapInterval(VideoContext::swapInterval);

    // From here the display module owns the framebuffer: it opens the display
    // change event, asks applet and vi2 what the console drives, sets the window
    // size and the GL viewport, and reports the result in boot.log. Dock and
    // undock are handled by the same code, from the platform's main loop.
    wili::displayInitialize(this->window);

    const wili::Framebuffer framebuffer = wili::displayFramebuffer();
    Application::setWindowSize(framebuffer.width, framebuffer.height);

    int width, height;
    SDL_GetWindowSize(this->window, &width, &height);

    int xPos, yPos;
    SDL_GetWindowPosition(this->window, &xPos, &yPos);
    Application::setWindowPosition(xPos, yPos);

    if (!VideoContext::FULLSCREEN) {
        VideoContext::sizeW = width;
        VideoContext::sizeH = height;
        VideoContext::posX  = static_cast<float>(xPos);
        VideoContext::posY  = static_cast<float>(yPos);
    }
}

void SDLVideoContext::beginFrame() {}

void SDLVideoContext::endFrame() { SDL_GL_SwapWindow(this->window); }

void SDLVideoContext::setSwapInterval(int interval) {
    VideoContext::swapInterval = interval;
    if (SDL_GL_SetSwapInterval(interval) < 0)
        brls::Logger::warning("switch2: SDL_GL_SetSwapInterval({}) failed: {}", interval, SDL_GetError());
    // -1 means "adaptive vsync was requested and is not supported". The loop
    // presents once per iteration, so what it actually got is the panel rate.
    brls::Logger::info("switch2: swap interval {} (effective {})", interval, SDL_GL_GetSwapInterval());
}

void SDLVideoContext::clear(NVGcolor color) {
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void SDLVideoContext::resetState() {
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
}

double SDLVideoContext::getScaleFactor() { return wili::displayScaleFactor(); }

SDLVideoContext::~SDLVideoContext() {
    if (this->nvgContext) nvgDeleteGLES2(this->nvgContext);
    SDL_DestroyWindow(this->window);
    SDL_Quit();
}

NVGcontext* SDLVideoContext::getNVGContext() { return this->nvgContext; }

SDL_Window* SDLVideoContext::getSDLWindow() { return this->window; }

void SDLVideoContext::fullScreen(bool fs) {
    SDL_SetWindowFullscreen(this->window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

}  // namespace brls
