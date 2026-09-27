// C_CharacterAdvancement.GetAllEntries, built from DBFilesClient\\CharacterAdvancement.dbc.
#include <Ascension/AscCharacterAdvancement.hpp>
#include <cstring>
#include <map>
#include <string>
#include <utility>
//
// PARTIAL BY DESIGN, and the user chose this trade deliberately: an empty tree is useless, so we
// emit the columns that are authoritatively known and omit the rest rather than guess at them.
//
// The column map comes from the SERVER, which reads the same DBC --
// modules/mod-ascension-compat/src/AscensionCoATalentData.cpp:
//     ADVANCEMENT_ID = 0, ADVANCEMENT_REQUIRED = 2 (x3), ADVANCEMENT_SPELLS = 5 (x5),
//     ADVANCEMENT_AE_COST = 14, ADVANCEMENT_TE_COST = 15, ADVANCEMENT_LEVEL = 26,
//     ADVANCEMENT_GROUP = 29, ADVANCEMENT_CLASS_TYPE = 32, ADVANCEMENT_TAB = 33
// (note LEVEL is 26, not 28 as an older note recorded).
//
// What the Lua also reads but nothing maps: Flags, Type, ParentNode, Distance, Color, Anchor,
// NodeType, DraftRequired_XOR, PositionX/Y, Masteries. Those keys are simply absent. The consumer
// tolerates that -- FrameXML\Data\CharacterAdvancement.lua does `data.Flags = data.Flags or 0` and
// `data.Class = data.Class and ClassRemap[data.Class]` -- so entries carry real ids, spell chains,
// levels and tabs, but no layout geometry. Adding the missing columns means finding their indices;
// the client's own serializer FUN_10172DF0 is reflection-style and does not name them.
//
// `Class` is left numeric (the class-type id). The Lua remaps it through ClassRemap, which is keyed
// by NAME, so it resolves to nil rather than to a wrong class -- deliberately, since inventing a
// name-to-id mapping here would be a guess.

#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/SFile.hpp>

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace AscLua;

namespace
{
    const uint32_t WDBC_MAGIC = 0x43424457;

    enum : uint32_t
    {
        COL_ID          = 0,
        COL_REQUIRED    = 2,    // x3
        COL_SPELLS      = 5,    // x5
        COL_AE_COST     = 14,
        COL_TE_COST     = 15,
        COL_LEVEL       = 26,
        COL_GROUP       = 29,
        COL_CLASS_TYPE  = 32,
        COL_TAB         = 33,
        REQUIRED_COUNT  = 3,
        SPELL_COUNT     = 5,
        MIN_FIELDS      = COL_TAB + 1,
        // Unaligned float byte offsets within the record; see AscCAEntry.
        OFF_POS_X       = 398,
        OFF_POS_Y       = 402,
        OFF_SIZE_X      = 406,
        OFF_SIZE_Y      = 410,
        OFF_CONNECTED   = 415,   // u32 x5, stride 4
        CONNECTED_COUNT = 5,
    };

    std::vector<uint8_t> g_rows;
    uint32_t g_recordCount = 0;
    uint32_t g_recordSize  = 0;
    uint32_t g_fieldCount  = 0;
    bool     g_loaded      = false;
    bool     g_tried       = false;

    uint32_t Field(const uint8_t* row, uint32_t index)
    {
        return *reinterpret_cast<const uint32_t*>(row + index * 4);
    }

    // Unaligned read: these fields do not sit on a dword boundary.
    float FloatAt(const uint8_t* row, uint32_t byteOffset)
    {
        float v = 0.0f;
        memcpy(&v, row + byteOffset, sizeof(v));
        return v;
    }

    void ReadLayout(const uint8_t* row, AscCAEntry& e, uint32_t recordSize)
    {
        if (recordSize < OFF_SIZE_Y + 4)
            return;
        e.posX  = FloatAt(row, OFF_POS_X);
        e.posY  = FloatAt(row, OFF_POS_Y);
        e.sizeX = FloatAt(row, OFF_SIZE_X);
        e.sizeY = FloatAt(row, OFF_SIZE_Y);
        for (uint32_t k = 0; k < CONNECTED_COUNT; ++k)
        {
            const uint32_t at = OFF_CONNECTED + k * 4;
            if (at + 4 > recordSize) break;
            uint32_t v = 0;
            memcpy(&v, row + at, sizeof(v));
            e.connected[k] = v;
        }
    }

