# Changelog

## v0.3.12 (2026-10-17)

- A mod's `Info.ModVersion` and `PublishVersion` are a new table on every read, as upstream's are. bg3le handed
  every caller the same table, so a mod that removed the build number from its copy left three parts for everyone
  else, and code formatting the version as four numbers failed. Dependency, conflict and addon entries get the same.
- Assigning a list of tables to an array on a resource object fills in each element, as upstream does. bg3le
  resized the array but left every struct element empty and still reported success: a `CharacterVisual`'s
  `VisualSet.Slots` lost each slot's name, bone and visual, and the model drew without its head.

## v0.3.11 (2026-10-14)

- `Ext.Timer` runs on the game's own clock, as upstream does: the `GameTime` the engine hands each side's update.
  bg3le counted wall-clock time before, so a mod's `WaitFor` delay could run out sooner than on Windows when the
  game's clock fell behind real time. `WaitFor` and `WaitForRealtime` count from the last update and due timers
  fire in upstream's order; `Ext.Timer.GameTime()` and the `Tick` event report the engine's time and delta. If
  either hook can't be found after a game update, the log says so and timers fall back to the old clock.
- Bitfields read as upstream's bitfield objects: a flags field, or an `Ext.Enums` bitfield entry, indexed by label
  is `true` or `false`, and by number is the n-th set label, with `#`, `pairs`, `__Labels`, `__Value` and `&`, `|`,
  `~` as upstream has them. They were plain lists of labels, so `Flags["Label"]` was always `nil`: MCM never saw
  controller mode and did not show its gamepad notice. Plain enums are unchanged.
- The mouse cursor hides again when you press a controller button. The overlay showed the cursor every frame
  whether or not one of its windows was under the mouse; it now changes the cursor only over its own windows.

## v0.3.10 (2026-10-10)

- Each mod's `BootstrapModule.lua` runs before the game loads its stats, and `ModuleLoadStarted` fires, as upstream
  does, so path overrides a mod registers there take effect. Expansion uses one to swap its own stats file for an
  empty one and makes the same changes from Lua; without it both applied, and Mobile and Defensive Blade Flourish
  dealt no damage.
- Mods' `StatsLoaded` handlers run on the first launch after a game update. bg3le found the mod list late without
  its cache, after `StatsLoaded` had fired, so no mod's stat changes applied that session. It now reads the list
  from the game's client, as upstream does.
- Client Lua reads the client's entity world from the main menu on, and server Lua the server's. Mazzle_Lib's client
  scripts failed to load with "the ECS container has not been captured yet".
- Reading a property a UI object does not have raises upstream's "Property does not exist" error again; only
  `:GetProperty()` logs and returns `nil`. This replaces the v0.3.8 change, which made Mazzle_Lib's per-frame UI
  checks write thousands of "has no property named" lines to the log.

## v0.3.9 (2026-10-09)

- Arrays take a numeric string as an index, `"2"` meaning 2, and ignore a write past their end, as upstream does.
  Appearance Edit Enhanced's Restore failed partway and left the hairstyle unrestored, and its Magic Mirror did
  nothing for an origin whose appearance had never been edited.
- String-typed UI properties read their value instead of `nil`: a dice style's `Name`, and the roll screen's text.
  Dice Roulette's blacklist, weights, holidays and "avoid repeats" work again.

## v0.3.8 (2026-10-09)

- Reading a property a Noesis object does not have logs "Object X has no property named 'Y'" and returns `nil`,
  as upstream does, instead of raising an error that ended the calling script. Writing one only logs too.

## v0.3.7 (2026-10-09)

- A Noesis collection reached from Lua, such as an element's `Children` or a view model's list, indexes from 1,
  takes `#` and walks with `ipairs` and `pairs`, as upstream's array proxy does. Indexing one failed with "Object
  UIElementCollection has no property named '1'", which broke Dice Roulette on every roll.

## v0.3.6 (2026-10-08)

