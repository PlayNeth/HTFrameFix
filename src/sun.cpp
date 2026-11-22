#include "common.h"
#include "sun.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"

// ---------------------------------------------------------------------------
// Sun
// ---------------------------------------------------------------------------
// sunBehindScenery: the sun and its flare (0x409760, drawn by the HUD) are
// sprites drawn on top of everything (depth test off: 0x42FA30 mode 0), at a
// depth right in front of the camera (2.0), so the sun shows through cliffs and
// buildings. The flare's parts are 8 sprites along the line through the screen's
// middle, each at the sun's place times a factor (0x4FD45C); the ones at the sun
// itself (factor 1) are now drawn with the depth test on and far away: beyond
// all scenery, in front of the sky (which is drawn at the farthest depth there
// is, and writes it). The other parts, the rings across the screen, are drawn
// as before.
static const ht::Sig kSunSigs[] = {
    { 0x409A29, "\x68\x00\x00\x00\x40\x50\x51\x8B\x8E\x1C\xD4\x4F\x00\xE8\xB5\xDA\x02\x00", 18 },   // push 2.0 ... call 0x4374F0
    { 0x42FA30, "\x83\xF9\x03\x56\x8B\x35\x00\xEA\x51\x00", 10 },  // depth mode
    { 0x4374F0, "\x81\xEC\xBC\x00\x00\x00\xD9\x05\x3C\x92\x48\x00", 12 },   // sprite
};

typedef void (__fastcall *Sprite_t)(void* obj, DWORD angle, float x, float y, float depth, float size);
typedef DWORD (__fastcall *DepthMode_t)(DWORD mode);
extern "C" { DWORD g_sunPart = 0; void* g_sunSpriteFn = nullptr; void SunSpriteStub(); }
static const float kSunDepth = 60000.0f;                           // (the sky's is 65534, the farthest)
static void __fastcall SunSprite(void* obj, DWORD angle, float x, float y, float depth, float size) {
    const Sprite_t draw = reinterpret_cast<Sprite_t>(0x4374F0);
    const float factor = g_sunPart < 0x20 ? G<float>(0x4FD45C + g_sunPart) : 0.0f;
    if (factor < 0.99f || factor > 1.01f) { draw(obj, angle, x, y, depth, size); return; }
    const DepthMode_t mode = reinterpret_cast<DepthMode_t>(0x42FA30);
    DWORD was = mode(2);                                           // depth test on, no depth written
    draw(obj, angle, x, y, kSunDepth, size);
    mode(was);
}
asm(".text\n"
    // 0x409A36: call 0x4374F0 (ecx = sprite, edx = angle, 4 stack arguments, its own ret 16); esi = the part * 4
    ".globl _SunSpriteStub\n"
    "_SunSpriteStub:\n"
    "    movl %esi, _g_sunPart\n"
    "    jmp *_g_sunSpriteFn\n");

void InstallSun() {
    if (g_cfg.sunBehindScenery) {
        g_sunSpriteFn = reinterpret_cast<void*>(&SunSprite);
        bool ok = SigsMatch(kSunSigs, sizeof(kSunSigs) / sizeof(kSunSigs[0])) &&
                  PatchCall(0x409A36, reinterpret_cast<void*>(&SunSpriteStub), 5);
        Logf("sun: %s\n", ok ? "hidden by scenery in front of it" : "patch failed, as the game's");
    } else Logf("sun: drawn over everything (sunBehindScenery=0)\n");
}
