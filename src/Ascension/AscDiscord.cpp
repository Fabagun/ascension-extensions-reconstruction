// DiscordMgr (FUN_10220d50 -> static 0x10BE21F0) over the Discord Game SDK, which the original links
// (DiscordCreate imported from discord_game_sdk.dll, which ships beside the exe) and wraps in the
// SDK's own C++ helper classes. Transcribed from ghidra-ext/out/all/10220*.c / 10221*.c and the
// wrapper at 0x10088xxx-0x10089xxx; the helper classes are replaced by the C API they call.
//
//   +0x04 bool enabled   +0x05 bool activity subscribers registered   +0x10 DiscordUser (current)
//   +0x1A8 discord::Core*
//
//   DiscordMgr::Initialize (FUN_10220b10), run by the original's detour 0x10A46350 after the client's
//   0x4DB9F0 (hook object 0x10BCCB30): registers SMSG 0x90C / 0x90D / 0x90E; unless already enabled,
//   discord::Core::Create(app 0x0C7C30A11D421000, flags 1 = NoRequireDiscord). No core: CMSG 0x52E
//   {u32 result}. A core: subscribe OnCurrentUserUpdate, enabled = true, and a detached thread runs
//   RunCallbacks every 50 ms while enabled (so the callbacks below send from that thread, as the
//   original does).
//
//   OnCurrentUserUpdate  GetCurrentUser -> +0x10; on success CMSG 0x52A {u64 id, cstring username}.
//   SMSG 0x90C  an activity "Selecting World" with small image "ascension_logo" -> UpdateActivity.
//   SMSG 0x90D  u8 skipped; sized strings state, details; u64 start, end; sized large image, small
//               image, large text, small text, party id; u32 party size, max; sized match, join,
//               spectate secrets. The FIRST 0x90D subscribes OnActivityJoinRequest (CMSG 0x529
//               {u64 user id}) and OnActivityJoin (CMSG 0x52B {cstring secret}) and calls
//               UpdateActivity; later ones build the activity and never send it (kept).
//   SMSG 0x90E  sized invite code -> OverlayManager::OpenGuildInvite.
//
// Deviations: the original's activity in 0x90C is uninitialised stack apart from the fields it sets
// (zeroed here); the original imports DiscordCreate statically, so without the SDK DLL it would not
// load at all -- here a missing SDK leaves Discord disabled.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <cstring>
#include <ctime>
#include <process.h>
#include <string>

using namespace AscScript;

namespace
{
    // ---- the Game SDK's C ABI (discord_game_sdk.h, DISCORD_VERSION 2) --------------------------------
    struct DiscordUser { int64_t id; char username[256]; char discriminator[8]; char avatar[128]; bool bot; };
    struct DiscordActivity
    {
        int32_t type;
        int64_t applicationId;
        char name[128], state[128], details[128];
        struct { int64_t start, end; } timestamps;
        struct { char largeImage[128], largeText[128], smallImage[128], smallText[128]; } assets;
        struct { char id[128]; struct { int32_t current, max; } size; } party;
        struct { char match[128], join[128], spectate[128]; } secrets;
        bool instance;
    };
    static_assert(sizeof(DiscordUser) == 0x198, "DiscordUser");
    static_assert(sizeof(DiscordActivity) == 0x5B0, "DiscordActivity");

    typedef void(__cdecl* ResultCallback)(void* data, int32_t result);
    struct IDiscordUserManager { int32_t(__cdecl* get_current_user)(IDiscordUserManager*, DiscordUser*); };
    struct IDiscordActivityManager
    {
        void* register_command;
        void* register_steam;
        void(__cdecl* update_activity)(IDiscordActivityManager*, DiscordActivity*, void*, ResultCallback);
    };
    struct IDiscordOverlayManager
    {
        void(__cdecl* is_enabled)(IDiscordOverlayManager*, bool*);
        void* is_locked;
        void* set_locked;
        void* open_activity_invite;
        void(__cdecl* open_guild_invite)(IDiscordOverlayManager*, const char*, void*, ResultCallback);
    };
    struct IDiscordCore
    {
        void(__cdecl* destroy)(IDiscordCore*);
        int32_t(__cdecl* run_callbacks)(IDiscordCore*);
        void* set_log_hook;
        void* get_application_manager;
        IDiscordUserManager*(__cdecl* get_user_manager)(IDiscordCore*);
        void* get_image_manager;
        IDiscordActivityManager*(__cdecl* get_activity_manager)(IDiscordCore*);
        void* get_relationship_manager;
        void* get_lobby_manager;
        void* get_network_manager;
        IDiscordOverlayManager*(__cdecl* get_overlay_manager)(IDiscordCore*);
    };
    struct IDiscordUserEvents { void(__cdecl* on_current_user_update)(void*); };
    struct IDiscordActivityEvents
    {
        void(__cdecl* on_activity_join)(void*, const char*);
        void(__cdecl* on_activity_spectate)(void*, const char*);
        void(__cdecl* on_activity_join_request)(void*, DiscordUser*);
        void(__cdecl* on_activity_invite)(void*, int32_t, DiscordUser*, DiscordActivity*);
    };
    struct DiscordCreateParams
    {
        int64_t clientId;
        uint64_t flags;
        void* events;
        void* eventData;
        struct { void* events; int32_t version; } managers[12];   // application, user, image, activity,
                                                                  // relationship, lobby, network, overlay,
                                                                  // storage, store, voice, achievement
    };
    static_assert(sizeof(DiscordCreateParams) == 0x78, "DiscordCreateParams");

