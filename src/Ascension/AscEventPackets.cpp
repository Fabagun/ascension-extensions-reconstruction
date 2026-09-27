// The packet handlers of installer FUN_100d4b90 (its hooks are in AscGossipQuest.cpp). Most turn an
// SMSG straight into a UI event; three repeat a stock handler over a counted batch.
//
//   0x588 (FUN_100d3800)  u32 n: n ? BATTLEGROUND_COUNTDOWN_STARTED("%u", n) : BATTLEGROUND_STARTED
//   0x58E / 0x58F / 0x590 u32 n, then the stock handler 0x635190 / 0x6354D0 / 0x635230 n times on the
//                         same packet (FUN_100d3ad0 / FUN_100d3dc0 / FUN_100d4290); 0 as soon as one fails
//   0x5A5 (FUN_100d49c0)  u32 length + bytes -> TOGGLE_GAME_MODE_RESULT("%s")
//   0x687 (FUN_100d45a0)  s, s, u32, u32, u32, s, s, u32, u8 -> TALKING_HEAD_FRAME_DISPLAY
//                         ("%s%s%u%u%u%s%s%u%b")
//   0x736 (FUN_100d4440)  s -> SUBMIT_GAME_FEEDBACK_RESULT("%s")
//   0x73E (FUN_100d4030)  u32 -> OPEN_CUSTOM_STORE("%u")
//   0x76A (FUN_100d42f0)  u8, u32 -> SET_ACTION_BUTTON_SPELL_PAYLOAD("%u%u")
//   0x76B / 0x76C         u32, u8, u8 -> ITEM_USED_PAYLOAD / ITEM_USE_FAILED_PAYLOAD("%u%u%u")
//                         (the original pushes the two bytes with stale upper bytes -- IMPROVEMENTS)
//   0x76D (FUN_100d4150)  u32, u32 -> PROCESSED_STATISTIC_QUERY_PAYLOAD("%u%d")
//   0x76E (FUN_100d40b0)  u32, u32 -> PROCESSED_ASSET_QUERY_PAYLOAD("%u%u")
//   0x754..0x75D (FUN_10225970, all ten) C string -> DRAFT_RESULT("%s"), then the Logger (channel 2)
//                         line "Draft result <opcode>: <text>"
//   0x75F / 0x760 (FUN_10190700 / FUN_10190a30) C string: its index in the ACTIVATE_CHARACTER_* (8) /
//                         DEACTIVATE_CHARACTER_* (7) names, else the Logger (channel 2) line "Unexpected
//                         Value: <s>" and the last (_FAILED); then CHARACTER_ACTIVATE_RESULT /
//                         CHARACTER_DEACTIVATE_RESULT("%b%s", index == 0, s)
//   0x904 SMSG_PACKET_BROADCAST (FUN_102295e0): u64 source, u64 target, float range, u16 opcode, u16 size,
//                         payload. Dropped when the player is the source or the target; with a source, it
//                         must exist, and (range >= FLT_EPSILON) the player must be within range + 10 of it
//                         (object vtable +0x2C positions, 2D distance). The payload is then dispatched as a
//                         received packet: [u16 opcode][payload] into the client's 0x631FE0(time, &store, 1).
//   0x661 (FUN_101102c0)  nothing read -> BUILD_DRAFT_PICK_PROMPT
//   0x73C (FUN_10275310)  C string -> TOGGLE_FEL_COMMUTATION_STATUS_RESULT("%s")
//   0x92B (FUN_100e6de0)  u32 a, b, c -> ASCENSION_BG_QUEUE_ALERT("%u%u%u")
//   0x675 (FUN_10a4f0c0, attach init) u64 guid, u8 online: the player -> GROUP_JOINED / GROUP_LEFT when it
//                         has a unit token; another unit -> UNIT_CONNECTION("%s%b", its first token, online)
//   0x68F (FUN_10a4fdd0, attach init) u32 encounter, u32 state; the client's DungeonEncounter.dbc row must
//                         exist. d = the current map's (0xADFBC4) Map.dbc +0x44, or 0.
//                         1 -> ENCOUNTER_START("%u%s%u%u", id, row +0x14, row +8, d)
//                         2 -> ENCOUNTER_END("%u%s%u%u%u", id, row +0x14, row +8, d, 0)
//                         3 -> ENCOUNTER_END(..., 1), then BOSS_KILL("%u%s", id, row +0x14)
//   0x68E (FUN_10a4fa40, attach init) C string: fires the event of that name, no arguments
//   0x668 (FUN_10a4f5c0, attach init) C string name, C string value, u8 set, u8 flag: a server CVar
//                         override. set: the map 0x10D3DC48 takes the CVar's current value (overwriting an
//                         earlier save), then the CVar gets `value`; else the saved value is put back and
//                         the entry erased. Unknown CVars are ignored. Both use FUN_10114c40 (a2 = 1,
//                         a3 = a4 = 0, a5 = flag).
//   0x906 (FUN_100d55e0)  u8 window, u8 visible -> <window's name (FUN_100d3620)>("%b", visible != 0)
//   0x742 (FUN_10a4fb30, attach init) u32 spell, u32 value: for a known spell, the client string
//                         ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI (0x819D40) formatted with (spell name, value) is
//                         signalled as stock event 0xBB ("%s") through FrameScript_SignalEvent 0x81B530
//   0x72D (FUN_10193a00)  u32 -> DAT_10BDEBB4 (reset to 0 at the glue screen, FUN_101939f0); its accessor
//                         FUN_10193a50 has no caller in the plain code
// and, from the client-DBC patch installer (FUN_1022958a):
//   0x9B1 (FUN_10235a90)  u32 spell, u32-length texture, u32 position (low byte), float scale, u32 r, g, b
//                         -> SPELL_ACTIVATION_SHOW("%u%s%s%f%u%u%u", spell, texture, position name, ...)
//   0x9B2 (FUN_10235840)  u32 spell, u32 position (low byte): 0 -> SPELL_ACTIVATION_HIDE("%u", spell),
//                         else ("%u%s", spell, position name)
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscCrashContext.hpp>
#include <Client/CVar.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace
{
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    uint8_t U8(CDataStore* p) { const uint8_t v = static_cast<uint8_t>(p->m_buffer[p->m_read]); p->m_read += 1; return v; }
    std::string CStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    typedef int(__cdecl* StockHandler)(void*, uint32_t, uint32_t, CDataStore*);
    int Repeat(uint32_t handler, void* a, uint32_t opcode, uint32_t time, CDataStore* p)
    {
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
            if (!reinterpret_cast<StockHandler>(handler)(a, opcode, time, p))
                return 0;
        return 1;
    }
    int __cdecl On58E(void* a, uint32_t op, uint32_t t, CDataStore* p) { return Repeat(0x635190, a, op, t, p); }
    int __cdecl On58F(void* a, uint32_t op, uint32_t t, CDataStore* p) { return Repeat(0x6354D0, a, op, t, p); }
    int __cdecl On590(void* a, uint32_t op, uint32_t t, CDataStore* p) { return Repeat(0x635230, a, op, t, p); }

    int __cdecl On588(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t n = U32(p);
        if (n)
            AscRuntime::Signal("BATTLEGROUND_COUNTDOWN_STARTED", "%u", n);
        else
            AscRuntime::Signal("BATTLEGROUND_STARTED");
        return 1;
    }

    int __cdecl On5A5(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t len = U32(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        AscRuntime::Signal("TOGGLE_GAME_MODE_RESULT", "%s", s.c_str());
        return 1;
    }

    int __cdecl On687(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string s1 = CStr(p), s2 = CStr(p);
        const uint32_t a = U32(p), b = U32(p), c = U32(p);
        const std::string s3 = CStr(p), s4 = CStr(p);
        const uint32_t d = U32(p);
        const uint32_t e = U8(p);
        AscRuntime::Signal("TALKING_HEAD_FRAME_DISPLAY", "%s%s%u%u%u%s%s%u%b", s1.c_str(), s2.c_str(), a, b, c,
                           s3.c_str(), s4.c_str(), d, e);
        return 1;
    }

    int __cdecl On736(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string s = CStr(p);
        AscRuntime::Signal("SUBMIT_GAME_FEEDBACK_RESULT", "%s", s.c_str());
        return 1;
    }

    int __cdecl On73E(void*, uint32_t, uint32_t, CDataStore* p)
    {
        AscRuntime::Signal("OPEN_CUSTOM_STORE", "%u", U32(p));
        return 1;
    }

    int __cdecl On76A(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t a = U8(p);
        const uint32_t b = U32(p);
        AscRuntime::Signal("SET_ACTION_BUTTON_SPELL_PAYLOAD", "%u%u", a, b);
        return 1;
    }

    void ItemPayload(const char* name, CDataStore* p)
    {
        const uint32_t item = U32(p);
        const uint32_t b = U8(p);
        const uint32_t c = U8(p);
        AscRuntime::Signal(name, "%u%u%u", item, b, c);
    }
    int __cdecl On76B(void*, uint32_t, uint32_t, CDataStore* p) { ItemPayload("ITEM_USED_PAYLOAD", p); return 1; }
    int __cdecl On76C(void*, uint32_t, uint32_t, CDataStore* p) { ItemPayload("ITEM_USE_FAILED_PAYLOAD", p); return 1; }

    int __cdecl On76D(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t a = U32(p), b = U32(p);
        AscRuntime::Signal("PROCESSED_STATISTIC_QUERY_PAYLOAD", "%u%d", a, b);
        return 1;
    }

    int __cdecl On76E(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t a = U32(p), b = U32(p);
        AscRuntime::Signal("PROCESSED_ASSET_QUERY_PAYLOAD", "%u%u", a, b);
        return 1;
    }

    int __cdecl OnDraftResult(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        const std::string s = CStr(p);
        AscRuntime::Signal("DRAFT_RESULT", "%s", s.c_str());
        AscLogger::Write(2, "Draft result " + std::to_string(opcode) + ": " + s);
        return 1;
    }

    void CharacterResult(CDataStore* p, const char* const* names, uint32_t count, const char* event)
    {
        const std::string s = CStr(p);
        uint32_t index = 0;
        while (index < count && s != names[index])
            ++index;
        if (index == count)
        {
            AscLogger::Write(2, "Unexpected Value: " + s);
            index = count - 1;
        }
        AscRuntime::Signal(event, "%b%s", index == 0 ? 1u : 0u, s.c_str());
    }

    int __cdecl On75F(void*, uint32_t, uint32_t, CDataStore* p)
    {
        static const char* const kNames[8] = {"ACTIVATE_CHARACTER_OK", "ACTIVATE_CHARACTER_NOT_FOUND",
            "ACTIVATE_CHARACTER_NOT_OWNED", "ACTIVATE_CHARACTER_ALREADY_ACTIVE", "ACTIVATE_CHARACTER_TRANSFER_LOCKED",
            "ACTIVATE_CHARACTER_ONLINE", "ACTIVATE_CHARACTER_MAX_ACTIVE", "ACTIVATE_CHARACTER_FAILED"};
        CharacterResult(p, kNames, 8, "CHARACTER_ACTIVATE_RESULT");
        return 1;
    }

    int __cdecl On760(void*, uint32_t, uint32_t, CDataStore* p)
    {
        static const char* const kNames[7] = {"DEACTIVATE_CHARACTER_OK", "DEACTIVATE_CHARACTER_NOT_FOUND",
            "DEACTIVATE_CHARACTER_NOT_OWNED", "DEACTIVATE_CHARACTER_ALREADY_INACTIVE",
            "DEACTIVATE_CHARACTER_TRANSFER_LOCKED", "DEACTIVATE_CHARACTER_ONLINE", "DEACTIVATE_CHARACTER_FAILED"};
        CharacterResult(p, kNames, 7, "CHARACTER_DEACTIVATE_RESULT");
        return 1;
    }

    int __cdecl On904(void*, uint32_t, uint32_t time, CDataStore* p)
    {
        typedef void*(__cdecl* ObjectByGuid_t)(uint32_t, uint32_t, uint32_t);
        const ObjectByGuid_t ObjectByGuid = reinterpret_cast<ObjectByGuid_t>(0x4D4DB0);
        const uint64_t player = reinterpret_cast<uint64_t(__cdecl*)()>(0x4D3790)();
        const uint32_t srcLo = U32(p), srcHi = U32(p), dstLo = U32(p), dstHi = U32(p);
        float range;
        memcpy(&range, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        uint16_t opcode, size;
        memcpy(&opcode, p->m_buffer + p->m_read, 2);
        memcpy(&size, p->m_buffer + p->m_read + 2, 2);
        p->m_read += 4;
        const uint32_t plLo = static_cast<uint32_t>(player), plHi = static_cast<uint32_t>(player >> 32);
        if ((plLo == srcLo && plHi == srcHi) || (plLo == dstLo && plHi == dstHi))
            return 1;
        if (srcLo | srcHi)
        {
            if (!ObjectByGuid(srcLo, srcHi, 0xFFFF))
                return 1;
            if (range >= 1.1920929e-07f)
            {
                uint8_t* src = static_cast<uint8_t*>(ObjectByGuid(srcLo, srcHi, 0xFFFF));
                uint8_t* me = static_cast<uint8_t*>(ObjectByGuid(plLo, plHi, 0xFFFF));
                if (src)
                {
                    const float r = range + 10.0f;
                    if (r <= 1.1920929e-07f)
                        return 1;
                    float a[3], b[3];
                    typedef void(__thiscall* Pos_t)(void*, float*);
                    (*reinterpret_cast<Pos_t*>(*reinterpret_cast<uint8_t**>(src) + 0x2C))(src, a);
                    (*reinterpret_cast<Pos_t*>(*reinterpret_cast<uint8_t**>(me) + 0x2C))(me, b);
                    const float dx = b[0] - a[0], dy = b[1] - a[1];
                    if (!(r * r >= dy * dy + dx * dx))
                        return 1;
                }
            }
        }
        uint8_t* msg = static_cast<uint8_t*>(malloc(size + 2u));   // FUN_10ae66d0
        memcpy(msg, &opcode, 2);
        memcpy(msg + 2, p->m_buffer + p->m_read, size);
        p->m_read += size;
        CDataStore store;
        reinterpret_cast<void(__thiscall*)(CDataStore*)>(0x401050)(&store);
        if (store.m_buffer)
            free(store.m_buffer);
        store.m_buffer = reinterpret_cast<int8_t*>(msg);
        store.m_alloc = -1;
        store.m_size = size + 2;
        store.m_read = 0;
        void* net = reinterpret_cast<void*(__cdecl*)()>(0x6B0970)();
        reinterpret_cast<int(__thiscall*)(void*, uint32_t, CDataStore*, int)>(0x631FE0)(net, time, &store, 1);
        free(msg);
        reinterpret_cast<void(__thiscall*)(CDataStore*)>(0x403880)(&store);   // m_alloc -1: the buffer is not freed again
        return 1;
    }

    int __cdecl On661(void*, uint32_t, uint32_t, CDataStore*)
    {
        AscRuntime::Signal("BUILD_DRAFT_PICK_PROMPT");
        return 1;
    }

    int __cdecl On73C(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string s = CStr(p);
        AscRuntime::Signal("TOGGLE_FEL_COMMUTATION_STATUS_RESULT", "%s", s.c_str());
        return 1;
    }

    int __cdecl On92B(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t a = U32(p), b = U32(p), c = U32(p);
        AscRuntime::Signal("ASCENSION_BG_QUEUE_ALERT", "%u%u%u", a, b, c);
        return 1;
    }

    int __cdecl On675(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t guid[2];
        guid[0] = U32(p);
        guid[1] = U32(p);
        const uint32_t online = U8(p);
        typedef const char* const*(__cdecl* Tokens_t)(uint32_t*, int*);
        int count = 0;
        const uint64_t player = reinterpret_cast<uint64_t(__cdecl*)()>(0x4D3790)();
        if (static_cast<uint32_t>(player) == guid[0] && static_cast<uint32_t>(player >> 32) == guid[1])
        {
            reinterpret_cast<Tokens_t>(0x60BB70)(guid, &count);
            if (count)
                AscRuntime::Signal(online ? "GROUP_JOINED" : "GROUP_LEFT");
            return 1;
        }
        const char* const* tokens = reinterpret_cast<Tokens_t>(0x60BB70)(guid, &count);
        if (count)
            AscRuntime::Signal("UNIT_CONNECTION", "%s%b", tokens[0], online);
        return 1;
    }

    int __cdecl On68F(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t id = U32(p);
        const uint32_t state = U32(p);
        const uint8_t* row = AscClientDbc::Row(0xAD36B0, id);   // DungeonEncounter.dbc (FUN_100b1600)
        if (!row)
            return 1;
        auto difficulty = []() -> uint32_t
        {
            const uint8_t* map = AscClientDbc::Row(0xAD4160, *reinterpret_cast<const uint32_t*>(0xADFBC4));   // Map.dbc
            return map ? *reinterpret_cast<const uint32_t*>(map + 0x44) : 0u;
        };
        const char* name = *reinterpret_cast<const char* const*>(row + 0x14);
        const uint32_t f8 = *reinterpret_cast<const uint32_t*>(row + 8);
        if (state == 2)
            AscRuntime::Signal("ENCOUNTER_END", "%u%s%u%u%u", id, name, f8, difficulty(), 0u);
        else if (state == 1)
            AscRuntime::Signal("ENCOUNTER_START", "%u%s%u%u", id, name, f8, difficulty());
        else if (state == 3)
        {
            AscRuntime::Signal("ENCOUNTER_END", "%u%s%u%u%u", id, name, f8, difficulty(), 1u);
            AscRuntime::Signal("BOSS_KILL", "%u%s", id, name);
        }
        return 1;
    }

    int __cdecl On68E(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string name = CStr(p);
        AscRuntime::Signal(name.c_str());
        return 1;
    }

    // FUN_10114c40 (thiscall on the CVar): the client's CVar set 0x7668C0, after the original's length assert.
    void SetCVar(CVar* cvar, const char* value, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        const char* key = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(cvar) + 0x14);
        if (strlen(value) + strlen(key) > 0xF9)
        {
            const std::string msg = std::string("CVar::Set (m_key.str + value) > 249, m_key = ") + key;
            AscCrashContext::Assert(msg.c_str(), nullptr, 0);
            return;
        }
        reinterpret_cast<void(__thiscall*)(CVar*, const char*, uint32_t, uint32_t, uint32_t, uint32_t)>(0x7668C0)(cvar, value, a, b, c, d);
    }

    std::unordered_map<std::string, std::string> g_cvarOverrides;   // 0x10D3DC48

    int __cdecl On668(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string name = CStr(p);
        const std::string value = CStr(p);
        const uint8_t set = U8(p);
        const uint8_t flag = U8(p);
        CVar* cvar = CVar::Lookup(name.c_str());   // FUN_10114480
        if (!cvar)
            return 1;
        if (set)
        {
            const char* current = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(cvar) + 0x28);
            g_cvarOverrides[name] = current ? current : "";
            SetCVar(cvar, value.c_str(), 1, 0, 0, flag);
            return 1;
        }
        auto it = g_cvarOverrides.find(name);
        if (it == g_cvarOverrides.end())
            return 1;
        SetCVar(cvar, it->second.c_str(), 1, 0, 0, flag);
        g_cvarOverrides.erase(it);
        return 1;
    }

    int __cdecl On906(void*, uint32_t, uint32_t, CDataStore* p)
    {
        static const char* const kNames[7] = {"ASCENSION_UNKNOWN_WINDOW_VISIBILITY_CHANGED",
            "ASCENSION_REFORGE_ENCHANTMENT_WINDOW_VISIBILITY_CHANGED", "ASCENSION_FEL_COMMUTATION_WINDOW_VISIBILITY_CHANGED",
            "ASCENSION_SKILL_CARDS_WINDOW_VISIBILITY_CHANGED", "ASCENSION_LUCKY_SKILL_CARDS_WINDOW_VISIBILITY_CHANGED",
            "ASCENSION_MYTHIC_PLUS_KEYSTONE_ACTIVATION_WINDOW_VISIBILITY_CHANGED", "ASCENSION_MAX_WINDOW_VISIBILITY_CHANGED"};
        const uint8_t window = U8(p);
        const uint8_t visible = U8(p);
        AscRuntime::Signal(window < 7 ? kNames[window] : "ASCENSION_UNHANDLED_WINDOW_VISIBILITY_CHANGED", "%b", visible ? 1u : 0u);
        return 1;
    }

    int __cdecl On742(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t spell = U32(p);
        const uint32_t value = U32(p);
        uint8_t rec[0x2A8];
        if (!AscScript::FetchSpell(spell, rec))   // FUN_100b2cb0
            return 1;
        const char* fmt = reinterpret_cast<const char*(__cdecl*)(const char*, int, int)>(0x819D40)("ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI", -1, 1);
        const char* name = *reinterpret_cast<const char* const*>(rec + 0x220);
        const int n = _snprintf(nullptr, 0, fmt, name, value);   // FUN_100b82f0 sizes, then formats
        if (n < 0)
            throw std::runtime_error("Error during formatting.");
        std::string text(static_cast<size_t>(n), '\0');
        _snprintf(&text[0], text.size(), fmt, name, value);
        reinterpret_cast<int(__cdecl*)(int, const char*, ...)>(0x81B530)(0xBB, "%s", text.c_str());
        return 1;
    }

    uint32_t g_value72D = 0;   // DAT_10BDEBB4
    int __cdecl On72D(void*, uint32_t, uint32_t, CDataStore* p) { g_value72D = U32(p); return 1; }
    void Reset72D() { g_value72D = 0; }

    // FUN_10228a60: a position enum value as its name, or UNEXPECTED_ENUM_VALUE_<n>.
    std::string PositionName(uint8_t v)
    {
        static const char* const kNames[14] = {"NONE", "CENTER", "LEFT", "RIGHT", "TOP", "BOTTOM", "TOPRIGHT", "TOPLEFT",
                                               "BOTTOMRIGHT", "BOTTOMLEFT", "LEFT_FLIPPED", "RIGHT_FLIPPED", "TOP_FLIPPED",
                                               "BOTTOM_FLIPPED"};
        return v < 14 ? kNames[v] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(v);
    }

    int __cdecl On9B1(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t spell = U32(p);
        const uint32_t len = U32(p);   // FUN_100d3680
        const std::string texture(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        const std::string position = PositionName(static_cast<uint8_t>(U32(p)));
        float scale;
        memcpy(&scale, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        const uint32_t r = U32(p), g = U32(p), b = U32(p);
        AscRuntime::Signal("SPELL_ACTIVATION_SHOW", "%u%s%s%f%u%u%u", spell, texture.c_str(), position.c_str(),
                           static_cast<double>(scale), r, g, b);
        return 1;
    }

    int __cdecl On9B2(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t spell = U32(p);
        const uint8_t position = static_cast<uint8_t>(U32(p));
        if (position == 0)
            AscRuntime::Signal("SPELL_ACTIVATION_HIDE", "%u", spell);
        else
            AscRuntime::Signal("SPELL_ACTIVATION_HIDE", "%u%s", spell, PositionName(position).c_str());
        return 1;
    }

    // FUN_100d3550 (after every 0x403340): ASCENSION_PLAYER_LEVEL_UP(new, old) whenever the player's level
    // (descriptor +0xD8, 0 without a player) differs from the last one seen (0x10BDB688).
    uint32_t g_lastLevel = 0;
    void WatchLevel()
    {
        const uint8_t* player = AscScript::ActivePlayer();
        const uint32_t level = player ? *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 8) + 0xD8) : 0;
        if (level == g_lastLevel)
            return;
        AscRuntime::Signal("ASCENSION_PLAYER_LEVEL_UP", "%u%u", level, g_lastLevel);
        g_lastLevel = level;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x588, CNetClientCustomPacket((void*)&On588, nullptr));
        sDC.AddPacketHandler(0x58E, CNetClientCustomPacket((void*)&On58E, nullptr));
        sDC.AddPacketHandler(0x58F, CNetClientCustomPacket((void*)&On58F, nullptr));
        sDC.AddPacketHandler(0x590, CNetClientCustomPacket((void*)&On590, nullptr));
        sDC.AddPacketHandler(0x5A5, CNetClientCustomPacket((void*)&On5A5, nullptr));
        sDC.AddPacketHandler(0x687, CNetClientCustomPacket((void*)&On687, nullptr));
        sDC.AddPacketHandler(0x736, CNetClientCustomPacket((void*)&On736, nullptr));
        sDC.AddPacketHandler(0x73E, CNetClientCustomPacket((void*)&On73E, nullptr));
        sDC.AddPacketHandler(0x76A, CNetClientCustomPacket((void*)&On76A, nullptr));
        sDC.AddPacketHandler(0x76B, CNetClientCustomPacket((void*)&On76B, nullptr));
        sDC.AddPacketHandler(0x76C, CNetClientCustomPacket((void*)&On76C, nullptr));
        sDC.AddPacketHandler(0x76D, CNetClientCustomPacket((void*)&On76D, nullptr));
        sDC.AddPacketHandler(0x76E, CNetClientCustomPacket((void*)&On76E, nullptr));
        sDC.AddPacketHandler(0x75F, CNetClientCustomPacket((void*)&On75F, nullptr));
        sDC.AddPacketHandler(0x760, CNetClientCustomPacket((void*)&On760, nullptr));
        for (uint32_t op = 0x754; op <= 0x75D; ++op)
            sDC.AddPacketHandler(op, CNetClientCustomPacket((void*)&OnDraftResult, nullptr));
        sDC.AddPacketHandler(0x675, CNetClientCustomPacket((void*)&On675, nullptr));
        sDC.AddPacketHandler(0x68F, CNetClientCustomPacket((void*)&On68F, nullptr));
        sDC.AddPacketHandler(0x68E, CNetClientCustomPacket((void*)&On68E, nullptr));
        sDC.AddPacketHandler(0x668, CNetClientCustomPacket((void*)&On668, nullptr));
        AscRuntime::OnGlueScreen(&Reset72D);
        sDC.AddPacketHandler(0x72D, CNetClientCustomPacket((void*)&On72D, nullptr));
        sDC.AddPacketHandler(0x742, CNetClientCustomPacket((void*)&On742, nullptr));
        sDC.AddPacketHandler(0x906, CNetClientCustomPacket((void*)&On906, nullptr));
        sDC.AddPacketHandler(0x661, CNetClientCustomPacket((void*)&On661, nullptr));
        sDC.AddPacketHandler(0x73C, CNetClientCustomPacket((void*)&On73C, nullptr));
        sDC.AddPacketHandler(0x92B, CNetClientCustomPacket((void*)&On92B, nullptr));
        sDC.AddPacketHandler(0x904, CNetClientCustomPacket((void*)&On904, nullptr));
        sDC.AddPacketHandler(0x9B1, CNetClientCustomPacket((void*)&On9B1, nullptr));
        sDC.AddPacketHandler(0x9B2, CNetClientCustomPacket((void*)&On9B2, nullptr));
        AscRuntime::OnAfter403340(&WatchLevel);
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
