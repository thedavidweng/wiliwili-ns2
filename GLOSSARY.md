# wiliwili-NS2

The Switch 2 port of wiliwili: a controller-first Bilibili client built on the upstream C++ codebase. This glossary fixes the vocabulary shared by the spec, tickets and code so the port does not invent parallel terms for concepts the upstream already has.

## Content identity

**BV id**:
Bilibili's current alphanumeric video identifier (code field `bvid`).
_Avoid_: video id, bvid

**AV id**:
The legacy numeric video identifier still carried by API results (code field `aid`).
_Avoid_: legacy id, aid

**CID**:
The identifier of one playable content stream or part, distinct from the video identifier.
_Avoid_: content id, part id

**Part**:
One numbered segment of a multi-part user upload (`VideoDetailPage`, 分P).
_Avoid_: 分P, page, episode

## Series and people

**Season (PGC)**:
Bilibili's official episodic-series entity for professionally produced content, addressed by `season_id`.
_Avoid_: series, bangumi (as the entity)

**Episode**:
One playable installment inside a Season.
_Avoid_: part, video

**UGC season**:
A user-assembled collection of a creator's own videos; season-shaped but not PGC.
_Avoid_: 合集, collection, season (unqualified)

**Bangumi**:
The UI category for episodic animation (追番).
_Avoid_: anime, season

**Cinema**:
The UI category for film and television PGC content.
_Avoid_: movies, theatre

**Creator**:
The author of a user-uploaded video.
_Avoid_: UP主, uploader, owner

**Streamer**:
The broadcaster attached to a live room.
_Avoid_: 主播, creator (when referring to live)

**Triple**:
The combined like, coin and favorite action on one video.
_Avoid_: 三连, like

## Danmaku

**Danmaku**:
Timed user comments overlaid on a playing video.
_Avoid_: bullet comment, dm

**Live danmaku**:
The chat-message stream of a live room, received over WebSocket.
_Avoid_: live chat, bullet comment

**BAS danmaku**:
The script-driven advanced danmaku variant using Bilibili Animation Script.
_Avoid_: advanced danmaku

**Smart mask**:
An image mask that keeps danmaku off on-screen subtitles and text.
_Avoid_: dm_mask, subtitle mask

**Danmaku filter**:
One independent visibility rule set — top, bottom, scroll, color, advanced or level — for overlay comments.
_Avoid_: danmaku setting, block list

## API and account

**WBI signature**:
Bilibili's web-side request authentication scheme, required for `/x/web-interface/` endpoints.
_Avoid_: WBI, web sign

**QN**:
The numeric quality code requested from playurl APIs, such as 80 for 1080P and 116 for 1080P60; display labels are not interchangeable with codes.
_Avoid_: quality code, resolution, quality level

**Session**:
The persisted cookie and refreshToken pair that keeps a user logged in without rescanning a QR code.
_Avoid_: credentials, login token, account

**CSRF token**:
The `bili_jct` cookie field that authorizes Bilibili write actions.
_Avoid_: csrf, jct

**DASH**:
Bilibili's playback form that delivers video and audio as separate streams.
_Avoid_: adaptive streaming, HLS, separate streams

## Playback and UI

**Playback core**:
The app-wide playback engine (code name MPVCore) through which load, seek, pause and status flow.
_Avoid_: player module, decoder adapter

**UI framework**:
The Borealis-based framework (code name Borealis) that owns views, focus and navigation.
_Avoid_: GUI toolkit, GUI library

**Shader profile**:
A user-defined Anime4K profile together with its per-anime bindings.
_Avoid_: shader pack, Anime4K profile

**Watch Later**:
The personal queue of videos saved for later viewing.
_Avoid_: 稍后再看, later list

## Switch 2 port

**kaiHEN**:
The pre-existing homebrew environment and launcher on the target Switch 2, which the port assumes without modifying.
_Avoid_: exploit, jailbreak, CFW

**NS2 NRO**:
The single installable Switch 2 binary produced for release, distinct from the upstream Switch 1 build.
_Avoid_: NRO (unqualified), NSP forwarder, package

**libounce**:
The Switch 2 platform API layer, libnx-compatible, exposing PadPointer/Nmouse, VideoDecoder, swkbd and shared fonts to this port.
_Avoid_: libnx, Switch 1 SDK, devkitPro portlibs

**PadPointer / Nmouse**:
libounce's native Switch 2 optical Joy-Con 2 mouse and pointer interface (`padPointer*` over `hid*Nmouse`), integrated into the Borealis mouse abstraction rather than treated as touch input.
_Avoid_: simulated touch, emulated cursor, generic SDL mouse

**OunceVK**:
The Switch 2 Mesa NVK (Vulkan 1.4) and Zink (EGL/OpenGL/GLES) graphics runtime used by the port.
_Avoid_: deko3d, Switch 1 GPU libraries

**Shared font fallback**:
Reading CJK glyphs from system font services at runtime instead of redistributing console font binaries.
_Avoid_: bundled font, system font binary, static UI-subset font

## Port gates

**Device gate**:
A mandatory real-console acceptance decision whose evidence must be supplied by a human tester against a specific NRO SHA-256.
_Avoid_: CI check, smoke test, synthetic screenshot

**G1**:
The UI and platform device gate covering boot, interface rendering, controller, touch and Joy-Con 2 mouse input, Chinese input, fonts, dynamic display modes and lifecycle; it blocks Phase 2.
_Avoid_: boot test, UI smoke test

**G2**:
The software-playback and performance-baseline device gate requiring continuous authorized 1080p DASH playback with audio, danmaku, mouse-driven player/timeline control and p50/p95/p99 frame-time benchmarks; it blocks hardware R&D, native Vulkan research and release.
_Avoid_: playback test, decode benchmark, 720p diagnostic

**G3**:
The human decision gate that permits merging the libounce VideoDecoder hardware path (bounded-copy or GPU-imported) on measured benefit and acceptable maintenance cost.
_Avoid_: hardware feature gate, performance review

**Software baseline**:
The pinned FFmpeg 7.1 and mpv 0.36.0 software-decoding configuration that remains the known-good compatibility and recovery path after G2.
_Avoid_: legacy path

**Hardware track**:
The prioritized post-G2 branch investigating libounce VideoDecoder and frame transport, kept separate until G3 approval so it cannot destabilize the software baseline.
_Avoid_: second app

**Zero-copy interop**:
Passing decoded VideoDecoder surfaces to EGL/GLES or Vulkan without CPU readback; an optimization goal to be proven rather than assumed or treated as the sole acceptable hardware-decode path.
_Avoid_: native frame interop, hardware decoding
