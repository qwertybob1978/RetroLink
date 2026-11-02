#!/usr/bin/env bash
set -euo pipefail

source_dir="${HOME}/retro68-src"
build_dir="${HOME}/retro68-build"

if [[ ! -d "${source_dir}/.git" ]]; then
    git clone --recurse-submodules https://github.com/autc04/Retro68.git "${source_dir}"
else
    git -C "${source_dir}" submodule update --init --recursive
fi

if [[ ! -x "${source_dir}/build-toolchain.bash" ]]; then
    echo "Retro68 build script is missing or not executable: ${source_dir}/build-toolchain.bash" >&2
    exit 1
fi

mkdir -p "${build_dir}"
cd "${build_dir}"
bash "${source_dir}/build-toolchain.bash" --no-carbon

toolchain_bin="${build_dir}/toolchain/bin"
for tool in m68k-apple-macos-gcc powerpc-apple-macos-gcc Rez; do
    if [[ ! -x "${toolchain_bin}/${tool}" ]]; then
        echo "Retro68 build completed without the required tool: ${toolchain_bin}/${tool}" >&2
        exit 1
    fi
    printf '%s: %s\n' "${tool}" "${toolchain_bin}/${tool}"
done
