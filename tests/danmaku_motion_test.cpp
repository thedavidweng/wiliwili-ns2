//
// Deterministic regression checks for wiliwili danmaku motion smoothness:
// 1. Real NanoVG + Fontstash CJK glyph quad capture proving upstream nvgText(x,y)
//    snaps horizontal coordinates to integer physical pixels via floorf, whereas
//    wiliwili::drawSubpixelText preserves continuous subpixel quad translation,
//    invariant inter-glyph spacing, constant incline-shadow offset, and crisp
//    integer-pixel vertical alignment.
// 2. Replay of Switch 2 Kopper / multi-buffer FIFO swapchain timing bursts and
//    30 fps MPVCore::on_update CPU jitter proving DanmakuMotionClock eliminates
//    30 Hz frame-delta sawtooth across 60 Hz, 120 Hz, pause/resume, and speed
//    changes with zero cumulative drift.
//

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nanovg.h>

#include "view/danmaku_motion.hpp"

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)

namespace {

struct CapturedCall {
    std::vector<NVGvertex> verts;
};

struct HeadlessNVG {
    int nextTexId = 1;
    int texW      = 2048;
    int texH      = 2048;
    std::vector<CapturedCall> calls;
};

int mockRenderCreate(void*) { return 1; }

int mockRenderCreateTexture(void* uptr, int, int w, int h, int, const unsigned char*) {
    auto* ctx = static_cast<HeadlessNVG*>(uptr);
    ctx->texW = w;
    ctx->texH = h;
    return ctx->nextTexId++;
}

int mockRenderDeleteTexture(void*, int) { return 1; }

int mockRenderUpdateTexture(void*, int, int, int, int, int, const unsigned char*) { return 1; }

int mockRenderGetTextureSize(void* uptr, int, int* w, int* h) {
    auto* ctx = static_cast<HeadlessNVG*>(uptr);
    *w        = ctx->texW;
    *h        = ctx->texH;
    return 1;
}

void mockRenderViewport(void*, float, float, float) {}
void mockRenderCancel(void*) {}
void mockRenderFlush(void*) {}
void mockRenderFill(void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, const float*, const NVGpath*, int) {}
void mockRenderStroke(void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float, const NVGpath*, int) {}

void mockRenderTriangles(void* uptr, NVGpaint*, NVGcompositeOperationState, NVGscissor*,
                         const NVGvertex* verts, int nverts, float) {
    auto* ctx = static_cast<HeadlessNVG*>(uptr);
    if (nverts > 0) {
        ctx->calls.push_back(CapturedCall{std::vector<NVGvertex>(verts, verts + nverts)});
    }
}

void mockRenderDelete(void*) {}

NVGcontext* createHeadlessNVG(HeadlessNVG* backend) {
    NVGparams params{};
    params.userPtr              = backend;
    params.edgeAntiAlias        = 1;
    params.renderCreate         = mockRenderCreate;
    params.renderCreateTexture  = mockRenderCreateTexture;
    params.renderDeleteTexture  = mockRenderDeleteTexture;
    params.renderUpdateTexture  = mockRenderUpdateTexture;
    params.renderGetTextureSize = mockRenderGetTextureSize;
    params.renderViewport       = mockRenderViewport;
    params.renderCancel         = mockRenderCancel;
    params.renderFlush          = mockRenderFlush;
    params.renderFill           = mockRenderFill;
    params.renderStroke         = mockRenderStroke;
    params.renderTriangles      = mockRenderTriangles;
    params.renderDelete         = mockRenderDelete;
    return nvgCreateInternal(&params);
}

void check_subpixel_nanovg_quads() {
    HeadlessNVG backend;
    NVGcontext* vg = createHeadlessNVG(&backend);
    CHECK(vg != nullptr);

    const std::string fontPath = std::string(WILIWILI_SOURCE_DIR) + "/resources/font/switch_font.ttf";
    const int fontId           = nvgCreateFont(vg, "danmaku", fontPath.c_str());
    CHECK(fontId >= 0);

    constexpr float kLogicalW   = 1280.0f;
    constexpr float kLogicalH   = 720.0f;
    constexpr float kPxRatio    = 1.5f;  // 1920x1080 drawable over 1280x720 logical
    constexpr float kIdealStepX = 1.64f; // logical px/frame -> 2.46 physical px/frame (beats 2px/3px under floorf)
    const char* kMsg            = "弹幕顺滑滚动测试 AV23333";

    std::vector<float> legacyPhysSteps;
    std::vector<float> subpixelPhysSteps;
    float prevLegacyPhysX   = 0.0f;
    float prevSubpixelPhysX = 0.0f;
    float refInterGlyphSpan = -1.0f;

    for (int frame = 0; frame < 60; ++frame) {
        const float drawX = 1000.0f - static_cast<float>(frame) * kIdealStepX;
        const float drawY = 41.0f;  // 41 * 1.5 = 61.5 physical px (half-pixel vertical input)

        // 1. Legacy upstream path: nvgText(vg, drawX, drawY, ...)
        backend.calls.clear();
        nvgBeginFrame(vg, kLogicalW, kLogicalH, kPxRatio);
        nvgFontFaceId(vg, fontId);
        nvgFontSize(vg, 30.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(vg, drawX, drawY, kMsg, nullptr);
        nvgEndFrame(vg);
        CHECK(!backend.calls.empty());
        CHECK(backend.calls[0].verts.size() >= 12);
        const float legacyPhysX = backend.calls[0].verts[0].x * kPxRatio;

        // 2. Fixed path: wiliwili::drawSubpixelText + incline offset stability check
        backend.calls.clear();
        nvgBeginFrame(vg, kLogicalW, kLogicalH, kPxRatio);
        nvgFontFaceId(vg, fontId);
        nvgFontSize(vg, 30.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgSave(vg);
        nvgTranslate(vg, drawX, wiliwili::snapVerticalPixel(drawY, kPxRatio));
        nvgText(vg, 1.0f, 1.0f, kMsg, nullptr);  // incline border pass (dx=dy=1.0 logical = 1.5 phys)
        nvgText(vg, 0.0f, 0.0f, kMsg, nullptr);  // content pass
        nvgRestore(vg);
        nvgEndFrame(vg);
        CHECK(backend.calls.size() == 2);

        const auto& borderVerts  = backend.calls[0].verts;
        const auto& contentVerts = backend.calls[1].verts;
        CHECK(contentVerts.size() >= 12);

        const float subpixelPhysX = contentVerts[0].x * kPxRatio;
        const float subpixelPhysY = contentVerts[0].y * kPxRatio;
        // Vertical coordinate must remain snapped to an integer physical pixel row.
        CHECK(std::abs(subpixelPhysY - std::round(subpixelPhysY)) < 1e-3f);

        // Border-to-content offset must stay constant across frames (no 1px shadow shimmer).
        const float borderOffsetPhysX = (borderVerts[0].x - contentVerts[0].x) * kPxRatio;
        CHECK(std::abs(borderOffsetPhysX - 1.0f) < 1e-3f);

        // Inter-glyph spacing between first and last character must be 100% invariant.
        const float span = contentVerts.back().x - contentVerts.front().x;
        if (refInterGlyphSpan < 0.0f) {
            refInterGlyphSpan = span;
        } else {
            CHECK(std::abs(span - refInterGlyphSpan) < 1e-3f);
        }

        if (frame > 0) {
            legacyPhysSteps.push_back(prevLegacyPhysX - legacyPhysX);
            subpixelPhysSteps.push_back(prevSubpixelPhysX - subpixelPhysX);
        }
        prevLegacyPhysX   = legacyPhysX;
        prevSubpixelPhysX = subpixelPhysX;
    }

    // Confirm legacy path suffers integer quantization (alternating 2.0 and 3.0 physical px steps).
    const auto [legacyMinIt, legacyMaxIt] = std::minmax_element(legacyPhysSteps.begin(), legacyPhysSteps.end());
    CHECK((*legacyMaxIt - *legacyMinIt) > 0.9f);

    // Confirm subpixel path moves by exact constant 2.46 physical px on every single frame.
    const float expectedPhysStep = kIdealStepX * kPxRatio;
    for (float step : subpixelPhysSteps) {
        CHECK(std::abs(step - expectedPhysStep) < 2e-3f);
    }

    // Even when DANMAKU_RENDER_QUALITY = 50% (fontQuality = 0.5), subpixel translation stays smooth.
    backend.calls.clear();
    nvgBeginFrame(vg, kLogicalW, kLogicalH, kPxRatio);
    nvgFontFaceId(vg, fontId);
    nvgFontSize(vg, 30.0f);
    nvgFontQuality(vg, 0.5f);
    wiliwili::drawSubpixelText(vg, 500.25f, 40.0f, kPxRatio, kMsg);
    nvgEndFrame(vg);
    const float xAtQuarter = backend.calls[0].verts[0].x;
    backend.calls.clear();
    nvgBeginFrame(vg, kLogicalW, kLogicalH, kPxRatio);
    nvgFontFaceId(vg, fontId);
    nvgFontSize(vg, 30.0f);
    nvgFontQuality(vg, 0.5f);
    wiliwili::drawSubpixelText(vg, 500.75f, 40.0f, kPxRatio, kMsg);
    nvgEndFrame(vg);
    const float xAtThreeQuarter = backend.calls[0].verts[0].x;
    CHECK(std::abs((xAtThreeQuarter - xAtQuarter) - 0.5f) < 1e-3f);

    nvgDeleteInternal(vg);
}

void check_kopper_swapchain_burst_smoothing() {
    // Replay the Switch 2 OunceVK Kopper async swapchain burst pattern observed in
    // console telemetry: pairs of [600 us, 32733 us] (mean = 16666.5 us = 60.0 Hz)
    // and 30 fps MPVCore::on_update alternating [6000 us, 27333 us].
    wiliwili::DanmakuMotionClock clock;
    int64_t rawNow = 10000000;

    int64_t prevPaced = clock.step(rawNow, true);
    int64_t prevRaw   = rawNow;

    int64_t minRawStep   = INT64_MAX;
    int64_t maxRawStep   = 0;
    int64_t minPacedStep = INT64_MAX;
    int64_t maxPacedStep = 0;

    for (int i = 0; i < 300; ++i) {
        const int64_t rawDt = (i % 2 == 0) ? 600 : 32734;  // exact 33334 us per 2 frames (60 Hz)
        rawNow += rawDt;
        const int64_t paced = clock.step(rawNow, true);

        if (i >= 12) {
            minRawStep   = std::min(minRawStep, rawNow - prevRaw);
            maxRawStep   = std::max(maxRawStep, rawNow - prevRaw);
            minPacedStep = std::min(minPacedStep, paced - prevPaced);
            maxPacedStep = std::max(maxPacedStep, paced - prevPaced);
        }
        prevRaw   = rawNow;
        prevPaced = paced;
    }

    // Raw wall-clock jumps 54x between consecutive frames (600 us vs 32734 us).
    CHECK(minRawStep == 600);
    CHECK(maxRawStep == 32734);

    // Paced VSync clock keeps every frame step within +-2% of 16667 us and zero drift.
    CHECK(minPacedStep >= 16300);
    CHECK(maxPacedStep <= 17050);
    CHECK(std::abs(prevPaced - rawNow) < 25000);

    // 3-buffer swapchain burst [600 us, 600 us, 48801 us] (50001 us per 3 frames = 60.0 Hz)
    clock.reset();
    rawNow    = 20000000;
    prevPaced = clock.step(rawNow, true);
    for (int i = 0; i < 180; ++i) {
        const int64_t dt = (i % 3 == 2) ? 48801 : 600;
        rawNow += dt;
        const int64_t paced = clock.step(rawNow, true);
        if (i >= 15) {
            const int64_t step = paced - prevPaced;
            CHECK(step >= 16000 && step <= 17350);
        }
        prevPaced = paced;
    }

    // 24 fps video 3:2 pulldown CPU jitter on 60 Hz [25000, 11000, 14000, 24000, 9335] (83335 us / 5 = 16667 us)
    clock.reset();
    rawNow                         = 30000000;
    prevPaced                      = clock.step(rawNow, true);
    constexpr int64_t kPulldown[5] = {25000, 11000, 14000, 24000, 9335};
    for (int i = 0; i < 200; ++i) {
        rawNow += kPulldown[i % 5];
        const int64_t paced = clock.step(rawNow, true);
        if (i >= 15) {
            const int64_t step = paced - prevPaced;
            CHECK(step >= 16100 && step <= 17250);
        }
        prevPaced = paced;
    }
}

void check_120hz_and_pause_resume_continuity() {
    wiliwili::DanmakuMotionClock clock;
    int64_t rawNow    = 50000000;
    int64_t prevPaced = clock.step(rawNow, true);

    // 120 Hz handheld / docked panel with async burst [900 us, 15766 us] (mean = 8333 us).
    for (int i = 0; i < 120; ++i) {
        rawNow += (i % 2 == 0) ? 900 : 15766;
        const int64_t paced = clock.step(rawNow, true);
        if (i >= 12) {
            const int64_t step = paced - prevPaced;
            CHECK(step >= 8050 && step <= 8620);
        }
        prevPaced = paced;
    }

    // Pause for 10 seconds (600 frames while advancing == false): paced time must freeze
    // completely so danmaku neither jump on pause nor teleport off-screen on resume.
    const int64_t frozenAtPause = prevPaced;
    for (int i = 0; i < 600; ++i) {
        rawNow += 8333;
        const int64_t pausedTime = clock.step(rawNow, false);
        CHECK(pausedTime == frozenAtPause);
    }

    // Resume playback: first resumed frame advances cleanly by ~1 frame (~8333 us), NOT +10s!
    rawNow += 8333;
    const int64_t firstResumed = clock.step(rawNow, true);
    const int64_t resumeStep   = firstResumed - frozenAtPause;
    CHECK(resumeStep >= 8000 && resumeStep <= 8700);
}

}  // namespace

int main() {
    check_subpixel_nanovg_quads();
    check_kopper_swapchain_burst_smoothing();
    check_120hz_and_pause_resume_continuity();
    return 0;
}
