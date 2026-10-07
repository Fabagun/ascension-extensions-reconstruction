// Ascension's Tooltip:InsertLine / Tooltip:InsertLines (patch-B, SharedXML\TypeExtensions\Tooltip.lua) write a
// new line into GameTooltipTextLeft<i> whenever that font string exists, even past NumLines(). Font strings
// survive from an earlier, longer tooltip, so the text lands on a hidden line and the AddLine calls that follow
// reuse the same slot and overwrite it: a keystone tooltip keeps one Dungeon Modifier of several. Live had the
// same defect: its 6,800 cached keystone items match the server's, so its keystone tooltip was just as short.
//
// Not in the genuine DLL. Applied early (IMPROVEMENTS.md, "Keystone tooltip shows one Dungeon Modifier"): no
// server-side route exists. After the world UI has loaded (Tooltip.lua runs inside it, and again on every
// reload), both methods on the shared GameTooltip method table are wrapped: for the length of the call the
// globals of the lines past NumLines() are hidden, so the original's own "else AddLine" branch runs for them,
// exactly as the contributor's one-condition fix (`if line and i <= numLines`) does. The globals are restored
// afterwards, whatever the call did.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>

namespace
{
    const char* const kFix = R"lua(
local get = GetGameTooltipMetatable
local methods = get and get().__index
if not methods then return end
for _, name in ipairs({ "InsertLine", "InsertLines" }) do
    local original = methods[name]
    if original and not methods["AscFixed" .. name] then
        methods["AscFixed" .. name] = true
        methods[name] = function(self, ...)
            local prefix = self:GetName()
            if not prefix then
                return original(self, ...)
            end
            prefix = prefix .. "TextLeft"
            local hidden = {}
            local i = self:NumLines() + 1
            while _G[prefix .. i] do
                hidden[i] = _G[prefix .. i]
                _G[prefix .. i] = nil
                i = i + 1
            end
            local ok, err = pcall(original, self, ...)
            for index, line in pairs(hidden) do
                _G[prefix .. index] = line
            end
            if not ok then
                error(err, 0)
            end
        end
    end
end
)lua";

    void Apply()
    {
        reinterpret_cast<void(__cdecl*)(const char*, const char*, void*)>(0x819210)(kFix, "AscTooltipInsertFix", nullptr);
    }

    void Init()
    {
        AscRuntime::OnAfterEnterWorld(&Apply);
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
