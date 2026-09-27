// C_ClassInfo.GetAllSpecs (FUN_100d8800) / GetSpecInfo (FUN_100dac60) over the DLL's ChrSpecs.dbc.
//
// Row (layout d x29, L, L, d, d): +0 id, +4 class, +8 spec, +0xC SpecFilename, +0x10..+0x1C
// Cloth/Leather/Mail/Plate, +0x20..+0x28 primary stats, +0x2C..+0x40 MeleeDPS/RangedDPS/CasterDPS/
// Tank/Healer/Support, +0x44 DifficultyRating, +0x48..+0x50 primary resources, +0x54..+0x5C
// secondary resources, +0x60..+0x68 example spells, +0x6C PassiveSpell, +0x70 PassiveID, +0x74 Name,
// +0x78 Description, +0x7C SortOrder, +0x80 LootSpecID. Class/spec match is exact (case-sensitive).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    AscDbc::Table& Specs() { return AscDbc::Get("DBFilesClient\\ChrSpecs.dbc"); }

    int GetAllSpecs(lua_State* L)
    {
        std::string cls;
        if (!ReadString(L, cls))
            return 0;
        std::vector<const char*> specs;
        AscDbc::Table& t = Specs();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* row = t.Row(id))
                if (cls == t.Str(row, 4))
                    specs.push_back(t.Str(row, 8));
        AscLua::lua_createtable(L, 0, static_cast<int>(specs.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < specs.size(); ++i)
        {
            PushNum(L, i + 1);
            PushStr(L, specs[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    void SetStr(lua_State* L, const char* k, const char* v)
    {
        AscLua::lua_pushstring(L, k);
        PushStr(L, v);
        AscLua::lua_settable(L, -3);
    }
    void SetBool(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushstring(L, k);
        PushBool(L, v);
        AscLua::lua_settable(L, -3);
    }
    void SetNum(lua_State* L, const char* k, int32_t v)
    {
        AscLua::lua_pushstring(L, k);
        PushNum(L, v);
        AscLua::lua_settable(L, -3);
    }

    // Three string columns as an array, with every entry equal to `none` removed (FUN_100d12d0).
    void SetStrList(lua_State* L, AscDbc::Table& t, const uint8_t* row, const char* k, uint32_t off, const char* none)
    {
        std::vector<std::string> v;
        for (uint32_t i = 0; i < 3; ++i)
            if (std::string s = t.Str(row, off + i * 4); s != none)
                v.push_back(s);
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, i + 1);
            PushStr(L, v[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    int GetSpecInfo(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return 0;
        const std::string cls = CheckString(L, 1);
        const std::string spec = CheckString(L, 2);
        AscDbc::Table& t = Specs();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (!row || cls != t.Str(row, 4) || spec != t.Str(row, 8))
                continue;
            auto I = [&](uint32_t off) { return AscDbc::Table::I32(row, off); };
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetNum(L, "ID", I(0));
            SetStr(L, "Class", t.Str(row, 4));
            SetStr(L, "Spec", t.Str(row, 8));
            SetStr(L, "SpecFilename", t.Str(row, 0xC));
            SetBool(L, "Cloth", I(0x10) != 0);
            SetBool(L, "Leather", I(0x14) != 0);
            SetBool(L, "Mail", I(0x18) != 0);
            SetBool(L, "Plate", I(0x1C) != 0);
            SetStrList(L, t, row, "PrimaryStats", 0x20, "None");
            SetBool(L, "MeleeDPS", I(0x2C) != 0);
            SetBool(L, "RangedDPS", I(0x30) != 0);
            SetBool(L, "CasterDPS", I(0x34) != 0);
            SetBool(L, "Tank", I(0x38) != 0);
            SetBool(L, "Healer", I(0x3C) != 0);
            SetBool(L, "Support", I(0x40) != 0);
            SetStr(L, "DifficultyRating", t.Str(row, 0x44));
            SetStrList(L, t, row, "PrimaryResources", 0x48, "NONE");
            SetStrList(L, t, row, "SecondaryResources", 0x54, "NONE");
            // FUN_100c0270 + remove(0): the three example spell ids, zeros dropped.
            std::vector<int32_t> spells;
            for (uint32_t off = 0x60; off <= 0x68; off += 4)
                if (I(off) != 0)
                    spells.push_back(I(off));
            AscLua::lua_pushstring(L, "ExampleSpells");
            AscLua::lua_createtable(L, 0, static_cast<int>(spells.size()));
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < spells.size(); ++i)
            {
                PushNum(L, i + 1);
                PushInt(L, spells[i]);
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
            SetStr(L, "Name", t.Str(row, 0x74));
            SetStr(L, "Description", t.Str(row, 0x78));
            SetNum(L, "SortOrder", I(0x7C));
            SetNum(L, "PassiveSpell", I(0x6C));
            SetNum(L, "PassiveID", I(0x70));
            SetNum(L, "LootSpecID", I(0x80));
            return 1;
        }
        // The original pushes nil here and still returns 0.
        AscLua::lua_pushnil(L);
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_ClassInfo", "GetAllSpecs", GetAllSpecs},
        {"C_ClassInfo", "GetSpecInfo", GetSpecInfo},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), nullptr);
}
