// SMSG_PATCH_* handlers that AscDbcPatch.generated.inc does not cover: registered outside the
// original's FUN_101e5880, missing from the decompile, or carrying a side effect. Each was read by
// hand; all copy the raw wire row (id first) and upsert it (FUN_10133ac0):
//   0x568 DUNGEON_ENCOUNTER_EXTRA  (no decompile; code sits in FUN_101e14f0's tail) 0x10
//   0x681 ZONE_LIGHT_POINT         FUN_1021a8a0                                      0x14
//   0x68C MANASTORM_PLAYER_GROUP_MODIFIERS FUN_102a3ff0                              0x14
//   0x68D MANASTORM_MODIFIERS      FUN_102a3e60                                      0x3C
//   0x693 ITEM_APPEARANCES         handler_0x0693, then refreshes the appearance managers
//                                  (FUN_1020fc80, FUN_101b5660) -- hook them in via the after-
//                                  callback when the appearance collection is transcribed.
//   0x579..0x57B LFG_*           struct image + u32-length strings (AscDbcPatch::RegisterSized)
//   0x686 SPELL_ADDON              FUN_101e36b0 (0x5C): an existing row is overwritten in place and
//                                  nothing else happens; a new row is upserted and ALSO indexed by
//                                  its +4 (the spell) through FUN_101da400 -- AscPatchData_SpellAddonForSpell().
// Still hand-work in this family: 0x567 creature (store), 0x56A item display collections, 0x56B
// mystic enchant, 0x573 vanity, 0x574 spell affect,
// 0x591 challenge, 0x692 appearances. (0x680 zone light: AscZoneLight.)
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscCrashContext.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <Ascension/AscLog.hpp>
#include <map>
#include <unordered_map>
#include <vector>

void AscAppearance_ItemAppearancesPatched();
void AscCAFilter_TagTypesPatched();
void AscAppearance_RebuildOutfits();
void AscCharacterCreate_ArchetypesPatched();
void AscAppearance_ItemSetAppearancesPatched();

void AscGeosets_Rebuild(const uint8_t*);   // AscGeosets.cpp (FUN_101ceee0)

namespace
{
    // Map 0x10BE0708: spell (+4) -> SpellAddon.dbc row. Built by the DBC manager (FUN_101e7670 over every
    // row in file order, key lambda 0x1020b200 = row +4, a later row replacing an earlier one), then kept
    // current by 0x686 (FUN_101da400). Rows are held by id so an in-place 0x686 overwrite is seen, as the
    // original's row pointers see it. Its miss generator (DAT_10BE074C) is never set.
    std::unordered_map<uint32_t, uint32_t>& SpellAddonIndex()
    {
        static std::unordered_map<uint32_t, uint32_t> m;
        static bool built = false;
        if (!built)
        {
            built = true;
            AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SpellAddon.dbc");
            for (uint32_t i = 0; i < t.Count(); ++i)
                if (const uint8_t* row = t.RowAt(i))
                    m[AscDbc::Table::U32(row, 4)] = AscDbc::Table::U32(row, 0);
        }
        return m;
    }

    void __cdecl OnSpellAddon(void*, uint32_t, uint32_t, CDataStore* p)   // 0x686
    {
        std::vector<uint8_t> row(p->m_buffer + p->m_read, p->m_buffer + p->m_read + 0x5C);
        p->m_read += 0x5C;
        uint32_t id, spell;
        memcpy(&id, row.data(), 4);
        memcpy(&spell, row.data() + 4, 4);
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SpellAddon.dbc");
        if (const uint8_t* live = t.Row(id))
        {
            memcpy(const_cast<uint8_t*>(live), row.data(), row.size());
            return;
        }
        SpellAddonIndex();   // the manager built the index at DBC init, before any packet
        t.Upsert(id, row);
        SpellAddonIndex()[spell] = id;
    }

