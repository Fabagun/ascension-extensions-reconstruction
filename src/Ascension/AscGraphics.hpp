#pragma once
// The original's render module (installer FUN_10265d20). Its nineteen CVars are queued through
// FUN_10114540 / FUN_101145c0 at attach, so they live in the glue CVar vector (CVar.cpp).
#include <cstddef>

namespace AscGraphics
{
    struct CVarSpec
    {
        const char* name;
        const char* defaultValue;
        void* callback;     // __cdecl (CVar*, const char* previous, const char* value, void* arg)
        bool a7;            // the registrar's last argument (1 only for fogOverride)
    };

    const CVarSpec* CVars(size_t& count);
}
