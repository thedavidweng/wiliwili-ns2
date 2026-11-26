# Switch 2 build container for wiliwili-NS2.
#
# devkitA64 from devkitPro, the official [ounce] devkit (libounce, OunceVK,
# libkaihen) from https://pkg.ouncebrew.org, and the FFmpeg 7.1 / dav1d /
# mpv 0.36.0 portlibs the software-decoding baseline is pinned to.
#
#   docker build -f scripts/switch2/Dockerfile -t wiliwili-ns2-toolchain:local .
#
# Everything is pinned: the base image by digest, the [ounce] signing key by
# fingerprint, and the portlib packages by sha256. Bump them together with
# UPSTREAM.json.
FROM docker.io/devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

ARG OUNCE_REPO_URL=https://pkg.ouncebrew.org
ARG OUNCE_KEY_FINGERPRINT=DFEAE7FA965D1E6C0D41D4D75DB9FDBFA4A4DE29
ARG SWITCH_PORTLIBS_URL=https://github.com/dragonflylee/mpv-build/releases/download/switch-portlibs

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates curl zstd && \
    rm -rf /var/lib/apt/lists/*

# The [ounce] repository. Its signing key is imported only after the
# fingerprint matches the one ouncebrew/ounce-devkit's install.sh pins, and
# that key alone is trusted for the repository.
RUN set -eux; \
    curl -fsSL -o /tmp/ounce.gpg "${OUNCE_REPO_URL}/ounce.gpg"; \
    fingerprint="$(gpg --with-colons --show-keys /tmp/ounce.gpg | awk -F: '/^fpr:/ { print $10; exit }')"; \
    test "${fingerprint}" = "${OUNCE_KEY_FINGERPRINT}"; \
    dkp-pacman-key --add /tmp/ounce.gpg; \
    dkp-pacman-key --lsign-key "${OUNCE_KEY_FINGERPRINT}"; \
    rm -f /tmp/ounce.gpg; \
    printf '\n[ounce]\nServer = %s\n' "${OUNCE_REPO_URL}" >> "${DEVKITPRO}/pacman/etc/pacman.conf"

# The Switch 2 devkit plus the devkitPro packages wiliwili and the UI
# framework need. libounce installs switch2vars.sh, the CMake toolchain and
# $DEVKITPRO/libounce; OunceVK installs Vulkan, EGL, OpenGL and OpenGL ES in
# $DEVKITPRO/portlibs/switch2.
RUN dkp-pacman -Sy --noconfirm --needed \
        --overwrite '/opt/devkitpro/libounce/*' \
        --overwrite '/opt/devkitpro/portlibs/switch2/*' \
        --overwrite /opt/devkitpro/switch2vars.sh \
        libounce ouncevk libkaihen \
        switch-dev switch-cmake switch-pkg-config dkp-toolchain-vars \
        switch-zlib switch-bzip2 switch-mbedtls switch-sdl2 switch-curl \
        switch-libwebp switch-freetype switch-harfbuzz switch-libfribidi \
        switch-libass && \
    dkp-pacman -Scc --noconfirm

# The software-decoding baseline: FFmpeg 7.1.x with dav1d, and mpv 0.36.0.
# They live in the Switch 1 portlibs because they only use the C library and
# the libnx-compatible API, and MPVCore only reaches their software paths.
RUN set -eux; \
    cd /tmp; \
    for pinned in \
        "switch-dav1d-1.5.5-1-any.pkg.tar.zst e7614ec10b8d017379446bcdbf3e741becf14ae9a55c2cf5d5bba191f200f96b" \
        "switch-ffmpeg-7.1.5-5-any.pkg.tar.zst f79966b9fb75188393cc64da4d24a30500f46ebd991a8f373bc2fcafbf48032c" \
        "switch-libmpv-0.36.0-2-any.pkg.tar.zst 1af27369a6c19dcb0e53a6748914a18e12d780a85c6585e680035604065450cf"; \
    do \
        set -- ${pinned}; \
        curl -fsSLO "${SWITCH_PORTLIBS_URL}/$1"; \
        echo "$2  $1" | sha256sum -c -; \
    done; \
    dkp-pacman -U --noconfirm --overwrite '/opt/devkitpro/portlibs/switch/*' ./*.pkg.tar.zst; \
    rm -f ./*.pkg.tar.zst; \
    dkp-pacman -Scc --noconfirm

# Record what the image contains, so a report can name the exact toolchain.
RUN { aarch64-none-elf-g++ --version | head -1; \
      dkp-pacman -Q libounce ouncevk libkaihen devkitA64 \
          switch-dav1d switch-ffmpeg switch-libmpv switch-sdl2; \
    } > /opt/devkitpro/wiliwili-ns2-toolchain.txt

ENV DEVKITPRO=/opt/devkitpro
WORKDIR /src
