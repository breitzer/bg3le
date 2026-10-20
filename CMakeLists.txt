cmake_minimum_required(VERSION 3.16)

# clang is required, not merely supported: the vendored bg3se sources need
# -fdeclspec, -fms-extensions and -fdelayed-template-parsing, none of which gcc
# has. Set before project() so the toolchain is chosen correctly.
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(bg3le C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "bg3le requires clang; ${CMAKE_CXX_COMPILER_ID} cannot compile the "
        "vendored bg3se sources (-fdeclspec, -fms-extensions, "
        "-fdelayed-template-parsing)")
endif()

# Default to Release. Without NDEBUG the vendored sources compile their debug
# branches, which pull in more Windows API surface and enable upstream asserts.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# libc++ is not a preference. The native game is built against it, so its
# std::string is 24 bytes; libstdc++ would give 32 and silently shift every
# field after a string in a component. Everything in one library has to agree
# on a standard library, so this applies to bg3le's own sources too.
#
# Linked statically, for the same reason Lua is vendored: an LD_PRELOAD shim
# loaded inside the Steam runtime container cannot rely on host libraries.
add_compile_options(-stdlib=libc++)
# Link the static standard library explicitly rather than via
# -static-libstdc++. clang appends its own -lc++ last, which puts it after
# libc++abi and leaves the __cxa_* symbols unresolved; naming both archives
# ourselves keeps them in dependency order (libc++ needs libc++abi).
add_link_options(-stdlib=libc++ -nostdlib++ -static-libgcc)

find_library(LIBCXX_STATIC NAMES libc++.a REQUIRED)
find_library(LIBCXXABI_STATIC NAMES libc++abi.a REQUIRED)
link_libraries(${LIBCXX_STATIC} ${LIBCXXABI_STATIC})

# std::variant laid out as the game lays it out.
#
# The game is built against libc++ ABI version 2 -- every standard-library
# symbol in it is in std::__2 -- and bg3le against the stable ABI 1, because
# that is the libc++.a there is to link. For almost everything the two agree
# on layout. std::variant is where they do not: version 2 stores the index in
# one byte straight after the union, version 1 in four. A variant with a
# small union is then a different size, every struct holding one inline is
# laid out differently from the engine's, and bg3le reads those by offsetof.
#
# libc++ keeps the one-byte form behind this macro, which only <variant>
# reads and which touches nothing compiled into libc++.a. Measured against a
# genuine ABI-2 configuration, it produces the same size for every flat
# variant. It does not change how this libc++ pads a variant nested inside a
# variant -- that differs between libc++ versions rather than ABIs, and
# src/vendor/component_meta.cpp supplies those from measurement.
#
# Everything C++ in the library has to agree, including the prebuilt
# externals: protobuf-lite takes a std::variant by value and abseil aliases
# absl::variant to it. tools/fetch-externals.sh builds them with the same
# flags and records them; configuring against anything else is refused.
set(BG3LE_LIBCXX_ABI_FLAGS -D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION)
add_compile_options(${BG3LE_LIBCXX_ABI_FLAGS})

set(BG3LE_ABI_STAMP "${CMAKE_CURRENT_SOURCE_DIR}/external/third_party/.libcxx-abi-flags"
    CACHE FILEPATH "ABI stamp for the selected abseil and protobuf builds")
if(EXISTS ${BG3LE_ABI_STAMP})
    file(READ ${BG3LE_ABI_STAMP} BG3LE_EXTERNALS_ABI)
    string(STRIP "${BG3LE_EXTERNALS_ABI}" BG3LE_EXTERNALS_ABI)
else()
    set(BG3LE_EXTERNALS_ABI "<not recorded>")
endif()
if(NOT BG3LE_EXTERNALS_ABI STREQUAL "${BG3LE_LIBCXX_ABI_FLAGS}")
    message(FATAL_ERROR
        "abseil and protobuf were built with libc++ ABI flags "
        "'${BG3LE_EXTERNALS_ABI}', not '${BG3LE_LIBCXX_ABI_FLAGS}'. Linking "
        "them would give std::variant two layouts in one library. Run "
        "tools/fetch-externals.sh, which rebuilds them.")
endif()

