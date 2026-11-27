#!/bin/sh
# Deterministic host test for scripts/switch2/package-release.sh (#20).
set -eu

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd -P)}"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

mkdir -p "$tmp_dir/build"
python3 - "$tmp_dir/build" <<'PYEOF'
import os
import sys

build_dir = sys.argv[1]
with open(os.path.join(build_dir, "wiliwili-NS2.nro"), "wb") as f:
    f.write(b"FAKE-NRO-PAYLOAD-FOR-PACKAGE-TEST")
with open(os.path.join(build_dir, "wiliwili-NS2.elf"), "wb") as f:
    f.write(b"\x7fELF-FAKE-DEBUG-SYMBOLS")
with open(os.path.join(build_dir, "wiliwili.map"), "w", encoding="utf-8") as f:
    f.write("Archive member included\n")
PYEOF

sh "$repo_root/scripts/switch2/package-release.sh" "$tmp_dir/build" "$tmp_dir/dist" --audit-passed

python3 - "$tmp_dir/build" "$tmp_dir/dist" <<'PYEOF'
import hashlib
import json
import os
import sys
import tarfile
import zipfile

build_dir, dist_dir = sys.argv[1:3]

with open(os.path.join(build_dir, "wiliwili-NS2.nro"), "rb") as f:
    expected_nro = f.read()

# 1. Verify SD zip contents & exact byte match
sd_zip = os.path.join(dist_dir, "wiliwili-NS2-sd.zip")
with zipfile.ZipFile(sd_zip, "r") as zf:
    names = zf.namelist()
    assert names == ["switch/wiliwili-ns2/wiliwili-NS2.nro"], f"unexpected zip members: {names}"
    assert zf.read(names[0]) == expected_nro, "zip NRO payload mismatch"

# 2. Verify debug symbols tarball contains only clean relative members
dbg_tar = os.path.join(dist_dir, "wiliwili-NS2-debug-symbols.tar.gz")
with tarfile.open(dbg_tar, "r:gz") as tf:
    members = [m.name for m in tf.getmembers()]
    assert members == ["wiliwili-NS2.elf", "wiliwili.map"], f"unexpected tar members: {members}"
    for m in members:
        assert not m.startswith("/") and "/Users/" not in m, f"unsafe path in tar: {m}"

# 3. Verify BUILDINFO.json metadata & hashes
with open(os.path.join(dist_dir, "BUILDINFO.json"), "r", encoding="utf-8") as f:
    info = json.load(f)

assert info["project"] == "wiliwili-plus"
assert info["artifact"] == "wiliwili-NS2.nro"
assert info["audit_passed"] is True
assert info["upstream_pins"]["schema"] == 1
assert info["upstream_pins"]["ounce_packages"]["libounce"]

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

for fname in ["wiliwili-NS2.nro", "wiliwili-NS2-sd.zip", "wiliwili-NS2-debug-symbols.tar.gz"]:
    path = os.path.join(dist_dir, fname)
    assert info["files"][fname]["bytes"] == os.path.getsize(path)
    assert info["files"][fname]["sha256"] == sha256_file(path)

# 4. Verify SHA256SUMS covers all 4 deliverables accurately
with open(os.path.join(dist_dir, "SHA256SUMS"), "r", encoding="utf-8") as f:
    seen = {}
    for line in f:
        digest, fname = line.rstrip("\n").split("  ", 1)
        seen[fname] = digest

expected_files = {
    "BUILDINFO.json",
    "wiliwili-NS2-debug-symbols.tar.gz",
    "wiliwili-NS2-sd.zip",
    "wiliwili-NS2.nro",
}
assert set(seen.keys()) == expected_files, f"unexpected SHA256SUMS keys: {seen.keys()}"
for fname, digest in seen.items():
    assert digest == sha256_file(os.path.join(dist_dir, fname)), f"hash mismatch for {fname}"

print("switch2_release_package passed")
PYEOF
