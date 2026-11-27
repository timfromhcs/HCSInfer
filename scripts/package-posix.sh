#!/usr/bin/env bash
# Packs the POSIX release binaries into HCSInfer-<version>-<os>-<arch>.tar.gz (Linux / macOS; the Windows ZIP is built
# by scripts/package.ps1).
#
#   scripts/package-posix.sh --bin-dir DIR --version 1.1.0 --os linux|macos --arch x86_64|aarch64|arm64 --out DIR
#
# DIR must hold HCSInfer (required) and optionally HCSInfer-vulkan. The archive has one top-level folder with the
# binaries, LICENSE, THIRD_PARTY_NOTICES.md, the licence texts the dependency lock points at (when the submodules are
# checked out), a MANIFEST.json (sha256 of every file, version, commit) and a short README-install.txt. Models are never
# included. The tar is deterministic (sorted names, fixed mtime from SOURCE_DATE_EPOCH or the commit time, uid/gid 0,
# gzip -n) so the same inputs give the same archive.
set -euo pipefail

bindir=""; version=""; os=""; arch=""; out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin-dir) bindir="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --os) os="$2"; shift 2 ;;
    --arch) arch="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$bindir" ] && [ -n "$version" ] && [ -n "$os" ] && [ -n "$arch" ] && [ -n "$out" ] || { echo "missing argument (see header)" >&2; exit 2; }
case "$os" in linux|macos) ;; *) echo "--os must be linux or macos" >&2; exit 2 ;; esac
case "$arch" in x86_64|aarch64|arm64) ;; *) echo "--arch must be x86_64, aarch64 or arm64" >&2; exit 2 ;; esac
# version is a file-name component: refuse anything that is not a plain semver-ish token
printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$' || { echo "bad version: $version" >&2; exit 2; }
[ -f "$bindir/HCSInfer" ] || { echo "missing $bindir/HCSInfer" >&2; exit 1; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
py="$(command -v python3 || command -v python)" || { echo "python3 is required" >&2; exit 1; }
name="HCSInfer-$version-$os-$arch"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
pkg="$stage/$name"
mkdir -p "$pkg/licenses" "$out"

cp "$bindir/HCSInfer" "$pkg/HCSInfer"
[ -f "$bindir/HCSInfer-vulkan" ] && cp "$bindir/HCSInfer-vulkan" "$pkg/HCSInfer-vulkan"
chmod 0755 "$pkg/HCSInfer"; [ ! -f "$pkg/HCSInfer-vulkan" ] || chmod 0755 "$pkg/HCSInfer-vulkan"
[ -f "$root/LICENSE" ] && cp "$root/LICENSE" "$pkg/LICENSE"
[ -f "$root/THIRD_PARTY_NOTICES.md" ] && cp "$root/THIRD_PARTY_NOTICES.md" "$pkg/THIRD_PARTY_NOTICES.md"

# Licence texts named by the dependency lock (submodule / vendored files); a missing one is reported, not invented.
"$py" - "$root" "$pkg/licenses" <<'PYEOF'
import json, os, re, shutil, sys
root, dst = sys.argv[1], sys.argv[2]
lock = json.load(open(os.path.join(root, "external", "dependency-lock.json"), encoding="utf-8"))
for d in lock["dependencies"]:
    rel = d.get("licence_file")
    if not rel:
        continue
    src = os.path.join(root, *rel.split("/"))
    if os.path.isfile(src):
        safe = re.sub(r"[^A-Za-z0-9._-]", "_", d["name"])
        shutil.copyfile(src, os.path.join(dst, safe + "-" + os.path.basename(src)))
    else:
        print("WARNING: licence file for %s not found: %s (submodules not checked out?)" % (d["name"], rel), file=sys.stderr)
PYEOF

commit="$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
cat > "$pkg/README-install.txt" <<EOF
HCSInfer $version for $os-$arch (EXPERIMENTAL on this platform unless the release notes say otherwise).

  ./HCSInfer --help
  ./HCSInfer doctor --json
  ./HCSInfer models scan <folder with .gguf files>
  ./HCSInfer serve --model-root <folder>        # API + web UI on 127.0.0.1:8765, token on stderr

HCSInfer-vulkan (when present) additionally needs the system Vulkan loader (libvulkan.so.1) and a driver (Mesa RADV or a
vendor ICD); real-GPU runs on this platform are NOT RUN by the project. There is no Metal backend on macOS.
The binaries are unsigned. Verify the archive against SHA256SUMS from the release page. Models are not included.
Licences: LICENSE, THIRD_PARTY_NOTICES.md, licenses/.
EOF

# MANIFEST.json (written by python so the JSON is always valid)
"$py" - "$pkg" "$version" "$os" "$arch" "$commit" <<'PYEOF'
import hashlib, json, os, sys
pkg, version, osn, arch, commit = sys.argv[1:6]
files = {}
for base, _, names in os.walk(pkg):
    for n in sorted(names):
        p = os.path.join(base, n)
        rel = os.path.relpath(p, pkg).replace(os.sep, "/")
        if rel == "MANIFEST.json":
            continue
        files[rel] = hashlib.sha256(open(p, "rb").read()).hexdigest()
json.dump({"schema_version": 1, "name": "HCSInfer", "version": version, "os": osn, "arch": arch,
           "git_commit": commit, "files_sha256": dict(sorted(files.items()))},
          open(os.path.join(pkg, "MANIFEST.json"), "w", encoding="utf-8"), indent=2)
PYEOF

epoch="${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct 2>/dev/null || echo 0)}"
find "$pkg" -exec touch -d "@$epoch" {} + 2>/dev/null || find "$pkg" -exec touch -t "$(date -u -r "$epoch" +%Y%m%d%H%M.%S)" {} +
archive="$out/$name.tar.gz"
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
  (cd "$stage" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" -cf - "$name") | gzip -n -9 > "$archive"
else
  # bsdtar (macOS): no --sort; feed a sorted file list
  (cd "$stage" && find "$name" | LC_ALL=C sort | tar --uid 0 --gid 0 --uname root --gname root -cf - -T -) | gzip -n -9 > "$archive"
fi
echo "packed $archive"
tar -tzf "$archive" | head -20
