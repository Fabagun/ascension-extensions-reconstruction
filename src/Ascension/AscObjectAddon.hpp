#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Server-driven custom object fields: SMSG_UPDATE_OBJECT_ADDON 0x578 (handler_0x0578 / FUN_102ce0c0).
// Per GUID the DLL keeps a vector of 12-byte fields {value, callback, callback arg}. Units and players
// (FUN_102cdcf0, map 0x10BE3648) have 0x58 fields, items (FUN_102cdbb0, map 0x10BE3608) one; the
// other GUID kinds get empty records.
namespace AscObjectAddon
{
    struct Field
    {
        uint32_t value = 0;
        void (__cdecl* callback)(uint32_t guidLo, uint32_t guidHi, uint32_t index, void* arg, Field* field) = nullptr;
        void* arg = nullptr;
    };
    static_assert(sizeof(Field) == 12, "addon field");

    // The lookups create the record (sized) when it does not exist yet, as the originals do.
    std::vector<Field>& Unit(uint64_t guid);   // FUN_102cdcf0
    std::vector<Field>& Item(uint64_t guid);   // FUN_102cdbb0

    inline float AsFloat(const Field& f) { float v; static_assert(sizeof v == 4, ""); memcpy(&v, &f.value, 4); return v; }

    // FUN_102ce900 -> FUN_102ce690 with a zero GUID: a callback for field `index` of every object of
    // the given kind (0..7, the FUN_102ce0c0 classification), run after the field's own callback.
    typedef void (__cdecl* TypeCallback)(uint32_t guidLo, uint32_t guidHi, uint32_t index, void* arg, Field* field);
    void RegisterTypeCallback(uint32_t kind, uint32_t index, TypeCallback fn, void* arg);
}
