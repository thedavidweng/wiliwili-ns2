# Gate G1: device checklist

Gate G1 is the UI, input, display and lifecycle gate for Switch 2
(`#6`, blocks Phase 2). This page is the deploy-and-check procedure for the
person holding the console. It is not a test suite: host tests stop at the
decisions a desktop can reproduce (`docs/building.md`), everything below needs
hardware.

Record one table row per item with `pass`, `fail`, `blocked` or `not tested`.
`blocked` means a prerequisite failed; `not tested` means nobody tried. A gate
item is only accepted with the provenance block filled in, and the owner decides
G1 on that evidence (`docs/adr/0004-device-gates-not-ci.md`).

## 1. Provenance (fill this in first)

| Field | Value |
|---|---|
| Commit SHA | |
| `wiliwili-NS2.nro` SHA-256 | |
| Switch 2 model | |
| Firmware version | |
| kaiHEN / launcher version | |
| `[ounce]` SDK (`libounce`/`OunceVK`/`libkaihen`) | from `UPSTREAM.json` |
| Output mode | handheld / docked, resolution + refresh from `boot.log` |
| Controller setup | Joy-Con 2 left + right attached, slide/bare mode |
| Audio output | console speakers / TV / headphones |
| Network | Wi-Fi SSID band or Ethernet |
| `boot.log` from `/config/wiliwili-ns2/boot.log` | attached |

```sh
sha256sum build/switch2-release/wiliwili-NS2.nro      # or shasum -a 256
```

## 2. Deploy

```sh
scripts/switch2/build.sh configure
scripts/switch2/build.sh build
scripts/switch2/build.sh audit        # must end with "Switch 2 artifact audit passed"
```

Copy `build/switch2-release/wiliwili-NS2.nro` to the SD card as
`/switch/wiliwili-NS2.nro`, keep `wiliwili-NS2.elf` next to the build for symbol
lookups, and start the NRO from kaiHEN's homebrew menu. Settings and `boot.log`
live in `/config/wiliwili-ns2/`, never in Switch 1's `/config/wiliwili/`.

Before the first run, delete `/config/wiliwili-ns2/` if this is not a fresh
install: an old `wiliwili_config.json` wins over any new default, including
`tls_verify`.

After each run, copy `boot.log` off the card. It is the only record of the boot
stages, the driver report, the display report and the lifecycle probe.

## 3. What `boot.log` has to say

One line per stage. The ones G1 reads:

| Line | Pass looks like | If it does not |
|---|---|---|
| `[boot]`, `[socket]`, `[romfs]`, `[pl]`, `[setsys]`, `[set]`, `[psm]`, `[nifm]`, `[lbl]` | `ok` | the stage that says `failed: 0x...` is the cause; a boot that cannot start prints the reason on a text console |
| `[gpu] gpu api: OpenGL ES (Zink)` | `GL_RENDERER` names Zink, `GL_VERSION` says `OpenGL ES 3.1` or newer, `[gpu] vkDeviceName` names NVK on the Switch 2 GPU | still on a screen means Zink is not the driver; note the strings verbatim |
| `[gpu] gpu api: EGL (OunceVK)` | `EGL_CONTEXT_CLIENT_VERSION: 3`, `EGL_RENDERABLE_TYPE` includes the ES3 bit `0x40` (decimal values such as `64` or `77`), `GL_MAJOR_MINOR_VERSION: 3.1` or newer, `EGL_DEPTH_STENCIL_SIZE` ends in `/8` | an `[sdl] SDL_GL_CreateContext failed: ...` line instead means no ES 3.1 context; copy it verbatim for `#23` |
| `[display] render WxH, refresh N Hz, output WxH handheld\|docked` | handheld shows `output 1920x1080 handheld`; the render size is at most 1920x1080 | a `render` of 0 or a missing line means the display query failed; note it, the UI still runs |
| `[display] vi2 ...` | either the mode list and the 120 Hz capability result, or `vi2 unavailable (0x...)` | both are non-fatal. Note which one and whether the UI still updates |
| `[probe] boot: audio N outputs, socket connected, dns configured` | exactly that | `audio init failed` / `socket not connected` / `dns unresolved` name the failing half |
| `[lifecycle] resumed` | one per sleep/wake | a missing line means the resume hook did not fire |
| `[probe] resume: ...` | follows a resume when the probe is due (it is rate limited to 30 s) | a stale `socket not connected` explains a first request that fails right after a wake |

