// Pure Switch 2 pointer decisions, shared by the Borealis input override and
// the host tests. Nothing here may include <switch.h>: the raw pad, HID and
// Nmouse calls live in switch2_pointer.cpp / borealis/switch_input.cpp.
//
// Vocabulary: the PadPointer/Nmouse "pointer area" is the framebuffer, while
// Borealis draws in logical pixels already multiplied by
// Application::windowScale. Every pointer and touch sample therefore goes
// through toLogical() exactly once.

#pragma once

#include <algorithm>
#include <cstdint>

namespace wili::switch2 {

// The framebuffer the pointer area covers, plus the scale that maps it to the
// logical content Borealis lays out.
struct Viewport {
    int raw_width  = 0;
    int raw_height = 0;
    float scale    = 1.0f;

    bool valid() const { return this->raw_width > 0 && this->raw_height > 0 && this->scale > 0.0f; }
    float logical_width() const { return this->valid() ? (float)this->raw_width / this->scale : 0.0f; }
    float logical_height() const { return this->valid() ? (float)this->raw_height / this->scale : 0.0f; }
};

struct LogicalPoint {
    float x = 0.0f;
    float y = 0.0f;
};

inline Viewport makeViewport(int raw_width, int raw_height, float scale) {
    if (raw_width <= 0 || raw_height <= 0 || !(scale > 0.0f)) return {};
    return Viewport{raw_width, raw_height, scale};
}

// Raw pointer/touch pixel into logical content coordinates. Values outside the
// content are clamped: the pointer area can still be the previous dock mode for
// one frame after a resolution change.
inline LogicalPoint toLogical(const Viewport& viewport, int raw_x, int raw_y) {
    if (!viewport.valid()) return {};

    return {std::clamp((float)raw_x / viewport.scale, 0.0f, viewport.logical_width()),
            std::clamp((float)raw_y / viewport.scale, 0.0f, viewport.logical_height())};
}

// The physical handheld digitizer domain: Horizon's default, and the range this
// port asks Horizon for. hidSetTouchScreenOutputRanges has to run before
// hidInitializeTouchScreen, and only the runtime answer says which domain is
// active.
inline constexpr int TOUCH_DEFAULT_WIDTH  = 1280;
inline constexpr int TOUCH_DEFAULT_HEIGHT = 720;
inline constexpr int TOUCH_OUTPUT_WIDTH   = 1920;
inline constexpr int TOUCH_OUTPUT_HEIGHT  = 1080;

struct TouchDomain {
    int width  = TOUCH_DEFAULT_WIDTH;
    int height = TOUCH_DEFAULT_HEIGHT;
};

inline TouchDomain touchDomain(bool output_ranges_accepted) {
    if (!output_ranges_accepted) return {TOUCH_DEFAULT_WIDTH, TOUCH_DEFAULT_HEIGHT};
    return {TOUCH_OUTPUT_WIDTH, TOUCH_OUTPUT_HEIGHT};
}

// Touch keeps a viewport of its own. The digitizer resolution does not change
// when the console is docked or the UI scale moves, so it must not be mapped
// through the pointer's framebuffer scale: its own domain maps onto the logical
// content, and the width ratio is the one scale (the digitizer and the panel
// share their aspect).
inline Viewport makeTouchViewport(const TouchDomain& domain, float content_width, float content_height) {
    if (domain.width <= 0 || domain.height <= 0 || !(content_width > 0.0f) || !(content_height > 0.0f)) return {};
    return makeViewport(domain.width, domain.height, (float)domain.width / content_width);
}

// One wheel notch scrolls like one notch of a desktop mouse: Borealis applies
// RawMouseState::scroll straight to the pan gesture, and the GLFW/SDL backends
// use 30 logical pixels per notch.
inline constexpr float SCROLL_PIXELS_PER_STEP = 30.0f;

struct ScrollDelta {
    float x = 0.0f;  // horizontal
    float y = 0.0f;  // vertical
};

inline ScrollDelta toScroll(int wheel_x, int wheel_y) {
    return {(float)wheel_x * SCROLL_PIXELS_PER_STEP, (float)wheel_y * SCROLL_PIXELS_PER_STEP};
}

// Which Joy-Con the pointer follows (PadPointerSide).
enum class PointerSide : uint8_t {
    None  = 0,
    Left  = 1,
    Right = 2,
};

// The Joy-Con shoulder buttons the system service maps to HidMouseButton_Left
// by default, from switch/services/hid.h: L = BIT(6), R = BIT(7), ZL = BIT(8),
// ZR = BIT(9). switch2_pointer.cpp asserts these against the real headers.
inline constexpr uint64_t CLAIM_LEFT_BUTTONS  = (1ull << 6) | (1ull << 8);
inline constexpr uint64_t CLAIM_RIGHT_BUTTONS = (1ull << 7) | (1ull << 9);

inline uint64_t claimMaskForSide(PointerSide side) {
    switch (side) {
        case PointerSide::Left:
            return CLAIM_LEFT_BUTTONS;
        case PointerSide::Right:
            return CLAIM_RIGHT_BUTTONS;
        default:
            return 0;
    }
}

// Button ownership: while a Joy-Con is used as a mouse, its L/ZL or R/ZR are the
// mouse click, so the gamepad mapping must never see them.
//
// The two halves are separate calls on purpose. update() runs once per frame
// from the one pad that drives PadPointer; a second controller's idle frame must
// not be able to wipe the latch, and its own shoulder buttons are not a click.
// strip() then applies the latch to that pad only.
class ButtonClaims {
public:
    // pointer_pad_buttons: the buttons of the pad that drives PadPointer.
    // claim_mask: the side's shoulder buttons while the pointer reports a click.
    // The claim survives until the button is physically released.
    uint64_t update(uint64_t pointer_pad_buttons, uint64_t claim_mask) {
        this->claimed = (this->claimed | claim_mask) & pointer_pad_buttons;
        return this->claimed;
    }

