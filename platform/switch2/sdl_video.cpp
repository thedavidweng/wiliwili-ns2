/*
 * Borealis' SDL video backend, for Switch 2.
 *
 * Replaces library/borealis/library/lib/platforms/sdl/sdl_video.cpp when
 * PLATFORM_SWITCH2=ON (see library/CMakeLists.txt). The Borealis submodule stays
 * at its pinned commit; the Switch 2 changes live here.
 *
 * Differences from the upstream file:
 * - The `#ifdef __SWITCH__` block that asks SDL for a desktop OpenGL 4.3 core
 *   context no longer applies here: Switch 2 asks for OpenGL ES 3.1, the
 *   version OunceVK's EGL documents for Zink, and nanovg draws with its GLES3
 *   backend. USE_GLES2 still builds the GLES 2.0 context for the one-off
 *   GLES2/GLES3 comparison on hardware; the preset only builds GLES3.
 * - SDL_Init also starts the timer subsystem, and every SDL/GL call that can
 *   fail is checked and reported instead of leaving a black screen.
 * - The window opens at the console's own handheld resolution, so the
 *   framebuffer is 1:1 with the panel and Nanovg scales the 1280x720 layout up.
 *   The size, the viewport and the device pixel ratio all come from
 *   switch2_display.cpp, which is also what follows a dock/undock; this file
 *   only owns the window and the GL context.
 * - The driver capabilities (GL strings, the context SDL and EGL actually
 *   created, GLES and EGL extensions, and the NVK Vulkan device underneath
 *   Zink) are written to boot.log.
 * - SDL events are not watched here: SwitchPlatform pumps the queue every frame,
 *   because nothing else on this platform drains it.
 */

#include <EGL/egl.h>
#include <fmt/format.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/util.hpp>
#include <borealis/platforms/sdl/sdl_video.hpp>
#include <glad/glad.h>

#if defined(USE_GLES2)
#define NANOVG_GLES2_IMPLEMENTATION
#elif defined(USE_GLES3)
#define NANOVG_GLES3_IMPLEMENTATION
#define NANOVG_GL_USE_UNIFORMBUFFER 1
#else
#error "Switch 2 renders with OpenGL ES: configure with USE_GLES3 (or USE_GLES2 for the comparison build)"
#endif
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

const char* eglString(EGLDisplay display, EGLint name) {
    const char* value = display != EGL_NO_DISPLAY ? eglQueryString(display, name) : nullptr;
    return value ? value : "(unavailable)";
}

std::string sdlAttribute(SDL_GLattr attribute) {
    int value = 0;
    return SDL_GL_GetAttribute(attribute, &value) == 0 ? fmt::format("{}", value) : "(unavailable)";
}

// GL_MAJOR_VERSION and friends only exist from GLES 3.0 on; a GLES 2.0 context
// answers GL_INVALID_ENUM, which is cleared so it does not surface in nanovg.
std::string glInteger(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    if (glGetError() != GL_NO_ERROR) return "(unavailable)";
    return fmt::format("{}", value);
}

std::string eglContextAttribute(EGLDisplay display, EGLContext context, EGLint name) {
    EGLint value = 0;
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT || !eglQueryContext(display, context, name, &value))
        return "(unavailable)";
    return fmt::format("{}", value);
}

std::string eglConfigAttribute(EGLDisplay display, EGLConfig config, EGLint name) {
    EGLint value = 0;
    if (!config || !eglGetConfigAttrib(display, config, name, &value)) return "(unavailable)";
    return fmt::format("{}", value);
}

