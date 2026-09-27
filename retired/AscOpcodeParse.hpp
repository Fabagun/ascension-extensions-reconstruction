// Field-descriptor interpreter for the generated extension-opcode table.
//
// The atlas describes every server->client extension opcode as a flat list of fields (with repeat
// blocks keyed by an earlier count field). Rather than emit 538 near-identical parsers, the
// generator emits descriptors and this walks them.
//
// The payload check is the point: if the declared layout does not consume the packet EXACTLY, the
// atlas layout is wrong for this build. 417 of its server->client layouts are "named" -- derived
// from decompilation and never checked against live traffic -- against only 4 "proven".
#pragma once

#include <cstdint>

struct CDataStore;

enum AscFieldType : uint8_t
{
    FT_U8, FT_U16, FT_U32, FT_I32, FT_U64, FT_F32,
    FT_LPSTRING,        // u32 length, then that many bytes (client's FUN_100d3680 helper: no NUL)
    FT_CSTRING,         // NUL-terminated
    FT_PACKED_GUID,     // u8 mask, then one byte per set bit
    FT_BYTES,           // fixed width, from the descriptor
    FT_BYTES_VAR,       // variable: consumes the remainder
    FT_REPEAT_BEGIN,    // repeatCount names an earlier field holding the iteration count
    FT_REPEAT_END,
    FT_UNKNOWN,         // layout known to be incomplete; stop parsing, do not guess
};

struct AscField
{
    AscFieldType type;
    uint16_t     size;          // for FT_BYTES
    const char*  name;
    const char*  repeatCount;   // for FT_REPEAT_BEGIN: name of the count field
};

struct AscOpcodeDesc
{
    uint32_t         opcode;
    const char*      name;
    const char*      layoutState;    // proven | named | widths | empty | unknown
    const AscField*  fields;
    uint16_t         fieldCount;
};

// Parses the packet per the descriptor, logs the outcome, and always leaves the CDataStore
// finished exactly once (mirroring the native no-handler path).
void AscOpcodeParse(const AscOpcodeDesc& desc, CDataStore* ds);
