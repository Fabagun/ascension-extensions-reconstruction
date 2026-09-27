// C_Token (FUN_1033cf10 / FUN_1033d040 / FUN_1033d2d0) and the token manager behind it.
//
// SMSG_TOKEN_LIST 0x66F (FUN_1033c8a0): u32 n, then n x {str name, u32 amount, u32 extra}; unknown
// names are skipped without consuming their two dwords (as the original does).
// SMSG_TOKEN_UPDATE 0x670 (FUN_1033cb60): str name, u32 amount, u32 extra; fires
// TOKEN_UPDATED("%s%u%u", name, old amount, new amount) when either dword changed.
#include <Ascension/AscToken.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    // PTR_s_TOKEN_TYPE_SCROLL_OF_FORTUNE_I_10b58850: 20 tiers x {"", _ABILITIES, _TALENTS}, then
    // CALLBOARD_CACHE_POINTS and MAX.
    const std::vector<std::string>& TypeNames()
    {
        static std::vector<std::string> names;
        if (names.empty())
        {
            static const char* const kRoman[20] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
                "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX"};
            for (const char* suffix : {"", "_ABILITIES", "_TALENTS"})
                for (const char* r : kRoman)
                    names.push_back(std::string("TOKEN_TYPE_SCROLL_OF_FORTUNE_") + r + suffix);
            names.push_back("TOKEN_TYPE_CALLBOARD_CACHE_POINTS");
            names.push_back("TOKEN_TYPE_MAX");
        }
        return names;
    }

    struct Entry { uint32_t type = AscToken::kCount, pad = 0; int32_t amount = 0; uint32_t extra = 0; };

    std::vector<Entry>& Tokens();
    // FUN_1033c260 (every glue screen): every entry's amount (+8) and second dword (+0xC) zeroed.
    void ResetAmounts()
    {
        for (Entry& e : Tokens())
        {
            e.amount = 0;
            e.extra = 0;
        }
    }

    std::vector<Entry>& Tokens()
    {
        static std::vector<Entry> v;
        if (v.empty())
        {
            v.resize(AscToken::kCount);
            for (uint32_t i = 0; i < v.size(); ++i)
                v[i].type = i;
        }
        return v;
    }

    uint32_t ReadU32(CDataStore* p) { uint32_t v = *reinterpret_cast<const uint32_t*>(p->m_buffer + p->m_read); p->m_read += 4; return v; }
    std::string ReadStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    void __cdecl OnTokenList(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        for (uint32_t n = ReadU32(pkt); n; --n)
        {
            const std::string name = ReadStr(pkt);
            uint32_t i;
            if (!AscToken::TypeIndex(name.c_str(), i) || i >= Tokens().size())
                continue;
            Tokens()[i].amount = static_cast<int32_t>(ReadU32(pkt));
            Tokens()[i].extra = ReadU32(pkt);
        }
    }

    void __cdecl OnTokenUpdate(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        const std::string name = ReadStr(pkt);
        uint32_t i;
        if (!AscToken::TypeIndex(name.c_str(), i) || i >= Tokens().size())
            return;
        Entry& e = Tokens()[i];
        const int32_t oldAmount = e.amount;
        const uint32_t oldExtra = e.extra;
        e.amount = static_cast<int32_t>(ReadU32(pkt));
        e.extra = ReadU32(pkt);
        if (oldAmount != e.amount || oldExtra != e.extra)
            AscRuntime::Signal("TOKEN_UPDATED", "%s%u%u", name.c_str(), oldAmount, e.amount);
    }

    AscDbc::Table& TokenTypes() { return AscDbc::Get("DBFilesClient\\TokenTypes.dbc"); }

    int GetTokenAmount(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        uint32_t i;
        if (!AscToken::TypeIndex(name.c_str(), i))
            return 0;
        PushNum(L, AscToken::Amount(i));
        return 1;
    }

    // Row: +4 name, +8/+0xC/+0x10 localized, +0x14/+0x18 strings, +0x1C bool, +0x20 int.
    int GetTokenInfo(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        AscDbc::Table& t = TokenTypes();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (!row || name != t.Str(row, 4))
                continue;
            for (uint32_t off = 8; off <= 0x18; off += 4)
                PushStr(L, t.Str(row, off));
            PushBool(L, AscDbc::Table::I32(row, 0x1C) != 0);
            PushInt(L, AscDbc::Table::I32(row, 0x20));
            return 7;
        }
        return PushNils(L, 7);
    }

    // FUN_100beb90: { [1] = name, ... } in id order.
    int GetTokenTypes(lua_State* L)
    {
        std::vector<std::string> names;
        AscDbc::Table& t = TokenTypes();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* row = t.Row(id))
                names.push_back(t.Str(row, 4));
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < names.size(); ++i)
        {
            PushNum(L, i + 1);
            PushStr(L, names[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&ResetAmounts);   // FUN_1033c260
        sDC.AddPacketHandler(0x66F, CNetClientCustomPacket((void*)&OnTokenList, nullptr));
        sDC.AddPacketHandler(0x670, CNetClientCustomPacket((void*)&OnTokenUpdate, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Token", "GetTokenAmount", GetTokenAmount},
        {"C_Token", "GetTokenInfo", GetTokenInfo},
        {"C_Token", "GetTokenTypes", GetTokenTypes},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

const char* AscToken::TypeName(uint32_t index)
{
    const std::vector<std::string>& names = TypeNames();
    return index < names.size() ? names[index].c_str() : nullptr;
}

bool AscToken::TypeIndex(const char* name, uint32_t& index)
{
    const std::vector<std::string>& names = TypeNames();
    for (uint32_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
        {
            index = i;
            return true;
        }
    return false;
}

bool AscToken::AtLeast(uint32_t index, uint32_t lo, uint32_t hi)
{
    const uint32_t amount = index < Tokens().size() ? static_cast<uint32_t>(Tokens()[index].amount) : 0;
    const uint32_t extra = index < Tokens().size() ? Tokens()[index].extra : 0;
    return extra > hi || (extra == hi && amount >= lo);
}

int32_t AscToken::Amount(uint32_t index)
{
    return index < Tokens().size() ? Tokens()[index].amount : 0;
}

uint64_t AscToken::Amount64(uint32_t index)
{
    if (index >= Tokens().size())
        return 0;
    return (static_cast<uint64_t>(Tokens()[index].extra) << 32) | static_cast<uint32_t>(Tokens()[index].amount);
}