// What was actually created, as opposed to what was asked for: SDL's view of
// the context, EGL's view of the context and its config, and the version GL
// itself reports.
void reportContext() {
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();

    EGLConfig config = nullptr;
    EGLint configId = 0, count = 0;
    if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT &&
        eglQueryContext(display, context, EGL_CONFIG_ID, &configId)) {
        const EGLint attributes[] = {EGL_CONFIG_ID, configId, EGL_NONE};
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) config = nullptr;
    }

    wili::GpuDiagnostics egl;
    egl.api    = "EGL (OunceVK)";
    egl.fields = {
        {"EGL_VENDOR", eglString(display, EGL_VENDOR)},
        {"EGL_VERSION", eglString(display, EGL_VERSION)},
        {"EGL_CLIENT_APIS", eglString(display, EGL_CLIENT_APIS)},
        {"EGL_CONTEXT_CLIENT_TYPE", eglContextAttribute(display, context, EGL_CONTEXT_CLIENT_TYPE)},
        {"EGL_CONTEXT_CLIENT_VERSION", eglContextAttribute(display, context, EGL_CONTEXT_CLIENT_VERSION)},
        {"EGL_CONFIG_ID", eglContextAttribute(display, context, EGL_CONFIG_ID)},
        {"EGL_RENDER_BUFFER", eglContextAttribute(display, context, EGL_RENDER_BUFFER)},
        {"EGL_RENDERABLE_TYPE", eglConfigAttribute(display, config, EGL_RENDERABLE_TYPE)},
        {"EGL_RGBA_SIZE", fmt::format("{}/{}/{}/{}", eglConfigAttribute(display, config, EGL_RED_SIZE),
                                      eglConfigAttribute(display, config, EGL_GREEN_SIZE),
                                      eglConfigAttribute(display, config, EGL_BLUE_SIZE),
                                      eglConfigAttribute(display, config, EGL_ALPHA_SIZE))},
        {"EGL_DEPTH_STENCIL_SIZE", fmt::format("{}/{}", eglConfigAttribute(display, config, EGL_DEPTH_SIZE),
                                               eglConfigAttribute(display, config, EGL_STENCIL_SIZE))},
        {"EGL_SAMPLES", eglConfigAttribute(display, config, EGL_SAMPLES)},
        {"SDL_GL_CONTEXT_VERSION",
         fmt::format("{}.{}", sdlAttribute(SDL_GL_CONTEXT_MAJOR_VERSION), sdlAttribute(SDL_GL_CONTEXT_MINOR_VERSION))},
        {"SDL_GL_CONTEXT_PROFILE_MASK", sdlAttribute(SDL_GL_CONTEXT_PROFILE_MASK)},
        {"GL_MAJOR_MINOR_VERSION", fmt::format("{}.{}", glInteger(GL_MAJOR_VERSION), glInteger(GL_MINOR_VERSION))},
    };

    std::string report = wili::formatGpuDiagnostics(egl);
    report += wili::formatExtensionList("EGL_EXTENSIONS", splitExtensions(eglString(display, EGL_EXTENSIONS)));
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
        {"GL_MAX_TEXTURE_SIZE", glInteger(GL_MAX_TEXTURE_SIZE)},
        {"GL_MAX_SAMPLES", glInteger(GL_MAX_SAMPLES)},
        {"GL_MAX_UNIFORM_BLOCK_SIZE", glInteger(GL_MAX_UNIFORM_BLOCK_SIZE)},
    };

    std::string report = wili::formatGpuDiagnostics(gl);
    report += wili::formatExtensionList("GL_EXTENSIONS", splitExtensions(glString(GL_EXTENSIONS)));
    writeReport("gpu", report);

    reportContext();

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

#ifdef USE_GLES2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // A 3.1 request makes SDL pick an EGL_OPENGL_ES3_BIT config and ask for at
    // least 3.1; Mesa then grants the highest ES version Zink supports.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
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

#ifdef USE_GLES2
    this->nvgContext = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#else
    this->nvgContext = nvgCreateGLES3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
#endif
    if (!this->nvgContext) {
        wiliwili_boot_note("sdl", "nanovg context creation failed");
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
#ifdef USE_GLES2
    if (this->nvgContext) nvgDeleteGLES2(this->nvgContext);
#else
    if (this->nvgContext) nvgDeleteGLES3(this->nvgContext);
#endif
    SDL_DestroyWindow(this->window);
    SDL_Quit();
}

NVGcontext* SDLVideoContext::getNVGContext() { return this->nvgContext; }

SDL_Window* SDLVideoContext::getSDLWindow() { return this->window; }

void SDLVideoContext::fullScreen(bool fs) {
    SDL_SetWindowFullscreen(this->window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

}  // namespace brls
