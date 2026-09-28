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
        void** out = nullptr;         // entry +0: receives the CVar* at every registration (0x10114090)
    };

    const CVarSpec* CVars(size_t& count);
    // The attach init's entries keep their output slots in their owning modules (e.g. DAT_10D3DA24).
    void SetOutSlot(const char* name, void** slot);

    // Q1 (FUN_10114540) CVars queued by other modules' inits; registered after the attach init's own.
    void QueueWorldCVar(const CVarSpec& spec);
    const CVarSpec* QueuedWorldCVars(size_t& count);
}