`render` and `output` are different numbers on purpose: the UI renders at the
framebuffer budget while the console may be driving a larger docked mode, and
neither is a 4K or HDR claim. Do not record a resolution or refresh rate that is
not in this line.

## 4. G1 items

Mark every row. Detail follows the table.

| # | Item | Result | Evidence |
|---|---|---|---|
| 1 | NRO launches from kaiHEN into Borealis, native 1080p handheld, `/config/wiliwili-ns2` used, GPU/display facts logged | | `boot.log`, photo |
| 2 | Controller navigation accurate in handheld and docked | | photo/video |
| 3 | Touch mapping accurate in handheld and docked (no offset) | | video |
| 4 | Joy-Con 2 mouse: engagement, cursor scale, click, hover, stick-wheel scroll, drag | | video, left and right |
| 5 | Joy-Con 2 mouse: no duplicate shoulder/trigger action while clicking | | video |
| 6 | Switching mouse / controller / touch keeps focus and cursor sane | | video |
| 7 | Mouse recovers across reconnect, side switch, sleep/wake, `swkbd`, exit | | video |
| 8 | Chinese IME composes Simplified/Traditional, returns UTF-8 | | video of a search |
| 9 | IME cancel/reopen restores focus, no stuck input | | video |
| 10 | Mixed CJK/Japanese/Korean text renders without tofu, no bundled fonts | | screenshot, `audit` output |
| 11 | >=3 dock/undock transitions adapt output, refresh and coordinates | | `boot.log`, video |
| 12 | >=3 sleep/wake transitions recover display, input, network, audio | | `boot.log`, video |
| 13 | HTTPS/DNS/TLS succeed, pre-playback probe says healthy | | `boot.log`, Network Diagnostics |
| 14 | >=5 launch and clean-exit cycles, no leak or hang | | `boot.log` per cycle |
| 15 | GLES3 context and NanoVG rendering correct (`#23`) | | `boot.log` `[gpu]` lines, screenshots |
| 16 | GLES3 vs GLES2 comparison build on identical workloads (`#23`) | | frame times, memory, screenshots per build |

### 4.1 Boot, GPU and display (1, 2, 3)

1. Start the NRO. The UI appears; no black screen, no crash log.
2. `boot.log`: every stage `ok`, a Zink `GL_RENDERER`, a `[display]` line with
   `output 1920x1080 handheld`, a `[probe] boot: ...` line.
3. Navigate every tab with the controller, open a video detail page and back.
4. Touch a tab, drag a feed, and check the touched control is the one that
   responds (a half-screen offset means the touch scale is wrong).
5. Dock the console and repeat 2-4. `output` must change; `render` must not
   exceed 1920x1080.

### 4.2 Joy-Con 2 mouse (4, 5, 6, 7)

1. Slide the right Joy-Con 2 on a surface: the cursor appears and tracks.
   Repeat with the left Joy-Con 2.
2. Click a tab, hover a card, scroll a feed with the Joy-Con's stick, drag the
   player timeline.
3. While clicking, watch for a duplicate `R`/`ZR`/`L`/`ZL` gamepad action: a
   click must not also fire the shoulder button.
4. Switch to the controller, then to touch, then back to the mouse. Focus must
   stay where it was, the cursor must come back where it left.
5. Detach and reattach the Joy-Con; sleep and wake; open the search keyboard and
   close it; then use the mouse again. No stuck click, no cursor teleport.

Record the answer to the one question the source cannot settle: is the pointer
reported active during ordinary handheld play without sliding? If the cursor
appears when nothing is moving, note it — the claim gate is `#4`'s.

### 4.3 Chinese input (8, 9)

1. Open Search, type a Simplified Chinese query with `swkbd`, confirm, and check
   the search runs.
2. Switch the system keyboard to Traditional Chinese and repeat.
3. Cancel with `+` and reopen. Focus must return to the search box, no input
   stays latched.
4. Search for a Japanese and a Korean title and confirm the glyphs render.

### 4.4 Display and lifecycle (11, 12)