    // ---- DiscordMgr ----------------------------------------------------------------------------------
    struct Mgr
    {
        volatile bool enabled = false;          // +0x04
        bool activitySubscribed = false;        // +0x05
        DiscordUser user{};                     // +0x10
        IDiscordCore* core = nullptr;           // +0x1A8
        bool userSubscribed = false;            // Initialize's OnCurrentUserUpdate lambda
    };
    Mgr g_mgr;

    void __cdecl IgnoreResult(void*, int32_t) {}   // the empty lambdas (0x10221A90)

    // Initialize's lambda (FUN_10221aa0).
    void __cdecl OnCurrentUserUpdate(void*)
    {
        if (!g_mgr.userSubscribed || !g_mgr.core)
            return;
        IDiscordUserManager* users = g_mgr.core->get_user_manager(g_mgr.core);
        if (users->get_current_user(users, &g_mgr.user) == 0)
            Packet(0x52A).U64(static_cast<uint64_t>(g_mgr.user.id)).Str(g_mgr.user.username).Send();
    }

    // 0x90D's lambdas: OnActivityJoinRequest (FUN_10221bb0) and OnActivityJoin (FUN_10221c90).
    void __cdecl OnActivityJoinRequest(void*, DiscordUser* user)
    {
        if (g_mgr.activitySubscribed)
            Packet(0x529).U64(static_cast<uint64_t>(user->id)).Send();
    }
    void __cdecl OnActivityJoin(void*, const char* secret)
    {
        if (g_mgr.activitySubscribed)
            Packet(0x52B).Str(secret).Send();
    }
    void __cdecl Ignore(void*, ...) {}

    IDiscordUserEvents g_userEvents = {&OnCurrentUserUpdate};
    IDiscordActivityEvents g_activityEvents = {
        &OnActivityJoin,
        reinterpret_cast<void(__cdecl*)(void*, const char*)>(&Ignore),
        &OnActivityJoinRequest,
        reinterpret_cast<void(__cdecl*)(void*, int32_t, DiscordUser*, DiscordActivity*)>(&Ignore),
    };

    unsigned __stdcall CallbackThread(void*)   // FUN_10220ae0
    {
        while (g_mgr.enabled)
        {
            g_mgr.core->run_callbacks(g_mgr.core);
            Sleep(0x32);
        }
        return 0;
    }

