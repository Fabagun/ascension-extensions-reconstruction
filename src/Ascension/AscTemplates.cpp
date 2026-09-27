// GetQuestTemplate / GetCreatureTemplate / GetCreatureModelData / GetCreatureDisplayInfo(Extra).
// The DLL builds a nlohmann::json object from the client record (FUN_1021ecb0, FUN_1021b2d0,
// FUN_100b39f0, FUN_100b2d20, FUN_100b3350) and converts it with FUN_100a4580/FUN_100a3d60:
// object -> table keyed by name, array -> 1-based table, integer -> lua_pushinteger, float ->
// lua_pushnumber, string -> lua_pushstring of c_str(). That conversion is reproduced directly.
// Missing record / row -> no return values.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <cstring>

using namespace AscScript;

namespace
{
    struct Table
    {
        lua_State* L;
        explicit Table(lua_State* s) : L(s) { AscLua::lua_createtable(L, 0, 0); }

        template <typename T> const T& At(const uint8_t* rec, uint32_t off) { return *reinterpret_cast<const T*>(rec + off); }

        void Int(const char* key, int32_t v)
        {
            AscLua::lua_pushinteger(L, v);
            AscLua::lua_setfield(L, -2, key);
        }
        void Float(const char* key, float v)
        {
            AscLua::lua_pushnumber(L, v);
            AscLua::lua_setfield(L, -2, key);
        }
        void Str(const char* key, const char* s)
        {
            PushStr(L, s);
            AscLua::lua_setfield(L, -2, key);
        }
        // std::string(first, first + n): lua_pushstring stops at the first NUL, and never reads past n.
        void Fixed(const char* key, const char* s, size_t n)
        {
            PushFixed(s, n);
            AscLua::lua_setfield(L, -2, key);
        }
        void Ints(const char* key, const uint8_t* p, int n)
        {
            AscLua::lua_createtable(L, 0, 0);
            for (int i = 0; i < n; ++i)
            {
                AscLua::lua_pushinteger(L, reinterpret_cast<const int32_t*>(p)[i]);
                AscLua::lua_rawseti(L, -2, i + 1);
            }
            AscLua::lua_setfield(L, -2, key);
        }
        void PushFixed(const char* s, size_t n)
        {
            char buf[0x801];
            const size_t len = strnlen(s, n);
            memcpy(buf, s, len);
            buf[len] = 0;
            AscLua::lua_pushstring(L, buf);
        }
    };