    void Load()
    {
        if (g_tried)
            return;
        g_tried = true;

        HANDLE file = nullptr;
        if (!SFile::OpenFileEx(nullptr, "DBFilesClient\\CharacterAdvancement.dbc", 0x20000, &file) || !file)
        {
            AscLog::Printf("CharacterAdvancement.dbc: not present in the archives");
            return;
        }

        uint32_t header[5] = {};
        if (!SFile::ReadFile(file, header, sizeof(header), nullptr, nullptr, 0) || header[0] != WDBC_MAGIC)
        {
            AscLog::Printf("CharacterAdvancement.dbc: bad header (magic 0x%08X)", header[0]);
            SFile::CloseFile(file);
            return;
        }

        g_recordCount = header[1];
        g_fieldCount  = header[2];
        g_recordSize  = header[3];

        // Measured 2026-09-23: this DBC reports 179 fields but a 692-byte row, i.e. 173 dwords.
        // The header's field count and the row size disagree, so trust the row size -- it bounds
        // what we can actually index without reading past the record.
        const uint32_t usableFields = g_recordSize / 4;
        if (!g_recordCount || usableFields < MIN_FIELDS)
        {
            AscLog::Printf("CharacterAdvancement.dbc: unexpected shape (%u rows, rowSize %u => %u usable "
                           "fields, header claims %u; need at least %u), NOT loaded",
                           g_recordCount, g_recordSize, usableFields, g_fieldCount, (uint32_t)MIN_FIELDS);
            SFile::CloseFile(file);
            return;
        }
        g_fieldCount = usableFields;

        g_rows.resize((size_t)g_recordCount * g_recordSize);
        const bool ok = SFile::ReadFile(file, g_rows.data(), g_recordCount * g_recordSize, nullptr, nullptr, 0);
        SFile::CloseFile(file);

        if (!ok)
        {
            AscLog::Printf("CharacterAdvancement.dbc: read failed");
            g_rows.clear();
            return;
        }
        g_loaded = true;
        AscLog::Printf("CharacterAdvancement.dbc: %u rows x %u fields loaded", g_recordCount, g_fieldCount);
    }

    void SetNumber(lua_State* L, const char* key, double v)
    {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, key);
    }
}

// Returns an array of entry tables. Empty (not nil) if the DBC is missing or malformed -- the Lua
// does `ipairs(GetAllEntries())` and nil there aborts the whole file.
int AscCA_GetAllEntries(lua_State* L)
{
    Load();

    lua_createtable(L, g_loaded ? (int)g_recordCount : 0, 0);
    if (!g_loaded)
        return 1;

    int emitted = 0;
    for (uint32_t r = 0; r < g_recordCount; ++r)
    {
        const uint8_t* row = g_rows.data() + (size_t)r * g_recordSize;

        lua_pushinteger(L, emitted + 1);
        lua_createtable(L, 0, 8);

        SetNumber(L, "ID",        (double)Field(row, COL_ID));
        SetNumber(L, "Level",     (double)Field(row, COL_LEVEL));
        SetNumber(L, "Group",     (double)Field(row, COL_GROUP));
        SetNumber(L, "Class",     (double)Field(row, COL_CLASS_TYPE));
        SetNumber(L, "Tab",       (double)Field(row, COL_TAB));
        SetNumber(L, "CostAE",    (double)Field(row, COL_AE_COST));
        SetNumber(L, "CostTE",    (double)Field(row, COL_TE_COST));

        lua_createtable(L, SPELL_COUNT, 0);
        int n = 0;
        for (uint32_t i = 0; i < SPELL_COUNT; ++i)
        {
            const uint32_t spell = Field(row, COL_SPELLS + i);
            if (!spell)
                continue;
            lua_pushinteger(L, ++n);
            lua_pushnumber(L, (double)spell);
            lua_settable(L, -3);
        }
        lua_setfield(L, -2, "Spells");

        lua_createtable(L, REQUIRED_COUNT, 0);
        n = 0;
        for (uint32_t i = 0; i < REQUIRED_COUNT; ++i)
        {
            const uint32_t req = Field(row, COL_REQUIRED + i);
            if (!req)
                continue;
            lua_pushinteger(L, ++n);
            lua_pushnumber(L, (double)req);
            lua_settable(L, -3);
        }
        lua_setfield(L, -2, "RequiredIDs");

        lua_settable(L, -3);
        ++emitted;
    }

    AscLog::Printf("C_CharacterAdvancement.GetAllEntries: %d entries (partial columns)", emitted);
    return 1;
}

