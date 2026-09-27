// C_Config (FUN_10198370 .. FUN_10198940) and SMSG_UPDATE_CONFIGS 0x58D (FUN_10196c90).
//
// The packet replaces the int, bool, float and rate maps wholesale:
//   u32 n x {u32 len, name[len], i32}   ints
//   u32 n x {u32 len, name[len], u8}    bools
//   u32 n x {u32 len, name[len], f32}   floats
//   u32 n x {u32 len, name[len], f32}   rates
// and, only if at least 8 bytes remain, merges into the vector maps:
//   u32 n x {u32 len, name[len], u32 k, i32[k]}   int vectors
//   u32 n x {u32 len, name[len], u32 k, f32[k]}   float vectors
// While in world (0xBD0792) each scalar whose value differs from the previous packet's (a missing
// previous value counts as 0 / false) fires INT_/BOOL_/FLOAT_/RATE_CONFIG_UPDATED(name, old, new).
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <type_traits>
#include <string>
#include <unordered_map>

using namespace AscScript;

namespace
{
    struct Store
    {
        std::unordered_map<std::string, int32_t> ints;
        std::unordered_map<std::string, bool> bools;
        std::unordered_map<std::string, float> floats;
        std::unordered_map<std::string, float> rates;
        std::unordered_map<std::string, std::vector<int32_t>> intVectors;
        std::unordered_map<std::string, std::vector<float>> floatVectors;
    } g_store;

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadName(CDataStore* p)
    {
        const uint32_t len = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        return s;
    }

    bool InWorld() { return *reinterpret_cast<const uint8_t*>(0xBD0792) != 0; }

    template <class T, class Fire>
    void ReadScalars(CDataStore* p, std::unordered_map<std::string, T>& map, Fire fire)
    {
        const std::unordered_map<std::string, T> previous = map;
        map.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const std::string name = ReadName(p);
            const T value = Read<T>(p);
            map[name] = value;
            if (!InWorld())
                continue;
            const auto it = previous.find(name);
            if (it != previous.end() && it->second == value)
                continue;
            fire(name, it != previous.end() ? it->second : T{}, value);
        }
    }

    template <class T>
    void ReadVectors(CDataStore* p, std::unordered_map<std::string, std::vector<T>>& map)
    {
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const std::string name = ReadName(p);
            std::vector<T> values;
            for (uint32_t k = Read<uint32_t>(p); k; --k)
                values.push_back(Read<T>(p));
            map[name] = values;
        }
    }

    void __cdecl OnUpdateConfigs(void*, uint32_t, uint32_t, CDataStore* p)
    {
        ReadScalars<int32_t>(p, g_store.ints, [](const std::string& n, int32_t o, int32_t v)
            { AscRuntime::Signal("INT_CONFIG_UPDATED", "%s%u%u", n.c_str(), o, v); });
        ReadScalars<bool>(p, g_store.bools, [](const std::string& n, bool o, bool v)
            { AscRuntime::Signal("BOOL_CONFIG_UPDATED", "%s%b%b", n.c_str(), static_cast<int>(o), static_cast<int>(v)); });
        ReadScalars<float>(p, g_store.floats, [](const std::string& n, float o, float v)
            { AscRuntime::Signal("FLOAT_CONFIG_UPDATED", "%s%f%f", n.c_str(), static_cast<double>(o), static_cast<double>(v)); });
        ReadScalars<float>(p, g_store.rates, [](const std::string& n, float o, float v)
            { AscRuntime::Signal("RATE_CONFIG_UPDATED", "%s%f%f", n.c_str(), static_cast<double>(o), static_cast<double>(v)); });
        if (static_cast<uint32_t>(p->m_size - p->m_read) > 7)
        {
            ReadVectors<int32_t>(p, g_store.intVectors);
            ReadVectors<float>(p, g_store.floatVectors);
        }
    }

    template <class M>
    const typename M::mapped_type* Find(const M& map, const char* name)
    {
        const auto it = map.find(name);
        return it != map.end() ? &it->second : nullptr;
    }

    int GetIntConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const int32_t* v = AscConfig::Int(name.c_str()))
            PushInt(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetBoolConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const bool* v = AscConfig::Bool(name.c_str()))
            PushBool(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetFloatConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const float* v = AscConfig::Float(name.c_str()))
            PushNum(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetRateConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const float* v = AscConfig::Rate(name.c_str()))
            PushNum(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    template <class T>
    void PushArray(lua_State* L, const std::vector<T>& v)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, i + 1);
            if constexpr (std::is_same_v<T, float>)
                PushNum(L, v[i]);
            else
                PushInt(L, v[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    int GetIntVectorConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const std::vector<int32_t>* v = AscConfig::IntVector(name.c_str()))
            PushArray(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetFloatVectorConfig(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        if (const std::vector<float>* v = AscConfig::FloatVector(name.c_str()))
            PushArray(L, *v);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x58D, CNetClientCustomPacket((void*)&OnUpdateConfigs, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Config", "GetIntConfig", GetIntConfig},
        {"C_Config", "GetBoolConfig", GetBoolConfig},
        {"C_Config", "GetFloatConfig", GetFloatConfig},
        {"C_Config", "GetRateConfig", GetRateConfig},
        {"C_Config", "GetIntVectorConfig", GetIntVectorConfig},
        {"C_Config", "GetFloatVectorConfig", GetFloatVectorConfig},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

const int32_t* AscConfig::Int(const char* name)                    { return Find(g_store.ints, name); }
const bool* AscConfig::Bool(const char* name)                      { return Find(g_store.bools, name); }
const float* AscConfig::Float(const char* name)                    { return Find(g_store.floats, name); }
const float* AscConfig::Rate(const char* name)                     { return Find(g_store.rates, name); }
const std::vector<int32_t>* AscConfig::IntVector(const char* name) { return Find(g_store.intVectors, name); }
const std::vector<float>* AscConfig::FloatVector(const char* name) { return Find(g_store.floatVectors, name); }
