// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
#pragma once
#include "common.h"

namespace ht {
    const DWORD callFrame    = 0x47E68D;   // main loop: call 0x471AB0
    const DWORD fnFrame      = 0x471AB0;   // one loop: pump, tick, render, flip, sleep
    const DWORD fnPump       = 0x47E270;   // messages + input
    const DWORD fnBeginFrame = 0x478080;   // clear polygon lists, begin scene
    const DWORD fnClear      = 0x478060;   // fastcall(ecx = colour index, -1 = keep)
    const DWORD fnTick       = 0x471B80;   // the game tick (update and draw)
    const DWORD fnRender     = 0x478110;   // sort and send the frame's polygons
    const DWORD fnPresent    = 0x478040;   // flip
    const DWORD fnFade       = 0x42F7B0;   // fastcall(ecx): screen fade, after the draw
    const DWORD fnQuit       = 0x47D920;   // WM_QUIT handling of the game's pump
    const DWORD callDraw     = 0x42F78A;   // call [0x14549BC] (6 bytes): the mode's draw
    const DWORD pushMulA     = 0x4694C0;   // push matrix (ecx) on the stack: top = top * ecx via 0x4695B0
    const DWORD mulInto      = 0x4695B0;   // ecx = parent * [esp+4] (edx = parent); also called directly
    const DWORD mulFromPush  = 0x4694E7;   // return address of 0x4694C0's call to it
    // matrices built from the current top of the stack (facing the camera): already follow the view
    const DWORD faceBuild[3] = { 0x4696D0, 0x469760, 0x4697F0 };
    // and two globals built the same way by 0x408EF0 / 0x408F70 just before the
    // keyframe draw 0x408B10 pushes them (turned about one axis to the camera)
    const DWORD faceGlobal[2] = { 0x4FD330, 0x4FD3A0 };
    // 0x408B10 pushing the animation's keyframe matrix (a new one each frame of
    // the animation, from a table other objects may share): returns of those pushes
    const DWORD keyframePush[4] = { 0x408BC6, 0x408BEC, 0x408C2D, 0x408C3E };
    const DWORD spriteDraw   = 0x45BD80;   // draws one of the objects at [0x569EC0] (0x54 each): spray,
                                           //   splashes; its matrix faces the camera object [0x144FA50]
    const DWORD spritePush   = 0x45C088;   //   and is pushed here (call 0x469500)
    const DWORD screenUp     = 0xF90BF0;   // 1 = a menu screen is up (pause, options): the race is paused
    const DWORD pushMulB     = 0x469500;   // same, general multiply
    const DWORD viewSet      = 0x46947F;   // in 0x469470 after the view base is built (esi = camera)
    const DWORD drawRace     = 0x44C9C0;   // race draw function

    // game globals
    const DWORD slowFrame    = 0xF90C04;   // 1 = skip drawing this loop
    const DWORD lastFrameUs  = 0x4F527C;   // previous loop's time (us)
    const DWORD clearCount   = 0x503D4C;   // frames before the screen is cleared again
    const DWORD drawFn       = 0x14549BC;
    const DWORD drawArg      = 0x14549B8;
    const DWORD pendingDrawFn = 0x51E270;  // copied to drawFn at the start of 0x42F700
    const DWORD fadeCount    = 0x51E260;
    const DWORD fadeArg      = 0x145544C;
    const DWORD tickCount    = 0x145499C;  // ticks; advanced after the draw (0x42F940)
    const DWORD tickFloat    = 0x14549B4;
    const DWORD tickDelta    = 0x1454990;
    const DWORD tickParity   = 0x1454998;
    const DWORD tickOne      = 0x1454994;
    const DWORD viewBase     = 0x144FA60;  // matrix stack base (the view), 0x6C bytes
    const DWORD matStack     = 0x144FA60;  // the matrix stack: 32 entries of 0x6C
    const DWORD matStackEnd  = 0x14507E0;  //   (its depth counter follows it)
    const DWORD warnArmed    = 0x4FD550;   // 0x409390 (race draw): counts draws while set, a sound at 10
    const DWORD warnCount    = 0x4FD748;

