#!/bin/bash
#
# Fetches the third-party dependencies that vendor/bg3se needs, into
# external/third_party/. Kept separate from external/lua, which is bg3le's own
# vendored Lua 5.4.9 and is tracked in git.
# Pass --sources-only to fetch and patch sources without building host libraries.
# Derived from External/pull-externals.bat in the BG3 Script Extender by
# Norbyte and the bg3se contributors (https://github.com/Norbyte/bg3se),
# MIT + Commons Clause. With thanks to them.
#
# Detours is deliberately absent: bg3le hooks via PLT interposition, vtable
# slot patching and call-site patching. SDL2 and Vulkan come from the Steam
# runtime container at run time. oneTBB is deliberately not used at all --
# vendor/compat implements the two concurrent containers bg3se needs over the
# standard library, because libtbb is not in that container.
set -euo pipefail

SOURCES_ONLY=false
case "${1:-}" in
    --sources-only) SOURCES_ONLY=true ;;
    "") ;;
    *) echo "Usage: $0 [--sources-only]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/external/third_party"
mkdir -p "$EXT"
cd "$EXT"

# libc++'s layout for std::variant, which has to be the game's.
#
# The stable libc++ ABI stores a variant's index as four bytes; the game's
# build stores one, straight after the union, as libstdc++ and MSVC both do.
# That is not a detail: a variant with a small union is a different size, so
# every struct holding one inline is laid out differently from the engine's,
# and bg3le reads those structs by offsetof. libc++ keeps the one-byte form
# behind this macro, which only <variant> reads.
#
# It has to apply to every piece of C++ linked into libbg3le.so, not only
# bg3le's own: protobuf-lite's FailDynamicCast takes a std::variant by value,
# abseil aliases absl::variant to std::variant, and both leave weak template
# instantiations whose mangled names are the same under either layout. So
# they are built with it too, and CMakeLists.txt refuses to configure
# against externals built with anything else. The stamp is how it knows.
LIBCXX_ABI_FLAGS="-D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION"
ABI_STAMP="$EXT/.libcxx-abi-flags"
if [ "$SOURCES_ONLY" = false ] && [ "$(cat "$ABI_STAMP" 2>/dev/null || true)" != "$LIBCXX_ABI_FLAGS" ]; then
    # Built with some other layout, or before this was recorded at all: the
    # prebuilt archives are the thing that disagrees, so they go.
    if [ -d "$EXT/abseil/build" ] || [ -d "$EXT/protobuf/build" ]; then
        echo "== libc++ ABI flags changed; rebuilding abseil and protobuf =="
    fi
    rm -rf "$EXT/abseil/build" "$EXT/abseil/install" "$EXT/protobuf/build"
fi

clone() {  # clone <dir> <url> [branch]
    if [ -d "$1" ]; then echo "  $1: present"; return; fi
    if [ -n "${3:-}" ]; then git clone --depth 1 --branch "$3" "$2" "$1"
    else git clone --depth 1 "$2" "$1"; fi
}

echo "== Noesis (the Windows SDK ships portable C++ headers) =="
if [ -d Noesis ]; then
    echo "  Noesis: present"
else
    curl -fL http://bg3se-updates.norbyte.dev/Stuff/NoesisGUI-NativeSDK-win-3.1.7-Indie.zip \
         -o noesis.zip
    mkdir -p Noesis
    # GNU tar cannot read zip archives; on Windows tar is bsdtar, which can.
    bsdtar -xf noesis.zip -C Noesis
    rm noesis.zip
fi

echo "== header-only / portable sources =="
clone glm       https://github.com/g-truc/glm 1.0.3
clone imgui     https://github.com/Norbyte/imgui
clone lua       https://github.com/Norbyte/lua-dos
clone rapidjson https://github.com/tencent/rapidjson
clone tinycrypt https://github.com/intel/tinycrypt
clone Vulkan    https://github.com/KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.357
clone optick    https://github.com/Norbyte/optick

clone abseil https://github.com/abseil/abseil-cpp 20260817.0
if [ ! -d protobuf ]; then
    git clone --depth 1 --branch v36.1 --recurse-submodules --shallow-submodules \
        https://github.com/protocolbuffers/protobuf protobuf
fi

if [ "$SOURCES_ONLY" = false ]; then
    echo "== abseil (built from source against libc++) =="
    # protobuf needs abseil, and 45 of the symbols it calls take or return
    # std::string. The distribution abseil is libstdc++, so it has to be rebuilt
    # too or the mismatch just moves one layer down. The version has to match
    # whatever protobuf expects.
    ABSL_PREFIX="$EXT/abseil/install"
    if [ ! -f "$ABSL_PREFIX/lib/libabsl_strings.a" ]; then
        cmake -S abseil -B abseil/build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_FLAGS="-stdlib=libc++ $LIBCXX_ABI_FLAGS" \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DCMAKE_CXX_STANDARD=17 \
            -DABSL_PROPAGATE_CXX_STD=ON \
            -DABSL_ENABLE_INSTALL=ON \
            -DBUILD_TESTING=OFF \
            -DCMAKE_INSTALL_PREFIX="$ABSL_PREFIX"
        cmake --build abseil/build
        cmake --install abseil/build
    else
        echo "  abseil: present"
    fi

    echo "== protobuf (built from source against libc++) =="
    # The distribution package is built against libstdc++ and exports
    # std::__cxx11 symbols. bg3se's generated message code is part of the extender
    # core, not just its networking, so it has to link a protobuf whose
    # std::string matches ours -- mixing the two ABIs in one process is not
    # something that fails loudly at runtime. Upstream builds protobuf from source
    # on Windows for the same reason.
    if [ -d protobuf/build/libprotobuf-lite.a ] || [ -f protobuf/build/libprotobuf-lite.a ]; then
        echo "  protobuf: present"
    else
        cmake -S protobuf -B protobuf/build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_FLAGS="-stdlib=libc++ $LIBCXX_ABI_FLAGS" \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DCMAKE_CXX_STANDARD=17 \
            -Dprotobuf_BUILD_TESTS=OFF \
            -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
            -Dprotobuf_BUILD_SHARED_LIBS=OFF \
            -Dprotobuf_ABSL_PROVIDER=package \
            -DCMAKE_PREFIX_PATH="$ABSL_PREFIX"
        cmake --build protobuf/build --target libprotobuf-lite
    fi

    # Only once both have been built with the flags above.
    echo "$LIBCXX_ABI_FLAGS" > "$ABI_STAMP"
fi

echo "== patch Noesis for clang =="
# NsCore/TypePropertyImpl.h marks void Get(const void*, void*) const as
# override, but TypeProperty declares GetCopy and no such overload, so it never
# overrode anything. clang rejects it; dropping the keyword changes no
# dispatch. Noesis is third-party and not vendored, so this runs at fetch time.
NS="Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include/NsCore/TypePropertyImpl.h"
if grep -q "void Get(const void\* ptr, void\* dest) const override;" "$NS"; then
    sed -i "s|void Get(const void\* ptr, void\* dest) const override;|void Get(const void* ptr, void* dest) const;|" "$NS"
    echo "  patched TypePropertyImpl.h"
else
    echo "  TypePropertyImpl.h: already patched"
fi

echo "done"
