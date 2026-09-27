// Entry point. Replaces WotLKExtensions/src/Main.cpp (excluded from the build): the upstream DllMain
// runs its patches on a worker thread, which is wrong for us -- Ascension.exe LoadLibrary's this DLL
// from the main thread inside a glue Lua call, and we must register into that live state before
// returning (that is what the real Extensions.dll does; it "does everything from DllMain").
#include <Windows.h>
#include <PatchConfig.hpp>
#include <Main.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRegistry.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscBindings.hpp>
namespace AscWidget { void ApplyPatches(); }
#include <Ascension/NetPatch.hpp>
#include <Misc/Util.hpp>
#include <cstring>

// The real DLL's single export; the exe never calls it but tooling looks for it.
extern "C" __declspec(dllexport) int ClientExtensionsDummy() { return 1; }

// The scaffold plumbing still in use: custom opcode dispatch and glue CVars (audit step 3 continues). Lua
// registration is the original's 0x855060 detour (AscRegistry) and events its 0x81B5F0 detour (AscRuntime), so
// the scaffold's CGlueMgr / CustomLua / FrameScript call-site hooks (0x4DA71D, 0x52AB17, 0x52AB26) are gone, as
// are its 0xD415B8 / 0xD415BC range writes (the original disables that check by stubbing 0x86B5A0, AscAttachLate).
// Removed 2026-09-27, per the live exactness audit (tools/live_audit.py: patches the original client never
// makes): the upstream WotLK-Extensions feature patches WoWTime (+ the year-offset setting only it reads),
// CMap, Misc, CGPlayer (char-create race fix, combo-point fix, LFD class roles), CGTooltip, CMissile,
// MacroConditions, plus Spell / LuaUnlock (already no-ops here).
// Deliberately NOT installed (the one remaining hook gap vs the original, 2026-09-27 live audit): the three
// hooks of Ascension's own logon protocol --
//   0x8CCE00 -> FUN_100e3dc0 (into the encrypted/VM region), 0x9A83E0 -> FUN_100e5d70 (copies the session key
//   material into the client's auth object), 0x9A88C0 -> FUN_100e5dd0 (the 20-byte server proof,
//   HMAC(K, "OK")).
// Installing them replaces the client's native SRP6 with Ascension's logon, which our AzerothCore authserver
// cannot answer until the translation proxy (BUILD_PLAN P3) exists. The JS client re-routes exactly these three
// for the same reason. Project decision: SRP6-restore (CLAUDE.md G0-02).
static void ApplyUpstreamPatches()
{
#if CUSTOMPACKETS_PATCH
    CNetClient::ApplyPatches();
#endif
#if GLUEMGREXTENSION
    CVar::ApplyPatches();
#endif
}

// Ascension keeps world packet headers PLAINTEXT after the CMSG_AUTH_SESSION proof, and so must we
// if one realm is to serve both this client and the collaborator's. Pair with the server's
// AscensionCompat.PlaintextWorldHeaders = 1 (which is also what the module's .conf.dist ships).
//
// PROVEN 2026-09-22 on the wire. The JS client, traced against its own PTR realm, sends
// CMSG_WARDEN_DATA framed in the clear straight after the auth proof -- the exact point at which our
// client used to switch to an enciphered header. Ours does that because our DLL restores native
// stock SRP6, so the client receives a real 40-byte session key and keys the header cipher exactly
// as stock does.
//
// The lever is the cipher INIT, not any of the flags or branches tried before. 0x466BF0 is
// SARC4-style init(key, keyLen, direction, seed, seedLen), located from the 3.3.5 header-cipher HMAC
// seeds embedded in the client (0x9E8A9C server->client, 0x9E8AAC client->server, the latter
// referenced from 0x466C06). It is called from 0x6326AC and 0x632904 and returns `ret 0x14`, i.e.
// __thiscall with five stack args that the callee cleans. Stubbing the entry to `xor eax,eax;
// ret 0x14` means the cipher is never keyed and every header stays plaintext.
//
// Verified by running with it skipped against a plaintext server: SMSG_AUTH_RESPONSE dispatched,
// character list populated, world entered, full in-world opcode flow, zero disconnects.
//
// Five earlier models of this mechanism were wrong -- [conn+0x538] as the send gate, a ret-4 stack
// theory for 0x4A81B0, 0x466B50 as the stream function (the JS client calls it 4+ times yet sends
// plaintext), and the 0x4D9FE0 predicate (already returns 0 for us). Do not revisit those.
static uint32_t g_cipherInitSkips = 0;

