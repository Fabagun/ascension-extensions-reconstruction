// Alpha ambience sounds (installer FUN_1031e8c0).
//
//   CVar useAlphaAmbienceSound (FUN_10114540 -> world queue; flags 1, "0", category 7, last argument 1;
//     storage 0x10BE4128), callback FUN_1031e840: on "1" every SoundAmbience.dbc row (client container
//     0xAD464C, FUN_100b2110) named in the table below gets its day / night ambience ids (+4 / +8)
//     replaced in place. Any other value does nothing -- the stock ids come back only with a restart.
//   The table is an unordered_map built by the static initialiser FUN_1007dd80 from 15 triples; key 0x2A
//     appears twice and the first one wins.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <cstdint>
#include <cstring>

namespace
{
    struct Ambience { uint32_t id, day, night; };
    const Ambience kAlpha[] = {   // 0x10BE410C (FUN_1007dd80; the second 0x2A entry {0x33FAF, 0x33FB0} is dropped)
        {0x19, 0x33F77, 0x33F78}, {0x1C, 0x33F7F, 0x33F80}, {0x1E, 0x33F75, 0x33F76}, {0x1F, 0x33F6F, 0x33F70},
        {0x20, 0x33F9F, 0x33FA0}, {0x21, 0x33FA1, 0x33FA2}, {0x25, 0x33F93, 0x33F94}, {0x26, 0x33F79, 0x33F7A},
        {0x28, 0x33F83, 0x33F84}, {0x2A, 0x33F73, 0x33F74}, {0x2E, 0x33F7B, 0x33F7C}, {0x2F, 0x33FAD, 0x33FAE},
        {0x30, 0x33F88, 0x33F89}, {0x31, 0x33F81, 0x33F82},
    };

    int __cdecl OnChanged(void*, const char*, const char* value, void*)   // FUN_1031e840
    {
        if (strcmp(value, "1") == 0)
            for (const Ambience& a : kAlpha)
                if (uint8_t* row = AscClientDbc::Row(0xAD464C, a.id))
                {
                    *reinterpret_cast<uint32_t*>(row + 4) = a.day;
                    *reinterpret_cast<uint32_t*>(row + 8) = a.night;
                }
        return 1;
    }

    void Init()   // FUN_1031e8c0
    {
        AscClientOptions::CVarSpec spec{"useAlphaAmbienceSound", "0", 1, 7, reinterpret_cast<void*>(&OnChanged)};
        spec.a7 = true;
        AscClientOptions::QueueWorldCVar(spec);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
