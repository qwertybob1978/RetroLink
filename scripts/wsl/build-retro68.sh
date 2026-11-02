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

# Keep the C++ compiler frontend for Retro68's CMake checks, but skip target
# libstdc++, which fails on missing math declarations in the current Newlib.
if ! grep -Eq -- '--disable-libstdcxx([[:space:]]|$)' \
    "${source_dir}/build-toolchain.bash"; then
    if grep -Fq -- '--enable-languages=c,c++' "${source_dir}/build-toolchain.bash"; then
        sed -i 's/--enable-languages=c,c++/--enable-languages=c,c++ --disable-libstdcxx/g' \
            "${source_dir}/build-toolchain.bash"
    elif grep -Fq -- '--enable-languages=c' "${source_dir}/build-toolchain.bash"; then
        sed -i 's/--enable-languages=c/--enable-languages=c,c++ --disable-libstdcxx/g' \
            "${source_dir}/build-toolchain.bash"
    else
        echo "Could not configure Retro68 without target libstdc++." >&2
        exit 1
    fi
fi

cmake_file="${source_dir}/CMakeLists.txt"
if ! grep -Fq 'RETRO68_TOOLCHAIN_ONLY' "${cmake_file}"; then
    if ! grep -Fq 'add_subdirectory(Console)' "${cmake_file}" \
        || ! grep -Fq 'add_subdirectory(AutomatedTests)' "${cmake_file}"; then
        echo "Could not limit the Retro68 build to its required runtime." >&2
        exit 1
    fi
    sed -i '/add_subdirectory(Console)/i if(NOT "$ENV{RETRO68_TOOLCHAIN_ONLY}" STREQUAL "1")' \
        "${cmake_file}"
    sed -i '/add_subdirectory(AutomatedTests)/a endif()' "${cmake_file}"
fi

mkdir -p "${build_dir}"
cd "${build_dir}"
export RETRO68_TOOLCHAIN_ONLY=1
bash "${source_dir}/build-toolchain.bash" --no-carbon --no-ppc

toolchain_bin="${build_dir}/toolchain/bin"
for tool in m68k-apple-macos-gcc Rez; do
    if [[ ! -x "${toolchain_bin}/${tool}" ]]; then
        echo "Retro68 build completed without the required tool: ${toolchain_bin}/${tool}" >&2
        exit 1
    fi
    printf '%s: %s\n' "${tool}" "${toolchain_bin}/${tool}"
done
