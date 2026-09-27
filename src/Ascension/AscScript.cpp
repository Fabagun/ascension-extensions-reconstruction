#include <Ascension/AscScript.hpp>
#include <Ascension/AscLog.hpp>
#include <cstring>
#include <Windows.h>
#include <shellapi.h>
#include <vector>
#pragma comment(lib, "shell32.lib")

namespace AscScript
{
namespace
{
    const char* const kExpected[] = {
        "nil", "boolean", "lightuserdata", "number", "string", "table", "function", "userdata", "thread",
    };

    typedef double(__cdecl* CheckNumber_t)(lua_State*, int);
    typedef const char*(__cdecl* CheckLString_t)(lua_State*, int, size_t*);
    typedef void(__thiscall* Ctor_t)(CDataStore*);
    typedef void(__thiscall* PutU32_t)(CDataStore*, uint32_t);
    typedef void(__thiscall* PutData_t)(CDataStore*, const void*, uint32_t);
    typedef void(__thiscall* PutString_t)(CDataStore*, const char*);
    typedef void(__thiscall* Finalize_t)(CDataStore*);
    typedef void(__thiscall* Dtor_t)(CDataStore*);
    typedef void(__cdecl* Send_t)(CDataStore*);
    typedef uint64_t(__cdecl* ActivePlayer_t)();
    typedef void*(__cdecl* ObjectPtr_t)(uint64_t, uint32_t, const char*, int);
}

// FUN_10309e50. Trailing nil slots are dropped from the required COUNT but still type-checked (a
// nil slot only rejects a present, non-nil argument). The original also logs the addon, function
// and object names; the index and expected type are what identify the call.
bool ValidateInput(lua_State* L, std::initializer_list<int> types)
{
    return ValidateInput(L, types.begin(), types.size());
}

bool ValidateInput(lua_State* L, const int* t, size_t count)
{
    int required = static_cast<int>(count);
    while (required > 0 && t[required - 1] == NIL)
        --required;

    const int top = AscLua::lua_gettop(L);
    if (top < required)
    {
        AscLog::Printf("Script::ValidateInput Invalid argument count. Expected %d, got %d.", required, top);
        return false;
    }

    int idx = 1;
    for (size_t i = 0; i < count; ++i)
    {
        const int want = t[i];
        bool ok;
        switch (want)
        {
        case NIL:     ok = AscLua::lua_type(L, idx) <= 0; break;
        case BOOLEAN: { int ty = AscLua::lua_type(L, idx); ok = ty == 0 || ty == 1 || ty == 3; break; }
        case NUMBER:  ok = AscLua::lua_isnumber(L, idx) != 0; break;
        case STRING:  ok = AscLua::lua_isstring(L, idx) != 0; break;
        case LIGHTUSERDATA: case TABLE: case FUNCTION: case USERDATA: case THREAD:
            ok = AscLua::lua_type(L, idx) == want; break;
        default:      ok = false; break;
        }
        if (!ok)
        {
            AscLog::Printf("Script::ValidateInput Invalid argument type at index %d. Expected %s.", idx,
                           (want >= 0 && want <= THREAD) ? kExpected[want] : "unknown");
            return false;
        }
        ++idx;
    }
    return true;
}

double CheckNumber(lua_State* L, int idx)
{
    return reinterpret_cast<CheckNumber_t>(0x84FAB0)(L, idx);
}

const char* CheckString(lua_State* L, int idx)
{
    return reinterpret_cast<CheckLString_t>(0x84F9F0)(L, idx, nullptr);
}

bool ReadNumber(lua_State* L, uint32_t& out)
{
    if (!ValidateInput(L, {NUMBER}))
        return false;
    out = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
    return true;
}

bool ReadString(lua_State* L, std::string& out)
{
    if (!ValidateInput(L, {STRING}))
        return false;
    out.assign(CheckString(L, 1));
    return true;
}

bool ReadInt2(lua_State* L, int32_t& a, int32_t& b)
{
    if (!ValidateInput(L, {NUMBER, NUMBER}))
        return false;
    a = ToInt(CheckNumber(L, 1));
    b = ToInt(CheckNumber(L, 2));
    return true;
}

bool ReadBool(lua_State* L, bool& out)
{
    if (!ValidateInput(L, {BOOLEAN}))
        return false;
    out = AscLua::lua_toboolean(L, 1) != 0;
    return true;
}

Packet::Packet(uint32_t opcode)
{
    reinterpret_cast<Ctor_t>(0x401050)(&m_store);
    reinterpret_cast<PutU32_t>(0x47B0A0)(&m_store, opcode);
}

Packet::~Packet()
{
    reinterpret_cast<Dtor_t>(0x403880)(&m_store);
}

Packet& Packet::Data(const void* p, uint32_t n)
{
    reinterpret_cast<PutData_t>(0x47B1C0)(&m_store, p, n);
    return *this;
}

Packet& Packet::Str(const char* s)
{
    reinterpret_cast<PutString_t>(0x47B300)(&m_store, s ? s : "");
    return *this;
}

// FUN_100de790: mask byte, then each non-zero byte of the guid, low byte first.
Packet& Packet::PackedGuid(uint64_t guid)
{
    uint8_t buf[9] = {};
    int n = 1;
    for (int i = 0; i < 8 && guid; ++i, guid >>= 8)
    {
        if (guid & 0xFF)
        {
            buf[0] |= static_cast<uint8_t>(1u << i);
            buf[n++] = static_cast<uint8_t>(guid & 0xFF);
        }
    }
    return Data(buf, static_cast<uint32_t>(n));
}

void Packet::Send()
{
    reinterpret_cast<Finalize_t>(0x401130)(&m_store);
    // 0x6B0B50 is exactly the DLL's inlined pair: ecx = [0xC79CF4] (what 0x6B0970 checks), then
    // __thiscall 0x632B50(packet).
    reinterpret_cast<Send_t>(0x6B0B50)(&m_store);
}

bool FetchSpell(uint32_t spellId, uint8_t* rec)
{
    typedef int(__thiscall* GetRec_t)(void*, uint32_t, void*);
    return reinterpret_cast<GetRec_t>(0x4CFD20)(reinterpret_cast<void*>(0xAD49D0), spellId, rec) != 0;
}

bool UnitHasAuraSpell(void* unit, uint32_t spellId)
{
    return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x7282A0)(unit, spellId) != 0;
}

