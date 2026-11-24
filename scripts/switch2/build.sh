#!/bin/sh
# Build the Switch 2 toolchain image and the NS2 NRO inside it.
#
#   scripts/switch2/build.sh image       build wiliwili-ns2-toolchain:local
#   scripts/switch2/build.sh configure   cmake --preset switch2-release
#   scripts/switch2/build.sh build       cmake --build --preset switch2-release
#   scripts/switch2/build.sh <command>   run any command in the container
#
# WILIWILI_SWITCH2_IMAGE overrides the image name.
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd -P)"
image="${WILIWILI_SWITCH2_IMAGE:-wiliwili-ns2-toolchain:local}"

run() {
    docker run --rm -v "${root}:/src" -w /src "${image}" "$@"
}

case "${1:-}" in
    image) docker build -f "${root}/scripts/switch2/Dockerfile" -t "${image}" "${root}" ;;
    configure) run cmake --preset switch2-release ;;
    build) run cmake --build --preset switch2-release ;;
    "")
        echo "usage: $0 {image|configure|build|<command>}" >&2
        exit 1
        ;;
    *)
        run "$@"
        ;;
esac
