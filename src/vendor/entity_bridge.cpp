// Bridges bg3le's captured ECS pointers to bg3se's ECS implementation.
//
// bg3le supplies what only it can: the component type indices, read from the
// named TypeId statics in the symbol table, and the EntityStorageContainer,
// captured from a hooked GetEntityStorage call. bg3se supplies what it already
// reverse engineered: the layout of the container, of EntityStorageData, and
// the page-map walk that turns an EntityHandle plus a component index into a
// pointer.
//
// Its layout has been checked against this build twice over -- the component
// mask is 0x110 bytes, matching ComponentMapSize = 0x880, and the container
// begins with Array<EntityStorageData*>, which is where the disassembled
// lookup reads its buffer pointer from.
//
// The bg3se code this calls is by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); only the bridge is ours.

// Utils.h first: upstream relies on its own translation units pulling the
// logging macros in before the Lua headers.
#include <cstring>
#include "../mem.h"
#include <Extender/Shared/Utils.h>

#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/Components/All.h>

#include "../log.h"
#include "../hook.h"
#include "../targets.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <pthread.h>
#include <vector>

extern "C" bool bg3le_game_allocator_ready();

namespace bg3le {

// Returns the component of the given type for an entity, or nullptr if the
// entity has no such component. componentSize is what bg3se uses to stride the
// component page, so it has to match the engine's real component size.
extern "C" void* bg3le_entity_component(void* container, std::uint64_t handle,
                                        std::uint16_t componentIndex,
                                        std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto storageIndex = storages->GetEntityStorageIndex(entity);
    if (!storageIndex.has_value()) return nullptr;

    auto* storage = storages->GetEntityStorage(*storageIndex);
    if (storage == nullptr) return nullptr;

    return storage->GetComponent(entity,
                                 bg3se::ecs::ComponentTypeIndex(componentIndex),
                                 componentSize);
}

// End-to-end proof: reads Health off an entity. Keeps every bg3se type inside
// this translation unit, so the rest of bg3le needs none of its headers.
//
// The component size matters: bg3se strides the component page with it, so it
// has to match the engine's. Taking it from bg3se's own struct is exactly the
// earlier finding that its component definitions lay out correctly here,
// because they are plain structs the compiler arranges rather than hardcoded
// offsets.
extern "C" bool bg3le_entity_health(void* container, std::uint64_t handle,
                                    std::uint16_t componentIndex,
                                    std::int32_t* hp, std::int32_t* maxHp) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    *hp = component->Hp;
    *maxHp = component->MaxHp;
    return true;
}

// Reports each step of the walk separately, because a single null cannot
// distinguish "the entity genuinely has no such component" from "the storage
// lookup failed". Returns the storage index (-1 if none), the storage pointer,
// and the component pointer.
extern "C" void bg3le_entity_probe(void* container, std::uint64_t handle,
                                   std::uint16_t componentIndex,
                                   std::int32_t* storageIndex,
                                   void** storage, void** component) {
    *storageIndex = -1;
    *storage = nullptr;
    *component = nullptr;
    if (container == nullptr) return;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return;
    *storageIndex = *index;

    auto* data = storages->GetEntityStorage(*index);
    if (data == nullptr) return;
    *storage = data;

    *component = data->GetComponent(entity,
                                    bg3se::ecs::ComponentTypeIndex(componentIndex),
                                    sizeof(bg3se::HealthComponent));
}

// Walks the container's storages looking for one whose component set includes
// the given type, and returns an entity out of it. Needed because the handle
// the thunk captures is whatever the engine touched last, which is usually
// something with no Health at all.
extern "C" std::uint64_t bg3le_find_entity_with(void* container,
                                                std::uint16_t componentIndex,
                                                std::uint32_t* storagesSeen) {
    *storagesSeen = 0;
    if (container == nullptr) return 0;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        ++*storagesSeen;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        // First live entity in this storage. HashMap keeps its keys in an
        // array, so they can be read without walking buckets.
        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle != bg3se::EntityHandle::NullHandle) return key.Handle;
        }
    }
    return 0;
}

// Every live entity, or every entity carrying one component.
//
// The walk is the one bg3le already uses to find a single entity, run to
// completion: each storage's InstanceToPageMap holds its entities as keys
// in an array, so they come out without touching buckets or page masks.
// Filtering is per storage rather than per entity, because a storage is an
// archetype -- every entity in it has exactly the same component set, so
// one lookup decides for all of them.
//
// Returns how many exist, which may exceed `max`; the caller sizes its
// buffer from a first call and reads on a second, and a count that grew in
// between is truncated rather than overrunning.
extern "C" std::size_t bg3le_entities_collect(void* container,
                                              int componentIndex,
                                              std::uint64_t* out,
                                              std::size_t max) {
    if (container == nullptr) return 0;

    auto* storages =
        reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const bool filtered = componentIndex >= 0;
    const auto type =
        bg3se::ecs::ComponentTypeIndex((std::uint16_t)componentIndex);

    std::size_t found = 0;
    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        // ComponentsInClass, which is what bg3se's own HasComponent
        // tests. ComponentTypeToIndex is a slot map and matched only 19
        // entities for Health, none of which read one back.
        if (filtered && !storage->HasComponent(type)) continue;

        // InstanceToPageMap's keys, which are the handles the rest of
        // bg3le already resolves -- the same ones find_entity_with hands
        // to the component readers. The handle pages looked like the
        // authoritative list and are not: their entries came back with a
        // different high half and GetEntityStorageIndex rejected every
        // one.
        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle == bg3se::EntityHandle::NullHandle) continue;
            if (out != nullptr && found < max) out[found] = key.Handle;
            ++found;
        }
    }
    return found;
}

