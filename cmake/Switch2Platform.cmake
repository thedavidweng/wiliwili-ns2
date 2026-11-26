# Switch 2 (libounce + OunceVK) platform integration.
#
# Included by the top level CMakeLists.txt right after Borealis'
# cmake/toolchain.cmake, and only active with -DPLATFORM_SWITCH2=ON.
#
# The [ounce] devkit's CMake toolchain
# ($DEVKITPRO/libounce/cmake/Switch2.cmake, the CMAKE_TOOLCHAIN_FILE of the
# switch2 presets) has already run by then, but Borealis' PLATFORM_SWITCH
# branch then re-configures the Switch 1 devkit on top of it: NX_ROOT points at
# $DEVKITPRO/libnx, the Switch 1 portlibs go in front of the Switch 2 ones, and
# CMAKE_C_FLAGS/CMAKE_CXX_FLAGS become the libnx include paths. This file puts
# the Switch 2 devkit back before project() loads the platform file, so every
# later -l and find_package resolves against libounce and OunceVK.

if (NOT PLATFORM_SWITCH2)
    return()
endif ()

if (NOT CMAKE_TOOLCHAIN_FILE MATCHES "libounce/cmake/Switch2\\.cmake$")
    message(FATAL_ERROR
            "PLATFORM_SWITCH2 needs the [ounce] CMake toolchain.\n"
            "Build with: cmake --preset switch2-release\n"
            "or pass: -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake")
endif ()

if (NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "Please set DEVKITPRO in your environment.")
endif ()

set(NX_ROOT "$ENV{DEVKITPRO}/libounce")
set(OUNCE_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch2")
set(DKP_INSTALL_PREFIX_INIT "${OUNCE_PORTLIBS}")

# libounce and the Switch 2 portlibs first, the Switch 1 portlibs after them:
# libs that only use the C library (SDL2, webp, dav1d, curl, ass, ...) still
# come from the Switch 1 portlibs, while FFmpeg, libmpv, EGL, GLES, Vulkan,
# glapi and drm_nouveau resolve to $DEVKITPRO/portlibs/switch2.
__dkp_platform_prefix("${OUNCE_PORTLIBS}" "${NX_ROOT}")

# Borealis' Switch 1 branch runs Switch.cmake before project() loads
# Switch2.cmake, caching Switch 1's pkg-config (whose PKG_CONFIG_LIBDIR skips
# portlibs/switch2/) and libnx's default icon. Force both back to [ounce].
if (NOT PKG_CONFIG_EXECUTABLE STREQUAL "${OUNCE_PORTLIBS}/bin/aarch64-none-elf-pkg-config")
    unset(PKG_CONFIG_EXECUTABLE CACHE)
    find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config
            HINTS "${OUNCE_PORTLIBS}/bin" NO_DEFAULT_PATH)
    if (NOT PKG_CONFIG_EXECUTABLE)
        message(FATAL_ERROR "Could not find ${OUNCE_PORTLIBS}/bin/aarch64-none-elf-pkg-config")
    endif ()
endif ()
unset(NX_DEFAULT_ICON CACHE)
find_file(NX_DEFAULT_ICON NAMES default_icon.jpg HINTS "${NX_ROOT}" NO_CMAKE_FIND_ROOT_PATH)

# Borealis' Switch 1 branch replaced these with the libnx include paths; drop
# the override so the -D__SWITCH2__ and Cortex-A78C flags the toolchain put in
# the cache apply again.
unset(CMAKE_C_FLAGS)
unset(CMAKE_CXX_FLAGS)

# A link map makes it checkable that no Switch 1 archive (libnx, the libnx GPU
# libraries) ends up in the NRO. Wrapping cpu_features_get_time_usec replaces
# Borealis' hardcoded 19.2 MHz Switch 1 tick conversion with cntpct_el0/cntfrq_el0
# (31.25 MHz on Switch 2) without modifying the pinned submodule.
list(APPEND APP_PLATFORM_LINK_OPTION
        "-Wl,-Map=${CMAKE_BINARY_DIR}/wiliwili.map"
        "-Wl,--wrap=cpu_features_get_time_usec")

# This port ships without third-party telemetry: analytics.h then compiles
# without an endpoint, an identifier or a key in the binary.
set(ANALYTICS OFF CACHE BOOL "Using Google Analytics" FORCE)
list(APPEND APP_PLATFORM_OPTION -DNO_GA)
