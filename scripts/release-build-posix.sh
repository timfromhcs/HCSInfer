#!/usr/bin/env bash
# Builds one POSIX release binary (Linux x86_64 / aarch64, macOS arm64), verifies it and stages it for the release
# pipeline. Used by .github/workflows/release.yml; runs the same way on a developer machine.
#
#   scripts/release-build-posix.sh --kind cpu|vulkan --out DIR [--info-dir DIR] [--jobs N] [--run-tests]
#                                  [--bootstrap-manylinux] [--glibc-max 2.28] [--verify-only EXISTING_BINARY]
# --verify-only skips configure/build/tests and runs the staging + gate checks on an already built binary.
#
# Output: DIR/HCSInfer (kind cpu) or DIR/HCSInfer-vulkan (kind vulkan); nothing else is written to DIR, because the
# wheels workflow stages the whole directory into the wheel. Evidence (BUILD-INFO-<kind>.txt: compiler, commit,
# NEEDED libraries, glibc floor, sha256) goes to --info-dir.
#
# Linux gate (fails the build, never warns): the binary may only need libc/libm/libpthread/libdl/librt/ld-linux
# (+ libvulkan.so.1 for the Vulkan kind and never for the CPU kind), must carry no RPATH/RUNPATH, and must not
# reference a GLIBC symbol version above --glibc-max (default 2.28 = manylinux_2_28). libstdc++/libgcc are static.
# macOS gate: only /usr/lib and /System dylibs/frameworks.
set -euo pipefail

kind=cpu; out=""; info=""; jobs=""; run_tests=0; bootstrap=0; glibc_max="2.28"; verify_only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --kind) kind="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --info-dir) info="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    --run-tests) run_tests=1; shift ;;
    --bootstrap-manylinux) bootstrap=1; shift ;;
    --glibc-max) glibc_max="$2"; shift 2 ;;
    --verify-only) verify_only="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
