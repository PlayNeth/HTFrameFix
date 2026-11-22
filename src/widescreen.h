// What the other parts use from widescreen.cpp.
#pragma once
#include "common.h"
#include "game.h"

namespace ws {
    struct Imm { DWORD addr; int off; bool height; };
    const Imm kImm[] = {                       // 640 / 480 immediates of the Direct3D renderer
        { 0x46CFA2, 1, true  }, { 0x46CFA7, 1, false },   // window size (0x46D0B0)
        { 0x46CFDF, 1, true  }, { 0x46CFE4, 1, false },   // SetDisplayMode
        { 0x46CFF8, 1, true  }, { 0x46CFFD, 1, false },   // window size
        { 0x46D035, 4, false }, { 0x46D03F, 4, true  },   // GetDisplayMode check
        { 0x46D5B9, 4, false }, { 0x46D5C1, 4, true  },   // Z buffer
        { 0x46D804, 4, false }, { 0x46D80C, 4, true  },   // viewport
    };
    // reads of the half screen width (320.5, [0x14508D8]) by the light glows
    // (their screen radius); the address is the last 4 bytes
    const DWORD kHalfWReads[] = { 0x4097B7, 0x4097C2 };
    // The markers for opponents close behind (drawn by the scene draw 0x4174A0,
    // two loops): at the left or right edge (push x = -/+316.25, centred on
    // the screen) or along the bottom (x = offset * [0x48B404] (245) * 1.25)
    const DWORD kMarkerEdges[] = { 0x41783B, 0x417888, 0x417A6B, 0x417AA2 };   // push imm32: -, +, -, +
    const DWORD kMarkerSpread[] = { 0x4178B9, 0x417AB2 };                      // fmul [0x48B404]: address at +2

    const DWORD tagSlot    = 0x46D89F;   // 0x46D850's "lea eax, [eax+0x6364C0] / ret": a triangle's slot
    const DWORD flush      = 0x46CE20;   // draws the triangle buffer
    const DWORD projEnd    = 0x4528AF;   // 0x452530's "add esp, 0x14 / ret 4"
    const DWORD raceDraw   = 0x44C9C0;
    const DWORD callHud    = 0x44CB1B;   // call 0x4316E0
    const DWORD callLast   = 0x44CB20;   // call 0x4352D0 (draws nothing), then the race draw returns
    const DWORD glows      = 0x409760;
    const DWORD callSky    = 0x44CACF;   // the race draw's call 0x44D1E0
    const DWORD glyphQuad  = 0x4513B2;   // call 0x47B7F0 for one glyph (esi = its record + 0x24)
    const DWORD glyphDraw  = 0x451280;   // ecx = first glyph record, edx = count
    const DWORD strQueue   = 0x450E40;   // queue a string's glyphs (or draw them at once)
    const DWORD fadeQuad   = 0x47BB59;   // call 0x47B7F0, 640x480
    const DWORD lockBB     = 0x46BA40;   // lock the back buffer for direct writes
    const DWORD unlockBB   = 0x46BB50;
    const DWORD isD3D      = 0x471FC0;   // 1 = the Direct3D renderer
    const DWORD callBegin  = 0x4780B1;   // 0x478080: call 0x46CDA0 (BeginScene)
    const DWORD callFlip   = 0x478040;   // 0x478040: call 0x472050 (flip)
    const DWORD device     = 0x63F4C0;   // IDirect3DDevice7*

    const DWORD triBuf     = 0x636520;   // D3DTLVERTEX x3 per triangle (0x60)
    const DWORD triCount   = 0x63F550;
    const int   kTriMax    = 0x154;
    const DWORD glyphs     = 0x559200;   // 0x2C each
    const DWORD glyphCount = 0x559138;   // (word)
    const int   kGlyphMax  = 1024;
    const DWORD backBuffer = 0x63F4B4;   // IDirectDrawSurface7*
    const DWORD deviceUp   = 0x63F51C;
    const DWORD lockState  = 0x63F528;
    const DWORD lockBack   = 0x63F52C;
    const DWORD fmt565     = 0x4F497C;

