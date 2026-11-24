// PadPointer/Nmouse glue for the Borealis input override.
//
// libounce exposes the Joy-Con 2 optical mouse twice: the raw service in
// <switch2/services/nmouse.h> and the PadPointer wrapper in <switch/runtime/pad.h>.
// The wrapper is used here because it already picks the Joy-Con that last moved
// and sums its samples, which is the behaviour the cursor needs.

#include <borealis/core/logger.hpp>

#include "switch2_pointer_device.hpp"

// The pure decisions cannot include <switch.h>, so their bit assignments are
// restated there. These asserts are what keeps the two copies honest.
static_assert(wili::switch2::CLAIM_LEFT_BUTTONS == (HidNpadButton_L | HidNpadButton_ZL),
              "left Joy-Con mouse buttons must match HidNpadButton_L | HidNpadButton_ZL");
static_assert(wili::switch2::CLAIM_RIGHT_BUTTONS == (HidNpadButton_R | HidNpadButton_ZR),
              "right Joy-Con mouse buttons must match HidNpadButton_R | HidNpadButton_ZR");
static_assert((int)wili::switch2::PointerSide::Left == (int)PadPointerSide_Left, "PadPointerSide_Left");
static_assert((int)wili::switch2::PointerSide::Right == (int)PadPointerSide_Right, "PadPointerSide_Right");
static_assert((int)wili::switch2::PointerSide::None == (int)PadPointerSide_None, "PadPointerSide_None");

namespace wili::switch2 {

namespace {

PadPointer pad_pointer;
bool pointer_started = false;
HidMouseState standard_mouse;
bool standard_mouse_seen = false;

}  // namespace

bool pointerStart(int32_t width, int32_t height) {
    if (pointer_started) return true;
    if (width <= 0 || height <= 0) return false;

    const Result rc = padPointerInitialize(&pad_pointer, width, height);
    if (R_FAILED(rc)) {
        brls::Logger::warning(
            "switch2: Joy-Con pointer unavailable ({:#x}), "
            "falling back to controller and touch input",
            rc);
        return false;
    }

    pointer_started = true;
    brls::Logger::info("switch2: Joy-Con pointer ready ({}x{})", width, height);
    return true;
}

void pointerStop() {
    if (!pointer_started) return;

    padPointerExit(&pad_pointer);
    pointer_started     = false;
    standard_mouse_seen = false;
}

void pointerSetArea(int32_t width, int32_t height) {
    if (!pointer_started || width <= 0 || height <= 0) return;

    const Result rc = hidSetNmousePointerRanges(width, height);
    if (R_FAILED(rc))
        brls::Logger::warning("switch2: could not set the pointer area to {}x{} ({:#x})", width, height, rc);
    else
        brls::Logger::info("switch2: pointer area {}x{}", width, height);
}

RawPointerSample pointerSample(PadState* pad) {
    RawPointerSample sample;

    padUpdate(pad);

    if (pointer_started) {
        padPointerUpdate(&pad_pointer, pad);

        sample.pad.present = pad_pointer.active;
        sample.pad.side    = (PointerSide)pad_pointer.side;
        sample.pad.buttons = padGetButtons(pad);
        sample.pad.x       = pad_pointer.x;
        sample.pad.y       = pad_pointer.y;
        sample.pad.delta_x = pad_pointer.delta_x;
        sample.pad.delta_y = pad_pointer.delta_y;
        sample.pad.wheel_y = pad_pointer.wheel_delta_y;

        const u32 buttons = padPointerGetButtons(&pad_pointer);
        sample.pad.left   = (buttons & HidMouseButton_Left) != 0;
        sample.pad.middle = (buttons & HidMouseButton_Middle) != 0;
        sample.pad.right  = (buttons & HidMouseButton_Right) != 0;

        // Only the shoulder buttons of the Joy-Con that owns the pointer are
        // clicks; the other side stays available for gameplay.
        if (sample.pad.left) sample.pad.claim_mask = claimMaskForSide(sample.pad.side);
    }

    if (R_SUCCEEDED(hidGetMouseStates(&standard_mouse, 1)) &&
        (standard_mouse.attributes & HidMouseAttribute_IsConnected)) {
        if (!standard_mouse_seen) {
            standard_mouse_seen = true;
            brls::Logger::info("switch2: standard mouse connected");
        }

        sample.mouse.present = true;
        sample.mouse.delta_x = standard_mouse.delta_x;
        sample.mouse.delta_y = standard_mouse.delta_y;
        sample.mouse.wheel_x = standard_mouse.wheel_delta_x;
        sample.mouse.wheel_y = standard_mouse.wheel_delta_y;
        sample.mouse.left    = (standard_mouse.buttons & HidMouseButton_Left) != 0;
        sample.mouse.middle  = (standard_mouse.buttons & HidMouseButton_Middle) != 0;
        sample.mouse.right   = (standard_mouse.buttons & HidMouseButton_Right) != 0;
    } else if (standard_mouse_seen) {
        standard_mouse_seen = false;
        brls::Logger::info("switch2: standard mouse disconnected");
    }

    return sample;
}

}  // namespace wili::switch2
