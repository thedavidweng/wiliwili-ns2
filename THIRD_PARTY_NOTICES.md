# Third-Party Notices and Corresponding Source (`wiliwili-plus`)

## License and Corresponding Source

`wiliwili-plus` (including Nintendo Switch 2, Xbox / Windows UWP, and desktop builds) is distributed under the **GNU General Public License v3.0 (GPL-3.0)**. See [`LICENSE`](LICENSE).

All corresponding build recipes, container definitions, package pins, patches, and platform glue required to rebuild distributed binaries from source are hosted in this repository:

- Exact upstream/reference commits, container digests, and portlib SHA-256 pins: [`UPSTREAM.json`](UPSTREAM.json)
- Nintendo Switch 2 toolchain container & audit/packaging scripts: [`scripts/switch2/Dockerfile`](scripts/switch2/Dockerfile), [`scripts/switch2/build.sh`](scripts/switch2/build.sh), [`scripts/switch2/audit-nro.sh`](scripts/switch2/audit-nro.sh), [`scripts/switch2/package-release.sh`](scripts/switch2/package-release.sh)
- Switch 2 platform backend: [`platform/switch2/`](platform/switch2/)
- Xbox / Windows UWP build & WinRT packaging: [`platform/uwp/`](platform/uwp/)

## Upstream and Reference Projects

| Project | Attribution / Role | License / Reference |
|---|---|---|
| [`xfangfang/wiliwili`](https://github.com/xfangfang/wiliwili) | Core cross-platform Bilibili client (`v1.6.0`) | GPL-3.0 |
| [`dragonflylee/wiliwili`](https://github.com/dragonflylee/wiliwili) | Backported playback/API fixes and [`dragonflylee/mpv-build`](https://github.com/dragonflylee/mpv-build) Switch media portlib recipes | GPL-3.0 |
| [`ikas-mc/borealis`](https://github.com/ikas-mc/borealis) (`winrt-dev`) | WinRT / UWP Borealis fork basis (maintained at [`thedavidweng/borealis`](https://github.com/thedavidweng/borealis)) | Apache-2.0 / upstream notices |
| [`MrTangLuyao/Bilibili-thread-ripper`](https://github.com/MrTangLuyao/Bilibili-thread-ripper) & `SukkaW/Make-Bilibili-Great-Than-Ever-Before` (#31) | Multi-CDN candidate classification and concurrent Range strategy inspiration | Reference |
| [`HDErick/OpenBLOPS-NX`](https://github.com/HDErick/OpenBLOPS-NX) | Reference for Switch `Nmouse` / PadPointer button arbitration only | GPL-3.0 |
| [`Sank-kab/KaiTube`](https://github.com/Sank-kab/KaiTube) & [`mizuiroame/kaivideo`](https://github.com/mizuiroame/kaivideo) | Switch 2 devkit path redirection and VideoDecoder boundary references | Reference |

## Nintendo Switch 2 `[ounce]` Toolchain & System Libraries

Exact versions pinned in [`UPSTREAM.json`](UPSTREAM.json):

| Component | Repository | License |
|---|---|---|
| `devkitA64` + `ounce-devkit` | [`ouncebrew/ounce-devkit`](https://github.com/ouncebrew/ounce-devkit) | MIT / devkitPro toolchains |
| `libounce` | [`ouncebrew/libounce`](https://github.com/ouncebrew/libounce) | ISC |
| `ouncevk` (Mesa NVK + Zink) | [`ouncebrew/ouncevk`](https://github.com/ouncebrew/ouncevk) | MIT (Mesa drivers) |
| `libkaihen` / kaiHEN | [`ouncebrew/kaihen`](https://github.com/ouncebrew/kaihen) | GPL-2.0 |

## Core Bundled & Linked Libraries

- **borealis** (`library/borealis`): Hardware-accelerated UI framework rendered via NanoVG (upstream `xfangfang/borealis` `wiliwili` fork on Desktop/Switch 2; `thedavidweng/borealis` `winrt-dev` on UWP).
- **FFmpeg (`switch-ffmpeg`) & libmpv (`switch-libmpv`)**: Core media demuxing, decoding, and video output pipeline (`mpv` GPLv2+/LGPLv2.1+, `FFmpeg` LGPL/GPL components matching upstream `dragonflylee/mpv-build` portlib builds, plus `switch-dav1d`, `switch-libass`, `switch-libplacebo`).
- **cpr (`library/cpr`) & libcurl**: HTTP/HTTPS client layer.
- **nlohmann/json**, **OpenCC (`library/OpenCC`)**, **mongoose (`library/mongoose`)**, **fmt**, **pystring (`library/pystring`)**, **lunasvg (`library/lunasvg`)**, **QR-Code-generator (`library/QR-Code-generator`)**, **MemoryModule (`library/MemoryModule`)**, **libwebp**, **libusb / SDL2**.
