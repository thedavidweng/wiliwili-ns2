// Device side of the Switch 2 pointer: the PadPointer/Nmouse calls and the
// standard USB/Bluetooth mouse that share the one Borealis cursor. Only
// switch2_pointer.cpp and the Switch 2 Borealis overrides include this header.

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
        bool present         = false;
        bool has_npad_id     = false;
        HidNpadIdType npad_id = HidNpadIdType_No1;
        PointerSide side     = PointerSide::None;
        int x                = 0;
        int y                = 0;
        int delta_x          = 0;
        int delta_y          = 0;
        int wheel_y          = 0;
        bool left            = false;
        bool middle          = false;
        bool right           = false;
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

// Starts the Joy-Con pointers across all Npad slots (No1..No8 and Handheld).
// The first call initializes Nmouse for the whole process. Returns false when
// the pointer is unavailable, in which case controller and touch keep working.
bool pointerStart(int32_t width, int32_t height);

// Releases the Joy-Con pointers. Safe to call when start failed.
void pointerStop();

// Applies a new pointer area, for example after a dock/undock resolution change.
void pointerSetArea(int32_t width, int32_t height);

// Reads one frame across all connected controllers and resolves the Npad ID of
// whichever Joy-Con currently owns the pointer.
RawPointerSample pointerSample();

// True when pointer or touch input occurred on the latest frame, so the
// platform frame pacer stays at panel refresh instead of idling down to 30 Hz.
bool pointerFrameActive();

// Drops the latched L|ZL / R|ZR ownership. Implemented by the input manager,
// which owns the latches: called on focus loss, sleep, exit and around the
// system keyboard, so a click can never stay held across an applet switch.
void releasePointerClaims();

}  // namespace wili::switch2