// Finds the component of a given type on whichever entity carries it, by
// scanning the container's storages. Used for the singleton components, which
// bg3se normally reaches through EntityWorld -- and the world is the one thing
// with no symbol and no capture point, so this route avoids needing it.
static void* find_any_component(void* container, std::uint16_t componentIndex,
                                std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle == bg3se::EntityHandle::NullHandle) continue;
            auto* component = storage->GetComponent(key, type, componentSize);
            if (component != nullptr) return component;
        }
    }
    return nullptr;
}

// UUID string -> EntityHandle, through ls::uuid::ToHandleMappingComponent.
// Returns 0 if the mapping component cannot be found or the UUID is unknown.
extern "C" std::uint64_t bg3le_uuid_to_handle(void* container,
                                              std::uint16_t mappingIndex,
                                              char const* uuid) {
    auto* mapping = static_cast<bg3se::UuidToHandleMappingComponent*>(
        find_any_component(container, mappingIndex,
                           sizeof(bg3se::UuidToHandleMappingComponent)));
    if (mapping == nullptr || uuid == nullptr) return 0;

    const auto guid = bg3se::Guid::ParseGuidString(uuid);
    if (!guid.has_value()) return 0;

    auto* handle = mapping->Mappings.try_get(*guid);
    return handle != nullptr ? handle->Handle : 0;
}

// Marks a component dirty so the change is picked up and replicated.
//
// bg3se routes this through EntityWorld::MarkComponentAsChanged, but that
// function only touches the storage and the container's UsedFrameDataStorages
// bitset -- and EntityWorld::Storage *is* the container we captured. So it
// needs no world either.
//
// This is MarkComponentAsChanged on its own, which makes the server act on
// the new value. Getting it to the client as well takes the replication flags
// too -- see bg3le_replicate_component below.
extern "C" bool bg3le_mark_component_changed(void* container,
                                             std::uint64_t handle,
                                             std::uint16_t componentIndex) {
    if (container == nullptr) return false;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return false;

    auto* storage = storages->GetEntityStorage(*index);
    if (storage == nullptr) return false;

    if (!storage->MarkComponentAsChanged(entity, type)) return false;

    if (!storages->UsedFrameDataStorages[storage->StorageIndex]) {
        storages->UsedFrameDataStorages.Set(storage->StorageIndex);
    }
    return true;
}

// Health is read and written through typed accessors rather than raw offsets,
// so the field layout comes from bg3se's struct rather than being restated.
extern "C" bool bg3le_set_health(void* container, std::uint64_t handle,
                                 std::uint16_t componentIndex,
                                 std::int32_t hp, bool setMax) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    component->Hp = hp;
    if (setMax) component->MaxHp = hp;
    return true;
}


// Recovers the EntityWorld from the captured container.
//
// EntityWorld has no symbol and no capture point, but it turns out not to need
// one: EntityStorageContainer::ComponentRegistry points at EntityWorld's
// ComponentRegistry_, which is a by-value member sitting immediately after
// Replication. So the world is that registry pointer less the member's offset.
//
// Two independent pointers confirm it: world->Storage has to come back out as
// the container we started from, and &world->Queries has to equal the
// container's own Queries pointer. bg3le_world_probe reports both, so a layout
// drift shows up as a failed check rather than as a wild pointer.
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
constexpr std::size_t kComponentRegistryOffset =
    offsetof(bg3se::ecs::EntityWorld, ComponentRegistry_);
#pragma clang diagnostic pop

// Replication is the first member, so the registry is one pointer in.
static_assert(kComponentRegistryOffset == sizeof(void*),
              "EntityWorld::Replication is expected to precede ComponentRegistry_");

bg3se::ecs::EntityWorld* world_from_container(void* container) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    auto* registry = storages->ComponentRegistry;
    if (registry == nullptr) return nullptr;

    return reinterpret_cast<bg3se::ecs::EntityWorld*>(
        reinterpret_cast<char*>(registry) - kComponentRegistryOffset);
}

}  // namespace

// The EntityWorld a captured container belongs to, or null. What an EntityRef
// written from Lua is paired with when it has no world of its own.
extern "C" void* bg3le_entity_world(void* container) {
    return world_from_container(container);
}

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
constexpr std::size_t kWorldStorageOffset = offsetof(bg3se::ecs::EntityWorld, Storage);
constexpr std::size_t kStorageRegistryOffset =
    offsetof(bg3se::ecs::EntityStorageContainer, ComponentRegistry);
#pragma clang diagnostic pop

// `world`'s own container, if it is an EntityWorld: its Storage, whose registry
// leads back to it. Read without trusting the pointer.
void* container_of_world(std::uintptr_t world) {
    std::uintptr_t storage = 0;
    std::uintptr_t registry = 0;
    if (world < 0x10000 || (world & 7) != 0
        || !bg3le::safe_read(reinterpret_cast<void const*>(world + kWorldStorageOffset), &storage, sizeof(storage))
        || storage < 0x10000
        || !bg3le::safe_read(reinterpret_cast<void const*>(storage + kStorageRegistryOffset), &registry,
                             sizeof(registry))
        || registry != world + kComponentRegistryOffset) {
        return nullptr;
    }
    return reinterpret_cast<void*>(storage);
}

// Where one of ecl::EoCClient and esv::EoCServer keeps its EntityWorld: bg3se's
// offset first, then a window around it, searched for the pointer that
// round-trips as a world.
struct WorldField {
    char const* Name;
    std::uintptr_t (*Global)();
    std::size_t Expected, From, To;
    std::atomic<long> Offset{-1};
    // The last world and storage that checked out, so the check runs once per change.
    std::atomic<std::uintptr_t> World{0}, Storage{0};
    std::atomic<std::int64_t> NextSearch{0};
};

WorldField g_client_world_field{"ecl::EoCClient", &bg3le::target::EoCClient, 0x1a0, 0x100, 0x260};
WorldField g_server_world_field{"esv::EoCServer", &bg3le::target::EoCServer, 0x288, 0x200, 0x300};

