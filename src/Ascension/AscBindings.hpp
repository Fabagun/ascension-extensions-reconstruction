#pragma once
#include <Ascension/AscLua.hpp>
#include <cstddef>

// Exact transcriptions of the original Extensions.dll's Lua bindings (see AscBindings.cpp).
namespace AscBindings
{
    struct Binding
    {
        const char* ns;     // C_* namespace, nullptr for a global, or "@Name" for a method of the
                            // registry metatable Name (also _G.Name, __index = itself), or "#Type" for a
                            // widget method of Frame / Minimap / Model (added to the client's method table)
        const char* name;
        lua_CFunction fn;
    };

    // A transcribed subsystem: its bindings, plus the attach-time wiring the original does in the
    // subsystem's init (event names, lifecycle callbacks). Declared at namespace scope in the
    // subsystem's .cpp:
    //     static AscBindings::Module s_module{kBindings, count, &Init};
    struct Module
    {
        Module(const Binding* bindings, size_t count, void (*init)() = nullptr);
    };

    // Runs every Module's init. Call once at attach, after AscRuntime::InstallHooks.
    void InitModules();

    // Call after every other registration pass: these replace earlier inferences. The glue state gets only
    // the natives the original registers there (AscBindingStates.generated.inc, measured from a live
    // capture of the near-original client); the world state gets all but the glue-only ones.
    void Register(lua_State* L, bool glue);

    // FUN_102783f0's list 0x10BE2A2C runs each time the world Lua state gets its natives; some modules
    // hang work on it. Register from a Module's init.
    void OnWorldRegistered(void (*cb)());
    void RunWorldRegistered();
    // FUN_102787e0's list 0x10BE284C runs right after the glue Lua state gets its natives (FUN_10278410's
    // list 0x10BE2A0C is the glue counterpart of 0x10BE2A2C). FUN_102787c0's 0x10BE2A4C, the world
    // "after" list, is folded into OnWorldRegistered.
    void OnGlueRegistered(void (*cb)());
    void RunGlueRegistered();

    // Every "#<widget>" entry (kBindings and modules), for AscWidget to add to the client's table.
    void ForEachWidgetMethod(const char* widget, void (*cb)(const char* name, lua_CFunction fn, void* ctx), void* ctx);
}