    uint32_t ArgId(lua_State* L) { return static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1))); }

    const uint8_t* CacheRecord(uintptr_t fn, uintptr_t cache, uint32_t id)
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(fn)(
            reinterpret_cast<void*>(cache), id, nullptr, nullptr, nullptr, 0));
    }

    // Quest cache record (0x67DE90 on 0xC5DA48), FUN_1021ecb0.
    int GetQuestTemplate(lua_State* L)
    {
        const uint8_t* r = CacheRecord(0x67DE90, 0xC5DA48, ArgId(L));
        if (!r)
            return 0;
        Table t(L);
        static const struct { const char* key; uint32_t off; } kInts[] = {
            {"QuestID", 0x00}, {"QuestMethod", 0x04}, {"QuestLevel", 0x08}, {"QuestMinLevel", 0x0C},
            {"QuestSortID", 0x10}, {"QuestType", 0x14}, {"SuggestedGroupNum", 0x18},
            {"FriendlyFactionID", 0x1C}, {"FriendlyFactionAmount", 0x20}, {"HostileFactionID", 0x24},
            {"HostileFactionAmount", 0x28}, {"RewardNextQuest", 0x2C}, {"RewardXPDifficulty", 0x30},
            {"RewardMoney", 0x34}, {"RewardBonusMoney", 0x38}, {"RewardDisplaySpell", 0x3C},
            {"RewardSpell", 0x40}, {"RewardHonor", 0x44}, {"StartItem", 0x4C}, {"Flags", 0x50},
            {"POIContinent", 0xA4}, {"POIPriority", 0xB0}, {"RewardTitleId", 0x2494},
            {"RequiredPlayerKills", 0x2498}, {"RewardTalents", 0x249C}, {"RewardArenaPoints", 0x24A0},
            {"Unknown", 0x2CE0},
        };
        for (const auto& f : kInts)
            t.Int(f.key, t.At<int32_t>(r, f.off));
        t.Float("RewardKillHonor", t.At<float>(r, 0x48));
        t.Float("POIx", t.At<float>(r, 0xA8));
        t.Float("POIy", t.At<float>(r, 0xAC));
        t.Ints("RewardItems", r + 0x54, 4);
        t.Ints("RewardAmount", r + 0x64, 4);
        t.Ints("ChoiceItems", r + 0x74, 6);
        t.Ints("ChoiceItemAmount", r + 0x8C, 6);
        t.Fixed("Title", reinterpret_cast<const char*>(r + 0xB4), 0x200);
        t.Fixed("Objectives", reinterpret_cast<const char*>(r + 0x2B4), 0xBB8);
        t.Fixed("Details", reinterpret_cast<const char*>(r + 0xE6C), 0xBB8);
        t.Fixed("AreaDescription", reinterpret_cast<const char*>(r + 0x1A24), 0x200);
        t.Ints("RequiredNpcOrGo", r + 0x1C24, 4);
        t.Ints("RequiredNpcOrGoCount", r + 0x1C34, 4);
        t.Ints("RequiredItemId", r + 0x1C44, 6);
        t.Ints("RequiredItemCount", r + 0x1C5C, 6);
        t.Ints("ItemDrop", r + 0x1C74, 4);
        t.Ints("ItemDropCount", r + 0x1C84, 4);
        // FUN_1021e970: four 0x200-byte objective strings from +0x1C94.
        AscLua::lua_createtable(L, 0, 0);
        for (int i = 0; i < 4; ++i)
        {
            t.PushFixed(reinterpret_cast<const char*>(r + 0x1C94 + i * 0x200), 0x200);
            AscLua::lua_rawseti(L, -2, i + 1);
        }
        AscLua::lua_setfield(L, -2, "ObjectiveText");
        t.Fixed("CompletedText", reinterpret_cast<const char*>(r + 0x24A4), 0x800);
        t.Ints("RewardFactionValue", r + 0x2CA4, 5);
        t.Ints("RewardFactionID", r + 0x2CB8, 5);
        t.Ints("RewardFactionValueOverride", r + 0x2CCC, 5);
        return 1;
    }

    // Creature cache record (0x67B6A0 on 0xC5D690), FUN_1021b2d0.
    int GetCreatureTemplate(lua_State* L)
    {
        const uint8_t* r = CacheRecord(0x67B6A0, 0xC5D690, ArgId(L));
        if (!r)
            return 0;
        Table t(L);
        t.Int("ID", t.At<int32_t>(r, 0x6C));
        t.Str("SubName", t.At<const char*>(r, 0x04));
        t.Str("IconName", t.At<const char*>(r, 0x08));
        t.Int("Flags", t.At<int32_t>(r, 0x0C));
        t.Int("Type", t.At<int32_t>(r, 0x10));
        t.Int("Family", t.At<int32_t>(r, 0x14));
        t.Int("Rank", t.At<int32_t>(r, 0x18));
        t.Ints("ProxyCreatureID", r + 0x1C, 2);
        t.Ints("DisplayID", r + 0x24, 4);
        t.Float("HealthModifier", t.At<float>(r, 0x34));
        t.Float("PowerModifier", t.At<float>(r, 0x38));
        t.Int("RacialLeader", t.At<int32_t>(r, 0x3C));
        t.Ints("QuestItem", r + 0x40, 6);
        t.Int("MovementID", t.At<int32_t>(r, 0x58));
        t.Str("Name", t.At<const char*>(r, 0x5C));
        return 1;
    }

    // CreatureModelData row, FUN_100b39f0. GeoBoxMax* read the GeoBoxMin* columns in the original.
    int GetCreatureModelData(lua_State* L)
    {
        const uint8_t* r = ClientDbcRow(0xAD3500, ArgId(L));
        if (!r)
            return 0;
        Table t(L);
        t.Int("ID", t.At<int32_t>(r, 0x00));
        t.Int("Flags", t.At<int32_t>(r, 0x04));
        t.Str("ModelName", t.At<const char*>(r, 0x08));
        t.Int("SizeClass", t.At<int32_t>(r, 0x0C));
        t.Float("ModelScale", t.At<float>(r, 0x10));
        t.Int("BloodID", t.At<int32_t>(r, 0x14));
        t.Int("FootprintTextureID", t.At<int32_t>(r, 0x18));
        t.Float("FootprintTextureLength", t.At<float>(r, 0x1C));
        t.Float("FootprintTextureWidth", t.At<float>(r, 0x20));
        t.Float("FootprintParticleScale", t.At<float>(r, 0x24));
        t.Int("FoleyMaterialID", t.At<int32_t>(r, 0x28));
        t.Int("FootstepShakeSize", t.At<int32_t>(r, 0x2C));
        t.Int("DeathThudShakeSize", t.At<int32_t>(r, 0x30));
        t.Int("SoundID", t.At<int32_t>(r, 0x34));
        t.Float("CollisionWidth", t.At<float>(r, 0x38));
        t.Float("CollisionHeight", t.At<float>(r, 0x3C));
        t.Float("MountHeight", t.At<float>(r, 0x40));
        t.Float("GeoBoxMinX", t.At<float>(r, 0x44));
        t.Float("GeoBoxMinY", t.At<float>(r, 0x48));
        t.Float("GeoBoxMinZ", t.At<float>(r, 0x4C));
        t.Float("GeoBoxMaxX", t.At<float>(r, 0x44));
        t.Float("GeoBoxMaxY", t.At<float>(r, 0x48));
        t.Float("GeoBoxMaxZ", t.At<float>(r, 0x4C));
        t.Float("WorldEffectScale", t.At<float>(r, 0x5C));
        t.Float("AttachedEffectScale", t.At<float>(r, 0x60));
        t.Float("MissileCollisionRadius", t.At<float>(r, 0x64));
        t.Float("MissileCollisionPush", t.At<float>(r, 0x68));
        t.Float("MissileCollisionRaise", t.At<float>(r, 0x6C));
        return 1;
    }

    // CreatureDisplayInfo row, FUN_100b2d20.
    int GetCreatureDisplayInfo(lua_State* L)
    {
        const uint8_t* r = ClientDbcRow(0xAD34B8, ArgId(L));
        if (!r)
            return 0;
        Table t(L);
        t.Int("ID", t.At<int32_t>(r, 0x00));
        t.Int("ModelID", t.At<int32_t>(r, 0x04));
        t.Int("SoundID", t.At<int32_t>(r, 0x08));
        t.Int("ExtendedDisplayInfoID", t.At<int32_t>(r, 0x0C));
        t.Float("CreatureModelScale", t.At<float>(r, 0x10));
        t.Int("CreatureModelAlpha", t.At<int32_t>(r, 0x14));
        AscLua::lua_createtable(L, 0, 0);
        for (int i = 0; i < 3; ++i)
        {
            PushStr(L, t.At<const char*>(r, 0x18 + i * 4));
            AscLua::lua_rawseti(L, -2, i + 1);
        }
        AscLua::lua_setfield(L, -2, "TextureVariation");
        t.Str("PortraitTextureName", t.At<const char*>(r, 0x24));
        t.Int("BloodLevel", t.At<int32_t>(r, 0x28));
        t.Int("BloodID", t.At<int32_t>(r, 0x2C));
        t.Int("NpcSoundID", t.At<int32_t>(r, 0x30));
        t.Int("ParticleColorID", t.At<int32_t>(r, 0x34));
        t.Int("CreatureGeosetData", t.At<int32_t>(r, 0x38));
        t.Int("ObjectEffectPackageID", t.At<int32_t>(r, 0x3C));
        return 1;
    }

    // CreatureDisplayInfoExtra row, FUN_100b3350.
    int GetCreatureDisplayInfoExtra(lua_State* L)
    {
        const uint8_t* r = ClientDbcRow(0xAD3494, ArgId(L));
        if (!r)
            return 0;
        Table t(L);
        static const char* const kInts[] = {"ID", "DisplayRaceID", "DisplaySexID", "SkinID", "FaceID",
                                            "HairStyleID", "HairColorID", "FacialHairID"};
        for (uint32_t i = 0; i < 8; ++i)
            t.Int(kInts[i], t.At<int32_t>(r, i * 4));
        t.Ints("NPCItemDisplay", r + 0x20, 11);
        t.Int("Flags", t.At<int32_t>(r, 0x4C));
        t.Str("BakeName", t.At<const char*>(r, 0x50));
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetCreatureDisplayInfo", GetCreatureDisplayInfo},
        {nullptr, "GetCreatureDisplayInfoExtra", GetCreatureDisplayInfoExtra},
        {nullptr, "GetCreatureModelData", GetCreatureModelData},
        {nullptr, "GetCreatureTemplate", GetCreatureTemplate},
        {nullptr, "GetQuestTemplate", GetQuestTemplate},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), nullptr);
}
