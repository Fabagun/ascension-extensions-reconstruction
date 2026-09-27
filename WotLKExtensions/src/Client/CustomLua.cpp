#include <Client/CustomLua.hpp>
#include <Client/FrameScript.hpp>
#include <Ascension/AscRegistry.hpp>
#include <Misc/Util.hpp>

// Scaffold plumbing only: the call-site hook at 0x52AB17 runs the client's world Lua registration and then
// ours (AscRegistry::OnWorldLoad). The upstream out-of-band Lua functions this file used to register
// (FlashGameWindow, GetShapeshiftFormID, GetSpellNameById, ConvertCoordsToScreenSpace, the combat-rating,
// custom-file and dev-helper sets) were removed on 2026-09-27: none exists in the original client (live
// Lua-registry capture). GetSpellDescription and PortGraveyard are exact in AscSpellText / AscGlobalsC.
void CustomLua::ApplyPatches()
{
    Util::OverwriteUInt32AtAddress(0x52AB17, reinterpret_cast<uint32_t>(&LoadScriptFunctionsCustom) - 0x52AB1B);
}

int32_t CustomLua::LoadScriptFunctionsCustom()
{
    int32_t r = FrameScript::LoadFunctions();
    AscRegistry::OnWorldLoad();
    return r;
}
