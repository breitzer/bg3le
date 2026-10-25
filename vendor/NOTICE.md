# Third-party code

## bg3se — Baldur's Gate 3 Script Extender

`vendor/bg3se/` is a partial copy of the [Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se) by **Norbyte** and the bg3se
contributors, licensed MIT with the Commons Clause (see `vendor/bg3se/LICENSE`).

**Thank you.** The 640 component definitions, the Lua binding framework, the
extender core and the code generators in this directory represent an enormous
amount of reverse engineering. bg3le reuses them rather than rediscovering
them, and would not be a realistic project otherwise.

Upstream commit: `af85fd2b`, tag `v33` (2026-10-12). Newer upstream commits are brought in
by applying upstream's own diff over this tree, which keeps the changes listed
below; regenerate the generated files afterwards.

Copied subsystems: `CoreLib/`, `BG3Extender/GameDefinitions/`,
`BG3Extender/Lua/`, `BG3Extender/Extender/`, `BG3Extender/GameHooks/`,
`BG3Extender/Osiris/`, `BG3Extender/LuaScripts/` (the builtin:// bundle,
unmodified, embedded by `cmake/embed-builtin-lua.cmake`), plus `stdafx.h`,
`resource.h` and `BG3Updater/ExtenderAPI.h`.

`external/lua` is Norbyte's Lua fork; see
[external/lua/README.bg3le](../external/lua/README.bg3le) for why that one is
not optional.

### Changes made

The upstream build is MSVC-only. Every change below exists solely to compile
the same code with clang, and each is a construct MSVC would also accept, so
they are candidates for an upstream conformance PR. Nothing was changed for
behaviour.

Run `tools/check-vendor-patches.py` to confirm they are all still applied —
re-copying files from an upstream checkout silently reverts them.

**A requires-clause cannot begin with `!` on a primary expression** — wrapped
the constraint in parentheses:

- `BG3Extender/GameDefinitions/Base/BaseTypeInformation.h:349`
- `BG3Extender/Lua/Helpers/LuaGet.h:295,298`
- `BG3Extender/Lua/Helpers/LuaGetObject.h:5,13`
- `BG3Extender/Lua/Helpers/LuaTypeCheck.h:241,244`
- `BG3Extender/Lua/Helpers/LuaTypeCheckObject.h:26,38`
- `BG3Extender/Lua/LuaSerializers.h:543`

**Stray `typename` before a builtin type** — removed:

- `BG3Extender/Lua/Helpers/LuaTypeCheckObject.h:21`
- `BG3Extender/Lua/Shared/LuaTypeValidators.h:302,723,731`

**Friend declaration of a class template without its arguments** —
`friend FunctionImpl;` became
`template <class TFun, class TData> friend class FunctionImpl;`:

- `CoreLib/Base/BaseFunction.h:186`

**A variadic macro leaves a trailing comma when given no variadic argument.**
MSVC drops it; standard C++ needs `__VA_OPT__`. Applied to `DEBUG`, `INFO`,
`WARN`, `ERR`, their `_LOCAL` variants and `WARN_ONCE`:

- `CoreLib/Utils.h:10-21`

**`__FUNCTION__` is a variable under clang, not a string literal**, so it
cannot be concatenated at compile time. The stringstream macros stream it and
the `...S` macros build the string at runtime:

- `BG3Extender/Extender/Shared/Utils.h` (`LuaError`, `OsiError`, `OsiWarn`,
  `OsiErrorS`, `OsiWarnS`, `OsiMsgS`)

**`std::thread` was forward-declared.** libc++ declares it in an inline
namespace, so a second declaration is a distinct type and every use becomes
ambiguous. Replaced with `#include <thread>`:

- `BG3Extender/Extender/Shared/Utils.h:6`

**`FixedStringUnhashed` had no stream operator.** It is a sibling of
`FixedString`, not a `FixedString`, so the existing overload did not apply and
insertion was ambiguous between the base class conversions to `char const*`
and to `StringView`. Added the matching overload:

- `CoreLib/Base/BaseString.h`

