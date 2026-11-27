# Nintendo Switch 2 Port Status & Release Readiness (`wiliwili-plus`)

This page tracks the implemented Switch 2 architecture, current verification scope, honest limitations, and pending real-console gates for `wiliwili-NS2.nro`.

## Architecture & Implemented Capabilities (`main`)

| Area | Implemented Behavior | Status |
|---|---|---|
| **GPU & Rendering Stack** | `Borealis / NanoVG + libmpv -> SDL2 / EGL / OpenGL ES 3.1 -> OunceVK Zink -> NVK Vulkan` (`USE_GLES3=ON`) | Implemented & cross-compiled in CI (`#7`, `#8`) |
| **Video Decoding Baseline (`main`)** | `FFmpeg 7.1.5` + `libmpv 0.36.0` Software baseline (`hwdec=no`, with `14ef15c5f` active render loop pacing & `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME` disabled) | Implemented; hardware decode R&D isolated on `feat/switch2-viddec` (`#14`, `#15`) |
| **Audio & Danmaku Sync** | 12-frame VSync phase-locked `DanmakuMotionClock`, subpixel NanoVG translation, monotonic danmaku culling | Implemented & verified by `danmaku_motion` host tests |
| **Input & Text Entry** | Dual Joy-Con 2 mouse mode arbitration (`switch2_pointer.hpp`), touch + controller focus, `swkbd` UTF-8 input with sensitive-field masking, shared CJK font chain | Implemented & verified by `switch2_input` host tests (`#7`) |
| **Display & Lifecycle** | Dynamic 1080p handheld / docked resolution & refresh detection, sleep/wake & network reconnect probes, isolated `/config/wiliwili-ns2/` settings & `boot.log` | Implemented & verified by `switch2_lifecycle` host tests (`#7`, `#8`) |
| **Playback Acceleration & Diagnostics** | Opt-in Multi-CDN concurrent Range proxy (`multi_cdn_proxy`), sanitized `PerfDiagnostics`, Bilibili API WBI + session cookie clearing (`BilibiliClient::clearCookies`) | Implemented & verified by `multi_cdn` / `bilibili_api` tests (`#12`, `#17`–`#19`) |
| **Privacy / Telemetry** | `NO_GA` enforced in Switch 2 preset and audited in ELF (`audit-nro.sh`); raw QR auth payloads stripped from stdout | Enforced in CI (`audit-nro.sh`) |

## Pending Real-Console Acceptance Gates

> [!NOTE]
> Host tests and cross-compilation pass cleanly in GitHub Actions, **but physical Nintendo Switch 2 hardware testing is still required** before declaring final console release sign-off. No hardware FPS/`p50`/`p95`/`p99` claims are published until Gate G2 is measured on console.

| Gate | Scope | Current Status |
|---|---|---|
| `#6` (Gate G1) | Boot `wiliwili-NS2.nro` under kaiHEN, Joy-Con 2 mouse, CJK font rendering, `swkbd`, isolated `/config/wiliwili-ns2/boot.log` | Pending console run |
| `#10` (Gate G2) | Real-console 1080p DASH playback, mouse timeline scrubbing, recorded `p50`/`p95`/`p99` baseline | Pending console run |
| `#15` (Gate G3) | Benchmark & owner merge decision for isolated `feat/switch2-viddec` (`h264_ounce` / `hwdec=ounce-copy` via `wiliwili-NS2-hwdec.nro`) | Pending console run (`#16` gated on `#15`) |
| `#23` | Verify `OunceVK` `GLES3` runtime stability on Switch 2 vs legacy `GLES2` path | Pending console run |
| `#21` | Submit `wiliwili-NS2` entry to `ouncebrew/hblist` after console acceptance | Pending `#20` + console acceptance |

## Toolchain & Compatibility Matrix

All toolchain versions, container digests, and portlib SHA-256 hashes are pinned in [`UPSTREAM.json`](../UPSTREAM.json):

- **Toolchain base:** `devkitpro/devkita64:20260219` (GCC 15.2.0, `devkitA64 r29.2-1`, `newlib 4.6.0.20260123`)
- **Ounce SDK packages:** `libounce 20261117.1-1`, `ouncevk 20261117.1-1`, `libkaihen 20261117.1-1`
- **Media portlibs:** `switch-ffmpeg 7.1.5-5`, `switch-libmpv 0.36.0-2`, `switch-dav1d 1.5.5-1`, `switch-libass 0.17.3-1`
- **Release packaging:** Produced by [`scripts/switch2/package-release.sh`](../scripts/switch2/package-release.sh) (`wiliwili-NS2.nro`, `wiliwili-NS2-sd.zip`, `wiliwili-NS2-debug-symbols.tar.gz`, `BUILDINFO.json`, `SHA256SUMS`).

## Sanitized Issue Reporting

When reporting a Switch 2 issue on `thedavidweng/wiliwili-plus`:

1. Include `/config/wiliwili-ns2/boot.log` and the build's `BUILDINFO.json` (or `wiliwili-NS2.nro` SHA-256 from `SHA256SUMS`).
2. State your kaiHEN version, firmware version, docked/handheld mode, and reproduction steps.
3. **Never attach `/config/wiliwili-ns2/wiliwili_config.json`** — it contains your personal Bilibili session cookies and `refresh_token`.