std::uintptr_t plain_load(std::uintptr_t at) { return *reinterpret_cast<std::uintptr_t const*>(at); }

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Called on every component read: once the field is known, three plain loads
// (the global is in the image, its object lives as long as the game), and the
// round-trip check only when the world or its storage changes.
void* world_container_of(WorldField& field) {
    if (field.Global() == 0) return nullptr;
    const std::uintptr_t eoc = plain_load(bg3le::load_bias() + field.Global());
    if (eoc == 0) return nullptr;

    const long known = field.Offset.load(std::memory_order_acquire);
    if (known >= 0) {
        const std::uintptr_t world = plain_load(eoc + (std::size_t)known);
        if (world == 0) return nullptr;
        const std::uintptr_t storage = plain_load(world + kWorldStorageOffset);
        if (world == field.World.load(std::memory_order_acquire)
            && storage == field.Storage.load(std::memory_order_acquire)) {
            return reinterpret_cast<void*>(storage);
        }
        void* checked = container_of_world(world);
        if (checked == nullptr) return nullptr;
        field.Storage.store(reinterpret_cast<std::uintptr_t>(checked), std::memory_order_release);
        field.World.store(world, std::memory_order_release);
        return checked;
    }

    // Not found yet: the object may be up before its world, so search again, at
    // most once a second.
    const std::int64_t now = now_ms();
    if (now < field.NextSearch.load(std::memory_order_relaxed)) return nullptr;
    field.NextSearch.store(now + 1000, std::memory_order_relaxed);
    auto at = [&](std::size_t offset) -> void* {
        std::uintptr_t world = 0;
        if (!bg3le::safe_read(reinterpret_cast<void const*>(eoc + offset), &world, sizeof(world))) return nullptr;
        return container_of_world(world);
    };
    void* container = at(field.Expected);
    std::size_t found = field.Expected;
    for (std::size_t offset = field.From; container == nullptr && offset < field.To; offset += 8) {
        container = at(offset);
        found = offset;
    }
    if (container != nullptr) {
        field.Offset.store((long)found, std::memory_order_release);
        bg3le::logf("ecs: %s keeps its EntityWorld at +%#zx%s", field.Name, found,
                    found == field.Expected ? "" : " (not where bg3se has it)");
    }
    return container;
}

}  // namespace

// The calling context's own world, as upstream's GetEntitySystemHelpers()
// ->GetEntityWorld() gives it: the client's for client Lua, the server's for
// server Lua. Null while that world does not exist.
extern "C" void* bg3le_context_container(bool client) {
    return world_container_of(client ? g_client_world_field : g_server_world_field);
}

// Reports everything needed to judge whether the recovered world is real: the
// pointer itself, its Replication buffers, the size of the replication pool
// array -- which should be close to the number of ReplicatedTypeContext
// indices in the symbol table -- and whether Storage and Queries round-trip.
extern "C" void bg3le_world_probe(void* container, void** world,
                                  void** replication, std::int32_t* poolCount,
                                  bool* storageMatches, bool* queriesMatch) {
    *world = nullptr;
    *replication = nullptr;
    *poolCount = -1;
    *storageMatches = false;
    *queriesMatch = false;

    auto* w = world_from_container(container);
    if (w == nullptr) return;
    *world = w;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    *storageMatches = (w->Storage == storages);
    *queriesMatch = (&w->Queries == storages->Queries);

    if (w->Replication == nullptr) return;
    *replication = w->Replication;
    *poolCount = (std::int32_t)w->Replication->ComponentPools.Size();
}

// A one-frame component, which does not live in the entity page at all.
//
// These are the transient event components -- a request or a notification that
// exists for a single tick. The engine keeps them in a per-storage pool keyed
// by entity rather than in the page, so reading one through the page path
// returns whatever happens to be at that offset. That is what the 17 size
// mismatches SizeAudit reported were: not a wrong struct, a wrong storage
// mechanism. Same failure shape as the proxy components, silent in the same
// way.
extern "C" void* bg3le_entity_one_frame_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t componentIndex) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    auto* storage = storages->GetEntityStorage(entity);
    if (storage == nullptr) return nullptr;

    return storage->GetOneFrameComponent(
        entity, bg3se::ecs::ComponentTypeIndex(componentIndex));
}

// The engine's own recorded size for a component.
//
// Worth having because the size is not cosmetic: GetComponent returns
// buf + componentSize * entryIndex, so if bg3se's struct is the wrong size
// every read of that component is misaligned -- for every entity except
// whichever one happens to sit at index 0, which is exactly the sort of bug
// that looks like correct code failing intermittently.
//
// The engine keeps the sizes per storage, indexed by the component's slot
// within that storage, so this searches the storages for one that carries the
// component. Returns -1 if none does, which only means no live entity has it.
extern "C" int bg3le_component_engine_size(void* container,
                                           std::uint16_t componentIndex) {
    if (container == nullptr) return -1;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr || storage->ComponentSizes == nullptr) continue;
        auto const* slot = storage->ComponentTypeToIndex.try_get(type);
        if (slot == nullptr) continue;
        return (int)storage->ComponentSizes[*slot];
    }
    return -1;
}

// Whether this container belongs to the server world.
//
// The client and server each have an EntityWorld, and replication buffers are
// only allocated on the server's -- which is how bg3se's ReplicateComponent
// decides it is being called from the wrong side. So the same test identifies
// which of the two captured containers is which, without depending on the
// order the engine happened to touch them in.
extern "C" bool bg3le_container_is_server(void* container) {
    auto* world = world_from_container(container);
    return world != nullptr && world->Replication != nullptr;
}

