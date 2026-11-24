// Device side of the Switch 2 pointer: the PadPointer/Nmouse calls and the
// standard USB/Bluetooth mouse that share the one Borealis cursor. Only
// switch2_pointer.cpp includes the headers declared here.

#pragma once

#include <switch.h>

#include "switch2_pointer.hpp"

namespace wili::switch2 {

// One frame of raw pointer data. The two sources are reported separately so the
// input manager can keep a single logical cursor instead of letting one of them
// teleport it.
struct RawPointerSample {
    // Joy-Con 2 pointer (PadPointer), absolute position in the pointer area.
    struct Pad {
        bool present        = false;
        PointerSide side    = PointerSide::None;
        uint64_t buttons    = 0;  // the pad's buttons this frame, for the claim latch
        uint64_t claim_mask = 0;  // L|ZL or R|ZR while the pointer reports a click
        int x               = 0;
        int y               = 0;
        int delta_x         = 0;
        int delta_y         = 0;
        int wheel_y         = 0;
        bool left           = false;
        bool middle         = false;
        bool right          = false;
    } pad;

    // Standard USB/Bluetooth mouse: relative movement only, its absolute
    // coordinates have no relation to the framebuffer.
    struct Mouse {
        bool present = false;
        int delta_x  = 0;
        int delta_y  = 0;
        int wheel_x  = 0;
        int wheel_y  = 0;
        bool left    = false;
        bool middle  = false;
        bool right   = false;
    } mouse;
};

// Starts the Joy-Con pointers. The first call initializes Nmouse for the whole
// process. Returns false when the pointer is unavailable, in which case the
// controller and touchscreen keep working.
bool pointerStart(int32_t width, int32_t height);

// Releases the Joy-Con pointers. Safe to call when start failed.
void pointerStop();

// Applies a new pointer area, for example after a dock/undock resolution change.
void pointerSetArea(int32_t width, int32_t height);

// Reads one frame. `pad` is updated in place because padPointerUpdate needs it.
RawPointerSample pointerSample(PadState* pad);

// Drops the latched L|ZL / R|ZR ownership. Implemented by the input manager,
// which owns the latches: called on focus loss, sleep, exit and around the
// system keyboard, so a click can never stay held across an applet switch.
void releasePointerClaims();

}  // namespace wili::switch2
