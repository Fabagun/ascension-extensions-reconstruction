// Widget-method registration: adding methods to the client's widget method tables.
//
// The original detours three client registrars (hook objects 0x10bcd0e0, 0x10bccbac, 0x10bccd90 via
// FUN_101aec30): after the stock registrar leaves the method table on the stack it adds its own
// entries with settable(-3):
//   0x49E540 Frame    (FUN_10a46770)  GetAllAttributes
//   0x57C040 Minimap  (FUN_10a43680)  IsInside, GetViewRadius, SetPOIMaxRange
//   0x9603D0 Model    (FUN_10a467e0)  SetSpell, SetSpellVisual, GetSpell, SetUnitGUID, SetSheathe
//   0x970610 MovieFrame (FUN_101ae890, installer FUN_101aeac0)  SetVolume
//
// Each registrar is `push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]` (7 bytes, detoured) and leaves the widget's
// method table on the stack; the Minimap / Model / MovieFrame registrars call the Frame one (0x49E540) first, so
// Frame's additions reach them too, as in the original. (Until 2026-09-27 this repointed the registrars'
// `push count / push table` immediates at extended copies of the stock tables instead.)
//
// The added methods themselves are ordinary AscBindings entries with namespace "#Frame" /
// "#Minimap" / "#Model" (AscBindings::ForEachWidgetMethod).

#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>

#include <Windows.h>
#include <cstdint>
#include <cstring>

using namespace AscLua;

namespace
{
    // One detour per registrar: the original, then each "#<widget>" binding set into the table on the stack.
    typedef int(__cdecl* Registrar_t)(lua_State*);
    struct Site
    {
        const char* widget;
        uint32_t    target;
        Registrar_t original;
    };
    // Census markers (the table below is detoured in ApplyPatches):
    // hooked 0x49E540
    // hooked 0x57C040
    // hooked 0x9603D0
    // hooked 0x970610
    Site g_sites[] = {
        {"Frame", 0x49E540, nullptr},        // FUN_10a46770
        {"Minimap", 0x57C040, nullptr},      // FUN_10a43680
        {"Model", 0x9603D0, nullptr},        // FUN_10a467e0
        {"MovieFrame", 0x970610, nullptr},   // FUN_101ae890
    };

    void AddMethod(const char* name, lua_CFunction fn, void* ctx)
    {
        lua_State* L = static_cast<lua_State*>(ctx);
        lua_pushstring(L, name ? name : "");
        lua_pushcclosure(L, fn, 0);
        lua_settable(L, -3);
    }

    template <int I>
    int __cdecl RegistrarDetour(lua_State* L)
    {
        const int r = g_sites[I].original(L);
        AscBindings::ForEachWidgetMethod(g_sites[I].widget, &AddMethod, L);
        return r;
    }
    void* const kDetours[] = {reinterpret_cast<void*>(&RegistrarDetour<0>), reinterpret_cast<void*>(&RegistrarDetour<1>),
                              reinterpret_cast<void*>(&RegistrarDetour<2>), reinterpret_cast<void*>(&RegistrarDetour<3>)};

    // ---- Frame:GetAllAttributes (FUN_10a6bde0) -------------------------------------------------
    // self[0] is the widget object; its attribute list starts at +0x1D8 (low bit set = empty) and
    // links through +0x1D0 bytes into each node; node +0x14 name, +0x18 registry ref of the value.
    typedef void  (__cdecl* RawGetI_t)(lua_State*, int, int);
    typedef void* (__cdecl* ToUserdata_t)(lua_State*, int);
    const RawGetI_t    lua_rawgeti    = reinterpret_cast<RawGetI_t>(0x84E670);
    const ToUserdata_t lua_touserdata = reinterpret_cast<ToUserdata_t>(0x84E1C0);

    int GetAllAttributes(lua_State* L)
    {
        lua_rawgeti(L, 1, 0);
        const uint8_t* frame = static_cast<const uint8_t*>(lua_touserdata(L, -1));
        lua_settop(L, -2);
        lua_createtable(L, 0, 0);
        if (!frame)
            return 1;
        const uint32_t next = *reinterpret_cast<const uint32_t*>(frame + 0x1D0);
        uint32_t node = *reinterpret_cast<const uint32_t*>(frame + 0x1D8);
        if (node & 1)
            node = 0;
        while (node)
        {
            const char* name = *reinterpret_cast<const char* const*>(node + 0x14);
            lua_pushstring(L, name ? name : "");
            lua_rawgeti(L, -10000, *reinterpret_cast<const int*>(node + 0x18));
            lua_settable(L, -3);
            const uint32_t n = *reinterpret_cast<const uint32_t*>(node + 4 + next);
            node = (n & 1) ? 0 : n;
        }
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {"#Frame", "GetAllAttributes", GetAllAttributes},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));

}

namespace AscWidget
{
    // At attach, before any Lua state exists.
    void ApplyPatches()
    {
        for (size_t i = 0; i < sizeof(g_sites) / sizeof(g_sites[0]); ++i)
            g_sites[i].original = reinterpret_cast<Registrar_t>(AscRuntime::Detour(g_sites[i].target, 7, kDetours[i]));
    }
}
