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
  own logon protocol. They are not installed, so the client authenticates with stock SRP6 against an AzerothCore
  authserver (see `AscMain.cpp`, and `CLAUDE.md` G0-02).
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