- `Ext.Template` reads the right templates after loading a save with a different mod list. That load makes the
  game rebuild every root template, and bg3le kept reading the old ones' freed memory: EasyCheat's Spawn Items
  showed "?" icons, filed items under the wrong filters and failed to spawn them. The templates are now read
  again whenever the client reloads its modules.
- Osiris functions whose outputs are not their last parameters return them. `TemplateIsInPartyInventory`, whose
  count is its third parameter of four, always returned 0, so Wye Fey Potions' proxies stayed at one and locked;
  `TemplateIsInUserInventory` and `StartDialog_Internal` were wrong the same way. The first level load after
  updating reads Osiris' signatures again, once.

## v0.3.5 (2026-10-08)

- A string, number or boolean assigned to a Noesis object property, such as `widget.Tag = "Test1"`, is boxed as the
  game boxes it and reads back as assigned. It failed with "expected a Noesis object" (bg3se issue #603).
- The ImGui overlay keeps one descriptor pool and sampler per device instead of leaking one of each every time the
  window is resized. Binding and unbinding a texture take the overlay's lock, so neither reaches the ImGui backend
  while the overlay is being rebuilt (bg3se issue #597).
- Plugins can register a frame handler with `add_frame_handler`: called once per client frame, on the client's game
  thread, with the seconds since the previous frame. It is a new field at the end of `bg3le_host`, so the ABI stays 1;
  check `host->size` before using it.

## v0.3.4 (2026-10-07)

- `esv::Item::CreateCacheTemplate` works, as upstream's: the item's template is cloned into the server's cache,
  the item switched to the clone, and the switch recorded so clients follow. Trials Ascension uses it for every
  reward, merchant and custom loot item, and failed on each one. A local template, or anything bg3le cannot verify
  before writing, returns `nil` with a log line instead.
- A flag of a component's flags field reads and writes as a boolean, as upstream's `P_BITMASK` makes it:
  `ServerItem.IsLadder`, `.CanBePickedUp` and the rest. Objects already did; components raised "has no field".
- A fixed array of enums takes labels for each element, so `Resistances.Resistances` can be assigned as lists
  of flag names. It failed with "number expected, got table".

## v0.3.3 (2026-10-07)

- `SetWorldTranslate`, `SetWorldRotate` and `SetWorldScale` work on `MoveableObject` and every render class
  upstream derives from it (`Visual`, `Effect`, `LightComponent` and the rest).
- Assigning a table to a map entry whose value is a struct replaces it with a value filled from the table, as
  upstream does, instead of failing. Applies to component and object maps.
- `Ext.IMGUI.NewWindow` and `Ext.IMGUI.LoadFont` take a number as the name, using its text, as upstream does.
- Osiris calls accept an empty string argument. They were refused as "could not be interned".

## v0.3.2 (2026-10-07)

- `StatsLoaded` now fires where upstream fires it: in the client, from the engine's stats load at startup, after the
  client's mods load into a fresh Lua state. It used to fire in both contexts as each session started, after
  character creation was already built. Compatibility Framework's subclasses now appear in character creation (14
  Cleric subclasses instead of 8), shared `StatsLoaded` handlers no longer run twice, and starting a session is
  faster: bg3le's mod loading there went from 4.75 s to about 1 s with 5eSpells, Expansion and UAWarCaster.
- `Ext.Stats.Sync` during `StatsLoaded` no longer warns that a prototype manager is not located: the engine builds
  every prototype from the stats right after, as upstream relies on.
- The stats are found again after every module load, and a search that failed during the load no longer hides them
  from `StatsLoaded` for ten seconds.
- ImGui widgets are userdata, as upstream's are; mods tell a widget from a list of widgets by `type()`. Fixes
  Trials Ascension's GUI errors.
- Pointers print as MSVC's `%p` does (16 uppercase hex digits, no `0x`). Trials Ascension seeds its random numbers
  from one, and its scripts failed to load.

## v0.3.1 (2026-10-07)

- Fixed a crash loading any save when a mod writes a stat condition before the load, at the main menu. A condition
  longer than 15 characters kept its text in a buffer from the wrong allocator, and the engine frees every
  condition's buffer when it resets the stats for a load. The buffer now comes from the engine's own heap.
- The release zip carries a `VERSION` file, which `install.py` copies to `~/.local/share/bg3le/version`, so mod
  managers can tell the installed release from the newest one. Releases are also published on GitHub, where
  Amethyst Mod Manager's bg3le wizard finds them.

## v0.3.0 (2026-10-06)

- MCM and other ImGui windows are smooth again on Linux. Input the overlay keeps from the game was hidden by
  telling the game its event queue was empty, as upstream does; Linux delivers every pointer motion (Windows
  coalesces them), so with the cursor over a window the game took one event a frame, the rest backed up, and the
  overlay skipped frames. The kept events are now skipped over instead.
- Fixed a crash in the NVIDIA driver opening MCM (`ImGui_ImplVulkan_AddTexture`): bg3le now tracks the game's
  image views and refuses to draw a texture whose view is not live, logging which one, instead of handing the driver
  a bad handle.
- MCM icons no longer go missing: ImGui images hold a reference to their texture through the engine's
  `TextureManager`, as upstream's do, so the engine keeps it loaded, and an image whose view is replaced anyway binds
  the new one. A dead view is logged as destroyed or never seen created, to tell which. ImGui images of a texture
  resource (not an icon) load too.
- Fixed a crash loading a save when a mod syncs a spell during load (`Ext.Stats.Sync`; Expansion). bg3le indexed the
  engine's prototypes once, sometimes at the main menu, and the engine rebuilds them when a save loads, so a sync
  could hand the engine a freed prototype. Every lookup is now checked against the engine's live map, and the index
  read again when the engine has rebuilt it.
- `Ext.Stats.GetCachedSpell`, `GetCachedStatus`, `GetCachedInterrupt` and `GetCachedPassive` no longer list every
  prototype on each call.
- Enum fields on objects and components are upstream's `EnumValue`s (`Label`, `Value`, `EnumName`), equal to and
  ordered against their label or number, their label as a table key, and their label in JSON and `Serialize`.
  Progression Preview read `DiceValue.Label`. Two small changes to the Lua fork make the comparisons and keys work;
  see `external/lua/README.bg3le`.
- `getmetatable` on an entity gives `"EntityProxy"`, as upstream's does; FocusCore's character test (EasyCheat)
  depended on it, so EasyCheat's party, camp and unrecruited lists were empty and teleporting spammed errors.
- The server now gets `GameStateChanged` too (LoadSession, Sync, Running, Save, ...), and `Ext.Utils.GetGameState`
  reports the server's state there. Bag of Holding Reforged builds its state on Running.
- Every context's `StatsLoaded` comes before any `SessionLoaded`, as upstream's do. UAWarCaster creates its statuses
  in a client `StatsLoaded` that the server checks for.
- `Ext.Entity.HandleToUuid` takes an entity (EasyCheat via AahzLib); lists with holes are written as upstream does,
  skipping the hole (Subclass Compatibility Framework's spell lists); upstream's legacy field names (`field_1` for
  `Controller` and the rest) resolve again (Auto-Sorting Hotbar); the `Tick` event's time carries `Ticks`
  (Mazzle's EZ-Documentation).
- Loading is faster: a session load with this setup went from about 60 s to 30 s. `Ext.Stats.Create` no longer
  rebuilds the stats index, stat reads skip most of their fault-tolerant memory reads, functor attributes read
  lazily, and class field tables are built once.
- Corrected mod archives are now a few kilobytes on any filesystem: the copy holds only the changed files and its
  file list, and the original is read as its second part. Copies of the previous format and of archives no longer
  installed are removed. A bare file name or a relative Lua path outside the mod's own `ScriptExtender/Lua` is no
  longer taken for a reference (Mazzle's EZ-Documentation keeps a `config.json` under `Ext.IO`).

## v0.2.6 (2026-10-06)

- Mod archives are corrected for two things the native build trips over and Windows does not, by reading a fixed
  copy in place of the original (`~/.local/share/bg3le/pakfix/`; the player's files are not touched):
  - Empty files are given a single newline. An empty stats `.txt` hung the first load at about 95% (Expansion,
    and likely Druid Wild Shape Overhaul); Cilraaz found an empty `.khn` crashing Goon's Barbarian Overhaul
    (not yet retested with the newline).
  - Where a mod's own files refer to a file it contains with different case, the names and references are made
    to agree, lowercase where they differ. GUI textures become `.DDS`, as the engine requires: a `.dds` icon
    showed the missing-texture "?" (Expansion, 5e Spells, Mind Weaver, Clerics and others).

  Every fix is logged as `pakfix: <archive>: ...`; `BG3LE_PAKFIX=0` turns it off.

## v0.2.5 (2026-10-05)

- Fixed a crash at startup on the Steam Deck in the game's Wwise audio engine (`JobManager_dispatchMultiple`, about
  20 seconds in). bg3le widened every thread's CPU affinity, including the three Wwise pins to its own cores; freed,
  its event thread could dispatch before the job manager's queues existed. Wwise's threads are now left as it sets
  them; the engine's own threads are still widened. `BG3LE_AFFINITY=off` was the workaround.
- Mods packed with zstd (newer LSLib builds) now load: their `meta.lsx` and scripts could not be read, so their
  scripts never ran (AutomaticMagicalSecretsExtender). Zstandard's decompressor is vendored next to LZ4.
- On a first launch, or the first after a game update, client mods now wait for bg3le to find the game's module
  list instead of loading without it. Mods that call `Ext.Mod.GetMod` as they load (MCM) failed with "0 loaded,
  0 available".

## v0.2.4 (2026-10-04)

- Fixed components of an entity created this tick reading as nil: an item just made with `Osi.CreateAt` keeps its
  components in the engine's immediate cache until they are committed, and bg3le read only committed storage.
  Upstream falls back to that cache and then the command buffer, and now so does bg3le. Armory's item preview
  failed on `.Data`, and removing a transmog lost the item: the transmogged piece was deleted and the restored one
  was left at the world origin.
- `Ext.Types.Unserialize` writes nested structs, arrays and maps in place, as upstream's does, instead of refusing
  them as "not writable". Arrays of structs can be resized, so assigning or appending to one (`Use.Boosts`,
  `ServerBaseWeapon.DamageList`, `Weapon.Rolls`) works. Armory's transmog copies whole components this way.
- `widget:Destroy()` removes an ImGui widget from its parent's `Children`, as upstream's does. A destroyed child
  stayed listed, and a mod walking `Children` again got "this widget no longer exists" (Armory's equipment picker).

## v0.2.3 (2026-10-04)

- Fixed a crash at startup on the Steam Deck with lsfg-vk (Decky's Lossless Scaling frame generation), even with
  the plugin switched off. lsfg-vk makes its own Vulkan device while the game creates its swapchain, and the ImGui
  overlay took it for the game's. The overlay now stays on the game's device. `BG3LE_IMGUI=0` was the workaround.
- **Logs moved out of `/tmp`.** bg3le's log is now `~/.local/share/bg3le/logs/bg3le-<date>-<time>-<pid>.log`, one
  per launch, keeping the last ten, so it survives a reboot and is easy to find from the Deck's Desktop Mode. Only the
  game gets a log; the launch chain's helper processes no longer leave files. `BG3LE_LOG` still overrides it.
- After a crash, the next launch names the crashed run's log on the main menu.
- The ImGui overlay's Vulkan setup is logged step by step, so a crash there shows which step it was.
- A Lua context is now entered only after its lock is held. A tick waiting on a save load's reset could otherwise
  have run on the Lua state the reset had just closed.

## v0.2.1 (2026-10-03)

- **Multiplayer.** `Ext.Net` and synced `Ext.Vars` now cross between machines over the game's connection, in
  bg3se's own protocol: a bg3le host serves clients running bg3se on Windows and bg3le alike, and a bg3le client
  joins either kind of host. Before this they never left the machine, so on a bg3le host other players' Mod
  Configuration Menu could not reach the server. Message ID 400, the `" Extender_0"` connect tag and the hello
  handshake are upstream's; see reference/NETWORK.md. Tested over the game's own loopback, not yet with a second
  machine: reports welcome.
- `Ext.Net` follows upstream's API exactly: the server context has `BroadcastMessage`, `PostMessageToClient`,
  `PostMessageToUser` and `PlayerHasExtender`, the client `PostMessageToServer`, both `IsHost` and `Version`; net
  channels are upstream's `NetChannel`, with requests answered by reply ID.
- Messages carry real user IDs (peer << 16 | slot, so 65537 for the host's player) instead of 1, matching a
  character's `UserID`.
- A host's Lua reset also resets clients on other machines, as upstream's does.
- Fixed `Ext.IMGUI` InputText's `Text`, which read as nothing and could not be set: upstream declares it as a getter
  and setter, which bg3le's field tables leave out. Forms that check their fields, such as Armory's preset editor,
  said every field was empty.
- Added `Ext.ClientNet`, `ClientInput`, `ClientTemplate`, `ClientLevel`, `ClientAudio`, `ClientIMGUI` and `ClientUI`
  in the client context, and kept `ServerNet`, `ServerLevel` and `ServerTemplate` to the server's, as upstream
  registers them.
- `Ext.Mod.GetMod` no longer logs a line for an argument that is not a UUID (upstream returns nil quietly; Armory's
  item report probes every mod by name), and a stat's `ModId` and `OriginalModId` are `""` rather than nil when no
  mod is known, as upstream's are.
- Fixed nested objects in `Ext.StaticData` resources whose type name is also a component's, such as `Origin`:
  they were typed from the component, so `Origin.DisplayName` had no `Get()` (Armory's preset activation stopped
  there).
- Added `Ext.System` (upstream's SystemMap: `Ext.System.ClientVisual` and the rest, as views of the live systems),
  `Ext.CoreLib(name)`, `ServerCharacter:GetStatus`, the deprecated `ServerCharacter.Character` and
  `ServerItem.Item`, templates' `TemplateStorageType`, and ImGui's `DragDropType` and `ParentElement`: members
  upstream declares as code rather than fields, which bg3le's tables leave out. What is still missing is listed in
  reference/COMPUTED-MEMBERS.md.
- tools/check-surface.sh checks the whole captured `Ext` surface, not only its functions.

## v0.2.0 (2026-10-02)

- **Native plugins.** bg3le loads shared libraries from `~/.local/share/bg3le/plugins` (or `$BG3LE_PLUGINS_DIR`)
  with the game, with no `LD_PRELOAD` or launch option change. A plugin gets logging, SDL event handlers that can
  keep events from the game, SDL's own functions, and settings.
- **Plugin settings in Lua.** `Ext.Plugins.List`, `GetSettings`, `Get` and `Set`, so a Script Extender mod can show
  a plugin's settings in the Mod Configuration Menu.
- Settings persist in `<plugin>.settings.json` beside the plugin, applied from its first frame. The file lists every
  setting and can be edited by hand with the game closed.
- The release package includes `include/bg3le_plugin.h`, the whole plugin API. See "Native plugins" in the README.
- First plugin: [Linux Native Camera Tweaks](https://github.com/lenonk/LinuxNativeCameraTweaks).

## v0.1.1

- Fixed Mod Configuration Menu 1.41 failing to load ("attempt to call a nil value (method 'GetSettingValue')").
  Mod script chunks are now named `<Directory>/<file>` as upstream names them, which MCM uses to tell which mod
  is calling.
- Releases are a zip, which Nexus Mods accepts, and keep `install.py` executable when unzipped.

## v0.1.0

- First release: bg3se's public `Ext` API on Baldur's Gate 3's native Linux build, in both contexts, with Script
  Extender mods loading from their paks, plus an installer.
