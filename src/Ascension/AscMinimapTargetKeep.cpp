// Indoors, after some /reloads, the minimap shows part of another UI texture (often the calendar) instead of the
// room. Players saw it on the original client; it reproduces with and without DXVK, and the 64-bit CoA64 client
// does not show it.
//
// Each world-UI teardown (0x57FBD0) releases the 256x256 indoor render target (the texture handle at 0xBF5F14)
// through 0x47BF30, and each world-UI init (0x5831C0) creates a new one. The target made at login always draws; a
// new target from a reload can draw the wrong texture, and drawing into the login target again fixes it. The cause
// is not known.
//
// Not in the genuine DLL. Applied early (IMPROVEMENTS.md, "Indoor minimap shows another texture"). The login target
// is kept for the whole session: the teardown takes it before the release, and the next init releases the target it
// has just created, puts the kept one back and asks for a redraw (0xBF5F18).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>

namespace
{
    void** const kTarget = reinterpret_cast<void**>(0xBF5F14);
    uint8_t* const kRedraw = reinterpret_cast<uint8_t*>(0xBF5F18);

    typedef void(__cdecl* Fn_t)();
    typedef void(__cdecl* Release_t)(void*);
    const Release_t Release = reinterpret_cast<Release_t>(0x47BF30);

    Fn_t g_leaveWorld = nullptr;
    Fn_t g_enterWorld = nullptr;
    void* g_kept = nullptr;

    void __cdecl LeaveWorld()
    {
        if (*kTarget)
        {
            g_kept = *kTarget;
            *kTarget = nullptr;
        }
        g_leaveWorld();
    }

    void __cdecl EnterWorld()
    {
        g_enterWorld();
        if (!g_kept)
            return;
        if (*kTarget && *kTarget != g_kept)
            Release(*kTarget);
        *kTarget = g_kept;
        g_kept = nullptr;
        *kRedraw = 1;
    }

    void Init()
    {
        // 0x57FBD0: push ebp; mov ebp, esp; sub esp, 8
        g_leaveWorld = reinterpret_cast<Fn_t>(AscRuntime::Detour(0x57FBD0, 6, reinterpret_cast<void*>(&LeaveWorld)));
        // 0x5831C0: push ebp; mov ebp, esp; push ecx; push esi
        g_enterWorld = reinterpret_cast<Fn_t>(AscRuntime::Detour(0x5831C0, 5, reinterpret_cast<void*>(&EnterWorld)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
