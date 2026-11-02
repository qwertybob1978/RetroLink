#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
toolchain_file="${HOME}/retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"
source_dir="${1:-${repo_root}/macintosh/src}"
build_dir="${2:-${repo_root}/.local-tools/retrolink-build}"
app_name="${3:-DesignerForm}"

if [[ ! "${app_name}" =~ ^[A-Za-z][A-Za-z0-9_]{0,30}$ ]]; then
    echo "Application name must start with a letter and contain only letters, digits, or underscores." >&2
    exit 1
fi

if [[ ! -f "${toolchain_file}" ]]; then
    echo "Retro68 68k toolchain is missing: ${toolchain_file}" >&2
    echo "Run scripts/wsl/build-retro68.sh first." >&2
    exit 1
fi

if [[ ! -f "${source_dir}/CMakeLists.txt" ]]; then
    echo "Application source directory has no CMakeLists.txt: ${source_dir}" >&2
    exit 1
fi

cmake -S "${source_dir}" \
    -B "${build_dir}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}"
cmake --build "${build_dir}" --target "${app_name}_APPL" --parallel

artifact="${build_dir}/${app_name}.dsk"
if [[ ! -f "${artifact}" ]]; then
    echo "Retro68 build completed without the expected artifact: ${artifact}" >&2
    exit 1
fi

sha256sum "${artifact}"