    // 0x5F4 SPELL_CUSTOM_ATTR (FUN_101e39a0, 0x2C): an existing row is overwritten in place and the
    // spell -> row map rebuilt (FUN_1020b080); a new row is only upserted, so the map misses it until the
    // next rebuild.
    void __cdecl OnSpellCustomAttr(void*, uint32_t, uint32_t, CDataStore* p)
    {
        std::vector<uint8_t> row(p->m_buffer + p->m_read, p->m_buffer + p->m_read + 0x2C);
        p->m_read += 0x2C;
        uint32_t id;
        memcpy(&id, row.data(), 4);
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SpellCustomAttr.dbc");
        if (const uint8_t* live = t.Row(id))
        {
            memcpy(const_cast<uint8_t*>(live), row.data(), row.size());
            AscCA::RebuildSpellCustomAttr();
            return;
        }
        t.Upsert(id, row);
    }

    // Handlers that read their record field by field (U = u32, S = C string) instead of as a struct image:
    // an existing row is overwritten in place, a new one inserted (FUN_10133ac0); nothing else.
    //   0x71F EXTRA_ACTION_BUTTONS           FUN_101e1740 / reader FUN_101d2fe0   UUSSSSUUUU
    //   0x723 CHARACTER_ADVANCEMENT_CLASS_TYPES FUN_101dd3b0                     USUUUUS
    //         (the original keeps +0x18 as a pointer into the packet buffer -- see IMPROVEMENTS)
    std::map<uint32_t, std::pair<const char*, const char*>>& FieldSpecs()
    {
        static std::map<uint32_t, std::pair<const char*, const char*>> m;
        return m;
    }

    void __cdecl OnFieldPatch(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        auto spec = FieldSpecs().find(opcode);
        if (spec == FieldSpecs().end())
            return;
        AscDbc::Table& t = AscDbc::Get(spec->second.first);
        const char* layout = spec->second.second;
        std::vector<uint8_t> row(strlen(layout) * 4);
        for (size_t f = 0; layout[f]; ++f)
        {
            uint32_t v;
            if (layout[f] == 'S')
            {
                const char* str = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
                v = t.AddString(str);
                p->m_read += static_cast<int32_t>(strlen(str) + 1);
            }
            else
            {
                memcpy(&v, p->m_buffer + p->m_read, 4);
                p->m_read += 4;
            }
            memcpy(&row[f * 4], &v, 4);
        }
        uint32_t id;
        memcpy(&id, row.data(), 4);
        if (const uint8_t* live = t.Row(id))
        {
            memcpy(const_cast<uint8_t*>(live), row.data(), row.size());
            return;
        }
        t.Upsert(id, row);
    }

    void RegisterFields(uint32_t opcode, const char* table, const char* layout)
    {
        FieldSpecs()[opcode] = {table, layout};
        sDC.AddPacketHandler(opcode, CNetClientCustomPacket((void*)&OnFieldPatch, nullptr));
    }


    // 0x6C1 SPELL_TAGS (FUN_101e40c0, {id, spell, tag}): an existing row first drops its old tag and the new
    // one is added (FUN_10217510 / FUN_102175b0), then it is overwritten in place; a new row adds its tag and
    // is upserted. Then the CA manager's known tags are rebuilt (FUN_10182ff0) and SPELL_TAGS_CHANGED fires.
    void __cdecl OnSpellTag(void*, uint32_t, uint32_t, CDataStore* p)
    {
        std::vector<uint8_t> row(p->m_buffer + p->m_read, p->m_buffer + p->m_read + 0xC);
        p->m_read += 0xC;
        uint32_t f[3];
        memcpy(f, row.data(), sizeof f);
        AscDbc::Table& t = AscDbc::Get("DBFilesClient/SpellTags.dbc");
        if (const uint8_t* live = t.Row(f[0]))
        {
            AscCA::SpellTagRemove(AscDbc::Table::U32(live, 4), AscDbc::Table::U32(live, 8));
            AscCA::SpellTagAdd(f[1], f[2]);
            memcpy(const_cast<uint8_t*>(live), row.data(), row.size());
        }
        else
        {
            AscCA::SpellTagAdd(f[1], f[2]);
            t.Upsert(f[0], row);
        }
        AscCA::RebuildKnownTags();
        AscRuntime::Signal("SPELL_TAGS_CHANGED");
    }

