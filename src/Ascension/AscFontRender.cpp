// FontRender.cpp: the client's text rendered by FreeType 2.14.1 as signed distance fields (module init
// FUN_10237650; the original links FreeType statically, as this build does -- third_party/freetype).
//
// The client links a FreeType 2.0-era library. The original swaps its entry points for the linked 2.14.1
// (the hooks never call the originals) and replaces the one client function that reads FreeType's structs:
//   0x991320 -> 0x10237150    library creation: one shared FT_Init_FreeType library for the process; if
//                             an outline renderer exists, the sdf and bsdf renderers get "spread" = 4
//   0x992CB0 -> 0x10237100    library release: FT_Done_FreeType on it, and the next creation starts over
//   0x993370 FT_New_Memory_Face   0x992610 FT_Done_Face       0x9911A0 FT_Get_Char_Index
//   0x991050 FT_Get_Kerning       0x992DA0 FT_Load_Glyph      0x992780 FT_Set_Pixel_Sizes
//   0x992B60 FT_Render_Glyph
//   0x6C8CC0 -> FUN_10237240  one glyph into the client's glyph record: loaded unhinted, outlines
//                             emboldened by size (and less for bold faces), rendered normally and then as
//                             an SDF; the bitmap is copied without its 2-pixel border
//   0x6BE230 -> 0x10237210    font system start-up: first loads pixel shader "Font" from Shaders\Pixel
//   0x6C4AD0 -> FUN_10236fd0  before a string draws: binds that shader (0x4E) with its constants c4 =
//                             {0,0,0,1} and c5 = {font +0x24C, outline 0 / 1 / 2 (+0x180 bits 1, 8), 0, 0}
//   0x6C8E70 / 0x6C9330 / 0x6C9420 / 0x6C9C70 -> FUN_10238130  the glyph into the font texture (below)
// Patches: 0x6BE290 NOPs the client's own module setup after library creation (call 0x990650); the
// setne at 0x6CA067 becomes "mov al, 1".
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_RENDER_H
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace
{
    const char kSource[] = "C:\\a\\Ascension.CustomDLLs\\Ascension.CustomDLLs\\src\\Ascension.Extensions\\FontRender.cpp";

    void* ClientAlloc(uint32_t size, int line)   // FUN_100b2ce0 -> SMemAlloc 0x76E540
    {
        return reinterpret_cast<void*(__stdcall*)(uint32_t, const char*, int, int)>(0x76E540)(size, kSource, line, 0);
    }

    void WriteCode(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    }

    // ---- the shared library (0x10BE2518 / flag 0x10BE2514) ----------------------------------------------
    FT_Library g_library = nullptr;
    bool g_libraryReady = false;
    const FT_Int kSpread = 4;   // 0x10B3E1B0

    FT_Error __cdecl CreateLibrary(void* /*memory*/, FT_Library* out)   // 0x10237150
    {
        if (g_libraryReady)
        {
            *out = g_library;
            return 0;
        }
        const FT_Error error = FT_Init_FreeType(&g_library);
        if (FT_Get_Renderer(g_library, FT_GLYPH_FORMAT_OUTLINE))
        {
            FT_Property_Set(g_library, "sdf", "spread", &kSpread);
            FT_Property_Set(g_library, "bsdf", "spread", &kSpread);
        }
        g_libraryReady = true;
        *out = g_library;
        return error;
    }

    FT_Error __cdecl ReleaseLibrary()   // 0x10237100
    {
        const FT_Error error = FT_Done_FreeType(g_library);
        g_library = nullptr;
        g_libraryReady = false;
        return error;
    }

    // ---- 0x6C8CC0: one glyph (FUN_10237240) ---------------------------------------------------------------
    // The client's glyph record, 0x38 bytes.
    struct Glyph
    {
        uint8_t* bitmap;     // +0x00  SMemAlloc'd
        uint32_t size;       // +0x04  bytes
        uint32_t width;      // +0x08
        uint32_t rows;       // +0x0C
        int32_t cellWidth;   // +0x10
        float advance;       // +0x14
        float left;          // +0x18
        uint32_t stride;     // +0x1C
        int32_t y;           // +0x20
        int32_t top;         // +0x24
        uint32_t reserved[4];
    };
    static_assert(sizeof(Glyph) == 0x38, "Glyph");

    bool __cdecl RenderGlyph(FT_Face face, uint32_t pixelSize, FT_ULong code, int32_t baseline, Glyph* out,
                             uint32_t /*a6*/, uint32_t minCell)
    {
        const FT_UInt index = FT_Get_Char_Index(face, code);
        if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
            return false;
        FT_GlyphSlot slot = face->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
        {
            float strength = 0.0f;
            bool embolden = true;
            if (!(face->style_flags & FT_STYLE_FLAG_BOLD))
            {
                if (pixelSize <= 10)
                    strength = 0.05f;
                else if (pixelSize <= 12)
                    strength = 0.1f;
                else
                    embolden = false;
            }
            else if (pixelSize <= 10)
                strength = 0.03f;
            else if (pixelSize <= 12)
                strength = 0.06f;
            else
                embolden = false;
            if (embolden)
                FT_Outline_Embolden(&slot->outline, static_cast<FT_Pos>(strength * 64.0f));
        }
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != 0)
            return false;

        slot = face->glyph;
        FT_Pos adv = slot->metrics.horiAdvance;
        const float advance = static_cast<float>((adv + ((adv >> 31) & 0x3F)) >> 6) + 1.0f;
        uint8_t* buffer = slot->bitmap.buffer;
        if (!buffer || !slot->bitmap.width || !slot->bitmap.rows)
        {
            const uint32_t cell = minCell < 6 ? 6 : minCell;
            memset(out, 0, sizeof(Glyph));
            uint8_t* p = static_cast<uint8_t*>(ClientAlloc(1, 0xB6));
            if (!p)
                return false;
            *p = 0;
            out->bitmap = p;
            out->cellWidth = static_cast<int32_t>(cell + 1);
            out->size = 1;
            out->stride = 1;
            out->width = 1;
            out->rows = 1;
            out->left = 0.0f;
            out->y = 0;
            out->top = 0;
            FT_Pos a = face->glyph->metrics.horiAdvance;
            out->advance = static_cast<float>((a + ((a >> 31) & 0x3F)) >> 6) + 1.0f;
            return true;
        }

        const uint32_t cell = minCell < 6 ? 6 : minCell;
        const uint32_t rows = slot->bitmap.rows;
        const int32_t pitch = slot->bitmap.pitch;
        const int32_t bitmapLeft = slot->bitmap_left;
        const uint32_t width = slot->bitmap.width > 4 ? slot->bitmap.width - 4 : 0;   // without the 2-px border
        const uint32_t height = rows > 4 ? rows - 4 : 0;
        const uint32_t stride = width ? width : 1;
        const uint32_t size = stride * height;
        const int32_t cellBase = static_cast<int32_t>(cell <= 3 ? 3 : cell - 3);

        if (!width || !height)
        {
            memset(out, 0, sizeof(Glyph));
            uint8_t* p = static_cast<uint8_t*>(ClientAlloc(1, 0xDC));
            out->bitmap = p;
            if (!p)
                return false;
            *p = 0;
            out->advance = advance;
            out->cellWidth = cellBase;
            out->left = static_cast<float>(bitmapLeft);
            out->stride = 1;
            out->width = 0;
            out->rows = 0;
            out->y = 0;
            out->top = baseline - slot->bitmap_top;
            memset(out->reserved, 0, sizeof(out->reserved));
            return true;
        }

        uint8_t* dst = static_cast<uint8_t*>(ClientAlloc(size, 0xF2));
        if (!dst)
            return false;
        if (pitch >= 0)
        {
            buffer += 2;
            for (uint32_t i = 0; i < height; ++i)
                memcpy(dst + i * stride, buffer + (i + 2) * pitch, width);
        }
        else
        {
            for (uint32_t i = 0; i < height; ++i)
                memcpy(dst + i * stride, buffer - static_cast<int32_t>(rows - i - 3) * pitch + 2, width);
        }
        out->bitmap = dst;
        out->size = size;
        out->stride = stride;
        out->advance = advance;
        out->cellWidth = cellBase + static_cast<int32_t>(width);
        out->left = static_cast<float>(bitmapLeft);
        out->width = width;
        out->rows = height;

        // Vertical placement in the pixelSize-high cell (unsigned compares as in the original).
        const uint32_t inner = height - 4;
        int32_t y = 0;
        uint32_t top = 0;
        bool clampTop = true;
        if (inner > pixelSize)
            top = 0;
        else
        {
            const int32_t bearing = slot->bitmap_top - 4;
            if (bearing > baseline)
            {
                y = bearing - baseline;
                top = 0;
                clampTop = false;
            }
            else
                top = static_cast<uint32_t>(baseline - bearing);
        }
        if (clampTop)
        {
            y = 0;
            if (static_cast<int32_t>(top) <= 0)
                top = 0;
            const uint32_t room = pixelSize - inner;
            if (room < top)
            {
                y = static_cast<int32_t>(pixelSize - top - inner);
                top = room;
            }
        }
        int32_t t = static_cast<int32_t>(top) - 2;
        if (t < 0)
        {
            y -= t;
            t = 0;
        }
        else
        {
            const int32_t over = t - static_cast<int32_t>(pixelSize) + static_cast<int32_t>(height);
            if (over > 0)
            {
                t -= over;
                y -= over;
            }
        }
        out->top = t;
        out->y = y;
        memset(out->reserved, 0, sizeof(out->reserved));
        return true;
    }

    // ---- 0x6C8E70 / 0x6C9330 / 0x6C9420 / 0x6C9C70: glyph into the font texture (FUN_10238130) -----------
    // All four client copiers (one per texture format) are replaced by one: the 8-bit SDF value goes into
    // the high byte of a 16-bit texel, rows 0x200 bytes apart. The cell is cleared first; each row gets a
    // 3-texel left border of its first value and is padded to the cell width with its last.
    uint16_t Texel(uint8_t v) { return static_cast<uint16_t>(((v >> 4) << 4 | (v & 0xF)) << 8); }

    void __fastcall CopyGlyph(void* self, void* /*edx*/, const Glyph* g, uint8_t* dst)
    {
        const uint8_t* src = g->bitmap;
        const int32_t stride = static_cast<int32_t>(g->stride);
        const int32_t cellWidth = g->cellWidth;
        const int32_t rows = static_cast<int32_t>(g->rows);
        int32_t height = rows;
        if (self)
            if (const uint8_t* owner = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(self) + 4))
                height = *reinterpret_cast<const int32_t*>(owner + 0x178);

        const int32_t top = g->top;
        int32_t dstRow = top >= 0 ? top : 0;
        int32_t srcSkip = top >= 0 ? 0 : -top;
        if (srcSkip > rows)
            srcSkip = rows;
        if (height < dstRow)
            dstRow = height;
        int32_t count = height - dstRow;
        if (rows - srcSkip < count)
            count = rows - srcSkip;

        for (int32_t r = 0; r < height; ++r)
            if (cellWidth > 0)
                memset(dst + r * 0x200, 0, static_cast<size_t>(cellWidth) * 2);

        uint8_t* base = dst + dstRow * 0x200;
        for (int32_t r = 0; r < count; ++r)
        {
            const uint8_t* line = stride >= 0 ? src + (srcSkip + r) * stride : src - (rows - r - srcSkip - 1) * stride;
            uint16_t* row = reinterpret_cast<uint16_t*>(base + r * 0x200);
            const uint32_t w = g->width;
            uint16_t first = 0, last = 0;
            if (w)
            {
                first = Texel(line[0]);
                last = Texel(line[w - 1]);
            }
            row[0] = row[1] = row[2] = first;
            for (uint32_t i = 0; i < w; ++i)
                row[3 + i] = Texel(line[i]);
            for (int32_t i = static_cast<int32_t>(w) + 3; i < cellWidth; ++i)
                row[i] = last;
        }
    }

    // ---- the "Font" pixel shader ----------------------------------------------------------------------------
    void* g_fontShader = nullptr;   // 0x10BE251C

    typedef int(__cdecl* Fn0_t)();
    Fn0_t g_6BE230 = nullptr;
    int __cdecl FontStartup()   // 0x10237210
    {
        void* device = *reinterpret_cast<void**>(0xC5DF88);
        reinterpret_cast<void(__thiscall*)(void*, void**, int, const char*, const char*, int)>(0x6AA130)(
            device, &g_fontShader, 4, "Shaders\\Pixel", "Font", 1);
        return g_6BE230();
    }

    typedef int(__fastcall* Fn6C4AD0_t)(void*, void*);
    Fn6C4AD0_t g_6C4AD0 = nullptr;
    int __fastcall DrawString(void* self, void* edx)   // FUN_10236fd0
    {
        const uint8_t* font = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(self) + 0x18);
        if (g_fontShader && reinterpret_cast<int(__thiscall*)(void*)>(0x689A50)(g_fontShader) && font)
        {
            reinterpret_cast<void(__cdecl*)(int, void*)>(0x408240)(0x4E, g_fontShader);
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(font + 0x180);
            const uint32_t size = *reinterpret_cast<const uint32_t*>(font + 0x24C);
            float outline = 0.0f;
            if (flags & 1)
                outline = (flags & 8) ? 2.0f : 1.0f;
            float c4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            reinterpret_cast<void(__cdecl*)(int, int, const float*, int)>(0x408210)(4, 4, c4, 1);
            float c5[4] = {static_cast<float>(static_cast<double>(size)), outline, 0.0f, 0.0f};
            reinterpret_cast<void(__cdecl*)(int, int, const float*, int)>(0x408210)(4, 5, c5, 1);
        }
        return g_6C4AD0(self, edx);
    }

    void Init()
    {
        static const uint8_t kMovAl1[3] = {0xB0, 0x01, 0x90};
        WriteCode(0x6CA067, kMovAl1, sizeof(kMovAl1));
        static const uint8_t kNop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        WriteCode(0x6BE290, kNop5, sizeof(kNop5));

        AscRuntime::ReplaceFunction(0x6C9C70, reinterpret_cast<void*>(&CopyGlyph));
        AscRuntime::ReplaceFunction(0x6C9330, reinterpret_cast<void*>(&CopyGlyph));
        AscRuntime::ReplaceFunction(0x6C9420, reinterpret_cast<void*>(&CopyGlyph));
        AscRuntime::ReplaceFunction(0x6C8E70, reinterpret_cast<void*>(&CopyGlyph));
        AscRuntime::ReplaceFunction(0x6C8CC0, reinterpret_cast<void*>(&RenderGlyph));
        AscRuntime::ReplaceFunction(0x991320, reinterpret_cast<void*>(&CreateLibrary));
        AscRuntime::ReplaceFunction(0x993370, reinterpret_cast<void*>(&FT_New_Memory_Face));
        AscRuntime::ReplaceFunction(0x992610, reinterpret_cast<void*>(&FT_Done_Face));
        AscRuntime::ReplaceFunction(0x992DA0, reinterpret_cast<void*>(&FT_Load_Glyph));
        AscRuntime::ReplaceFunction(0x992780, reinterpret_cast<void*>(&FT_Set_Pixel_Sizes));
        AscRuntime::ReplaceFunction(0x9911A0, reinterpret_cast<void*>(&FT_Get_Char_Index));
        AscRuntime::ReplaceFunction(0x991050, reinterpret_cast<void*>(&FT_Get_Kerning));
        AscRuntime::ReplaceFunction(0x992CB0, reinterpret_cast<void*>(&ReleaseLibrary));
        AscRuntime::ReplaceFunction(0x992B60, reinterpret_cast<void*>(&FT_Render_Glyph));
        g_6BE230 = reinterpret_cast<Fn0_t>(AscRuntime::Detour(0x6BE230, 6, reinterpret_cast<void*>(&FontStartup)));
        g_6C4AD0 = reinterpret_cast<Fn6C4AD0_t>(AscRuntime::Detour(0x6C4AD0, 6, reinterpret_cast<void*>(&DrawString)));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
