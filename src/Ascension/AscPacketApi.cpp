// The original's Lua packet API (0x1030f1b0..0x1030fdc0): CreatePacket / Put* / Send, Get* on a received
// packet, RegisterPacket / ClearPacket to route an opcode to a Lua function.
//
// A packet is a 4-byte userdata holding a CDataStore*, given the registry metatable "CDataStore".
// Nothing in the original (or the client) ever creates that metatable, so as shipped CreatePacket
// returns nil and a RegisterPacket handler is called as fn(opcode, nil). Transcribed as-is. The only
// deviation: Put*/Get*/Send on a nil packet return without dereferencing it (the original crashes).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <string>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <unordered_map>

using namespace AscScript;

namespace
{
    constexpr int kRegistry = -10000;   // LUA_REGISTRYINDEX

    typedef void* (__cdecl* NewUserdata_t)(lua_State*, size_t);
    typedef void* (__cdecl* ToUserdata_t)(lua_State*, int);
    typedef void  (__cdecl* RawGet_t)(lua_State*, int);
    typedef void  (__cdecl* RawGetI_t)(lua_State*, int, int);
    typedef int   (__cdecl* SetMetatable_t)(lua_State*, int);
    typedef void  (__cdecl* CheckType_t)(lua_State*, int, int);
    typedef int   (__cdecl* Ref_t)(lua_State*, int);
    typedef int   (__cdecl* PCall_t)(lua_State*, int, int, int);

    NewUserdata_t  lua_newuserdata  = reinterpret_cast<NewUserdata_t>(0x84F0F0);
    ToUserdata_t   lua_touserdata   = reinterpret_cast<ToUserdata_t>(0x84E1C0);
    RawGet_t       lua_rawget       = reinterpret_cast<RawGet_t>(0x84E600);
    RawGetI_t      lua_rawgeti      = reinterpret_cast<RawGetI_t>(0x84E670);
    SetMetatable_t lua_setmetatable = reinterpret_cast<SetMetatable_t>(0x84EA90);
    CheckType_t    luaL_checktype   = reinterpret_cast<CheckType_t>(0x84F960);
    Ref_t          luaL_ref         = reinterpret_cast<Ref_t>(0x84F6C0);
    PCall_t        lua_pcall        = reinterpret_cast<PCall_t>(0x84EC50);

    std::unordered_map<uint16_t, int> g_handlers;   // 0x10be3e98: opcode -> registry ref

    // The shared tail of CreatePacket and the handler: wrap a CDataStore* and attach the metatable,
    // or leave nil in its place when "CDataStore" is not in the registry.
    void PushPacket(lua_State* L, CDataStore* store)
    {
        *static_cast<CDataStore**>(lua_newuserdata(L, 4)) = store;
        AscLua::lua_pushstring(L, "CDataStore");
        lua_rawget(L, kRegistry);
        if (AscLua::lua_type(L, -1) == LUA_TTABLE)
        {
            lua_setmetatable(L, -2);
        }
        else
        {
            AscLua::lua_settop(L, -3);
            AscLua::lua_pushnil(L);
        }
    }

    CDataStore* Arg(lua_State* L)
    {
        CDataStore** ud = static_cast<CDataStore**>(lua_touserdata(L, 1));
        return ud ? *ud : nullptr;
    }

    // FUN_1030f620: a heap CDataStore with the opcode already written.
    int CreatePacket(lua_State* L)
    {
        const uint32_t opcode = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        CDataStore* store = new CDataStore{};
        CDataStore::GenPacket(store);
        CDataStore::PutInt32(store, static_cast<int32_t>(opcode));
        PushPacket(L, store);
        return 1;
    }

    // handler_Send: Finalize, then 0x406F40 (ClientServices send wrapper).
    int Send(lua_State* L)
    {
        if (CDataStore* s = Arg(L))
        {
            reinterpret_cast<void(__thiscall*)(CDataStore*)>(0x401130)(s);
            reinterpret_cast<void(__cdecl*)(CDataStore*)>(0x406F40)(s);
        }
        return 0;
    }

    template <class T>
    void Put(lua_State* L, T v)
    {
        if (CDataStore* s = Arg(L))
            reinterpret_cast<void(__thiscall*)(CDataStore*, const void*, uint32_t)>(0x47B1C0)(s, &v, sizeof(T));
    }
    int PutBool(lua_State* L)   { Put<uint8_t>(L, AscLua::lua_toboolean(L, 2) != 0); return 0; }            // handler_PutBool
    int PutInt8(lua_State* L)   { Put<uint8_t>(L, static_cast<uint8_t>(static_cast<int>(CheckNumber(L, 2)))); return 0; }   // FUN_1030fb80
    int PutInt16(lua_State* L)  { Put<uint16_t>(L, static_cast<uint16_t>(static_cast<int>(CheckNumber(L, 2)))); return 0; } // FUN_1030fb00
    int PutInt32(lua_State* L)  { Put<int32_t>(L, static_cast<int>(CheckNumber(L, 2))); return 0; }             // handler_PutInt32
    int PutUInt32(lua_State* L) { Put<uint32_t>(L, static_cast<uint32_t>(ToInt(CheckNumber(L, 2)))); return 0; } // FUN_1030fcc0
    int PutFloat(lua_State* L)  { Put<float>(L, static_cast<float>(CheckNumber(L, 2))); return 0; }             // handler_PutFloat

