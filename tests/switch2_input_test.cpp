// Host checks for the Switch 2 input, IME and font decisions.
// Run by ctest as the switch2_input case.
//
// Everything checked here is the pure part of issue #4: the coordinate
// transforms that map raw pointer and touch pixels into the Borealis viewport,
// the L|ZL / R|ZR button-ownership latch (including the multi-pad case), the
// cursor and hover arbitration, the system keyboard buffer sizing and masking,
// the Pinyin fallback that keeps Chinese text entry possible without swkbd, and
// the shared-font fallback chain. The device side (padPointer*,
// hidSetTouchScreenOutputRanges, swkbd, pl) is verified on a real console
// through gate G1.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "switch2_cjk.hpp"
#include "switch2_pointer.hpp"
#include "test_check.hpp"

using namespace wili::switch2;

namespace {

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

// Touch has its own domain. The handheld digitizer does not change resolution
// when the console is docked, so it must never be mapped through the pointer's
// framebuffer scale.
void check_touch_viewport() {
    // hidSetTouchScreenOutputRanges decides the domain at startup; Horizon's own
    // default stays in force when the runtime refuses the larger range.
    const TouchDomain ns2 = touchDomain(true);
    CHECK_EQ(ns2.width, TOUCH_OUTPUT_WIDTH);
    CHECK_EQ(ns2.height, TOUCH_OUTPUT_HEIGHT);
    const TouchDomain horizon = touchDomain(false);
    CHECK_EQ(horizon.width, TOUCH_DEFAULT_WIDTH);
    CHECK_EQ(horizon.height, TOUCH_DEFAULT_HEIGHT);
    CHECK_EQ(TOUCH_DEFAULT_WIDTH, 1280);
    CHECK_EQ(TOUCH_DEFAULT_HEIGHT, 720);

    // 1920x1080 digitizer over the 1280x720 logical layout.
    const Viewport touch = makeTouchViewport(ns2, 1280.0f, 720.0f);
    CHECK(touch.valid());
    CHECK_EQ((int)touch.logical_width(), 1280);
    CHECK_EQ((int)touch.logical_height(), 720);
    CHECK_EQ((int)toLogical(touch, 0, 0).x, 0);
    CHECK_EQ((int)toLogical(touch, 0, 0).y, 0);
    CHECK_EQ((int)toLogical(touch, 960, 540).x, 640);
    CHECK_EQ((int)toLogical(touch, 960, 540).y, 360);
    CHECK_EQ((int)toLogical(touch, 1919, 1079).x, 1279);
    CHECK_EQ((int)toLogical(touch, 1919, 1079).y, 719);

    // The 1280x720 fallback domain covers the same layout at 1:1.
    const Viewport fallback = makeTouchViewport(horizon, 1280.0f, 720.0f);
    CHECK_EQ((int)fallback.logical_width(), 1280);
    CHECK_EQ((int)fallback.logical_height(), 720);
    CHECK_EQ((int)toLogical(fallback, 640, 360).x, 640);
    CHECK_EQ((int)toLogical(fallback, 640, 360).y, 360);
    CHECK_EQ((int)toLogical(fallback, 1280, 720).x, 1280);
    CHECK_EQ((int)toLogical(fallback, 1280, 720).y, 720);

    // A docked framebuffer is twice or four times the digitizer and must not move
    // a touch: the same physical point lands on the same logical one.
    const Viewport docked4k = makeViewport(3840, 2160, 3.0f);
    CHECK_EQ((int)toLogical(docked4k, 1920, 1080).x, 640);
    CHECK_EQ((int)toLogical(docked4k, 1920, 1080).y, 360);
    CHECK_EQ((int)toLogical(touch, 960, 540).x, 640);
    CHECK_EQ((int)toLogical(touch, 960, 540).y, 360);

    // The UI scale changes the logical layout, not the digitizer.
    const Viewport scaled = makeTouchViewport(ns2, 1536.0f, 864.0f);
    CHECK_EQ((int)scaled.logical_width(), 1536);
    CHECK_EQ((int)scaled.logical_height(), 864);
    CHECK_EQ((int)toLogical(scaled, 960, 540).x, 768);
    CHECK_EQ((int)toLogical(scaled, 960, 540).y, 432);

    // Unusable input yields the origin, never a division by zero.
    CHECK(!makeTouchViewport({0, 1080}, 1280.0f, 720.0f).valid());
    CHECK(!makeTouchViewport({1920, 0}, 1280.0f, 720.0f).valid());
    CHECK(!makeTouchViewport(ns2, 0.0f, 720.0f).valid());
    CHECK(!makeTouchViewport(ns2, 1280.0f, 0.0f).valid());
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

    // One pointer pad: update() runs once per frame, strip() once for that pad.
    ButtonClaims claims;
    // Click on the left Joy-Con: L and unrelated buttons.
    CHECK_EQ(claims.update(L | A, claimMaskForSide(PointerSide::Left)), L);
    CHECK_EQ(claims.strip(L | A, true), A);
    // The claim stays while L is still held, even if the pointer stops reporting.
    CHECK_EQ(claims.update(L | A, 0), L);
    CHECK_EQ(claims.strip(L | A, true), A);
    // Releasing L frees it: the next press is an ordinary gamepad L again.
    CHECK_EQ(claims.update(A, 0), (uint64_t)0);
    CHECK_EQ(claims.strip(A, true), A);
    CHECK_EQ(claims.strip(L | A, true), L | A);

    // Both sides can own their own shoulder buttons at the same time.
    ButtonClaims zl;
    CHECK_EQ(zl.update(ZL | R, claimMaskForSide(PointerSide::Left)), ZL);
    CHECK_EQ(zl.strip(ZL | R, true), R);
    // A right Joy-Con click must not swallow the left Joy-Con buttons used for
    // gameplay at the same time.
    CHECK_EQ(zl.update(ZL | R, claimMaskForSide(PointerSide::Right)), ZL | R);
    CHECK_EQ(zl.strip(ZL | R | L, true), L);

    // No pointer side means no ownership at all.
    ButtonClaims idle;
    CHECK_EQ(idle.update(L | R | ZL | ZR, claimMaskForSide(PointerSide::None)), (uint64_t)0);
    CHECK_EQ(idle.strip(L | R | ZL | ZR, true), L | R | ZL | ZR);

    // Unrelated buttons are never claimed and focus loss clears everything.
    ButtonClaims other;
    CHECK_EQ(other.update(Plus, claimMaskForSide(PointerSide::Left)), (uint64_t)0);
    CHECK_EQ(other.strip(Plus, true), Plus);
    CHECK_EQ(other.update(L, claimMaskForSide(PointerSide::Left)), L);
    other.reset();
    CHECK_EQ(other.held(), (uint64_t)0);
    CHECK_EQ(other.strip(L, true), L);
}

// Regression: a second pad must not be able to clear the pointer's claim, and it
// keeps its own shoulder buttons. Two controllers are normally present (the
// handheld pair plus an external pad), and the claim used to be applied to every
// pad in the frame, so an idle pad's empty button set wiped it.
void check_multi_pad_claims() {
    const uint64_t R = 1ull << 7, ZR = 1ull << 9;

    ButtonClaims claims;

    // Frame 1: the external pad drives PadPointer and holds R as the click; the
    // handheld pad reports no buttons at all.
    claims.update(R, claimMaskForSide(PointerSide::Right));
    CHECK_EQ(claims.held(), R);
    CHECK_EQ(claims.strip(R, true), (uint64_t)0);  // pointer pad: no gamepad R
    CHECK_EQ(claims.strip(0, false), (uint64_t)0);

    // Frame 2: the pointer stops reporting the click but R is still down, and the
    // idle pad still reports nothing. The claim has to survive both.
    claims.update(R, 0);
    CHECK_EQ(claims.held(), R);
    CHECK_EQ(claims.strip(R, true), (uint64_t)0);
    CHECK_EQ(claims.strip(0, false), (uint64_t)0);

    // Frame 3: the handheld pad has its own ZR down; the pointer pad still holds
    // the claimed R. A pad that does not own the pointer keeps its buttons.
    claims.update(R, 0);
    CHECK_EQ(claims.strip(ZR, false), ZR);
    CHECK_EQ(claims.strip(R, true), (uint64_t)0);

    // Frame 4: R is released, so the next press is ordinary gamepad input again.
    claims.update(0, 0);
    CHECK_EQ(claims.held(), (uint64_t)0);
    CHECK_EQ(claims.strip(R, true), R);
    CHECK_EQ(claims.strip(ZR, false), ZR);
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

// The cursor frames the control under it. Borealis clears MouseState::view while
// no button is active, so this is the input manager's own decision, and it is
// only ever drawn (never focused).
void check_hover_feedback() {
    HoverState state;
    CHECK(!showHoverFrame(state));  // nothing under the cursor yet

    state.cursor_visible = true;
    CHECK(!showHoverFrame(state));  // no target: null, hidden or non focusable
    state.has_target = true;
    CHECK(showHoverFrame(state));

    // A click drag owns the view; a frame around it would be noise.
    state.dragging = true;
    CHECK(!showHoverFrame(state));
    state.dragging = false;

    // The gamepad already frames the focused view, so the cursor does not.
    state.target_is_focus = true;
    state.gamepad_input   = true;
    CHECK(!showHoverFrame(state));

    // With the mouse driving the interface that focus frame is not drawn, so the
    // hover frame is the only highlight.
    state.gamepad_input = false;
    CHECK(showHoverFrame(state));

    // A hidden cursor never draws anything.
    state.cursor_visible = false;
    CHECK(!showHoverFrame(state));
}

// Regression for real-console Joy-Con 2 mouse mode:
// 1. An idle sample on boot must not wake the cursor, while either hardware
//    deltas or same-side x/y coordinate changes wake it cleanly.
// 2. While the cursor is visible on one Joy-Con, that side's stick scroll
//    directions and rail SL/SR buttons are suppressed on the owning pad (so
//    scrolling or resting the rail on a desk never flips Borealis back to
//    GAMEPAD mode and hides the cursor), while face/D-pad buttons stay active.
void check_pad_motion_and_active_masks() {
    PadMotionTracker tracker;

    // Boot idle frame: PadPointer reports active=true with (960, 540), delta=(0, 0).
    PadDelta d0 = tracker.update(true, PointerSide::Right, 960, 540, 0, 0);
    CHECK(!d0.moved);
    CHECK_EQ(d0.dx, 0);
    CHECK_EQ(d0.dy, 0);

    // Hardware delta moves the cursor.
    PadDelta d1 = tracker.update(true, PointerSide::Right, 978, 534, 18, -6);
    CHECK(d1.moved);
    CHECK_EQ(d1.dx, 18);
    CHECK_EQ(d1.dy, -6);

    // Same-side coordinate change with zero hardware delta still moves the cursor.
    PadDelta d2 = tracker.update(true, PointerSide::Right, 990, 540, 0, 0);
    CHECK(d2.moved);
    CHECK_EQ(d2.dx, 12);
    CHECK_EQ(d2.dy, 6);

    // Switching from Right to Left without a hardware delta re-primes without a
    // spurious jump across the screen.
    PadDelta d3 = tracker.update(true, PointerSide::Left, 200, 300, 0, 0);
    CHECK(!d3.moved);
    PadDelta d4 = tracker.update(true, PointerSide::Left, 215, 305, 0, 0);
    CHECK(d4.moved);
    CHECK_EQ(d4.dx, 15);
    CHECK_EQ(d4.dy, 5);

    // Resetting on focus loss re-primes cleanly.
    tracker.reset();
    CHECK(!tracker.update(true, PointerSide::Left, 400, 400, 0, 0).moved);

    // Active mouse pad masks: while hidden, nothing is masked.
    CHECK_EQ(activeMousePadMask(PointerSide::Left, false), (uint64_t)0);
    CHECK_EQ(activeMousePadMask(PointerSide::Right, false), (uint64_t)0);
    CHECK_EQ(activeMousePadMask(PointerSide::None, true), (uint64_t)0);

    // Right Joy-Con mouse suppresses StickR pseudo-buttons + RightSL/SR, never A/B/R-stick-click/Left.
    const uint64_t A       = 1ull << 0;
    const uint64_t StickLUp = 1ull << 17;
    const uint64_t StickRUp = 1ull << 21;
    const uint64_t LeftSL  = 1ull << 24;
    const uint64_t RightSR = 1ull << 27;
    const uint64_t rmask   = activeMousePadMask(PointerSide::Right, true);
    CHECK((rmask & StickRUp) != 0);
    CHECK((rmask & RightSR) != 0);
    CHECK_EQ(rmask & A, (uint64_t)0);
    CHECK_EQ(rmask & StickLUp, (uint64_t)0);
    CHECK_EQ(rmask & LeftSL, (uint64_t)0);

    const uint64_t lmask = activeMousePadMask(PointerSide::Left, true);
    CHECK((lmask & StickLUp) != 0);
    CHECK((lmask & LeftSL) != 0);
    CHECK_EQ(lmask & StickRUp, (uint64_t)0);
    CHECK_EQ(lmask & RightSR, (uint64_t)0);
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
// field that must be masked has to be recognisable as such.
void check_ime_requests() {
    // 32 characters of Chinese take at most 128 bytes plus the NUL.
    CHECK(imeBufferBytes(32) >= 32 * 3 + 1);
    CHECK_EQ(imeBufferBytes(32), (std::size_t)129);
    CHECK_EQ(imeBufferBytes(512), (std::size_t)2049);
    // A zero or negative request still needs a usable buffer.
    CHECK_EQ(imeBufferBytes(0), (std::size_t)5);
    CHECK_EQ(imeBufferBytes(-3), (std::size_t)5);

    // Masking is its own flag: forbidding spaces never made a field a password,
    // and a masked field may still contain a space.
    CHECK(imeIsSensitive(KEYBOARD_SENSITIVE_INPUT));
    CHECK(!imeIsSensitive(0));
    CHECK(!imeIsSensitive(1));  // KEYBOARD_DISABLE_SPACE
    CHECK(!imeIsSensitive(2));  // '@' only
    CHECK(!imeIsSensitive(4));  // '%' only
    CHECK(imeIsSensitive(KEYBOARD_SENSITIVE_INPUT | 1 | 2 | 4));

    // The flag is outside Borealis' 0..7 range and is stripped before the mapping
    // onto SwkbdKeyDisableBitmask, so it can never forbid a character.
    CHECK((KEYBOARD_SENSITIVE_INPUT & 0xFF) == 0);
    CHECK_EQ(imeKeyDisableBits(KEYBOARD_SENSITIVE_INPUT), 0);
    CHECK_EQ(imeKeyDisableBits(KEYBOARD_SENSITIVE_INPUT | 1), 1);
    CHECK_EQ(imeKeyDisableBits(1 | 2 | 4), 1 | 2 | 4);
    CHECK_EQ(imeKeyDisableBits(0), 0);

    // A proxy URL with credentials is a secret; a plain one is not.
    CHECK(wiliwili_proxy_is_sensitive("http://user:pass@proxy.example:8080"));
    CHECK(wiliwili_proxy_is_sensitive("socks5://user:pass@127.0.0.1:1080"));
    CHECK(!wiliwili_proxy_is_sensitive("http://proxy.example:8080"));
    CHECK(!wiliwili_proxy_is_sensitive("proxy.example:8080"));
    CHECK(!wiliwili_proxy_is_sensitive(""));
    CHECK(!wiliwili_proxy_is_sensitive(nullptr));
}

// The Pinyin fallback: the system keyboard composes Chinese, but when swkbdCreate
// fails at runtime this table is the only path to a Chinese search term.
void check_pinyin_fallback() {
    const std::vector<std::string> ni = pinyinCandidates("ni");
    CHECK(!ni.empty());
    CHECK_EQ(ni.front(), "你");
    CHECK_EQ(pinyinCandidates("hao").front(), "好");
    CHECK_EQ(pinyinCandidates("dong").front(), "动");
    CHECK_EQ(pinyinCandidates("man").front(), "漫");
    CHECK_EQ(pinyinCandidates("pin").front(), "频");
    CHECK(pinyinCandidates("zzz").empty());
    CHECK(pinyinCandidates("").empty());

    // The table is well formed: a lowercase syllable per entry, at least one
    // UTF-8 candidate, and no duplicate syllable.
    for (const PinyinSyllable& entry : pinyinTable()) {
        CHECK(entry.syllable != nullptr && entry.hanzi != nullptr);
        const std::string syllable = entry.syllable;
        CHECK(!syllable.empty());
        CHECK(syllable.size() <= PINYIN_MAX_SYLLABLE);
        for (char letter : syllable) CHECK(letter >= 'a' && letter <= 'z');

        const std::vector<std::string> candidates = pinyinCandidates(syllable);
        CHECK(!candidates.empty());
        for (const std::string& candidate : candidates) {
            CHECK(!candidate.empty());
            // At least three bytes: one UTF-8 encoded Hanzi.
            CHECK(candidate.size() >= 3);
        }
    }

    // Composition state machine: letters build a syllable, commit turns it into
    // the most common Hanzi, and nothing else is swallowed.
    PinyinComposer composer;
    CHECK(composer.append('N'));
    CHECK(composer.append('i'));
    CHECK_EQ(composer.syllable(), "ni");
    CHECK(!composer.candidates().empty());
    CHECK_EQ(composer.commit(), "你");
    CHECK_EQ(composer.syllable(), "");

    // A second syllable composes the same way, so "ni" + "hao" is 你好.
    CHECK(composer.append('h'));
    CHECK(composer.append('a'));
    CHECK(composer.append('o'));
    CHECK_EQ(composer.commit(0), "好");

    // Candidate selection keeps the alternatives reachable.
    CHECK(composer.append('d'));
    CHECK(composer.append('e'));
    CHECK_EQ(composer.commit(1), "得");
    CHECK_EQ(composer.commit(9), "");  // nothing to commit, and no crash

    // Keys that are not letters stay ordinary text, and backspace edits one
    // letter at a time.
    CHECK(!composer.append('1'));
    CHECK(!composer.append(' '));
    CHECK(!composer.append('\x01'));
    CHECK(composer.append('n'));
    CHECK(composer.append('i'));
    CHECK(composer.append('h'));
    CHECK(composer.backspace());
    CHECK_EQ(composer.syllable(), "ni");
    CHECK(composer.backspace());
    CHECK(composer.backspace());
    CHECK(!composer.backspace());
    composer.clear();
    CHECK_EQ(composer.syllable(), "");

    // The longest syllable in the table fits the buffer.
    CHECK(composer.append('z'));
    CHECK(composer.append('h'));
    CHECK(composer.append('u'));
    CHECK(composer.append('a'));
    CHECK(composer.append('n'));
    CHECK(composer.append('g'));
    CHECK_EQ(composer.syllable(), "zhuang");
    CHECK(!composer.append('x'));  // past the longest syllable
    CHECK_EQ(composer.commit(), "");
}

// Mixed CJK strings are laid out in the regular font, so the regular font needs
// the whole CJK chain too, and every script the client can show is present.
void check_font_fallbacks() {
    const std::vector<FallbackFont>& chain = sharedFontFallbacks();
    CHECK(!chain.empty());

    bool has_hans = false, has_hans_ext = false, has_hant = false, has_ko = false;
    bool has_icons = false, has_material = false, has_emoji = false;
    for (const FallbackFont& font : chain) {
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
    for (std::size_t i = 0; i < chain.size(); ++i) {
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
    for (const FallbackFont& font : chain) {
        const int type = (int)font.shared;
        CHECK(type >= (int)SharedFont::Standard && type <= (int)SharedFont::NintendoExt);

        const std::string name = font.name;
        const bool cjk         = name == FONT_NAME_CHINESE_SIMPLIFIED || name == FONT_NAME_CHINESE_SIMPLIFIED_EXT ||
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
    check_touch_viewport();
    check_button_claims();
    check_multi_pad_claims();
    check_cursor_arbitration();
    check_hover_feedback();
    check_pad_motion_and_active_masks();
    check_scroll_mapping();
    check_ime_requests();
    check_pinyin_fallback();
    check_font_fallbacks();
    check_keyboard_fallback();

    return test_check::report("switch2 input checks passed");
}
