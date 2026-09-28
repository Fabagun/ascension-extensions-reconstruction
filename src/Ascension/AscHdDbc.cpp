// The HD model tables (FUN_101d3d20, 0x101D9251..0x101D9BA3, run from the 0x634E00 set -- see
// AscBridgeStore.cpp). When Data\Patch-Q.mpq opens and the command line does not say "-hd 0", ten
// DBFilesClient\HD*.dbc files load into the DLL's own WowClientDB stores (FUN_101f3b50-style loaders over
// FUN_10204750-style readers; stores 0x10BDFB04..0x10BDFC48), then each is written over the client's rows
// (FUN_1020cae0-style appliers). Nothing else reads the stores.
//
//   Loader: WDBC signature, record count (kept once read, even when a later check fails), the expected
//   field count and row size, string block size; rows are read field by field, and every string column
//   becomes a pointer into the store's string block (null when the block is), which is read last and
//   never freed. The original logs each failure; its id index (+0x20) is built but unused.
//   Appliers: the client row with the HD row's id is overwritten whole, string pointers included.
//   CharacterFacialHairStyles has no id: its HD file prepends one, and every client row whose (race, sex,
//   variation) matches HD fields 1..3 takes HD fields 4..8 as its five geosets.
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <share.h>

namespace
{
    typedef int(__stdcall* SFileOpen_t)(void*, const char*, uint32_t, void**);
    typedef int(__stdcall* SFileRead_t)(void*, void*, uint32_t, uint32_t*, void*, uint32_t);
    typedef int(__stdcall* SFileClose_t)(void*);
    bool Read(void* h, void* buf, uint32_t n) { return reinterpret_cast<SFileRead_t>(0x422530)(h, buf, n, nullptr, nullptr, 0) != 0; }

    struct Store
    {
        uint32_t count;     // +0x08
        char* strings;      // +0x14
        uint8_t* rows;      // +0x1C
    };

    struct Table
    {
        const char* path;
        uint32_t fields, rowSize;
        uint32_t container;                         // the client's WowClientDB
        std::initializer_list<uint32_t> strings;    // string columns, by offset
        Store store;
    };

    Table g_tables[] = {
        {"DBFilesClient\\HDCreatureDisplayInfo.dbc", 0x10, 0x40, 0xAD34B8, {0x18, 0x1C, 0x20, 0x24}},     // 0x10BDFB70
        {"DBFilesClient\\HDCreatureDisplayInfoExtra.dbc", 0x15, 0x54, 0xAD3494, {0x50}},                  // 0x10BDFB94
        {"DBFilesClient\\HDCreatureModelData.dbc", 0x1C, 0x70, 0xAD3500, {0x8}},                          // 0x10BDFBB8
        {"DBFilesClient\\HDSpellVisualEffectName.dbc", 0x7, 0x1C, 0xAD4A18, {0x4, 0x8}},                  // 0x10BDFC24
        {"DBFilesClient\\HDSpellVisualKitModelAttach.dbc", 0xA, 0x28, 0xAD4A84, {}},                      // 0x10BDFC48
        {"DBFilesClient\\HDCharacterFacialHairStyles.dbc", 0x9, 0x24, 0xAD3398, {}},                      // 0x10BDFB04
        {"DBFilesClient\\HDCharHairGeosets.dbc", 0x6, 0x18, 0xAD3308, {}},                                // 0x10BDFB28
        {"DBFilesClient\\HDCharSections.dbc", 0xA, 0x28, 0xAD332C, {0x10, 0x14, 0x18}},                   // 0x10BDFB4C
        {"DBFilesClient\\HDEmotesTextSound.dbc", 0x5, 0x14, 0xAD37AC, {}},                                // 0x10BDFBDC
        {"DBFilesClient\\HDHelmetGeosetVisData.dbc", 0x8, 0x20, 0xAD3CBC, {}},                            // 0x10BDFC00
    };
    const uint32_t kFacialHair = 5;

    // FUN_101f3b50-style.
    void Load(Table& t)
    {
        void* h = nullptr;
        if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, t.path, 0x20000, &h))
            return;
        // Only the no-records and success paths close the file; every other failure leaves it open.
        Store& s = t.store;
        uint32_t magic = 0, fields = 0, rowSize = 0, stringSize = 0;
        if (!Read(h, &magic, 4) || magic != 0x43424457 || !Read(h, &s.count, 4))
            return;
        if (s.count == 0)
        {
            reinterpret_cast<SFileClose_t>(0x422910)(h);
            return;
        }
        if (!Read(h, &fields, 4) || fields != t.fields || !Read(h, &rowSize, 4) || rowSize != t.rowSize || !Read(h, &stringSize, 4))
            return;
        s.strings = static_cast<char*>(malloc(stringSize));
        s.rows = static_cast<uint8_t*>(malloc(s.count * t.rowSize));
        for (uint32_t i = 0; i < s.count; ++i)
        {
            uint8_t* row = s.rows + i * t.rowSize;
            for (uint32_t f = 0; f < t.fields; ++f)
                Read(h, row + f * 4, 4);
            for (uint32_t off : t.strings)
            {
                uint32_t& slot = *reinterpret_cast<uint32_t*>(row + off);
                slot = s.strings ? reinterpret_cast<uint32_t>(s.strings) + slot : 0;
            }
        }
        if (!Read(h, s.strings, stringSize))
            return;
        reinterpret_cast<SFileClose_t>(0x422910)(h);
    }

    // FUN_1020cae0-style. A store whose file failed after its record count has rows == null and is read
    // anyway, as the original (IMPROVEMENTS.md).
    void Apply(const Table& t)
    {
        for (uint32_t i = 0; i < t.store.count; ++i)
        {
            const uint8_t* src = t.store.rows + i * t.rowSize;
            if (uint8_t* dst = AscClientDbc::Row(t.container, *reinterpret_cast<const uint32_t*>(src)))
                memcpy(dst, src, t.rowSize);
        }
    }

    // FUN_1020c940: CharacterFacialHairStyles (container 0xAD3398: +8 rows, +0x20 the row pointers).
    void ApplyFacialHair(const Table& t)
    {
        for (uint32_t i = 0; i < t.store.count; ++i)
        {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(t.store.rows + i * t.rowSize);
            for (uint32_t j = 0; j < *reinterpret_cast<const uint32_t*>(0xAD33A0); ++j)
            {
                uint32_t** index = *reinterpret_cast<uint32_t***>(0xAD33B8);
                uint32_t* dst = index ? index[j] : nullptr;
                if (dst && dst[0] == src[1] && dst[1] == src[2] && dst[2] == src[3])
                    memcpy(dst + 3, src + 4, 5 * 4);
            }
        }
    }

    // 0x101D9251: Data\Patch-Q.mpq opens (std::ifstream, _SH_DENYNO) and "-hd" is not "0".
    bool Enabled()
    {
        {
            std::ifstream probe("Data\\Patch-Q.mpq", std::ios::in, _SH_DENYNO);
            if (!probe.is_open())
                return false;
        }
        const char* hd = AscScript::CommandLineArg("hd");
        return !(hd && strcmp(hd, "0") == 0);
    }
}

namespace AscHdDbc
{
    void Apply()
    {
        if (!Enabled())
            return;
        for (Table& t : g_tables)
            Load(t);
        for (uint32_t i = 0; i < sizeof(g_tables) / sizeof(g_tables[0]); ++i)
            if (i == kFacialHair)
                ApplyFacialHair(g_tables[i]);
            else
                ::Apply(g_tables[i]);
    }
}