**A static data member of a class template specialisation needs `template<>`**:

- `BG3Extender/Extender/Client/SDLManager.h:15` (the `SDL_HOOK` macro)
- `BG3Extender/Lua/Libs/ClientUI/Symbols.inl:11` (the `FOR_NOESIS_TYPE` macro,
  expanded for 23 Noesis types)

**`operator new` must take `size_t` exactly.** Upstream declares it as
`unsigned __int64`, which is the same width as LP64 `size_t` but a different
type (`unsigned long long` vs `unsigned long`):

- `BG3Extender/Lua/Libs/ClientUI/Builtins.inl:169,174`

**A `void*` cannot be `static_cast` to a function pointer**; that needs
`reinterpret_cast`:

- `BG3Extender/Lua/Libs/ClientUI/NsHelpers.inl:717`

**SFINAE has to depend on a parameter of the function template, not of the
enclosing class.** `TryOpOrFail<T>` detects whether `T` has `Do`/`DoInPlace`
via a trailing return type, but `T` is fixed once the class is instantiated,
so a missing member is a hard error rather than a substitution failure in the
immediate context. Aliasing `T` as a defaulted parameter on each member
template moves the lookup to where SFINAE applies:

- `BG3Extender/Lua/Libs/Math.inl` (`TryOpOrFail::Do` and `::DoInPlace`)

**`lua_Integer` is `long long`, while `int64_t` is `long` on LP64**, so
constructing `std::variant<char const*, int64_t, double>` from a `lua_Integer`
has no viable alternative without an explicit cast. On MSVC they are the same
type:

- `BG3Extender/Lua/Libs/Json.inl:373,375`

**An `STDString` was passed through a variadic function.** That is undefined on
both platforms; MSVC only warns:

- `BG3Extender/Lua/Libs/ClientAudio.inl:56` — now passes `c_str()`

**`std::derived_from` requires complete types**, so testing an incomplete type
is a hard error rather than a false. `IsArray` is evaluated against types that
are only forward-declared at that point, so the check is now guarded by an
`IsCompleteType` concept:

- `BG3Extender/GameDefinitions/Base/TypeMetadata.h:19`

**Declaring any class-scope `operator delete` hides the global ones**, and an
inherited virtual destructor still needs a usual deallocation function.
`NsCustomDataContext` declared only the placement form:

- `BG3Extender/Lua/Libs/ClientUI/CustomProperties.inl`

**A non-type template parameter must match the exact type to deduce.**
`std::array`'s extent is `std::size_t`, so `template <class T, int Size>` never
matches it. MSVC deduces anyway. This one accounted for 71 errors in
`LuaObjectProxies.cpp` alone, and their own serialisation helpers already used
`size_t`:

- `BG3Extender/Lua/Shared/Proxies/LuaArrayProxy.h:641,647`
- `BG3Extender/GameDefinitions/Base/BaseTypeInformation.h:270`

**Converting a function pointer to `void*` is conditionally supported**, not
standard. MSVC does it implicitly; the casts are now explicit:

- `BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp` (the `P_FALLBACK` macro)

**libc++ has no wide-path `fstream` constructor**; MSVC provides one as an
extension. These now convert with upstream's own `ToUTF8`:

- `BG3Extender/Lua/Shared/LuaBundle.cpp:39`
- `CoreLib/Crypto.cpp:85`

**A pointer-to-member-function is not the same width as a function pointer on
the Itanium ABI.** MSVC makes them both 8 bytes for a single-inheritance
non-virtual class, so upstream reinterprets one as the other wholesale. Linux
uses two words, `{entry, this-adjustment}`, so the conversion has to move one
word: reading a `MethodType` out of a `FunctionType` over-reads by eight
bytes. Valid only for non-virtual members, since Itanium encodes a virtual one
as a vtable offset:

- `CoreLib/Wrappers.h` (`MethodPtrHelpers::ToFunction`/`::ToMethod`, and the
  `static_assert` that compared the two sizes)

**Converting a function pointer to or from `void*` is conditionally supported**
and never implicit. MSVC does it silently:

