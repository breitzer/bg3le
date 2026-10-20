#!/bin/bash
#
# The portable build: libbg3le.so against the Steam Runtime 3 ("sniper") SDK, for release.
#
# build/ is built against the host's glibc, and asks for whatever symbol versions that has (acosf@GLIBC_2.43 on a
# rolling distribution), so it won't load on an older one. The game runs inside the sniper container, which
# guarantees glibc 2.31; building against the SDK's glibc makes the library load anywhere the game does. glibc
# can't be linked statically into a library loaded into the game (a process has one libc), so this is the fix.
#
# The host's clang builds everything with the SDK as its sysroot (the SDK's own compilers are too old for C++23):
#   1. the SDK image, unpacked into build-sniper/sysroot
#   2. libc++ and libc++abi, from the LLVM release matching the host clang, static, against that sysroot
#   3. abseil and protobuf, from external/third_party's sources, against both
#   4. libbg3le.so, then a check that it asks for nothing newer than glibc 2.31
#
# Each step is skipped when its output is already there; delete build-sniper/ (or one directory in it) to redo it.
# Needs docker, clang, cmake, ninja and git. Install the result with:
#   ./install.py --lib build-sniper/bg3le/libbg3le.so
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BG3LE_SNIPER_BUILD:-$ROOT/build-sniper}"
IMAGE="${BG3LE_SNIPER_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}"
EXT="$ROOT/external/third_party"
TOOLCHAIN="$ROOT/cmake/sniper-toolchain.cmake"
GLIBC_MAX="2.31"
# The same std::variant layout as every other piece of C++ in the library; see tools/fetch-externals.sh.
LIBCXX_ABI_FLAGS="-D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION"

export BG3LE_SNIPER_SYSROOT="$OUT/sysroot"
LIBCXX="$OUT/libcxx"
mkdir -p "$OUT"

# Fetch and patch sources without requiring a host libc++ installation.
"$ROOT/tools/fetch-externals.sh" --sources-only

echo "== sysroot: $IMAGE =="
if [ -f "$BG3LE_SNIPER_SYSROOT/.image" ]; then
    echo "  present ($(cat "$BG3LE_SNIPER_SYSROOT/.image"))"
else
    docker pull "$IMAGE"
    rm -rf "$BG3LE_SNIPER_SYSROOT"
    mkdir -p "$BG3LE_SNIPER_SYSROOT"
    cid="$(docker create "$IMAGE")"
    trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
    # Device nodes can't be created without root, and nothing links against them. Anchored: unanchored, --exclude=sys
    # also drops usr/include/x86_64-linux-gnu/sys/, glibc's own headers.
    docker export "$cid" | tar -x -C "$BG3LE_SNIPER_SYSROOT" --anchored --exclude=dev --exclude=proc --exclude=sys
    docker rm -f "$cid" >/dev/null
    trap - EXIT
    # Absolute symlinks would lead the linker back into the host's /usr/lib; make them point inside the sysroot.
    find "$BG3LE_SNIPER_SYSROOT" -type l -lname '/*' -print0 | while IFS= read -r -d '' link; do
        target="$(readlink "$link")"
        ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$BG3LE_SNIPER_SYSROOT$target")" "$link"
    done
    docker image inspect --format '{{.Id}}' "$IMAGE" > "$BG3LE_SNIPER_SYSROOT/.image"
    echo "  unpacked"
fi

CLANG_VERSION="$(clang --version | sed -n 's/.*clang version \([0-9][0-9.]*\).*/\1/p' | head -1)"
echo "== libc++ $CLANG_VERSION =="
if [ "$(cat "$LIBCXX/.version" 2>/dev/null || true)" = "$CLANG_VERSION $LIBCXX_ABI_FLAGS" ]; then
    echo "  present"
