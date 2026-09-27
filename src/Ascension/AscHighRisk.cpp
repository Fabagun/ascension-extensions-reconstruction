// C_HighRisk (FUN_10275520 / handler_GetInsuranceCostPerSlot / handler_IsFelCommutationActive /
// FUN_102756d0). GetInsuranceTotalCost is registered with the same function as
// GetInsuranceCostPerSlot (both 0x10275670).
//
// SMSG_FEL_COMMUTATION_STATUS_UPDATE 0x73D (FUN_102752e0): u8 active (0x10BE2828).
// ToggleFelCommutation sends CMSG 0x73B with the requested u8 state.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cmath>

using namespace AscScript;

namespace
{
    bool g_felActive = false;

    void __cdecl OnFelStatus(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_felActive = p->m_buffer[p->m_read] != 0;
        p->m_read += 1;
    }

    // PTR 0x10B40A34: TOGGLE_FEL_COMMUTATION_* by result code.
    const char* const kToggleResults[5] = {
        "TOGGLE_FEL_COMMUTATION_OK", "TOGGLE_FEL_COMMUTATION_UNKNOWN",
        "TOGGLE_FEL_COMMUTATION_ALREADY_ACTIVE", "TOGGLE_FEL_COMMUTATION_ALREADY_INACTIVE",
        "TOGGLE_FEL_COMMUTATION_NO_ALTAR_NEARBY",
    };

    // (true, nil) when the requested state differs from the current one, else (false, reason).
    int CanToggleFelCommutation(lua_State* L)
    {
        bool want;
        if (!ReadBool(L, want))
            return 0;
        if (want != g_felActive)
        {
            PushBool(L, true);
            AscLua::lua_pushnil(L);
            return 2;
        }
        PushBool(L, false);
        PushStr(L, kToggleResults[want ? 2 : 3]);
        return 2;
    }

    int IsFelCommutationActive(lua_State* L)
    {
        PushBool(L, g_felActive);
        return 1;
    }

    int ToggleFelCommutation(lua_State* L)
    {
        bool want;
        if (!ReadBool(L, want))
            return 0;
        if (want == g_felActive)
        {
            PushBool(L, false);
            return 1;
        }
        Packet(0x73B).U8(want ? 1 : 0).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_1008e380: at (or above) the server's max level; on a development realm levels 60/70/80
    // also count.
    bool AtMaxLevel(const uint8_t* player)
    {
        const uint32_t level = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0xD8);
        if (level >= AscGameEvents::ServerMaxLevel())
            return true;
        return RealmInfoSvc::Get().Dev() && (level == 60 || level == 70 || level == 80);
    }

    // FUN_10275060: the player's addon field 5 (a float, truncated) times the multiplier of the last
    // tier whose threshold it reaches, +100000 at max level, floored. -1 without a player.
    int32_t InsuranceCost()
    {
        const uint8_t* player = ActivePlayer();
        if (!player)
            return -1;
        const uint64_t guid = **reinterpret_cast<const uint64_t* const*>(player + 8);
        const uint32_t value = static_cast<uint32_t>(static_cast<int64_t>(AscObjectAddon::AsFloat(AscObjectAddon::Unit(guid)[5])));
        struct Tier { uint32_t threshold; double multiplier; };
        static const Tier kTiers[9] = {
            {0, 28.0}, {26, 85.0}, {36, 256.0}, {46, 361.0}, {55, 1250.0},
            {63, 1500.0}, {106, 2000.0}, {115, 2500.0}, {124, 2750.0},
        };
        double multiplier = kTiers[8].multiplier;
        for (const Tier& t : kTiers)
        {
            if (t.threshold > value)
                break;
            multiplier = t.multiplier;
        }
        double cost = static_cast<double>(static_cast<int32_t>(value)) * multiplier;
        if (AtMaxLevel(player))
            cost += 100000.0;
        return static_cast<int32_t>(std::floor(cost));
    }

    int GetInsuranceCost(lua_State* L)
    {
        PushNum(L, InsuranceCost());
        return 1;
    }

    void ResetFel() { g_felActive = false; }   // FUN_10275050 (every glue screen)

    void Init()
    {
        AscRuntime::OnGlueScreen(&ResetFel);
        sDC.AddPacketHandler(0x73D, CNetClientCustomPacket((void*)&OnFelStatus, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_HighRisk", "CanToggleFelCommutation", CanToggleFelCommutation},
        {"C_HighRisk", "GetInsuranceCostPerSlot", GetInsuranceCost},
        {"C_HighRisk", "GetInsuranceTotalCost", GetInsuranceCost},
        {"C_HighRisk", "IsFelCommutationActive", IsFelCommutationActive},
        {"C_HighRisk", "ToggleFelCommutation", ToggleFelCommutation},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
