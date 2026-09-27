// The client's WowClientDB row readers for 41 tables, replaced (module init FUN_101e5880, the FUN_100010f0
// hooks on 0x8A8370..0x8BD8C0; the originals are never called).
//
// Measured by emulating every original reader against the stock one (tools/dbc_reader_schema.py): 40 of
// them read the same bytes into the same record layout as the client's; the only change is which of a
// locstring's 16 slots is kept (FUN_101b0c80's locale map below, not the client's slot = locale), so for
// enUS they are byte-identical. The 41st is ItemPurchaseGroup, which the original gives ItemLimitCategory's
// reader (FUN_101e6ff0 installed twice): 80 bytes of a 104-byte row. Reproduced; see IMPROVEMENTS.md.
//
// Each reader is a fixed read programme, recovered per table by tools/dbc_reader_gen.py.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>
#include <utility>

namespace AscDbcReaders
{
namespace
{
    struct Op
    {
        char kind;   // R raw, S string, L locstring, X skip
        uint16_t offset;
        uint16_t bytes;
    };

    struct Reader
    {
        uintptr_t clientReader;
        const char* table;
        uint32_t rowBytes;
        uintptr_t original;
        Op ops[32];
    };

    const Reader kReaders[] = {
#include <Ascension/AscDbcReaders.generated.inc>
    };
    constexpr size_t kReaderCount = sizeof(kReaders) / sizeof(kReaders[0]);

    // SFileReadFile (__stdcall, 6 arguments).
    typedef int(__stdcall* SFileReadFile_t)(void* file, void* buffer, uint32_t bytes, uint32_t* read, void* overlapped, uint32_t flags);
    const SFileReadFile_t SFileReadFile = reinterpret_cast<SFileReadFile_t>(0x422530);

    bool ReadInto(void* file, void* buffer, uint32_t bytes)
    {
        return SFileReadFile(file, buffer, bytes, nullptr, nullptr, 0) != 0;
    }

    // FUN_101b0c80's slot for the client locale [0xC5DE9C].
    uint32_t LocaleSlot()
    {
        switch (*reinterpret_cast<const uint32_t*>(0xC5DE9C))
        {
            case 0: return 0;
            case 1: return 2;
            case 2: return 3;
            case 3: return 4;
            case 4: return 6;
            case 5: return 8;
            case 6: return 9;
            case 7: return 10;
            case 8: return 11;
            default: return 0xF;
        }
    }

    const char* StringAt(const char* strings, uint32_t offset) { return strings ? strings + offset : nullptr; }

    int ReadRow(const Reader& reader, uint8_t* record, void* file, const char* strings)
    {
        bool ok = true;
        for (const Op& op : reader.ops)
        {
            if (op.kind == 0)
                break;
            switch (op.kind)
            {
                case 'R':
                    ok &= ReadInto(file, record + op.offset, op.bytes);
                    break;
                case 'S':
                {
                    uint32_t offset = 0;
                    ok &= ReadInto(file, &offset, 4);
                    *reinterpret_cast<const char**>(record + op.offset) = StringAt(strings, offset);
                    break;
                }
                case 'L':
                {
                    const uint32_t slot = LocaleSlot();
                    uint32_t kept = 0, dropped = 0;
                    for (uint32_t i = 0; i < 16; ++i)
                        ok &= ReadInto(file, i == slot ? &kept : &dropped, 4);
                    ok &= ReadInto(file, &dropped, 4);   // the locstring flags
                    *reinterpret_cast<const char**>(record + op.offset) = StringAt(strings, kept);
                    break;
                }
                case 'X':
                {
                    uint8_t scratch[0x100];
                    ok &= ReadInto(file, scratch, op.bytes);
                    break;
                }
            }
        }
        return ok ? 1 : 0;
    }

    // One __thiscall(record, file, strings) entry per table; the index picks the programme.
    template <size_t I>
    int __fastcall RowReader(void* record, void* /*edx*/, void* file, const char* strings)
    {
        return ReadRow(kReaders[I], static_cast<uint8_t*>(record), file, strings);
    }

    template <size_t... I>
    void ReplaceAll(std::index_sequence<I...>)
    {
        void* const fns[] = {reinterpret_cast<void*>(&RowReader<I>)...};
        for (size_t i = 0; i < sizeof...(I); ++i)
            AscRuntime::ReplaceFunction(kReaders[i].clientReader, fns[i]);
    }

    void Init()
    {
        ReplaceAll(std::make_index_sequence<kReaderCount>());
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
}