// bg3se's ReplicateComponent, reached through the recovered world.
//
// Marking a component changed makes the server act on the new value;
// replication is what sends it to the client, and without it anything reading
// the replicated copy -- the UI included -- never sees it. Whole-component
// replication is qword 0 with every flag set, as EntityProxyMetatable::
// Replicate passes.
//
// Status: 0 ok, 1 no container, 2 no world, 3 no replication buffers (a
// client-side world; only the server replicates), 4 replication type index out
// of range, 5 could not add the entity to the pool, 6 the engine allocator is
// not installed and this call would have had to allocate.
extern "C" bool bg3le_game_allocator_ready();

extern "C" std::int32_t bg3le_replicate_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t replicationTypeIndex,
                                                  std::uint32_t qword,
                                                  std::uint64_t flags) {
    if (container == nullptr) return 1;

    auto* world = world_from_container(container);
    if (world == nullptr) return 2;
    if (world->Replication == nullptr) return 3;

    auto& pools = world->Replication->ComponentPools;
    if (replicationTypeIndex >= pools.Size()) return 4;

    const auto entity = bg3se::EntityHandle(handle);
    auto& pool = pools[replicationTypeIndex];

    // add_key and EnsureSize both grow engine-owned memory, so they need the
    // engine's allocator to be installed. Refuse rather than call a null
    // Alloc, and refuse rather than mix a foreign heap into an engine
    // container.
    auto* syncFlags = pool.try_get(entity);
    if (syncFlags == nullptr) {
        if (!bg3le_game_allocator_ready()) return 6;
        syncFlags = pool.add_key(entity);
    }
    if (syncFlags == nullptr) return 5;

    if (syncFlags->NumQwords() <= qword && !bg3le_game_allocator_ready()) {
        return 6;
    }
    syncFlags->EnsureSize((qword + 1) * 64);
    const bool changed = (syncFlags->GetBuf()[qword] & flags) != flags;
    syncFlags->GetBuf()[qword] |= flags;
    if (changed) world->Replication->Dirty = true;

    return 0;
}

namespace {

bg3se::ecs::EntityStorageData* storage_of(void* container,
                                          bg3se::EntityHandle entity) {
    if (container == nullptr) return nullptr;
    auto* storages =
        reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto index = storages->GetEntityStorageIndex(entity);
    return index ? storages->GetEntityStorage(*index) : nullptr;
}

}  // namespace

// The entity methods below follow upstream's EntityProxyMetatable
// (Lua/Shared/Proxies/LuaEntityProxy.inl) over the captured container.

// IsAlive: whether the entity has a storage.
// Upstream's IsAliveEntity: the handle's slot in its thread's generator
// state still carries its index and salt. A null handle is alive, as there.
extern "C" bool bg3le_entity_alive(void* container, std::uint64_t handle) {
    const bg3se::EntityHandle h(handle);
    if (!h) return true;
    auto* world = world_from_container(container);
    if (world == nullptr || world->HandleGenerator == nullptr) return false;
    auto& generator = *world->HandleGenerator;
    if (h.GetThreadIndex() >= std::size(generator.ThreadStates)) return false;
    auto& state = generator.ThreadStates[h.GetThreadIndex()];
    if (h.GetIndex() >= state.Entries.size()) return false;
    auto const& entry = state.Entries[h.GetIndex()];
    return entry.Index == h.GetIndex() && entry.Salt == h.GetSalt();
}

// GetAllComponentNames: the storage's component types, then the one-frame
// pools that hold this entity. Returns how many there are; writes up to max.
extern "C" std::size_t bg3le_entity_component_types(void* container,
                                                    std::uint64_t handle,
                                                    std::uint16_t* out,
                                                    std::size_t max) {
    const auto entity = bg3se::EntityHandle(handle);
    auto* storage = storage_of(container, entity);
    if (storage == nullptr) return 0;

    std::size_t n = 0;
    auto put = [&](auto type) {
        if (n < max) out[n] = (std::uint16_t)type;
        ++n;
    };
    for (auto componentIdx : storage->ComponentTypeToIndex.keys()) {
        put(componentIdx);
    }
    if (storage->HasOneFrameComponents) {
        for (auto it : storage->OneFrameComponents) {
            if (it->Value().find(entity) != it->Value().end()) put(it->Key());
        }
    }
    return n;
}

// WasChanged, as EntityWorld::WasComponentChanged.
extern "C" bool bg3le_entity_was_changed(void* container, std::uint64_t handle,
                                         std::uint16_t componentIndex) {
    const auto entity = bg3se::EntityHandle(handle);
    auto* storage = storage_of(container, entity);
    if (storage == nullptr) return false;
    auto* storages =
        reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    return storages->UsedFrameDataStorages[storage->StorageIndex]
           && storage->WasComponentChanged(
               entity, bg3se::ecs::ComponentTypeIndex(componentIndex));
}

