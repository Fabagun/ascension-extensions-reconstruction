// C_ClassInfo -- the specialization catalogue the CoA talent panel's spec picker runs on.
//
// Measured from the UI rather than guessed:
//   CoASpecViewMixin:OnLoad   self.class = select(2, UnitClass("player"))
//                             local specs = C_ClassInfo.GetAllSpecs(self.class)
//                             specInfo   = C_ClassInfo.GetSpecInfo(self.class, spec)
//   CoATreeViewMixin:SetSpecID specInfo = C_ClassInfo.GetSpecInfoByID(specID)
//                             specFile = specInfo.Spec  -> GetSpecDBCByFile -> the CA tab
//                             classDBC = GetClassDBCByFile(specInfo.Class)
//                             SpecTree:SetClassTab(classDBC, specDBC)
//
// So GetAllSpecs returns the spec FILES for one class, and the info table must carry ID, Name, Spec
// and Class. Source: ChrSpecs.dbc -- [0] id, [1] class file, [2] spec name, [3] art. Verified:
// NECROMANCER yields DEATH(34), ANIMATION(35), RIME(36), which line up exactly with that class's
// CharacterAdvancement tabs Death(59), Animation(61), Rime(60).
//
// Without this the spec picker had nothing to list and the spec tree had no tab, which is why it
// drew the class tree instead.
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscArgs.hpp>
#include <Client/SFile.hpp>
#include <cstring>
#include <string>
#include <vector>

namespace AscClassInfo
{
namespace
{
    constexpr int GLOBALS = -10002;
    constexpr uint32_t WDBC_MAGIC = 0x43424457;

    struct Spec
    {
        uint32_t id = 0;
        std::string classFile;   // "NECROMANCER"
        std::string name;        // "ANIMATION" -- also the spec file the UI maps to a CA tab
        std::string art;         // icon, used as "Interface\Icons\\" .. SpecFilename
        std::string display;     // "Animation" -- the UI upper()s this for the header
        std::string difficulty;  // "Normal" / "Medium" / "Hard" -> SPEC_COMPLEXITY_<UPPER>
        std::string description;
        std::string primaryStat;
        std::string primaryResource;
        std::string secondaryResource;
        // MEASURED: cols 24/25 hold spell ids that ALSO appear as CharacterAdvancement node spells
        // (70/101 and 65/101) -- those are the example abilities. Col 27 never does (0/101), which
        // is what a spec-granted passive looks like. That asymmetry is what distinguishes them.
        uint32_t exampleSpells[2] = {0, 0};
        uint32_t passiveSpell = 0;
    };

    std::vector<Spec> g_specs;
    bool g_tried = false;

    void Load()
    {
        if (g_tried) return;
        g_tried = true;

        HANDLE file = nullptr;
        if (!SFile::OpenFileEx(nullptr, "DBFilesClient\\ChrSpecs.dbc", 0x20000, &file) || !file)
        {
            AscLog::Printf("ChrSpecs.dbc: not present in the archives");
            return;
        }
        uint32_t header[5] = {};
        if (!SFile::ReadFile(file, header, sizeof(header), nullptr, nullptr, 0) || header[0] != WDBC_MAGIC)
        {
            AscLog::Printf("ChrSpecs.dbc: bad header");
            SFile::CloseFile(file);
            return;
        }
        const uint32_t rows = header[1], rowSize = header[3], strSize = header[4];
        const uint32_t dwords = rowSize / 4;
        if (!rows || dwords < 4 || !strSize)
        {
            SFile::CloseFile(file);
            return;
        }
        std::vector<uint8_t> body(static_cast<size_t>(rows) * rowSize);
        std::vector<char> strs(strSize);
        const bool okRows = SFile::ReadFile(file, body.data(), rows * rowSize, nullptr, nullptr, 0);
        const bool okStrs = SFile::ReadFile(file, strs.data(), strSize, nullptr, nullptr, 0);
        SFile::CloseFile(file);
        if (!okRows || !okStrs) return;

        auto str = [&](uint32_t off) -> std::string
        {
            if (off >= strSize) return std::string();
            return std::string(strs.data() + off);
        };

        for (uint32_t i = 0; i < rows; ++i)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(body.data() + static_cast<size_t>(i) * rowSize);
            Spec s;
            s.id        = row[0];
            s.classFile = str(row[1]);
            s.name      = str(row[2]);
            s.art       = str(row[3]);
            // Column map measured off ChrSpecs.dbc, checked against NECROMANCER's three specs.
            s.primaryStat       = str(row[8]);    // "Intellect"
            s.difficulty        = str(row[17]);   // "Normal"/"Medium"/"Hard", 101/101 rows valid
            s.primaryResource   = str(row[18]);   // "MANA"
            s.secondaryResource = str(row[19]);   // "RUNIC_POWER"
            s.display           = str(row[29]);   // "Animation"
            s.description       = str(row[46]);
            s.exampleSpells[0]  = row[24];
            s.exampleSpells[1]  = row[25];
            s.passiveSpell      = row[27];
            if (s.id && !s.classFile.empty() && !s.name.empty())
                g_specs.push_back(s);
        }
        AscLog::Printf("ChrSpecs.dbc: %u specialization(s) loaded", static_cast<unsigned>(g_specs.size()));
    }

