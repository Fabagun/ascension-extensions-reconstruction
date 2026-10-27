## What this is

`Extensions.dll` is Project Ascension's client extension for the WoW 3.3.5a (build 12340) client `Ascension.exe`.
This project reconstructs it function by function from the decompiled original, so that the client runs
Ascension's UI and systems against our own server. The reconstruction aims to be **exact**: every Lua native,
packet handler, hook, CVar and code patch of the genuine DLL, with the same behaviour, including its bugs (those
are logged in `IMPROVEMENTS.md`, not fixed).

This document is for people who want to **use** the DLL's functions from Lua, from the server or from another
tool, without first finding and reading the C++. Every entry names the genuine DLL's function address, so anyone
who does want the original can go straight to it in the decompile.

## How the DLL works

- **Loading.** `Ascension.exe` loads `Extensions.dll` itself (a small patch in the exe, a `LoadLibrary` cave at
  `0x4E5CB0`). There is no injector. `DllMain` runs on the main thread (`AscMain.cpp`).
- **Attach order.** The WotLK-Extensions scaffold's remaining plumbing (custom packet dispatch, glue CVars), then
  `AscRuntime::InstallHooks`, then every module's `Init` (`AscBindings::InitModules`: its hooks, byte patches,
  packet handlers, CVars, lifecycle callbacks), then `AscRegistry::OnAttach`.
- **Lua registration.** The client has two Lua states, **glue** (login, realm list, character select and create)
  and **world** (in game). They are separate, and each is built from scratch whenever it is entered. The DLL
  registers its natives from its detour on the client's Lua library setup (`0x855060`), which runs for every new
  state *before* the client's own natives. The glue state gets 167 of the original's natives; the world state
  gets the rest (each binding's **State** column says which).
- **Packets.** Opcodes above `0x55E` are Ascension's. The DLL's detours on `SetMessageHandler` (`0x631FA0`) and
  `ProcessMessage` (`0x631FE0`) route them to its own handler table; everything at or below `0x55E` goes to the
  stock client. An opcode with no handler is logged as `Unhandled packet: <name> (<opcode>)` (C_Logger channel
  `Fatal`) and dropped without being read. Client packets are built with `AscScript::Packet(op)`, little-endian:
  `U8`/`U16`/`U32`/`U64`, `F32`, `Str` (NUL-terminated) and `Data`. Most variable-length strings are written as
  `u32 length` + bytes, **without** a terminator.
- **CVars.** The DLL registers its 93 CVars on one of two lists, as the original does. **Glue** CVars
  (`FUN_101145c0`, 19) are registered with the client's own at startup and exist from the login screen on.
  **World** CVars (`FUN_10114540`, 74) are registered after each world load, so they exist only in game.
  Per-character ones (flags `0x21`) are deleted when you log out and created again on the next world entry.
  Every registration writes the new `CVar*` into its entry's output slot (the original's global, the **Slot**
  column), so code that reads a CVar reads that slot, never a pointer it looked up once. A pointer kept
  across a logout points at freed memory.
- **Events.** The DLL fires FrameXML events (`AscRuntime::Signal(name, fmt, ...)`). Its custom event names are
  appended to the client's own event table at registration (detour on `0x81B5F0`), so `RegisterEvent` works for
  them like for stock events.
- **Client code.** Hooks are either detours (`AscRuntime::Detour`: the original runs through a trampoline),
  replacements (`ReplaceFunction`), or byte patches applied at attach (`AscAttachPatches` / `AscAttachLate` /
  `AscAttachInline`). The **Client hooks** table of each module lists what it touches.
- **Data.** Ascension's DBCs are read from the client's MPQs (`AscDbc`, `AscClientDbc`). The six large bridged
  DBCs (Creature, Quest, ItemAddon, Vanity, SpellAffect, Mythic) come from `MMgr64.exe` through the MemoryBridge
  (`AscBridgeStore`), as in the original.
- **Logging.** The reconstruction's own developer log (`Logs\Extensions.log`) is **off** by default. Start the
  client with `-extlog 1` to turn it on. The original's logging (`C_Logger`, `Logs\<Channel>.txt`) is always on.
- **Not in this DLL.** Diagnostics, probes and the unattended test driver live in `AscensionRebirth/harness`
  (a separate DLL injected only by `tools/harvest_errors.py`). The DLL runs no Lua of its own, just as the
  original runs none.

## Deliberate differences from the genuine DLL

- **Logon protocol.** The genuine DLL hooks `0x8CCE00`, `0x9A83E0` and `0x9A88C0` to replace SRP6 with Ascension's
  own logon protocol. They are not installed by default, so the client authenticates with stock SRP6 against an AzerothCore
  authserver (see `AscMain.cpp`, and `CLAUDE.md` G0-02). A native reconstruction of that protocol is compiled in only
  with `-DASCENSION_NATIVE_LOGON=ON` (`AscNativeLogon.cpp`, `docs/NATIVE-LOGON.md`).
