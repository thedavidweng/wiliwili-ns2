#!/bin/sh
# Audit a Switch 2 build tree: the NRO, its RomFS and the linker map.
#
#   scripts/switch2/audit-nro.sh [build-dir]
#
# Run inside the toolchain container (scripts/switch2/build.sh audit). It exits
# 77 without the cross toolchain or without a build tree, which ctest reports as
# a skip, so a desktop-only checkout stays green.
set -u

build_dir="${1:-build/switch2-release}"
elf="$build_dir/wiliwili-NS2.elf"
nro="$build_dir/wiliwili-NS2.nro"
nacp="$build_dir/wiliwili-NS2.nacp"
map="$build_dir/wiliwili.map"
romfs="$build_dir/resources"

skip() {
    echo "SKIP: $*"
    exit 77
}

readelf="$(command -v aarch64-none-elf-readelf 2>/dev/null || true)"
if [ -z "$readelf" ] && [ -n "${DEVKITPRO:-}" ] && [ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-readelf" ]; then
    readelf="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-readelf"
fi
[ -n "$readelf" ] || skip "aarch64-none-elf-readelf is not on PATH; run this inside wiliwili-ns2-toolchain"

for artifact in "$elf" "$nro" "$nacp" "$map"; do
    [ -e "$artifact" ] || skip "no build artifacts at $build_dir"
done

failures=0
fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}
ok() { echo "ok: $*"; }

# 1. The NRO: header magic, plausible size, embedded NACP and RomFS.
#    elf2nro writes a 0x10 byte homebrew header ("HOMEBREW" then the NRO0 magic
#    at 0x10); devkitPro's RomFS has no magic of its own, so the resource names
#    it stores are what shows it is there.
header="$(dd if="$nro" bs=1 skip=8 count=12 2>/dev/null)"
if [ "$header" = "HOMEBREWNRO0" ]; then
    ok "NRO header (HOMEBREWNRO0)"
else
    fail "$nro is not an NRO"
fi

nro_size="$(wc -c <"$nro" | tr -d ' ')"
if [ "$nro_size" -gt 1048576 ]; then
    ok "NRO is $nro_size bytes"
else
    fail "NRO is only $nro_size bytes: the RomFS is probably missing"
fi

# The author string exists only in the NACP, so finding it proves the metadata
# was embedded rather than left next to the NRO.
if LC_ALL=C grep -a -q 'xfangfang and wiliwili-NS2 contributors' "$nro"; then
    ok "NACP metadata is embedded"
else
    fail "no NACP metadata in the NRO"
fi

nacp_title="$(dd if="$nacp" bs=1 count=32 2>/dev/null | tr -d '\000')"
if [ "$nacp_title" = "wiliwili-NS2" ]; then
    ok "NACP title is wiliwili-NS2"
else
    fail "NACP title is '$nacp_title'"
fi

nacp_author="$(dd if="$nacp" bs=1 skip=512 count=48 2>/dev/null | tr -d '\000')"
case "$nacp_author" in
    'xfangfang and wiliwili-NS2 contributors'*) ok "NACP author keeps the upstream attribution" ;;
    *) fail "NACP author is '$nacp_author'" ;;
esac

# The version strings sit after the 16 language slots of title and author, with
# the tool's own trailer around them, so filter for a version-shaped string.
nacp_version="$(dd if="$nacp" bs=1 skip=12288 count=256 2>/dev/null \
    | LC_ALL=C tr -c '0-9.' '\n' \
    | LC_ALL=C grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1)"
case "$nacp_version" in
    [0-9]*.[0-9]*.[0-9]*) ok "NACP version is $nacp_version" ;;
    *) fail "NACP has no version string" ;;
esac

romfs_shipped=0
for entry in 's2t.json' 'MaterialIcons-Regular.ttf' 'cursor.png' 'wiliwili.json'; do
    if LC_ALL=C grep -a -q "$entry" "$nro"; then
        romfs_shipped=$((romfs_shipped + 1))
    else
        fail "RomFS is missing $entry"
    fi
done
if [ "$romfs_shipped" -eq 4 ]; then
    ok "RomFS image is embedded"
fi

# 2. The unstripped ELF keeps the symbols the NRO cannot carry.
if "$readelf" -S "$elf" | grep -q '\.debug_info'; then
    ok "ELF keeps .debug_info"
else
    fail "$elf has no debug information"
fi

if "$readelf" -S "$elf" | grep -q '\.symtab'; then
    ok "ELF keeps .symtab"
else
    fail "$elf has no symbol table"
fi

# 3. The link: libnx API from libounce, EGL/GLES from OunceVK, nothing from
#    Switch 1's libnx or GPU portlibs.
if grep -q '/libounce/lib/libnx\.a' "$map"; then
    ok "libnx API comes from libounce"
else
    fail "$map does not link /opt/devkitpro/libounce/lib/libnx.a"