// GetChangedComponents: the component types changed this frame.
extern "C" std::size_t bg3le_entity_changed_types(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t* out,
                                                  std::size_t max) {
    if (container == nullptr) return 0;
    auto* storages =
        reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);
    const auto storageIndex = storages->GetEntityStorageIndex(entity);
    if (!storageIndex || !storages->IsEntityStorageDirty(*storageIndex)) {
        return 0;
    }

    auto* storage = storages->GetEntityStorage(*storageIndex);
    if (storage == nullptr) return 0;
    auto instance = storage->InstanceToPageMap.try_get(entity);
    if (instance == nullptr) return 0;

    std::size_t n = 0;
    for (auto type : storage->ComponentTypeToIndex) {
        if (storage->ModifiedComponents[type.Value()]
            && storage->WasComponentChanged(*instance, type.Value())) {
            if (n < max) out[n] = (std::uint16_t)type.Key();
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Component construct/destroy events, as upstream's EntityComponentEventHooks
// (Lua/Shared/EntityComponentEvents.inl): a connection on the engine's own
// per-type signals in world->ComponentCallbacks.
//
// Events are queued here and delivered by Lua from the server tick, because
// the engine fires these from inside its own update and bg3le's Lua states
// must only be entered from their own thread.
// ---------------------------------------------------------------------------

namespace {

struct ComponentEvent {
    std::uint64_t Entity;
    std::uint64_t World;  // the EntityWorld that raised it
    std::uint16_t Type;
    std::uint8_t Kind;  // 1 construct, 2 destroy
};

std::mutex& events_lock() {
    static std::mutex m;
    return m;
}

std::vector<ComponentEvent>& event_queue() {
    static std::vector<ComponentEvent> q;
    return q;
}

void queue_event(std::uint16_t type, std::uint64_t entity, std::uint64_t world,
                 std::uint8_t kind) {
    static std::atomic<int> said{0};
    static std::atomic<int> saidOneFrame{0};
    if (said.fetch_add(1) < 3 || ((type & 0x8000) != 0 && saidOneFrame.fetch_add(1) < 3)) {
        bg3le::logf("component events: %s of type %u for %#llx on thread %lu",
                    kind == 1 ? "construct" : "destroy", type,
                    (unsigned long long)entity, (unsigned long)pthread_self());
    }
    const std::lock_guard<std::mutex> held(events_lock());
    if (event_queue().size() < (1u << 20)) {
        event_queue().push_back(ComponentEvent{entity, world, type, kind});
    }
}

using Connection = bg3se::ecs::ComponentSignal::Connection;
using Storage = bg3se::FunctionStorage<void(bg3se::ecs::EntityRef*, void*)>;

// The engine's signal passes its EntityRef by value. bg3se writes the
// parameter as EntityRef* because MSVC passes a 16-byte struct by hidden
// reference; System V passes it in two registers, so it arrives as the
// handle and the world, with the component third.
template <std::uint8_t Kind>
void component_signal_call(Storage const& self, std::uint64_t handle,
                           std::uint64_t world, void* /*component*/) {
    queue_event(*self.data<std::uint16_t>(), handle, world, Kind);
}

Storage* component_signal_copy(Storage const&, Storage const& src, Storage* dst) {
    if (dst == nullptr) return nullptr;
    dst->call_ = src.call_;
    dst->copy_ = src.copy_;
    dst->move_ = src.move_;
    std::memcpy(dst->data_, src.data_, sizeof(dst->data_));
    return dst;
}

Storage* component_signal_move(Storage const& self, Storage&& src, Storage* dst) {
    return component_signal_copy(self, src, dst);
}

// A Connection handler that calls component_signal_call<Kind> for one type.
template <std::uint8_t Kind>
bg3se::ecs::ComponentSignal::Function component_handler(std::uint16_t type) {
    Storage storage;
    storage.call_ = reinterpret_cast<Storage::CallProc*>(
        &component_signal_call<Kind>);
    storage.copy_ = &component_signal_copy;
    storage.move_ = &component_signal_move;
    std::memset(storage.data_, 0, sizeof(storage.data_));
    *storage.data<std::uint16_t>() = type;
    return bg3se::ecs::ComponentSignal::Function(storage);
}

// Signal::Add without its push_back: the array is the engine's, so a full one
// moves to a fresh engine allocation and the old buffer is left, never
// freed. A Function points at its own storage, so a moved one is re-pointed.
// An engine Array's own three members, which bg3se keeps private.
struct RawArray {
    void* Buffer;
    std::uint32_t Capacity;
    std::uint32_t Size;
};
static_assert(sizeof(RawArray) == sizeof(bg3se::Array<Connection>));

bool add_connection(bg3se::ecs::ComponentSignal& signal,
                    bg3se::ecs::ComponentSignal::Function const& handler) {
    auto* array = reinterpret_cast<RawArray*>(&signal.Connections);
    const std::uint32_t size = array->Size;
    const std::uint32_t capacity = array->Capacity;
    auto* buffer = static_cast<Connection*>(array->Buffer);

    if (size >= capacity) {
        if (!bg3le_game_allocator_ready()) return false;
        const std::uint32_t grown = capacity + capacity / 2 + 2;
        auto* fresh = static_cast<Connection*>(
            bg3se::GameAllocRaw(sizeof(Connection) * grown));
        if (fresh == nullptr) return false;
        std::memset((void*)fresh, 0, sizeof(Connection) * grown);
        for (std::uint32_t i = 0; i < size; ++i) {
            std::memcpy((void*)&fresh[i], (void const*)&buffer[i],
                        sizeof(Connection));
            auto** self = reinterpret_cast<void**>(&fresh[i].Handler);
            auto* oldStorage = reinterpret_cast<char*>(&buffer[i].Handler) + 8;
            if (*self == oldStorage) {
                *self = reinterpret_cast<char*>(&fresh[i].Handler) + 8;
            }
        }
        buffer = fresh;
        array->Buffer = fresh;
        array->Capacity = grown;
    }

    // Built in place, so its storage pointer is its own.
    new (&buffer[size]) Connection(handler, signal.NextRegistrantId++);
    __atomic_store_n(&array->Size, size + 1, __ATOMIC_RELEASE);
    return true;
}

// Whether a signal already carries bg3le's handler for this type. Asked of
// the engine's own array, so a new world (another save) starts unwatched.
bool has_connection(bg3se::ecs::ComponentSignal& signal, void const* call,
                    std::uint16_t type) {
    auto const* array = reinterpret_cast<RawArray const*>(&signal.Connections);
    auto* buffer = static_cast<Connection*>(array->Buffer);
    for (std::uint32_t i = 0; buffer != nullptr && i < array->Size; ++i) {
        auto const* storage = *reinterpret_cast<Storage* const*>(&buffer[i].Handler);
        if (storage != nullptr && reinterpret_cast<void const*>(storage->call_) == call
            && *storage->data<std::uint16_t>() == type) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Starts delivering construct and destroy events for one component type.
extern "C" bool bg3le_component_events_watch(void* container,
                                             std::uint16_t componentIndex) {
    auto* world = container ? world_from_container(container) : nullptr;
    if (world == nullptr) return false;
    auto& registry = world->ComponentCallbacks;
    // Upstream's SparseHashMapHash: one-frame types (0x8000) follow the others.
    const std::uint32_t slot = (componentIndex & 0x7fffu)
                               + (componentIndex >> 15) * bg3se::ecs::ComponentMapSize;
    if (slot >= registry.Callbacks.size()) return false;
    auto* callbacks = registry.Callbacks[slot];
    if (callbacks == nullptr) return false;

    const std::uint16_t type = componentIndex;
    if (!has_connection(callbacks->OnConstruct,
                        reinterpret_cast<void const*>(&component_signal_call<1>), type)
        && !add_connection(callbacks->OnConstruct, component_handler<1>(type))) {
        return false;
    }
    if (!has_connection(callbacks->OnDestroy,
                        reinterpret_cast<void const*>(&component_signal_call<2>), type)
        && !add_connection(callbacks->OnDestroy, component_handler<2>(type))) {
        return false;
    }
    bg3le::logf("component events: watching type %u (slot %u) in world %p", componentIndex, slot,
                (void*)world);
    return true;
}

// Takes up to max queued events raised by one world; returns how many were
// written. The other world's stay queued for its own context.
extern "C" std::size_t bg3le_component_events_take(void* world,
                                                   std::uint64_t* entities,
                                                   std::uint16_t* types,
                                                   std::uint8_t* kinds,
                                                   std::size_t max) {
    const std::lock_guard<std::mutex> held(events_lock());
    auto& q = event_queue();
    const auto want = (std::uint64_t)(std::uintptr_t)world;
    std::size_t n = 0;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q[i].World == want && n < max) {
            entities[n] = q[i].Entity;
            types[n] = q[i].Type;
            kinds[n] = q[i].Kind;
            ++n;
        } else {
            q[keep++] = q[i];
        }
    }
    q.resize(keep);
    return n;
}

// Replication changes, as upstream's ServerEntityReplicationEventHooks reads
// them after the world update: each watched replication pool's entities
// and the first qword of their dirty fields. The server tick runs after the
// update that set them, so each change is seen once.
namespace {
std::vector<bool>& watched_replication() {
    static std::vector<bool> w;
    return w;
}
}  // namespace

extern "C" void bg3le_replication_watch(std::uint16_t replicationTypeIndex) {
    auto& w = watched_replication();
    if (w.size() <= replicationTypeIndex) w.resize(replicationTypeIndex + 1);
    w[replicationTypeIndex] = true;
}

extern "C" std::size_t bg3le_replication_changes(void* container,
                                                 std::uint64_t* entities,
                                                 std::uint16_t* types,
                                                 std::uint64_t* fields,
                                                 std::size_t max) {
    // Not gated on Replication->Dirty as upstream's is: upstream reads
    // straight after the update, and by the server tick the flag has been
    // reset while the pools still hold that update's changes.
    auto* world = container ? world_from_container(container) : nullptr;
    if (world == nullptr || world->Replication == nullptr) return 0;
    auto const& watched = watched_replication();
    auto& pools = world->Replication->ComponentPools;
    std::size_t n = 0;
    for (unsigned i = 0; i < pools.size() && i < watched.size(); i++) {
        if (!watched[i]) continue;
        for (auto const& entry : pools[i]) {
            if (n >= max) return n;
            const std::uint64_t changed =
                entry.Value().NumQwords() > 0 ? *entry.Value().GetBuf() : 0;
            if (changed == 0) continue;
            entities[n] = entry.Key().Handle;
            types[n] = (std::uint16_t)i;
            fields[n] = changed;
            ++n;
        }
    }
    return n;
}

// Diagnostic: one component type's construct/destroy signals, as bg3se lays
// them out, to confirm the layout before anything is added to them.
extern "C" void bg3le_component_callbacks_probe(void* container,
                                                std::uint16_t componentIndex) {
    auto* world = container ? world_from_container(container) : nullptr;
    if (world == nullptr) {
        bg3le::logf("callbacks probe: no world");
        return;
    }
    auto& registry = world->ComponentCallbacks;
    bg3le::logf("callbacks probe: registry holds %u entries (capacity %u)",
                registry.Callbacks.size(), registry.Callbacks.capacity());
    const std::uint32_t slot = (componentIndex & 0x7fffu)
                               + (componentIndex >> 15) * bg3se::ecs::ComponentMapSize;
    if (slot >= registry.Callbacks.size()) return;
    auto* cb = registry.Callbacks[slot];
    bg3le::logf("callbacks probe: type %u (slot %u) -> %p", componentIndex, slot, (void*)cb);
    if (cb == nullptr) return;

    auto dump = [](char const* which, bg3se::ecs::ComponentSignal const& sig) {
        bg3le::logf("callbacks probe:   %s next id %llu, %u connections "
                    "(capacity %u)", which,
                    (unsigned long long)sig.NextRegistrantId,
                    sig.Connections.size(), sig.Connections.capacity());
        for (unsigned i = 0; i < sig.Connections.size() && i < 3; ++i) {
            auto const& c = sig.Connections[i];
            auto const* raw = reinterpret_cast<std::uintptr_t const*>(&c.Handler);
            bg3le::logf("callbacks probe:     [%u] at %p: %#lx %#lx %#lx %#lx "
                        "%#lx %#lx %#lx registrant %llu", i, (void const*)&c,
                        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                        (unsigned long long)c.RegistrantIndex);
        }
    };
    dump("construct", cb->OnConstruct);
    dump("destroy", cb->OnDestroy);
}

// GetRegisteredComponentTypes: every component type the world registers,
// as upstream walks ComponentRegistry_.
extern "C" std::size_t bg3le_registered_component_types(void* container,
                                                        std::uint16_t* out,
                                                        std::size_t max) {
    auto* world = container ? world_from_container(container) : nullptr;
    if (world == nullptr) return 0;

    auto const& registry = world->ComponentRegistry_;
    std::size_t n = 0;
    for (unsigned i = 0; i < registry.Bitmask.Size; i++) {
        if (registry.Bitmask[i]) {
            if (n < max) out[n] = (std::uint16_t)registry.Types[i].TypeId;
            ++n;
        }
    }
    return n;
}

// GetReplicationFlags: the qword of the entity's flags for one replicated
// type, zero where it has none. False when there is no replication.
extern "C" bool bg3le_entity_replication_flags(void* container,
                                               std::uint64_t handle,
                                               std::uint16_t replicationTypeIndex,
                                               std::uint32_t qword,
                                               std::uint64_t* flags) {
    *flags = 0;
    auto* world = container ? world_from_container(container) : nullptr;
    if (world == nullptr || world->Replication == nullptr) return false;

    auto& pools = world->Replication->ComponentPools;
    if (replicationTypeIndex >= pools.Size()) return false;

    auto* mask = pools[replicationTypeIndex].try_get(bg3se::EntityHandle(handle));
    if (mask != nullptr && qword < mask->NumQwords()) {
        *flags = mask->GetBuf()[qword];
    }
    return true;
}

}  // namespace bg3le

// ---------------------------------------------------------------------------
// System update hooks: upstream's SetSystemUpdateHook. A system's entry in
// its world's SystemRegistry has its UpdateProc swapped for a trampoline
// that tells bg3le before and after the original runs, on whatever thread
// the scheduler ran it on.
// ---------------------------------------------------------------------------

extern "C" void bg3le_system_update_event(bool client, std::int32_t index, bool post);

namespace {

using SystemUpdateProc = bg3se::ecs::SystemTypeEntry::UpdateProcType*;

struct HookedSystem {
    SystemUpdateProc Original;
    std::int32_t Index;
    bool Client;
};

std::mutex& system_hooks_lock() {
    static std::mutex m;
    return m;
}

std::unordered_map<void*, HookedSystem>& hooked_systems() {
    static std::unordered_map<void*, HookedSystem> m;
    return m;
}

void system_update_trampoline(bg3se::BaseSystem* system, bg3se::ecs::EntityWorld& world,
                              bg3se::GameTime const& time) {
    HookedSystem hook{};
    {
        const std::lock_guard<std::mutex> held(system_hooks_lock());
        auto found = hooked_systems().find(system);
        if (found == hooked_systems().end()) return;
        hook = found->second;
    }
    bg3le_system_update_event(hook.Client, hook.Index, false);
    hook.Original(system, world, time);
    bg3le_system_update_event(hook.Client, hook.Index, true);
}

// The entry for a system index, if the registry agrees about it.
bg3se::ecs::SystemTypeEntry* system_entry(void* container, std::int32_t index) {
    auto* world = container ? bg3le::world_from_container(container) : nullptr;
    if (world == nullptr || index < 0) return nullptr;
    auto& systems = world->Systems.Systems;
    if ((std::uint32_t)index >= systems.size()) return nullptr;
    auto& entry = systems[(std::uint32_t)index];
    if (entry.System == nullptr || (std::int32_t)entry.SystemIndex0 != index) return nullptr;
    return &entry;
}

}  // namespace

// Starts reporting a system's updates for the world a container belongs to.
extern "C" bool bg3le_system_hook(void* container, std::int32_t index, bool client) {
    auto* entry = system_entry(container, index);
    if (entry == nullptr || entry->UpdateProc == nullptr) return false;

    const std::lock_guard<std::mutex> held(system_hooks_lock());
    if (entry->UpdateProc == &system_update_trampoline) return true;
    hooked_systems()[entry->System] = HookedSystem{entry->UpdateProc, index, client};
    __atomic_store_n(&entry->UpdateProc, &system_update_trampoline, __ATOMIC_RELEASE);
    bg3le::logf("systems: hooked system %d in %s world", index, client ? "the client" : "the server");
    return true;
}

// For checking the registry's layout: the entry's own index and system.
extern "C" bool bg3le_system_probe(void* container, std::int32_t index, void** system,
                                   std::int32_t* ownIndex, void** update, std::uint32_t* count) {
    auto* world = container ? bg3le::world_from_container(container) : nullptr;
    if (world == nullptr) return false;
    auto& systems = world->Systems.Systems;
    *count = systems.size();
    if (index < 0 || (std::uint32_t)index >= systems.size()) return false;
    auto& entry = systems[(std::uint32_t)index];
    *system = entry.System;
    *ownIndex = (std::int32_t)entry.SystemIndex0;
    *update = (void*)entry.UpdateProc;
    return true;
}


extern "C" int bg3le_engine_thread_index();

// The engine addresses a thread's state as generator + thread * 64.
static_assert(offsetof(bg3se::ecs::EntityHandleGenerator, ThreadStates) == 0
              && sizeof(bg3se::ecs::EntityHandleGenerator::ThreadState) == 64);

// Upstream's Ext.Entity.Create and Destroy, through the calling thread's
// entity command buffer as upstream's go. 0 / false when the thread has no
// engine thread index, which would pick someone else's buffer.
extern "C" std::uint64_t bg3le_entity_create(void* container) {
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr || bg3le_engine_thread_index() < 0 || !bg3le_game_allocator_ready()) return 0;
    return world->Deferred()->CreateEntityImmediate().Handle;
}

// Upstream's CreateComponentRaw (deferred, through the thread's command
// buffer) or CreateComponentImmediateRaw (the immediate cache): the slot the
// component goes in, which for a proxy holds a pointer to it.
// The engine's own destructor for a type, from its ComponentRegistry_, else
// bg3le's. A frame's ECB storage keeps the destructor it was made with and
// destroys the engine's components of that type too, so it must be the engine's.
void* engine_dtor(bg3se::ecs::EntityWorld* world, std::uint16_t type, void* fallback) {
    auto const& registry = world->ComponentRegistry_;
    const std::uint32_t slot = (type & 0x7fffu) + (type >> 15) * bg3se::ecs::ComponentMapSize;
    if (slot < registry.Bitmask.Size && registry.Bitmask[slot] && slot < registry.Types.size()
        && registry.Types[slot].DtorProc != nullptr) {
        return registry.Types[slot].DtorProc;
    }
    return fallback;
}

extern "C" void* bg3le_entity_component_add(void* container, std::uint64_t handle,
                                            std::uint16_t type, std::uint16_t inlineSize,
                                            void* dtor, bool immediate) {
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr || bg3le_engine_thread_index() < 0 || !bg3le_game_allocator_ready()) return nullptr;
    const bg3se::EntityHandle entity{handle};
    const bg3se::ecs::ComponentTypeIndex typeId{type};
    if (immediate) {
        void* slot = nullptr;
        return world->Cache != nullptr && world->Cache->PrepareAddComponent(entity, typeId, slot) ? slot : nullptr;
    }
    bg3se::ecs::ComponentFrameStorageIndex index;
    return world->Deferred()->CreateComponentRaw(entity, typeId, inlineSize, index,
                                                 engine_dtor(world, type, dtor));
}

