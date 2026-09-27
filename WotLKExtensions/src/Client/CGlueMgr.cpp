#include <Client/CGlueMgr.hpp>
#include <Client/CustomLua.hpp>
#include <Client/FrameScript.hpp>
#include <Misc/DataContainer.hpp>
#include <Misc/Util.hpp>
#include <Ascension/AscRegistry.hpp>

void CGlueMgr::ApplyPatches()
{
    Util::OverwriteUInt32AtAddress(0x4DA71D, reinterpret_cast<uint32_t>(&LoadScriptFunctionsCustom) - 0x4DA721);

    RegisterFunctions();
}

int32_t CGlueMgr::LoadScriptFunctionsCustom()
{
    auto& luaFunctionMap = sDC.GetGlueLuaFunctionMap();

    for (auto& it : luaFunctionMap)
        FrameScript::RegisterFunction(it.first, it.second);

    int32_t r = LoadFunctions();
    AscRegistry::OnGlueLoad();
    return r;
}

void CGlueMgr::AddToFunctionMap(const char* name, void* ptr)
{
    sDC.AddGlueLuaFunction(name, ptr);
}

// The glue map is empty: the upstream FlashGameWindow it used to register does not exist in the original
// client (live Lua-registry capture, 2026-09-27).
void CGlueMgr::RegisterFunctions()
{
}

int32_t CGlueMgr::LoadFunctions()
{
    return reinterpret_cast<int32_t (__cdecl*)()>(0x60A120)();
}
