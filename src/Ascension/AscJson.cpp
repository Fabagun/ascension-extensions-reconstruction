// The original's JSON surface, over the same nlohmann::json 3.11.2 it links (third_party/nlohmann).
//
//   GenericJsonDataStoreMgr (FUN_10258ba0 -> 0x10BE267C): category -> id -> payload string, both
//   std::unordered_maps (so, under MSVC, the same hashes and iteration order as the original).
//     SMSG 0x77C AREA_POI_PAYLOAD, 0x77D MISC_PLAYER_DATA_PAYLOAD, 0x77E FELFORGED_RESET_PAYLOAD,
//     0x77F CHARACTER_ADVANCEMENT_LOADOUT_ACTIVE_PAYLOAD, 0x780 ..._LOADOUT_OWNED_PAYLOAD:
//     {u32 id, cstring payload}; 0x77B OVERWRITE_LFG_DATA_PAYLOAD: {cstring payload}, id 0.
//     The setter FUN_1025a6c0 insert-or-assigns and, in world (0xBD0792), signals client event 0x2CC
//     with ("%s%u", "ASCENSION_CUSTOM_JSON_<category>_UPDATE", id).
//     Each world registration (the tail of registrar FUN_10257740, list 0x10BE2A2C) empties eleven
//     BUILD_DRAFT_* / BUILD_CREATOR_* categories (FUN_10258190); the first name carries the original's
//     trailing space, so it creates and empties a category nothing else uses.
//   DecodeJSON(text): json::parse (exceptions on, no comments) inside a catch-all -> the table
//     FUN_100a4580 builds, or nothing.
//   DecodeJSONContent(name): JSONContentMgr (0x10BE2FA8) name -> json, cached as a registry ref of
//     the table built in the client's main Lua state (0x817DB0). Nothing in the plain code fills the
//     map (its only writer is in the VM-protected region), so this returns nothing.
//   LoadAscensionContentJSON(name): the text of <client dir (0x4218B0)>\Content\<name>.json, "" when
//     missing.
//
// JSON -> Lua (FUN_100a4580 / FUN_100a3d60): objects and arrays become tables; a digit-only key k becomes
// the integer k + 1 (so arrays are 1-based), any other key a string; strings, booleans; integers
// through lua_pushinteger; floats through lua_pushnumber((float)d); anything else nil.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <nlohmann/json.hpp>
#include <Ascension/AscJson.hpp>
#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace AscScript;
using nlohmann::json;

namespace
{
    // ---- json -> Lua --------------------------------------------------------------------------------------
    void PushKey(lua_State* L, const std::string& key)   // FUN_100a3d60's key half
    {
        bool digits = !key.empty();
        for (char c : key)
            if (!isdigit(static_cast<unsigned char>(c)))
            {
                digits = false;
                break;
            }
        if (digits)
            PushInt(L, atoi(key.c_str()) + 1);
        else
            PushStr(L, key.c_str());
    }

