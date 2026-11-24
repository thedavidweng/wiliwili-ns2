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
 * - The window asks for the console's own handheld resolution, so the
 *   framebuffer is 1:1 with the panel and Nanovg scales the 1280x720 layout up.
 * - The driver capabilities (GL strings, GLES extensions and the NVK Vulkan
 *   device underneath Zink) are written to boot.log.
 */

#include <switch.h>

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

namespace
{

double scaleFactor = 1.0;

const char* glString(GLenum name)
{
    const GLubyte* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "(unavailable)";
}

std::string apiVersionString(uint32_t version)
{
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
        VK_API_VERSION_PATCH(version));
}

// One boot.log line per report line, and the same text to the regular log.
void writeReport(const char* stage, const std::string& report)
{
    size_t start = 0;
    while (start < report.size())
    {
        const size_t end       = report.find('\n', start);
        const std::string line = report.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty())
        {
            brls::Logger::info("switch2: {}", line);
            wiliwili_boot_note(stage, line.c_str());
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

// Zink is a Vulkan driver as much as a GL one: the device it runs on is what
// tells us whether OunceVK got NVK. Purely informational, never fatal.
void reportVulkanDevice()
{
    wili::GpuDiagnostics vulkan;
    vulkan.api = "Vulkan (OunceVK)";

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
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
    {
        vulkan.fields.push_back({"vkInstance", "not available"});
        writeReport("gpu", wili::formatGpuDiagnostics(vulkan));
        return;
    }

    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0)
    {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) == VK_SUCCESS)
        {
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(devices[0], &properties);
            vulkan.fields.push_back({"vkDeviceName", properties.deviceName});
            vulkan.fields.push_back({"vkDeviceType", fmt::format("{}", static_cast<int>(properties.deviceType))});
            vulkan.fields.push_back({"vkDriverVersion", apiVersionString(properties.driverVersion)});
            vulkan.fields.push_back({"vkApiVersion", apiVersionString(properties.apiVersion)});
            vulkan.fields.push_back({"vkDeviceCount", fmt::format("{}", deviceCount)});
        }
    }
    else
    {
        vulkan.fields.push_back({"vkPhysicalDevices", "none"});
    }
    vkDestroyInstance(instance, nullptr);

    writeReport("gpu", wili::formatGpuDiagnostics(vulkan));
}

void reportGraphicsCapabilities()
{
    wili::GpuDiagnostics gl;
    gl.api = "OpenGL ES (Zink)";

    const char* extensions = glString(GL_EXTENSIONS);
    size_t extensionCount  = 0;
    if (extensions[0] && extensions[0] != '(')
    {
        extensionCount = 1;
        for (const char* cursor = extensions; *cursor; ++cursor)
            if (*cursor == ' ') ++extensionCount;
    }

    gl.fields = {
        {"GL_VENDOR", glString(GL_VENDOR)},
        {"GL_RENDERER", glString(GL_RENDERER)},
        {"GL_VERSION", glString(GL_VERSION)},
        {"GL_SHADING_LANGUAGE_VERSION", glString(GL_SHADING_LANGUAGE_VERSION)},
        {"GL_EXTENSION_COUNT", fmt::format("{}", extensionCount)},
        {"GL_EXTENSIONS", extensions},
    };
    writeReport("gpu", wili::formatGpuDiagnostics(gl));

    reportVulkanDevice();
}

// The NRO draws to the console's own surface. Handheld is 1920x1080; docked may
// report more, but the software decode baseline has no room for a 4K
// framebuffer, so 1080p is the cap.
void nativeWindowSize(uint32_t& width, uint32_t& height)
{
    s32 displayWidth = 0, displayHeight = 0;
    const Result rc  = appletGetDefaultDisplayResolution(&displayWidth, &displayHeight);
    if (R_SUCCEEDED(rc) && displayWidth > 0 && displayHeight > 0)
    {
        width  = static_cast<uint32_t>(displayWidth);
        height = static_cast<uint32_t>(displayHeight);
    }
    if (width > 1920)
    {
        height = height * 1920 / width;
        width  = 1920;
    }
    if (height > 1080)
    {
        width  = width * 1080 / height;
        height = 1080;
    }
}

void onWindowResized(SDL_Window* window)
{
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    int fWidth, fHeight;
    SDL_GL_GetDrawableSize(window, &fWidth, &fHeight);
    if (!fWidth || !fHeight) return;

    scaleFactor = width > 0 ? fWidth * 1.0 / width : 1.0;
    glViewport(0, 0, fWidth, fHeight);
    brls::Application::onWindowResized(fWidth, fHeight);
}