case "$kind" in cpu|vulkan) ;; *) echo "--kind must be cpu or vulkan" >&2; exit 2 ;; esac
[ -n "$out" ] || { echo "--out is required" >&2; exit 2; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
os="$(uname -s)"; arch="$(uname -m)"
[ -n "$info" ] || info="$out/../buildinfo"
mkdir -p "$out" "$info"
if [ -z "$jobs" ]; then jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"; fi
infofile="$info/BUILD-INFO-$kind.txt"
: > "$infofile"
note() { echo "$*" | tee -a "$infofile"; }

preset="(verify-only)"
builddir=""
if [ -n "$verify_only" ]; then
  bin="$verify_only"
else
  # ---- bootstrap (manylinux_2_28 container: AlmaLinux 8, glibc 2.28, gcc-toolset on PATH) -------------------------
  if [ "$bootstrap" = 1 ]; then
    # The container runs as the invoking (non-root) user so no root-owned files remain in the workspace.
    export HOME="${HOME:-/tmp/hcs-home}"; [ -w "$HOME" ] || export HOME=/tmp/hcs-home
    mkdir -p "$HOME"
    /opt/python/cp312-cp312/bin/python -m venv /tmp/hcs-venv
    /tmp/hcs-venv/bin/pip install --quiet cmake ninja
    export PATH="/tmp/hcs-venv/bin:$PATH"
    git config --global --add safe.directory '*'
  fi

  if [ "$kind" = vulkan ]; then
    [ "$os" = Linux ] || { echo "the Vulkan kind is Linux only" >&2; exit 2; }
    if ! command -v glslc >/dev/null 2>&1 || ! command -v spirv-val >/dev/null 2>&1 || [ -z "${VULKAN_SDK:-}" ]; then
      # Pinned LunarG SDK (build time only; end users need just the system libvulkan.so.1). The sha256 is the publisher's
      # value for this exact version; the tarball ships its own glslc, which must be able to run on this (old-glibc) image.
      sdk_ver="${VULKAN_SDK_VERSION:-1.4.357.0}"
      case "$arch" in
        x86_64) sdk_tar="vulkansdk-linux-x86_64-$sdk_ver.tar.xz"; sdk_sha="0f09bf6a0625e346bf004be70b92907e934a4c76606b323441b2baf3a5a0e66d" ;;
        *) echo "no pinned Vulkan SDK tarball for $arch" >&2; exit 3 ;;
      esac
      [ "$sdk_ver" = "1.4.357.0" ] || { echo "VULKAN_SDK_VERSION $sdk_ver has no pinned sha256 in this script" >&2; exit 3; }
      sdk_dir="${HCS_VULKAN_SDK_DIR:-$root/build/vulkan-sdk}"
      mkdir -p "$sdk_dir"
      curl -fsSL -o "$sdk_dir/$sdk_tar" "https://sdk.lunarg.com/sdk/download/$sdk_ver/linux/$sdk_tar"
      echo "$sdk_sha  $sdk_dir/$sdk_tar" | sha256sum -c -
      tar -C "$sdk_dir" -xf "$sdk_dir/$sdk_tar"
      setup="$(find "$sdk_dir" -maxdepth 3 -name setup-env.sh | head -1)"
      [ -n "$setup" ] || { echo "setup-env.sh not found in the Vulkan SDK tarball" >&2; exit 3; }
      # shellcheck disable=SC1090
      . "$setup"
    fi
    glslc --version | head -2 | tee -a "$infofile" || { echo "glslc does not run on this system (glibc too old for the SDK build?)" >&2; exit 3; }
    spirv-val --version | head -1 | tee -a "$infofile"
  fi

  # ---- configure + build ------------------------------------------------------------------------------------------
  extra=(-DCMAKE_SKIP_BUILD_RPATH=ON)
  if [ "$os" = Darwin ]; then
    preset="macos-arm64-release"; [ "$arch" = arm64 ] || { echo "only macOS arm64 is built" >&2; exit 2; }
    [ "$kind" = cpu ] || exit 2
    builddir="build/$preset"
    cmake --preset "$preset" "${extra[@]}"
  elif [ "$arch" = x86_64 ]; then
    if [ "$kind" = vulkan ]; then preset="linux-release-vulkan"; else preset="linux-gcc-release"; fi
    builddir="build/$preset"
    cmake --preset "$preset" "${extra[@]}"
  else
    # aarch64 Linux: no preset yet (the x86 presets force AVX options). Same portable policy as posix-cpu-base.
    [ "$kind" = cpu ] || { echo "no Vulkan build is defined for $arch" >&2; exit 2; }
    preset="linux-aarch64-cpu (command line)"; builddir="build/linux-aarch64-cpu"
    cmake -S . -B "$builddir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHCS_BUILD_TESTS=ON \
      -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
      "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" "${extra[@]}"
  fi
  cmake --build "$builddir" -j "$jobs"

  bin="$builddir/bin/HCSInfer"
  [ -x "$bin" ] || { echo "build produced no $bin" >&2; exit 1; }

  # ---- tests (portable set; model tests report Skipped = NOT RUN when no model is present) ---------------------------
  if [ "$run_tests" = 1 ]; then
    ctest --test-dir "$builddir" -L '^(unit|contract|parser|kernel-cpu)$' --output-on-failure --no-tests=error -j "$jobs" \
      | tee "$info/ctest-$kind.txt"
  fi
fi

# ---- stage + verify ---------------------------------------------------------------------------------------------
name=HCSInfer; [ "$kind" = vulkan ] && name=HCSInfer-vulkan
cp "$bin" "$out/$name"
if [ "$os" = Linux ]; then strip --strip-unneeded "$out/$name"; else strip -x "$out/$name" || true; fi
chmod 0755 "$out/$name"
f="$out/$name"

