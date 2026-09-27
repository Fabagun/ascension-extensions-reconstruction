// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c): request senders.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <string>

using namespace AscScript;

namespace
{
    int32_t ArgInt(lua_State* L, int idx) { return ToInt(AscLua::lua_tonumber(L, idx)); }

    // FUN_10296d60: levels lua_getstack (0x84FE40) can walk.
    int GetCallstackHeight(lua_State* L)
    {
        uint8_t ar[0x80];
        typedef int(__cdecl* GetStack_t)(lua_State*, int, void*);
        const GetStack_t getStack = reinterpret_cast<GetStack_t>(0x84FE40);
        int n = 0;
        while (getStack(L, n, ar))
            ++n;
        AscLua::lua_pushinteger(L, n);
        return 1;
    }

    int SendDraftSkip(lua_State*)    { Packet(0x74D).Send(); return 0; }
    int RedraftAbilities(lua_State*) { Packet(0x74E).Send(); return 0; }

    int SendDraftModeSpellSelection(lua_State* L)
    {
        Packet(0x74C).U32(static_cast<uint32_t>(ArgInt(L, 1))).Send();
        return 0;
    }

    int DeleteStablePet(lua_State* L)
    {
        Packet(0x67C).U32(static_cast<uint32_t>(static_cast<int>(AscLua::lua_tonumber(L, 1)))).Send();
        return 0;
    }

    // CMSG_UNLEARN_SKILL (0x202) for a SkillLine the player has a slot for (0x6DC1C0).
    int AbandonSkillID(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ArgInt(L, 1));
        if (ClientDbcRow(0xAD45E0, id))
            if (uint8_t* p = ActivePlayer())
                if (reinterpret_cast<int(__thiscall*)(void*, int)>(0x6DC1C0)(p, static_cast<int>(id)) != -1)
                    Packet(0x202).U32(id).Send();
        return 0;
    }

    int RequestStatisticQuery(lua_State* L)
    {
        uint32_t id;
        if (ReadNumber(L, id))
            Packet(0x777).U32(id).Send();
        return 0;
    }

    int CharacterActivate(lua_State* L)
    {
        if (const int32_t id = ArgInt(L, 1))
            Packet(0x72E).U32(static_cast<uint32_t>(id)).Send();
        return 0;
    }

    int CharacterDeactivate(lua_State* L)
    {
        if (const int32_t id = ArgInt(L, 1))
            Packet(0x72F).U32(static_cast<uint32_t>(id)).Send();
        return 0;
    }

    // CMSG 0x523 { u8 2, u32 item } -- the vanity-collection flavour of the delivery request.
    int RequestDeliverVanityCollectionItem(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ArgInt(L, 1));
        Packet(0x523).U8(2).U32(id).Send();
        return 0;
    }

    // FUN_10a4ebc0 / FUN_10a4ece0 / FUN_10a4ee00: a fixed site + the argument, opened by 0x86B790.
    int OpenUrl(lua_State* L, const char* base)
    {
        const std::string url = std::string(base) + CheckString(L, 1);
        reinterpret_cast<void(__cdecl*)(const char*)>(0x86B790)(url.c_str());
        return 0;
    }
    int OpenAscensionDBURL(lua_State* L)   { return OpenUrl(L, "https://db.ascension.gg/"); }
    int OpenAscensionURL(lua_State* L)     { return OpenUrl(L, "https://ascension.gg/"); }
    int OpenAscensionWikiURL(lua_State* L) { return OpenUrl(L, "https://project-ascension.fandom.com/wiki/Project_Ascension_Wiki/"); }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "AbandonSkillID", AbandonSkillID},
        {nullptr, "CharacterActivate", CharacterActivate},
        {nullptr, "CharacterDeactivate", CharacterDeactivate},
        {nullptr, "DeleteStablePet", DeleteStablePet},
        {nullptr, "GetCallstackHeight", GetCallstackHeight},
        {nullptr, "OpenAscensionDBURL", OpenAscensionDBURL},
        {nullptr, "OpenAscensionURL", OpenAscensionURL},
        {nullptr, "OpenAscensionWikiURL", OpenAscensionWikiURL},
        {nullptr, "RedraftAbilities", RedraftAbilities},
        {nullptr, "RequestDeliverVanityCollectionItem", RequestDeliverVanityCollectionItem},
        {nullptr, "RequestStatisticQuery", RequestStatisticQuery},
        {nullptr, "SendDraftModeSpellSelection", SendDraftModeSpellSelection},
        {nullptr, "SendDraftSkip", SendDraftSkip},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