    // pad_buttons without the claimed ones, for the pad that owns the pointer. A
    // pad that does not own it keeps every button: the other Joy-Con stays a
    // controller.
    uint64_t strip(uint64_t pad_buttons, bool pointer_pad) const {
        return pointer_pad ? pad_buttons & ~this->claimed : pad_buttons;
    }

    void reset() { this->claimed = 0; }

    uint64_t held() const { return this->claimed; }

private:
    uint64_t claimed = 0;
};

enum class PointerSource : uint8_t {
    None,
    Mouse,
    Touch,
    Gamepad,
};

// Cursor visibility and input-mode ownership. The cursor belongs to the mouse
// only, hides after a period of inactivity, and is never taken away in the
// middle of a drag.
class CursorArbiter {
public:
    static constexpr uint64_t HIDE_DELAY_MS = 4000;

    // A Joy-Con or USB mouse sample arrived.
    // moved: the cursor position or the wheel changed.
    // left_down: a pointer click is held (a drag is in progress).
    void onPointerSample(uint64_t now_ms, bool moved, bool left_down) {
        dragging = left_down;
        if (moved || left_down) {
            owner       = PointerSource::Mouse;
            last_active = now_ms;
            active      = true;
        }
    }

    // Touch input hides the cursor, unless the user is dragging with the mouse.
    void onTouch() {
        if (dragging) return;
        owner = PointerSource::Touch;
    }

    // A gamepad button hands the interface back to controller navigation.
    void onGamepad() {
        if (dragging) return;
        owner = PointerSource::Gamepad;
    }

    // Focus loss, sleep or a system applet covers the app: drop every claim.
    void onFocusLost() {
        owner    = PointerSource::None;
        dragging = false;
        active   = false;
    }

    PointerSource source() const { return owner; }

    bool isDragging() const { return dragging; }

    bool visible(uint64_t now_ms) const {
        if (dragging) return true;
        if (!active || owner != PointerSource::Mouse) return false;
        return now_ms - last_active <= HIDE_DELAY_MS;
    }

private:
    PointerSource owner  = PointerSource::None;
    uint64_t last_active = 0;
    bool active          = false;
    bool dragging        = false;
};

// Cursor hover feedback. Borealis drops MouseState::view as soon as no button or
// wheel is active, so a mouse that only moves never highlights the control under
// it; the input manager hit-tests the cursor itself instead. The frame is a
// cursor affordance, not a focus change: it is skipped while the cursor is
// hidden or a click drags, and on the view the gamepad already frames.
struct HoverState {
    bool cursor_visible  = false;
    bool dragging        = false;
    bool has_target      = false;
    bool target_is_focus = false;  // the target is Application::currentFocus
    bool gamepad_input   = false;  // the gamepad draws that focus frame
};

inline bool showHoverFrame(const HoverState& state) {
    if (!state.cursor_visible || state.dragging || !state.has_target) return false;
    return !(state.target_is_focus && state.gamepad_input);
}

}  // namespace wili::switch2
