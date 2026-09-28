#pragma once
// The original's render module (installer FUN_10265d20). Its nineteen CVars are queued at attach: fourteen
// through FUN_10114540 (registered after world load; AscGraphics.cpp queues them for the world
// registration) and five through FUN_101145c0 (the glue CVar vector, CVar.cpp).
#include <cstddef>

namespace AscGraphics
{
    struct CVarSpec
    {
        const char* name;
        const char* defaultValue;
        void* callback;     // __cdecl (CVar*, const char* previous, const char* value, void* arg)
        bool a7;            // the registrar's last argument (1 only for fogOverride)
        bool worldList;     // FUN_10114540 (after world load) rather than FUN_101145c0 (glue)
    };

    const CVarSpec* CVars(size_t& count);
}