int sdlWindowEventWatcher(void* data, SDL_Event* event)
{
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_RESIZED)
    {
        SDL_Window* window = SDL_GetWindowFromID(event->window.windowID);
        if (window == static_cast<SDL_Window*>(data)) onWindowResized(window);
    }
    return 0;
}

} // namespace

namespace brls
{

SDLVideoContext::SDLVideoContext(std::string windowTitle, uint32_t windowWidth, uint32_t windowHeight, float, float)
{
    nativeWindowSize(windowWidth, windowHeight);
    brls::Logger::info("switch2: requesting a {}x{} window", windowWidth, windowHeight);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
    {
        const std::string error = fmt::format("SDL_Init failed: {}", SDL_GetError());
        brls::Logger::error("sdl: {}", error);
        wiliwili_boot_note("sdl", error.c_str());
        fatal(error);
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    this->window = SDL_CreateWindow(windowTitle.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        windowWidth, windowHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);
    if (!this->window)
    {
        const std::string error = fmt::format("SDL_CreateWindow failed: {}", SDL_GetError());
        brls::Logger::error("sdl: {}", error);
        wiliwili_boot_note("sdl", error.c_str());
        fatal(error);
    }
    brls::Logger::info("switch2: SDL video driver: {}", SDL_GetCurrentVideoDriver());

    SDL_GLContext context = SDL_GL_CreateContext(this->window);
    if (!context)
    {
        const std::string error = fmt::format("SDL_GL_CreateContext failed: {}", SDL_GetError());
        brls::Logger::error("sdl: {}", error);
        wiliwili_boot_note("sdl", error.c_str());
        fatal(error);
    }
    if (SDL_GL_MakeCurrent(this->window, context) < 0)
    {
        const std::string error = fmt::format("SDL_GL_MakeCurrent failed: {}", SDL_GetError());
        brls::Logger::error("sdl: {}", error);
        wiliwili_boot_note("sdl", error.c_str());
        fatal(error);
    }
    wiliwili_boot_note("sdl", "GLES context created");

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        wiliwili_boot_note("sdl", "gladLoadGLLoader failed");
        fatal("gladLoadGLLoader failed");
    }

    reportGraphicsCapabilities();

    this->nvgContext = nvgCreateGLES2(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
    if (!this->nvgContext)
    {
        wiliwili_boot_note("sdl", "nvgCreateGLES2 failed");
        fatal("sdl: unable to init nanovg");
    }

    setSwapInterval(VideoContext::swapInterval);
    SDL_AddEventWatch(sdlWindowEventWatcher, this->window);

    int width, height;
    SDL_GetWindowSize(this->window, &width, &height);

    int fWidth, fHeight;
    SDL_GL_GetDrawableSize(this->window, &fWidth, &fHeight);
    scaleFactor = width > 0 ? fWidth * 1.0 / width : 1.0;
    Application::setWindowSize(fWidth, fHeight);
    glViewport(0, 0, fWidth, fHeight);

    int xPos, yPos;
    SDL_GetWindowPosition(this->window, &xPos, &yPos);
    Application::setWindowPosition(xPos, yPos);

    if (!VideoContext::FULLSCREEN)
    {
        VideoContext::sizeW = width;
        VideoContext::sizeH = height;
        VideoContext::posX  = static_cast<float>(xPos);
        VideoContext::posY  = static_cast<float>(yPos);
    }
}

void SDLVideoContext::beginFrame() {}

void SDLVideoContext::endFrame()
{
    SDL_GL_SwapWindow(this->window);
}

void SDLVideoContext::setSwapInterval(int interval)
{
    VideoContext::swapInterval = interval;
    SDL_GL_SetSwapInterval(interval);
}

void SDLVideoContext::clear(NVGcolor color)
{
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void SDLVideoContext::resetState()
{
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
}

double SDLVideoContext::getScaleFactor()
{
    return scaleFactor;
}

SDLVideoContext::~SDLVideoContext()
{
    if (this->nvgContext) nvgDeleteGLES2(this->nvgContext);
    SDL_DestroyWindow(this->window);
    SDL_Quit();
}

NVGcontext* SDLVideoContext::getNVGContext()
{
    return this->nvgContext;
}

SDL_Window* SDLVideoContext::getSDLWindow()
{
    return this->window;
}

void SDLVideoContext::fullScreen(bool fs)
{
    SDL_SetWindowFullscreen(this->window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

} // namespace brls
