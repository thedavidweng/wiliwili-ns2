# Building wiliwili-NS2

`CMakePresets.json` is the single source of truth for configurations: every build
below is a preset, and every preset uses Ninja. Build directories live in
`build/`, which git ignores.

## Host (desktop)

```sh
brew install mpv webp            # macOS; Linux: libmpv-dev, libwebp-dev, libssl-dev

cmake --preset desktop-release
cmake --build --preset desktop-release
ctest --preset desktop-release --output-on-failure

# symbols, no optimisation
cmake --preset desktop-debug
cmake --build --preset desktop-debug
```

Both host presets set `BRLS_UNITY_BUILD=OFF` and `WILIWILI_BUILD_TESTS=ON`.
Verified on macOS 27.2.1 (arm64) with CMake 4.5.1, Ninja 1.13.2, Homebrew
mpv 0.42.0 and webp 1.6.0: configure, build and `ctest` pass. The `.app` bundle
target (`cmake --build --preset desktop-release --target wiliwili.app`) is
unchanged upstream behaviour.

## Switch 2 (NS2 NRO)

The container is pinned in `UPSTREAM.json` and defined in
`scripts/switch2/Dockerfile`: devkitPro `devkita64:20260219` by digest, the
official `[ounce]` devkit (`libounce`, `OunceVK`, `libkaihen`
`20261117.1-1`) from `https://pkg.ouncebrew.org` with its signing key verified
against the fingerprint `ouncebrew/ounce-devkit` pins, and the FFmpeg 7.1 /
dav1d / mpv 0.36.0 portlibs verified by sha256. Those three portlib packages
only install headers, static libraries and `.pc` files under
`portlibs/switch`; they ship no GPU or libnx archive, and `mpv.pc`'s
`Requires: egl > 1.4.0` resolves to OunceVK's `egl.pc`.

```sh
scripts/switch2/build.sh image        # docker build -t wiliwili-ns2-toolchain:local
scripts/switch2/build.sh configure    # cmake --preset switch2-release
scripts/switch2/build.sh build        # wiliwili-NS2.nro (+ the unstripped wiliwili-NS2.elf)
scripts/switch2/build.sh audit        # NRO, RomFS and linker map checks
```

`build` sizes the job count to the container: `cc1plus` needs about 1.2 GB for
the heavy translation units, so a 2 GB VM builds with `-j 1` and a larger one
with every core. `WILIWILI_SWITCH2_JOBS` overrides it.

The same by hand:

```sh
docker build -f scripts/switch2/Dockerfile -t wiliwili-ns2-toolchain:local .
docker run --rm -v "$PWD:/src" -w /src wiliwili-ns2-toolchain:local cmake --preset switch2-release
docker run --rm -v "$PWD:/src" -w /src wiliwili-ns2-toolchain:local cmake --build --preset switch2-release -j 1
docker run --rm -v "$PWD:/src" -w /src wiliwili-ns2-toolchain:local \
    sh scripts/switch2/audit-nro.sh build/switch2-release
```

Docker Desktop has to share the checkout (on macOS a path under `/Users`; `/tmp`
is not shared by default, and a bind mount of it comes up empty instead of
failing).

### What the `switch2-release` preset sets

| Variable | Value | Why |
|---|---|---|
| `PLATFORM_SWITCH` / `PLATFORM_SWITCH2` | `ON` | Borealis' Switch backend plus the libounce portlibs and GPU libraries |
| `USE_SDL2`, `USE_EGL` | `ON` | SDL2 window/input on an EGL surface from OunceVK |
| `USE_GLES2` | `ON` | Zink exposes GLES 3.1, but the GLES2 path is the one Borealis and MPVCore exercise today; GLES3 is deferred until device gate G1 |
| `USE_LIBROMFS` | `OFF` | the resources go into the NRO's own `--romfsdir` section, mounted by `romfsInit()` as `romfs:/`. Embedding them as a C++ array would duplicate every asset, turn OpenCC off, and OOM `cc1plus` in a 2 GB container |
| `BUILTIN_NSP`, `USE_DEKO3D` | `OFF` | no Switch 1 NSP forwarder and no deko3d |
| `CMAKE_C_FLAGS_RELEASE`, `CMAKE_CXX_FLAGS_RELEASE` | `-O2 -g -DNDEBUG` | `-O2` matches libounce's `switch2vars.sh`; `-g` keeps `wiliwili.elf` usable as a symbol source next to the stripped NRO |
| `BRLS_UNITY_BUILD` | `OFF` | unity builds hide missing includes and break the Switch 2 include order |