    void PushValue(lua_State* L, const json& v);
    void PushItems(lua_State* L, const json& v)   // FUN_100a4580 (the table) / FUN_100a3d60 containers
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const auto& el : v.items())
        {
            PushKey(L, el.key());
            PushValue(L, el.value());
            AscLua::lua_settable(L, -3);
        }
    }

    void PushValue(lua_State* L, const json& v)
    {
        switch (v.type())
        {
        case json::value_t::object:
        case json::value_t::array: PushItems(L, v); break;
        case json::value_t::string: PushStr(L, v.get_ref<const std::string&>().c_str()); break;
        case json::value_t::boolean: PushBool(L, v.get<bool>()); break;
        case json::value_t::number_integer: PushInt(L, static_cast<int32_t>(v.get<int64_t>())); break;
        case json::value_t::number_unsigned: PushInt(L, static_cast<int32_t>(v.get<uint64_t>())); break;
        case json::value_t::number_float: PushNum(L, static_cast<double>(static_cast<float>(v.get<double>()))); break;
        default: AscLua::lua_pushnil(L); break;
        }
    }

    // ---- GenericJsonDataStoreMgr --------------------------------------------------------------------------
    std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> g_cache;

    void Set(const std::string& category, uint32_t id, const std::string& payload)   // FUN_1025a6c0
    {
        g_cache[category].insert_or_assign(id, payload);
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
        {
            const std::string name = "ASCENSION_CUSTOM_JSON_" + category + "_UPDATE";
            reinterpret_cast<int(__cdecl*)(int, const char*, ...)>(0x81B530)(0x2CC, "%s%u", name.c_str(), id);
        }
    }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadCString(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    template <const char* Category> void __cdecl OnPayload(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t id = Read<uint32_t>(p);
        Set(Category, id, ReadCString(p));
    }
    extern const char kAreaPoi[] = "AREA_POI_PAYLOAD";
    extern const char kMiscPlayer[] = "MISC_PLAYER_DATA_PAYLOAD";
    extern const char kFelforgedReset[] = "FELFORGED_RESET_PAYLOAD";
    extern const char kLoadoutActive[] = "CHARACTER_ADVANCEMENT_LOADOUT_ACTIVE_PAYLOAD";
    extern const char kLoadoutOwned[] = "CHARACTER_ADVANCEMENT_LOADOUT_OWNED_PAYLOAD";

    void __cdecl OnOverwriteLfg(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10259430
    {
        Set("OVERWRITE_LFG_DATA_PAYLOAD", 0, ReadCString(p));
    }

    void ResetBuildPayloads()   // FUN_10258190
    {
        static const char* const kCategories[] = {
            "BUILD_DRAFT_DRAFTABLE_PAYLOAD ", "BUILD_DRAFT_SINGLE_DRAFTABLE_PAYLOAD", "BUILD_DRAFT_ROLE_PROMPT_PAYLOAD",
            "BUILD_DRAFT_DRAFTED_PAYLOAD", "BUILD_DRAFT_COMPLETED_PAYLOAD", "BUILD_CREATOR_SUMMARY_PAYLOAD",
            "BUILD_CREATOR_DATA_PAYLOAD", "BUILD_CREATOR_ACTIVE_PAYLOAD", "BUILD_CREATOR_OWNED_PAYLOAD",
            "BUILD_CREATOR_UPVOTED_PAYLOAD", "BUILD_CREATOR_SAVE_RESULT_PAYLOAD",
        };
        for (const char* c : kCategories)
            g_cache[c].clear();
    }

    // ClearJsonCache([category [, id]]): everything, one category (created if missing), or one entry.
    int ClearJsonCache(lua_State* L)
    {
        switch (AscLua::lua_gettop(L))
        {
        case 0: g_cache.clear(); break;
        case 1: g_cache[CheckString(L, 1)].clear(); break;
        case 2:
        {
            const std::string category = CheckString(L, 1);
            const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
            g_cache[category].erase(id);
            break;
        }
        default: break;
        }
        return 0;
    }

    int HasJsonCacheData(lua_State* L)
    {
        const std::string category = CheckString(L, 1);
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        auto c = g_cache.find(category);
        PushBool(L, c != g_cache.end() && c->second.count(id) != 0);
        return 1;
    }

    // GetJsonCacheCategory(category): the payloads (the category is created if missing), keyed 1..n.
    int GetJsonCacheCategory(lua_State* L)
    {
        const std::string category = CheckString(L, 1);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int32_t i = 0;
        for (const auto& e : g_cache[category])
        {
            PushNum(L, static_cast<double>(++i));
            PushStr(L, e.second.c_str());
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetJsonCacheData(lua_State* L)
    {
        const std::string category = CheckString(L, 1);
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const char* text = "";
        auto c = g_cache.find(category);
        if (c != g_cache.end())
        {
            auto e = c->second.find(id);
            if (e != c->second.end())
                text = e->second.c_str();
        }
        PushStr(L, text);
        return 1;
    }

    // ---- decoding -----------------------------------------------------------------------------------------
    int DecodeJSON(lua_State* L)   // FUN_1028c4e0
    {
        const std::string text = CheckString(L, 1);
        if (text.empty())
            return 0;
        try
        {
            const json root = json::parse(text, nullptr, true, false);
            PushItems(L, root);
            return 1;
        }
        catch (...)
        {
            return 0;
        }
    }

    // JSONContentMgr (FUN_1028c360): name -> {json, registry ref (-1 until built)}.
    struct Content { json value; int ref = -1; };
    std::unordered_map<std::string, Content> g_content;

    int DecodeJSONContent(lua_State* L)   // FUN_1028c660
    {
        const std::string name = CheckString(L, 1);
        if (name.empty())
            return 0;
        auto it = g_content.find(name);
        if (it == g_content.end())
            return 0;
        if (it->second.ref == -1)
        {
            lua_State* main = reinterpret_cast<lua_State*(__cdecl*)()>(0x817DB0)();
            PushItems(main, it->second.value);
            it->second.ref = reinterpret_cast<int(__cdecl*)(lua_State*, int)>(0x84F6C0)(main, -10000);   // luaL_ref
        }
        reinterpret_cast<void(__cdecl*)(lua_State*, int, int)>(0x84E670)(L, -10000, it->second.ref);   // lua_rawgeti
        return 1;
    }

    int LoadAscensionContentJSON(lua_State* L)   // FUN_100dc2d0
    {
        const std::string name = CheckString(L, 1);
        std::string text;
        if (!name.empty())
        {
            std::string path = reinterpret_cast<const char*(__cdecl*)()>(0x4218B0)();
            if (!path.empty() && path.back() != '\\' && path.back() != '/')
                path += '\\';
            path += "Content\\" + name + ".json";
            const DWORD attr = GetFileAttributesA(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                if (FILE* f = _fsopen(path.c_str(), "r", 0x40))
                {
                    char buf[4096];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                        text.append(buf, n);
                    fclose(f);
                }
        }
        PushStr(L, text.c_str());
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x77B, CNetClientCustomPacket((void*)&OnOverwriteLfg, nullptr));
        sDC.AddPacketHandler(0x77C, CNetClientCustomPacket((void*)&OnPayload<kAreaPoi>, nullptr));
        sDC.AddPacketHandler(0x77D, CNetClientCustomPacket((void*)&OnPayload<kMiscPlayer>, nullptr));
        sDC.AddPacketHandler(0x77E, CNetClientCustomPacket((void*)&OnPayload<kFelforgedReset>, nullptr));
        sDC.AddPacketHandler(0x77F, CNetClientCustomPacket((void*)&OnPayload<kLoadoutActive>, nullptr));
        sDC.AddPacketHandler(0x780, CNetClientCustomPacket((void*)&OnPayload<kLoadoutOwned>, nullptr));
        // FUN_102577d0 (-> FUN_10258190) on the glue-screen list; FUN_1025ac40 on the two Lua lists only
        // registers the natives.
        AscRuntime::OnGlueScreen(&ResetBuildPayloads);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "ClearJsonCache", ClearJsonCache},
        {nullptr, "HasJsonCacheData", HasJsonCacheData},
        {nullptr, "GetJsonCacheCategory", GetJsonCacheCategory},
        {nullptr, "GetJsonCacheData", GetJsonCacheData},
        {nullptr, "DecodeJSON", DecodeJSON},
        {nullptr, "DecodeJSONContent", DecodeJSONContent},
        // The world state also has it as _DecodeJSONContent (same function 0x1028C660; live capture 2026-09-27).
        {nullptr, "_DecodeJSONContent", DecodeJSONContent},
        {nullptr, "LoadAscensionContentJSON", LoadAscensionContentJSON},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

void AscJson::Push(lua_State* L, const nlohmann::json& value) { PushValue(L, value); }
