// Host checks for the Switch 2 input, IME and font decisions.
// Run by ctest as the switch2_input case.
//
// Everything checked here is the pure part of issue #4: the coordinate
// transform that maps raw pointer/touch pixels into the Borealis viewport, the
// L|ZL / R|ZR button-ownership latch, the cursor/input-mode arbitration, the
// system keyboard buffer sizing and the shared-font fallback chain. The device
// side (padPointer*, hidSetNmousePointerRanges, swkbd, pl) is verified on a real
// console through G1.

#include <cstdint>
#include <iostream>
#include <string>

#include "switch2_cjk.hpp"
#include "switch2_pointer.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, int line) {
    if (ok) return;
    std::cerr << "FAIL (line " << line << "): " << what << '\n';
    ++failures;
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected)                                                    \
    check((actual) == (expected),                                                     \
          std::string(#actual) + " == " + std::string(#expected) + ": got " +          \
              std::to_string(actual) + ", want " + std::to_string(expected),           \
          __LINE__)

using namespace wili::switch2;

// The 1920x1080 handheld framebuffer is not special: the same transform has to
// work for the docked modes and for the UI scale settings.
void check_viewport_transform() {
    // 1080p handheld with the default 1280x720 logical size: windowScale 1.5.
    const Viewport hd = makeViewport(1920, 1080, 1.5f);
    CHECK(hd.valid());
    CHECK_EQ((int)hd.logical_width(), 1280);
    CHECK_EQ((int)hd.logical_height(), 720);
    CHECK_EQ((int)toLogical(hd, 0, 0).x, 0);
    CHECK_EQ((int)toLogical(hd, 0, 0).y, 0);
    CHECK_EQ((int)toLogical(hd, 960, 540).x, 640);
    CHECK_EQ((int)toLogical(hd, 960, 540).y, 360);
    CHECK_EQ((int)toLogical(hd, 1919, 1079).x, 1279);
    CHECK_EQ((int)toLogical(hd, 1919, 1079).y, 719);

    // A docked 4K output with the same logical size.
    const Viewport uhd = makeViewport(3840, 2160, 3.0f);
    CHECK_EQ((int)uhd.logical_width(), 1280);
    CHECK_EQ((int)toLogical(uhd, 1920, 1080).x, 640);
    CHECK_EQ((int)toLogical(uhd, 1920, 1080).y, 360);

    // A larger UI scale shrinks the logical area, it does not change the mapping rule.
    const Viewport scaled = makeViewport(1920, 1080, 1.25f);
    CHECK_EQ((int)scaled.logical_width(), 1536);
    CHECK_EQ((int)scaled.logical_height(), 864);
    CHECK_EQ((int)toLogical(scaled, 1250, 1000).x, 1000);
    CHECK_EQ((int)toLogical(scaled, 1250, 1000).y, 800);

    // A non 16:9 docked output keeps the logical width and grows the content
    // height, which is exactly what Application::setWindowSize computes.
    const Viewport wide = makeViewport(2560, 1080, 2.0f);
    CHECK_EQ((int)wide.logical_width(), 1280);
    CHECK_EQ((int)wide.logical_height(), 540);
    CHECK_EQ((int)toLogical(wide, 2560, 1080).x, 1280);
    CHECK_EQ((int)toLogical(wide, 2560, 1080).y, 540);

    // Stale ranges (a dock change that has not been applied yet) must not put
    // the cursor outside the content.
    const LogicalPoint clamped = toLogical(hd, 4000, -20);
    CHECK_EQ((int)clamped.x, 1280);
    CHECK_EQ((int)clamped.y, 0);

    // An unusable viewport yields the origin instead of dividing by zero.
    CHECK(!makeViewport(0, 1080, 1.5f).valid());
    CHECK(!makeViewport(1920, 1080, 0.0f).valid());
    CHECK_EQ((int)toLogical(makeViewport(0, 0, 0.0f), 500, 500).x, 0);
}

// L|ZL on the left Joy-Con and R|ZR on the right Joy-Con are mouse clicks and
// must never reach the gamepad mapping, until the button is really released.
void check_button_claims() {
    // The masks are the hid.h bits, not the Borealis button indices.
    CHECK_EQ(claimMaskForSide(PointerSide::Left), (uint64_t)((1ull << 6) | (1ull << 8)));
    CHECK_EQ(claimMaskForSide(PointerSide::Right), (uint64_t)((1ull << 7) | (1ull << 9)));
    CHECK_EQ(claimMaskForSide(PointerSide::None), (uint64_t)0);
    CHECK(claimMaskForSide(PointerSide::Left) != claimMaskForSide(PointerSide::Right));

    const uint64_t L = 1ull << 6, R = 1ull << 7, ZL = 1ull << 8, ZR = 1ull << 9;
    const uint64_t A = 1ull << 0, Plus = 1ull << 10;

    ButtonClaims claims;
    // Click on the left Joy-Con: L and unrelated buttons.
    CHECK_EQ(claims.apply(L | A, claimMaskForSide(PointerSide::Left)), A);
    CHECK_EQ(claims.held(), L);
    // The claim stays while L is still held, even if the pointer stops reporting.
    CHECK_EQ(claims.apply(L | A, 0), A);
    CHECK_EQ(claims.held(), L);
    // Releasing L frees it: the next press is an ordinary gamepad L again.
    CHECK_EQ(claims.apply(A, 0), A);
    CHECK_EQ(claims.held(), (uint64_t)0);
    CHECK_EQ(claims.apply(L | A, 0), L | A);

    // Both sides can own their own shoulder buttons at the same time.
    ButtonClaims zl;
    CHECK_EQ(zl.apply(ZL | R, claimMaskForSide(PointerSide::Left)), R);
    CHECK_EQ(zl.held(), ZL);
    CHECK_EQ(zl.apply(ZL | R, claimMaskForSide(PointerSide::Right)), (uint64_t)0);
    CHECK_EQ(zl.held(), ZL | R);

    // A right Joy-Con click must not swallow the left Joy-Con buttons used for
    // gameplay at the same time.
    ButtonClaims right;
    CHECK_EQ(right.apply(R | ZR | L, claimMaskForSide(PointerSide::Right)), L);
    CHECK_EQ(right.held(), R | ZR);

    // No pointer side means no ownership at all.
    ButtonClaims idle;
    CHECK_EQ(idle.apply(L | R | ZL | ZR, claimMaskForSide(PointerSide::None)), L | R | ZL | ZR);
    CHECK_EQ(idle.held(), (uint64_t)0);

    // Unrelated buttons are never claimed and focus loss clears everything.
    ButtonClaims other;
    CHECK_EQ(other.apply(Plus, claimMaskForSide(PointerSide::Left)), Plus);
    CHECK_EQ(other.apply(L, claimMaskForSide(PointerSide::Left)), (uint64_t)0);
    other.reset();
    CHECK_EQ(other.held(), (uint64_t)0);
    CHECK_EQ(other.apply(L, 0), L);
}

// The cursor belongs to the mouse, hides when idle and is never hidden mid-drag.
void check_cursor_arbitration() {
    CursorArbiter cursor;
    CHECK(!cursor.visible(0));
    CHECK(cursor.source() == PointerSource::None);

    // Movement takes the cursor and keeps it for the hide delay.
    cursor.onPointerSample(1000, true, false);
    CHECK(cursor.source() == PointerSource::Mouse);
    CHECK(cursor.visible(1000));
    CHECK(cursor.visible(1000 + CursorArbiter::HIDE_DELAY_MS));
    CHECK(!cursor.visible(1000 + CursorArbiter::HIDE_DELAY_MS + 1));

    // More movement refreshes the delay.
    cursor.onPointerSample(2000, true, false);
    CHECK(cursor.visible(2000 + CursorArbiter::HIDE_DELAY_MS));

    // A click without movement is still pointer activity.
    cursor.onPointerSample(3000, false, true);
    CHECK(cursor.isDragging());
    CHECK(cursor.visible(3000));

    // A drag never hides, no matter how long it lasts.
    cursor.onPointerSample(4000, false, true);
    CHECK(cursor.visible(4000 + 10 * CursorArbiter::HIDE_DELAY_MS));

    // Neither touch nor a gamepad button may interrupt the drag.
    cursor.onTouch();
    cursor.onGamepad();
    CHECK(cursor.source() == PointerSource::Mouse);
    CHECK(cursor.visible(4300));

    // Releasing the drag ends the pointer session after the delay.
    cursor.onPointerSample(5000, false, false);
    CHECK(!cursor.isDragging());
    CHECK(cursor.visible(5000));
    CHECK(!cursor.visible(5000 + CursorArbiter::HIDE_DELAY_MS + 1));

    // Touch and controller take over the interface when nothing is dragged.
    cursor.onTouch();
    CHECK(cursor.source() == PointerSource::Touch);
    CHECK(!cursor.visible(6000));
    cursor.onPointerSample(7000, true, false);
    CHECK(cursor.source() == PointerSource::Mouse);
    cursor.onGamepad();
    CHECK(cursor.source() == PointerSource::Gamepad);
    CHECK(!cursor.visible(8000));

    // An idle pointer sample must not steal the interface back from the gamepad.
    cursor.onPointerSample(9000, false, false);
    CHECK(cursor.source() == PointerSource::Gamepad);
    CHECK(!cursor.visible(9000));

    // Focus loss drops the drag and the latched state; the next sample recovers.
    cursor.onPointerSample(10000, false, true);
    cursor.onFocusLost();
    CHECK(cursor.source() == PointerSource::None);
    CHECK(!cursor.isDragging());
    CHECK(!cursor.visible(10000));
    cursor.onPointerSample(11000, true, false);
    CHECK(cursor.visible(11000));
}

// The stick wheel is a vertical scroll; the up axis is +y in Borealis.
void check_scroll_mapping() {
    CHECK_EQ(toScroll(0, 0).x, 0.0f);
    CHECK_EQ(toScroll(0, 0).y, 0.0f);
    CHECK_EQ(toScroll(0, 1).y, SCROLL_PIXELS_PER_STEP);
    CHECK_EQ(toScroll(0, 1).x, 0.0f);
    CHECK_EQ(toScroll(0, -3).y, -3 * SCROLL_PIXELS_PER_STEP);
    CHECK_EQ(toScroll(2, 0).x, 2 * SCROLL_PIXELS_PER_STEP);
    // A single notch must actually move a list, not round away to nothing.
    CHECK(toScroll(0, 1).y >= 1.0f);
}

// The system keyboard buffer must fit the requested characters as UTF-8, and a
// password request must be recognisable, and nothing else.
void check_ime_requests() {
    // 32 characters of Chinese take at most 128 bytes plus the NUL.
    CHECK(imeBufferBytes(32) >= 32 * 3 + 1);
    CHECK_EQ(imeBufferBytes(32), (std::size_t)129);
    CHECK_EQ(imeBufferBytes(512), (std::size_t)2049);
    // A zero or negative request still needs a usable buffer.
    CHECK_EQ(imeBufferBytes(0), (std::size_t)5);
    CHECK_EQ(imeBufferBytes(-3), (std::size_t)5);

    // Masking is driven by the one caller signal Borealis has: no spaces.
    CHECK(imeIsSensitive(KEYBOARD_DISABLE_SPACE_MASK));
    CHECK(!imeIsSensitive(0));
    CHECK(!imeIsSensitive(2));  // '@' only
    CHECK(!imeIsSensitive(4));  // '%' only
    CHECK(!imeIsSensitive(8));  // '/' only
    CHECK(imeIsSensitive(KEYBOARD_DISABLE_SPACE_MASK | 2 | 4));
}

// Mixed CJK strings are laid out in the regular font, so the regular font needs
// the whole CJK chain too, and every script the client can show is present.
void check_font_fallbacks() {
    const std::vector<FallbackFont>& chain = sharedFontFallbacks();
    CHECK(!chain.empty());

    bool has_hans = false, has_hans_ext = false, has_hant = false, has_ko = false;
    bool has_icons = false, has_material = false, has_emoji = false;
    for (const FallbackFont& font : chain)
    {
        CHECK(font.name != nullptr && font.name[0] != '\0');
        const std::string name = font.name;
        has_hans |= name == FONT_NAME_CHINESE_SIMPLIFIED;
        has_hans_ext |= name == FONT_NAME_CHINESE_SIMPLIFIED_EXT;
        has_hant |= name == FONT_NAME_CHINESE_TRADITIONAL;
        has_ko |= name == FONT_NAME_KOREAN;
        has_icons |= name == FONT_NAME_SWITCH_ICONS;
        has_material |= name == FONT_NAME_MATERIAL_ICONS;
        has_emoji |= name == FONT_NAME_EMOJI;
    }
    CHECK(has_hans);
    CHECK(has_hans_ext);
    CHECK(has_hant);
    CHECK(has_ko);
    CHECK(has_icons);
    CHECK(has_material);
    CHECK(has_emoji);

    // Simplified Chinese is tried first, the font names are unique, and the
    // Nintendo symbol font comes before the decorative ones.
    CHECK(std::string(chain.front().name) == std::string(FONT_NAME_CHINESE_SIMPLIFIED));
    for (std::size_t i = 0; i < chain.size(); ++i)
        for (std::size_t j = i + 1; j < chain.size(); ++j)
            CHECK(std::string(chain[i].name) != std::string(chain[j].name));

    std::size_t icons = chain.size(), material = chain.size(), emoji = chain.size();
    for (std::size_t i = 0; i < chain.size(); ++i)
    {
        const std::string name = chain[i].name;
        if (name == FONT_NAME_SWITCH_ICONS) icons = i;
        if (name == FONT_NAME_MATERIAL_ICONS) material = i;
        if (name == FONT_NAME_EMOJI) emoji = i;
    }
    CHECK(icons < material);
    CHECK(material < emoji);

    // Every entry names a valid system font type; the CJK and symbol fonts come
    // from the system at runtime, the icons from the app, the emoji from the
    // user's config directory.
    for (const FallbackFont& font : chain)
    {
        const int type = (int)font.shared;
        CHECK(type >= (int)SharedFont::Standard && type <= (int)SharedFont::NintendoExt);

        const std::string name = font.name;
        const bool cjk = name == FONT_NAME_CHINESE_SIMPLIFIED || name == FONT_NAME_CHINESE_SIMPLIFIED_EXT ||
                         name == FONT_NAME_CHINESE_TRADITIONAL || name == FONT_NAME_KOREAN ||
                         name == FONT_NAME_SWITCH_ICONS;
        if (cjk)
            CHECK_EQ((int)FontOrigin::Shared, (int)font.origin);
        else if (name == FONT_NAME_MATERIAL_ICONS)
            CHECK_EQ((int)FontOrigin::Resources, (int)font.origin);
        else
            CHECK_EQ((int)FontOrigin::User, (int)font.origin);
    }
}

// The in-app fallback turns USB keyboard keys into UTF-8 directly.
void check_keyboard_fallback() {
    // brls::BrlsKeyboardScancode printable values are the ASCII code.
    CHECK(keyboardKeyUtf8('A', false) == "a");
    CHECK(keyboardKeyUtf8('A', true) == "A");
    CHECK(keyboardKeyUtf8('Z', false) == "z");
    CHECK(keyboardKeyUtf8('7', false) == "7");
    CHECK(keyboardKeyUtf8('7', true) == "&");
    CHECK(keyboardKeyUtf8('0', true) == ")");
    CHECK(keyboardKeyUtf8(' ', false) == " ");
    CHECK(keyboardKeyUtf8('\'', true) == "\"");
    CHECK(keyboardKeyUtf8('-', true) == "_");
    CHECK(keyboardKeyUtf8('[', true) == "{");
    CHECK(keyboardKeyUtf8('\\', true) == "|");
    // Keys that do not produce text, and the non-ASCII brls codes (257 = ENTER).
    CHECK(keyboardKeyUtf8(257, false).empty());
    CHECK(keyboardKeyUtf8(262, false).empty());
    CHECK(keyboardKeyUtf8(0, false).empty());
}

}  // namespace

int main() {
    check_viewport_transform();
    check_button_claims();
    check_cursor_arbitration();
    check_scroll_mapping();
    check_ime_requests();
    check_font_fallbacks();
    check_keyboard_fallback();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "switch2 input checks passed\n";
    return 0;
}
