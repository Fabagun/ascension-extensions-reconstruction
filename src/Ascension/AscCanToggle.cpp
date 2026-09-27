// CanToggle<X>Mode(enable): can the player switch a challenge mode on/off right now?
// Every one reads a boolean (FUN_100b8aa0), collects reason codes into a vector, and returns
// (reasons empty, reasons ? { "TOGGLE_<X>_<REASON>", ... } : nil) -- FUN_100beb90 builds the list.
// The shared checks, when enabling:
//   config bool CONFIG_<X>_ENABLE(D) missing or false  -> DISABLED (2)
//   game-mode bits 15..21 (bank, mail, trade, ...)      -> one reason each, then OUTSIDE_INTERACTION (1)
// and always: enable == the mode's own bit             -> ALREADY_ACTIVE (3) / ALREADY_INACTIVE (4).
#include <Ascension/AscAccount.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscScript.hpp>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    const char* const kStandard[] = {"OK", "OUTSIDE_INTERACTION", "DISABLED", "ALREADY_ACTIVE", "ALREADY_INACTIVE",
                                     "AUCTION_HOUSE", "LEVEL_UP", "BANK", "GUILD_BANK", "REALM_BANK", "MAIL", "TRADE"};
    const char* const kFelforged[] = {"OK", "OUTSIDE_INTERACTION", "DISABLED", "ALREADY_ACTIVE", "ALREADY_INACTIVE",
                                      "CANNOT_BE_DISABLED", "OTHER_GAME_MODE_ACTIVE", "BLOCKED_AURAS", "AUCTION_HOUSE",
                                      "LEVEL_UP", "BANK", "GUILD_BANK", "REALM_BANK", "MAIL", "TRADE"};
    const char* const kBuildDraft[] = {"OK", "LEVEL_TOO_HIGH", "DISABLED", "ALREADY_ACTIVE", "ALREADY_INACTIVE",
                                       "NOT_HERO", "DRAFT_ACTIVE", "NOT_NEAR_GAME_MODE_NPC"};

    bool ConfigOn(const char* name)
    {
        const bool* b = AscConfig::Bool(name);
        return b && *b;
    }

    // Mode bits 15..21 -> first + 0..6, then any of them -> OUTSIDE_INTERACTION.
    void InteractionReasons(std::vector<uint32_t>& out, uint32_t first)
    {
        const uint32_t mode = AscGameMode::Mode();
        for (uint32_t bit = 15; bit <= 21; ++bit)
            if ((mode >> bit) & 1)
                out.push_back(first + (bit - 15));
        if (mode & 0x3F8000)
            out.push_back(1);
    }

    int Result(lua_State* L, const std::vector<uint32_t>& reasons, const char* prefix, const char* const* names, uint32_t count)
    {
        AscLua::lua_pushboolean(L, reasons.empty());
        if (reasons.empty())
        {
            AscLua::lua_pushnil(L);
            return 2;
        }
        AscLua::lua_createtable(L, 0, static_cast<int>(reasons.size()));
        for (size_t i = 0; i < reasons.size(); ++i)
        {
            const std::string s = reasons[i] < count ? std::string("TOGGLE_") + prefix + "_" + names[reasons[i]]
                                                     : "UNEXPECTED_ENUM_VALUE_" + std::to_string(reasons[i]);
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushstring(L, s.c_str());
            AscLua::lua_settable(L, -3);
        }
        return 2;
    }

    int Standard(lua_State* L, const char* config, uint32_t modeBit, const char* prefix)
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        std::vector<uint32_t> reasons;
        if (enable)
        {
            if (!ConfigOn(config))
                reasons.push_back(2);
            InteractionReasons(reasons, 5);
        }
        if (enable == (((AscGameMode::Mode() >> modeBit) & 1) != 0))
            reasons.push_back(enable ? 3 : 4);
        return Result(L, reasons, prefix, kStandard, 12);
    }

    int CanToggleCrusaderMode(lua_State* L)    { return Standard(L, "CONFIG_CRUSADER_ENABLED", 12, "CRUSADER"); }
    int CanToggleIronmanMode(lua_State* L)     { return Standard(L, "CONFIG_IRONMAN_ENABLE", 1, "IRONMAN"); }
    int CanToggleNightmareMode(lua_State* L)   { return Standard(L, "CONFIG_NIGHTMARE_ENABLE", 8, "NIGHTMARE"); }
    int CanToggleResoluteMode(lua_State* L)    { return Standard(L, "CONFIG_RESOLUTE_ENABLE", 5, "RESOLUTE"); }
    int CanToggleSurvivalistMode(lua_State* L) { return Standard(L, "CONFIG_SURVIALIST_ENABLE", 2, "SURVIVALIST"); }
    int CanToggleWildCardMode(lua_State* L)    { return Standard(L, "CONFIG_WILD_CARD_ENABLE", 6, "WILDCARD"); }

    // FUN_100d6180: also needs game event 0x148; only GM level >= 2 may disable; other modes and four
    // auras (checked with 0x7282A0 on the player) block enabling.
    int CanToggleFelforgedMode(lua_State* L)
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        std::vector<uint32_t> reasons;
        if (!enable)
        {
            if (AscAccount::GmLevel() < 2)
                reasons.push_back(5);
        }
        else
        {
            if (!ConfigOn("CONFIG_FELFORGED_ENABLE") || !AscGameEvents::IsActive(0x148))
                reasons.push_back(2);
            InteractionReasons(reasons, 8);
        }
        const uint32_t mode = AscGameMode::Mode();
        if (enable == (((mode >> 7) & 1) != 0))
            reasons.push_back(enable ? 3 : 4);
        if (enable)
        {
            if (mode & (0x2 | 0x4 | 0x100 | 0x20 | 0x1000))
                reasons.push_back(6);
            if (uint8_t* p = ActivePlayer())
            {
                typedef int(__thiscall* HasAura_t)(void*, uint32_t);
                const HasAura_t hasAura = reinterpret_cast<HasAura_t>(0x7282A0);
                if (hasAura(p, 0xC7B7E) || hasAura(p, 0x9788BF) || hasAura(p, 0xC7B8B) || hasAura(p, 0xF619C))
                    reasons.push_back(7);
            }
        }
        return Result(L, reasons, "FELFORGED", kFelforged, 15);
    }

    // FUN_100d57d0: level <= 9, Hero class (UNIT_FIELD_BYTES_0 byte 1 == 10) on a player object, and no
    // draft mode (mode bit 3); "already" compares against the active spec's build-draft flag.
    int CanToggleBuildDraftMode(lua_State* L)
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        std::vector<uint32_t> reasons;
        if (enable && !ConfigOn("CONFIG_BUILD_DRAFT_ENABLED"))
            reasons.push_back(2);
        uint8_t* p = ActivePlayer();
        const uint8_t* d = p ? *reinterpret_cast<uint8_t**>(p + 8) : nullptr;
        if (enable && (!p || *reinterpret_cast<const uint32_t*>(d + 0xD8) > 9))
            reasons.push_back(1);
        if (enable == AscGameMode::SpecBuildDraft(AscGameMode::ActiveSpecIndex()))
            reasons.push_back(enable ? 3 : 4);
        if (enable)
        {
            const int32_t type = p ? *reinterpret_cast<const int32_t*>(p + 0x14) : 0;
            if (!p || (type != 3 && type != 4) || static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(d + 0x5C) >> 8) != 10)
                reasons.push_back(5);
            if (AscGameMode::Mode() & 8)
                reasons.push_back(6);
        }
        return Result(L, reasons, "BUILDDRAFT", kBuildDraft, 8);
    }

    // FUN_100d5*: enabling asks CONFIG_DRAFT_ENABLE (true when the config is absent); no reasons list.
    int CanToggleDraftMode(lua_State* L)
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        bool ok = true;
        if (enable)
            if (const bool* b = AscConfig::Bool("CONFIG_DRAFT_ENABLE"))
                ok = *b;
        AscLua::lua_pushboolean(L, ok);
        AscLua::lua_pushnil(L);
        return 2;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "CanToggleBuildDraftMode", CanToggleBuildDraftMode},
        {nullptr, "CanToggleCrusaderMode", CanToggleCrusaderMode},
        {nullptr, "CanToggleDraftMode", CanToggleDraftMode},
        {nullptr, "CanToggleFelforgedMode", CanToggleFelforgedMode},
        {nullptr, "CanToggleIronmanMode", CanToggleIronmanMode},
        {nullptr, "CanToggleNightmareMode", CanToggleNightmareMode},
        {nullptr, "CanToggleResoluteMode", CanToggleResoluteMode},
        {nullptr, "CanToggleSurvivalistMode", CanToggleSurvivalistMode},
        {nullptr, "CanToggleWildCardMode", CanToggleWildCardMode},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