`-mcpu`/`-mtp`/`-ftls-model`, `-fPIE`, `-specs=.../libounce/switch.specs` and
`-lnx` come from `$DEVKITPRO/libounce/cmake/Switch2.cmake` and
`switch2_cpu.mk`, so they follow the devkit instead of being copied here.

Not used on purpose: `-flto` (no measured gain for a first release, much slower
builds), `-ffast-math` (the software decoder's float paths must stay exact),
and any clock or performance-mode change (the port does not overclock).

`cmake/Switch2Platform.cmake` re-applies the Switch 2 platform after Borealis'
`toolchain.cmake`, whose `PLATFORM_SWITCH` branch still configures the Switch 1
devkit: it points `NX_ROOT` at `$DEVKITPRO/libounce`, puts
`$DEVKITPRO/portlibs/switch2` and `$DEVKITPRO/libounce` in front of the Switch 1
portlibs, forces `ANALYTICS` off with `-DNO_GA` so the port compiles without the
Google Analytics endpoint and key, and adds the link map the audit reads.

### What the Switch 2 sources replace and add

Borealis' Switch 1 backend assumes hardware this console does not have, and the
input, text, display and lifecycle work the port needs has no upstream home. All
of it lives in this repository so `library/borealis` stays at its pinned commit:
`library/CMakeLists.txt` marks each replaced submodule file `HEADER_FILE_ONLY`
and compiles the Switch 2 copy instead. `platform/switch2/switch2_pointer.cpp`,
the header-only decisions and `lifecycle_probe.cpp` are additions rather than
replacements.

| File | Why |
|---|---|
| `platform/switch2/sdl_video.cpp` | Borealis' SDL backend asks for a desktop OpenGL 4.3 core context on any `__SWITCH__` build. Here the `#ifdef __SWITCH__` block is Switch 1 only, so Switch 2 gets the GLES2 context OunceVK provides, the window is created at the console's own handheld resolution, every failing SDL/GL call is reported, and the GL strings, the GLES extension list and the NVK Vulkan device with its instance and device extension lists are written to `boot.log`. `library/CMakeLists.txt` sets the submodule's file to `HEADER_FILE_ONLY` and compiles this one instead |
| `platform/switch2/switch2_wrapper.c` | `userAppInit`: same services as the Switch 1 wrapper, each stage recorded in `boot.log`; a boot that cannot start now reports on a text console instead of going black |
| `platform/switch2/borealis/switch_platform.cpp` | Borealis' Switch platform only handles focus and exit, gates `AppletHookType_OnOperationMode` behind `BOREALIS_USE_DEKO3D` (dead code on this build) and never pumps `SDL_PollEvent`, so the queue used to grow for the whole session and a dock or a resume was invisible to the UI. The replacement handles every applet hook, pumps SDL, follows the display on dock/undock, paces frames from the panel refresh and releases the platform in a fixed teardown order |
| `platform/switch2/borealis/switch_input.cpp` | Borealis' Switch input manager knows nothing about the Joy-Con 2 optical mouse, maps touch through the pointer's framebuffer scale, applies the `L|ZL` / `R|ZR` claim to every pad in the frame and never draws a hover frame. The replacement drives `PadPointer` beside the standard mouse, keeps one logical cursor, latches the claim once per frame from the pointer pad only, frames the control under the cursor, and maps touch through the digitizer's own domain |
| `platform/switch2/borealis/switch_ime.cpp` | The upstream system keyboard sizes its buffer at a fixed `0x100` bytes (which truncates UTF-8 CJK), treats "no spaces" as password masking and has no path at all when `swkbdCreate` fails. The replacement sizes the buffer per character, takes masking from `KEYBOARD_SENSITIVE_INPUT`, and falls back to an in-app dialog with Pinyin composition and the linked OpenCC dictionary for Traditional Chinese |
| `platform/switch2/borealis/switch_font.cpp` | The upstream loader gives only one font a CJK fallback chain. The replacement loads the Standard, Simplified, Extended Simplified, Traditional, Korean and Nintendo symbol shared fonts and hangs the whole chain off both `FONT_REGULAR` and `FONT_CHINESE_SIMPLIFIED`, so mixed Latin/CJK strings do not render tofu |
| `platform/switch2/switch2_pointer.{hpp,cpp}`, `switch2_pointer_device.hpp` | The `PadPointer`/Nmouse glue and the pure decisions behind it (viewport transform, touch domain, button claims, cursor arbitration, hover state) that the host tests exercise without a console |
| `platform/switch2/switch2_cjk.hpp` | Pure text decisions shared with the host tests: the UTF-8 keyboard buffer, the sensitive-input flag, the Pinyin fallback table and composer, and the shared-font chain |
| `platform/switch2/switch2_display.{hpp,cpp}` | The display decisions: what the console reports (`appletGetDefaultDisplayResolution`, `appletGetOperationMode`), what it can refresh at (`vi2QueryDisplayRefreshRate`, `vi2QueryDisplayModeCapability`, a `vi2SetDisplay120HzCapability` capability declaration), and how that becomes a framebuffer, a GL viewport and a Borealis logical size. All of it best effort: VI2 may answer busy while OunceVK owns the window, and 120 Hz presentation itself is Gate G1 |
| `platform/switch2/lifecycle_probe.{hpp,cpp}` | The pre-playback probe (`audoutInitialize`/`audoutListAudioOuts` over libounce's `aud:u` route plus `nifm` connection, address and DNS state, one sanitized `boot.log` line), the network watch that polls `nifm` for a reconnect without a console sleep, and the teardown order the platform destructor follows |

`platform/switch2/compat/SDL2/SDL.h` exists because devkitPro's SDL2 only puts
`.../portlibs/switch/include/SDL2` on the include path, and adding its parent
would let the Switch 1 EGL, GLES and Vulkan headers shadow OunceVK's.

### The NRO, the ELF and the RomFS

`wiliwili-NS2.nro` is built with `nacptool` and `elf2nro` from the unstripped
`wiliwili-NS2.elf`, which stays in the build directory as its symbol source. The
NRO carries no debug information: a ~40 MB image against a ~300 MB ELF, and no
source paths from the ELF appear in it. The NACP keeps the upstream author:
title `wiliwili-NS2`, author `xfangfang and wiliwili-NS2 contributors`, version
from `git describe` (the CMake version when the build has no git).

The RomFS is staged into `build/switch2-release/resources` before `elf2nro`
runs: Borealis' `resources/img/sys` and `resources/material` first, then
wiliwili's `resources/`, minus the Switch 1 fonts that are either proprietary
(`switch_font.ttf`, `switch_icons.ttf`) or unused because `SwitchFontLoader`
reads the system shared fonts (`font/keymap_*.ttf`).

### Checking the result

At configure time the resolved link line starts with the Switch 2 libraries:

```
-L/opt/devkitpro/portlibs/switch2/lib -L/opt/devkitpro/libounce/lib -L/opt/devkitpro/portlibs/switch/lib
-specs=/opt/devkitpro/libounce/switch.specs
```

There is no `-I/opt/devkitpro/libnx` anywhere in the generated build files, and
`libnx.a` only exists in `$DEVKITPRO/libounce/lib`, so `-lnx` links libounce.
OunceVK also installs empty `libGLESv2.a`, `libglapi.a` and `libdrm_nouveau.a`
in `portlibs/switch2`, which shadow the Switch 1 copies, and its `libEGL.a` /
`libvulkan.a` are linker scripts that pull `libouncevk-gl.a` / `libouncevk.a`
(Zink, NVK and the KHR/VK_NN surface code). After a link,
`build/switch2-release/wiliwili.map` answers the same question, and
`scripts/switch2/audit-nro.sh` runs every check at once:

```sh
grep -E "/opt/devkitpro/libnx|portlibs/switch/lib/lib(EGL|glapi|drm_nouveau)" \
     build/switch2-release/wiliwili.map   # must print nothing
scripts/switch2/build.sh audit
```

The audit also checks the `HOMEBREWNRO0` header, the embedded NACP (title,
author, version) and RomFS, that `wiliwili-NS2.elf` still carries `.debug_info`
and `.symtab`, that the RomFS has the Borealis cursor and Material icons plus
wiliwili's `xml`, `i18n` and `opencc` data and none of the proprietary fonts,
and that no settings file or telemetry endpoint ended up in the artifact.

### Validation scope

The cross build completes in the 2 GB container (`scripts/switch2/build.sh build`
picks `-j 1` there): `wiliwili-NS2.nro` and `wiliwili-NS2.elf` link against
libounce and OunceVK only, and the audit above passes. Nothing here has run on
hardware yet: the runtime gates for the window on Zink/NVK, input, audio and
playback are the follow-up tickets, and `boot.log` is what those bring-ups read
first. `docs/g1-checklist.md` is the procedure for the first of them (gate G1):
what to deploy, which `boot.log` lines to read and what to record.

### Deploying

Copy `build/switch2-release/wiliwili-NS2.nro` to the SD card as
`/switch/wiliwili-NS2.nro` and start it from kaiHEN's homebrew menu. Settings,
themes, custom fonts and `boot.log` live in `/config/wiliwili-ns2/`, next to
Switch 1's `/config/wiliwili/` and never mixed with it. If the app does not
start, `/config/wiliwili-ns2/boot.log` records every init stage, and a boot that
fails before the window exists says so on a text console. Keep
`wiliwili-NS2.elf` next to the release artifacts: the NRO is created from it by
`elf2nro`, and crash addresses only resolve against that exact ELF.

## Tests

`ctest --preset desktop-release` runs five host checks (built when
`WILIWILI_BUILD_TESTS=ON`):

- `config_pins` (`tests/config_test.cpp`) checks that `UPSTREAM.json` still
  records the revisions and digests this port was built against, that no pin
  floats (`latest`, `:latest`, `head`), that each of the six
  `dragonflylee/wiliwili` `dev` commits is either integrated or explicitly
  skipped, that `scripts/switch2/Dockerfile` installs exactly the digests and
  sha256 values `UPSTREAM.json` records, and that the presets keep the Switch 2
  configuration (libounce toolchain, no deko3d, no NSP forwarder, no libromfs,
  safe release flags, `wiliwili-NS2.nro`).
- `boot_diagnostics` (`tests/boot_diagnostics_test.cpp`) checks the contract the
  device depends on: the Switch 2 config directory is `/config/wiliwili-ns2` and
  never `/config/wiliwili`, telemetry is off there, a driver string cannot break
  the one-line-per-note format of `boot.log`, the GPU report formatting is
  stable, a long extension list wraps instead of losing its tail, the video
  backend reports the Vulkan instance and device extensions, and `main()` plus
  the settings loader never construct `MPVCore`.
- `switch2_artifact_audit` runs `scripts/switch2/audit-nro.sh` against
  `build/switch2-release`. It skips itself when the cross toolchain or the
  artifacts are absent, so a desktop-only checkout stays green.
- `switch2_input` (`tests/switch2_input_test.cpp`) checks the input and text
  decisions that do not need hardware: the framebuffer-to-logical viewport
  transform, touch mapped through the digitizer's own 1920x1080 domain (and the
  1280x720 fallback) rather than the pointer's framebuffer scale, the button
  claim latching across frames while an idle pad reports no buttons, cursor
  arbitration, when a hover frame is drawn, wheel-to-scroll mapping, the UTF-8
  keyboard buffer and the `KEYBOARD_SENSITIVE_INPUT` flag (kept out of Borealis'
  0..7 disable bits and never mapped onto `SwkbdKeyDisableBitmask`), proxy-URL
  credential detection, the Pinyin fallback table and composer, and the shared
  CJK font chain.
- `switch2_lifecycle` (`tests/switch2_lifecycle_test.cpp`) checks the console
  lifecycle decisions that do not need hardware: the framebuffer fitted from the
  reported output mode (1080p handheld passes through, a 4K docked report is
  fitted while keeping its aspect ratio, nothing is stretched), refresh word
  decoding and the diagnostics line that keeps render size, refresh rate and
  output mode apart, the presentation pacer (full rate while busy, 30 Hz after a
  quiet second, never overriding a frame rate the user pinned), the
  pre-playback probe state machine and its sanitized summary (including a probe
  that skips the audio route while playback owns it), the 2 s `nifm` network
  watch that turns a reconnect or an address change into a socket and DNS reset,
  and the teardown order the platform destructor follows.

## Switch 2 SDK capability review

Verified against the built image by reading the installed headers and packages,
not from documentation. `aarch64-none-elf-g++ (devkitA64) 15.2.0`, newlib
`4.6.0.20260123`, `libounce`/`ouncevk`/`libkaihen` `20261117.1-1`.

| Capability | Verified API | Use in this port |
|---|---|---|
| Joy-Con 2 mouse / pointer | `switch/runtime/pad.h`: `PadPointer` (`active`, `side`, `x`, `y`, `delta_x`, `delta_y`, `wheel_delta_y`; there is no `engaged` field), `PadPointerSide_Left/Right`, `padPointerInitialize(PadPointer*, int width, int height)`, `padPointerUpdate(pad)` per frame, `padPointerGetButtons(PadPointer*)` returning `HidMouseButton` bits (not `HidNpadButton_*`); `switch2/services/nmouse.h`: `HidNmouseState`, `HidNmouseStateWithDelta`, `hidInitializeNmouse(buffer, size, width, height, pointer_type)`, `hidGetNmouseHandles`, `hidGetNmouseStates`, `HidNmouseButtonConfigJoyLeftJcms/JoyRightJcms/StandardMouse` | Feeds Borealis' mouse abstraction (pointer, drag, scroll) for gate G1. Both the Joy-Con 2 pointer and a standard USB mouse are covered |
| Display and refresh | `switch2/services/vi2.h` (`vi2QueryDisplayRefreshRate`, `vi2QueryDisplayModeCapability`, `vi2SetDisplay120HzCapability`), `appletGetDefaultDisplayResolution` + its change event, `appletGetOperationMode`, `switch2/services/gsv.h`, `switch/display/{native_window,framebuffer,buffer_producer,binder,parcel}.h` | SDL2 window on an EGL surface; dock/undock and refresh reporting are wired in `platform/switch2/switch2_display.cpp`. `vi2SetDisplay120HzCapability` only declares the capability to VI2; the read-back rate drives the diagnostics and the idle pacing, and 120 Hz presentation through OunceVK/Zink is a gate G1 item on the console, see `docs/g1-checklist.md` |
| GPU / graphics | OunceVK `portlibs/switch2/lib/libEGL.a` (EGL, OpenGL, GLES 3.1 through Zink), `libvulkan.a` (Mesa NVK, Vulkan 1.4), empty `libGLESv2.a`/`libglapi.a`/`libdrm_nouveau.a` stubs; pkg-config `egl`, `glesv2`, `gl`, `vulkan`, `libdrm_nouveau` version `26.2.2` | The existing MPVCore framebuffer GL path and Borealis' nanovg GL driver, unchanged |
| Hardware video decode | `switch2/services/viddec.h`: `ViddecCodec`, `ViddecOutputFormat`, `ViddecTilingMode`, `viddecOpenDecoder`, `viddecDecoder{Initialize,Start,Stop,Flush,Submit,AcquireImage,GetOutputProperties,ReleaseImage}`, NV12 image pools (`viddecImagePoolCreate`, `viddecImagePoolReadNv12`, `viddecCopyImageNv12`) with `GsvNativeSync` producer/consumer fences | Deferred: software decoding is the baseline (ADR 0001). The zero-copy or bounded-copy path is the post-G2 hardware track, decided at G3 |
| Audio | `switch/audio/audio.h`, `switch/audio/driver.h` | SDL2 audio output through the libnx-compatible driver; no change needed for the software baseline |
| Lifecycle and applets | `switch/services/applet.h`, `switch/applets/swkbd.h` (`swkbdCreate`, `swkbdConfig`, `SwkbdType_All`), `switch2/services/audu.h` | Applet and lifecycle hooks feed the Borealis Switch platform; the system keyboard covers Chinese input, with an in-app Pinyin fallback when it cannot start |
| Shared fonts | `switch/services/pl.h`: `plGetSharedFont`, `plGetSharedFontByType` | CJK glyph fallback from the system font service, so no console font binary has to be redistributed |
| kaiHEN | `libkaihen` (`kaihen.h`, `libkaihen.a`, `libkaihen_plugin.a`, `kaihen-jit.o`) | Not used. No plugin, JIT or gaming-only feature is imported |

Deferred with a reason:

- Vulkan nanovg backend: `ollix/nanovg-vulkan` is not reachable (HTTP 404), and
  borealis has no Vulkan driver to plug it into.
- mpv Vulkan render API: mpv's public `render.h` still has no caller-owned
  Vulkan render path (`mpv#18343` is open), so MPVCore keeps EGL/GLES.
- `VideoDecoder` zero-copy GPU import: needs the G2 benchmark first (G3).
- GLES3 contexts: available through Zink, but only worth switching once a device
  run shows a benefit.
