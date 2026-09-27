// Game mode + per-specialization links -- the original's manager at 0x10be4138 (accessor
// FUN_1031ef10, init 0x1031ee80, Lua table FUN_1031f580). 64 functions in the original read it.
//
//   +0x00 custom game mode (what GetCustomGameMode reports)   +0x04 active mode mask (tutorial and
//   skill-card gating read this)   +0x08 + i*0x20, i < 20: one record per specialization:
//     +0x00 flags (bit 0 = build-draft)  +0x04 mystic-enchant preset  +0x08 has preset
//     +0x10 equipment-set guid (u64)     +0x18 has equipment set
//
// SMSG_GAME_MODE_CHANGED            0x90B { u32 mode }                 mode = mask = value
// SMSG_UPDATE_SPECIALIZATION_FLAGS  0x5CC { u32 spec, u32 flags }       SPECIALIZATION_FLAGS_UPDATED
// (0x737) mystic-enchant link       { u32 spec, u8 has, [u32 preset] }  MYSTIC_ENCHANT_SPECIALIZATION_LINK_UPDATED
// (0x738) equipment-set link        { u32 spec, u8 has, [u64 guid] }    EQUIPMENT_SET_SPECIALIZATION_LINK_UPDATED
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <cstring>
#include <Ascension/AscRuntime.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <string>
#include <vector>

using namespace AscScript;

namespace AscGameMode
{
namespace
{
    struct SpecRecord
    {
        uint32_t flags = 0;
        uint32_t preset = 0;
        uint8_t hasPreset = 0;
        uint64_t equipSet = 0;
        uint8_t hasEquipSet = 0;
    };

    uint32_t g_mode = 0, g_mask = 0;
    SpecRecord g_specs[20];
    std::vector<void (*)()> g_modeListeners;

    bool InWorld() { return *reinterpret_cast<const uint8_t*>(0xBD0792) != 0; }   // *DAT_10bcc3e4

    uint32_t ReadU32(CDataStore* p) { uint32_t v = *reinterpret_cast<const uint32_t*>(p->m_buffer + p->m_read); p->m_read += 4; return v; }
    uint8_t ReadU8(CDataStore* p)   { return static_cast<uint8_t>(p->m_buffer[p->m_read++]); }

    // handler_0x090b
    void __cdecl OnGameModeChanged(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_mode = ReadU32(pkt);
        g_mask = g_mode;
        for (auto cb : g_modeListeners)   // FUN_1031bd50: skill-card containers reset (their module registers)
            cb();
        if (InWorld())
            AscRuntime::Signal("ASCENSION_CUSTOM_GAME_MODE_CHANGED", "%u", g_mode);
    }

    // FUN_1031eca0
    void __cdecl OnSpecializationFlags(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        const uint32_t spec = ReadU32(pkt);
        if (spec >= 20)
            return;
        const uint32_t flags = ReadU32(pkt);
        const uint32_t old = g_specs[spec].flags;
        g_specs[spec].flags = flags;
        AscRuntime::Signal("SPECIALIZATION_FLAGS_UPDATED", "%u%u", spec, flags);
        if ((old & 1) != (flags & 1))
            AscRuntime::Signal("ASCENSION_CUSTOM_GAME_MODE_CHANGED", "%u", g_mode);
    }

    // handler_0x0737
    void __cdecl OnMysticEnchantLink(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        const uint32_t spec = ReadU32(pkt);
        if (spec >= 20)
            return;
        const uint8_t has = ReadU8(pkt);
        g_specs[spec].preset = has ? ReadU32(pkt) : 0;
        g_specs[spec].hasPreset = has ? 1 : 0;
        AscRuntime::Signal("MYSTIC_ENCHANT_SPECIALIZATION_LINK_UPDATED");
    }

    // handler_0x0738
    void __cdecl OnEquipmentSetLink(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        const uint32_t spec = ReadU32(pkt);
        if (spec >= 20)
            return;
        const uint8_t has = ReadU8(pkt);
        uint64_t guid = 0;
        if (has)
        {
            guid = ReadU32(pkt);
            guid |= static_cast<uint64_t>(ReadU32(pkt)) << 32;
        }
        g_specs[spec].equipSet = guid;
        g_specs[spec].hasEquipSet = has ? 1 : 0;
        AscRuntime::Signal("EQUIPMENT_SET_SPECIALIZATION_LINK_UPDATED");
    }

