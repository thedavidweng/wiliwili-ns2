/*
    Copyright 2019  WerWolv
    Copyright 2019  p-sam
    Copyright 2023 xfangfang

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

// Switch 2 system keyboard. Replaces Borealis' switch/switch_ime.cpp through the
// HEADER_FILE_ONLY override in library/CMakeLists.txt.
//
// Differences from the upstream file:
// - SwkbdType_All, so Simplified and Traditional Chinese composition is offered;
// - the output buffer is sized for UTF-8 (maxStringLength * 4 + 1) instead of a
//   fixed 0x100 bytes, which truncated long CJK input;
// - masking is its own request (KEYBOARD_SENSITIVE_INPUT), not "this field may
//   not contain spaces": the flag is stripped before the disable bits are mapped
//   onto SwkbdKeyDisableBitmask, and neither the returned text nor the initial
//   text is ever logged;
// - a user cancel (LibAppletBadExit) is told apart from the keyboard being
//   unavailable at runtime, and the unavailable case falls back to the in-app
//   EditTextDialog fed by a USB/Bluetooth keyboard, with Pinyin composition so
//   Chinese search terms stay possible;
// - the pointer claims are released around the keyboard and the controller gets
//   the focus back afterwards.

#include <switch.h>

#include <cstring>
#include <memory>

#include <fmt/format.h>

#include <borealis/core/application.hpp>
#include <borealis/core/input.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/platforms/switch/switch_ime.hpp>
#include <borealis/views/edit_text_dialog.hpp>
#include <borealis/views/label.hpp>

#include <switch2_cjk.hpp>
#include <switch2_pointer_device.hpp>

static_assert((KEYBOARD_SENSITIVE_INPUT & 0xFF) == 0,
              "the sensitive-input flag has to live outside Borealis' 0..7 disable bits");

namespace brls {

using namespace wili::switch2;

namespace {

// libnx reports a user cancel as an applet that closed without a result.
constexpr Result SWKBD_USER_CANCEL = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);

int getSwkbdKeyDisableBitmask(int borealis_key) {
    // translate brls to Switch libnx values. The sensitive-input flag is not a
    // character restriction, so it is masked out first.
    int ret      = 0;
    borealis_key = imeKeyDisableBits(borealis_key);

    if (borealis_key == brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE) return 0;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE)
        // Disable space-bar
        ret |= SwkbdKeyDisableBitmask_Space;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_AT)
        // Disable '@'.
        ret |= SwkbdKeyDisableBitmask_At;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_PERCENT)
        // Disable '%'.
        ret |= SwkbdKeyDisableBitmask_Percent;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_FORWSLASH)
        // Disable '/'.
        ret |= SwkbdKeyDisableBitmask_ForwardSlash;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_BACKSLASH)
        // Disable '\'.
        ret |= SwkbdKeyDisableBitmask_Backslash;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NUMBERS)
        // Disable numbers.
        ret |= SwkbdKeyDisableBitmask_Numbers;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_DOWNLOADCODE)
        // Used for swkbdConfigMakePresetDownloadCode.
        ret |= SwkbdKeyDisableBitmask_DownloadCode;

    if (borealis_key & brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_USERNAME)
        // Used for swkbdConfigMakePresetUserName. Disables '@', '%', and '\'.
        ret |= SwkbdKeyDisableBitmask_UserName;

    return ret;
}

// The text entry used when the system keyboard cannot start: Borealis'
// EditTextDialog (Apache-2.0, no bundled assets) driven by a USB or Bluetooth
// keyboard. Letters compose Pinyin, so a Chinese search term is still reachable
// without swkbd; a run of letters that is not a syllable is kept as typed, which
// is what makes a Latin term work in the same field.
class UsbKeyboardInput {
public:
    UsbKeyboardInput(EditTextDialog* dialog, std::string hint, std::size_t limit, std::string initial, bool sensitive)
        : dialog(dialog), hint(std::move(hint)), limit(limit), text(std::move(initial)), sensitive(sensitive) {}

    void refresh() {
        const std::string pending = this->composer.syllable();
        // A sensitive field never echoes what is typed, the pending syllable
        // included.
        const std::string shown =
            this->sensitive ? std::string(this->text.size() + pending.size(), '*') : this->text + pending;

        this->dialog->setText(shown);
        this->dialog->setCountText(fmt::format("{}/{}", this->text.size() + pending.size(), this->limit));

        // The candidates of the syllable being composed, with the digit that
        // picks each one.
        const std::vector<std::string> candidates = this->composer.candidates();
        std::string candidateHint;
        for (std::size_t i = 0; i < candidates.size() && i < 9; ++i) {
            if (!candidateHint.empty()) candidateHint += ' ';
            candidateHint += fmt::format("{}.{}", i + 1, candidates[i]);
        }
        this->dialog->setHintText(candidateHint.empty() ? this->hint : candidateHint);
    }

    void type(KeyState state) {
        const std::string utf8 = keyboardKeyUtf8((int)state.key, (state.mods & BRLS_KBD_MODIFIER_SHIFT) != 0);
        if (utf8.empty()) return;

        // 1..9 pick a candidate of the syllable being composed.
        if (utf8.size() == 1 && utf8[0] >= '1' && utf8[0] <= '9' && !this->composer.candidates().empty()) {
            this->append(this->commit((std::size_t)(utf8[0] - '1')));
            return;
        }

        // A letter joins the syllable; commit decides whether it becomes Hanzi or
        // stays as typed.
        if (utf8.size() == 1 && ((utf8[0] >= 'a' && utf8[0] <= 'z') || (utf8[0] >= 'A' && utf8[0] <= 'Z'))) {
            if (!this->composer.append(utf8[0])) {
                // The buffer is full, which a Latin word reaches and no syllable
                // does: flush it as typed and start again, so no key is dropped.
                this->append(this->commit(0));
                this->composer.append(utf8[0]);
            }
            this->refresh();
            return;
        }

        // Space (or any other key) commits the syllable first, so "ni" + space is
        // 你 and a second syllable can follow.
        this->append(this->commit(0));
        if (utf8 != " ") this->append(utf8);
        this->refresh();
    }

    void backspace() {
        if (this->composer.backspace()) {
            this->refresh();
            return;
        }
        if (this->text.empty()) return;

        // Drop a whole UTF-8 sequence, not a byte.
        std::size_t begin = this->text.size() - 1;
        while (begin > 0 && (this->text[begin] & 0xC0) == 0x80) --begin;
        this->text.erase(begin);
        this->refresh();
    }

    std::string takeText() {
        this->append(this->commit(0));
        return this->text;
    }

private:
    // The Hanzi for the composed syllable, or the letters themselves when they
    // are not one. Always clears the composer.
    std::string commit(std::size_t index) {
        const std::string typed = this->composer.syllable();
        const std::string hanzi = this->composer.commit(index);
        return hanzi.empty() ? typed : hanzi;
    }

    void append(const std::string& utf8) {
        if (utf8.empty()) return;
        if (this->text.size() + utf8.size() > this->limit) return;
        this->text += utf8;
    }

    EditTextDialog* dialog;
    std::string hint;
    std::size_t limit;
    std::string text;
    bool sensitive;
    PinyinComposer composer;
};

void openWithUsbKeyboard(std::function<void(std::string)> callback, const std::string& headerText,
                         const std::string& subText, int maxStringLength, const std::string& initialText,
                         bool sensitive) {
    EditTextDialog* dialog           = new EditTextDialog();
    const std::size_t characterLimit = maxStringLength < 1 ? 1 : (std::size_t)maxStringLength;
    auto input = std::make_shared<UsbKeyboardInput>(dialog, subText, characterLimit, initialText, sensitive);

    dialog->setHeaderText(headerText);
    input->refresh();

    // One key press at a time from the standard keyboard. The dialog closes
    // itself on A (submit) and B (cancel), which fire the events below; the
    // subscription must go away with it.
    InputManager* manager = Application::getPlatform()->getInputManager();
    auto subscription     = std::make_shared<Event<KeyState>::Subscription>();
    auto unsubscribe = [manager, subscription]() { manager->getKeyboardKeyStateChanged()->unsubscribe(*subscription); };

    *subscription = manager->getKeyboardKeyStateChanged()->subscribe([input](KeyState state) {
        if (!state.pressed || (state.mods & BRLS_KBD_MODIFIER_CTRL) || (state.mods & BRLS_KBD_MODIFIER_META)) return;
        input->type(state);
    });

    dialog->getBackspaceEvent()->subscribe([input]() { input->backspace(); });

    dialog->getSubmitEvent()->subscribe([callback, input, unsubscribe]() {
        unsubscribe();
        // The fallback table is Simplified; the linked OpenCC s2t dictionary
        // gives Traditional Chinese, exactly as the labels do.
        const std::string text = input->takeText();
        callback(Label::STConverter(text));
    });
    dialog->getCancelEvent()->subscribe([unsubscribe]() { unsubscribe(); });

    // Same contract as the SDL/GLFW backends: text entry is in progress and the
    // callback decides when it is done.
    dialog->open();
}

}  // namespace

bool SwitchImeManager::openForText(std::function<void(std::string)> f, std::string headerText, std::string subText,
                                   int maxStringLength, std::string initialText, int kbdDisableBitmask) {
    const bool sensitive = imeIsSensitive(kbdDisableBitmask);

    // A click must not stay latched while the system keyboard is in front.
    releasePointerClaims();

    SwkbdConfig config;
    Result rc = swkbdCreate(&config, 0);
    if (R_FAILED(rc)) {
        Logger::error("switch: the system keyboard did not start ({:#x}), using the in-app text entry", rc);
        openWithUsbKeyboard(f, headerText, subText, maxStringLength, initialText, sensitive);
        return true;
    }

    if (sensitive)
        swkbdConfigMakePresetPassword(&config);
    else
        swkbdConfigMakePresetDefault(&config);

    // All languages, so Simplified and Traditional Chinese are composed by the
    // system input method; the preset only picks the initial layout.
    swkbdConfigSetType(&config, SwkbdType_All);
    swkbdConfigSetHeaderText(&config, headerText.c_str());
    swkbdConfigSetSubText(&config, subText.c_str());
    swkbdConfigSetStringLenMax(&config, (u32)(maxStringLength < 1 ? 1 : maxStringLength));
    swkbdConfigSetInitialText(&config, initialText.c_str());
    swkbdConfigSetBlurBackground(&config, true);
    swkbdConfigSetKeySetDisableBitmask(&config, getSwkbdKeyDisableBitmask(kbdDisableBitmask));
    if (sensitive) swkbdConfigSetPasswordFlag(&config, 1);

    const std::size_t bufferSize = imeBufferBytes(maxStringLength);
    std::unique_ptr<char[]> buffer(new char[bufferSize]());
    rc = swkbdShow(&config, buffer.get(), bufferSize);
    swkbdClose(&config);

    // The keyboard is gone: give the controller the focus back and drop any
    // button that was held when it opened.
    releasePointerClaims();

    if (R_SUCCEEDED(rc)) {
        // The text itself is never logged, masked or not.
        f(std::string(buffer.get()));
        return true;
    }

    if (rc == SWKBD_USER_CANCEL)
        Logger::info("switch: system keyboard cancelled by the user");
    else
        Logger::error("switch: the system keyboard failed ({:#x})", rc);

    return false;
}

bool SwitchImeManager::openForNumber(std::function<void(long)> f, std::string headerText, std::string subText,
                                     int maxStringLength, std::string initialText, std::string leftButton,
                                     std::string rightButton, int kbdDisableBitmask) {
    releasePointerClaims();

    SwkbdConfig config;
    Result rc = swkbdCreate(&config, 0);
    if (R_FAILED(rc)) {
        Logger::error("switch: the system keyboard did not start ({:#x}), using the in-app text entry", rc);
        openWithUsbKeyboard(
            [f](std::string text) {
                try {
                    if (!text.empty()) f(std::stol(text));
                } catch (const std::exception& error) {
                    Logger::error("switch: not a number: {}", error.what());
                }
            },
            headerText, subText, maxStringLength, initialText, false);
        return true;
    }

    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetType(&config, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&config, headerText.c_str());
    swkbdConfigSetSubText(&config, subText.c_str());
    swkbdConfigSetStringLenMax(&config, (u32)(maxStringLength < 1 ? 1 : maxStringLength));
    swkbdConfigSetInitialText(&config, initialText.c_str());
    swkbdConfigSetLeftOptionalSymbolKey(&config, leftButton.c_str());
    swkbdConfigSetRightOptionalSymbolKey(&config, rightButton.c_str());
    swkbdConfigSetKeySetDisableBitmask(&config, getSwkbdKeyDisableBitmask(kbdDisableBitmask));
    swkbdConfigSetBlurBackground(&config, true);

    const std::size_t bufferSize = imeBufferBytes(maxStringLength);
    std::unique_ptr<char[]> buffer(new char[bufferSize]());
    rc = swkbdShow(&config, buffer.get(), bufferSize);
    swkbdClose(&config);

    releasePointerClaims();

    if (R_SUCCEEDED(rc) && std::strlen(buffer.get()) > 0) {
        try {
            f(std::stol(buffer.get()));
        } catch (const std::exception& error) {
            Logger::error("switch: not a number: {}", error.what());
            return false;
        }
        return true;
    }

    if (R_FAILED(rc) && rc != SWKBD_USER_CANCEL) Logger::error("switch: the system keyboard failed ({:#x})", rc);

    return false;
}

};  // namespace brls
