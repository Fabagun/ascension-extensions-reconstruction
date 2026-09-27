#pragma once
// JSON -> Lua for native subsystems (AscJson.cpp; FUN_100a4580).
#include <nlohmann/json.hpp>

struct lua_State;

namespace AscJson
{
    // FUN_100a4580: the value as a Lua table (objects and arrays; digit-only keys become index + 1).
    void Push(lua_State* L, const nlohmann::json& value);
}
