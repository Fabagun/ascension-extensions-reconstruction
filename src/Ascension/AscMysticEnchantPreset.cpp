// C_MysticEnchantPreset -- transcribed from the original's subsystem at 0x102F1780..0x102F37F0.
//
// Manager DAT_10BCC088 (accessor FUN_102F3030; static init +0x00 = 0, +0x10 = -1):
//   +0x00 u32 active preset (0-based)   +0x04 vector<vector<u32>> presets (slot enchants)
//   +0x10 u32 preset an Activate is waiting on
// SMSG 0x5FF replaces the presets (u32 n, then per preset u32 m + m spells); 0x600 sets the active one;
// 0x602 / 0x60D / 0x616 are the save / activate / unlock results (a NUL-terminated RE_PRESET_* name).
// Back at the glue screen (FUN_102F2230): active = -1, no presets, pending = 0.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    const char* const kSave[4] = {  // 0x10B1F9E4
        "RE_PRESET_SAVE_OK", "RE_PRESET_SAVE_UNKNOWN", "RE_PRESET_SAVE_BUILD_DRAFT", "RE_PRESET_SAVE_BAD_CLASS"};
    const char* const kSetActive[5] = {  // 0x10B1D0C8
        "RE_PRESET_SET_ACTIVE_OK", "RE_PRESET_SET_ACTIVE_UNKNOWN", "RE_PRESET_SET_ACTIVE_BUILD_DRAFT",
        "RE_PRESET_SET_ACTIVE_NOT_WHILE_CASTING", "RE_PRESET_SET_ACTIVE_BAD_CLASS"};
    const char* const kUnlock[7] = {  // 0x10B22924
        "RE_PRESET_UNLOCK_OK", "RE_PRESET_UNLOCK_UNKNOWN", "RE_PRESET_UNLOCK_MAX_VALUE_REACHED",
        "RE_PRESET_UNLOCK_NO_MONEY", "RE_PRESET_UNLOCK_BUILD_DRAFT", "RE_PRESET_UNLOCK_NOT_WHILE_CASTING",
        "RE_PRESET_UNLOCK_BAD_CLASS"};
    const uint32_t kUnlockToken = 0x1B9271;

    struct Presets
    {
        uint32_t active = 0;                          // +0x00
        std::vector<std::vector<uint32_t>> list;      // +0x04
        uint32_t pending = 0xFFFFFFFF;                // +0x10
    };
    Presets& P() { static Presets p; return p; }

    std::string EnumName(const char* const* table, uint32_t count, uint32_t v)
    {
        return v < count ? std::string(table[v]) : "UNEXPECTED_ENUM_VALUE_" + std::to_string(v);
    }
    // The error-list pushers FUN_102F18D0 / FUN_102F1AC0 / FUN_102F1CB0: {[i] = name}.
    void PushErrors(lua_State* L, const char* const* table, uint32_t count, const std::vector<uint32_t>& e)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(e.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < e.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, EnumName(table, count, e[i]).c_str());
            AscLua::lua_settable(L, -3);
        }
    }

    uint8_t PlayerClass(const uint8_t* unit)
    {
        if (!unit)
            return 0;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(unit + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0x5C) >> 8);
    }
    bool CoAClass(const uint8_t* unit) { const uint8_t c = PlayerClass(unit); return c >= 12 && c <= 32; }   // FUN_100C5F50
    bool Draft() { return AscGameMode::SpecBuildDraft(AscGameMode::ActiveSpecIndex()); }
    // FUN_10308470: how many of `item` are in the backpack and equipped bags.
    uint32_t BagItemCount(uint32_t item)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return 0;
        auto count = [](const uint8_t* it) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0x38); };
        auto entry = [](const uint8_t* it) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0xC); };
        uint32_t n = 0;
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(player + 8);
        for (uint32_t s = 0x17; s < 0x27; ++s)
            if (uint8_t* it = static_cast<uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + s * 8), 2)))
                if (entry(it) == item)
                    n += count(it);
        const uint64_t* bags = reinterpret_cast<const uint64_t*>(0xC23540);
        for (uint32_t b = 0; b < 4; ++b)
        {
            uint8_t* container = static_cast<uint8_t*>(ObjectPtr(bags[b], 4));
            if (!container)
                continue;
            typedef void*(__thiscall* Inventory_t)(void*);
            if (!(*reinterpret_cast<Inventory_t**>(container))[0x24 / 4](container))
                continue;
            const uint32_t slots = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(container + 8) + 0x100);
            for (uint32_t i = 0; i < slots; ++i)
                if (uint8_t* it = reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t)>(0x754390)(container, i))
                    if (entry(it) == item)
                        n += count(it);
        }
        return n;
    }

    std::vector<uint32_t> CheckActivate()   // FUN_102F1EA0 (the preset index is not examined)
    {
        std::vector<uint32_t> e;
        if (Draft())
            e.push_back(2);
        if (const uint8_t* player = ActivePlayer())
        {
            if (*reinterpret_cast<const uint32_t*>(player + 0xA60) != 0)
                e.push_back(3);
            if (CoAClass(player))
                e.push_back(4);
        }
        return e;
    }
    std::vector<uint32_t> CheckSave()   // FUN_102F1FC0
    {
        std::vector<uint32_t> e;
        if (Draft())
            e.push_back(2);
        if (const uint8_t* player = ActivePlayer())
            if (CoAClass(player))
                e.push_back(3);
        return e;
    }
    std::vector<uint32_t> CheckUnlock()   // FUN_102F20B0
    {
        std::vector<uint32_t> e;
        if (P().list.size() > 99)
            e.push_back(2);
        if (BagItemCount(kUnlockToken) == 0)
            e.push_back(3);
        if (Draft())
            e.push_back(4);
        if (const uint8_t* player = ActivePlayer())
        {
            if (*reinterpret_cast<const uint32_t*>(player + 0xA60) != 0)
                e.push_back(5);
            if (CoAClass(player))
                e.push_back(6);
        }
        return e;
    }

    int Activate(lua_State* L)   // 0x102F3090
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        Packet(0x60C).U32(index - 1).Send();
        P().pending = index - 1;
        PushBool(L, true);
        return 1;
    }
    int GetPresetData(lua_State* L)   // 0x102F3490
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        static const std::vector<uint32_t> kEmpty;   // DAT_10BE3A2C
        const std::vector<uint32_t>& v = index - 1 < P().list.size() ? P().list[index - 1] : kEmpty;
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
        PushBool(L, P().active == index - 1);
        return 2;
    }
    int GetNumPresets(lua_State* L)   // 0x102F3460
    {
        PushInt(L, static_cast<int32_t>(P().list.size()));
        return 1;
    }
    int Unlock(lua_State* L)   // 0x102F3610
    {
        Packet(0x615).Send();
        PushBool(L, true);
        return 1;
    }
    int PushCheck(lua_State* L, const std::vector<uint32_t>& e, const char* const* table, uint32_t count)
    {
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushErrors(L, table, count, e);
        return 2;
    }
    int CanSave(lua_State* L)   // 0x102F32C0
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        return PushCheck(L, CheckSave(), kSave, 4);
    }
    int CanActivate(lua_State* L)   // 0x102F31B0
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        return PushCheck(L, CheckActivate(), kSetActive, 5);
    }
    int CanUnlock(lua_State* L)   // 0x102F33C0
    {
        return PushCheck(L, CheckUnlock(), kUnlock, 7);
    }

    uint32_t ReadU32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    std::string ReadCStr(CDataStore* p)
    {
        const std::string v(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(v.size() + 1);
        return v;
    }
    int ResultIndex(const std::string& s, const char* const* table, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (s == table[i])
                return static_cast<int>(i);
        AscLog::Printf("Unexpected Value: %s", s.c_str());
        return -1;
    }

    void __cdecl OnPresetData(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FF
    {
        const uint32_t n = ReadU32(p);
        P().list.assign(n, {});
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t m = ReadU32(p);
            P().list[i].assign(m, 0);
            for (uint32_t k = 0; k < m; ++k)
                P().list[i][k] = ReadU32(p);
        }
    }
    void __cdecl OnActivePreset(void*, uint32_t, uint32_t, CDataStore* p)   // 0x600
    {
        P().active = ReadU32(p);
    }
    void __cdecl OnSaveResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x602
    {
        const std::string r = ReadCStr(p);
        if (ResultIndex(r, kSave, 4) == 0 && P().active < P().list.size())
        {
            uint32_t count;
            const uint32_t* slots = AscMysticEnchant::Slots(count);
            P().list[P().active].assign(slots, slots + count);
        }
        AscRuntime::Signal("MYSTIC_ENCHANT_PRESET_SAVE_RESULT", "%s", r.c_str());
    }
    void __cdecl OnSetActiveResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x60D
    {
        const std::string r = ReadCStr(p);
        if (ResultIndex(r, kSetActive, 5) == 0)
            P().active = P().pending;
        P().pending = 0xFFFFFFFF;
        AscRuntime::Signal("MYSTIC_ENCHANT_PRESET_SET_ACTIVE_RESULT", "%s", r.c_str());
    }
    void __cdecl OnUnlockResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x616
    {
        const std::string r = ReadCStr(p);
        if (ResultIndex(r, kUnlock, 7) == 0)
            P().list.emplace_back();
        AscRuntime::Signal("MYSTIC_ENCHANT_PRESET_UNLOCK_RESULT", "%s", r.c_str());
    }

    void Reset()   // FUN_102F2230
    {
        P().active = 0xFFFFFFFF;
        P().list.clear();
        P().pending = 0;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x5FF, CNetClientCustomPacket((void*)&OnPresetData, nullptr));
        sDC.AddPacketHandler(0x600, CNetClientCustomPacket((void*)&OnActivePreset, nullptr));
        sDC.AddPacketHandler(0x602, CNetClientCustomPacket((void*)&OnSaveResult, nullptr));
        sDC.AddPacketHandler(0x60D, CNetClientCustomPacket((void*)&OnSetActiveResult, nullptr));
        sDC.AddPacketHandler(0x616, CNetClientCustomPacket((void*)&OnUnlockResult, nullptr));
        AscRuntime::OnGlueScreen(&Reset);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_MysticEnchantPreset", "Activate", Activate},
        {"C_MysticEnchantPreset", "GetPresetData", GetPresetData},
        {"C_MysticEnchantPreset", "GetNumPresets", GetNumPresets},
        {"C_MysticEnchantPreset", "Unlock", Unlock},
        {"C_MysticEnchantPreset", "CanSave", CanSave},
        {"C_MysticEnchantPreset", "CanActivate", CanActivate},
        {"C_MysticEnchantPreset", "CanUnlock", CanUnlock},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace AscItems
{
    uint32_t BagItemCount(uint32_t item) { return ::BagItemCount(item); }   // FUN_10308470
}
