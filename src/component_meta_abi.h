// The shape of the component field tables, shared between bg3le and the
// translation unit that extracts them from bg3se's generated metadata.
//
// Kept free of bg3se headers deliberately: src/vendor/component_meta.cpp is
// the only place that includes them, and everything else works from this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bg3le {

// What a field holds. Only the kinds bg3le can convert without interpretation
// are named; everything else is Unsupported and is reported as such rather
// than guessed at.
enum class FieldKind : std::uint8_t {
    Unsupported = 0,
    Bool,
    Float,
    Double,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Guid,
    Entity,
    // A 32-bit index into the engine's global string table, readable only once
    // that table has been found -- see src/vendor/fixed_string.cpp.
    FixedString,
    // Larian's own string, sixteen bytes: inline up to fifteen characters
    // with the length in the last byte, otherwise a pointer with a size and
    // a capacity. See vendor/bg3se/CoreLib/Base/LSString.h -- it is not any
    // std::string, which is why it needs a kind of its own.
    LSString,
    // A fixed-extent array of one of the scalar kinds above, which is how the
    // engine stores the per-ability and per-skill tables. ElemKind and
    // ElemCount describe the elements.
    ScalarArray,
    // A nested struct. TypeName names its type; whether it is traversable
    // depends on bg3se describing that type too, which is resolved by name at
    // load rather than at compile time -- a field's type does not have to have
    // a field table of its own for the field itself to be recorded.
    Struct,
    // A dynamically sized array. Its length and buffer are read through
    // Count and Data below rather than through guessed member offsets.
    // ElemKind, ElemSize and ElemTypeName describe the elements.
    DynArray,
    // A hash map. Its keys and values are two parallel contiguous runs, so
    // slot i holds key i and value i; KeyData and Data reach them. The
    // element fields describe the values, the key fields the keys.
    Map,
    // A value that may or may not be there. Count is nought or one and Data
    // points at it when there is one, so it indexes like a container -- which
    // keeps "empty" distinct from "cannot be read".
    Optional,
    // One of several types, with the active one known only at runtime.
    // Alternatives lists a descriptor per alternative and ActiveIndex says
    // which is live, so indexing it selects an alternative -- and only
    // resolves for the one actually held, since the bytes are not the others.
    Variant,
    // Not a field: records that the class also has the fields of the class
    // named in Name. Classes are declared in dependency-free order, so bases
    // are resolved by name at load rather than by pointer.
    Inherit,
    // A component handle: an integer to Lua, or nil when null.
    ComponentHandle,
    // A stats condition's id, read as the condition's text.
    ConditionId,
    // A pointer to a class bg3se describes: reads as the target's address,
    // or nil, and a path continues through it, as upstream follows one.
    // TypeName names the target type.
    Pointer,
    // Text the field holds in some form other than the two string kinds
    // above -- a C string, a string view, a byte buffer, a Noesis string --
    // read through ReadText, as upstream pushes each: a string, or nil.
    Text,
    // bg3se's Version, a packed uint64, read as {major, minor, revision,
    // build}.
    Version,
    // EntityOrVec3Variant: a position when its type byte is set, else an
    // entity.
    EntityOrVec3,
    // BitArray: a table of ElemCount booleans, read and written whole.
    BitArray,
    // gn::GenomeVariant: a type descriptor and a value, read and written as the
    // value, as upstream's LuaGetValue and LuaSetValue.
    GenomeVariant,
};

struct FieldDesc;