- `CoreLib/Wrappers.h` — `ResolveRealFunctionAddress` arguments, the
  `gRegisteredTrampolines` inserts, the `func_` assignments, and
  `static_cast` to the hook types, which needs `reinterpret_cast`
- `BG3Extender/GameDefinitions/EntitySystem.cpp:886,944` — `ProxyDestroy`

**A `##` cannot lead a replacement list** — the operator has to sit between two
tokens. MSVC ignores a stray one, and this single macro broke three TUs:

- `BG3Extender/Extender/Shared/ExtenderConfig.h:79` (`PERF_REPORT`)

**A `decltype` specifier cannot appear in a declarative nested name
specifier**, so the hook type has to be named:

- `BG3Extender/Extender/Shared/Hooks.cpp:8`

**libc++ has no wide-path `fstream` constructor**; MSVC provides one as an
extension. These convert with upstream's own `ToUTF8`:

- `BG3Extender/Lua/Shared/LuaBundle.cpp:39`
- `BG3Extender/Extender/Shared/ScriptHelpers.cpp:70,117`
- `CoreLib/Crypto.cpp:85`
- `CoreLib/Utils.cpp` (four sites)

**Winsock spellings that POSIX names differently:**

- `BG3Extender/Osiris/Debugger/DebugInterface.cpp` — `in_addr::S_un.S_addr` is
  `s_addr`, and `accept` takes a `socklen_t*` rather than an `int*`

**SEH does not exist on Linux.** `BEGIN_GUARDED`/`END_GUARDED` wrapped engine
callbacks in `__try`/`__except`, and clang cannot compile that when targeting
Linux; the handler also lives in `CrashReporter.cpp`, which bg3le does not
build. They now expand to a C++ `try`/`catch`, which covers the portable half
of the intent — keeping an exception from escaping into engine code across the
ABI boundary — but cannot catch a hardware fault. bg3le reports those from a
signal handler instead:

- `BG3Extender/GameDefinitions/Base/Base.h:13`

**`push` was ambiguous for `long long`.** The overloads cover `int64_t` and
`uint64_t`, which on LP64 are `long` and `unsigned long`, so a `long long`
converts equally well to either. On Windows `int64_t` *is* `long long`, so it
matched exactly; the added overloads are guarded out there to avoid
redefining:

- `BG3Extender/Lua/Helpers/LuaPush.h`

**A function-pointer cast is not a constant expression.** The property-map
fallback entries type-erase function pointers into `void*`, so the tables
cannot be `constexpr`. `AllClassDefns` only stores their addresses, which
stays constant, and they are walked at runtime, so `inline const` is
sufficient:

- `BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp` (`Definitions`)

**`Wrap(void*)` will not take a typed function pointer implicitly**, and a
`decltype` specifier cannot appear in a declarative nested name specifier, so
the static hook members have to be defined through an alias:

- `BG3Extender/Extender/ScriptExtender.cpp:26`
- `BG3Extender/Extender/Server/ScriptExtenderServer.cpp:7` (`STATIC_HOOK`)
- `BG3Extender/Extender/Client/ScriptExtenderClient.cpp:7` (`STATIC_HOOK`)
- `BG3Extender/Extender/Shared/Hooks.cpp:8`

**An explicit specialisation of a static data member is only a definition if
it has an initialiser.** Adding `template<>` to satisfy the rule above turns
the declaration into just that — a declaration — and the symbol is never
emitted. Every one of them needs an initialiser:

