#pragma once
#include <cstdint>

// SMSG_PATCH_* runtime DBC row patching (see AscDbcPatch.cpp).
namespace AscDbcPatch
{
    typedef void (*AfterPatch)(const uint8_t* row);
    // stringMask bit n = struct field n arrives as a trailing C string.
    void Register(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after);
    // Same, for handlers whose strings arrive as u32 length + bytes (FUN_100d3680).
    // Same as Register, but `after` runs only when the row was inserted (not when overwritten).
    void RegisterInsertHook(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after);
    void RegisterSized(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after);
}
