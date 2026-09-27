#pragma once
// The original's MemoryBridgeClient (0x102B0000..0x102B7FFF): the DLL's side of Ascension's MMgr64.exe
// out-of-process memory / table store. MMgr64.exe itself is Ascension's and is used as-is.
//
// Public API = the original's thin wrappers 0x102B7A70..0x102B7DB0 (instance 0x10BE3354); each bumps the
// operation statistics FUN_102b7cf0 where the original does. Failures throw std::runtime_error exactly where
// the original throws (_CxxThrowException) and return false where it returns false.
#include <cstdint>
#include <string>
#include <vector>

namespace AscMemoryBridge
{
    bool Initialize();                                                   // 0x102B7B60 -> FUN_102b5dd0
    void Shutdown();                                                     // FUN_102b7130

    uint32_t AllocateMemory(uint32_t sizeLo, uint32_t sizeHi);           // 0x102B7A70, command 1
    bool FreeMemory(uint32_t handle);                                    // 0x102B7B50, command 2
    bool ReadMemory(uint32_t handle, uint32_t offset, void* dst, uint32_t size);          // 0x102B7BE0, command 3
    bool WriteMemory(uint32_t handle, uint32_t offset, const void* src, uint32_t size);   // 0x102B7D60, command 4
    uint32_t CreateTable(uint32_t a, uint32_t b);                        // 0x102B7AB0, command 100
    bool DestroyTable(uint32_t table);                                   // 0x102B7AE0, command 101
    bool Command66(uint32_t a, uint32_t offset, const void* src, uint32_t size);          // 0x102B7D80
    bool Command68(uint32_t a, uint32_t offset, const void* src, uint32_t size);          // 0x102B7DB0
    bool Command6A(uint32_t a, uint32_t b);                              // 0x102B7A90
    bool Command6B(uint32_t a, uint32_t b, uint32_t c, std::vector<uint32_t>& out);        // 0x102B7B70
    bool Command6C(uint32_t a, uint32_t b, std::string& out);           // 0x102B7C00
    bool Command73(uint32_t a, uint32_t b, const void* src, uint32_t size);                // 0x102B7BB0
    // Batched key queries (FUN_102b35a0). `out` = u32 total count + the concatenated reply payloads.
    bool Command6D(uint32_t a, uint32_t c, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out,
                   const std::vector<uint32_t>& extra);                                    // 0x102B7CB0
    bool Command6E(uint32_t a, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out);          // 0x102B7C40
    bool Command6F(uint32_t a, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out,
                   const std::vector<uint32_t>& extra);                                    // 0x102B7C70
}