    template <class T>
    bool Get(lua_State* L, T& out)
    {
        CDataStore* s = Arg(L);
        if (!s)
            return false;
        out = *reinterpret_cast<const T*>(s->m_buffer + s->m_read);
        s->m_read += sizeof(T);
        return true;
    }
    int GetBool(lua_State* L)   { uint8_t v;  if (!Get(L, v)) return 0; PushBool(L, v != 0); return 1; }
    int GetInt8(lua_State* L)   { int8_t v;   if (!Get(L, v)) return 0; PushInt(L, v); return 1; }
    int GetUInt8(lua_State* L)  { uint8_t v;  if (!Get(L, v)) return 0; PushInt(L, v); return 1; }
    int GetInt16(lua_State* L)  { int16_t v;  if (!Get(L, v)) return 0; PushInt(L, v); return 1; }
    int GetUInt16(lua_State* L) { uint16_t v; if (!Get(L, v)) return 0; PushInt(L, v); return 1; }
    int GetInt32(lua_State* L)  { int32_t v;  if (!Get(L, v)) return 0; PushInt(L, v); return 1; }   // also GetUInt32
    int GetFloat(lua_State* L)  { float v;    if (!Get(L, v)) return 0; PushNum(L, v); return 1; }
    // FUN_1030fbd0: CDataStore::PutString (0x47B300), NUL included.
    int PutString(lua_State* L)
    {
        CDataStore* s = Arg(L);
        const std::string v = CheckString(L, 2);
        if (s)
            reinterpret_cast<void(__thiscall*)(CDataStore*, const char*)>(0x47B300)(s, v.c_str());
        return 0;
    }

    // FUN_1030f8f0: a NUL-terminated string at the read cursor.
    int GetString(lua_State* L)
    {
        CDataStore* s = Arg(L);
        if (!s)
            return 0;
        const std::string v(reinterpret_cast<const char*>(s->m_buffer + s->m_read));
        s->m_read += static_cast<int32_t>(v.size() + 1);
        AscLua::lua_pushstring(L, v.c_str());
        return 1;
    }

    // FUN_1030f7c0: a u64 read, formatted by 0x74D0D0.
    int GetGUID(lua_State* L)
    {
        uint64_t v;
        if (!Get(L, v))
            return 0;
        char buf[32] = {};
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, char*)>(0x74D0D0)(static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32), buf);
        AscLua::lua_pushstring(L, buf);
        return 1;
    }

    // FUN_1030f410: the handler every RegisterPacket opcode is routed to.
    int __cdecl OnRegisteredPacket(void*, uint32_t opcode, uint32_t, CDataStore* msg)
    {
        auto it = g_handlers.find(static_cast<uint16_t>(opcode));
        if (it == g_handlers.end())
            return 1;
        lua_State* L = AscLua::GetState();
        lua_rawgeti(L, kRegistry, it->second);
        PushInt(L, static_cast<int32_t>(opcode & 0xFFFF));
        PushPacket(L, msg);
        if (lua_pcall(L, 2, 0, 0) != 0)
        {
            AscLog::Printf("RegisterPacket handler 0x%X: %s", opcode, AscLua::tostring(L, -1));
            AscLua::lua_settop(L, -2);
        }
        return 1;
    }

    // FUN_102c4400: above 0x55E the DLL's own table (assigning, i.e. replacing), otherwise the
    // client's SetMessageHandler (0x6B0B80).
    void RouteOpcode(uint32_t opcode)
    {
        if (opcode > 0x55E)
        {
            auto& map = sDC.GetPacketHandlerMap();
            map.erase(opcode);
            map.insert(std::make_pair(opcode, CNetClientCustomPacket((void*)&OnRegisteredPacket, nullptr)));
        }
        else
            reinterpret_cast<void(__cdecl*)(uint32_t, void*, void*)>(0x6B0B80)(opcode, (void*)&OnRegisteredPacket, nullptr);
    }

    // FUN_1030fd10: RegisterPacket(opcode, function)
    int RegisterPacket(lua_State* L)
    {
        const uint32_t opcode = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        g_handlers[static_cast<uint16_t>(opcode)] = luaL_ref(L, kRegistry);
        RouteOpcode(opcode);
        return 0;
    }

    // FUN_1030f550: forget the Lua function (the opcode stays routed; the handler then finds nothing).
    int ClearPacket(lua_State* L)
    {
        g_handlers.erase(static_cast<uint16_t>(ToInt(CheckNumber(L, 1))));
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "CreatePacket", CreatePacket}, {"@CDataStore", "Send", Send},
        {nullptr, "RegisterPacket", RegisterPacket}, {nullptr, "ClearPacket", ClearPacket},
        {"@CDataStore", "PutBool", PutBool}, {"@CDataStore", "PutInt8", PutInt8}, {"@CDataStore", "PutUInt8", PutInt8},
        {"@CDataStore", "PutInt16", PutInt16}, {"@CDataStore", "PutUInt16", PutInt16}, {"@CDataStore", "PutInt32", PutInt32},
        {"@CDataStore", "PutUInt32", PutUInt32}, {"@CDataStore", "PutFloat", PutFloat},
        {"@CDataStore", "GetBool", GetBool}, {"@CDataStore", "GetInt8", GetInt8}, {"@CDataStore", "GetUInt8", GetUInt8},
        {"@CDataStore", "GetInt16", GetInt16}, {"@CDataStore", "GetUInt16", GetUInt16}, {"@CDataStore", "GetInt32", GetInt32},
        {"@CDataStore", "GetUInt32", GetInt32}, {"@CDataStore", "GetFloat", GetFloat}, {"@CDataStore", "GetGUID", GetGUID}, {"@CDataStore", "GetString", GetString}, {"@CDataStore", "PutString", PutString},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