// Upstream's CreateComponentImmediateRaw ends with this since v33: the type's
// OnConstruct callbacks, which OnCreate subscriptions hang off.
extern "C" void bg3le_entity_component_finalize(void* container, std::uint64_t handle,
                                                std::uint16_t type, void* slot) {
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr || world->Cache == nullptr || slot == nullptr) return;
    world->Cache->FinalizeAddComponent(bg3se::EntityHandle{handle},
                                       bg3se::ecs::ComponentTypeIndex{type}, slot);
}

// Upstream's RemoveComponent and RemoveComponentImmediate.
extern "C" bool bg3le_entity_component_remove(void* container, std::uint64_t handle,
                                              std::uint16_t type, std::uint16_t inlineSize,
                                              void* dtor, bool immediate) {
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr || bg3le_engine_thread_index() < 0 || !bg3le_game_allocator_ready()) return false;
    const bg3se::EntityHandle entity{handle};
    const bg3se::ecs::ComponentTypeIndex typeId{type};
    if (immediate) return world->Cache != nullptr && world->Cache->RemoveComponent(entity, typeId);
    // As upstream's since v33: through the ECB, a component the entity lacks, or
    // one on an entity still in the buffer, crashes.
    auto storage = world->GetEntityStorage(entity);
    if (!storage || storage->ComponentTypeToIndex.find(typeId) == storage->ComponentTypeToIndex.end()) {
        return false;
    }
    world->Deferred()->RemoveComponent(entity, typeId, inlineSize, engine_dtor(world, type, dtor));
    return true;
}

