#pragma once
// The original DLL's CM2Model helpers shared between the Model widget methods (AscModelMethods.cpp)
// and the character-creation preview (AscCharacterCreate.cpp).
#include <cstdint>

namespace AscModelKit
{
    // pos xyz, rotation xyz (radians), scale -- the 7-float transform the original builds matrices from.
    struct Transform { float pos[3]; float rot[3]; float scale; };

    // Matrix from identity (0x407F40): translate (FUN_102bd530), rotate x / y / z (FUN_102bd230 / 310 /
    // 3f0), scale (FUN_102bd4d0); set on the model with 0x4D8630.
    void SetTransform(void* model, const Transform& t);

    // FUN_103097f0: the spell's SpellVisual state kit (+0x10) through the kit player, looping.
    bool PlayStateKit(void* model, uint32_t spell, bool noAnim);
    // FUN_10309760: the spell's SpellVisual precast kit (+0x04) through the kit player.
    bool PlayPrecastKit(void* model, uint32_t spell, bool noAnim);

    void PlayAnim(void* model, uint32_t anim);   // 0x832AB0(-1, anim, NaN, 0, 1.0, 1, 1)
    void Detach(void* model, uint32_t attach);   // 0x827560
}
