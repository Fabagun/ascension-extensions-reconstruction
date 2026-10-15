// The original's detour 0x10A42DD0 over a game object's use (0x70F680): after the client function returns,
// the mystic-altar branch (FUN_102EA1A0 / FUN_102EA2C0), then GAMEOBJECT_USED("%u", entry) for every use.
// FrameXML's FelCommutationUtil toggles Fel Commutation from it (Enum.GameObject.FelCommutationPortal).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>

namespace
{
    void OnGameObjectUse(void* object)
    {
        const uint8_t* go = static_cast<const uint8_t*>(object);
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(go + 8) + 0xC);
        AscMysticEnchant::GameObjectUsed(entry);
        AscRuntime::Signal("GAMEOBJECT_USED", "%u", entry);
    }

    void Init() { AscRuntime::OnGameObjectUse(&OnGameObjectUse); }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
