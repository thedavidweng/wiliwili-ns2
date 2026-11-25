/*
    Copyright 2021 natinusala

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

// Switch 2 input manager. Replaces Borealis' switch/switch_input.cpp through the
// HEADER_FILE_ONLY override in library/CMakeLists.txt, so library/borealis stays
// at its pinned commit.
//
// Differences from the upstream file:
// - the Joy-Con 2 optical mouse (PadPointer/Nmouse) drives the cursor, and the
//   standard USB/Bluetooth mouse keeps working beside it;
// - one logical cursor: pointer and touch samples both go through
//   toLogical(), so a dock/undock or a UI scale change cannot skew them;
// - L/ZL and R/ZR stay claimed as mouse clicks until they are released, so a
//   click never also fires BUTTON_LB/LT or BUTTON_RB/RT;
// - the cursor hides when the mouse goes idle, while touch or the controller
//   takes over, and never in the middle of a drag.

#include <algorithm>
#include <cmath>

#include <switch.h>

#include <borealis/core/activity.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/view.hpp>
#include <borealis/core/time.hpp>
#include <borealis/platforms/switch/switch_input.hpp>

#include <switch2_pointer.hpp>
#include <switch2_pointer_device.hpp>

namespace wili::switch2 {

namespace {

using namespace brls;

// The Joy-Con pointer has no per-instance state in Borealis' SwitchInputManager
// header, and that header belongs to the pinned submodule. The platform creates
// exactly one input manager, so the pointer runtime lives here.
struct PointerRuntime {
    ButtonClaims claims;
    CursorArbiter cursor;
    PadMotionTracker motion;
    Viewport viewport;
    // The digitizer domain and the viewport it maps through are separate from
    // the pointer's: the physical touchscreen does not change resolution when the
    // console is docked.
    TouchDomain touch_domain;
    Viewport touch_viewport;
    // The pad PadPointer follows: only its buttons are claimed or masked while
    // that Joy-Con drives the cursor.
    PadState* pointer_pad      = nullptr;
    uint32_t prev_gamepad_mask = 0;
    LogicalPoint position;
    LogicalPoint offset;
    ScrollDelta scroll;
    RawPointerSample sample;
    bool left_down    = false;
    bool middle_down  = false;
    bool right_down   = false;
    bool moved        = false;
    bool touch_active = false;
    bool subscribed   = false;
    brls::Event<>::Subscription size_subscription;
    brls::Event<bool>::Subscription focus_subscription;
};

PointerRuntime runtime;

uint64_t nowMs() { return (uint64_t)getCPUTimeUsec() / 1000; }

int viewportWidth() {
    if (Application::windowWidth > 0) return (int)Application::windowWidth;
    return (int)Application::ORIGINAL_WINDOW_WIDTH;
}

int viewportHeight() {
    if (Application::windowHeight > 0) return (int)Application::windowHeight;
    return (int)Application::ORIGINAL_WINDOW_HEIGHT;
}

float viewportScale() { return Application::windowScale > 0.0f ? Application::windowScale : 1.0f; }

void clampCursorToViewport() {
    runtime.position.x = std::clamp(runtime.position.x, 0.0f, runtime.viewport.logical_width());
    runtime.position.y = std::clamp(runtime.position.y, 0.0f, runtime.viewport.logical_height());
}

void applyViewport() {
    const int width  = viewportWidth();
    const int height = viewportHeight();

    pointerSetArea(width, height);

    runtime.viewport = makeViewport(width, height, viewportScale());
    // Touch reports in the handheld digitizer's own domain, which a dock change
    // does not move: it maps onto the logical content, never through the
    // framebuffer scale above.
    runtime.touch_viewport =
        makeTouchViewport(runtime.touch_domain, runtime.viewport.logical_width(), runtime.viewport.logical_height());
    // A dock/undock can shrink the logical content under the cursor.
    clampCursorToViewport();
}

// The focusable view under the cursor. Borealis clears MouseState::view as soon
// as no button or wheel is active, so a mouse that only moves never carries one:
// the input manager finds the hover target itself instead.
View* hoverTarget() {
    const std::vector<Activity*> stack = Application::getActivitiesStack();
    if (stack.empty()) return nullptr;

    View* content = stack.back()->getContentView();
    if (content == nullptr) return nullptr;

    View* target = content->hitTest(Point(runtime.position.x, runtime.position.y));
    // Box::hitTest answers with the deepest box under the point even when it is
    // not focusable; a frame around a background container is only noise.
    return target != nullptr && target->isFocusable() ? target : nullptr;
}

// A subtle frame around the view a click would hit. It never moves the focus:
// setting Application::currentFocus would drag the gamepad highlight and play the
// focus sound on every mouse move.
void drawHoverFrame(NVGcontext* vg, uint64_t now) {
    View* target = hoverTarget();

    const HoverState hover{
        runtime.cursor.visible(now),
        runtime.cursor.isDragging(),
        target != nullptr,
        target != nullptr && target == Application::getCurrentFocus(),
        Application::getInputType() != InputType::TOUCH,
    };
    if (!showHoverFrame(hover)) return;

    const Rect frame    = target->getFrame();
    const float scale   = viewportScale();
    const float padding = 3.0f;

    NVGcolor color = Application::getTheme().getColor("brls/highlight/color1");
    color.a        = 0.8f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, frame.getMinX() - padding, frame.getMinY() - padding, frame.getWidth() + 2 * padding,
                   frame.getHeight() + 2 * padding, 6.0f);
    nvgStrokeColor(vg, color);
    // Logical units: drawCursor runs inside nvgScale(windowScale).
    nvgStrokeWidth(vg, 2.0f / scale);
    nvgStroke(vg);
}

}  // namespace

// Called from the system keyboard and from every place a click must not survive
// (focus loss, sleep, applet switch).
bool pointerFrameActive() {
    return runtime.moved || runtime.left_down || runtime.middle_down || runtime.right_down || runtime.touch_active;
}

void releasePointerClaims() {
    runtime.claims.reset();
    runtime.cursor.onFocusLost();
    runtime.motion.reset();
    runtime.pointer_pad  = nullptr;
    runtime.left_down    = false;
    runtime.middle_down  = false;
    runtime.right_down   = false;
    runtime.moved        = false;
    runtime.touch_active = false;
    runtime.offset       = {};
    runtime.scroll       = {};
}

}  // namespace wili::switch2

namespace brls {

using namespace wili::switch2;

static const uint64_t HidNpadButton_None = ((uint64_t)1 << (63));

static const uint64_t SWITCH_BUTTONS_FULL_MAPPING[_BUTTON_MAX] = {
    HidNpadButton_ZL,  // BUTTON_LT
    HidNpadButton_L,   // BUTTON_LB

    HidNpadButton_StickL,  // BUTTON_LSB

    HidNpadButton_Up,     // BUTTON_UP
    HidNpadButton_Right,  // BUTTON_RIGHT
    HidNpadButton_Down,   // BUTTON_DOWN
    HidNpadButton_Left,   // BUTTON_LEFT

    HidNpadButton_Minus,  // BUTTON_BACK
    HidNpadButton_None,   // BUTTON_GUIDE
    HidNpadButton_Plus,   // BUTTON_START

    HidNpadButton_StickR,  // BUTTON_RSB

    HidNpadButton_Y,  // BUTTON_Y
    HidNpadButton_B,  // BUTTON_B
    HidNpadButton_A,  // BUTTON_A
    HidNpadButton_X,  // BUTTON_X

    HidNpadButton_R,   // BUTTON_RB
    HidNpadButton_ZR,  // BUTTON_RT

    HidNpadButton_AnyUp,     // BUTTON_NAV_UP
    HidNpadButton_AnyRight,  // BUTTON_NAV_RIGHT
    HidNpadButton_AnyDown,   // BUTTON_NAV_DOWN
    HidNpadButton_AnyLeft,   // BUTTON_NAV_LEFT
};



static const size_t SWITCH_AXIS_MAPPING[_AXES_MAX] = {
    LEFT_X,
    LEFT_Y,
    RIGHT_X,
    RIGHT_Y,
};

SwitchInputManager::SwitchInputManager() {
    padConfigureInput(GAMEPADS_MAX, HidNpadStyleSet_NpadStandard);
    // Reset to Horizon's upright Joy-Con and dual-rail handheld defaults (another
    // NRO in the host process may have changed them): detaching one Joy-Con to
    // slide on a desk transitions cleanly onto No1..No8, and Horizon's Nmouse
    // pointer policy engages instead of treating single Joy-Cons as sideways pads.
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical);
    hidSetNpadHandheldActivationMode(HidNpadHandheldActivationMode_Dual);

    padInitialize(&this->padStateHandheld, HidNpadIdType_Handheld);
    hidInitializeVibrationDevices(m_vibration_device_handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    padUpdate(&this->padStateHandheld);

    for (int i = 0; i < GAMEPADS_MAX; i++) {
        padInitialize(&this->padsState[i], (HidNpadIdType)i);
        padUpdate(&this->padsState[i]);
        padsStyleSet[i] = this->padsState[i].style_set;
        clearVibration(i);
    }

    hidInitializeMouse();
    hidInitializeKeyboard();
    hidInitializeKeyboardMap();

    m_hid_keyboard_state.assign(256, false);

    // It's necessary to initialize these separately as they all have different handle values
    hidGetSixAxisSensorHandles(&this->m_six_axis_sensor_handle[0], 1, HidNpadIdType_Handheld,
                               HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&this->m_six_axis_sensor_handle[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&this->m_six_axis_sensor_handle[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidStartSixAxisSensor(this->m_six_axis_sensor_handle[0]);
    hidStartSixAxisSensor(this->m_six_axis_sensor_handle[1]);
    hidStartSixAxisSensor(this->m_six_axis_sensor_handle[2]);
    hidStartSixAxisSensor(this->m_six_axis_sensor_handle[3]);

    // The handheld digitizer keeps its own resolution, and it is not the
    // framebuffer: Horizon has to be told the range before the touchscreen is
    // initialized, and only the answer says which domain is active.
    const Result touch_ranges = hidSetTouchScreenOutputRanges(TOUCH_OUTPUT_WIDTH, TOUCH_OUTPUT_HEIGHT);
    runtime.touch_domain      = touchDomain(R_SUCCEEDED(touch_ranges));
    if (R_FAILED(touch_ranges))
        Logger::warning("switch2: touch digitizer stays at {}x{} ({:#x})", runtime.touch_domain.width,
                        runtime.touch_domain.height, touch_ranges);
    hidInitializeTouchScreen();

    // Joy-Con 2 optical mouse. Without it the controller and the touchscreen
    // keep working exactly as before.
    applyViewport();
    pointerStart(runtime.viewport.raw_width, runtime.viewport.raw_height);

    runtime.position           = {runtime.viewport.logical_width() / 2.0f, runtime.viewport.logical_height() / 2.0f};
    this->lastCursorPosition.x = runtime.position.x;
    this->lastCursorPosition.y = runtime.position.y;

    runtime.size_subscription = Application::getWindowSizeChangedEvent()->subscribe([]() {
        applyViewport();
        brls::Logger::info("switch2: pointer viewport now {}x{} at scale {}", runtime.viewport.raw_width,
                           runtime.viewport.raw_height, runtime.viewport.scale);
    });

    runtime.focus_subscription = Application::getWindowFocusChangedEvent()->subscribe([](bool focused) {
        // Sleep, the home menu and the system keyboard all take the focus
        // away: a held click must not stay latched behind them.
        if (!focused) releasePointerClaims();
    });

    runtime.subscribed = true;
}

SwitchInputManager::~SwitchInputManager() {
    releasePointerClaims();
    pointerStop();

    if (runtime.subscribed) {
        Application::getWindowSizeChangedEvent()->unsubscribe(runtime.size_subscription);
        Application::getWindowFocusChangedEvent()->unsubscribe(runtime.focus_subscription);
        runtime.subscribed = false;
    }

    NVGcontext* vg = Application::getNVGContext();

    if (vg && this->cursorTexture != 0) nvgDeleteImage(vg, this->cursorTexture);

    hidStopSixAxisSensor(this->m_six_axis_sensor_handle[0]);
    hidStopSixAxisSensor(this->m_six_axis_sensor_handle[1]);
    hidStopSixAxisSensor(this->m_six_axis_sensor_handle[2]);
    hidStopSixAxisSensor(this->m_six_axis_sensor_handle[3]);
}

void SwitchInputManager::clearVibration(int controller) {
    Logger::debug("Vibration clear #{}", controller);
    hidInitializeVibrationDevices(m_vibration_device_handles[controller], 2, (HidNpadIdType)controller,
                                  HidNpadStyleTag_NpadJoyDual);
    sendRumbleInternal(m_vibration_device_handles[controller], m_vibration_values[controller], 160.0f, 320.0f, 0.0f,
                       0.0f);
}

void SwitchInputManager::updateUnifiedControllerState(ControllerState* state) {
    for (size_t i = 0; i < _BUTTON_MAX; i++) state->buttons[i] = false;

    for (size_t i = 0; i < _AXES_MAX; i++) state->axes[i] = 0;

    auto connected = getControllersConnectedCount();
    for (int i = 0; i < connected; i++) {
        ControllerState localState;
        updateControllerState(&localState, i);
        for (size_t j = 0; j < _BUTTON_MAX; j++) state->buttons[j] |= localState.buttons[j];

        for (size_t j = 0; j < _AXES_MAX; j++) {
            state->axes[j] += localState.axes[j];

            if (state->axes[j] < -1)
                state->axes[j] = -1;
            else if (state->axes[j] > 1)
                state->axes[j] = 1;
        }
    }

    state->buttons[BUTTON_NAV_UP] |= getKeyboardKeyState(BRLS_KBD_KEY_UP);
    state->buttons[BUTTON_NAV_RIGHT] |= getKeyboardKeyState(BRLS_KBD_KEY_RIGHT);
    state->buttons[BUTTON_NAV_DOWN] |= getKeyboardKeyState(BRLS_KBD_KEY_DOWN);
    state->buttons[BUTTON_NAV_LEFT] |= getKeyboardKeyState(BRLS_KBD_KEY_LEFT);

    if (Application::isSwapInputKeys()) {
        state->buttons[BUTTON_B] |= getKeyboardKeyState(BRLS_KBD_KEY_ENTER);
        state->buttons[BUTTON_A] |= getKeyboardKeyState(BRLS_KBD_KEY_ESCAPE);
    } else {
        state->buttons[BUTTON_A] |= getKeyboardKeyState(BRLS_KBD_KEY_ENTER);
        state->buttons[BUTTON_B] |= getKeyboardKeyState(BRLS_KBD_KEY_ESCAPE);
    }

    uint32_t current_mask = 0;
    for (size_t i = 0; i < _BUTTON_MAX; i++) {
        if (state->buttons[i]) current_mask |= (1u << i);
    }
    const uint32_t newly_pressed = current_mask & ~runtime.prev_gamepad_mask;
    runtime.prev_gamepad_mask    = current_mask;
    if (newly_pressed != 0) runtime.cursor.onGamepad();
}

short SwitchInputManager::getControllersConnectedCount() {
    padUpdate(&this->padStateHandheld);
    int extra       = padStateHandheld.active_handheld ? 1 : 0;
    int controllers = extra;
    for (int i = 0; i < GAMEPADS_MAX - extra; i++) {
        padUpdate(&this->padsState[i]);
        if (padsState[i].style_set == 0) break;
        controllers++;
    }
    return controllers;
}

void SwitchInputManager::updateControllerState(ControllerState* state, int controller) {
    padUpdate(&this->padStateHandheld);
    if (controller == 0 && padStateHandheld.active_handheld) {
        updateControllerStateInner(state, &padStateHandheld);
        return;
    }

    int localController = padStateHandheld.active_handheld ? controller - 1 : controller;
    PadState* pad       = &this->padsState[localController];
    updateControllerStateInner(state, pad);

    if (padsStyleSet[localController] != pad->style_set) {
        padsStyleSet[localController] = pad->style_set;
        clearVibration(localController);
    }
}

void SwitchInputManager::updateControllerStateInner(ControllerState* state, PadState* pad) {
    padUpdate(pad);
    uint64_t keysDown = padGetButtons(pad);

    const bool is_pointer_pad = (pad == runtime.pointer_pad);
    const bool cursor_visible = runtime.cursor.visible(nowMs());

    // L/ZL or R/ZR of the Joy-Con driving the cursor are the mouse click, and
    // while the cursor is visible that Joy-Con's stick is the scroll wheel and
    // its rail SL/SR face the desk. Strip them only on the owning pad so every
    // other controller keeps all of its buttons.
    keysDown = runtime.claims.strip(keysDown, is_pointer_pad);
    if (is_pointer_pad) {
        keysDown &= ~activeMousePadMask(runtime.sample.pad.side, cursor_visible);
    }

    for (size_t i = 0; i < _BUTTON_MAX; i++) {
        uint64_t switchKey = SWITCH_BUTTONS_FULL_MAPPING[i];
        state->buttons[i]  = (keysDown & switchKey) != 0;
    }

    HidAnalogStickState analog_stick_l = padGetStickPos(pad, 0);
    HidAnalogStickState analog_stick_r = padGetStickPos(pad, 1);

    if (is_pointer_pad && cursor_visible) {
        if (runtime.sample.pad.side == PointerSide::Left) analog_stick_l = {0, 0};
        if (runtime.sample.pad.side == PointerSide::Right) analog_stick_r = {0, 0};
    }

    state->axes[LEFT_X]  = (float)analog_stick_l.x / (float)0x7FFF;
    state->axes[LEFT_Y]  = (float)analog_stick_l.y / (float)0x7FFF * -1.0f;
    state->axes[RIGHT_X] = (float)analog_stick_r.x / (float)0x7FFF;
    state->axes[RIGHT_Y] = (float)analog_stick_r.y / (float)0x7FFF * -1.0f;

    state->axes[LEFT_Z]  = 0;  // SWITCH NOT SUPPORT ZL AXIS
    state->axes[RIGHT_Z] = 0;  // SWITCH NOT SUPPORT ZR AXIS
}

bool SwitchInputManager::getKeyboardKeyState(BrlsKeyboardScancode key) {
    return m_hid_keyboard_state[m_brls_hid_map[key]];
}

void SwitchInputManager::updateTouchStates(std::vector<RawTouchState>* states) {
    // Get touchscreen state
    static HidTouchScreenState hidState;
    runtime.touch_active = false;

    if (hidGetTouchScreenStates(&hidState, 1) && hidState.count > 0) {
        // Touch takes the interface over from the mouse, which hides the cursor.
        runtime.touch_active = true;
        runtime.cursor.onTouch();

        for (int i = 0; i < hidState.count; i++) {
            RawTouchState state;
            state.pressed = true;

            const LogicalPoint position =
                toLogical(runtime.touch_viewport, hidState.touches[i].x, hidState.touches[i].y);
            state.fingerId   = hidState.touches[i].finger_id;
            state.position.x = position.x;
            state.position.y = position.y;
            states->push_back(state);
        }
    }
}

void SwitchInputManager::sendRumbleInternal(HidVibrationDeviceHandle vibration_device[2],
                                            HidVibrationValue vibration_values[2], unsigned short lowFreqMotor,
                                            unsigned short highFreqMotor) {
    float low  = (float)lowFreqMotor / 0xFFFF;
    float high = (float)highFreqMotor / 0xFFFF;

    vibration_values[0].amp_low   = low;
    vibration_values[0].freq_low  = low * 50;
    vibration_values[0].amp_high  = high;
    vibration_values[0].freq_high = high * 100;

    vibration_values[1].amp_low   = low;
    vibration_values[1].freq_low  = low * 50;
    vibration_values[1].amp_high  = high;
    vibration_values[1].freq_high = high * 100;

    hidSendVibrationValues(vibration_device, vibration_values, 2);
}

void SwitchInputManager::sendRumbleInternal(HidVibrationDeviceHandle vibration_device[2],
                                            HidVibrationValue vibration_values[2], float lowFreq, float highFreq,
                                            float lowAmp, float highAmp) {
    vibration_values[0].amp_low   = lowAmp;
    vibration_values[0].freq_low  = lowFreq;
    vibration_values[0].amp_high  = highAmp;
    vibration_values[0].freq_high = highFreq;

    vibration_values[1].amp_low   = lowAmp;
    vibration_values[1].freq_low  = lowFreq;
    vibration_values[1].amp_high  = highAmp;
    vibration_values[1].freq_high = highFreq;

    hidSendVibrationValues(vibration_device, vibration_values, 2);
}

void SwitchInputManager::sendRumbleRaw(float lowFreq, float highFreq, float lowAmp, float highAmp) {
    padUpdate(&this->padStateHandheld);
    if (padStateHandheld.active_handheld) {
        sendRumbleInternal(m_vibration_device_handheld, m_vibration_values_handheld, lowFreq, highFreq, lowAmp,
                           highAmp);
    } else {
        sendRumbleInternal(m_vibration_device_handles[0], m_vibration_values[0], lowFreq, highFreq, lowAmp, highAmp);
    }
}

void SwitchInputManager::sendRumble(unsigned short controller, unsigned short lowFreqMotor,
                                    unsigned short highFreqMotor) {
    padUpdate(&this->padStateHandheld);
    if (controller == 0 && padStateHandheld.active_handheld) {
        sendRumbleInternal(m_vibration_device_handheld, m_vibration_values_handheld, lowFreqMotor, highFreqMotor);
        return;
    }

    int localController = padStateHandheld.active_handheld ? controller - 1 : controller;
    sendRumbleInternal(m_vibration_device_handles[localController], m_vibration_values[localController], lowFreqMotor,
                       highFreqMotor);
}

void SwitchInputManager::updateMouseStates(RawMouseState* state) {
    state->position     = Point(runtime.position.x, runtime.position.y);
    state->offset       = Point(runtime.offset.x, runtime.offset.y);
    state->scroll       = Point(runtime.scroll.x, runtime.scroll.y);
    state->leftButton   = runtime.left_down;
    state->middleButton = runtime.middle_down;
    state->rightButton  = runtime.right_down;

    lastCursorPosition = state->position;
}

void SwitchInputManager::runloopStart() {
    upToDateMouseState();
    handleMouse();
    handleKeyboard();
    handleControllerSensors();
}

void SwitchInputManager::upToDateMouseState() {
    const uint64_t now = nowMs();

    runtime.sample = pointerSample();

    PadState* resolved_pad = nullptr;
    if (runtime.sample.pad.has_npad_id) {
        const HidNpadIdType id = runtime.sample.pad.npad_id;
        if (id == HidNpadIdType_Handheld) {
            resolved_pad = &this->padStateHandheld;
        } else if ((int)id >= 0 && (int)id < GAMEPADS_MAX) {
            resolved_pad = &this->padsState[(int)id];
        }
    }
    if (resolved_pad != nullptr) {
        runtime.pointer_pad = resolved_pad;
    } else if (runtime.claims.held() == 0) {
        runtime.pointer_pad = nullptr;
    }

    runtime.moved  = false;
    runtime.offset = {};
    runtime.scroll = {};
    bool left = false, middle = false, right = false;

    const PadDelta pad_delta = runtime.motion.update(
        runtime.sample.pad.present, runtime.sample.pad.side, runtime.sample.pad.x, runtime.sample.pad.y,
        runtime.sample.pad.delta_x, runtime.sample.pad.delta_y);

    if (runtime.sample.pad.present) {
        // The Joy-Con pointer reports an absolute position inside the pointer
        // area, which is the framebuffer we handed to the service.
        runtime.position = toLogical(runtime.viewport, runtime.sample.pad.x, runtime.sample.pad.y);

        if (pad_delta.moved) {
            runtime.moved  = true;
            runtime.offset = {(float)pad_delta.dx / runtime.viewport.scale,
                              (float)pad_delta.dy / runtime.viewport.scale};
        }

        if (runtime.sample.pad.wheel_y != 0) {
            runtime.moved  = true;
            runtime.scroll = toScroll(0, runtime.sample.pad.wheel_y);
        }

        left   = left || runtime.sample.pad.left;
        middle = middle || runtime.sample.pad.middle;
        right  = right || runtime.sample.pad.right;
    }

    if (runtime.sample.mouse.present) {
        // A standard mouse only has relative movement: its absolute coordinates
        // do not describe the framebuffer.
        if (runtime.sample.mouse.delta_x != 0 || runtime.sample.mouse.delta_y != 0) {
            runtime.moved  = true;
            runtime.offset = {(float)runtime.sample.mouse.delta_x / runtime.viewport.scale,
                              (float)runtime.sample.mouse.delta_y / runtime.viewport.scale};
            runtime.position.x += runtime.offset.x;
            runtime.position.y += runtime.offset.y;
            clampCursorToViewport();
        }

        if (runtime.sample.mouse.wheel_x != 0 || runtime.sample.mouse.wheel_y != 0) {
            runtime.moved  = true;
            runtime.scroll = toScroll(runtime.sample.mouse.wheel_x, runtime.sample.mouse.wheel_y);
        }

        left   = left || runtime.sample.mouse.left;
        middle = middle || runtime.sample.mouse.middle;
        right  = right || runtime.sample.mouse.right;
    }

    runtime.left_down   = left;
    runtime.middle_down = middle;
    runtime.right_down  = right;

    runtime.cursor.onPointerSample(now, runtime.moved || runtime.middle_down || runtime.right_down, runtime.left_down);

    const bool cursor_visible = runtime.cursor.visible(now);

    // Claim L/ZL or R/ZR for the entire time the Joy-Con cursor is visible (plus
    // any held click), so the first frame of a shoulder press never leaks into
    // gamepad LB/RB/LT/RT before Nmouse projects HidMouseButton_Left.
    uint64_t pointer_pad_buttons = 0;
    if (runtime.pointer_pad != nullptr) {
        padUpdate(runtime.pointer_pad);
        pointer_pad_buttons = padGetButtons(runtime.pointer_pad);
    }
    const uint64_t claim_mask =
        (runtime.sample.pad.present && (cursor_visible || runtime.sample.pad.left))
            ? claimMaskForSide(runtime.sample.pad.side)
            : 0;
    runtime.claims.update(pointer_pad_buttons, claim_mask);

    if (cursor_visible) {
        this->pointerLocked = false;
        Application::setDrawCoursor(true);
    }
}

void SwitchInputManager::handleMouse() {
    if (runtime.offset.x != 0 || runtime.offset.y != 0)
        getMouseCusorOffsetChanged()->fire(Point(runtime.offset.x, runtime.offset.y));

    if (runtime.scroll.x != 0 || runtime.scroll.y != 0)
        getMouseScrollOffsetChanged()->fire(Point(runtime.scroll.x, runtime.scroll.y));
}

void SwitchInputManager::handleKeyboard() {
    HidKeyboardState state;

    if (hidGetKeyboardStates(&state, 1)) {
        for (int i = 0; i < 256; ++i) {
            auto is_pressed = (state.keys[i / 64] & (1ul << (i % 64))) != 0;
            if (m_hid_keyboard_state[i] != is_pressed) {
                m_hid_keyboard_state[i] = is_pressed;

                // Keyboard navigation is controller-like input: the mouse
                // cursor steps aside.
                if (is_pressed) runtime.cursor.onGamepad();

                const BrlsKeyboardScancode brlsKey = m_hid_brls_map[i];

                KeyState keyState{};
                keyState.key     = brlsKey;
                keyState.pressed = is_pressed;

                if (state.modifiers & HidKeyboardModifier_LeftAlt) keyState.mods |= BRLS_KBD_MODIFIER_ALT;

                if (state.modifiers & HidKeyboardModifier_Control) keyState.mods |= BRLS_KBD_MODIFIER_CTRL;

                if (state.modifiers & HidKeyboardModifier_Shift) keyState.mods |= BRLS_KBD_MODIFIER_SHIFT;

                if (state.modifiers & HidKeyboardModifier_Gui) keyState.mods |= BRLS_KBD_MODIFIER_META;

                getKeyboardKeyStateChanged()->fire(keyState);
            }
        }
    } else {
        Logger::debug("Keyboard failed!");
    }
}

void SwitchInputManager::handleControllerSensors() {
    HidSixAxisSensorState sixaxis = {0};

    uint64_t style_set = padGetStyleSet(&padsState[0]);
    if (padStateHandheld.active_handheld)
        hidGetSixAxisSensorStates(this->m_six_axis_sensor_handle[0], &sixaxis, 1);
    else if (style_set & HidNpadStyleTag_NpadFullKey)
        hidGetSixAxisSensorStates(this->m_six_axis_sensor_handle[1], &sixaxis, 1);
    else if (style_set & HidNpadStyleTag_NpadJoyDual) {
        // For JoyDual, read from either the Left or Right Joy-Con depending on which is/are connected
        u64 attrib = padGetAttributes(&padsState[0]);
        if (attrib & HidNpadAttribute_IsLeftConnected)
            hidGetSixAxisSensorStates(this->m_six_axis_sensor_handle[2], &sixaxis, 1);
        else if (attrib & HidNpadAttribute_IsRightConnected)
            hidGetSixAxisSensorStates(this->m_six_axis_sensor_handle[3], &sixaxis, 1);
    }

    auto accelState = SensorEvent{
        0, SensorEventType::ACCEL, {-sixaxis.acceleration.x, -sixaxis.acceleration.z, sixaxis.acceleration.y}, 0};
    auto gyroState = SensorEvent{0,
                                 SensorEventType::GYRO,
                                 {static_cast<float>(sixaxis.angular_velocity.x * 2.0f * M_PI),
                                  static_cast<float>(sixaxis.angular_velocity.z * 2.0f * M_PI),
                                  static_cast<float>(-sixaxis.angular_velocity.y * 2.0f * M_PI)},
                                 0};
    getControllerSensorStateChanged()->fire(accelState);
    getControllerSensorStateChanged()->fire(gyroState);
    Application::setActiveEvent(true);
}

void SwitchInputManager::setPointerLock(bool lock) { pointerLocked = lock; }

void SwitchInputManager::drawCursor(NVGcontext* vg) {
    initCursor(vg);

    if (this->cursorTexture == 0) return;

    const uint64_t now = nowMs();
    // The cursor belongs to the mouse, hides when it goes idle, and survives a
    // drag even if wiliwili locked it when the last click promoted the input
    // type to the gamepad.
    if (!runtime.cursor.visible(now)) return;
    if (pointerLocked && !runtime.cursor.isDragging()) return;

    drawHoverFrame(vg, now);

    // Drawn in logical units inside Application::frame()'s nvgScale(windowScale),
    // so the cursor scales cleanly with handheld 1080p, docked 4K and UI scale.
    this->paint.xform[4] = runtime.position.x;
    this->paint.xform[5] = runtime.position.y;

    nvgBeginPath(vg);
    nvgRect(vg, runtime.position.x, runtime.position.y, (float)this->cursorWidth, (float)this->cursorHeight);
    nvgFillPaint(vg, this->paint);
    nvgFill(vg);
}

void SwitchInputManager::initCursor(NVGcontext* vg) {
    if (cursorInited) return;
    if (vg) {
#ifdef USE_LIBROMFS
        auto image          = romfs::get("img/sys/cursor.png");
        this->cursorTexture = nvgCreateImageMem(vg, 0, (unsigned char*)image.data(), image.size());
#else
        this->cursorTexture = nvgCreateImage(vg, BRLS_ASSET("img/sys/cursor.png"), 0);
#endif

        int width = 0, height = 0;
        nvgImageSize(vg, cursorTexture, &width, &height);
        float aspect       = width > 0 ? (float)height / (float)width : 1.0f;
        this->cursorWidth  = 20;
        this->cursorHeight = (int)std::lround(20.0f * aspect);

        this->paint = nvgImagePattern(vg, 0, 0, (float)this->cursorWidth, (float)this->cursorHeight, 0,
                                      this->cursorTexture, 1.0f);
        this->cursorInited = true;
    }
}

static BrlsKeyboardScancode hidKey2BrlsKey(int key) {
    if (KBD_A <= key && key <= KBD_Z) {
        return (BrlsKeyboardScancode)(key - KBD_A + BRLS_KBD_KEY_A);
    }
    if (KBD_1 <= key && key <= KBD_9) {
        return (BrlsKeyboardScancode)(key - KBD_1 + BRLS_KBD_KEY_1);
    }
    if (KBD_F1 <= key && key <= KBD_F12) {
        return (BrlsKeyboardScancode)(key - KBD_F1 + BRLS_KBD_KEY_F1);
    }
    if (KBD_KP1 <= key && key <= KBD_KP9) {
        return (BrlsKeyboardScancode)(key - KBD_KP1 + BRLS_KBD_KEY_KP_1);
    }

    switch (key) {
        case KBD_0:
            return BRLS_KBD_KEY_0;
        case KBD_SPACE:
            return BRLS_KBD_KEY_SPACE;
        case KBD_APOSTROPHE:
            return BRLS_KBD_KEY_APOSTROPHE;
        case KBD_COMMA:
            return BRLS_KBD_KEY_COMMA;
        case KBD_MINUS:
            return BRLS_KBD_KEY_MINUS;
        case KBD_DOT:
            return BRLS_KBD_KEY_PERIOD;
        case KBD_SLASH:
            return BRLS_KBD_KEY_SLASH;
        case KBD_SEMICOLON:
            return BRLS_KBD_KEY_SEMICOLON;
        case KBD_EQUAL:
            return BRLS_KBD_KEY_EQUAL;
        case KBD_LEFTBRACE:
            return BRLS_KBD_KEY_LEFT_BRACKET;
        case KBD_RIGHTBRACE:
            return BRLS_KBD_KEY_RIGHT_BRACKET;
        case KBD_BACKSLASH:
            return BRLS_KBD_KEY_BACKSLASH;
        case KBD_GRAVE:
            return BRLS_KBD_KEY_GRAVE_ACCENT;
        case KBD_ESC:
            return BRLS_KBD_KEY_ESCAPE;
        case KBD_ENTER:
            return BRLS_KBD_KEY_ENTER;
        case KBD_TAB:
            return BRLS_KBD_KEY_TAB;
        case KBD_BACKSPACE:
            return BRLS_KBD_KEY_BACKSPACE;
        case KBD_CAPSLOCK:
            return BRLS_KBD_KEY_CAPS_LOCK;
        case KBD_LEFTSHIFT:
            return BRLS_KBD_KEY_LEFT_SHIFT;
        case KBD_LEFTCTRL:
            return BRLS_KBD_KEY_LEFT_CONTROL;
        case KBD_LEFTALT:
            return BRLS_KBD_KEY_LEFT_ALT;
        case KBD_LEFTMETA:
            return BRLS_KBD_KEY_LEFT_SUPER;
        case KBD_RIGHTSHIFT:
            return BRLS_KBD_KEY_RIGHT_SHIFT;
        case KBD_RIGHTCTRL:
            return BRLS_KBD_KEY_RIGHT_CONTROL;
        case KBD_RIGHTALT:
            return BRLS_KBD_KEY_RIGHT_ALT;
        case KBD_RIGHTMETA:
            return BRLS_KBD_KEY_RIGHT_SUPER;
        case KBD_LEFT:
            return BRLS_KBD_KEY_LEFT;
        case KBD_RIGHT:
            return BRLS_KBD_KEY_RIGHT;
        case KBD_UP:
            return BRLS_KBD_KEY_UP;
        case KBD_DOWN:
            return BRLS_KBD_KEY_DOWN;

        case KBD_SYSRQ:
            return BRLS_KBD_KEY_PRINT_SCREEN;
        case KBD_SCROLLLOCK:
            return BRLS_KBD_KEY_SCROLL_LOCK;
        case KBD_PAUSE:
            return BRLS_KBD_KEY_PAUSE;
        case KBD_INSERT:
            return BRLS_KBD_KEY_INSERT;
        case KBD_HOME:
            return BRLS_KBD_KEY_HOME;
        case KBD_PAGEUP:
            return BRLS_KBD_KEY_PAGE_UP;
        case KBD_DELETE:
            return BRLS_KBD_KEY_DELETE;
        case KBD_END:
            return BRLS_KBD_KEY_END;
        case KBD_PAGEDOWN:
            return BRLS_KBD_KEY_PAGE_DOWN;

        case KBD_NUMLOCK:
            return BRLS_KBD_KEY_NUM_LOCK;
        case KBD_KPSLASH:
            return BRLS_KBD_KEY_KP_DIVIDE;
        case KBD_KPASTERISK:
            return BRLS_KBD_KEY_KP_MULTIPLY;
        case KBD_KPMINUS:
            return BRLS_KBD_KEY_KP_SUBTRACT;
        case KBD_KPPLUS:
            return BRLS_KBD_KEY_KP_ADD;
        case KBD_KPENTER:
            return BRLS_KBD_KEY_KP_ENTER;
        case KBD_KPDOT:
            return BRLS_KBD_KEY_KP_DECIMAL;
        case KBD_KP0:
            return BRLS_KBD_KEY_KP_0;

        case KBD_102ND:
            return BRLS_KBD_KEY_WORLD_1;

        default:
            return BRLS_KBD_KEY_UNKNOWN;
    }
}

void SwitchInputManager::hidInitializeKeyboardMap() {
    m_hid_brls_map[KBD_NONE]             = BRLS_KBD_KEY_UNKNOWN;
    m_brls_hid_map[BRLS_KBD_KEY_UNKNOWN] = KBD_NONE;

    for (int i = 0; i < 256; ++i) {
        const auto brlsKey = hidKey2BrlsKey(i);
        if (brlsKey == BRLS_KBD_KEY_UNKNOWN) continue;
        m_hid_brls_map[i]       = brlsKey;
        m_brls_hid_map[brlsKey] = i;
    }
}

}  // namespace brls