fi

if grep -q 'libouncevk-gl\.a' "$map"; then
    ok "EGL/GLES come from OunceVK (libouncevk-gl)"
else
    fail "$map does not link OunceVK's GL stack"
fi

if grep -q '__wrap_cpu_features_get_time_usec' "$map"; then
    ok "cpu_features_get_time_usec wrapped for Switch 2 31.25 MHz counter"
else
    fail "$map does not wrap cpu_features_get_time_usec"
fi

objdump="${readelf%readelf}objdump"
nm_bin="${readelf%readelf}nm"
if [ -x "$objdump" ] && [ -x "$nm_bin" ]; then
    mpv_ctor="$("$nm_bin" "$elf" | awk '/ _ZN7MPVCoreC1Ev$/{print $1; exit}')"
    if [ -n "$mpv_ctor" ] && "$objdump" -d --start-address="0x$mpv_ctor" --stop-address="$((0x$mpv_ctor + 0x180))" "$elf" | grep -Eq 'mov[[:space:]]+w[0-9]+, #0xc([[:space:]]|$)'; then
        ok "MPVCore disables MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME (#0xc) in ELF"
    else
        fail "MPVCore::MPVCore() does not set MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME (#0xc)"
    fi
fi

if grep -q '/opt/devkitpro/libnx/' "$map"; then
    fail "$map links Switch 1 libnx archives:"
    grep -o '/opt/devkitpro/libnx/[^ ]*' "$map" | sort -u | sed 's/^/      /'
else
    ok "no Switch 1 libnx archives"
fi

if grep -q 'libdeko3d' "$map"; then
    fail "$map links libdeko3d"
else
    ok "no deko3d"
fi

# nvtegra and the NVIDIA user mode driver are Switch 1's GPU libraries, linked
# from libnx on a deko3d build. The pinned Switch FFmpeg carries its own nvtegra
# hwaccel objects (libavcodec.a(nvtegra_*.o)), which is a codec wrapper inside
# that FFmpeg build rather than a GPU library: nothing here may link the driver
# itself.
switch1_nv="$(grep -oE 'lib(nvtegra|nvidia|nvdrv)\.a' "$map" | sort -u)"
if [ -n "$switch1_nv" ]; then
    fail "Switch 1 NVIDIA libraries in the link map:"
    echo "$switch1_nv" | sed 's/^/      /'
else
    ok "no nvtegra, nvidia or nvdrv libraries"
fi

switch1_gpu="$(grep -oE '/opt/devkitpro/portlibs/switch/lib/lib(EGL|GLESv2|glapi|drm_nouveau|vulkan)\.a' "$map" | sort -u)"
if [ -n "$switch1_gpu" ]; then
    fail "Switch 1 GPU archives in the link map:"
    echo "$switch1_gpu" | sed 's/^/      /'
else
    ok "no Switch 1 GPU archives"
fi

# 4. The RomFS: the UI framework's assets, wiliwili's assets, and none of the
#    Switch 1 proprietary or dead weight files.
required="img/sys/cursor.png material/MaterialIcons-Regular.ttf xml/activity i18n/zh-Hans/wiliwili.json opencc/s2t.json"
for entry in $required; do
    if [ -e "$romfs/$entry" ]; then
        ok "RomFS has $entry"
    else
        fail "RomFS is missing $entry"
    fi
done

forbidden="font/switch_font.ttf font/switch_icons.ttf font/keymap_xbox.ttf font/keymap_ps.ttf font/keymap_keyboard.ttf font/keymap_keyboard_swap.ttf wiliwili_config.json"
for entry in $forbidden; do
    if [ -e "$romfs/$entry" ]; then
        fail "RomFS still ships $entry"
    fi
done

proprietary="$(find "$romfs" \( -name '*.bfttf' -o -name 'switch_font*.ttf' -o -name 'keymap_*.ttf' \) 2>/dev/null)"
if [ -n "$proprietary" ]; then
    fail "proprietary Switch 1 fonts in the RomFS:"
    echo "$proprietary" | sed 's/^/      /'
else
    ok "no proprietary fonts in the RomFS"
fi

if [ -z "$(find "$romfs" -name 'wiliwili_config.json' 2>/dev/null)" ]; then
    ok "no settings file in the RomFS"
fi

if LC_ALL=C grep -r -q '/Users/' "$romfs" 2>/dev/null; then
    fail "developer home paths in the RomFS"
else
    ok "no developer home paths in the RomFS"
fi

# 5. Nothing that reports to a third party: this port builds with NO_GA, so even
#    the endpoint string is gone.
if LC_ALL=C grep -a -q 'google-analytics\.com' "$elf"; then
    fail "$elf still contains the Google Analytics endpoint"
else
    ok "no telemetry endpoint in the ELF"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "Switch 2 artifact audit passed"