    bool EqualsNoCase(const std::string& a, const std::string& b)
    {
        return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
    }

    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, v.c_str());
        AscLua::lua_setfield(L, -2, k);
    }
    void SetNum(lua_State* L, const char* k, double v)
    {
        AscLua::lua_pushnumber(L, v);
        AscLua::lua_setfield(L, -2, k);
    }
    void SetBool(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushboolean(L, v ? 1 : 0);
        AscLua::lua_setfield(L, -2, k);
    }
    // One-element array, or empty when the DBC says "None".
    void SetList(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_createtable(L, 1, 0);
        if (!v.empty() && _stricmp(v.c_str(), "None") != 0)
        {
            AscLua::lua_pushinteger(L, 1);
            AscLua::lua_pushstring(L, v.c_str());
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_setfield(L, -2, k);
    }

    void SetEmptyTable(lua_State* L, const char* k)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_setfield(L, -2, k);
    }

    // The 20-field contract the ORIGINAL GetSpecInfo binding writes, taken from the decompile
    // rather than inferred from call sites -- which is how SpecFilename was missed. It is the ICON
    // name: CoATalentFrame.lua does "Interface\Icons\\" .. specInfo.SpecFilename, and ChrSpecs.dbc
    // column 3 holds exactly that string. ID/Name/Spec are added on top because the tree view and
    // the spec picker read them.
    void PushSpecInfo(lua_State* L, const Spec& s)
    {
        AscLua::lua_createtable(L, 0, 24);
        SetNum(L, "ID", s.id);
        SetStr(L, "Name", s.display.empty() ? s.name : s.display);
        SetStr(L, "Spec", s.name);
        SetStr(L, "Class", s.classFile);
        SetStr(L, "SpecFilename", s.art);

        // Armour and role classification live in later ChrSpecs columns that are not mapped yet;
        // typed so the UI runs, values still to be sourced.
        SetBool(L, "Cloth", false);
        SetBool(L, "Leather", false);
        SetBool(L, "Plate", false);
        SetBool(L, "MeleeDPS", false);
        SetBool(L, "RangedDPS", false);
        SetBool(L, "CasterDPS", false);
        SetBool(L, "Healer", false);
        SetBool(L, "Support", false);
        // A STRING, not a number: CoASpecChoiceMixin does
        //   _G["SPEC_COMPLEXITY_" .. specInfo.DifficultyRating:upper()]
        // so a number here throws "attempt to index field 'DifficultyRating' (a number value)".
        // Valid values come from the SPEC_COMPLEXITY_* globals: NORMAL, MEDIUM, HARD, VERYHARD,
        // IMPOSSIBLE. ChrSpecs column 17 holds exactly one of them for every row.
        SetStr(L, "DifficultyRating", s.difficulty.empty() ? std::string("NORMAL") : s.difficulty);
        SetNum(L, "SortOrder", s.id);
        SetNum(L, "PassiveSpell", s.passiveSpell);
        SetNum(L, "PassiveID", 0);
        SetNum(L, "LootSpecID", 0);
        SetStr(L, "Description", s.description);
        // These are iterated with ipairs, so they must be arrays. "None"/"NONE" means absent.
        SetList(L, "PrimaryStats", s.primaryStat);
        SetList(L, "PrimaryResources", s.primaryResource);
        SetList(L, "SecondaryResources", s.secondaryResource);
        // CoASpecChoiceMixin ipairs these and calls icon:SetSpell(id), so the array has to hold the
        // real ids or the sample-ability row renders empty.
        AscLua::lua_createtable(L, 2, 0);
        int ex = 1;
        for (uint32_t id : s.exampleSpells)
        {
            if (!id) continue;
            AscLua::lua_pushinteger(L, ex++);
            AscLua::lua_pushinteger(L, static_cast<int>(id));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_setfield(L, -2, "ExampleSpells");
    }

    // GetSpecInfoByID(specID). CoATreeViewMixin indexes the result immediately and calls
    // specInfo.Name:upper(), so a nil here kills the tree view -- fall back to an empty-but-typed
    // table rather than nil if the id is unknown.
    int GetSpecInfoByID(lua_State* L)
    {
        Load();
        int id = 0;
        AscArgs::OptInt(L, 1, id, 0);
        for (const auto& s : g_specs)
            if (static_cast<int>(s.id) == id)
            {
                PushSpecInfo(L, s);
                return 1;
            }
        Spec empty;
        empty.id = static_cast<uint32_t>(id);
        PushSpecInfo(L, empty);
        return 1;
    }

    // GetAllSpecs / GetSpecInfo are transcribed exactly in AscSpecs.cpp. GetSpecInfoByID is NOT an
    // original binding (absent from the DLL's namespace tables) -- kept only until whatever calls it
    // is traced.
    const AscLua::Reg REGS[] = {
        {"GetSpecInfoByID", GetSpecInfoByID},
    };
}

unsigned int SpecIdByName(const char* name)
{
    Load();
    if (!name || !*name) return 0;
    for (const auto& s : g_specs)
        if (_stricmp(s.name.c_str(), name) == 0) return s.id;
    return 0;
}

void Register(lua_State* L)
{
    if (!L) return;
    int top = AscLua::lua_gettop(L);
    AscLua::lua_getfield(L, GLOBALS, "C_ClassInfo");
    if (AscLua::lua_type(L, -1) != LUA_TTABLE)
    {
        AscLua::lua_settop(L, top);
        AscLua::lua_createtable(L, 0, 8);
    }
    for (const auto& r : REGS)
    {
        AscLua::lua_pushcclosure(L, r.fn, 0);
        AscLua::lua_setfield(L, -2, r.name);
    }
    AscLua::lua_setfield(L, GLOBALS, "C_ClassInfo");
    AscLua::lua_settop(L, top);
    AscLog::Printf("C_ClassInfo: %u real natives registered",
                   static_cast<unsigned>(sizeof(REGS) / sizeof(REGS[0])));
}
}