    // Memory the race draw changes as it draws, so drawing twice in a tick
    // would differ from drawing once. Saved before and after the tick's draw;
    // an in-between frame starts from the first copy and leaves the second.
    struct Range { DWORD addr; DWORD len; };
    constexpr Range kDrawState[] = {
        { 0x557980, 0x760 },   // sky (0x44D1E0): layers at 0x557A90 (0xB8 each) are scrolled and their
                               //   texture coordinates scaled (/256) in place; colours and flags after them,
                               //   and the state of 0x44E300 / 0x44E530 / 0x44F7F0 up to 0x5580DC
        { 0x4F0250, 0x8 },     // fog near/far: eased 1% toward the target per draw (0x45D290)
        { 0x4FD548, 0x208 },   // 0x409390: draw counter, flags and the lights it adds
    };

    struct Sig { DWORD addr; const char* bytes; int len; };
    const Sig kSigs[] = {
        { callFrame,    "\xE8\x1E\x34\xFF\xFF\xEB\xF9", 7 },             // call 0x471AB0 / jmp
        { fnFrame,      "\xE8\x7B\xBA\x00\x00\xA3\xE0\xFC", 8 },
        { fnPump,       "\xA1\xD0\xF6\x44\x01\x83\xEC\x24", 8 },
        { fnBeginFrame, "\xE8\xAB\x3E\x00\x00\xE8\x26\x3E", 8 },
        { fnClear,      "\x83\xF9\xFF\x74\x06\x89\x0D\x04", 8 },
        { fnTick,       "\xE8\x8B\x0D\xF9\xFF\xE8\x16\x10", 8 },
        { fnRender,     "\xE8\xAB\x9E\xFF\xFF\x85\xC0\x74", 8 },
        { fnPresent,    "\xE8\x0B\xA0\xFF\xFF\xFF\x15\xF4", 8 },
        { fnFade,       "\x51\x53\x55\x56\x57\x8B\xE9\xE8", 8 },
        { fnQuit,       "\xA1\xC0\xF6\x44\x01\x85\xC0\x74", 8 },
        { 0x42F784,     "\x8B\x0D\xB8\x49\x45\x01\xFF\x15\xBC\x49\x45\x01\xA1\x60\xE2\x51\x00", 17 },
        { pushMulA,     "\x8B\x15\xE8\x07\x45\x01\xA1\xE0\x07\x45\x01", 11 },
        { 0x4694E2,     "\xE8\xC9\x00\x00\x00", 5 },                     // call 0x4695B0 (returns to 0x4694E7)
        { mulInto,      "\x8B\x44\x24\x04\x56\x57\x8B\x78\x68", 9 },
        { 0x4696D0,     "\x83\xEC\x0C\x56\x8B\xF1\xE8", 7 },
        { 0x469760,     "\x83\xEC\x0C\x56\x8B\xF1\xE8", 7 },
        { 0x4697F0,     "\x83\xEC\x0C\x56\x8B\xF1\xE8", 7 },
        { spriteDraw,   "\x83\xEC\x38\xD9\x41\x14\x8B\x41", 8 },       // sub esp,0x38 / fld [ecx+0x14]
        { 0x45C083,     "\xE8\x78\xD4\x00\x00", 5 },                     // call 0x469500 (returns to spritePush)
        { 0x476655,     "\xA3\x64\x0B\x69\x00\x89\x0D\xF0\x0B\xF9\x00", 11 },   // screenUp set
        { pushMulB,     "\x8B\x15\xE8\x07\x45\x01\xA1\xE0\x07\x45\x01", 11 },
        { 0x469470,     "\x56\x8B\xF1\x8B\xD6\xB9\x60\xFA\x44\x01\xE8", 11 },
        { viewSet,      "\x8B\x0D\xE4\x07\x45\x01\xA1\xE8\x07\x45\x01", 11 },
        { drawRace,     "\x83\xEC\x10\x33\xC9\xE8\x46\xE5", 8 },
        { 0x44C05F,     "\xB9\xC0\xC9\x44\x00", 5 },                     // mov ecx, 0x44C9C0 (race draw)
        { 0x42F940,     "\x83\xEC\x08\xA1\x9C\x49\x45\x01", 8 },         // tick counter advance
        { 0x409760,     "\xA1\xE0\x63\x4F\x00", 5 },                     // light glows: mov eax, [0x4F63E0]
        { 0x44D120,     "\x51\xA1\x10\x80\x55\x00\x85\xC0", 8 },     // sky angles (called again, not patched)
        { 0x42F70F,     "\xA1\x70\xE2\x51\x00", 5 },                     // mov eax, [0x51E270] (pending draw)
        { 0x42F720,     "\xA3\xBC\x49\x45\x01", 5 },                     // mov [0x14549BC], eax
        { 0x408BC1,     "\xE8\xFA\x08\x06\x00", 5 },                     // keyframe pushes (call 0x4694C0)
        { 0x408BE7,     "\xE8\xD4\x08\x06\x00", 5 },
        { 0x408C28,     "\xE8\x93\x08\x06\x00", 5 },
        { 0x408C39,     "\xE8\x82\x08\x06\x00", 5 },
        { 0x408BF1,     "\xB9\x30\xD3\x4F\x00", 5 },                     // mov ecx, 0x4FD330 (pushed)
        { 0x408C00,     "\xB9\xA0\xD3\x4F\x00", 5 },                     // mov ecx, 0x4FD3A0
        { 0x408C32,     "\xB9\x30\xD3\x4F\x00", 5 },
        { 0x408C43,     "\xB9\xA0\xD3\x4F\x00", 5 },
    };

