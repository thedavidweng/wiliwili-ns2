/*
    Copyright 2019 p-sam
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

// Switch 2 fonts. Replaces Borealis' switch/switch_font.cpp through the
// HEADER_FILE_ONLY override in library/CMakeLists.txt.
//
// Differences from the upstream file:
// - the CJK fallback chain is applied to FONT_REGULAR as well as
//   FONT_CHINESE_SIMPLIFIED. On Switch the default font is "zh-Hans", but the
//   regular font still lays out strings that mix Latin and CJK, which rendered
//   tofu because it had no fallbacks at all;
// - a user font keeps the chain instead of losing CJK and emoji coverage;
// - nothing is bundled: the glyphs come from the pl service at runtime, and when
//   pl is unavailable the licensed open CJK font the user drops into the config
//   directory is the only fallback, which is reported in boot.log.

#include <unistd.h>

#include <switch.h>

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/platforms/switch/switch_font.hpp>

#include <switch2_cjk.hpp>

// The pure chain cannot include <switch.h>, so it restates the shared-font types.
static_assert((int)wili::switch2::SharedFont::Standard == (int)PlSharedFontType_Standard, "shared font standard");
static_assert((int)wili::switch2::SharedFont::ChineseSimplified == (int)PlSharedFontType_ChineseSimplified, "shared font zh-Hans");
static_assert((int)wili::switch2::SharedFont::ExtChineseSimplified == (int)PlSharedFontType_ExtChineseSimplified, "shared font zh-Hans-ext");
static_assert((int)wili::switch2::SharedFont::ChineseTraditional == (int)PlSharedFontType_ChineseTraditional, "shared font zh-Hant");
static_assert((int)wili::switch2::SharedFont::Korean == (int)PlSharedFontType_KO, "shared font korean");
static_assert((int)wili::switch2::SharedFont::NintendoExt == (int)PlSharedFontType_NintendoExt, "shared font switch icons");

namespace brls
{

using namespace wili::switch2;

namespace
{

// Loads one shared font under the Borealis name the chain uses.
bool loadSharedFont(PlSharedFontType type, const std::string& name)
{
    PlFontData font;
    const Result rc = plGetSharedFontByType(&font, type);
    if (R_FAILED(rc))
    {
        Logger::error("switch: shared font {} unavailable ({:#x})", name, rc);
        return false;
    }

    // false: the memory belongs to the pl service, nanovg must not free it.
    if (!Application::loadFontFromMemory(name, font.address, font.size, false))
    {
        Logger::error("switch: shared font {} did not load", name);
        return false;
    }
    return true;
}

void addFallback(NVGcontext* vg, int base, const std::string& name)
{
    const int fallback = Application::getFont(name);
    if (fallback == FONT_INVALID || fallback == base)
        return;
    nvgAddFallbackFontId(vg, base, fallback);
}

// Gives one base font the whole chain, so mixed Latin/CJK/Korean/icons/emoji
// strings render from it.
void applyFallbackChain(NVGcontext* vg, int base, const std::string& latinBase)
{
    if (base == FONT_INVALID)
        return;

    for (const FallbackFont& font : sharedFontFallbacks())
        addFallback(vg, base, font.name);

    // The Latin base is the last resort for ASCII punctuation the CJK fonts miss.
    addFallback(vg, base, latinBase);
}

} // namespace

void SwitchFontLoader::loadFonts()
{
    NVGcontext* vg = Application::getNVGContext();

    Logger::info("switch system locale: {}", Application::getPlatform()->getLocale());

    const bool has_user_font = access(USER_FONT_PATH.c_str(), F_OK) != -1;
    if (!has_user_font)
        Logger::warning("No custom font found, (searched at: {})", USER_FONT_PATH);

    // The chain is loaded once and then referenced by both base fonts.
    for (const FallbackFont& font : sharedFontFallbacks())
    {
        switch (font.origin)
        {
            case FontOrigin::Shared:
                loadSharedFont((PlSharedFontType)font.shared, font.name);
                break;
            case FontOrigin::Resources:
                if (!this->loadMaterialFromResources())
                    Logger::error("switch: could not load the {} font from the resources", font.name);
                break;
            case FontOrigin::User:
                if (!USER_EMOJI_PATH.empty() && access(USER_EMOJI_PATH.c_str(), F_OK) != -1)
                    this->loadFontFromFile(font.name, USER_EMOJI_PATH);
                break;
        }
    }

    // "zh-Hans" is the default font on Switch, so it is the one that must carry
    // Simplified and Traditional Chinese, Korean, the icons and the emoji.
    if (has_user_font)
    {
        // A user font replaces the shared Simplified Chinese font, which is what
        // themes and the app dialogs do today.
        this->loadFontFromFile(FONT_CHINESE_SIMPLIFIED, USER_FONT_PATH);
        loadSharedFont(PlSharedFontType_Standard, FONT_REGULAR);
    }
    else if (!loadSharedFont(PlSharedFontType_ChineseSimplified, FONT_CHINESE_SIMPLIFIED))
    {
        // The shared fonts unusable: a licensed open CJK font (Noto Sans CJK,
        // Source Han Sans, ...) in the config directory is the only path left.
        Logger::error("switch: no shared font is available. Drop an open CJK font such as Noto Sans CJK "
                      "at {} to render Chinese text.",
            USER_FONT_PATH);
    }
    else
    {
        loadSharedFont(PlSharedFontType_Standard, FONT_REGULAR);
    }

    applyFallbackChain(vg, Application::getFont(FONT_CHINESE_SIMPLIFIED), FONT_REGULAR);
    applyFallbackChain(vg, Application::getFont(FONT_REGULAR), FONT_CHINESE_SIMPLIFIED);

    Logger::info("switch: fonts ready (regular {}, zh-Hans {})", Application::getFont(FONT_REGULAR) != FONT_INVALID,
        Application::getFont(FONT_CHINESE_SIMPLIFIED) != FONT_INVALID);
}

} // namespace brls
