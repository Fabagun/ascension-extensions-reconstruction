#pragma once
#include <cstdint>

// Token manager (FUN_1033ce30): a vector of 61 (TOKEN_TYPE_MAX) 16-byte entries, +0 type index,
// +8 amount, +0xC second dword. Filled by SMSG_TOKEN_LIST 0x66F / SMSG_TOKEN_UPDATE 0x670.
namespace AscToken
{
    const uint32_t kCount = 0x3D;
    bool TypeIndex(const char* name, uint32_t& index);   // FUN_102fe780 (62 names incl. TOKEN_TYPE_MAX)
    int32_t Amount(uint32_t index);                       // 0 (DAT_10b5dbb0) when out of range
    uint64_t Amount64(uint32_t index);                    // FUN_1033c290: {extra:amount}
    const char* TypeName(uint32_t index);
    // FUN_1033cd90: the entry's {extra:amount} as a u64 is >= {hi:lo}; out of range reads zeros.
    bool AtLeast(uint32_t index, uint32_t lo, uint32_t hi);                 // PTR_s_TOKEN_TYPE_..._10b58850[index], nullptr >= 62
}
