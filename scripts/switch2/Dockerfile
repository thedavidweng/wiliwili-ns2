# Switch 2 build container for wiliwili-NS2.
#
# devkitA64 from devkitPro, the official [ounce] devkit (libounce, OunceVK,
# libkaihen) from https://pkg.ouncebrew.org, dav1d from switch-portlibs, and
# FFmpeg 7.1.5 (with ffmpeg-ounce.patch) + libmpv 0.36.0 compiled against
# libounce into $DEVKITPRO/portlibs/switch2.
#
#   docker build -f scripts/switch2/Dockerfile -t wiliwili-ns2-toolchain:local .
#
# Everything is pinned: the base image by digest, the [ounce] signing key by
# fingerprint, and every package/source/patch by commit and sha256. Bump them
# together with UPSTREAM.json.
FROM docker.io/devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

ARG OUNCE_REPO_URL=https://pkg.ouncebrew.org
ARG OUNCE_KEY_FINGERPRINT=DFEAE7FA965D1E6C0D41D4D75DB9FDBFA4A4DE29
ARG SWITCH_PORTLIBS_URL=https://github.com/dragonflylee/mpv-build/releases/download/switch-portlibs
ARG MPV_BUILD_COMMIT=a04eb13eda5860b38d7a7cd738c10b0edbd0596f

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates curl meson ninja-build patch xz-utils zstd && \
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
        switch-libass switch-libjpeg-turbo && \
    dkp-pacman -Scc --noconfirm

# Software AV1 fallback codec (dav1d) from the pinned Switch portlibs release.
RUN set -eux; \
    cd /tmp; \
    curl -fsSLO "${SWITCH_PORTLIBS_URL}/switch-dav1d-1.5.5-1-any.pkg.tar.zst"; \
    echo "e7614ec10b8d017379446bcdbf3e741becf14ae9a55c2cf5d5bba191f200f96b  switch-dav1d-1.5.5-1-any.pkg.tar.zst" | sha256sum -c -; \
    dkp-pacman -U --noconfirm --overwrite '/opt/devkitpro/portlibs/switch/*' ./switch-dav1d-1.5.5-1-any.pkg.tar.zst; \
    rm -f ./switch-dav1d-1.5.5-1-any.pkg.tar.zst; \
    dkp-pacman -Scc --noconfirm