Dock and undock at least three times, with the UI on a scrolling feed and once
while a video is open. Each transition has to update the picture without a
stretched or cropped frame, and `boot.log` has to show a new `[display]` line
whose `output` matches the new mode.

Note whether the reported refresh rate changes when docking (`refresh 60 Hz` vs
`refresh 120 Hz`) and whether `vi2SetDisplay120HzCapability` was accepted. Both
are reported facts only: the port declares the capability to VI2 and reads
`vi2QueryDisplayRefreshRate` back for the diagnostics and the idle pacing. It
does not claim to present 120 frames a second through OunceVK/Zink. That claim
needs a measurement this checklist has to supply: a frame-time capture on the
console while the UI animates, compared against the reported rate. Until then
the honest statement is "the console reports 120 Hz and the capability was
accepted", never "the UI runs at 120 Hz".

Sleep and wake at least three times, once from the home feed, once with a video
playing, once with the search keyboard open. After each wake: the UI redraws, the
mouse and controller respond, and `boot.log` gains a `[lifecycle] resumed` line,
with a `[probe] resume:` line when the probe was due and its socket and DNS
halves saying `connected` / `configured` once the network is back.

Also verify the pacing claim: leave the home feed untouched for ten seconds. The
UI must stay responsive, not stutter, and no log line should be produced. If a
`p50` frame-time measurement is available, idle frames should sit at the idle cap
(30 Hz), not at 120.

### 4.5 Networking and TLS (13)

1. `[probe] boot: audio N outputs, socket connected, dns configured` in
   `boot.log`.
2. Open Settings -> Tools -> Network Diagnostics. API connectivity and the
   server time delta have to succeed.
3. `tls_verify` now defaults to `1` on Switch 2. If every request fails with a
   curl error instead of returning data, note the exact error string from the
   app log, then set `tls_verify` to `0` in Settings and retry to confirm that
   verification was the cause. Report it either way: this decides whether the new
   default stays.
4. Turn the console's Wi-Fi off and on while the UI is open, then open a fresh
   page. The request must succeed without restarting the app.

### 4.6 Clean exit (14)

Launch and exit five times in a row (`+` from the home feed, and once from inside
a player). Each exit returns to the home menu without a hang or a system error
dialog. Copy `boot.log` after the fifth cycle: it must show five complete
`userAppInit` / `main` / `main loop finished` / `userAppExit` sequences, and the
fifth launch must behave like the first (no slower animation, no missing image,
no missing audio).

### 4.7 OpenGL ES 3.1 rendering (15, 16)

The production NRO renders with OpenGL ES 3 (ADR 0005). This section gives
`#23` the evidence it needs to keep GLES3 as the only configuration.

1. Copy the `[gpu] gpu api: OpenGL ES (Zink)` and `[gpu] gpu api: EGL (OunceVK)`
   blocks, both extension lists included, into the report as they are.
2. Check the rendering in handheld and docked mode, with a screenshot of each:
   - rounded cards, focus highlights and their anti-aliased edges
   - Latin and CJK text, including a long title in a feed
   - thumbnails in a fast-scrolled feed
   - translucent dialogs and the dimmed background behind them
   - clipping: nothing draws outside a scrolled list or a tab frame
   - a clean full-screen redraw after each dock and undock

   The app log must show no `nanovg` GL error and no shader error.
3. Build the comparison NRO (`docs/building.md`, "The GLES2 comparison build").
   Run each NRO through the same workloads in the same output mode:
   - 10 s idle on the home feed
   - one fast scroll through a thumbnail feed
   - open and close a video detail page three times
   - 5 launch/exit cycles
   - 3 sleep/wake cycles

   For each build, record the frame-time capture (or p50/p95/p99 once `#9` has
   it), memory if the console shows it, any visual difference and any lifecycle
   failure. The two `GL_VERSION` lines can be identical: Mesa may grant ES 3.x to
   a 2.0 request. Record what each build reports. Do not assume a difference.

MPVCore video composition on GLES3 cannot be checked before playback works. It
is part of `#7` and is reported back on `#23`.

## 5. Reporting

Comment on `#6` with the provenance table, the item table and the `boot.log`
from each section that failed. Include the exact strings the console produced;
paraphrased results cannot be accepted for a device gate. If a `vi2` call, the
120 Hz capability or `tls_verify=1` fails, say so plainly — the port advertises only
what the console confirmed.