# ---------------------------------------------------------------------------
# Lua (Norbyte's fork; see external/lua/README.bg3le)
# ---------------------------------------------------------------------------
file(GLOB LUA_SOURCES external/lua/*.c)
# bg3se builds this fork as C++ (LuaLib.vcxproj sets CompileAsCpp), and its
# own headers rely on that: lua.h names CMetatable and TString without a
# struct tag, which is only legal in C++.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
# CMP0119 NEW passes -x c++ for LANGUAGE CXX sources;
# else clang++ warns about a .c input compiled as C++.
if(POLICY CMP0119)
    cmake_policy(SET CMP0119 NEW)
endif()
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC external/lua)
target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
# ltable.c uses uintptr_t, which MSVC's headers supply transitively and
# libc++ does not.
target_compile_options(lua_static PRIVATE -include cstdint)
# Upstream fork's C idioms that clang flags as C++;
# not ours to fix, so silenced for this target only.
target_compile_options(lua_static PRIVATE
    -Wno-string-plus-int -Wno-deprecated-enum-enum-conversion
    -Wno-null-conversion)
set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# ImGui (Norbyte's fork), for the extender's in-game UI. Only the Vulkan
# backend: the native Linux build renders through Vulkan, and the D3D11 one
# needs the DirectX headers.
# ---------------------------------------------------------------------------
set(EXTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/external/third_party)
# The SDL2 headers of whatever is being built against: the host, or the sysroot tools/build-sniper.sh sets.
set(SDL2_INCLUDE ${CMAKE_SYSROOT}/usr/include/SDL2)

add_library(imgui STATIC
    ${EXTERNAL}/imgui/imgui.cpp
    ${EXTERNAL}/imgui/imgui_draw.cpp
    ${EXTERNAL}/imgui/imgui_tables.cpp
    ${EXTERNAL}/imgui/imgui_widgets.cpp
    ${EXTERNAL}/imgui/imgui_demo.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_vulkan.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_sdl2.cpp
)
target_include_directories(imgui PUBLIC ${EXTERNAL}/imgui
    ${EXTERNAL}/Vulkan/include ${SDL2_INCLUDE})
target_compile_options(imgui PRIVATE -w)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Optick. USE_OPTICK is 0, so its macros compile away, but bg3se still refers
# to the Optick types directly in its profiler plumbing.
# ---------------------------------------------------------------------------
add_library(optick STATIC
    ${EXTERNAL}/optick/src/optick_capi.cpp
    ${EXTERNAL}/optick/src/optick_core.cpp
    ${EXTERNAL}/optick/src/optick_message.cpp
    ${EXTERNAL}/optick/src/optick_miniz.cpp
    ${EXTERNAL}/optick/src/optick_serialization.cpp
    ${EXTERNAL}/optick/src/optick_server.cpp
)
target_include_directories(optick PUBLIC ${EXTERNAL}/optick/src)
target_compile_options(optick PRIVATE -w)
set_target_properties(optick PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Vendored bg3se
#
# See vendor/NOTICE.md for attribution and for every change made to build this
# with clang. tools/vendor-exclude.txt records the translation units bg3le
# deliberately does not build, and why.
# ---------------------------------------------------------------------------
set(VENDOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/bg3se)
set(NOESIS ${EXTERNAL}/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include)

if(NOT EXISTS ${NOESIS})
    message(FATAL_ERROR "externals missing; run tools/fetch-externals.sh")
endif()

file(GLOB_RECURSE VENDOR_SOURCES ${VENDOR_ROOT}/*.cpp ${VENDOR_ROOT}/*.cc)

# Linux stand-ins for the components whose upstream implementations are
# Windows-only. These are bg3le's own code, so they are not silenced by the
# -w below; they are listed here because they have to see the vendored headers.
list(APPEND VENDOR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/console_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/platform_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/noesis_rtti_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/entity_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/component_meta.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/fixed_string.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/static_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/mods.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stat_origins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stats_functors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/loca.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/templates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/level.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/functor_exec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/static_data_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/entity_trace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stat_functor_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/type_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/path_override.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/net_ids.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/ui_globals.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/prototypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stat_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stat_create.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stats_sets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/moveable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/cache_template.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/string_keys.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/translated_strings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/statics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/imgui_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/imgui_hdr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/json_binary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/boosts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/savegame_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/global_switches.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/extender_globals.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/imgui_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/file_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/sdl_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/imgui_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/imgui_methods.cpp)

# Kept in step with tools/vendor-exclude.txt.
set(VENDOR_EXCLUDE
    BG3Extender/Extender/Shared/dllmain.cpp        # DLL entry; we use LD_PRELOAD
    BG3Extender/Extender/Shared/CrashReporter.cpp  # Windows crash reporting
    BG3Extender/Extender/Shared/DWriteWrapper.cpp  # DWrite proxy-DLL trick
    BG3Extender/GameHooks/DataLibraries.cpp        # PE scanner; we read ELF
    BG3Extender/GameHooks/DataLibrariesBG3Game.cpp
    BG3Extender/GameHooks/OsirisWrappers.cpp       # Detours; we interpose PLT
    BG3Extender/GameHooks/EngineHooks.cpp
    CoreLib/SymbolMapper.cpp
    BG3Extender/Extender/Client/SDL.cpp
    CoreLib/Console.cpp                            # Windows console internals
    # Byte-identical duplicate of CoreLib/tinyxml2.cpp; building both would
    # define every symbol twice.
    BG3Extender/Extender/Shared/tinyxml2.cpp
)
foreach(rel ${VENDOR_EXCLUDE})
    list(REMOVE_ITEM VENDOR_SOURCES ${VENDOR_ROOT}/${rel})
endforeach()

add_library(bg3se_vendor STATIC ${VENDOR_SOURCES})
set_target_properties(bg3se_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(bg3se_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/compat
    ${VENDOR_ROOT}
    ${VENDOR_ROOT}/BG3Extender
    ${NOESIS}
    ${EXTERNAL}/glm
    ${EXTERNAL}/imgui
    ${EXTERNAL}/rapidjson/include
    ${EXTERNAL}/optick/src
    ${EXTERNAL}/tinycrypt/lib/include
    ${SDL2_INCLUDE}
)

target_compile_definitions(bg3se_vendor PRIVATE
    OSI_EOCAPP
    OSI_EXTENSION_BUILD
    NS_STATIC_LIBRARY
    BG3LE_NOESIS_FORWARD  # Noesis calls go to the game; src/noesis_forward.cpp
    GLM_FORCE_INTRINSICS   # MSVC enables SIMD implicitly; glm needs it set
    GOOGLE_PROTOBUF_NO_RTTI
    _ITERATOR_DEBUG_LEVEL=0
    BG3LE_NO_DX11   # the D3D11 ImGui backend needs the DirectX headers
)

# -fdeclspec/-fms-extensions: __declspec and MSVC struct extensions.
# -fdelayed-template-parsing: MSVC resolves dependent names at instantiation.
# Not -fms-compatibility: it de-keywords char16_t/char32_t and breaks libc++.
# MSVC enables SSE4.2 and POPCNT by default when targeting x64, so upstream
# uses _mm_popcnt_u64 and rapidjson's SSE4.2 scanner without asking for them.
# PRIVATE: these belong to the vendored sources and the Linux stand-ins in
# src/vendor/ that are part of this target. bg3le's own sources should not be
# getting Win32 typedefs or MSVC language extensions force-included.
target_compile_options(bg3se_vendor PRIVATE
    -msse4.2 -mpopcnt
    -fdeclspec -fms-extensions -fdelayed-template-parsing
    -Wno-delayed-template-parsing-in-cxx20
    -include ${CMAKE_CURRENT_SOURCE_DIR}/vendor/compat/msvc_compat.h
)
# Upstream warnings are not ours to fix, and there are some 1800 of them.
target_compile_options(bg3se_vendor PRIVATE -w)

# protobuf and abseil are built from source against libc++ by
# tools/fetch-externals.sh. The distribution packages are libstdc++ and export
# std::__cxx11 symbols; bg3se's generated message code is part of the extender
# core, so mixing the two std::string ABIs in one process is not an option.
# TBB is deliberately absent: it is not in the Steam runtime container, so
# vendor/compat implements the two concurrent containers over the standard
# library instead.
# tools/build-sniper.sh points these at its own builds of the two.
set(BG3LE_PROTOBUF_BUILD ${EXTERNAL}/protobuf/build CACHE PATH "protobuf build directory")
set(ABSL_PREFIX ${EXTERNAL}/abseil/install CACHE PATH "abseil install prefix")
set(PROTOBUF_LITE ${BG3LE_PROTOBUF_BUILD}/libprotobuf-lite.a)
if(NOT EXISTS ${PROTOBUF_LITE})
    message(FATAL_ERROR "protobuf not built; run tools/fetch-externals.sh")
endif()

file(GLOB ABSL_LIBS ${ABSL_PREFIX}/lib/libabsl_*.a)
list(APPEND ABSL_LIBS ${BG3LE_PROTOBUF_BUILD}/third_party/utf8_range/libutf8_validity.a)

# bg3le_noesis_*.inl, included from upstream ClientUI sources.
target_include_directories(bg3se_vendor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)

target_include_directories(bg3se_vendor PRIVATE
    ${EXTERNAL}/protobuf/src
    # utf8_validity.h, which protobuf's headers include; without this it came from the host's protobuf package.
    ${EXTERNAL}/protobuf/third_party/utf8_range
    ${ABSL_PREFIX}/include)

# The abseil archives are mutually dependent, so a single left-to-right pass
# cannot resolve them; --start-group makes the linker iterate.
target_link_libraries(bg3se_vendor PUBLIC
    lua_static imgui ${PROTOBUF_LITE}
    -Wl,--start-group ${ABSL_LIBS} -Wl,--end-group
    optick)

# ---------------------------------------------------------------------------
# bg3le
# ---------------------------------------------------------------------------
add_library(bg3le SHARED
    src/preload.cpp
    src/elf_symbols.cpp
    src/ecs_types.cpp
    src/ecs_world.cpp
    src/log.cpp
    src/mem.cpp
    src/lua_host.cpp
    src/savegame.cpp
    src/game_state.cpp
    src/noesis_ui.cpp
    src/noesis_forward.cpp
    src/osi.cpp
    src/resources.cpp
    src/debug_server.cpp
    src/hook.cpp
    src/resolve.cpp
    src/targets.cpp
    src/console.cpp
    src/stackdump.cpp
    src/fast_alloc.cpp
    src/win_compat.cpp
    src/affinity.cpp
    src/vulkan_memory.cpp
    src/vulkan_forward.cpp
    src/detour_interpose.cpp
    src/inflate.cpp
    src/pak.cpp
    src/lsf.cpp
    src/pak_fix.cpp
    src/pak_fix_hooks.cpp
    src/game_files.cpp
    src/sdl_forward.cpp
    src/math.cpp
    src/ext_libs.cpp
    src/builtin_lua.cpp
    src/plugins.cpp
    src/net.cpp
    # Yann Collet's LZ4, vendored rather than linked: the sniper container
    # need not have liblz4, and a missing DT_NEEDED would stop bg3le loading
    # at all. See vendor/NOTICE.md.
    external/lz4/lz4.c
    # zstd's single-file decompressor, vendored for the same reason.
    external/zstd/zstddeclib.c
)
set_source_files_properties(external/lz4/lz4.c PROPERTIES COMPILE_FLAGS -w)
set_source_files_properties(external/zstd/zstddeclib.c PROPERTIES COMPILE_FLAGS -w)
# The version plugins see in bg3le_host, from git at configure time.
execute_process(COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BG3LE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BG3LE_VERSION)
    set(BG3LE_VERSION unknown)
endif()
set_source_files_properties(src/plugins.cpp PROPERTIES
    COMPILE_DEFINITIONS "BG3LE_VERSION=\"${BG3LE_VERSION}\"")
# bg3se's builtin Lua scripts, for builtin:// paths.
set(BUILTIN_LUA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/bg3se/BG3Extender/LuaScripts)
file(GLOB_RECURSE BUILTIN_LUA_FILES ${BUILTIN_LUA_ROOT}/*.lua)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
# Upstream's IdeHelpersNativeData.lua, which it builds outside its repository.
set(BUILTIN_EXTRA_ROOT ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_extra)
file(GLOB BG3SE_LUA_LIBS ${VENDOR_ROOT}/BG3Extender/Lua/Libs/*.inl)
add_custom_command(
    OUTPUT ${BUILTIN_EXTRA_ROOT}/Libs/IdeHelpersNativeData.lua
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-ide-native-data.py
            ${VENDOR_ROOT}/BG3Extender ${BUILTIN_EXTRA_ROOT}/Libs/IdeHelpersNativeData.lua
    DEPENDS ${BG3SE_LUA_LIBS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-ide-native-data.py)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_lua.inc
    COMMAND ${CMAKE_COMMAND} -DROOT=${BUILTIN_LUA_ROOT} -DEXTRA=${BUILTIN_EXTRA_ROOT}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_lua.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-builtin-lua.cmake
    DEPENDS ${BUILTIN_LUA_FILES} ${BUILTIN_EXTRA_ROOT}/Libs/IdeHelpersNativeData.lua
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-builtin-lua.cmake)
set_source_files_properties(src/builtin_lua.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_lua.inc)
target_sources(bg3le PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_lua.inc)
target_include_directories(bg3le PRIVATE external/lz4 external/zstd
    ${EXTERNAL}/glm ${CMAKE_CURRENT_BINARY_DIR}/generated)
# vulkan and SDL2 resolve from the host process, which has both loaded before
# bg3le is.
target_link_libraries(bg3le PRIVATE dl lua_static bg3se_vendor vulkan SDL2)

# No --no-undefined: the Noesis typeinfo references are deliberately left
# unresolved and weak. See src/vendor/noesis_rtti_linux.cpp for why nothing on
# Linux can satisfy them, and why it is safe while Ext.ClientUI is off.
target_link_options(bg3le PRIVATE -Wl,--warn-unresolved-symbols)
target_compile_options(bg3le PRIVATE -Wall -Wextra -fvisibility=default)

add_executable(symdump tools/symdump.cpp src/elf_symbols.cpp src/log.cpp)

# Builds a modsettings.lsx from a directory of .paks, reusing the archive
# reader rather than reimplementing LSPK in a script.
# Greps another process memory, for questions that have to be answered
# with the shim absent.
# ---------------------------------------------------------------------------
# memsteer: the Vulkan memory steering on its own, for other native games.
#
# The same src/vulkan_memory.cpp bg3le links, against a stderr log instead of
# the extender's. One implementation deliberately: the reason to extract it was
# to point the same steering at another game, not to keep a second copy of it.
#
#     env MEMSTEER=on LD_PRELOAD=/path/to/memsteer.so ./the-game
#
# See MEMSTEER.md.
# ---------------------------------------------------------------------------
add_library(memsteer SHARED src/vulkan_memory.cpp src/memsteer_log.cpp)
set_target_properties(memsteer PROPERTIES PREFIX "" POSITION_INDEPENDENT_CODE ON)
target_include_directories(memsteer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The loader is linked, not only dlopen'd: without it nothing has pulled
# libvulkan in when the engine asks for its first proc address, dlsym's
# RTLD_NEXT misses, and every pointer handed back is null. The game then
# crashes in its own code with nothing of ours on the stack.
target_link_libraries(memsteer PRIVATE dl vulkan)

add_executable(memgrep tools/memgrep.c)
# A C program, so the global -stdlib=libc++ has nothing to act on and clang
# says so. The flag is added for every target above; dropping it here is
# narrower than making it conditional there.
target_compile_options(memgrep PRIVATE -Wno-unused-command-line-argument)

add_executable(modsettings tools/modsettings.cpp src/pak.cpp
               src/inflate.cpp src/log.cpp external/lz4/lz4.c
               external/zstd/zstddeclib.c)
target_include_directories(modsettings PRIVATE external/lz4 external/zstd src)

# The corrected copy bg3le reads in place of a mod archive, from the same code.
add_executable(pakfix tools/pakfix.cpp src/pak_fix.cpp src/lsf.cpp src/pak.cpp
               src/inflate.cpp src/log.cpp external/lz4/lz4.c
               external/zstd/zstddeclib.c)
target_include_directories(pakfix PRIVATE external/lz4 external/zstd src)