// This frame's command-buffer changes for an entity, as upstream's WasAdded,
// WasRemoved and the current-frame lists read them: each component change's
// type and, for an addition, the component (dereferenced when proxy[i]).
// Returns the count, or -1 with no change record; flags gets the entity's.
extern "C" int bg3le_entity_ecb_changes(void* container, std::uint64_t handle,
                                        std::uint16_t* types, void** components,
                                        bool (*isProxy)(std::uint16_t), int max,
                                        std::uint32_t* flags) {
    auto* world = bg3le::world_from_container(container);
    *flags = 0;
    if (world == nullptr || bg3le_engine_thread_index() < 0) return -1;
    auto ecb = world->Deferred();
    auto change = ecb->Data.GetEntityChange(bg3se::EntityHandle{handle});
    if (!change) return -1;
    *flags = (std::uint32_t)change->Flags;
    int n = 0;
    for (std::uint32_t i = 0; i < change->Store.size() && n < max; ++i) {
        auto const& comp = change->Store[i];
        types[n] = (std::uint16_t)comp.ComponentTypeId;
        void* component = nullptr;
        if (comp.Index) {
            component = ecb->GetComponentChange(comp.ComponentTypeId, comp.Index);
            if (component != nullptr && isProxy((std::uint16_t)comp.ComponentTypeId)) {
                component = bg3se::ecs::DereferenceProxyComponent(component);
            }
            if (component == nullptr) component = (void*)1;  // added, unreadable
        }
        components[n++] = component;
    }
    return n;
}

