#!/bin/sh
# Build the Switch 2 toolchain image and the NS2 NRO inside it.
#
#   scripts/switch2/build.sh image       build wiliwili-ns2-toolchain:local
#   scripts/switch2/build.sh configure   cmake --preset switch2-release
#   scripts/switch2/build.sh build       cmake --build --preset switch2-release
#   scripts/switch2/build.sh audit       audit the NRO, RomFS and linker map
#   scripts/switch2/build.sh package     bundle NRO, SD zip, debug symbols & provenance
#   scripts/switch2/build.sh <command>   run any command in the container
#
# WILIWILI_SWITCH2_IMAGE overrides the image name, WILIWILI_SWITCH2_JOBS the
# number of parallel compile jobs.
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd -P)"
image="${WILIWILI_SWITCH2_IMAGE:-wiliwili-ns2-toolchain:local}"

run() {
    docker run --rm -e WILIWILI_SWITCH2_JOBS="${WILIWILI_SWITCH2_JOBS:-}" \
        -v "${root}:/src" -w /src "${image}" "$@"
}

case "${1:-}" in
    image) docker build -f "${root}/scripts/switch2/Dockerfile" -t "${image}" "${root}" ;;
    configure) run cmake --preset switch2-release ;;
    # cc1plus needs around 1.2 GB for the heavy translation units (the vendored
    # headers plus -g), so a small container gets one job and a large one gets
    # every core. Set WILIWILI_SWITCH2_JOBS to override.
    build)
        run sh -c 'jobs="${WILIWILI_SWITCH2_JOBS:-}"
                   if [ -z "${jobs}" ]; then
                       mem_mb=$(awk "/^MemTotal:/ { print int(\$2 / 1024) }" /proc/meminfo)
                       if [ "${mem_mb}" -lt 4096 ]; then jobs=1; else jobs=$(nproc); fi
                   fi
                   echo "building with -j ${jobs}"
                   cmake --build --preset switch2-release -j "${jobs}"'
        ;;
    audit) run sh /src/scripts/switch2/audit-nro.sh /src/build/switch2-release ;;
    package) sh "${root}/scripts/switch2/package-release.sh" "${root}/build/switch2-release" "${root}/dist" ;;
    "")
        echo "usage: $0 {image|configure|build|audit|package|<command>}" >&2
        exit 1
        ;;
    *)
        run "$@"
        ;;
esac
