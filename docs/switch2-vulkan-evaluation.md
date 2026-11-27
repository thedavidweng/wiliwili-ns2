# Switch 2 Stage 3 Evaluation: Native Vulkan & GPU-Direct VideoDecoder Sharing (`#22`)

> [!NOTE]
> **Scope & non-blocking status:** This document captures technical evaluation and console verification criteria for Stage 3 GPU-direct sharing and native Vulkan rendering (`#22`). All experiments stay strictly isolated from `main` and do **not** block Gate G2 (`#10`), Stage 1 hardware decoding (`#14`–`#16`), or `wiliwili-NS2` public beta packaging (`#20`).

## 1. Five-Way Separation of Transfer, Rendering, and Presentation

Per ADR-0001 and ADR-0005, evaluations and diagnostics must never conflate these five independent layers (checking `hwdec-current=ounce-copy` alone is insufficient):

1. **Hardware decode + CPU copy-back (`Stage 1`, `#14` / `#16`):** FFmpeg `ounce` wrapper reads decoded `NV12` frames from `VideoDecoder` into CPU-accessible `AVFrame` planes (`AV_PIX_FMT_NV12`), uploaded into GL textures by `MPVCore`.
2. **Reference-backed pitch-linear `AVFrame` (`Stage 2`, deferred post-G3):** `AVBufferRef` in `buf[0]` wraps a pooled pitch-linear `VideoDecoder` surface directly for `avcodec_receive_frame` to eliminate one CPU memcpy without changing the GL renderer.
3. **GPU-direct shared decoder memory (`Stage 3`, `#22`):** Decoder output buffers are shared directly with the GPU without CPU copy (`VK_EXT_external_memory_host` + GSV `VideoDecoder` registration).
4. **GPU rendering backend (`OunceVK` Zink/GLES 3.1 vs native Vulkan):** How Borealis/NanoVG and `libmpv` issue draw calls (`USE_GLES3=ON` via Zink in `main` vs a hypothetical native Vulkan UI + video compositor).
5. **Presentation / FIFO pacing (`14ef15c5f`):** Swapchain presentation discipline and disabling `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME` so Zink/NVK FIFO queue drainage does not stall main-thread UI/danmaku frames.

## 2. Evaluation of Architectural Options

### A. Canonical Baseline: OunceVK Zink / OpenGL ES 3.1 (`main`)

- **Path:** `Borealis (NanoVG GLES3) + MPVCore (mpv_render_context GL) -> SDL2/EGL -> OunceVK Zink -> NVK Vulkan -> Switch 2 GPU`
- **Strengths:** Uses upstream `xfangfang/borealis` + `libmpv 0.36.0` unchanged; isolates all Switch 2 adaptations cleanly in `platform/switch2/`; preserves existing `#version 300 es` shaders, subtitles, Anime4K/GLSL hooks, and UI rendering.
- **Limitation:** Incurs Zink GL-to-Vulkan translation overhead and CPU-to-texture upload for Stage 1 `AV_PIX_FMT_NV12` frames. Measured impact vs `14ef15c5f`-paced FIFO presentation depends on real-console `#10` / `#15` telemetry.

### B. Native Vulkan Borealis (`XITRIX/borealis` @ `ecd896f068efe1899c09cf7b14ba548507fe7cec`)

- **Maintenance cost:** Adopting XITRIX's Vulkan Borealis backend requires maintaining a major divergence from `xfangfang/borealis` (`wiliwili` fork at `dd652691`), including custom Vulkan swapchain/sync/NanoVG shaders (`ollix/nanovg-vulkan` was unreachable HTTP 404 on 2026-11-23), and re-validating every Borealis layout, view, and UWP/desktop parity path.
- **Conclusion:** Migrating Borealis' entire UI renderer solely to enable Vulkan video sharing imposes high ongoing fork-maintenance cost and directly violates ADR-0005 unless measurements prove Zink/GLES3 cannot sustain target 1080p60/4K playback even with Stage 1/Stage 2 `VideoDecoder` optimizations.