    void Initialize()   // FUN_10220b10
    {
        if (g_mgr.enabled)
            return;
        typedef int32_t(__cdecl * DiscordCreate_t)(int32_t, DiscordCreateParams*, IDiscordCore**);
        static DiscordCreate_t create = nullptr;
        if (!create)
        {
            HMODULE sdk = LoadLibraryA("discord_game_sdk.dll");
            create = sdk ? reinterpret_cast<DiscordCreate_t>(GetProcAddress(sdk, "DiscordCreate")) : nullptr;
            if (!create)
            {
                AscLog::Printf("Discord: discord_game_sdk.dll / DiscordCreate unavailable; disabled");
                return;
            }
        }
        DiscordCreateParams params{};
        params.clientId = 0x0C7C30A11D421000ll;
        params.flags = 1;
        params.eventData = &g_mgr;
        for (auto& m : params.managers)
            m.version = 1;
        params.managers[1].events = &g_userEvents;
        params.managers[3].events = &g_activityEvents;
        IDiscordCore* core = nullptr;
        const int32_t result = create(2, &params, &core);
        if (result != 0 || !core)
        {
            if (core)
                core->destroy(core);
            g_mgr.core = nullptr;
            AscLog::Printf("Discord: DiscordCreate result %d", result);
            Packet(0x52E).U32(static_cast<uint32_t>(result)).Send();
            return;
        }
        g_mgr.core = core;
        g_mgr.userSubscribed = true;
        g_mgr.enabled = true;
        const uintptr_t thread = _beginthreadex(nullptr, 0, &CallbackThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(reinterpret_cast<HANDLE>(thread));
        AscLog::Printf("Discord: core created");
    }

    // ---- packets -------------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadSized(CDataStore* p)   // FUN_100d3680
    {
        const uint32_t n = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += n;
        return s;
    }
    void Copy(char* dst, const std::string& s)   // the wrapper's setters: strncpy 128, last byte 0
    {
        strncpy(dst, s.c_str(), 0x80);
        dst[0x7F] = 0;
    }

    void UpdateActivity(DiscordActivity& a)   // FUN_10088100
    {
        IDiscordActivityManager* m = g_mgr.core->get_activity_manager(g_mgr.core);
        m->update_activity(m, &a, nullptr, &IgnoreResult);
    }

    void __cdecl OnInitialize(void*, uint32_t, uint32_t, CDataStore*)   // handler_0x090c
    {
        if (!g_mgr.enabled)
            return;
        DiscordActivity a{};
        a.type = 0;
        Copy(a.state, "Selecting World");
        Copy(a.details, "");
        _time64(nullptr);   // taken and unused
        a.timestamps.start = 0;
        a.timestamps.end = 0;
        Copy(a.assets.largeImage, "");
        Copy(a.assets.smallImage, "ascension_logo");
        Copy(a.assets.largeText, "");
        Copy(a.assets.smallText, "");
        Copy(a.party.id, "");
        a.party.size.current = 0;
        a.party.size.max = 0;
        Copy(a.secrets.match, "");
        Copy(a.secrets.join, "");
        Copy(a.secrets.spectate, "");
        UpdateActivity(a);
    }

    void __cdecl OnUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10221060
    {
        if (!g_mgr.enabled)
            return;
        p->m_read += 1;
        const std::string state = ReadSized(p), details = ReadSized(p);
        const int64_t start = Read<int64_t>(p), end = Read<int64_t>(p);
        const std::string largeImage = ReadSized(p), smallImage = ReadSized(p), largeText = ReadSized(p),
                          smallText = ReadSized(p), partyId = ReadSized(p);
        const int32_t size = Read<int32_t>(p), max = Read<int32_t>(p);
        const std::string match = ReadSized(p), join = ReadSized(p), spectate = ReadSized(p);
        DiscordActivity a{};
        a.type = 0;
        Copy(a.state, state);
        Copy(a.details, details);
        a.timestamps.start = start;
        a.timestamps.end = end;
        Copy(a.assets.largeImage, largeImage);
        Copy(a.assets.smallImage, smallImage);
        Copy(a.assets.largeText, largeText);
        Copy(a.assets.smallText, smallText);
        Copy(a.party.id, partyId);
        a.party.size.current = size;
        a.party.size.max = max;
        Copy(a.secrets.match, match);
        Copy(a.secrets.join, join);
        Copy(a.secrets.spectate, spectate);
        if (!g_mgr.activitySubscribed)
        {
            g_mgr.activitySubscribed = true;
            UpdateActivity(a);
        }
    }

    void OpenGuildInvite(const std::string& code)   // FUN_10220dd0
    {
        if (!g_mgr.enabled)
            return;
        IDiscordOverlayManager* overlay = g_mgr.core->get_overlay_manager(g_mgr.core);
        overlay->open_guild_invite(overlay, code.c_str(), nullptr, &IgnoreResult);
    }

    void __cdecl OnGuildInvite(void*, uint32_t, uint32_t, CDataStore* p)   // handler_0x090e
    {
        if (g_mgr.enabled)
            OpenGuildInvite(ReadSized(p));
    }

    // ---- bindings ------------------------------------------------------------------------------------
    int DiscordFeaturesEnabled(lua_State* L)
    {
        PushBool(L, g_mgr.enabled);
        return 1;
    }

    int DiscordFeaturesOverlayEnabled(lua_State* L)
    {
        if (!g_mgr.enabled)
        {
            PushBool(L, false);
            return 1;
        }
        bool on = false;
        IDiscordOverlayManager* overlay = g_mgr.core->get_overlay_manager(g_mgr.core);
        overlay->is_enabled(overlay, &on);
        PushBool(L, on);
        return 1;
    }

    // Exactly one argument, a non-empty string.
    int DiscordOpenGuildInvite(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const std::string code = CheckString(L, 1);
        if (!code.empty())
            OpenGuildInvite(code);
        return 0;
    }

    // ---- the original's detour 0x10A46350 over 0x4DB9F0 ----------------------------------------------
    typedef void(__cdecl* Fn4DB9F0_t)();
    Fn4DB9F0_t g_4DB9F0 = nullptr;
    void __cdecl Detour4DB9F0()
    {
        g_4DB9F0();
        AscLog::Printf("Discord: client init 0x4DB9F0 returned");
        Initialize();
    }

    void Init()
    {
        sDC.AddPacketHandler(0x90C, CNetClientCustomPacket((void*)&OnInitialize, nullptr));
        sDC.AddPacketHandler(0x90D, CNetClientCustomPacket((void*)&OnUpdate, nullptr));
        sDC.AddPacketHandler(0x90E, CNetClientCustomPacket((void*)&OnGuildInvite, nullptr));
        g_4DB9F0 = reinterpret_cast<Fn4DB9F0_t>(AscRuntime::Detour(0x4DB9F0, 8, reinterpret_cast<void*>(&Detour4DB9F0)));
    }

    const AscBindings::Binding kBindings[] = {   // FUN_10221dc0
        {nullptr, "DiscordFeaturesEnabled", DiscordFeaturesEnabled},
        {nullptr, "DiscordFeaturesOverlayEnabled", DiscordFeaturesOverlayEnabled},
        {nullptr, "DiscordOpenGuildInvite", DiscordOpenGuildInvite},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