uint64_t ActivePlayerGuid()
{
    return reinterpret_cast<ActivePlayer_t>(0x4D3790)();
}

const char* CommandLineArg(const char* name)
{
    // The DLL keeps argv as a vector<std::string> built once at startup; so do we.
    static std::vector<std::string> argv;
    static bool parsed = false;
    if (!parsed)
    {
        parsed = true;
        int argc = 0;
        if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 0; i < argc; ++i)
            {
                char buf[1024];
                WideCharToMultiByte(CP_ACP, 0, w[i], -1, buf, sizeof(buf), nullptr, nullptr);
                argv.emplace_back(buf);
            }
            LocalFree(w);
        }
    }
    for (size_t i = 1; i + 1 < argv.size(); ++i)
    {
        const char* a = argv[i].c_str();
        if (*a != '-' && *a != '/')
            continue;
        ++a;
        if (*a == '-')
            ++a;
        if (strcmp(a, name) == 0)
            return argv[i + 1].c_str();
    }
    return nullptr;
}

uint8_t* ActivePlayer()
{
    return reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)();
}

bool UnitTokenGuid(const char* token, uint64_t& guid)
{
    guid = 0;
    return (reinterpret_cast<uint32_t(__cdecl*)(const char*, uint64_t*, int)>(0x60ABF0)(token, &guid, 0) & 0xFF) != 0;
}

const uint8_t* ClientDbcRow(uint32_t container, uint32_t id)
{
    const uint32_t rows = *reinterpret_cast<const uint32_t*>(container + 0x20);
    if (!rows || id < *reinterpret_cast<const uint32_t*>(container + 0x10) || id > *reinterpret_cast<const uint32_t*>(container + 0x0C))
        return nullptr;
    return *reinterpret_cast<const uint8_t* const*>(rows + (id - *reinterpret_cast<const uint32_t*>(container + 0x10)) * 4);
}

void* ObjectPtr(uint64_t guid, uint32_t typeMask)
{
    return reinterpret_cast<ObjectPtr_t>(0x4D4DB0)(guid, typeMask, "AscScript", 0);
}
bool InflatePacket(CDataStore* p, std::vector<uint8_t>& out)
{
    const uint32_t total = static_cast<uint32_t>(p->m_size - p->m_read);
    std::vector<uint8_t> in(p->m_buffer + p->m_read, p->m_buffer + p->m_read + total);
    p->m_read += static_cast<int32_t>(total);
    if (total < 4)
        return false;
    uint32_t size;
    memcpy(&size, in.data(), 4);
    out.resize(size);
    uint32_t srcLen = total - 4;
    const int r = reinterpret_cast<int(__stdcall*)(void*, uint32_t*, const void*, uint32_t*)>(0x778180)(
        out.data(), &size, in.data() + 4, &srcLen);
    if (r != 0)
        return false;
    out.resize(size);
    return true;
}

}