### C. `libmpv` / `libplacebo` Vulkan Embedding Constraints (`mpv#18343`)

- **Upstream gap:** `libmpv 0.36.0`'s public `mpv/render.h` provides mature OpenGL and D3D11 (`#697`) render APIs, whereas `mpv#18343` (caller-owned Vulkan render target/interop in `mpv_render_context`) lacks a settled upstream public API in `0.36.0`.
- **Implication:** Integrating `libmpv` into a native Vulkan compositor on Switch 2 would force either carrying a large private `libmpv`/`libplacebo` patch fork (breaking the pinned `dragonflylee/mpv-build` portlib model in `UPSTREAM.json`) or bypassing `mpv_render_context` with custom swapchain presentation.

### D. `Moonlight-Switch` `VK_EXT_external_memory_host` + GSV Sharing (`841fc353`)

- **Superseded earlier assumption:** Earlier `#14` notes ruled out zero-copy because `OunceVK 20261117.1-1` `EGL_EXTENSIONS` lacks `EGL_KHR_image_base` / `EGL_EXT_image_dma_buf_import` and `vkDeviceExtensions` lacks `VK_KHR_external_memory_fd`. That ruling was **too broad** for Vulkan: `OunceVK 20261117.1-1` *does* expose `VK_EXT_external_memory_host`, which `Moonlight-Switch` uses alongside GSV `VideoDecoder` memory registration to achieve GPU-direct frame sharing in a pure Vulkan path.
- **Safety & maintenance guardrails:**
  - Prefer upstream-supported allocation/import interfaces (`VK_EXT_external_memory_host` + official `libounce` `VideoDecoder` buffer APIs) rather than private `--wrap` SDK linker interception.
  - Because `wiliwili-plus` renders Borealis + `libmpv 0.36.0` via Zink GLES 3.1 (and not a bespoke streaming surface like `Moonlight-Switch`), Stage 3 GPU-direct sharing cannot simply drop into `main` without solving (B) and (C).

## 3. Preliminary Recommendation

1. **Retain `OunceVK` Zink / OpenGL ES 3.1 as the single canonical production renderer** (do **not** maintain permanent dual GLES3 + Vulkan production renderers).
2. Keep `#22` strictly non-blocking behind `#10` (Gate G2 `14ef15c5f`-paced Software baseline metrics) and `#15` (Gate G3 Stage 1 `h264_ounce` metrics).
3. Revisit Stage 3 native Vulkan/GPU-direct prototyping **only** if real-console measurements from `#10` + `#15` + `#22` show that Stage 1 (CPU copy-back) and Stage 2 (reference-backed pitch-linear `AVFrame`) still bottleneck `p95`/`p99` frame times or power/thermals on Switch 2.

## 4. On-Console Capability Probe & Benchmark Checklist (Required to Close `#22`)

Before any Stage 3 code changes are proposed, a future console run must record:

- [ ] **Vulkan instance/device capabilities on Switch 2 (`OunceVK` NVK):** verify actual availability and properties of `VK_NN_vi_surface`, `VK_EXT_external_memory_host`, image-sharing format/tiling features (`NV12` / linear vs block-linear), and whether any `VK_KHR_video_*` extensions are exposed.
- [ ] **5-way comparative metrics against the `14ef15c5f`-paced `OunceVK` Zink/GLES 3.1 baseline** on `#9`/`#10` 1080p60 workloads:
  - Binary/NRO size delta
  - Peak RAM / VRAM usage & shader-cache behavior
  - Playback startup latency
  - `p50` / `p95` / `p99` frame times (`ui_*` and `video_*`) & dropped frames
- [ ] **Final decision record:** either close `#22` confirming OunceVK/Zink as the sole production renderer, or file a narrowly scoped, owner-approved follow-up implementation issue with patch-maintenance budget.