// A component not committed yet, such as one on an item Osi.CreateAt has just
// made: upstream's fallback to the immediate cache, then the thread's command
// buffer (EntityWorld::GetRawComponent). *immediate is set for the cache, which
// holds the component itself rather than a proxy's slot.
extern "C" void* bg3le_entity_pending_component(void* container, std::uint64_t handle,
                                                std::uint16_t type, bool* immediate) {
    *immediate = false;
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr) return nullptr;
    const bg3se::EntityHandle entity{handle};
    const bg3se::ecs::ComponentTypeIndex typeId{type};
    if (world->Cache != nullptr) {
        void* change = world->Cache->WriteChanges.GetChange(entity, typeId);
        if (change == nullptr) change = world->Cache->ReadChanges.GetChange(entity, typeId);
        if (change != nullptr) {
            *immediate = true;
            return change;
        }
    }
    if (bg3le_engine_thread_index() < 0) return nullptr;
    return world->Deferred()->GetComponentChange(entity, typeId);
}

extern "C" bool bg3le_entity_destroy(void* container, std::uint64_t handle) {
    auto* world = bg3le::world_from_container(container);
    if (world == nullptr || bg3le_engine_thread_index() < 0 || !bg3le_game_allocator_ready()) return false;
    return world->Deferred()->DestroyEntity(bg3se::EntityHandle{handle});
}