note "preset      : $preset"
note "kind        : $kind"
note "host        : $os $arch"
note "compiler    : $(${CXX:-c++} --version | head -1)"
note "commit      : $(git rev-parse HEAD 2>/dev/null || echo unknown)"
note "size_bytes  : $(wc -c < "$f" | tr -d ' ')"
note "sha256      : $( (sha256sum "$f" 2>/dev/null || shasum -a 256 "$f") | cut -d' ' -f1)"

fail=0
if [ "$os" = Linux ]; then
  needed="$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort)"
  note "NEEDED      : $(echo "$needed" | tr '\n' ' ')"
  allowed='^(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|ld-linux-.*\.so\.[0-9]+|ld-linux\.so\.[0-9]+|libvulkan\.so\.1)$'
  while IFS= read -r lib; do
    [ -n "$lib" ] || continue
    echo "$lib" | grep -Eq "$allowed" || { echo "::error::unexpected shared library dependency: $lib"; fail=1; }
  done <<< "$needed"
  if [ "$kind" = vulkan ]; then
    echo "$needed" | grep -qx 'libvulkan.so.1' || { echo "::error::Vulkan binary does not link libvulkan.so.1"; fail=1; }
  else
    ! echo "$needed" | grep -qi vulkan || { echo "::error::CPU binary must not depend on libvulkan"; fail=1; }
  fi
  if readelf -d "$f" | grep -Eq '\((RPATH|RUNPATH)\)'; then echo "::error::binary carries an RPATH/RUNPATH"; fail=1; fi
  maxg="$(objdump -T "$f" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -V | uniq | tail -1 || true)"
  maxx="$(objdump -T "$f" | grep -o 'GLIBCXX_[0-9][0-9.]*' | sed 's/GLIBCXX_//' | sort -V | uniq | tail -1 || true)"
  note "max GLIBC   : ${maxg:-none} (limit $glibc_max)"
  note "max GLIBCXX : ${maxx:-none} (libstdc++ is linked statically; any dynamic GLIBCXX need is a failure)"
  if [ -n "$maxg" ] && [ "$(printf '%s\n%s\n' "$maxg" "$glibc_max" | sort -V | tail -1)" != "$glibc_max" ]; then
    echo "::error::$name needs GLIBC_$maxg, above the $glibc_max floor"; fail=1
  fi
  [ -z "$maxx" ] || { echo "::error::$name imports GLIBCXX_$maxx (libstdc++ must be static)"; fail=1; }
else
  otool -L "$f" | tee -a "$infofile"
  bad="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/Library/)' || true)"
  [ -z "$bad" ] || { echo "::error::non-system dylib dependency: $bad"; fail=1; }
  # The wheel tag claims macosx_11_0: the binary must not require a newer OS than that.
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b && /minos/{print $2; exit}')"
  note "minos       : ${minos:-unknown} (limit ${macos_min:-11.0})"
  if [ -z "$minos" ] || [ "$(printf '%s\n%s\n' "$minos" "${macos_min:-11.0}" | sort -V | tail -1)" != "${macos_min:-11.0}" ]; then
    echo "::error::minimum macOS version $minos is above ${macos_min:-11.0} (set MACOSX_DEPLOYMENT_TARGET)"; fail=1
  fi
fi
[ "$fail" = 0 ] || exit 1

# run it. The Vulkan kind needs libvulkan.so.1 at load time; where it is absent that is reported as NOT RUN, never PASS.
if out="$("$f" --version 2>&1)"; then
  echo "$out" | tee -a "$infofile"
  "$f" doctor --json > "$info/doctor-$kind.json"
  note "doctor --json: ok ($(wc -c < "$info/doctor-$kind.json" | tr -d ' ') bytes)"
elif [ "$kind" = vulkan ] && echo "$out" | grep -q 'libvulkan'; then
  note "run check   : NOT RUN (libvulkan.so.1 cannot be loaded in this build environment)"
else
  echo "$out"; echo "::error::$name --version failed"; exit 1
fi
echo "staged $f"
