// PadPointer/Nmouse glue for the Borealis input override.
//
// libounce exposes the Joy-Con 2 optical mouse twice: the raw service in
// <switch2/services/nmouse.h> and the PadPointer wrapper in <switch/runtime/pad.h>.
// We feed PadPointer a dedicated PadState initialized with padInitializeAny so
// Joy-Cons on any wireless slot (No1..No8) or Handheld are polled, ensure every
// discovered Nmouse handle is enabled, and resolve the owning HidNpadIdType so
// button claims strip only that controller.

#include <borealis/core/logger.hpp>
#include <fmt/format.h>

#include "switch2_pointer_device.hpp"
#include "utils/boot_api.h"

// The pure decisions cannot include <switch.h>, so their bit assignments are
// restated there. These asserts are what keeps the two copies honest.
static_assert(wili::switch2::CLAIM_LEFT_BUTTONS == (HidNpadButton_L | HidNpadButton_ZL),
              "left Joy-Con mouse buttons must match HidNpadButton_L | HidNpadButton_ZL");
static_assert(wili::switch2::CLAIM_RIGHT_BUTTONS == (HidNpadButton_R | HidNpadButton_ZR),
              "right Joy-Con mouse buttons must match HidNpadButton_R | HidNpadButton_ZR");
static_assert(wili::switch2::ACTIVE_MOUSE_LEFT_MASK ==
                  (HidNpadButton_StickLLeft | HidNpadButton_StickLUp | HidNpadButton_StickLRight |
                   HidNpadButton_StickLDown | HidNpadButton_LeftSL | HidNpadButton_LeftSR),
              "left Joy-Con active mouse mask must match StickL directions and LeftSL/SR");
static_assert(wili::switch2::ACTIVE_MOUSE_RIGHT_MASK ==
                  (HidNpadButton_StickRLeft | HidNpadButton_StickRUp | HidNpadButton_StickRRight |
                   HidNpadButton_StickRDown | HidNpadButton_RightSL | HidNpadButton_RightSR),
              "right Joy-Con active mouse mask must match StickR directions and RightSL/SR");
static_assert((int)wili::switch2::PointerSide::Left == (int)PadPointerSide_Left, "PadPointerSide_Left");
static_assert((int)wili::switch2::PointerSide::Right == (int)PadPointerSide_Right, "PadPointerSide_Right");
static_assert((int)wili::switch2::PointerSide::None == (int)PadPointerSide_None, "PadPointerSide_None");

namespace wili::switch2 {

namespace {

PadState pointer_any_pad;
PadPointer pad_pointer;
bool pointer_started      = false;
s32 last_logged_handles   = -1;
bool logged_first_motion  = false;
HidMouseState standard_mouse;
bool standard_mouse_seen = false;

void enableAndReportNmouseHandles() {
    HidNmouseHandle handles[16]{};
    s32 total = 0;
    if (R_FAILED(hidGetAllNmouseHandles(handles, 16, &total))) return;

    for (s32 i = 0; i < total; ++i) {
        hidEnableNmousePointer(handles[i], true);
    }

    if (total != last_logged_handles) {
        last_logged_handles = total;
        std::string summary = fmt::format("nmouse handles={}", total);
        for (s32 i = 0; i < total && i < 4; ++i) {
            u32 type_set          = 0;
            HidNpadIdType npad_id = HidNpadIdType_No1;
            bool enabled          = false;
            HidNmouseState st{};
            size_t st_count = 0;
            hidGetNmouseTypeSet(handles[i], &type_set);
            hidGetNmouseNpadIdType(handles[i], &npad_id);
            hidIsNmousePointerEnabled(handles[i], &enabled);
            hidGetNmouseStates(handles[i], &st, 1, &st_count);
            summary += fmt::format(" [#{}:type={:#x} npad={:#x} en={} n={} flags={:#x} pos={},{}]", i, type_set,
                                   (unsigned)npad_id, enabled ? 1 : 0, st_count, st_count ? st.flags : 0u,
                                   st_count ? st.x : 0, st_count ? st.y : 0);
        }
        brls::Logger::info("switch2: {}", summary);
        wiliwili_boot_note("pointer", summary.c_str());
    }
}

}  // namespace

bool pointerStart(int32_t width, int32_t height) {
    if (pointer_started) return true;
    if (width <= 0 || height <= 0) return false;

    padInitializeAny(&pointer_any_pad);

    const Result rc = padPointerInitialize(&pad_pointer, width, height);
    if (R_FAILED(rc)) {
        const std::string msg = fmt::format("padPointerInitialize failed ({:#x})", rc);
        brls::Logger::warning("switch2: {}, falling back to controller and touch input", msg);
        wiliwili_boot_note("pointer", msg.c_str());
        return false;
    }

    pointer_started     = true;
    last_logged_handles = -1;
    logged_first_motion = false;
    const std::string msg = fmt::format("Joy-Con pointer ready ({}x{})", width, height);
    brls::Logger::info("switch2: {}", msg);
    wiliwili_boot_note("pointer", msg.c_str());
    return true;
}

void pointerStop() {
    if (!pointer_started) return;

    padPointerExit(&pad_pointer);
    pointer_started     = false;
    last_logged_handles = -1;
    logged_first_motion = false;
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

RawPointerSample pointerSample() {
    RawPointerSample sample;

    if (pointer_started) {
        enableAndReportNmouseHandles();
        padUpdate(&pointer_any_pad);
        padPointerUpdate(&pad_pointer, &pointer_any_pad);

        sample.pad.present = pad_pointer.active;
        sample.pad.side    = (PointerSide)pad_pointer.side;
        sample.pad.x       = pad_pointer.x;
        sample.pad.y       = pad_pointer.y;
        sample.pad.delta_x = pad_pointer.delta_x;
        sample.pad.delta_y = pad_pointer.delta_y;
        sample.pad.wheel_y = pad_pointer.wheel_delta_y;

        if (pad_pointer.active && R_SUCCEEDED(hidGetNmouseNpadIdType(pad_pointer.current, &sample.pad.npad_id))) {
            sample.pad.has_npad_id = true;
        }

        const u32 buttons = padPointerGetButtons(&pad_pointer);
        sample.pad.left   = (buttons & HidMouseButton_Left) != 0;
        sample.pad.middle = (buttons & HidMouseButton_Middle) != 0;
        sample.pad.right  = (buttons & HidMouseButton_Right) != 0;

        if (!logged_first_motion && sample.pad.present &&
            (sample.pad.delta_x != 0 || sample.pad.delta_y != 0 || sample.pad.wheel_y != 0 || sample.pad.left)) {
            logged_first_motion   = true;
            const std::string msg = fmt::format("first motion side={} npad={:#x} pos={},{} delta={},{} wheel={}",
                                                (int)sample.pad.side, (unsigned)sample.pad.npad_id, sample.pad.x,
                                                sample.pad.y, sample.pad.delta_x, sample.pad.delta_y,
                                                sample.pad.wheel_y);
            brls::Logger::info("switch2: {}", msg);
            wiliwili_boot_note("pointer", msg.c_str());
        }
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
