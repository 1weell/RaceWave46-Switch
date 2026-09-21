#!/usr/bin/env bash
set -euo pipefail

# Independent from the working boot/debug build. Run from devkitPro MSYS2.
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export DEVKITPRO="${DEVKITPRO:-/c/devkitPro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
build="${WR64_PERF_BUILD_DIR:-$root/b-switch-performance}"
sdk="${WR64_SWITCH_VULKAN_SDK:-$root/../mesa-26.2.1-switch-unified-horizon-sdk/opt/devkitpro/portlibs/switch}"
export TMP="$root/tmp" TEMP="$root/tmp" TMPDIR="$root/tmp"
mkdir -p "$TMPDIR" "$build"
test -f "$sdk/lib/libvulkan.a"

cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/switch-devkitA64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWR64_SWITCH=ON -DWR64_ENABLE_RT64=ON -DWR64_ENABLE_FRONTEND=OFF \
    -DWR64_ENABLE_DIAGNOSTICS=OFF \
    -DWR64_ENABLE_IPO="${WR64_PERF_IPO:-OFF}" \
    -DWR64_IPO_RECOMPILED="${WR64_PERF_IPO_RECOMPILED:-ON}" \
    -DWR64_IPO_RUNTIME="${WR64_PERF_IPO_RUNTIME:-OFF}" \
    -DWR64_VULKAN_DXC="$root/tools/dxc-msys.sh" \
    -DWR64_HOST_FILE_TO_C="$root/host-file-to-c/file_to_c.exe" \
    -DWR64_SWITCH_VULKAN_SDK="$sdk" \
    -DWR64_SWITCH_VULKAN_LIBRARY="$sdk/lib/libvulkan.a"
cmake --build "$build" --target WaveRace64RecompiledNro \
    --parallel "${WR64_BUILD_JOBS:-2}"

# Keep a reproducibility record beside this artifact, including dirty state.
{
    printf 'Built UTC: %s\n' "$(date -u +%FT%TZ)"
    printf 'Commit: %s\n' "$(git -C "$root" rev-parse HEAD)"
    printf 'IPO requested: %s\n' "${WR64_PERF_IPO:-OFF}"
    printf 'IPO recompiled: %s\n' "${WR64_PERF_IPO_RECOMPILED:-ON}"
    printf 'IPO runtime: %s\n' "${WR64_PERF_IPO_RUNTIME:-OFF}"
    printf 'SDK: %s\n' "$sdk"
    "$DEVKITA64/bin/aarch64-none-elf-g++" --version
    sha256sum "$build/WaveRace64Recompiled.nro" "$sdk/lib/libvulkan.a"
    git -C "$root" status --short
} > "$build/performance-build-manifest.txt"
printf 'Performance NRO: %s\n' "$build/WaveRace64Recompiled.nro"