    // "Drawn this frame?" stamps: the draw marks scene nodes, objects, mesh and
    // light caches with the tick counter and skips what already carries it. An
    // in-between frame (same counter) would skip the whole scene, so these
    // reads of the counter are pointed at a stamp of our own: the counter
    // during ticks, a new value for every in-between frame. Each is
    // "mov reg, [0x145499C]"; the address is its last 4 bytes.
    const Sig kStampSites[] = {
        { 0x41757C, "\x8B\x15\x9C\x49\x45\x01", 6 },   // 0x4174A0: node drawn this frame? ([ecx+0xDC])
        { 0x41791B, "\x8B\x15\x9C\x49\x45\x01", 6 },
        { 0x45FB98, "\x8B\x35\x9C\x49\x45\x01", 6 },   // 0x45FB90 / 0x45FC30: draw a scene node once
        { 0x45FC38, "\x8B\x35\x9C\x49\x45\x01", 6 },
        { 0x453A40, "\xA1\x9C\x49\x45\x01", 5 },       // 0x4539F0: build the frame's lists once per node
        { 0x453A54, "\x8B\x15\x9C\x49\x45\x01", 6 },
        { 0x453AA7, "\x8B\x15\x9C\x49\x45\x01", 6 },
        { 0x453B6F, "\x8B\x1D\x9C\x49\x45\x01", 6 },
        { 0x45405C, "\x8B\x15\x9C\x49\x45\x01", 6 },   // 0x453E00: the same lists' stamps
        { 0x458FAA, "\xA1\x9C\x49\x45\x01", 5 },       // 0x458F90: draw an object once
        { 0x437991, "\xA1\x9C\x49\x45\x01", 5 },       // 0x437970: mesh vertex cache
        { 0x439889, "\x8B\x0D\x9C\x49\x45\x01", 6 },   // 0x439810: light colour cache
        { 0x4398EA, "\x8B\x15\x9C\x49\x45\x01", 6 },
    };
}