else
    LLVM="$OUT/llvm-project"
    if [ "$(git -C "$LLVM" describe --tags 2>/dev/null || true)" != "llvmorg-$CLANG_VERSION" ]; then
        rm -rf "$LLVM"
        # Only what the runtimes build reads, not the 2 GB of the rest.
        git clone --depth 1 --branch "llvmorg-$CLANG_VERSION" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$LLVM"
    fi
    # libc: libc++'s from_chars shares llvm-libc's float parsing (libc/shared).
    git -C "$LLVM" sparse-checkout set libcxx libcxxabi libunwind libc runtimes cmake \
        llvm/cmake llvm/utils/llvm-lit llvm/utils/lit third-party
    rm -rf "$OUT/libcxx-build" "$LIBCXX"
    # Static, with libc++abi merged into libc++.a as well, so a plain -stdlib=libc++ link (configure checks) works.
    # libgcc's unwinder, as the host build uses (-static-libgcc).
    env -u BG3LE_SNIPER_LIBCXX cmake -S "$LLVM/runtimes" -B "$OUT/libcxx-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$LIBCXX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
        -DLIBCXX_ENABLE_SHARED=OFF \
        -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
        -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF
    cmake --build "$OUT/libcxx-build"
    cmake --install "$OUT/libcxx-build"
    echo "$CLANG_VERSION $LIBCXX_ABI_FLAGS" > "$LIBCXX/.version"
fi
export BG3LE_SNIPER_LIBCXX="$LIBCXX"

# Passing CMAKE_CXX_FLAGS replaces the toolchain's initial flags, so the libc++ headers are named here again.
EXT_CXX_FLAGS="-nostdinc++ -isystem $LIBCXX/include/c++/v1 -stdlib=libc++ $LIBCXX_ABI_FLAGS"

echo "== abseil =="
ABSL="$OUT/abseil"
if [ -f "$ABSL/lib/libabsl_strings.a" ]; then
    echo "  present"
else
    rm -rf "$OUT/abseil-build"
    cmake -S "$EXT/abseil" -B "$OUT/abseil-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$EXT_CXX_FLAGS" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -DABSL_PROPAGATE_CXX_STD=ON \
        -DABSL_ENABLE_INSTALL=ON \
        -DBUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX="$ABSL"
    cmake --build "$OUT/abseil-build"
    cmake --install "$OUT/abseil-build"
fi

echo "== protobuf =="
PROTOBUF="$OUT/protobuf-build"
if [ -f "$PROTOBUF/libprotobuf-lite.a" ]; then
    echo "  present"
else
    rm -rf "$PROTOBUF"
    # absl_DIR directly: the toolchain confines package searches to the sysroot.
    cmake -S "$EXT/protobuf" -B "$PROTOBUF" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$EXT_CXX_FLAGS" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_ABSL_PROVIDER=package \
        -Dabsl_DIR="$ABSL/lib/cmake/absl"
    cmake --build "$PROTOBUF" --target libprotobuf-lite
fi

# Record the ABI of these archives independently of the host builds.
echo "$LIBCXX_ABI_FLAGS" > "$OUT/.libcxx-abi-flags"

echo "== bg3le =="
# The archives by path: the sysroot might carry a libc++ of its own for find_library to prefer.
cmake -S "$ROOT" -B "$OUT/bg3le" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBCXX_STATIC="$LIBCXX/lib/libc++.a" \
    -DLIBCXXABI_STATIC="$LIBCXX/lib/libc++abi.a" \
    -DBG3LE_PROTOBUF_BUILD="$PROTOBUF" \
    -DBG3LE_ABI_STAMP="$OUT/.libcxx-abi-flags" \
    -DABSL_PREFIX="$ABSL"
cmake --build "$OUT/bg3le" --target bg3le

LIB="$OUT/bg3le/libbg3le.so"
echo "== $LIB =="
readelf -d "$LIB" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/  needs \1/p'
newest="$(objdump -T "$LIB" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -V | tail -1)"
echo "  newest glibc symbol version: $newest (limit $GLIBC_MAX)"
if [ "$(printf '%s\n%s\n' "$newest" "$GLIBC_MAX" | sort -V | tail -1)" != "$GLIBC_MAX" ]; then
    echo "ERROR: asks for glibc $newest, newer than the sniper runtime's $GLIBC_MAX:" >&2
    objdump -T "$LIB" | grep -E "GLIBC_$newest" >&2
    exit 1
fi
echo "portable: loads with glibc $GLIBC_MAX and newer"