# Switch 2 FFmpeg 7.1.5 (with libounce VideoDecoder h264_ounce wrapper, nvtegra
# disabled) + libmpv 0.36.0 installed into $DEVKITPRO/portlibs/switch2 so
# Switch2.cmake resolves them ahead of Switch 1 portlibs.
COPY scripts/switch2/ffmpeg/ffmpeg-ounce.patch /tmp/ffmpeg-ounce.patch
RUN set -eux; \
    mkdir -p /tmp/media-build; \
    cd /tmp/media-build; \
    curl -fsSLO "https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz"; \
    echo "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f  ffmpeg-7.1.5.tar.xz" | sha256sum -c -; \
    MPV_BUILD_RAW="https://raw.githubusercontent.com/dragonflylee/mpv-build/${MPV_BUILD_COMMIT}"; \
    curl -fsSLO "${MPV_BUILD_RAW}/ffmpeg/ffmpeg-7.1.5.patch"; \
    echo "e4fe0eeaec9f721f7c79dd484433882da807e7f3925295d56ff001232cc62845  ffmpeg-7.1.5.patch" | sha256sum -c -; \
    curl -fsSLO "${MPV_BUILD_RAW}/ffmpeg/getnameinfo.patch"; \
    echo "150c56eff36b1179f5409bf2e30fbf6996ce603ba6c365b6bf94928f116c97fa  getnameinfo.patch" | sha256sum -c -; \
    curl -fsSL -o mpv-0.36.0.tar.gz "https://github.com/mpv-player/mpv/archive/v0.36.0.tar.gz"; \
    echo "29abc44f8ebee013bb2f9fe14d80b30db19b534c679056e4851ceadf5a5e8bf6  mpv-0.36.0.tar.gz" | sha256sum -c -; \
    curl -fsSLO "${MPV_BUILD_RAW}/mpv/deko3d.patch"; \
    echo "56256e0eb9dc9d74c2fe13350502efdeb4b0be67ca23961a6a75073a0b9c7a6e  deko3d.patch" | sha256sum -c -; \
    tar -xf ffmpeg-7.1.5.tar.xz; \
    cd ffmpeg-7.1.5; \
    patch -Np1 -i ../ffmpeg-7.1.5.patch; \
    patch -Np1 -i ../getnameinfo.patch; \
    patch -Np1 -i /tmp/ffmpeg-ounce.patch; \
    . /opt/devkitpro/switch2vars.sh; \
    export PKG_CONFIG_LIBDIR="${PORTLIBS_PREFIX}/lib/pkgconfig:${DEVKITPRO}/portlibs/switch/lib/pkgconfig"; \
    ./configure \
        --prefix="${PORTLIBS_PREFIX}" --enable-gpl --disable-shared --enable-static \
        --cross-prefix=aarch64-none-elf- --enable-cross-compile \
        --arch=aarch64 --cpu=cortex-a78c --target-os=horizon --enable-pic \
        --extra-cflags="-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE -O2 ${ARCH} -I${DEVKITPRO}/portlibs/switch/include -isystem ${DEVKITPRO}/libounce/include" \
        --extra-cxxflags="-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE -O2 ${ARCH} -I${DEVKITPRO}/portlibs/switch/include -isystem ${DEVKITPRO}/libounce/include" \
        --extra-ldflags="-fPIE -L${PORTLIBS_PREFIX}/lib -L${DEVKITPRO}/portlibs/switch/lib -L${DEVKITPRO}/libounce/lib" \
        --disable-runtime-cpudetect --disable-programs --disable-debug --disable-doc --disable-autodetect \
        --enable-asm --enable-neon --disable-avdevice --disable-encoders --disable-muxers \
        --enable-swscale --enable-swresample --enable-network \
        --disable-decoders --enable-decoder=flac,aac,opus,mp3,h264,hevc,vp8,vp9,mjpeg,libdav1d,h264_ounce \
        --disable-demuxers --enable-demuxer=mov,flv,hls \
        --disable-protocols --enable-protocol=file,http,tcp,udp,hls,https,tls,rtp,crypto,httpproxy \
        --enable-zlib --enable-bzlib --enable-libdav1d --enable-mbedtls --enable-version3 \
        --disable-nvtegra --enable-ounce \
        --disable-filters --enable-filter=hflip,vflip,transpose; \
    make -j"$(nproc)"; \
    make install; \
    cd /tmp/media-build; \
    tar -xf mpv-0.36.0.tar.gz; \
    cd mpv-0.36.0; \
    patch -Np1 -i ../deko3d.patch; \
    python3 -c "import os,shutil; p=os.environ['PORTLIBS_PREFIX']; d=os.environ['DEVKITPRO']; arch=os.environ['ARCH'].split(); cf=['-D__SWITCH__','-D__SWITCH2__',f'-I{p}/include',f'-I{d}/portlibs/switch/include','-isystem',f'{d}/libounce/include']+arch+['-O2','-ffunction-sections','-fdata-sections']; lf=arch+[f'-L{p}/lib',f'-L{d}/portlibs/switch/lib',f'-L{d}/libounce/lib','-lounce']; pkg=shutil.which('aarch64-none-elf-pkg-config') or shutil.which('pkg-config'); open('/tmp/media-build/switch2-cross.txt','w').write(f'[binaries]\nc = {shutil.which(\"aarch64-none-elf-gcc\")!r}\ncpp = {shutil.which(\"aarch64-none-elf-g++\")!r}\nar = {shutil.which(\"aarch64-none-elf-gcc-ar\")!r}\nstrip = {shutil.which(\"aarch64-none-elf-strip\")!r}\npkgconfig = {pkg!r}\n\n[built-in options]\nc_args = {cf!r}\nc_link_args = {lf!r}\ncpp_args = {cf!r}\ncpp_link_args = {lf!r}\n\n[host_machine]\nsystem = \'horizon\'\ncpu_family = \'aarch64\'\ncpu = \'cortex-a78c\'\nendian = \'little\'\n\n[target_machine]\nsystem = \'horizon\'\ncpu_family = \'aarch64\'\ncpu = \'cortex-a78c\'\nendian = \'little\'\n')" ; \
    meson setup --buildtype=plain --cross-file=/tmp/media-build/switch2-cross.txt \
        --default-library=static --prefix="${PORTLIBS_PREFIX}" --libdir=lib \
        -Dlibmpv=true -Dcplayer=false -Dtests=false -Dlua=disabled -Ddeko3d=disabled \
        -Dplain-gl=enabled -Dsdl2=disabled -Dlibarchive=disabled -Dlibplacebo=disabled \
        build .; \
    meson compile -C build -j"$(nproc)"; \
    meson install -C build; \
    rm -rf /tmp/media-build /tmp/ffmpeg-ounce.patch

# Record what the image contains, so a report can name the exact toolchain.
RUN { aarch64-none-elf-g++ --version | head -1; \
      dkp-pacman -Q libounce ouncevk libkaihen devkitA64 \
          switch-dav1d switch-sdl2; \
      echo "switch2-ffmpeg 7.1.5+ounce"; \
      echo "switch2-libmpv 0.36.0"; \
    } > /opt/devkitpro/wiliwili-ns2-toolchain.txt

ENV DEVKITPRO=/opt/devkitpro
WORKDIR /src
