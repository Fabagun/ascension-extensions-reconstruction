#pragma once
// The original DLL's custom-DBC tables (its own Addin/WowClientDB.h, a copy of Blizzard's).
//
// FUN_101e7c80 and its ~100 per-table siblings open "DBFilesClient/<Name>.dbc" through the client's
// SFile (0x424B50 open, 0x422530 read, 0x422910 close), check WDBC / column count / row size, then
// FUN_101feb50 keeps each row exactly as it is in the file, indexed by the id in its first dword:
//
//   container +0x08 records  +0x0C maxId  +0x10 minId  +0x14 string block  +0x1C rows  +0x20 index
//   row = index[id - minId]
//
// Bindings read rows at fixed byte offsets and treat string columns as char* (resolved at load).
// Here a string column is resolved when read, from the same block -- same result, no per-table
// column list needed. A NULL/absent string reads as "" (DAT_10b1b240).
//
// Tables load on first use rather than at DLL attach; nothing observable depends on the timing.
#include <cstdint>
#include <string>
#include <vector>

namespace AscDbc
{
    class Table
    {
    public:
        explicit Table(const char* path);

        const uint8_t* Row(uint32_t id);                         // nullptr if absent
        uint32_t Count();                                        // +0x08
        const uint8_t* RowAt(uint32_t i);                        // i-th record in file order
        uint32_t MinId() { Load(); return m_min; }
        uint32_t MaxId() { Load(); return m_max; }
        bool Loaded() { Load(); return m_ok; }

        static uint32_t U32(const uint8_t* row, uint32_t off) { return *reinterpret_cast<const uint32_t*>(row + off); }
        static int32_t I32(const uint8_t* row, uint32_t off)  { return *reinterpret_cast<const int32_t*>(row + off); }
        static float F32(const uint8_t* row, uint32_t off)    { return *reinterpret_cast<const float*>(row + off); }
        const char* Str(const uint8_t* row, uint32_t off);

        // FUN_10133ac0: point id at a caller-supplied row (the original's runtime DBC patching from
        // server packets). The row is copied and owned here; strings go through AddString.
        void Upsert(uint32_t id, const std::vector<uint8_t>& row);
        uint32_t AddString(const char* s);   // offset usable in a row's string column
        uint32_t RowSize() { Load(); return m_rowSize; }

    private:
        void Load();

        std::string m_path;
        bool m_tried = false, m_ok = false, m_openLogged = false;
        uint32_t m_count = 0, m_rowSize = 0, m_min = 0, m_max = 0;
        std::vector<uint8_t> m_rows;
        std::vector<char> m_strings;
        std::vector<const uint8_t*> m_index;
        std::vector<std::vector<uint8_t>*> m_patched;
    };

    // One instance per file for the life of the process, like the DLL's static tables.
    Table& Get(const char* path);
}
