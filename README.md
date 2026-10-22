# bg3le

A script extender for the **native Linux build** of Baldur's Gate 3.

The existing Script Extender targets the Windows build, so Linux players run
the game under Proton to get it. bg3le attaches to `bin/bg3` directly.

**Download it from [Nexus Mods](https://www.nexusmods.com/baldursgate3/mods/25431):**
a ready-built release that loads on any Linux system the game runs on. Unzip
it and run `./install.py`. Building from source is below.

## Credit

This project stands on [Norbyte's Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se). The game-structure definitions and
the Lua binding framework under `vendor/bg3se/` are theirs (MIT + Commons
Clause); bg3le reuses them rather than rediscovering years of reverse
engineering, and would not be a realistic project otherwise. **Thank you.**

See [vendor/NOTICE.md](vendor/NOTICE.md) for attribution and every change made
to the vendored code — mostly to compile under clang rather than MSVC, plus the
layout fixes this build's ABI needs and the hooks into bg3le. Its component
definitions have been checked against the
native build rather than assumed: `Ext._Internal.SizeAudit()` compares every
component's declared size with the size the engine recorded, and
`tools/meta-check.c` checks field offsets without needing the game.

## What works

bg3le implements the public `Ext` API of bg3se on the native build: every
function upstream registers is present in both contexts
(`tools/check-api.sh`, against upstream's own module declarations), and
`tools/count-refusals.py` finds none that refuse.
It is checked against output captured from the Windows extender
(`reference/`) and against upstream's own Lua tests. How each engine
structure was found, and what was measured, is in
[reference/IMPLEMENTATION-NOTES.md](reference/IMPLEMENTATION-NOTES.md).

- **Mods run.** Script Extender mods load straight out of their `.pak`s (or
  from loose directories via `BG3LE_MOD_PATH`) with upstream's mod
  environment, bootstraps (`BootstrapModule.lua` and `ModuleLoadStarted`
  before the stats load, then each context's own), `Ext.Require` and
  `require`. `PersistentVars`
  and persistent variables are saved in the savegame in bg3se's format, so a
  save moves between the two. Mod Configuration Menu works, menu and all.
- **Two Lua contexts**, server and client, each on its own thread and
  reading its own entity world, as upstream has them. `Ext.Net` messages and
  synced mod and user variables cross between them; sessions and
  `Ext.Debug.Reset()` rebuild both.
- **Multiplayer**: `Ext.Net` and synced variables travel over the game's
  connection in bg3se's own protocol, so a bg3le host serves clients running
  bg3se on Windows (or under Proton) and bg3le alike, and a bg3le client
  joins either. Mods such as Mod Configuration Menu work for every player.
  How it maps onto the Linux build:
  [reference/NETWORK.md](reference/NETWORK.md).
- **Upstream's Lua environment**: Norbyte's Lua fork, upstream's sandbox,
  its `Ext.Events` library, `Ext.Json`, `Ext.Math`, and `Ext.Types` over
  upstream's own type registry, including its IDE helper generator.
- **Osiris**: the engine's functions, the story's own procedures, databases
  and queries, and `RegisterListener`.
- **Entities**: every component field bg3se describes reads and writes;
  entities and components can be created and removed; `OnCreate`,
  `OnChange` and system-update subscriptions fire; replication, NetIds and
  tracing work.
- **Stats**: every attribute kind reads and writes; `Create`, `Sync` for
  spells, statuses, interrupts and passives; functor execution and editing;
  `LoadStatsFile` and `SetRawAttribute` parse text as the engine's own
  loader does. Spell and equipment sets, treasure tables and categories,
  item combinations and item and name groups read, and can be created or
  updated as upstream allows.
- **Game data**: `Ext.StaticData` (read, write, create), `Ext.Resource`,
  `Ext.Template`, `Ext.Loca`, and `Ext.Level`'s physics queries, AI grid,
  pathfinding and surface actions.
- **UI**: the `Ext.IMGUI` overlay (HDR-aware), `Ext.UI` on the game's own
  Noesis, input events and key injection.
- **Audio**: `Ext.Audio` over the game's Wwise: events, switches, states,
  RTPCs, banks and external sounds.
- **Files**: `Ext.IO` reads through the engine's own file reader and honours
  path overrides.
- **Achievements with mods active**, as bg3se's `EnableAchievements` does.
  Contributed by Igor Tarasyuk; `BG3LE_ACHIEVEMENTS=0` turns it off, and
  [reference/ACHIEVEMENTS-DIAGNOSIS.md](reference/ACHIEVEMENTS-DIAGNOSIS.md)
  has the addresses a maintainer needs when the binary updates.
- **Fixes for the native build itself**: level loads from 65–98 s to about
  1 s ([reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md)),
  an eGPU endgame save from 30 to 71 fps (also usable on its own, see
  [MEMSTEER.md](MEMSTEER.md)), the engine's thread pinning undone except on
  GameThread's core, and mod
  archives corrected for what only Linux trips over (below).
- **Native plugins**: shared libraries dropped in
  `~/.local/share/bg3le/plugins` load with the game, no launch option
  needed, and expose their settings to Lua (and so to MCM). See
  [Native plugins](#native-plugins).
- A Lua debugger server for the [bg3lua](https://github.com/lenonk/bg3lua)
  client (the `client/` submodule).

## Known gaps

- Flatpak Steam is not supported by the installer yet: its sandbox cannot
  see `~/.local/share/bg3le`.
- Some object methods and getters upstream declares as code rather than
  fields are not implemented yet (Noesis hit testing, visual transforms,
  several system methods); none was used by the mods checked. The list:
  [reference/COMPUTED-MEMBERS.md](reference/COMPUTED-MEMBERS.md).
- Multiplayer has been tested over the game's own loopback (single player
  with `Ext._Internal.NetForceRemote(true)`), not yet against a second
  machine or a Windows player running bg3se. Reports welcome.
- Two deliberate differences: a `require` after a mod has finished loading
  still works (upstream errors), and `Ext.Enums` entries are labels rather
  than `EnumValue` objects.

## Building

Needs clang, libc++ (including the static archives), CMake, SDL2 and the
Vulkan loader. protobuf and abseil are built from source by
`tools/fetch-externals.sh` rather than taken from the distribution, because the
packaged builds are compiled against libstdc++ and export `std::__cxx11`
symbols that cannot link into a libc++ library.

They are also built with one libc++ ABI flag the whole library shares. The
game is built against libc++ ABI version 2 and bg3le against ABI 1, and the
one place that shows is `std::variant`, whose index ABI 2 keeps in one byte
where ABI 1 keeps four — enough to change the size of every struct holding a
small variant inline. `_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION` gives
this build the game's layout, and it has to reach protobuf and abseil too,
since one passes a variant across its boundary and both alias it. CMake
refuses to configure against externals built without it; re-run
`tools/fetch-externals.sh` if it says so. See
[reference/LIBCXX-ABI.md](reference/LIBCXX-ABI.md).

    tools/fetch-externals.sh    # Noesis, glm, imgui, lua, rapidjson, Vulkan
    cmake -S . -B build && cmake --build build

Optional checks:

    tools/check-vendor-all.sh      # per-file error counts for vendor/bg3se
    tools/check-vendor-patches.py  # confirms the clang fixes are still applied
    tools/check-prelude.sh         # parses the Lua embedded in lua_host.cpp
    tools/check-views.py           # runs the array and map views against stubs
    cc -o /tmp/mc tools/meta-check.c -ldl && /tmp/mc build/libbg3le.so
                                   # field offsets, the container walks, and
                                   # how much of the surface converts

The first four need no game and no built library (`meta-check` needs the
library but not the game). The Lua prelude is a raw string literal, so a syntax
error in it is a runtime failure rather than a build one — hence
`check-prelude.sh`.

**clang is required, not merely supported.** The vendored bg3se sources need
`-fdeclspec`, `-fms-extensions` and `-fdelayed-template-parsing`, none of which
gcc has; CMake fails the configure step with any other compiler.

**libc++ is required too.** The native game is built against it, so
`std::string` is 24 bytes there as here — libstdc++ would give 32 and silently
shift every field after a string in a component. Everything in the library has
to agree on one standard library, so this applies to bg3le's own sources as
well. It is linked statically, for the same reason Lua is vendored: a shim
loaded inside the Steam runtime container cannot rely on host libraries.

### Release builds

`build/` is built against the host's glibc and asks for whatever symbol
versions it has — `acosf@GLIBC_2.43` on a rolling distribution — so it won't
load on an older one. glibc can't be linked statically into a library loaded
into the game, so a release is built against the oldest glibc it has to run
on: the Steam Runtime 3 ("sniper") SDK's 2.31, which is what the game runs
inside.

    tools/build-sniper.sh   # build-sniper/bg3le/libbg3le.so
    tools/package.sh        # dist/bg3le-<version>-linux-x86_64.zip

`build-sniper.sh` needs Docker, clang, CMake, Ninja and Git. It fetches dependency
sources without building against the host libc++, so no host libc++ installation
is required. It unpacks the sniper SDK image as a sysroot,
builds libc++ (from the LLVM release matching the host clang), abseil and
protobuf against it, then bg3le with the host's clang
(`cmake/sniper-toolchain.cmake`), and fails if the result asks for any glibc
newer than 2.31. Each step is skipped once built; delete `build-sniper/` to
start over. `package.sh` runs it, then packs the library with `install.py`, the
launch wrapper, the console client and the licenses: unpacked, `./install.py`
installs it with no arguments. The version is `git describe`'s, or
`BG3LE_VERSION`.

## Running

Build, then install:

    ./install.py              # or --dry-run to see what it would change

That copies the library, the console client and a launch wrapper into
`~/.local/share/bg3le` and puts the wrapper in front of `%command%` in the
game's Steam launch options, keeping whatever was there. The next launch from
Steam loads bg3le. Steam has to be closed because it rewrites
`localconfig.vdf` from memory when it exits, so if it is running the installer
asks before stopping it. The original is kept beside it as
`localconfig.vdf.bg3le-backup`. `./install.py --uninstall` takes the wrapper
back out and removes `~/.local/share/bg3le`. Running the installer again
updates the library in place.

The wrapper, `installer/bg3le-launch`, rewrites only the game's own argument
in Steam's command, so the preload reaches `bin/bg3` inside the runtime
container and nothing else in the chain. It records the last launch in
`~/.local/share/bg3le/launch.log`; if bg3le does not appear, that says whether
the wrapper found the game.

bg3le's own log is in `~/.local/share/bg3le/logs/`, one
`bg3le-<date>-<time>-<pid>.log` per launch, keeping the last ten. After a
crash, the next launch names the crashed run's log on the main menu. For a
bug report, send that log and `launch.log`. `BG3LE_LOG=<path>` writes
`<path>.<pid>` instead, for every process in the launch chain.

`run-native.sh` is the development harness. It runs the
game inside the Steam runtime container by default, and `SNIPER=0` runs it
straight on the host — the native binary needs only `libssl.so.1.1` and
`libcrypto.so.1.1`, which `compat-libs/` supplies. Running outside the
container matters for debugging: inside it, libraries are recorded under
`/run/host`, which does not resolve from outside the namespace, and `perf`
can symbolize nothing.

Mods live in `~/.local/share/Larian Studios/Baldur's Gate 3/Mods` with the
load order in `PlayerProfiles/Public/modsettings.lsx`, and one thing about
that will waste a day if you do not know it: the game writes a
`ModCrashSanityCheck` directory into the profile while it runs, deletes it on
a clean exit, and **disables every mod when it finds one left behind**. Kill
the game — as any test harness does — and the next run loads no mods. bg3le
removes it at startup, as bg3se does; `BG3LE_KEEP_SANITY_CHECK=1` keeps it,
which is how that was attributed (14 modules with it, 69 without).
[reference/MOD-LOADING.md](reference/MOD-LOADING.md) has the rest, including
that a savegame's module list replaces the load order.

Two things in mod archives work on Windows and break the native build, and
bg3le reads a corrected copy of any archive that has them:

- **Empty files.** An empty `Stats/Generated/Data/*.txt` hangs the first load
  at about 95%. Every empty file is given a single newline.
- **Case.** Linux file names are case-sensitive. Where a mod's own files refer
  to a file it contains with different case (paths in `.lsx`, `.lsf`, stats,
  Lua and JSON, GUI metadata keys, stats `Icon` names), the name and the
  references are made to agree, lowercase where they differ. Spellings the
  engine derives win: GUI textures are always `.DDS`, as every one in the base
  game is, and a `.dds` shows the missing-texture "?".

The player's files are not touched. The copy, in `~/.local/share/bg3le/pakfix/`,
holds only the changed files and a new file list, a few kilobytes; every other
entry points into the original, which the game reads as the copy's second part.
It is rebuilt only when the archive changes, and copies for archives no longer
installed are removed.
Every fix is logged as `pakfix: <archive>: ...`. `BG3LE_PAKFIX=0` turns it
off, and `build/pakfix IN.pak OUT.pak` writes the same copy and lists the
fixes.

### Surviving game updates

Nothing is pinned to an address. Each engine function, global and vtable slot
bg3le uses is described in `src/targets.cpp` by what it is, and found in
whatever build is running (`src/resolve.h`):

- **by symbol**, where the bundled libraries keep theirs (PhysX's
  TempAllocator, for the fast allocator);
- **by its bytes**, searched near where the last build had it and then across
  `.text`, with call targets and rip-relative displacements wildcarded;
- **through the code that uses it**: a global or a vtable by the instruction
  that loads it, a function with identical copies by a unique call site, an
  inlined instruction by the function it sits in.

A target is only used when its match is unique; anything not found is
reported on the debug console and in the log (`WARNING: resolve: X not found
in this build; what uses it is off`) and its feature turns off rather than the
game crashing. Results are cached per GNU build ID in `~/.cache/bg3le/`.
Hotfix v4.76.31.656 moved nearly every function by 64-128 bytes without
changing it; bg3le found all its targets without a code change.

Field offsets and vtable slots are checked the same way, against the engine
code that uses them, before anything is read or called through them: the
translated-string lock, ls::FileReader and the resource banks by the
instructions that access them; the save visitor's typed slots by the LSF type
code each passes; the Wwise manager's slots by the AK::SoundEngine function
each calls; functor and set vtables against the resolved ones. libOsiris'
globals are read off the exported `COsiris::COsiris()`, and its tuple slot off
`COsiris::Event`. A check that fails prints a `WARNING:` line on the console
and leaves only that feature off.

When a patch does rewrite a target, `tools/make-sigs.py OLD_BG3 NEW_BG3
name=kind:0xADDR...` takes the pattern from the old build, grows it until it
is unique, and reports where it lands in the new one; the old build's binaries
can be fetched with the Steam console (`steam -console`, then
`download_depot 1086940 2330359 <manifest>`, the previous manifest ID being in
`steamapps/depotcache`).

The reference capture in `reference/` was taken against game `v4.73.98.727`,
recorded in `reference/version.txt` so a later mismatch is attributable. Its
523 struct layouts still match v4.76.31.656 (`Ext._Internal.SizeAudit()`).

## Native plugins

A plugin is a shared library in `~/.local/share/bg3le/plugins` (or
`$BG3LE_PLUGINS_DIR`). bg3le loads each `.so` there, in name order, on the
game's first `SDL_PollEvent`, once the engine is up, and calls its
`bg3le_plugin_init()`. Nothing goes in `LD_PRELOAD` or the launch options.

The whole API is [include/bg3le_plugin.h](include/bg3le_plugin.h); a plugin
builds against that header alone. The host table it receives offers:

- `describe`, `log` and `warn`: its name and version, and lines in bg3le's
  log (warnings also on the debug console);
- `add_event_handler`: every SDL event the game is about to receive, after
  bg3le's overlay; returning nonzero keeps it from the game;
- `sdl_function`: SDL's own functions by name, which a `dlopen()`ed library
  can't reach with `dlsym(RTLD_NEXT)`;
- `add_setting`: a bool, int or float of the plugin's, readable and writable
  from Lua;
- `add_frame_handler`: a call once per client frame on the client's game
  thread, after the engine's update, with the seconds since the last frame.
  It is newer than v0.3.4, so a plugin checks `host->size` covers it first.

Settings persist in `<plugin>.settings.json` beside the plugin. bg3le
rewrites it, listing every setting, each time the plugin starts and whenever
Lua changes one, and applies the saved value when the plugin registers the
setting, so they hold from its first frame. With the game closed, the file
can be edited by hand instead of through Lua. Lua sees them as

```lua
Ext.Plugins.List()                     -- {Name, Version, File, Loaded, Error}
Ext.Plugins.GetSettings(name)          -- {Id, Type, Value, Min, Max}
Ext.Plugins.Get(name, id)
Ext.Plugins.Set(name, id, value)       -- true, or false and why
```

which is how an SE mod puts them in MCM. A plugin whose init fails is
reported in `Ext.Plugins.List()` with its handlers and settings dropped, but
stays loaded, since it may already have patched code that jumps into it.

[Linux Native Camera Tweaks](https://github.com/lenonk/LinuxNativeCameraTweaks),
a fork of Biiinks78's camera mod, is one, with its settings in MCM.

## Contributing

Patches welcome. Six checks want running before a pull request, all of which
work without the game:

    ./tools/check-symbols.sh        # nothing references an undefined bg3le symbol
    ./tools/check-prelude.sh        # the Lua embedded in lua_host.cpp parses
    python3 tools/check-views.py    # the container views, and JSON escaping
    python3 client/tools/check-output.py   # the console's terminal handling
    python3 client/tools/check-prompt.py   # prompt width against readline's idea of it
    python3 tools/check-installer.py       # launch-option edits and the wrapper

`check-symbols.sh` is the one that matters most: the library links with
undefined symbols allowed, because it has to interpose the engine's own, so a
missing definition of *ours* builds cleanly and then kills the game at the
first call. That has happened three times.

And three that need the game running with bg3le attached:

    ./tools/check-api.sh            # every function upstream registers, both contexts
    ./tools/check-reference.sh      # bg3le against the real extender's output
    ./tools/run-upstream-tests.sh [bg3se checkout]   # bg3se's own Lua tests

`check-api.sh` checks `reference/upstream-api.txt`, which
`tools/upstream-api.py` generates from a bg3se checkout's module
declarations; regenerate it when upstream adds functions.

`reference/*.txt` is output captured from the Script Extender on Windows, and
that replays the same queries here and reports how far apart the answers are.
It does not decide pass or fail — most of what differs is that the install is
not the same one — but it is what found three broken entity calls and a key
in every stat dump that upstream does not have. See
[reference/REFERENCE-DIFFS.md](reference/REFERENCE-DIFFS.md).
`run-upstream-tests.sh` runs the server-side tests from a bg3se checkout's
`LuaScripts/Tests` (default `../bg3se`). Some of their expectations predate
the current game, so it prints each failure's reason rather than a verdict.
Today 11 pass and 6 fail, each for a reason outside bg3le: `TestBaseMod` and
`TestModManager` expect the old base module; `TestStatAttributes` compares
functor objects as JSON, which upstream's own `Stringify` refuses without
`IterateUserdata`; `TestECSComponents` expects `DisplayName.Name` to be a
string, where upstream now returns a TranslatedString; `TestECSFunctions` calls
`GetEntityType`, which upstream has since removed; and `TestGuidResourceLayout`
meets 27 SpellLists (with this mod list) whose engine objects carry a zero
`ResourceUUID` under a real key.

Two conventions worth knowing. Anything located by content is validated
before use — a structure has to agree about something only the real one could
— and a diagnostic that established a layout stays behind an environment
variable rather than being deleted, so the next game patch can re-run it.
`git log` is written to be read; a commit explains why, not what.

## How it hooks

No instruction-length decoder, and nothing is patched in the middle of a
function. Four primitives, in `src/hook.cpp`, `src/preload.cpp` and
`src/detour_interpose.cpp`:

1. PLT/dynamic-symbol interposition, by mangled name
2. vtable-slot patching — one aligned store, and it verifies the slot's
   current contents first, so a shifted binary is refused rather than corrupted
3. call-site patching — rewrites `call rel32` displacements to a nearby
   trampoline, since rel32 cannot reach a shared library from the executable.
   Trampolines share pages: with one page each, the free space in range ran
   out on some address layouts
4. `DetourAttachEx`, for the vendored code that expects Microsoft Detours. It
   records the target and the replacement rather than patching either, and
   one exported forwarder per hooked function lets the dynamic linker do what
   Detours would have done. That is what brings up bg3se's seven Vulkan hooks
   for the ImGui overlay; the two mistakes it took to get right are in
   [reference/IMGUI-ASSESSMENT.md](reference/IMGUI-ASSESSMENT.md)

Symbols come from the native binary's own `.symtab` (102,920 of them) plus
11,214 recovered from embedded `__PRETTY_FUNCTION__` strings attributed to
their enclosing functions via `.eh_frame_hdr`.

## Licence

bg3le's own code is MIT ([LICENSE](LICENSE)). `vendor/bg3se/` remains under
its upstream MIT + Commons Clause terms; `vendor/bg3se/LICENSE` applies to it
and forbids selling the software. The built library contains that code, so a
release carries the Commons Clause as a whole; release packages ship the
licenses of everything compiled into it under `licenses/`.
