/*
 * <SDL2/SDL.h> for the Switch 2 video backend.
 *
 * devkitPro's SDL2 package puts .../portlibs/switch/include/SDL2 on the include
 * path, not its parent, and adding the parent would let the Switch 1 portlibs'
 * EGL, GLES and Vulkan headers shadow OunceVK's. This shim is the only file the
 * backend needs from that layout.
 */

#pragma once

#include <SDL.h>