// Detour rather than a bare stub, mirroring what actually worked under Frida
// (Interceptor.replace pointed the entry at our own code). Logs each call so we can finally see how
// many inits a real session makes and at which stage -- the bare `xor eax,eax; ret 0x14` stub gave
// no visibility and stalled world connect for reasons still unexplained.
// __thiscall with five stack args that the callee cleans, hence `ret 0x14`.
static void __declspec(naked) CipherInitDetour()
{
    __asm
    {
        pushad
        pushfd
    }
    ++g_cipherInitSkips;
    AscLog::Printf("plaintext-headers: cipher init skipped (#%u)", g_cipherInitSkips);
    __asm
    {
        popfd
        popad
        xor eax, eax
        ret 0x14
    }
}

static void ApplyPlaintextWorldHeaders()
{
    static const uint8_t expect[5] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC };   // push ebp; mov ebp,esp; sub esp,...

    uint8_t* site = reinterpret_cast<uint8_t*>(0x466BF0);
    if (memcmp(site, expect, sizeof(expect)) != 0)
    {
        AscLog::Printf("plaintext-headers: unexpected bytes at 0x466BF0 (%02X %02X %02X %02X %02X), NOT patched",
            site[0], site[1], site[2], site[3], site[4]);
        return;
    }

    const uint32_t rel = reinterpret_cast<uint32_t>(&CipherInitDetour) - (0x466BF0 + 5);
    Util::SetByteAtAddress(site, 0xE9);
    Util::OverwriteUInt32AtAddress(0x466BF1, rel);
    AscLog::Printf("plaintext-headers: cipher init 0x466BF0 detoured to %p "
                   "(requires server AscensionCompat.PlaintextWorldHeaders = 1)", (void*)&CipherInitDetour);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);
        AscLog::Init();
        __try
        {
            ApplyUpstreamPatches();
            AscLog::Printf("upstream patches applied");
            // The original DLL's module runtime: its custom event names, its world-entry / glue-screen
            // lifecycle hooks, then each transcribed subsystem's init (which registers into both).
            AscRuntime::RegisterEventNames();
            AscRuntime::InstallHooks();
            AscBindings::InitModules();
            // Must precede Lua-state creation: the client registers the Frame method table while
            // building each state, so the repoint has to be in place first.
            AscWidget::ApplyPatches();
            // DISABLED. Skipping the cipher init is NOT sufficient: the detour fires exactly
            // once (same as the Frida run) with correct ret 0x14, and world connect still stalls.
            // The Frida result that appeared to work is therefore unexplained and possibly an
            // artifact of its cdecl callback leaking the 5 args. See FINDINGS.md 2026-09-22.
            // ApplyPlaintextWorldHeaders();
            (void)&ApplyPlaintextWorldHeaders;
            // 2026-09-22: superseded -- the note below described the OLD [conn+0x538] approach
            // (AscensionCompat.PlaintextWorldHeaders = 0). Forcing the client plaintext does not
            // work: the flag at [conn+0x538] is now provably never set (0x6330B3's immediate is 0
            // and its only other writer at 0x632A9C is NOP'd, both confirmed in-process) and the
            // client STILL encrypts its outgoing world header. Whatever gates the send-side cipher
            // is not that flag. Since our reconstruction leaves the client stock in this respect,
            // pair it with a stock-behaving server rather than keep hunting for the gate.
            // Re-enabling this REQUIRES setting the server back to plaintext in the same change.
            // 2026-09-22: DISABLED. Reporting build 12344 on the wire gets the client rejected
            // outright by the authserver's own accepted-build check (LOGIN_BADVERSION) -- worse than
            // the "realm currently down" symptom it was meant to fix. The authserver wants 12340; the
            // "down" flag at realm-select is a separate, still-unexplained check. Do not re-enable
            // without confirming what the realm-select "down" flag is actually keyed on -- see
            // FINDINGS.md 2026-09-22.
            // NetPatch::Install();
            // Diagnostics only (no build rewrite): the session bounces back to character select
            // ~10s after world entry and we need to know whether the client asks to log out or the
            // socket is torn down.
            NetPatch::InstallDiagnostics();
            AscRegistry::OnAttach();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            AscLog::Printf("EXCEPTION during attach: 0x%08lX", GetExceptionCode());
        }
    }
    return TRUE;
}