    const ht::Sig kSigs[] = {
        { 0x46CFA2, "\x68\xE0\x01\x00\x00", 5 },
        { 0x46CFA7, "\xBA\x80\x02\x00\x00", 5 },
        { 0x46CFDF, "\x68\xE0\x01\x00\x00", 5 },
        { 0x46CFE4, "\x68\x80\x02\x00\x00", 5 },
        { 0x46CFF8, "\x68\xE0\x01\x00\x00", 5 },
        { 0x46CFFD, "\xBA\x80\x02\x00\x00", 5 },
        { 0x46D035, "\x81\x7C\x24\x10\x80\x02\x00\x00", 8 },
        { 0x46D03F, "\x81\x7C\x24\x0C\xE0\x01\x00\x00", 8 },
        { 0x46D5B9, "\xC7\x44\x24\x48\x80\x02\x00\x00", 8 },
        { 0x46D5C1, "\xC7\x44\x24\x44\xE0\x01\x00\x00", 8 },
        { 0x46D804, "\xC7\x44\x24\x08\x80\x02\x00\x00", 8 },
        { 0x46D80C, "\xC7\x44\x24\x0C\xE0\x01\x00\x00", 8 },
        { 0x46D89F, "\x8D\x80\xC0\x64\x63\x00\xC3", 7 },
        { 0x46CE20, "\xA1\x50\xF5\x63\x00", 5 },
        { 0x4528AF, "\x83\xC4\x14\xC2\x04\x00", 6 },
        { 0x44C9C0, "\x83\xEC\x10\x33\xC9", 5 },
        { 0x44CB1B, "\xE8\xC0\x4B\xFE\xFF", 5 },
        { 0x44CB20, "\xE8\xAB\x87\xFE\xFF", 5 },
        { 0x409760, "\xA1\xE0\x63\x4F\x00", 5 },
        { 0x4513B2, "\xE8\x39\xA4\x02\x00", 5 },
        { 0x451280, "\x83\xEC\x18\x56\x57", 5 },
        { 0x450E40, "\x83\xEC\x30\x8B\x54\x24\x38", 7 },
        { 0x47BB59, "\xE8\x92\xFC\xFF\xFF", 5 },
        { 0x44CACF, "\xE8\x0C\x07\x00\x00", 5 },       // race draw: call 0x44D1E0 (the sky)
        { 0x4097B7, "\xD9\x05\xD8\x08\x45\x01", 6 },
        { 0x4097C2, "\xD8\x0D\xD8\x08\x45\x01", 6 },
        { 0x41783B, "\x68\x00\x20\x9E\xC3", 5 },
        { 0x417888, "\x68\x00\x20\x9E\x43", 5 },
        { 0x417A6B, "\x68\x00\x20\x9E\xC3", 5 },
        { 0x417AA2, "\x68\x00\x20\x9E\x43", 5 },
        { 0x4178B9, "\xD8\x0D\x04\xB4\x48\x00", 6 },
        { 0x417AB2, "\xD8\x0D\x04\xB4\x48\x00", 6 },
        { 0x46BA40, "\x81\xEC\x80\x00\x00\x00\xA1\x1C\xF5\x63\x00", 11 },
        { 0x46BB50, "\xA1\x1C\xF5\x63\x00", 5 },
        { 0x471FC0, "\xE8\x8B\xFF\xFF\xFF", 5 },
        { 0x4780B1, "\xE8\xEA\x4C\xFF\xFF", 5 },
        { 0x478040, "\xE8\x0B\xA0\xFF\xFF", 5 },
        { 0x4513BB, "\x83\xC6\x2C", 3 },       // the glyph loop steps 0x2C
        { 0x4512DF, "\x83\xC6\x24", 3 },       //   from record + 0x24
        { 0x451179, "\x8D\x0C\x85\x00\x92\x55\x00", 7 },   // 0x450E40 draws its own glyphs at 0x559200
    };
}
extern bool g_wsOn;
extern "C" { extern int g_wsLayer; }
extern "C" void WsGlowStub();
void InstallWidescreen();
