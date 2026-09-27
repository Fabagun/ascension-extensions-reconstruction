#pragma once
struct lua_State;

// Real C_CharacterAdvancement bindings over AscensionCASvc state + CharacterAdvancement.dbc.
// Must run after AscStubs::RegisterAll so it wins over the generated placeholders.
namespace AscCharAdvancementApi
{
    void Register(lua_State* L);
}
