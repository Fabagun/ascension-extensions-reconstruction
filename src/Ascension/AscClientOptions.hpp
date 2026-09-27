#pragma once
// The attach init's (FUN_10a66100) option CVars: queued through FUN_10114540 with the original's
// flags, defaults, categories and callbacks; registered from CVar.cpp.
#include <cstddef>
#include <cstdint>

namespace AscClientOptions
{
    struct CVarSpec
    {
        const char* name;
        const char* defaultValue;
        uint32_t flags;
        uint32_t category;
        void* callback;     // __cdecl (CVar*, const char* previous, const char* value, void* arg)
        const char* help = nullptr;   // most of the original's registrations pass none
        bool a7 = false;              // the registrar's last argument (1 only for useAlphaAmbienceSound)
    };

    const CVarSpec* CVars(size_t& count);

    // Q1 (FUN_10114540) CVars queued by other modules' inits; registered after the attach init's own.
    void QueueWorldCVar(const CVarSpec& spec);
    const CVarSpec* QueuedWorldCVars(size_t& count);
}