- `BG3Extender/Extender/Client/SDLManager.h` (`SDL_HOOK`)
- `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (`VK_HOOK`)
- `BG3Extender/Lua/Libs/ClientUI/Symbols.inl` (`FOR_NOESIS_TYPE`)
- `BG3Extender/Extender/ScriptExtender.cpp`,
  `.../Server/ScriptExtenderServer.cpp`, `.../Client/ScriptExtenderClient.cpp`,
  `.../Shared/Hooks.cpp`

**The D3D11 ImGui backend cannot build on Linux.** There is no D3D11 build of
the native game, so only the Vulkan backend is compiled and the backend
selection is fixed accordingly, under `BG3LE_NO_DX11`:

- `BG3Extender/Extender/Client/IMGUI/IMGUI.cpp`

**An explicit specialisation of a variable template has external linkage and
is not implicitly inline**, so every including translation unit emits a
definition. MSVC folds them as COMDAT; ELF reported 134,567 duplicates across
the generated `StructID`/`EnumID` tables:

- `BG3Extender/GameDefinitions/Base/TypeMetadata.h` (`MARK_BY_VALUE_TYPE`,
  `MARK_INTEGRAL_ALIAS`)
- `BG3Extender/GameDefinitions/Base/LuaAnnotations.h`
- `BG3Extender/GameDefinitions/Enumerations.h` (the `BEGIN_ENUM` family)
- `BG3Extender/Lua/Shared/Proxies/LuaStructIDs.h` (the `DECLARE_CLS` family)

The same applies to the static data members in headers, which additionally
have to be `inline` rather than merely initialised:

- `BG3Extender/Extender/Client/SDLManager.h`,
  `BG3Extender/Lua/Libs/ClientUI/Symbols.inl`

**An inline function has to be defined in every translation unit that uses
it.** `VMCallEntry`'s constructor was declared `inline` in the header and
defined out-of-line in one `.cpp`, so no symbol was emitted and every other
caller was left with an undefined reference. MSVC emits it anyway:

- `BG3Extender/Lua/LuaBinding.h:108`

**A pure virtual destructor still needs a definition** — derived destructors
call it and the vtables reference it. `aspk::Component` only ever describes
engine memory, so MSVC never demanded the symbol; it is defined in
`src/vendor/platform_linux.cpp`.

**An include used the wrong directory case**, which resolves on Windows and
not on Linux:

- `BG3Extender/Lua/Shared/LuaStats.h:4` — `lua/LuaBinding.h` → `Lua/LuaBinding.h`

**UTF-16 sources** — MSVC accepts them, clang does not. Transcoded to UTF-8:

- `CoreLib/Config.h`
- `BG3Extender/Extender/BuildInfo.h`

### GlobalSwitches is laid out as this build's

`GameDefinitions/Misc.h` gains two hidden members: an eight-byte field before
`SoundSetting::UpdateProc`, which makes each of the twelve sound settings
0x68 bytes as the engine has them, and eight bytes after `SomeSettings`. With
those, all 46 members whose offset the engine's settings registration reveals
(`tools/relocs-xref.py`; see `reference/GLOBAL-SWITCHES.md`) sit where bg3se
declares them. Checked by `tools/check-vendor-patches.py`.

### The entity handle generator's thread states start at +0

`GameDefinitions/EntitySystem.h`'s `EntityHandleGenerator` no longer derives
from `ProtectedGameObject`. Its `ThreadState` does, and under the Itanium ABI
two `ProtectedGameObjectBase` subobjects cannot share an address, so
`ThreadStates` landed at +0x40; MSVC puts it at +0, as the engine indexes it
(`generator + thread * 64`, image+0x320b090). Every thread's state was read
from its neighbour's. `src/vendor/entity_bridge.cpp` asserts the offset.

### The entity handle generator grows as the engine's

`GameDefinitions/EntitySystem.cpp`'s `EntityHandleGenerator::ThreadState::Add`
grew an empty thread state twice -- `resize()` to a page, then an `add()` per
new entry past it -- leaving the first page's entries off the free list and a
second page the engine does not expect. It now grows by one page and links
every new entry into the free list, which is the state the engine's own
threads show: one 32,768-entry page, `{Index = i + 1, Salt = 1}`. Checked by
`tools/check-vendor-patches.py`.

### ModifierList::Name sits in the attribute manager's tail padding

`GameDefinitions/Stats/Stats.h`: `CNamedElementManager<Modifier>` holds 92
bytes of data in a 96-byte object, and the engine puts `ModifierList::Name` at
+92, where Itanium places a field after a potentially-overlapping subobject.
As a plain member it landed at +96 and read half a pointer. `Attributes` is
marked `[[no_unique_address]]`, which lays it out that way.
`src/vendor/stats.cpp` asserts the offset; checked by
`tools/check-vendor-patches.py`.

### Generated files

Upstream gitignores these; they are committed here so the tree builds without
a generation step. Regenerate with the upstream scripts, which run unchanged
under python3, and with protoc:

    python3 vendor/bg3se/BG3Extender/make_enumerations.py
    python3 vendor/bg3se/BG3Extender/make_property_map.py
    cd vendor/bg3se/BG3Extender
    protoc --cpp_out=. Extender/Shared/ExtenderProtocol.proto
    protoc --cpp_out=. Osiris/Debugger/osidebug.proto
    protoc --cpp_out=. Lua/Debugger/LuaDebug.proto

### Four changes of behaviour, not of syntax

Everything above is a clang or ABI fix: the code still does what upstream
wrote. These four do something different, because on Linux the thing
upstream relies on is not reachable. `tools/check-vendor-patches.py` checks
all four, since re-copying a file from upstream reverts them silently.

**`GameDefinitions/GameHelpers.cpp` — `MakeFileReader` reads the archives.**
Upstream opens a data file through `ls::FileReader`'s constructor, and no
engine function in this build carries a symbol to call. Every caller got
"File reader API not available!", including `IMGUIManager::LoadFont`, which
left the imgui font atlas empty — and Norbyte's imgui fork has
`AddFontDefault()` disabled, so an empty atlas draws nothing at all.
`FileReader` is a plain struct, so bg3le reads the game's own LSPK archives
(`src/game_files.cpp`) and fills one in (`src/vendor/file_reader.cpp`).
`DestroyFileReader` releases a reader that came from there and falls through
otherwise.

**`Extender/Client/SDLManager.h` — four public entry points.** Upstream
detours `SDL_CreateWindow`, `SDL_PollEvent` and the text-input trio. bg3le
has no inline hooks, but the game imports all five from `libSDL2.so` by name,
so `src/sdl_forward.cpp` exports them and the dynamic linker routes the calls
to `src/vendor/sdl_linux.cpp`, which is bg3le's implementation of the class.
`OnCreateWindow`, `OnPollEvent`, `OnIsTextInputActive` and `WantsTextInput`
are the same bodies as the private detour hooks, reachable from outside.

**`Lua/Shared/LuaDelegate.h` — a delegate is an id, not a registry
entry.** Upstream's holds a `lua::RegistryEntry`, which finds its manager
through `lua::State::FromLua(L)` — bg3se's own Lua state, which bg3le never
starts, so constructing one dereferenced a null. And calling one marshals the
arguments through bg3se's userdata machinery, whose metatables are registered
during that same state's init, so a widget would reach Lua as an object with
no methods on it. So `LuaDelegate` holds an id into bg3le's own table and
`Call` posts the arguments to a queue drained on the thread that owns the
context which registered the callback — `src/vendor/imgui_events.cpp`. A
widget fires on the render thread, which must not touch a Lua state.

**`Lua/LuaBinding.cpp` — `nse_lua_report_handled_error` returns while
there is no `gExtender`.** The Lua fork calls it on every error raised under
`xpcall`, and upstream reads `gExtender->GetLuaDebugger()` unconditionally.
Upstream creates the extender before any Lua runs; bg3le creates it when the
overlay starts, which is after the engine heap is up and never when running
headless. Until then `xpcall(f, debug.traceback)` around any error killed
the game.

**`Extender/Client/IMGUI/IMGUI.cpp` — `ImageReference::BindIcon` returns
false while `ls__gTextureAtlasMap` is unset.** Upstream dereferences it
unconditionally; bg3le has not located the texture atlas map, so an
`AddImageButton` with an icon crashed the game. It now fails the way an
unknown icon does, and the button is drawn without its image.

**`Extender/Client/IMGUI/IMGUI.cpp` — `IMGUIManager::Update` draws
`ShowErrorAndExitGame`'s dialog.** One call, `bg3le_imgui_draw_error()`, after
the mods' windows and before `ImGui::Render`, so the message is drawn in the
game's own frame on every machine rather than by a desktop dialog.

**`Extender/Client/IMGUI/IMGUI.cpp` — the texture loader reaches the engine
through bg3le.** `ls::gGlobalResourceManager` stays unset, because other
vendored code would read upstream's layouts through it. `LoadTexture` takes
the bank lookup from `bg3le_resource_bank_get` and the engine's reference
from `bg3le_texture_acquire` (`TextureManager::LoadTexture`, found by
signature); `Update` drops it with `bg3le_texture_release`
(`TextureManager::UnloadTexture`). A release still pending when the texture
is loaded again drops its reference, where upstream leaked it.

**`Extender/Client/IMGUI/IMGUI.{h,cpp}` — icon atlases register their
resident texture.** On this build `TextureAtlas::Texture` holds the atlas
texture's `TextureDescriptor` (a Vulkan image with one view), not a
`TextureResource`; a live atlas reads that way field for field. `BindIcon`
hands it to the texture loader, which takes an engine reference to the
atlas's texture as upstream does, so the engine keeps it while ImGui draws
it. Only a texture the engine does not track is registered as it is, marked
resident so no reference is dropped for it.

**`Lua/Libs/ClientUI/Builtins.inl` and `GameDefinitions/UI.h` — the Noesis
functions go to the game's own copies.** Upstream links no Noesis library, so
these reimplement `Reflection`, `SymbolManager`, `TypeClass`, `BaseCommand`
and the rest against Windows data layouts and SRWLOCKs; the Linux game has
all of them as local symbols and locks with `pthread_spin_lock`. Under
`BG3LE_NOESIS_FORWARD` the reimplementations are compiled out, and
`src/noesis_forward.cpp` defines each as a jump to the game's function,
resolved from `.symtab`. `src/vendor/bg3le_noesis_builtins.inl` keeps
`LuaDelegateCommand` and the one function the game lacks.

**`Lua/Libs/ClientUI/Module.inl` — `Ext.UI.GetRoot` returns the View's
content, and bg3le's Ext.UI bridge is included.** Upstream reads the root
from `gGlobalResourceManager`, which bg3le has not located; the View is
found by hooking its per-frame `Update`. `GetStateMachine` returns null
instead of dereferencing the missing manager. The bridge,
`src/vendor/bg3le_noesis_lua.inl`, is included at the end so it can use
upstream's class cache and custom-type builder.

**`GameDefinitions/Stats/Stats.h` — `RPGStats` has a fifth pointer before
`RNG`.** Between `StatsFunctors` (at +0x248 as declared) and `ExtraData` this
build holds ten words where the declaration has nine, so every member from
`ExtraData` on sat 8 bytes early: `ExtraData` at +0x2b0, `TreasureRarities`
at +0x328, `FixedStrings` at +0x348 and the four manager pointers at +0x398
to +0x3b0 on the live object. `field_280_bg3le` puts them back;
`src/vendor/stat_create.cpp` asserts the offsets and
`tools/check-vendor-patches.py` checks the field is there.

**`GameDefinitions/UI.h` — `UIStateMachine` has a map and a set more.** On
this build a 0x40-byte map follows `field_130_MHM_Guid_pState` and a 0x30-byte
set follows `field_1D0_pState`, so every field from there on sat 0x70 early:
`PlayerID` read 21512 and `RootState` nothing. With the two in, `RootState`
reads "Root" and `PlayerID` 1, and each map and set after them lines up with
the live object. `UIStateInstance` is re-laid out from thirty live instances:
`State` at +0x20 (each names its own `UIState`), `PlayerID` and the flags at
+0x28, the three strings at +0x70, the GUIDs at +0xd0 and `StateWidgets` at
+0x100; the bytes between hold nothing the declaration names, so `States`,
`field_40` and `field_44`, not found, sit where the memory reads empty.
`src/vendor/ui_globals.cpp` asserts the offsets; checked by
`tools/check-vendor-patches.py`.

**`Lua/Libs/ClientUI/CustomProperties.inl` — a custom property's
`WriteCallback` goes through bg3le's UI queue.** Upstream queues it on its
client Lua state's `DeferredUIEvents`, which bg3le does not have. Under
`BG3LE_NOESIS_FORWARD` the setter asks bg3le whether the class's property has
a callback and, when the value changes, hands the object and property name to
`src/vendor/bg3le_noesis_lua.inl`, which delivers them on the client tick as
upstream's `PostUpdate` does. Checked by `tools/check-vendor-patches.py`.

**`Lua/Libs/ClientUI/CustomProperties.inl` — a wrapped DataContext property
registers its base with bg3le.** `TypePropertyDataContextWrapped` forwards
`Get`/`GetCopy` to the base property in the headers' slot order, but the
engine's two Get overloads sit in the opposite slots, and some engine
properties register no getter at all. The constructor hands bg3le the wrapper
and its base, and `src/vendor/bg3le_noesis_lua.inl` reads such a property as
the base on the wrapped object, through its slot-aware accessors. Checked by
`tools/check-vendor-patches.py`.

**`Extender/Client/IMGUI/Vulkan.inl` — the overlay is composited onto an
HDR swapchain.** Upstream draws ImGui straight into the swapchain; when the
compositor offers HDR the game presents HDR10, and both ImGui's sRGB colours
and its blending came out wrong. For an HDR swapchain the overlay now draws
into an SDR image with its own render pass, and `src/vendor/imgui_hdr.cpp`
lays it over the game's frame with a fullscreen shader. An SDR swapchain
takes upstream's path unchanged.

**`Extender/Client/IMGUI/Vulkan.inl` — only live image views are drawn.**
`RegisterTexture` and `BindTexture` ask `src/vulkan_forward.cpp`, which
tracks `vkCreateImageView` and `vkDestroyImageView`, whether a texture's view
is live, and refuse it with an error if not. A bad view crashed NVIDIA's
driver in `ImGui_ImplVulkan_AddTexture` when MCM opened. When a registered
view dies anyway, `ImageReference::PrepareRender` binds the image again and
the loader registers the texture's current view, retiring the old
registration after `DeleteAfterFrames`.

**`Extender/Client/IMGUI/Vulkan.inl` — one descriptor pool and sampler per
device, and binds locked.** Upstream's `InitializeUI` made a new pool and
sampler each time the overlay was rebuilt, which is every swapchain
recreation, and freed them only with the device: one of each leaked per
window resize. They are made once and reused, so cached texture descriptors
stay valid too, and the device teardown destroys them before clearing
`device_` rather than after. `BindTexture` and `UnregisterTexture` take
`globalResourceLock_` and check `initialized_` as their siblings do, so
neither reaches the ImGui backend while `DestroyUI` shuts it down (bg3se
issue #597).

## vendor/compat — bg3le's own code

Shims that let the upstream sources compile unmodified. They are force-included
or sit ahead of the vendored tree on the include path.

- `msvc_compat.h` — SAL annotations, Win32 typedefs (`DWORD` and `LONG` are
  32-bit on Windows, so they are `unsigned int` and `int`, not `long`), the
  MSVC bit-scan intrinsics, the secure-CRT `sprintf_s` family, `_strdup`,
  `VirtualProtect` over `mprotect`, `QueryPerformanceCounter` over
  `CLOCK_MONOTONIC`, `GetCommandLineW` over `/proc/self/cmdline`,
  `GetProcAddress`/`GetModuleHandleW` over `dlsym`/`dlopen`, critical sections
  over recursive `pthread_mutex`, `SRWLOCK` as the engine's own
  `pthread_rwlock_t`, and the byte-swap and Interlocked intrinsics
  over the compiler builtins. Basic Win32 typedefs are declared first, since
  the rest of the header uses them
- `Shlwapi.h`, `shlwapi.h`, `combaseapi.h`, `WS2tcpip.h` — `PathFileExistsW`,
  the RPC UUID functions (faithful to the Windows GUID layout, since Guid
  values round-trip through Osiris), and the TCP/IP half of Winsock
- `concurrent_vector.h`, `concurrent_queue.h`, `ppl.h` — MSVC's
  `concurrency::` containers mapped onto [oneTBB](https://github.com/uxlfoundation/oneTBB)
  (Apache-2.0)
- `WinSock2.h` — the Osiris debugger interface is written against Winsock;
  Berkeley sockets map directly
- `detours.h` — declarations only. The sole upstream user is
  `CoreLib/Wrappers.h`, whose callers bg3le replaces with PLT interposition, so
  these refuse rather than hook; `Wrap()` already handles a non-zero return

## What the link still needs

`bg3le` does not link `vendor/bg3se` yet. It is short of eleven symbols: C++
RTTI for Noesis types (`typeinfo for Noesis::Panel` and siblings), reached
through `typeid`/`dynamic_cast` in the generated property-map metadata.

Nothing on Linux can satisfy them. The Noesis SDK we fetch is headers plus
Windows `.lib` files, and the native game carries no Noesis typeinfo either --
28,992 Noesis symbols in `.symtab` and not one typeinfo, because Noesis uses
its own reflection system rather than C++ RTTI. The fix is to keep the Noesis
types out of the generated property maps, not to shim a symbol.

## Not vendored

- **NoesisGUI** — proprietary SDK fetched by `tools/fetch-externals.sh`, which
  also drops a stray `override` in `NsCore/TypePropertyImpl.h` that clang
  rejects (the base declares `GetCopy`, not that overload, so it never
  overrode anything).
- **glm, imgui, rapidjson, tinycrypt, optick, Vulkan-Headers** — fetched, each
  under its own license.
- **protobuf, SDL2, oneTBB** — from the distribution.

## Zstandard — Meta Platforms and contributors

`external/zstd/` is the decompressor from [Zstandard](https://github.com/facebook/zstd)
v1.5.7 by **Yann Collet and the Zstandard contributors at Meta**, BSD
(see `external/zstd/LICENSE`). `zstddeclib.c` is the release's single-file
decoder, generated with its own `build/single_file_libs/combine.sh` from
`zstd-1.5.7.tar.gz` (sha256 `eb33e51f…6fa3`), and `zstd.h` and `zstd_errors.h` are the release's
headers. Unmodified.

**Thank you.** Newer LSLib builds pack mods with zstd, and without it bg3le
could not read those mods' scripts. Vendored rather than linked for the same
reason as LZ4.

## LZ4 — Yann Collet

`external/lz4/` is the block codec from [LZ4](https://github.com/lz4/lz4)
v1.10.0 by **Yann Collet**, BSD 2-Clause (see `external/lz4/LICENSE`).

**Thank you.** bg3le needs it to read Larian's LSPK archives, which is how it
works out which mod defines each stat — the one thing upstream gets by
hooking the engine and that no symbol in the Linux build allows.

Vendored rather than linked against the system library: bg3le is preloaded
into a game that may run inside the Steam sniper container, which need not
have `liblz4.so`, and a missing `DT_NEEDED` would stop it loading at all.
Unmodified.

### STDString is not std::string here

`CoreLib/Base/BaseString.h` defined `STDString` as
`std::basic_string<char, ..., GameAllocator<char>>`. That is right on
Windows, where Larian's string is MSVC's `std::string` at 32 bytes. The
native Linux build's is **16 bytes**, and is not any `std::string` --
libc++'s is 24. `CoreLib/Base/LSString.h` (bg3le's own, added here) defines
it: up to fifteen characters inline with the length in the last byte,
otherwise a pointer, a size and a capacity whose top bit marks the heap
form.

This is the one vendored change that is not purely about compiling with
clang, and it is not optional: every struct holding an `STDString` is laid
out wrong without it. It was proven twice against the running game --
"Shared" inline at `ModuleInfo+32` with its length at `+47`, which is the
only way the 240-byte `Module` stride adds up, and the 3,125 entries of
`RPGStats::Conditions` reading back as valid expressions at 16-byte spacing.

One call site changed with it: `LuaDebugger.cpp:1027` passes `.c_str()` to
`std::regex_match`, which has no overload for the new type.