- **Tooltip line insertion.** After the world UI loads, `AscTooltipInsertFix` wraps Ascension's
  `Tooltip:InsertLine` / `InsertLines` so lines past the tooltip's end are added instead of overwriting leftover
  font strings (a keystone tooltip otherwise shows one Dungeon Modifier of several).
- **Indoor minimap render target.** `AscMinimapTargetKeep` keeps the indoor minimap's 256x256 render target
  (`0xBF5F14`) made at login for the whole session instead of releasing it on each world-UI teardown (`0x57FBD0`)
  and creating a new one (`0x5831C0`). After some reloads a new target draws another UI texture (often the calendar)
  indoors; the login target never does. A workaround for a defect of unknown cause in the 32-bit client; the 64-bit
  CoA64 client does not show it.
- **Interface hot patches (build option, off by default).** Compiled in only with `-DASCENSION_HOT_PATCH=ON`
  (`AscHotPatch`, see its module entry and the README). Not in the genuine DLL:
  - SMSG `0x8F0` runs server-sent interface Lua (sized patch id, sized target addon, sized Lua) under guards: source
    only (no ESC byte, `loadstring` or `string.dump`), 32 KB per patch and 128 KB per session, once per patch id and
    content hash per Lua session, with no taint source when no addon is named or the named addon's taint at its
    `ADDON_LOADED`. Patches are kept until the next glue screen and run again after a `/reload`.
  - CMSG `0x8F1` announces the channel after every world entry (u32 protocol version 1, u8 mode); CMSG `0x8F2`
    reports each patch's outcome (0 ran, 1 applied, 2 deferred, 3 skipped, 4 duplicate, 5 refused by the player,
    6 refused by a guard, 7 error; sized detail).
  - The player's choice (allowed by default; `/hotpatch off`, `/hotpatch ask` per realm, `/hotpatch on`) is kept
    in `WTF\HotPatch.txt`; `/hotpatch` lists the session's patches. Lua: `C_HotPatch.GetMode`, `SetMode`,
    `GetPatches`, `SetRealmConsent`, `OnAddonLoaded`.
  - SMSG `0x5BC` (`ChallengeRequirementTypes`) rows are dropped unless every formatter is one `return <expression>`
    over `value`, literals, operators and the seven functions the shipped table calls. This closes the route servers
    used to smuggle Lua through a formatter. It uses `AscDbcPatch::SetValidator`, which is a no-op unless a module
    registers a validator, so the default build patches rows exactly as the original.
- **Streamed client DBC rows (build option, off by default).** Compiled in only with `-DASCENSION_DBC_STREAM=ON`
  (`AscDbcStream`). Not in the genuine DLL, which has no patch handler for these tables. SMSG `0x8F3`
  (GameObjectDisplayInfo), `0x8F4` (Map) and `0x8F5` (MapDifficulty) each carry a batch: u32 count, then rows field
  by field (u32, or u32-sized string). An existing id is overwritten in place, a new id gets its own row. A batch
  grows the table's index once, with headroom, so a long stream does not copy the index per row; strings the
  stream allocated are freed when their row is overwritten again. Objects already spawned keep their display, and
  a Map row the loading map already used takes effect at the next load. A new map also needs its terrain in the
  client's archives and in the server's `maps/`, `vmaps/` and `mmaps/`; streaming the row is not enough.
- **Anti-cheat.** It is loaded exactly as the original does (`DivxTac.dll` after world entry, plus the account
  kill switch). Nothing is added or removed.

## Reading the tables

- **Args** come from the native's own usage string where it has one (`Usage: C_X.Name(args)`); otherwise they are
  derived from its argument checks (`number`, `string`, `bool`, `table`; `[x]` = optional). **Returns** is the
  number of Lua values pushed (a range when it varies).
- **Original** is the genuine DLL's address for that name. `—` means the name has no registrar entry of its own
  (widget/metatable methods, or natives only the live capture found).
- **Notes** are the transcription's own comment at the implementation: usually the original function name
  (`FUN_...`), wire layouts and any quirk. The implementation column gives `file:line`.
- Opcode names are the original DLL's own table (`FUN_102c4a90`), not guesses.
- Widget methods appear under `#Frame`, `#Model`, `#Minimap`, `#MovieFrame`, `#GameTooltip` (call them as
  `frame:Name(...)`). `@Name` entries are methods of a registry metatable (for example `@LuaDBC`), reached through
  the global of the same name.