    // FUN_1020c060: a game object whose display (+0x1A4, id at +4) has a GameObjectDisplayInfoAddon.dbc row
    // naming a texture (+4, non-empty) gets that texture in its model's (+0xB4) slot 2 (0x825260), loaded with
    // the flags CGxTexFlags(3, 1, 1, 0, 0, 0, 1, 0, 0, 0) (0x681BE0) and status 0xAF5718.
    void RefreshGameObjectTexture(uint8_t* go)
    {
        const uint8_t* display = *reinterpret_cast<uint8_t* const*>(go + 0x1A4);
        if (!display)
            return;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient/GameObjectDisplayInfoAddon.dbc");
        const uint8_t* row = t.Row(*reinterpret_cast<const uint32_t*>(display + 4));
        const char* name = row ? t.Str(row, 4) : nullptr;   // our rows keep string-block offsets (AscDbc.hpp)
        if (!name || !*name)
            return;
        uint32_t flagsObj;
        const uint32_t* flags = reinterpret_cast<uint32_t*(__thiscall*)(void*, int, int, int, int, int, int, int, int, int, int)>(0x681BE0)(
            &flagsObj, 3, 1, 1, 0, 0, 0, 1, 0, 0, 0);
        void* tex = reinterpret_cast<void*(__cdecl*)(const char*, uint32_t, void*, int)>(0x4B9760)(name, *flags, reinterpret_cast<void*>(0xAF5718), 0);
        if (tex)
            reinterpret_cast<void(__thiscall*)(void*, int, void*)>(0x825260)(*reinterpret_cast<void**>(go + 0xB4), 2, tex);
    }
    int __cdecl RefreshEachGameObject(uint32_t lo, uint32_t hi, void*)   // LAB_1020c030
    {
        if (uint8_t* go = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 0x20))
            RefreshGameObjectTexture(go);
        return 1;
    }
    // Installer FUN_1020c390's two hooks re-apply the texture after the client builds a game object's model:
    //   0x713F50 CGGameObject_C::ModelLoaded (__thiscall(go, model), ret 4; FUN_1020c100) -- also records the
    //            entry (descriptor +0xC) and the model's CM2Shared name (+0x2C -> +0x3C) for crash reports;
    //   0x712400 (__thiscall(go, a), ret 4; FUN_1020c150).
    typedef int(__fastcall* GoFn_t)(uint8_t*, void*, uint32_t);
    GoFn_t g_713F50 = nullptr, g_712400 = nullptr;
    int __fastcall Hook713F50(uint8_t* go, void* edx, uint32_t model)
    {
        AscCrashContext::g_gameObjectEntry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(go + 8) + 0xC);
        const uint8_t* shared = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(model) + 0x2C);
        AscCrashContext::g_gameObjectModel = shared ? reinterpret_cast<const char*>(shared + 0x3C) : "Unknown (Missing CM2Shared)";
        const int r = g_713F50(go, edx, model);
        RefreshGameObjectTexture(go);
        return r;
    }
    int __fastcall Hook712400(uint8_t* go, void* edx, uint32_t a)
    {
        const int r = g_712400(go, edx, a);
        RefreshGameObjectTexture(go);
        return r;
    }

    // FUN_1020c500 (and the same code inline on the overwrite path): in world, every game object.
    void RefreshGameObjectTextures(const uint8_t*)
    {
        if (reinterpret_cast<void*(__cdecl*)()>(0x4038F0)())
            reinterpret_cast<void(__cdecl*)(int(__cdecl*)(uint32_t, uint32_t, void*), void*)>(0x4D4B30)(&RefreshEachGameObject, nullptr);
    }

    // FUN_1025ad00's tail (SMSG 0x676): the row's flags (+4) pick the Lua states it applies to -- bit 1 in
    // world (0xBD0792), bit 0 at glue -- and there the global key (+8) is set to the value (+0xC) in the
    // current Lua state (0x817DB0; lua_pushstring 0x84E350, lua_setfield 0x84E900 on LUA_GLOBALSINDEX).
    void ApplyGlobalString(const uint8_t* row)
    {
        const uint32_t flags = AscDbc::Table::U32(row, 4);
        const bool inWorld = *reinterpret_cast<const uint8_t*>(0xBD0792) != 0;
        if (!(inWorld ? (flags >> 1) & 1 : flags & 1))
            return;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\GlobalStrings.dbc");
        void* L = reinterpret_cast<void*(__cdecl*)()>(0x817DB0)();
        const char* value = t.Str(row, 0xC);
        reinterpret_cast<void(__cdecl*)(void*, const char*)>(0x84E350)(L, value ? value : "");
        reinterpret_cast<void(__cdecl*)(void*, int, const char*)>(0x84E900)(L, -10002, t.Str(row, 8));
    }

    // FUN_1025af40 (after the world natives, list 0x10BE2A4C) / FUN_1025afb0 (after the glue natives, list
    // 0x10BE284C): every GlobalStrings.dbc row whose flag (+4 bit 1 / bit 0) names that Lua state sets
    // its global key (+8) to the value (+0xC).
    void ApplyGlobalStrings(uint32_t bit)
    {
        void* L = reinterpret_cast<void*(__cdecl*)()>(0x817DB0)();
        if (!L)   // our attach-time glue pass runs before the client has a Lua state
            return;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\GlobalStrings.dbc");
        const uint32_t n = t.Count();
        uint32_t applied = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t* row = t.RowAt(i);
            if (!row)
                continue;
            const char* key = t.Str(row, 8);
            if (!((AscDbc::Table::U32(row, 4) >> bit) & 1))
                continue;
            const char* value = t.Str(row, 0xC);
            reinterpret_cast<void(__cdecl*)(void*, const char*)>(0x84E350)(L, value ? value : "");
            reinterpret_cast<void(__cdecl*)(void*, int, const char*)>(0x84E900)(L, -10002, key);
            ++applied;
        }
        AscLog::Printf("GlobalStrings.dbc: %u of %u rows applied to the %s state", applied, n, bit ? "world" : "glue");
    }
    void ApplyWorldGlobalStrings() { ApplyGlobalStrings(1); }
    void ApplyGlueGlobalStrings() { ApplyGlobalStrings(0); }

    void Init()
    {
        AscBindings::OnWorldRegistered(&ApplyWorldGlobalStrings);
        AscBindings::OnGlueRegistered(&ApplyGlueGlobalStrings);
        AscDbcPatch::Register(0x568, "DBFilesClient\\DungeonEncounterExtra.dbc", 0x10, 0, nullptr);
        // 0x56F (FUN_101e2a30, a whole Quest.dbc row) patches the MemoryBridge quest store: AscBridgeStore.
        AscDbcPatch::Register(0x681, "DBFilesClient\\ZoneLightPoint.dbc", 0x14, 0, nullptr);
        AscDbcPatch::Register(0x68C, "DBFilesClient\\ManastormPlayerGroupModifiers.dbc", 0x14, 0, nullptr);
        AscDbcPatch::Register(0x68D, "DBFilesClient\\ManastormModifiers.dbc", 0x3C, 0, nullptr);
        AscDbcPatch::Register(0x693, "DBFilesClient\\ItemAppearances.dbc", 0x0C, 0, [](const uint8_t*) { AscAppearance_ItemAppearancesPatched(); });
        AscDbcPatch::Register(0x6EC, "DBFilesClient\\ItemSetAppearances.dbc", 0x0C, 0, [](const uint8_t*) { AscAppearance_ItemSetAppearancesPatched(); });
        // 0x6FC (handler_0x06fc): then FUN_101ceee0 rebuilds the geoset map (AscGeosets.cpp).
        AscDbcPatch::Register(0x6FC, "DBFilesClient\\CreatureDisplayInfoGeosetData.dbc", 0x10, 0, &AscGeosets_Rebuild);
        // 0x5F3 CHR_SPECS (handler_0x05f3): 0x84-byte image, then 15 u32-length strings.
        AscDbcPatch::RegisterSized(0x5F3, "DBFilesClient\\ChrSpecs.dbc", 0x84, (7ull << 1) | (7ull << 8) | (0x7Full << 17) | (3ull << 29), nullptr);
        RegisterFields(0x71F, "DBFilesClient/ExtraActionButtons.dbc", "UUSSSSUUUU");
        RegisterFields(0x723, "DBFilesClient/CharacterAdvancementClassTypes.dbc", "USUUUUS");
        // 0x6ED APPEARANCE_DETAILS (FUN_101dcd30): 0x14-byte image, then one C string (+8).
        AscDbcPatch::Register(0x6ED, "DBFilesClient/AppearanceDetails.dbc", 0x14, 1ull << 2, nullptr);
        // 0x6FB GAMEOBJECT_DISPLAY_INFO_ADDON (FUN_1020c190): {id, C string}; then every game object's texture.
        AscDbcPatch::Register(0x6FB, "DBFilesClient/GameObjectDisplayInfoAddon.dbc", 0x08, 1ull << 1, &RefreshGameObjectTextures);
        g_713F50 = reinterpret_cast<GoFn_t>(AscRuntime::Detour(0x713F50, 6, reinterpret_cast<void*>(&Hook713F50)));
        g_712400 = reinterpret_cast<GoFn_t>(AscRuntime::Detour(0x712400, 6, reinterpret_cast<void*>(&Hook712400)));
        // Plain patches registered outside FUN_101e5880: 0x678 ZONE_STORY (FUN_1021a9b0), 0x676 GLOBAL_STRINGS
        // (FUN_1025ad00, + the Lua global), 0x66E TOKEN_TYPES (FUN_1033c2c0).
        AscDbcPatch::Register(0x678, "DBFilesClient/ZoneStory.dbc", 0x14, 1ull << 4, nullptr);
        AscDbcPatch::Register(0x676, "DBFilesClient\\GlobalStrings.dbc", 0x10, (1ull << 2) | (1ull << 3), &ApplyGlobalString);
        AscDbcPatch::Register(0x66E, "DBFilesClient/TokenTypes.dbc", 0x24, 0x7Eull, nullptr);
        // 0x6D6 CHARACTER_CREATION_ARCHETYPES (FUN_101df460): 0x74-byte image, then 21 C strings (+8, +0x20..+0x6C)
        // in field order; either way the creation manager is refreshed (FUN_1018b790).
        AscDbcPatch::Register(0x6D6, "DBFilesClient/CharacterCreationArchetypes.dbc", 0x74, (1ull << 2) | (0xFFFFFull << 8), [](const uint8_t*) { AscCharacterCreate_ArchetypesPatched(); });
        // 0x692 APPEARANCES (FUN_101dced0): 0x44-byte image, then one C string (+8); a NEW row also runs the
        // manager's FUN_100c6750.
        AscDbcPatch::RegisterInsertHook(0x692, "DBFilesClient/Appearances.dbc", 0x44, 1ull << 2, [](const uint8_t*) { AscAppearance_RebuildOutfits(); });
        // 0x6C0 SPELL_TAG_TYPES (FUN_101e3bd0): 0x74-byte image, then six C strings (+0x5C..+0x70); then
        // FUN_10217220 rebuilds the per-type stat lists.
        AscDbcPatch::Register(0x6C0, "DBFilesClient/SpellTagTypes.dbc", 0x74, 0x3Full << 23, [](const uint8_t*) { AscCAFilter_TagTypesPatched(); });
        // Struct image then u32-length strings (FUN_101e1df0 / FUN_101e1f60 / FUN_101e20a0).
        AscDbcPatch::RegisterSized(0x579, "DBFilesClient\\LFGActivities.dbc", 0x1C, 1ull << 4, nullptr);
        AscDbcPatch::RegisterSized(0x57A, "DBFilesClient\\LFGActivityCategories.dbc", 0x10, 1ull << 2, nullptr);
        AscDbcPatch::RegisterSized(0x57B, "DBFilesClient\\LFGActivityGroupType.dbc", 0x0C, 1ull << 1, nullptr);
        sDC.AddPacketHandler(0x686, CNetClientCustomPacket((void*)&OnSpellAddon, nullptr));
        sDC.AddPacketHandler(0x5F4, CNetClientCustomPacket((void*)&OnSpellCustomAttr, nullptr));
        sDC.AddPacketHandler(0x6C1, CNetClientCustomPacket((void*)&OnSpellTag, nullptr));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

// The map 0x10BE0708 lookup (e.g. FUN_103284e0): the SpellAddon.dbc row indexed under `spell`, or nullptr.
const uint8_t* AscPatchData_SpellAddonForSpell(uint32_t spell)
{
    auto it = SpellAddonIndex().find(spell);
    return it != SpellAddonIndex().end() ? AscDbc::Get("DBFilesClient\\SpellAddon.dbc").Row(it->second) : nullptr;
}