struct FieldDesc {
    char const* Name;       // a string literal from the generated metadata
    std::uint32_t Offset;
    std::uint16_t Size;
    FieldKind Kind;
    FieldKind ElemKind;       // ScalarArray only
    std::uint16_t ElemCount;  // ScalarArray only
    std::uint16_t ElemSize;   // element stride, for the array kinds
    // The field's C++ type, for Struct, and the element type for an array of
    // structs. Neither is NUL-terminated: both are slices of a
    // compiler-generated function-name string, so they carry their own length.
    char const* TypeName;
    std::uint16_t TypeNameLength;
    char const* ElemTypeName;
    std::uint16_t ElemTypeNameLength;
    // DynArray only: the container's own size() and data(), instantiated for
    // the field's exact type. Going through the real accessors rather than
    // reading a guessed offset for the length and buffer members keeps this
    // correct by construction, the same way offsetof does for a plain field.
    std::size_t (*Count)(void const* container);
    void* (*Data)(void const* container);
    // Map only: the key run, described in parallel with the value run above.
    FieldKind KeyKind;
    std::uint16_t KeySize;
    void* (*KeyData)(void const* container);
    // Set for a view that must not be written through. A hash set's elements
    // are its keys, and writing one in place would leave the table's hashes
    // pointing at the old value, so the set is readable and not writable.
    bool ReadOnly;
    // A full descriptor for the element type of any of the container kinds.
    //
    // The element fields above say enough to read a scalar element, but not to
    // index one that is itself a container -- a map of arrays needs the inner
    // array's own accessors, and those belong to the element type rather than
    // to the field. Indexing a container therefore continues with this
    // descriptor, which is how "Resources[0][1].Amount" works.
    FieldDesc const* ElemDesc;
    // Variant only. Alternatives is null-terminated; ActiveIndex returns the
    // live one, or the largest size_t when the variant is valueless.
    FieldDesc const* const* Alternatives;
    std::size_t (*ActiveIndex)(void const* variant);
    // Set only: replaces the whole set with the keys given, through the
    // container's own insert(), which rehashes rather than leaving the
    // table pointing at the old keys. This is what makes a set writable at
    // all -- an element cannot be, for the reason above, but the set can.
    //
    // Last on purpose. Putting it next to ReadOnly, where it belongs by
    // meaning, moved every member after it, and something initialises one
    // of these by position: the first call through a shifted Assign was a
    // jump to address zero.
    bool (*Assign)(void* container, void const* values, std::size_t count);
    // Optional only: engages or clears it through the container's own
    // emplace() and reset(), so the payload is default-constructed and the
    // flag is set the way the type itself sets it rather than by guessing
    // where libc++ keeps it.
    //
    // After Assign, for the reason above.
    void (*Engage)(void* container, bool engaged);
    // OverrideableProperty only: where its IsOverridden flag sits, from the
    // start of the field. Zero means the field is not one -- Value comes
    // first, so the flag is never at zero.
    //
    // Such a field is otherwise described as its Value, because that is how
    // upstream presents it: it reads as a plain T. Assigning it is the one
    // place the wrapper shows, since upstream's setter builds
    // {value, true} and so marks the property overridden.
    //
    // Last, for the reason Assign's comment gives.
    std::uint16_t OverrideFlagAt;
    // This build's sizeof for the field's type, where it differs from the
    // engine's (Size); zero where they agree. A difference means any struct
    // holding the field inline lays out its later members differently from
    // the engine, so it is worth knowing about before a read goes wrong.
    // Last, for the reason Assign's comment gives.
    std::uint16_t CompiledSize;
    // ecs::EntityRef only: where its World pointer sits, from the start of
    // the field. Zero means the field is not one -- Handle comes first, so
    // World is never at zero. See bg3le_meta_after_write.
    // Last, for the reason Assign's comment gives.
    std::uint16_t EntityWorldAt;
    // Map only: the key's enum type, so a key reads as its label the way
    // upstream pushes it. Null for a key that is not an enum.
    char const* KeyTypeName;
    std::uint16_t KeyTypeNameLength;
    // A glm vector or matrix, which upstream pushes as a plain Lua table
    // rather than as an array proxy.
    bool IsVector;
    // P_BITMASK: each label of this field's enum is also a boolean property
    // of the object, reading and writing its own bit. Last, as above.
    bool BitmaskFlags;
    // DynArray only: rebuilds the container at count default elements in a
    // fresh allocation, abandoning the old buffer rather than freeing it
    // (bg3le cannot know who allocated it). Last, as above.
    bool (*Resize)(void* container, std::size_t count);
    // Upstream's ordered erase and push_back of a default element, done without
    // destroying anything: the other elements are relocated into a fresh buffer.
    bool (*Erase)(void* container, std::size_t index);
    bool (*Append)(void* container);
    // Map only, for a map whose entries are nodes rather than two contiguous
    // runs (LegacyMap, LegacyRefMap): the value and the key of entry index,
    // used in place of Data and KeyData. Last, as above.
    void* (*ElemAt)(void const* container, std::size_t index);
    void* (*KeyAt)(void const* container, std::size_t index);
    // Text only: the text, and whether there is any (false for a null C
    // string or an empty buffer, which read as nil). Last, as above.
    bool (*ReadText)(void const* field, std::string* out);
    // Map only: adds a key with a default value, or removes one (leaking its
    // value rather than destroying it). Null where bg3le cannot. Last, as above.
    bool (*MapInsert)(void* container, void const* key);
    bool (*MapRemove)(void* container, void const* key);
    // P_RENAMED's old name: found by name, left out of listings, as
    // upstream's is. Last, as above.
    bool Legacy;
};

}  // namespace bg3le