    // FUN_1031e8f0 (glue screen)
    void Reset()
    {
        g_mode = g_mask = 0;
        for (SpecRecord& r : g_specs)
            r = SpecRecord();
    }

    // handler_GetCustomGameMode: bit 0x800 (build draft) only while the active spec has draft on.
    int GetCustomGameMode(lua_State* L)
    {
        const int spec = ActiveSpecIndex();
        const bool draft = spec >= 0 && spec < 20 && (g_specs[spec].flags & 1);
        PushInt(L, static_cast<int32_t>(draft ? g_mode : (g_mode & 0xFFFFF7FF)));
        return 1;
    }

    int IsBuildDraftModeEnabled(lua_State* L)
    {
        uint32_t spec;
        if (!ReadNumber(L, spec) || spec == 0)
            return 0;
        PushBool(L, spec - 1 < 20 && (g_specs[spec - 1].flags & 1));
        return 1;
    }

    int GetMysticEnchantPresetIDForSpecialization(lua_State* L)
    {
        uint32_t spec;
        if (!ReadNumber(L, spec) || spec == 0)
            return 0;
        if (spec - 1 < 20 && g_specs[spec - 1].hasPreset)
        {
            PushInt(L, static_cast<int32_t>(g_specs[spec - 1].preset));
            return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // handler_GetEquipmentSetIDForSpecialization: the name of the client's equipment set (list at
    // 0xACFC00 +8, entry +8 = set id, +0x114 = name) the link points at.
    int GetEquipmentSetIDForSpecialization(lua_State* L)
    {
        uint32_t spec;
        if (!ReadNumber(L, spec) || spec == 0)
            return 0;
        if (spec - 1 < 20 && g_specs[spec - 1].hasEquipSet)
        {
            const SpecRecord& r = g_specs[spec - 1];
            uint32_t node = *reinterpret_cast<const uint32_t*>(0xACFC00 + 8);
            if (node & 1)
                node = 0;
            while (node)
            {
                const uint8_t* set = *reinterpret_cast<uint8_t* const*>(node + 8);
                if (*reinterpret_cast<const uint32_t*>(set + 8) == static_cast<uint32_t>(r.equipSet) && (r.equipSet >> 32) == 0)
                {
                    PushStr(L, reinterpret_cast<const char*>(set + 0x114));
                    return 1;
                }
                node = *reinterpret_cast<const uint32_t*>(node + 4);
                if (node & 1)
                    node = 0;
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_1031f390: {number[, number]} -> CMSG 0x739 { u32 spec-1, u8 has, [u32 preset] }
    int SetMysticEnchantPresetIDForSpecialization(lua_State* L)
    {
        const bool has = AscLua::lua_type(L, 2) > 0;
        if (has ? !ValidateInput(L, {NUMBER, NUMBER}) : !ValidateInput(L, {NUMBER, NIL}))
            return 0;
        const int32_t spec = ToInt(CheckNumber(L, 1));
        const int32_t preset = has ? ToInt(CheckNumber(L, 2)) : 0;
        if (spec != 0 && static_cast<uint32_t>(spec - 1) < 20)
        {
            Packet p(0x739);
            p.U32(static_cast<uint32_t>(spec - 1)).U8(has ? 1 : 0);
            if (has)
                p.U32(static_cast<uint32_t>(preset));
            p.Send();
        }
        return 0;
    }

    // FUN_1031f1e0 / FUN_100d0500: {number[, string]}. A named set is looked up with 0x5AE600; an
    // unknown name sends nothing. -> CMSG 0x73A { u32 spec-1, u8 has, [u32 set id, u32 0] }
    int SetEquipmentSetIDForSpecialization(lua_State* L)
    {
        const bool named = AscLua::lua_type(L, 2) > 0;
        if (named ? !ValidateInput(L, {NUMBER, STRING}) : !ValidateInput(L, {NUMBER, NIL}))
            return 0;
        const uint32_t spec = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (spec == 0)
            return 0;
        uint32_t setId = 0;
        if (named)
        {
            const std::string name = CheckString(L, 2);
            const uint8_t* set = reinterpret_cast<const uint8_t*(__cdecl*)(const char*)>(0x5AE600)(name.c_str());
            if (!set)
                return 0;
            setId = *reinterpret_cast<const uint32_t*>(set + 8);
        }
        if (spec - 1 < 20)
        {
            Packet p(0x73A);
            p.U32(spec - 1).U8(named ? 1 : 0);
            if (named)
                p.U32(setId).U32(0);
            p.Send();
        }
        return 0;
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(Reset);
        sDC.AddPacketHandler(0x90B, CNetClientCustomPacket((void*)&OnGameModeChanged, nullptr));
        sDC.AddPacketHandler(0x5CC, CNetClientCustomPacket((void*)&OnSpecializationFlags, nullptr));
        sDC.AddPacketHandler(0x737, CNetClientCustomPacket((void*)&OnMysticEnchantLink, nullptr));
        sDC.AddPacketHandler(0x738, CNetClientCustomPacket((void*)&OnEquipmentSetLink, nullptr));
    }

    // handler_IsDraftMode: mode bit 3.
    int IsDraftMode(lua_State* L)
    {
        PushBool(L, ((g_mode >> 3) & 1) != 0);
        return 1;
    }

    // FUN_100de480: CMSG 0x5A4 {u32 length, mode name (no NUL), u8 enable}; unknown bits send "None".
    void SendToggleMode(uint32_t bit, bool enable)
    {
        const char* name;
        switch (bit)
        {
        case 0x2: name = "Ironman"; break;
        case 0x4: name = "Survivalist"; break;
        case 0x8: name = "Draft"; break;
        case 0x20: name = "Resolute"; break;
        case 0x40: name = "WildCard"; break;
        case 0x80: name = "Felforged"; break;
        case 0x100: name = "Nightmare"; break;
        case 0x800: name = "BuildDraft"; break;
        case 0x1000: name = "Crusader"; break;
        default: name = "None"; break;
        }
        const uint32_t len = static_cast<uint32_t>(strlen(name));
        Packet p(0x5A4);
        p.U32(len);
        p.Data(name, len);
        p.U8(enable ? 1 : 0);
        p.Send();
    }

    template <uint32_t Bit>
    int ToggleMode(lua_State* L)
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        SendToggleMode(Bit, enable);
        PushBool(L, true);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "IsDraftMode", IsDraftMode},
        {nullptr, "ToggleIronmanMode", ToggleMode<0x2>},
        {nullptr, "ToggleSurvivalistMode", ToggleMode<0x4>},
        {nullptr, "ToggleResoluteMode", ToggleMode<0x20>},
        {nullptr, "ToggleWildCardMode", ToggleMode<0x40>},
        {nullptr, "ToggleFelforgedMode", ToggleMode<0x80>},
        {nullptr, "ToggleNightmareMode", ToggleMode<0x100>},
        {nullptr, "ToggleBuildDraftMode", ToggleMode<0x800>},
        {nullptr, "ToggleCrusaderMode", ToggleMode<0x1000>},
        {nullptr, "GetCustomGameMode", GetCustomGameMode},
        {nullptr, "IsBuildDraftModeEnabled", IsBuildDraftModeEnabled},
        {nullptr, "GetMysticEnchantPresetIDForSpecialization", GetMysticEnchantPresetIDForSpecialization},
        {nullptr, "SetMysticEnchantPresetIDForSpecialization", SetMysticEnchantPresetIDForSpecialization},
        {nullptr, "GetEquipmentSetIDForSpecialization", GetEquipmentSetIDForSpecialization},
        {nullptr, "SetEquipmentSetIDForSpecialization", SetEquipmentSetIDForSpecialization},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

uint32_t Mode() { return g_mode; }
uint32_t ActiveMask() { return g_mask; }
bool SpecBuildDraft(int spec) { return static_cast<uint32_t>(spec) < 20 && (g_specs[spec].flags & 1) != 0; }   // FUN_1031efd0
void OnModeChanged(void (*cb)()) { g_modeListeners.push_back(cb); }

// FUN_1016f580: the player's record in the CharacterAdvancement manager (0x10bde440), +0x14. That
// manager is not transcribed yet; until it is, there is no record, which the original reports as -1.
int ActiveSpecIndex() { return AscCA::ActiveSpecIndex(); }   // FUN_1016f580
}
