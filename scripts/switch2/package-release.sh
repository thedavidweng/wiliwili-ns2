#!/bin/sh
# Package stripped Release NRO, SD card zip, debug symbols tarball,
# BUILDINFO.json, and verified SHA256SUMS for Switch 2 artifacts.
#
#   scripts/switch2/package-release.sh [build-dir] [out-dir] [--audit-passed]
set -eu

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
build_dir="build/switch2-release"
out_dir="dist"
audit_passed=0

pos=0
for arg in "$@"; do
    case "$arg" in
        --audit-passed)
            audit_passed=1
            ;;
        -h|--help)
            echo "usage: $0 [build-dir] [out-dir] [--audit-passed]" >&2
            exit 0
            ;;
        *)
            pos=$((pos + 1))
            case "$pos" in
                1) build_dir="$arg" ;;
                2) out_dir="$arg" ;;
                *)
                    echo "unexpected argument: $arg" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
done

nro="$build_dir/wiliwili-NS2.nro"
elf="$build_dir/wiliwili-NS2.elf"
map="$build_dir/wiliwili.map"

[ -f "$nro" ] || { echo "error: missing NRO artifact: $nro" >&2; exit 1; }
[ -f "$elf" ] || { echo "error: missing ELF debug artifact: $elf" >&2; exit 1; }

command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 1; }

mkdir -p "$out_dir"
rm -f \
    "$out_dir/wiliwili-NS2.nro" \
    "$out_dir/wiliwili-NS2-sd.zip" \
    "$out_dir/wiliwili-NS2-debug-symbols.tar.gz" \
    "$out_dir/BUILDINFO.json" \
    "$out_dir/SHA256SUMS"

cp "$nro" "$out_dir/wiliwili-NS2.nro"

git_commit="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo "unknown")"
git_describe="$(git -C "$repo_root" describe --tags --always --dirty 2>/dev/null || echo "unknown")"

python3 - "$repo_root/UPSTREAM.json" "$build_dir" "$out_dir" "$audit_passed" "$git_commit" "$git_describe" <<'PYEOF'
import datetime
import hashlib
import json
import os
import stat
import sys
import tarfile
import zipfile

upstream_path, build_dir, out_dir, audit_flag, git_commit, git_describe = sys.argv[1:7]
audit_passed = audit_flag == "1"

nro_src = os.path.join(build_dir, "wiliwili-NS2.nro")
elf_src = os.path.join(build_dir, "wiliwili-NS2.elf")
map_src = os.path.join(build_dir, "wiliwili.map")

nro_out = os.path.join(out_dir, "wiliwili-NS2.nro")
sd_zip_out = os.path.join(out_dir, "wiliwili-NS2-sd.zip")
dbg_tar_out = os.path.join(out_dir, "wiliwili-NS2-debug-symbols.tar.gz")
buildinfo_out = os.path.join(out_dir, "BUILDINFO.json")
sha_out = os.path.join(out_dir, "SHA256SUMS")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


# 1. SD archive: switch/wiliwili-ns2/wiliwili-NS2.nro
with open(nro_src, "rb") as f:
    nro_bytes = f.read()

with zipfile.ZipFile(sd_zip_out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
    zi = zipfile.ZipInfo("switch/wiliwili-ns2/wiliwili-NS2.nro", date_time=(2026, 1, 1, 0, 0, 0))
    zi.create_system = 3  # Unix permissions
    zi.external_attr = (stat.S_IFREG | 0o644) << 16
    zf.writestr(zi, nro_bytes)

# 2. Separate debug symbols archive: wiliwili-NS2.elf (+ wiliwili.map when present)
def clean_tarinfo(ti):
    ti.uid = 0
    ti.gid = 0
    ti.uname = ""
    ti.gname = ""
    ti.mtime = 0
    return ti


with tarfile.open(dbg_tar_out, "w:gz", compresslevel=6) as tf:
    tf.add(elf_src, arcname="wiliwili-NS2.elf", filter=clean_tarinfo)
    if os.path.isfile(map_src):
        tf.add(map_src, arcname="wiliwili.map", filter=clean_tarinfo)

# 3. BUILDINFO.json seeded directly from UPSTREAM.json
with open(upstream_path, "r", encoding="utf-8") as f:
    upstream = json.load(f)

sw2 = upstream.get("switch2_toolchain", {})
artifacts = [
    "wiliwili-NS2.nro",
    "wiliwili-NS2-sd.zip",
    "wiliwili-NS2-debug-symbols.tar.gz",
]
files_meta = {}
for name in artifacts:
    path = os.path.join(out_dir, name)
    files_meta[name] = {
        "bytes": os.path.getsize(path),
        "sha256": sha256_file(path),
    }

buildinfo = {
    "project": "wiliwili-plus",
    "artifact": "wiliwili-NS2.nro",
    "audit_passed": audit_passed,
    "git": {
        "commit": git_commit,
        "describe": git_describe,
    },
    "built_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "upstream_pins": {
        "schema": upstream.get("schema"),
        "upstream_commit": upstream.get("upstream", {}).get("commit"),
        "base_image": sw2.get("base_image"),
        "compiler": sw2.get("compiler"),
        "ounce_packages": sw2.get("ounce_packages", {}),
        "portlibs": sw2.get("portlibs", {}),
    },
    "files": files_meta,
}

with open(buildinfo_out, "w", encoding="utf-8") as f:
    json.dump(buildinfo, f, indent=2, sort_keys=True)
    f.write("\n")

# 4. SHA256SUMS across all release deliverables and self-verify on disk
sum_targets = sorted(artifacts + ["BUILDINFO.json"])
lines = []
for name in sum_targets:
    digest = sha256_file(os.path.join(out_dir, name))
    lines.append(f"{digest}  {name}\n")

with open(sha_out, "w", encoding="utf-8") as f:
    f.writelines(lines)

# Verify SHA256SUMS round-trip
with open(sha_out, "r", encoding="utf-8") as f:
    for raw in f:
        digest, name = raw.rstrip("\n").split("  ", 1)
        actual = sha256_file(os.path.join(out_dir, name))
        if actual != digest:
            raise SystemExit(f"SHA256 mismatch for {name}: {actual} != {digest}")

print(f"Packaged Switch 2 release artifacts in {out_dir} ({len(sum_targets)} files verified)")
PYEOF
