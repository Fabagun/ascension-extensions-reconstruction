#pragma once
#include <cstdint>
#include <vector>

// The server config store (FUN_101982d0 -> static 0x10BDEC20), filled by SMSG_UPDATE_CONFIGS 0x58D.
// Lookups return nullptr for an unknown name, as the original's accessors do.
namespace AscConfig
{
    const int32_t* Int(const char* name);                       // FUN_10196ad0  (+0x00)
    const bool* Bool(const char* name);                         // FUN_10196910  (+0x20)
    const float* Float(const char* name);                       // FUN_101969b0  (+0x40)
    const float* Rate(const char* name);                        // FUN_10196c10  (+0x60)
    const std::vector<int32_t>* IntVector(const char* name);    // FUN_10196b50  (+0x80)
    const std::vector<float>* FloatVector(const char* name);    // FUN_10196a30  (+0xA0)
}
