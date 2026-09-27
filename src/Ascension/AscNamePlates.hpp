#pragma once
// The original's nameplate module (installer FUN_102bf920). Its twelve CVars are queued through
// FUN_10114540 at attach, so they live in the glue CVar vector (CVar.cpp) with these callbacks.
#include <cstddef>

namespace AscNamePlates
{
    struct CVarSpec
    {
        const char* name;
        const char* defaultValue;
        void* callback;     // __cdecl (CVar*, const char* previous, const char* value, void* arg)
    };

    const CVarSpec* CVars(size_t& count);
}
