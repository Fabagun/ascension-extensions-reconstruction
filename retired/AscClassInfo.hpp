#pragma once
struct lua_State;

// Real C_ClassInfo spec catalogue from ChrSpecs.dbc. Must run after the C_ClassInfo table in
// AscRegistry, so these win.
namespace AscClassInfo
{
    void Register(lua_State* L);

    // ChrSpecs id for a spec name ("Animation"/"ANIMATION"), 0 when unknown. The CA tab names and
    // the ChrSpecs spec names are the same words in different case.
    unsigned int SpecIdByName(const char* name);
}