// Row lookup for the C_CharacterAdvancement bindings. Same loaded table GetAllEntries walks, so a
// caller never has to re-open the DBC.
// Row offset by entry id, built once. The linear scan this replaces ran over all 10,255 rows on
// every call, and the callers are per-node UI paths -- measured 2026-09-23: the client froze about
// 28s into a session once the talent tree started building.
static std::map<uint32_t, uint32_t> g_byId;

static void BuildIndex()
{
    if (!g_byId.empty() || !g_loaded)
        return;
    for (uint32_t i = 0; i < g_recordCount; ++i)
    {
        const uint8_t* row = g_rows.data() + static_cast<size_t>(i) * g_recordSize;
        g_byId.emplace(Field(row, COL_ID), i);
    }
}

bool AscCA_FindEntry(uint32_t id, AscCAEntry& out)
{
    Load();
    if (!g_loaded || g_fieldCount < MIN_FIELDS)
        return false;
    BuildIndex();
    auto it = g_byId.find(id);
    if (it != g_byId.end())
    {
        const uint8_t* row = g_rows.data() + static_cast<size_t>(it->second) * g_recordSize;
        out.id        = id;
        out.aeCost    = Field(row, COL_AE_COST);
        out.teCost    = Field(row, COL_TE_COST);
        out.level     = Field(row, COL_LEVEL);
        out.group     = Field(row, COL_GROUP);
        out.classType = Field(row, COL_CLASS_TYPE);
        out.tab       = Field(row, COL_TAB);
        for (uint32_t k = 0; k < SPELL_COUNT; ++k)
            out.spells[k] = Field(row, COL_SPELLS + k);
        for (uint32_t k = 0; k < REQUIRED_COUNT; ++k)
            out.required[k] = Field(row, COL_REQUIRED + k);
        ReadLayout(row, out, g_recordSize);
        return true;
    }
    return false;
}

uint32_t AscCA_EntryCount()
{
    Load();
    return g_loaded ? g_recordCount : 0;
}

// Cached: the talent tree asks for the same class/tab set repeatedly while it lays out, and each
// miss is a full 10,255-row walk.
static std::map<std::pair<uint32_t, int>, std::vector<AscCAEntry>> g_byClassTab;

void AscCA_CollectByClassTab(uint32_t classType, int tab, std::vector<AscCAEntry>& out)
{
    auto cached = g_byClassTab.find({classType, tab});
    if (cached != g_byClassTab.end())
    {
        out = cached->second;
        return;
    }
    out.clear();
    Load();
    if (!g_loaded || g_fieldCount < MIN_FIELDS)
        return;
    for (uint32_t i = 0; i < g_recordCount; ++i)
    {
        const uint8_t* row = g_rows.data() + static_cast<size_t>(i) * g_recordSize;
        if (classType && Field(row, COL_CLASS_TYPE) != classType)
            continue;
        if (tab >= 0 && Field(row, COL_TAB) != static_cast<uint32_t>(tab))
            continue;
        AscCAEntry e;
        e.id        = Field(row, COL_ID);
        e.aeCost    = Field(row, COL_AE_COST);
        e.teCost    = Field(row, COL_TE_COST);
        e.level     = Field(row, COL_LEVEL);
        e.group     = Field(row, COL_GROUP);
        e.classType = Field(row, COL_CLASS_TYPE);
        e.tab       = Field(row, COL_TAB);
        for (uint32_t k = 0; k < SPELL_COUNT; ++k)
            e.spells[k] = Field(row, COL_SPELLS + k);
        for (uint32_t k = 0; k < REQUIRED_COUNT; ++k)
            e.required[k] = Field(row, COL_REQUIRED + k);
        ReadLayout(row, e, g_recordSize);
        out.push_back(e);
    }
    g_byClassTab.emplace(std::make_pair(classType, tab), out);
}

