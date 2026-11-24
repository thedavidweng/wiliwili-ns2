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
// - a request that forbids spaces is treated as sensitive: the keyboard masks
//   the text, and neither the returned text nor the initial text is logged;
// - a user cancel (LibAppletBadExit) is told apart from the keyboard being
//   unavailable at runtime, and the unavailable case falls back to the in-app
//   EditTextDialog fed by a USB/Bluetooth keyboard;
// - the pointer claims are released around the keyboard and the controller gets
//   the focus back afterwards.

#include <switch.h>

#include <cstring>
#include <memory>

#include <borealis/core/application.hpp>
#include <borealis/core/input.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/platforms/switch/switch_ime.hpp>
#include <borealis/views/edit_text_dialog.hpp>

#include <switch2_cjk.hpp>
#include <switch2_pointer_device.hpp>

static_assert(wili::switch2::KEYBOARD_DISABLE_SPACE_MASK == (int)brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_SPACE,
    "the sensitive-input rule must follow the Borealis space mask");

namespace brls
{

using namespace wili::switch2;

namespace
{

// libnx reports a user cancel as an applet that closed without a result.
constexpr Result SWKBD_USER_CANCEL = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);

int getSwkbdKeyDisableBitmask(int borealis_key)
{
    // translate brls to Switch libnx values
    int ret = 0;
    if (borealis_key == brls::KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE)
        return 0;

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

// The in-app text entry used when the system keyboard cannot start. It is the
// Borealis EditTextDialog (Apache-2.0, no bundled assets) driven by the USB or
// Bluetooth keyboard, so text entry keeps working with a real UTF-8 buffer.
// Composing Chinese here needs an input method on the keyboard itself; the
// system keyboard is the composition path and this is only the last resort.
void openWithUsbKeyboard(std::function<void(std::string)> callback, const std::string& headerText,
    const std::string& subText, int maxStringLength, const std::string& initialText, bool sensitive)
{
    EditTextDialog* dialog = new EditTextDialog();
    auto buffer            = std::make_shared<std::string>(initialText);
    const std::size_t characterLimit = maxStringLength < 1 ? 1 : (std::size_t)maxStringLength;

    auto updateUI = [dialog, buffer, characterLimit, sensitive]()
    {
        const std::string shown = sensitive ? std::string(buffer->size(), '*') : *buffer;
        dialog->setText(shown);
        dialog->setCountText(fmt::format("{}/{}", buffer->size(), characterLimit));
    };

    dialog->setHeaderText(headerText);
    dialog->setHintText(subText);
    updateUI();

    // One UTF-8 sequence per key press from the standard keyboard. The dialog
    // closes itself on A (submit) and B (cancel), which fire the events below;
    // the subscription must go away with it.
    InputManager* input = Application::getPlatform()->getInputManager();
    auto subscription   = std::make_shared<Event<KeyState>::Subscription>();
    auto unsubscribe    = [input, subscription]()
    { input->getKeyboardKeyStateChanged()->unsubscribe(*subscription); };

    *subscription = input->getKeyboardKeyStateChanged()->subscribe(
        [buffer, characterLimit, updateUI](KeyState state)
        {
            if (!state.pressed || (state.mods & BRLS_KBD_MODIFIER_CTRL) || (state.mods & BRLS_KBD_MODIFIER_META))
                return;

            const std::string text = keyboardKeyUtf8((int)state.key, (state.mods & BRLS_KBD_MODIFIER_SHIFT) != 0);
            if (text.empty() || buffer->size() + text.size() > characterLimit)
                return;

            *buffer += text;
            updateUI();
        });

    dialog->getBackspaceEvent()->subscribe(
        [buffer, updateUI]()
        {
            if (buffer->empty())
                return;
            // Drop a whole UTF-8 sequence, not a byte.
            std::size_t begin = buffer->size() - 1;
            while (begin > 0 && ((*buffer)[begin] & 0xC0) == 0x80)
                --begin;
            buffer->erase(begin);
            updateUI();
        });

    dialog->getSubmitEvent()->subscribe(
        [callback, buffer, unsubscribe]()
        {
            unsubscribe();
            const std::string text = *buffer;
            callback(text);
        });
    dialog->getCancelEvent()->subscribe([unsubscribe]() { unsubscribe(); });

    // Same contract as the SDL/GLFW backends: text entry is in progress and the
    // callback decides when it is done.
    dialog->open();
}

} // namespace

bool SwitchImeManager::openForText(std::function<void(std::string)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText,
    int kbdDisableBitmask)
{
    const bool sensitive = imeIsSensitive(kbdDisableBitmask);

    // A click must not stay latched while the system keyboard is in front.
    releasePointerClaims();

    SwkbdConfig config;
    Result rc = swkbdCreate(&config, 0);
    if (R_FAILED(rc))
    {
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
    if (sensitive)
        swkbdConfigSetPasswordFlag(&config, 1);

    const std::size_t bufferSize = imeBufferBytes(maxStringLength);
    std::unique_ptr<char[]> buffer(new char[bufferSize]());
    rc = swkbdShow(&config, buffer.get(), bufferSize);
    swkbdClose(&config);

    // The keyboard is gone: give the controller the focus back and drop any
    // button that was held when it opened.
    releasePointerClaims();

    if (R_SUCCEEDED(rc))
    {
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

bool SwitchImeManager::openForNumber(std::function<void(long)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText,
    std::string leftButton, std::string rightButton,
    int kbdDisableBitmask)
{
    releasePointerClaims();

    SwkbdConfig config;
    Result rc = swkbdCreate(&config, 0);
    if (R_FAILED(rc))
    {
        Logger::error("switch: the system keyboard did not start ({:#x}), using the in-app text entry", rc);
        openWithUsbKeyboard([f](std::string text)
            {
                try
                {
                    if (!text.empty())
                        f(std::stol(text));
                }
                catch (const std::exception& error)
                {
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

    if (R_SUCCEEDED(rc) && std::strlen(buffer.get()) > 0)
    {
        try
        {
            f(std::stol(buffer.get()));
        }
        catch (const std::exception& error)
        {
            Logger::error("switch: not a number: {}", error.what());
            return false;
        }
        return true;
    }

    if (R_FAILED(rc) && rc != SWKBD_USER_CANCEL)
        Logger::error("switch: the system keyboard failed ({:#x})", rc);

    return false;
}

}; // namespace brls
