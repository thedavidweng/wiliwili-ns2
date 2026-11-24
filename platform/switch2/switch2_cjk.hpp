// Pure Switch 2 text decisions: the system keyboard buffer, the sensitive-input
// rule and the shared-font fallback chain. Shared by the Borealis overrides and
// the host tests, so nothing here may include <switch.h>.
//
// The numeric shared-font types are PlSharedFontType from switch/services/pl.h;
// switch_font.cpp asserts them against the real header.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wili::switch2
{

// The system keyboard counts characters, UTF-8 needs up to four bytes each, and
// the terminating NUL has to fit as well. A 0x100 buffer silently truncated
// long Chinese input.
inline constexpr std::size_t IME_BYTES_PER_CHARACTER = 4;

inline std::size_t imeBufferBytes(int max_string_length)
{
    const std::size_t characters = max_string_length < 1 ? 1 : (std::size_t)max_string_length;
    return characters * IME_BYTES_PER_CHARACTER + 1;
}

// brls::KEYBOARD_DISABLE_SPACE. The Borealis ImeManager has no separate password
// entry point, so the one caller signal that identifies a password or token
// field is "this text may not contain spaces": that mask both hides the input in
// the system keyboard and forbids the space key.
inline constexpr int KEYBOARD_DISABLE_SPACE_MASK = 1;

inline bool imeIsSensitive(int kbd_disable_bitmask)
{
    return (kbd_disable_bitmask & KEYBOARD_DISABLE_SPACE_MASK) != 0;
}

// UTF-8 for one USB/Bluetooth keyboard key, used by the in-app fallback when the
// system keyboard cannot start. key is a brls::BrlsKeyboardScancode, whose
// printable values are the character's ASCII code. Empty when the key does not
// produce text (arrows, modifiers, unknown).
inline std::string keyboardKeyUtf8(int key, bool shift)
{
    if (key >= 'A' && key <= 'Z')
        return std::string(1, (char)(shift ? key : key - 'A' + 'a'));
    if (key >= '0' && key <= '9')
    {
        static const char* shifted = ")!@#$%^&*(";
        return std::string(1, shift ? shifted[key - '0'] : (char)key);
    }

    switch (key)
    {
        case ' ':
            return " ";
        case '\'':
            return shift ? "\"" : "'";
        case ',':
            return shift ? "<" : ",";
        case '-':
            return shift ? "_" : "-";
        case '.':
            return shift ? ">" : ".";
        case '/':
            return shift ? "?" : "/";
        case ';':
            return shift ? ":" : ";";
        case '=':
            return shift ? "+" : "=";
        case '[':
            return shift ? "{" : "[";
        case '\\':
            return shift ? "|" : "\\";
        case ']':
            return shift ? "}" : "]";
        case '`':
            return shift ? "~" : "`";
        default:
            return {};
    }
}

// PlSharedFontType.
enum class SharedFont : int
{
    Standard             = 0, // Japan, US and Europe
    ChineseSimplified    = 1,
    ExtChineseSimplified = 2,
    ChineseTraditional   = 3,
    Korean               = 4,
    NintendoExt          = 5, // Nintendo specific symbols
};

// Borealis font names, see borealis/core/font.hpp.
inline constexpr const char* FONT_NAME_CHINESE_SIMPLIFIED     = "zh-Hans";
inline constexpr const char* FONT_NAME_CHINESE_SIMPLIFIED_EXT = "zh-Hans-ext";
inline constexpr const char* FONT_NAME_CHINESE_TRADITIONAL    = "zh-Hant";
inline constexpr const char* FONT_NAME_KOREAN                 = "korean";
inline constexpr const char* FONT_NAME_SWITCH_ICONS           = "switch";
inline constexpr const char* FONT_NAME_MATERIAL_ICONS         = "material";
inline constexpr const char* FONT_NAME_EMOJI                  = "emoji";

enum class FontOrigin : int
{
    Shared,    // runtime shared font (pl), never redistributed
    Resources, // font shipped inside the application resources
    User,      // font the user dropped into the config directory
};

struct FallbackFont
{
    const char* name;
    FontOrigin origin;
    SharedFont shared; // only meaningful for FontOrigin::Shared
};

// The fallback chain from the most to the least likely glyph source. Every base
// font (FONT_REGULAR and FONT_CHINESE_SIMPLIFIED alike) gets this chain: mixed
// Latin + CJK strings are laid out in FONT_REGULAR, so a chain hung off the
// Chinese base font alone still renders tofu.
inline const std::vector<FallbackFont>& sharedFontFallbacks()
{
    static const std::vector<FallbackFont> chain = {
        { FONT_NAME_CHINESE_SIMPLIFIED, FontOrigin::Shared, SharedFont::ChineseSimplified },
        { FONT_NAME_CHINESE_SIMPLIFIED_EXT, FontOrigin::Shared, SharedFont::ExtChineseSimplified },
        { FONT_NAME_CHINESE_TRADITIONAL, FontOrigin::Shared, SharedFont::ChineseTraditional },
        { FONT_NAME_KOREAN, FontOrigin::Shared, SharedFont::Korean },
        { FONT_NAME_SWITCH_ICONS, FontOrigin::Shared, SharedFont::NintendoExt },
        { FONT_NAME_MATERIAL_ICONS, FontOrigin::Resources, SharedFont::Standard },
        { FONT_NAME_EMOJI, FontOrigin::User, SharedFont::Standard },
    };
    return chain;
}

} // namespace wili::switch2