namespace
{
    // Generic [0]=id, [1]=name lookup over a small DBC, including its trailing string block.
    bool LoadNameTable(const char* path, std::map<std::string, uint32_t>& out)
    {
        HANDLE file = nullptr;
        if (!SFile::OpenFileEx(nullptr, path, 0x20000, &file) || !file)
        {
            AscLog::Printf("%s: not present in the archives", path);
            return false;
        }
        uint32_t header[5] = {};
        if (!SFile::ReadFile(file, header, sizeof(header), nullptr, nullptr, 0) || header[0] != WDBC_MAGIC)
        {
            SFile::CloseFile(file);
            return false;
        }
        const uint32_t rows = header[1], rowSize = header[3], strSize = header[4];
        const uint32_t dwords = rowSize / 4;
        if (!rows || dwords < 2 || !strSize)
        {
            SFile::CloseFile(file);
            return false;
        }
        std::vector<uint8_t> body(static_cast<size_t>(rows) * rowSize);
        std::vector<uint8_t> strs(strSize);
        const bool okRows = SFile::ReadFile(file, body.data(), rows * rowSize, nullptr, nullptr, 0);
        const bool okStrs = SFile::ReadFile(file, strs.data(), strSize, nullptr, nullptr, 0);
        SFile::CloseFile(file);
        if (!okRows || !okStrs)
            return false;

        for (uint32_t i = 0; i < rows; ++i)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(body.data() + static_cast<size_t>(i) * rowSize);
            const uint32_t id = row[0], off = row[1];
            if (!id || off >= strSize)
                continue;
            const char* name = reinterpret_cast<const char*>(strs.data() + off);
            if (*name)
                out.emplace(name, id);
        }
        AscLog::Printf("%s: %u name(s) indexed", path, static_cast<unsigned>(out.size()));
        return true;
    }

    std::map<std::string, uint32_t> g_classIds, g_tabIds;
    bool g_namesTried = false;

    void LoadNames()
    {
        if (g_namesTried)
            return;
        g_namesTried = true;
        LoadNameTable("DBFilesClient\\CharacterAdvancementClassTypes.dbc", g_classIds);
        LoadNameTable("DBFilesClient\\CharacterAdvancementTabTypes.dbc", g_tabIds);
    }

    uint32_t Lookup(const std::map<std::string, uint32_t>& m, const char* name)
    {
        if (!name || !*name)
            return 0;
        auto it = m.find(name);
        if (it != m.end()) return it->second;
        // The UI passes the class FILE ("NECROMANCER"); the DBC stores "Necromancer".
        for (const auto& kv : m)
            if (_stricmp(kv.first.c_str(), name) == 0) return kv.second;
        return 0;
    }
}

uint32_t AscCA_ClassIdByName(const char* name)
{
    LoadNames();
    return Lookup(g_classIds, name);
}

uint32_t AscCA_TabIdByName(const char* name)
{
    LoadNames();
    return Lookup(g_tabIds, name);
}

namespace
{
    struct EssenceRow { uint32_t level, cls, ae, te; };
    std::vector<EssenceRow> g_essence;
    bool g_essenceTried = false;

    void LoadEssence()
    {
        if (g_essenceTried) return;
        g_essenceTried = true;
        HANDLE file = nullptr;
        if (!SFile::OpenFileEx(nullptr, "DBFilesClient\\CharacterAdvancementEssence.dbc", 0x20000, &file) || !file)
        {
            AscLog::Printf("CharacterAdvancementEssence.dbc: not present in the archives");
            return;
        }
        uint32_t header[5] = {};
        if (!SFile::ReadFile(file, header, sizeof(header), nullptr, nullptr, 0) || header[0] != WDBC_MAGIC)
        {
            SFile::CloseFile(file);
            return;
        }
        const uint32_t rows = header[1], rowSize = header[3];
        if (!rows || rowSize / 4 < 9) { SFile::CloseFile(file); return; }
        std::vector<uint8_t> body(static_cast<size_t>(rows) * rowSize);
        const bool ok = SFile::ReadFile(file, body.data(), rows * rowSize, nullptr, nullptr, 0);
        SFile::CloseFile(file);
        if (!ok) return;
        for (uint32_t i = 0; i < rows; ++i)
        {
            const uint32_t* r = reinterpret_cast<const uint32_t*>(body.data() + static_cast<size_t>(i) * rowSize);
            g_essence.push_back({ r[1], r[2], r[7], r[8] });
        }
        AscLog::Printf("CharacterAdvancementEssence.dbc: %u budget row(s) loaded",
                       static_cast<unsigned>(g_essence.size()));
    }
}

bool AscCA_EssenceForLevel(uint32_t classType, uint32_t level, uint32_t& ae, uint32_t& te)
{
    LoadEssence();
    for (const auto& r : g_essence)
        if (r.cls == classType && r.level == level)
        {
            ae = r.ae;
            te = r.te;
            return true;
        }
    return false;
}

const char* AscCA_TabNameById(uint32_t id)
{
    LoadNames();
    for (const auto& kv : g_tabIds)
        if (kv.second == id) return kv.first.c_str();
    return "";
}
