// HTFrameFix.asi  v1.0.0
// Frame-rate unlock with interpolation, widescreen, arcade sound and arcade
// settings for Hydro Thunder (PC, Eurocom 2000)
//
// Analysed exe: Hydro.exe SHA-1 745cd3cf9aa0327fb5454a7763ff5f91bed9e3ed
// (PE timestamp 0x38EB1E5C, image base 0x400000, no ASLR). Every patch site is
// byte-checked first; on any other exe the ASI does nothing.
//
// How the game keeps time (main loop 0x47E68D: call 0x471AB0 / jmp back):
//  - One loop = one 30 Hz game tick. 0x471AB0 pumps messages and reads input
//    (0x47E270), runs the tick (0x471B80 -> 0x476930 -> 0x42F700), sends the
//    frame's polygons to the renderer (0x478110), flips (0x478040, Flip with
//    DDFLIP_WAIT), then Sleeps until 33333 us have passed.
//  - If a loop took more than 50 ms the next one is a "slow" frame: [0xF90C04]
//    = 1 skips drawing (0x47B320 drops polygons) and the flip.
//  - 0x42F700 runs the current mode's update function ([0x14549C0], ecx =
//    [0x14549C8]) and then its draw function ([0x14549BC], ecx = [0x14549B8],
//    called at 0x42F78A); afterwards 0x42F940 advances the tick counter
//    [0x145499C]. The race uses update 0x44C7D0 / draw 0x44C9C0.
//  - The camera sets the view through 0x469470 (base of the matrix stack,
//    0x144FA60), and every model is placed by multiplying matrices onto the
//    stack: 0x4694C0 / 0x469500 push one (ecx = 0x6C-byte matrix: rotation +0,
//    translation +0x24, inverse +0x30/+0x54, type +0x68), 0x4695B0 multiplies
//    two (also called directly by the models' code).
//
// What this ASI does (interpolate=1):
//  - The main loop runs ticks on an exact 30 Hz QPC schedule and draws a frame
//    every display refresh (or fpsLimit). In a race the tick's own draw is
//    hidden (the game's "skip drawing" flag, as in its slow frames) and only
//    records the exact matrices; every frame shown, on ticks too, is the race
//    draw called again, with the tick counter and its companions put back to
//    the values the tick's own draw saw, and every matrix blended between the
//    previous tick and this one by the time since the last tick (the frame
//    shows the scene one tick behind).
//  - A matrix is matched to the previous tick's by its call site and address
//    (closest one, the same object's first), by the object being drawn (the
//    caller's edi; for animated models the animation and the part), or by its
//    call site and order. It is blended only if it moved less than maxMove and
//    turned less than maxTurn; otherwise it is drawn as it is. Blends keep the
//    rotation rigid and rebuild the inverse from it. Matrices the game builds
//    to face the camera are not blended (they follow the blended view).
//  - The draw marks what it has drawn with the tick counter and skips anything
//    already marked (scene nodes, objects, mesh and light caches). Those reads
//    use a stamp of the mod's instead: the counter in ticks, a fresh value in
//    every in-between frame, so each frame draws the whole scene.
//  - Some of the race draw changes memory as it draws (the sky scrolls and
//    rescales its own vertices, the fog eases, a counter times a sound). That
//    memory is saved before and after the tick's draw; every in-between frame
//    starts from the first copy and leaves the second, so the game sees one
//    draw per tick.
//  - In-between frames are drawn only in races; menus, the pause screen and
//    the options keep 30 fps.
//
// widescreen=1: the Direct3D renderer runs at any resolution; see the
// Widescreen section.
// arcadeSound=1 with HTArcade.pak: the arcade's sounds, music and engines; see
// the Arcade sound section. trackDifficulty / aiDifficulty / arcadeSpeedway /
// arcadeLakePowell: see Game settings left from the arcade.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <d3dtypes.h>
#include <dsound.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>


#define HTFF_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct Config {
    int   interpolate = 1;
    int   fpsLimit = 0;          // 0 = refresh rate
    int   blendObjects = 1;      // 0 = only the camera is blended
    float maxMove = 100.0f;      // world units per tick; more = a jump (or another object), not blended
    float maxTurn = 50.0f;       // degrees per tick; more = not blended
    int   logToFile = 1;
    // widescreen (Direct3D renderer)
    int   widescreen = 1;
    int   wsWidth = 0;           // 0 = the desktop's resolution
    int   wsHeight = 0;
    int   wsFixFOV = 1;          // 1 = Hor+ 3D view; 0 = stretched
    int   wsHud = 1;             // 1 = 4:3 at the screen edges, 2 = 4:3 in the middle, 0 = stretched
    int   wsMenus = 1;           // 1 = 4:3 in the middle, 2 = stretched
    // sound
    int   arcadeSound = 1;       // 1 = sounds and music from HTArcade.pak when it is there
    // game
    int   trackDifficulty[13] = {};   // 1-100 (arcade operator setting); 0 = the game's (40)
    int   aiDifficulty[13] = {};      // 1-100; 0 = the game's (50)
    int   arcadeSpeedway = 0;    // 1 = the arcade's Hydro Speedway (left in the PC data, unused)
    int   arcadeLakePowell = 0;  // 1 = the arcade's Lake Powell
};
static Config g_cfg;
static char g_asiDir[MAX_PATH] = {};

static void InitDirs() {
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&InitDirs), &self) &&
        GetModuleFileNameA(self, g_asiDir, MAX_PATH)) {
        char* s = strrchr(g_asiDir, '\\'); if (s) s[1] = 0; else g_asiDir[0] = 0;
    }
}

static const char kIniText[] =
    "; HTFrameFix.ini (v" HTFF_VERSION ") - Hydro Thunder (PC)\n"
    "; the game runs at 30 ticks a second. 1 = in-between frames are drawn in races, so they run\n"
    ";   at the monitor's refresh rate; 0 = original (30 fps)\n"
    "interpolate=1\n"
    "; frame cap with interpolate=1: 0 = monitor refresh rate, or a number (30-1000)\n"
    "fpsLimit=0\n"
    "; 1 = objects move smoothly too; 0 = only the camera (objects move at 30 fps)\n"
    "blendObjects=1\n"
    "; an object that moves more than this between two ticks (world units) has jumped, and is\n"
    ";   drawn where it is instead of blended\n"
    "maxMove=100\n"
    "; an object that turns more than this between two ticks (degrees, under 90) is drawn as it is\n"
    "maxTurn=50\n"
    "; write HTFrameFix.log next to the ASI\n"
    "logToFile=1\n";

static const char kWsBlock[] =
    "; --- widescreen (Direct3D renderer) ---\n"
    "; 1 = the game runs at the resolution below instead of 640x480, at any aspect ratio;\n"
    ";   0 = the original 640x480\n"
    "widescreen=1\n"
    "; resolution; 0 = the desktop's\n"
    "wsWidth=0\n"
    "wsHeight=0\n"
    "; 1 = Hor+ 3D view (the 4:3 vertical view stays, you see more at the sides); 0 = stretched\n"
    "wsFixFOV=1\n"
    "; 1 = 4:3 HUD, its elements at the screen edges; 2 = 4:3 HUD in the middle; 0 = stretched\n"
    "wsHud=1\n"
    "; 1 = menus 4:3 in the middle; 2 = stretched\n"
    "wsMenus=1\n";

static const char kArcadeBlock[] =
    "; --- arcade sound ---\n"
    "; 1 = the arcade version's sounds and music (31250 Hz, from HTArcade.pak next to the ASI,\n"
    ";   built by HTArcadePak from the arcade hard disk) replace the PC's 11 kHz ones; 0 = PC sounds\n"
    "arcadeSound=1\n";

static const char kGameBlock[] =
    "; --- arcade settings and tracks ---\n"
    "; the arcade's operator difficulty settings, which the PC version keeps but has no menu for.\n"
    "; trackDifficulty: how much time the checkpoints give, 1 = most time, 100 = least; the game\n"
    ";   uses 40. aiDifficulty: how hard the other boats race, 1-100; the game uses 50.\n"
    "; one number for every track, or 13 separated by commas, in this order: Ship Graveyard, Lost\n"
    ";   Island, Venice Canals, Lake Powell, Arctic Circle, Nile Adventure, New York Disaster,\n"
    ";   Greek Isles, Far East, Catacomb, Thunder Park, Hydro Speedway, Castle Von Dandy.\n"
    "; 0 = the game's\n"
    "trackDifficulty=0\n"
    "aiDifficulty=0\n"
    "; 1 = race the arcade version of Hydro Speedway / Lake Powell instead of the PC's reworked one\n"
    ";   (the PC data still has them, unused)\n"
    "arcadeSpeedway=0\n"
    "arcadeLakePowell=0\n";

static void ParseList(const char* v, int* out, int n) {           // "40" or "40,45,..."
    int i = 0, last = 0;
    while (*v && i < n) {
        last = atoi(v); out[i++] = last;
        const char* c = strchr(v, ','); if (!c) break;
        v = c + 1;
    }
    if (i == 1) for (; i < n; i++) out[i] = last;
}

static void LoadConfig() {
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTFrameFix.ini", g_asiDir);
    FILE* f = fopen(path, "r");
    if (!f) {
        if ((f = fopen(path, "w"))) {
            fputs(kIniText, f); fputs("\n", f); fputs(kWsBlock, f);
            fputs("\n", f); fputs(kArcadeBlock, f);
            fputs("\n", f); fputs(kGameBlock, f); fclose(f);
        }
        return;
    }
    bool sawWs = false, sawArcade = false, sawGame = false;
    char line[MAX_PATH + 64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = line; while (*key == ' ' || *key == '\t') key++;
        char* ke = eq; while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        char* val = eq + 1; while (*val == ' ' || *val == '\t') val++;
        if (char* c = strchr(val, ';')) *c = 0;
        char* ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\r' || ve[-1] == '\n' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        int n = atoi(val);
        if      (!_stricmp(key, "interpolate"))  g_cfg.interpolate = n;
        else if (!_stricmp(key, "fpsLimit"))     g_cfg.fpsLimit = n;
        else if (!_stricmp(key, "blendObjects")) g_cfg.blendObjects = n;
        else if (!_stricmp(key, "maxMove"))      { float v = (float)atof(val); if (v > 0.0f) g_cfg.maxMove = v; }
        else if (!_stricmp(key, "maxTurn"))      { float v = (float)atof(val); if (v > 0.0f && v < 90.0f) g_cfg.maxTurn = v; }
        else if (!_stricmp(key, "logToFile"))    g_cfg.logToFile = n;
        else if (!_stricmp(key, "widescreen"))   { g_cfg.widescreen = n; sawWs = true; }
        else if (!_stricmp(key, "wsWidth"))      g_cfg.wsWidth = n;
        else if (!_stricmp(key, "wsHeight"))     g_cfg.wsHeight = n;
        else if (!_stricmp(key, "wsFixFOV"))     g_cfg.wsFixFOV = n;
        else if (!_stricmp(key, "wsHud"))        g_cfg.wsHud = n;
        else if (!_stricmp(key, "wsMenus"))      g_cfg.wsMenus = n;
        else if (!_stricmp(key, "arcadeSound"))  { g_cfg.arcadeSound = n; sawArcade = true; }
        else if (!_stricmp(key, "trackDifficulty")) { ParseList(val, g_cfg.trackDifficulty, 13); sawGame = true; }
        else if (!_stricmp(key, "aiDifficulty")) ParseList(val, g_cfg.aiDifficulty, 13);
        else if (!_stricmp(key, "arcadeSpeedway")) g_cfg.arcadeSpeedway = n;
        else if (!_stricmp(key, "arcadeLakePowell")) g_cfg.arcadeLakePowell = n;
    }
    fclose(f);
    if (!sawWs || !sawArcade || !sawGame) {                       // an older ini: add the new settings
        if (FILE* a = fopen(path, "a")) {
            if (!sawWs) { fputs("\n", a); fputs(kWsBlock, a); }
            if (!sawArcade) { fputs("\n", a); fputs(kArcadeBlock, a); }
            if (!sawGame) { fputs("\n", a); fputs(kGameBlock, a); }
            fclose(a);
        }
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void Logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------
static bool PatchBytes(DWORD at, const void* bytes, size_t len) {
    void* p = reinterpret_cast<void*>(at); DWORD old;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, len);
    DWORD tmp; VirtualProtect(p, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}
static bool PatchCall(DWORD at, const void* target, int len) {     // E8 rel32, rest NOP
    BYTE b[8] = { 0xE8 };
    DWORD rel = reinterpret_cast<DWORD>(target) - (at + 5); memcpy(b + 1, &rel, 4);
    for (int i = 5; i < len; i++) b[i] = 0x90;
    return PatchBytes(at, b, len);
}
static bool PatchJmp(DWORD at, const void* target, int len) {      // E9 rel32, rest NOP
    BYTE b[8] = { 0xE9 };
    DWORD rel = reinterpret_cast<DWORD>(target) - (at + 5); memcpy(b + 1, &rel, 4);
    for (int i = 5; i < len; i++) b[i] = 0x90;
    return PatchBytes(at, b, len);
}
static bool SigsMatch(const ht::Sig* sigs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const ht::Sig& s = sigs[i];
        if (IsBadReadPtr(reinterpret_cast<void*>(s.addr), s.len) ||
            memcmp(reinterpret_cast<void*>(s.addr), s.bytes, s.len) != 0) {
            Logf("exe check: bytes at 0x%06lX differ from the analysed Hydro.exe\n", s.addr);
            return false;
        }
    }
    return true;
}
static bool CheckSigs() {
    if (!SigsMatch(ht::kSigs, sizeof(ht::kSigs) / sizeof(ht::kSigs[0])) ||
        !SigsMatch(ht::kStampSites, sizeof(ht::kStampSites) / sizeof(ht::kStampSites[0]))) {
        Logf("exe check: not the analysed Hydro.exe - mod off\n");
        return false;
    }
    Logf("exe check: Hydro.exe matches\n");
    return true;
}

template <typename T> static inline T& G(DWORD a) { return *reinterpret_cast<T*>(a); }

// ---------------------------------------------------------------------------
// Game functions
// ---------------------------------------------------------------------------
typedef void (*Fn0)();
typedef void (__attribute__((fastcall)) *FnC)(DWORD);
static inline void CallFn0(DWORD a) { reinterpret_cast<Fn0>(a)(); }
static inline void CallFnC(DWORD a, DWORD ecx) { reinterpret_cast<FnC>(a)(ecx); }

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The game sets the FPU to 24-bit precision (0x476900), so times stay integers
// and only small differences become floats.
static LONGLONG g_qpf = 0, g_tickPeriod = 0, g_framePeriod = 0;
static inline LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void WaitUntil(LONGLONG t) {
    LONGLONG now = Qpc();
    while (t - now > 0) {
        if (t - now > g_qpf / 500) Sleep(1);                        // more than 2 ms left
        else YieldProcessor();
        now = Qpc();
    }
}

static int g_refreshHz = 0;
static DWORD g_refreshCheckMs = 0;
static void UpdateFramePeriod() {
    DWORD ms = GetTickCount();
    if (g_framePeriod && ms - g_refreshCheckMs < 1000) return;      // the mode can change: look again every second
    g_refreshCheckMs = ms;
    int fps = g_cfg.fpsLimit;
    if (fps <= 0) {
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        fps = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
        if (fps <= 1) fps = 60;
    }
    if (fps < 30) fps = 30;
    if (fps > 1000) fps = 1000;
    if (fps != g_refreshHz) {
        g_refreshHz = fps;
        Logf("frame rate: %d fps%s\n", fps, g_cfg.fpsLimit > 0 ? " (fpsLimit)" : " (refresh rate)");
    }
    g_framePeriod = g_qpf / fps;
}

// ---------------------------------------------------------------------------
// Matrix history
// ---------------------------------------------------------------------------
// A matrix as the game stores it: 26 floats, then the type (int).
struct Mat { float f[26]; int type; };
static_assert(sizeof(Mat) == 0x6C, "matrix size");

static const unsigned long long kViewTag = 0xC0DE000000000000ull;

struct Ent {                 // one matrix seen in a tick's draw
    unsigned long long base; // call site << 32 | matrix address (views: kViewTag | camera)
    int  ord;                // its order among pushes with the same base in that draw
    int  next;               // next entry with the same base, -1 = none
    DWORD obj;               // the object being drawn (the caller's edi, see EdiKey)
    Mat  m;
};
struct BaseSlot { unsigned long long base; DWORD stamp; int head; int tail; int count; };
struct OrdSlot  { unsigned long long base; DWORD stamp; int ord; int idx; };

const int kMaxEnt = 16384;
const int kBaseSlots = 32768;           // powers of two
const int kOrdSlots = 32768;
struct TickTable {
    DWORD    stamp;                     // generation (0 = empty); slots with another stamp are free
    DWORD    tick;                      // the tick it holds
    int      n;
    Ent      ent[kMaxEnt];
    BaseSlot bases[kBaseSlots];
    OrdSlot  ords[kOrdSlots];
};
static TickTable g_tab[2];
static int   g_cur = 0;                 // table of the current tick
static bool  g_prevValid = false;       // g_tab[g_cur ^ 1] holds the tick before
static DWORD g_curTick = 0xFFFFFFFF;
static DWORD g_gen = 0;                 // table generations: only ever grow (the tick counter restarts each race)

// draw-local ordinals
struct CntSlot { unsigned long long base; DWORD draw; int count; };
const int kCntSlots = 16384;
static CntSlot g_cnt[kCntSlots];

static inline DWORD Hash64(unsigned long long k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull; k ^= k >> 33;
    return (DWORD)k;
}

// draw state
extern "C" { DWORD g_drawStamp = 0; }    // read by the game's "drawn this frame?" checks (kStampSites)
static DWORD g_interpSeq = 0;
static bool  g_inDraw = false;
static DWORD g_drawId = 1;              // never 0 (empty slots)
static DWORD g_recDraw = 0;             // the draw that records the current tick
static float g_alpha = 1.0f;            // 0 = previous tick, 1 = current
static bool  g_blend = false;           // this draw blends (races only)
static bool  g_blendObjects = true;     // objects blended too, not just the camera (blendObjects)
static bool  g_tableFull = false;       // logged once
static Mat   g_viewExact;
static bool  g_viewSaved = false;
static bool  g_inDisplay = false;       // drawing an in-between (or tick's shown) frame

static void NewTick(DWORD t) {
    bool follows = (g_curTick != 0xFFFFFFFF && t == g_curTick + 1);
    g_cur ^= 1;
    const TickTable& P = g_tab[g_cur ^ 1];                           // the tick that just ended
    g_prevValid = follows && P.stamp != 0 && P.tick == g_curTick;
    TickTable& T = g_tab[g_cur];
    if (++g_gen == 0) g_gen = 1;
    T.stamp = g_gen;
    T.tick = t;
    T.n = 0;
    g_curTick = t;
    g_recDraw = g_drawId;
}

static int NextOrdinal(unsigned long long base) {
    DWORD h = Hash64(base);
    for (int i = 0; i < 64; i++) {
        CntSlot& s = g_cnt[(h + i) & (kCntSlots - 1)];
        if (s.draw != g_drawId) { s.base = base; s.draw = g_drawId; s.count = 1; return 0; }
        if (s.base == base) return s.count++;
    }
    return -1;                                                     // table crowded: not tracked
}

static void TableFull() {
    if (g_tableFull) return;
    g_tableFull = true;
    Logf("matrix table full: some matrices are drawn without blending\n");
}

static void Record(unsigned long long base, int ord, const Mat* m, DWORD obj) {
    TickTable& T = g_tab[g_cur];
    if (T.n >= kMaxEnt) { TableFull(); return; }
    DWORD stamp = T.stamp;
    // base list
    DWORD h = Hash64(base);
    BaseSlot* bs = nullptr;
    for (int i = 0; i < 64; i++) {
        BaseSlot& s = T.bases[(h + i) & (kBaseSlots - 1)];
        if (s.stamp != stamp) { s.base = base; s.stamp = stamp; s.head = -1; s.tail = -1; s.count = 0; bs = &s; break; }
        if (s.base == base) { bs = &s; break; }
    }
    if (!bs) { TableFull(); return; }
    // (base, ord) index
    DWORD h2 = Hash64(base ^ (0x9E3779B97F4A7C15ull * (unsigned)(ord + 1)));
    OrdSlot* os = nullptr;
    for (int i = 0; i < 64; i++) {
        OrdSlot& s = T.ords[(h2 + i) & (kOrdSlots - 1)];
        if (s.stamp != stamp) { os = &s; break; }
        if (s.base == base && s.ord == ord) { os = &s; break; }    // (can't happen: ordinals are unique)
    }
    if (!os) { TableFull(); return; }
    int idx = T.n++;
    Ent& e = T.ent[idx];
    e.base = base; e.ord = ord; e.next = -1; e.obj = obj; e.m = *m;
    if (bs->tail >= 0) T.ent[bs->tail].next = idx; else bs->head = idx;
    bs->tail = idx; bs->count++;
    os->base = base; os->stamp = stamp; os->ord = ord; os->idx = idx;
}

// Position used to tell a jump: the translation (objects), the camera's
// position (views: the inverse's translation).
static inline const float* PosOf(const Mat& m, bool view) { return view ? &m.f[21] : &m.f[9]; }

static inline float Dist2(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static float g_cos2Turn = 0.75f;        // cos^2(maxTurn)
const float kCos2Keyframe = 0.0076f;    // cos^2(85 degrees)

static inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Close enough to blend: the same type, moved at most maxMove, and each axis
// turned less than maxTurn (g_cos2Turn).
static bool Blendable(const Mat& a, const Mat& b, bool view) {
    if (a.type != b.type) return false;
    float d2 = Dist2(PosOf(a, view), PosOf(b, view));
    if (!(d2 <= g_cfg.maxMove * g_cfg.maxMove)) return false;     // (NaN: not blendable)
    for (int c = 0; c < 3; c++) {
        const float* u = &a.f[c * 3]; const float* v = &b.f[c * 3];
        float uv = Dot3(u, v), uu = Dot3(u, u), vv = Dot3(v, v);
        if (uu < 1e-12f || vv < 1e-12f) continue;                  // (degenerate: no direction to compare)
        if (!(uv > 0.0f && uv * uv >= g_cos2Turn * uu * vv)) return false;
    }
    return true;
}

// How different two blendable matrices are: squared distance plus turning
// (1 - cos per axis, weighted as 100 units^2 each) plus size (the difference in
// each axis's length, squared, weighted the same: the objects' scale matrices
// at 0x460686 all sit at 0,0,0 unturned and differ only in size).
static float Difference(const Mat& a, const Mat& b, bool view) {
    float d = Dist2(PosOf(a, view), PosOf(b, view));
    for (int c = 0; c < 3; c++) {
        const float* u = &a.f[c * 3]; const float* v = &b.f[c * 3];
        float uu = Dot3(u, u), vv = Dot3(v, v);
        if (uu < 1e-12f || vv < 1e-12f) continue;
        float lu = sqrtf(uu), lv = sqrtf(vv);
        d += 100.0f * (1.0f - Dot3(u, v) / (lu * lv)) + 100.0f * (lu - lv) * (lu - lv);
    }
    return d;
}

// The previous tick's matrix for this one: the closest blendable matrix the
// same base (call site + matrix) had then; the same object's first when one is
// given (a keyframe table shared by several balloons is the same matrix for
// all of them: another balloon's copy would match and nothing would move),
// any object's otherwise (the object value isn't always the same per object).
// The same order among the base's matrices only breaks ties: a shared matrix
// (one global reused for many objects) shifts order whenever an object ahead
// of it is culled.
static const Mat* FindPrev(unsigned long long base, int ord, const Mat& cur, bool view,
                           bool sameObj = false, DWORD obj = 0) {
    if (!g_prevValid) return nullptr;
    const TickTable& P = g_tab[g_cur ^ 1];
    const DWORD stamp = P.stamp;
    DWORD h = Hash64(base);
    const BaseSlot* bs = nullptr;
    for (int i = 0; i < 64; i++) {
        const BaseSlot& s = P.bases[(h + i) & (kBaseSlots - 1)];
        if (s.stamp != stamp) break;
        if (s.base == base) { bs = &s; break; }
    }
    if (!bs) return nullptr;
    const Mat* best = nullptr; float bestD = 0.0f; bool bestMine = false;
    int scanned = 0;
    for (int i = bs->head; i >= 0 && scanned < 256; i = P.ent[i].next, scanned++) {
        const Ent& e = P.ent[i];
        if (!Blendable(e.m, cur, view)) continue;
        bool mine = sameObj && e.obj == obj;
        if (best && bestMine && !mine) continue;                   // the object's own match wins
        float d = Difference(e.m, cur, view);
        bool same = (e.ord == ord);
        if (!best || (mine && !bestMine) || d < bestD - 1e-3f || (same && d <= bestD + 1e-3f)) {
            best = &e.m; bestD = d; bestMine = mine;
        }
    }
    return best;
}

// Blend two matrices: each number in between, then the rotation (the three
// columns at +0, +0xC, +0x18) made square and upright again, each column
// keeping its blended length, so a turning object isn't squashed or sheared.
static inline void Blend(Mat& out, const Mat& a, const Mat& b, float t) {
    for (int i = 0; i < 26; i++) out.f[i] = a.f[i] + (b.f[i] - a.f[i]) * t;
    out.type = b.type;
    float* c0 = &out.f[0]; float* c1 = &out.f[3]; float* c2 = &out.f[6];
    float l0 = sqrtf(Dot3(c0, c0)), l1 = sqrtf(Dot3(c1, c1)), l2 = sqrtf(Dot3(c2, c2));
    if (!(l0 > 1e-6f && l1 > 1e-6f && l2 > 1e-6f)) return;        // degenerate (or NaN): leave it
    float u0[3], u1[3], u2[3];
    for (int i = 0; i < 3; i++) u0[i] = c0[i] / l0;
    float d = Dot3(c1, u0) / l1;
    for (int i = 0; i < 3; i++) u1[i] = c1[i] / l1 - d * u0[i];
    float n1 = sqrtf(Dot3(u1, u1));
    if (!(n1 > 1e-6f)) return;
    for (int i = 0; i < 3; i++) u1[i] /= n1;
    u2[0] = u0[1] * u1[2] - u0[2] * u1[1];                         // the third: square to both,
    u2[1] = u0[2] * u1[0] - u0[0] * u1[2];                         //   on the side the blend had it
    u2[2] = u0[0] * u1[1] - u0[1] * u1[0];
    float side = Dot3(u2, c2) < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) {
        c0[i] = u0[i] * l0;
        c1[i] = u1[i] * l1;
        c2[i] = u2[i] * l2 * side;
    }
    // The inverse (+0x30 rotation, +0x54 translation) rebuilt from the result:
    // averaged on its own it shrinks and drifts off the forward one, and the
    // game takes the camera's place in the object from it (parts turned to the
    // camera). Square columns: inverse = transpose, each row / its length^2.
    float il[3] = { 1.0f / (l0 * l0), 1.0f / (l1 * l1), 1.0f / (l2 * l2) };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++)
            out.f[12 + k * 3 + i] = out.f[i * 3 + k] * il[i];
    for (int i = 0; i < 3; i++)                                    // -inverse * translation
        out.f[21 + i] = -(out.f[12 + i] * out.f[9] + out.f[15 + i] * out.f[10] + out.f[18 + i] * out.f[11]);
}

// Matrices built from the top of the stack (0x4696D0 / 0x469760 / 0x4697F0:
// turned to face the camera). The last ones built in this draw, with what
// they held, so a later use of the same, unchanged matrix is recognised.
struct FaceMat { const Mat* p; DWORD draw; Mat m; };
static FaceMat g_face[32];
static unsigned g_faceNext = 0;
extern "C" void NoteFaceBuilt(const Mat* m) {
    if (!g_inDraw) return;
    FaceMat& f = g_face[g_faceNext++ & 31];
    f.p = m; f.draw = g_drawId; f.m = *m;
}
static bool IsFaceBuilt(const Mat* m) {
    if ((DWORD)m == ht::faceGlobal[0] || (DWORD)m == ht::faceGlobal[1]) return true;
    for (const FaceMat& f : g_face)
        if (f.p == m && f.draw == g_drawId && !memcmp(&f.m, m, sizeof(Mat))) return true;
    return false;
}

// The previous tick's matrix with exactly this (base, order), if blendable;
// no nearest-match guessing (parts of different objects look alike).
extern "C" { DWORD g_callerEdi = 0; }   // edi of the code multiplying a matrix (set by the stubs)
static const Mat* FindPrevSameOrd(unsigned long long base, int ord, const Mat& cur) {
    if (!g_prevValid || ord < 0) return nullptr;
    const TickTable& P = g_tab[g_cur ^ 1];
    DWORD h2 = Hash64(base ^ (0x9E3779B97F4A7C15ull * (unsigned)(ord + 1)));
    for (int i = 0; i < 64; i++) {
        const OrdSlot& s = P.ords[(h2 + i) & (kOrdSlots - 1)];
        if (s.stamp != P.stamp) break;
        if (s.base == base && s.ord == ord) {
            const Mat& m = P.ent[s.idx].m;
            return Blendable(m, cur, false) ? &m : nullptr;
        }
    }
    return nullptr;
}

// Matrices built on the game's stack are keyed by their place below the start
// of the draw, not their address: the hidden tick draw and the frames shown are
// called from different depths.
static DWORD g_drawBase = 0;            // an address in RunDraw's frame (fixed distance to the draw)
static DWORD g_stackLo = 0, g_stackHi = 0;
static inline DWORD KeyOf(const Mat* m) {
    DWORD a = (DWORD)m;
    if (a >= g_stackLo && a < g_stackHi) return 0x80000000u | ((g_drawBase - a) & 0x7FFFFFFFu);
    return a;                                                      // a global or an object: its address
}
// The caller's edi, the same way: when it points into the stack, its place
// below the draw (the hidden tick draw and the frames shown run deeper or not).
static inline DWORD EdiKey(DWORD edi) {
    if (edi >= g_stackLo && edi < g_stackHi) return 0x80000000u | ((g_drawBase - edi) & 0x7FFFFFFFu);
    return edi;
}

// One matrix used to build the stack: recorded in the tick's draw, blended in
// the frames shown. slot picks the scratch copy (two can be in use at once).
static Mat g_scratch2[2];

// The objects drawn by 0x45BD80 (spray, splashes) share one matrix and face
// the camera: matched by the object itself, and only their position blended.
extern "C" { DWORD g_spriteObj = 0; }   // the object 0x45BD80 is drawing (set by SpriteStub)
static const Mat* FilterSprite(const Mat* m, DWORD ret, int slot) {
    unsigned long long base = ((unsigned long long)(ret | 0x20000000u) << 32) | g_spriteObj;
    int ord = NextOrdinal(base);
    if (g_recDraw == g_drawId && ord >= 0) Record(base, ord, m, 0);
    if (!g_blend || !g_blendObjects) return m;
    const Mat* p = FindPrev(base, ord, *m, false);
    if (!p) return m;
    Mat& out = g_scratch2[slot];
    out = *m;                                                      // turned to the camera as the game built it
    for (int i = 9; i < 12; i++) out.f[i] = p->f[i] + (m->f[i] - p->f[i]) * g_alpha;
    for (int i = 0; i < 3; i++)                                    // the inverse's translation: -inverse * position
        out.f[21 + i] = -(out.f[12 + i] * out.f[9] + out.f[15 + i] * out.f[10] + out.f[18 + i] * out.f[11]);
    return &out;
}

static const Mat* Filter(const Mat* m, DWORD ret, int slot) {
    if ((DWORD)m >= ht::matStack && (DWORD)m < ht::matStackEnd) return m;   // built from blends already
    DWORD t = G<DWORD>(ht::tickCount);
    if (t != g_curTick) NewTick(t);
    if (ret == ht::spritePush) return FilterSprite(m, ret, slot);
    unsigned long long base = ((unsigned long long)ret << 32) | KeyOf(m);
    int ord = NextOrdinal(base);
    // also by the object being drawn (the caller's edi, e.g. the animation
    // state in 0x408B10) and by call site alone: animated models pick a
    // keyframe matrix from a table by frame number, a new address every tick
    DWORD who = EdiKey(g_callerEdi);
    bool keyframe = false;
    for (DWORD k : ht::keyframePush) if (ret == k) keyframe = true;
    unsigned long long obj = ((unsigned long long)(ret ^ 0x10000000u) << 32) | who;
    // 0x408B10: edi = the object's animation state, [edi+8] = the part being
    // drawn. Its keyframe is matched by animation and part, not by its order
    // (the order shifts when a pass of the object is drawn in one draw only)
    if (keyframe && g_callerEdi)
        obj = ((unsigned long long)(*reinterpret_cast<const DWORD*>(g_callerEdi + 8) | 0x80000000u) << 32) | who;
    int oord = NextOrdinal(obj);
    unsigned long long site = ((unsigned long long)(ret ^ 0x40000000u) << 32) | 0xFFFFFFFFu;
    int sord = NextOrdinal(site);
    bool face = IsFaceBuilt(m);
    if (g_recDraw == g_drawId && !face) {
        if (ord >= 0) Record(base, ord, m, who);
        if (oord >= 0) Record(obj, oord, m, who);
        if (sord >= 0) Record(site, sord, m, who);
    }
    if (!g_blend || !g_blendObjects) return m;
    if (face) return m;                                            // follows the blended view already
    // a keyframe's address is another frame of the animation every tick: by
    // address only another object showing that frame could match (no motion)
    const Mat* p = keyframe ? nullptr : FindPrev(base, ord, *m, false, true, who);   // this object's first
    if (!p) {
        if (keyframe) {
            // the same part of the same animation: several objects can share
            // one animation (each at its own frame), told apart only by order,
            // and the order shifts when one is culled in one draw only (the
            // frames shown use the blended camera). So the closest keyframe it
            // had, not the one in the same place in the order; up to 85
            // degrees (spinning pickups turn more than maxTurn)
            float keep = g_cos2Turn;
            if (g_cos2Turn > kCos2Keyframe) g_cos2Turn = kCos2Keyframe;
            p = FindPrev(obj, oord, *m, false);
            g_cos2Turn = keep;
        } else {
            p = FindPrevSameOrd(obj, oord, *m);                     // same object, same order
        }
    }
    if (!p) p = FindPrevSameOrd(site, sord, *m);                   // same call site, same order
    if (!p) return m;
    Blend(g_scratch2[slot], *p, *m, g_alpha);
    return &g_scratch2[slot];
}

// 0x469500 (ecx = matrix pushed; ret = its caller)
extern "C" const Mat* PushFilter(const Mat* m, DWORD ret) {
    if (!g_inDraw) return m;
    return Filter(m, ret, 0);
}

// 0x4695B0: result (ecx) = parent (edx) * matrix ([esp+4]). Called by the
// push 0x4694C0 (parent = top of the stack, already built) and directly by the
// models' code (parent = any matrix, often an object's: blended too).
// frame: [0] parent, [1] result, [2] return address, [3] matrix, [4] the
// push's caller when called from the push. Pointers may be swapped for blends.
static const Mat* g_faceResult = nullptr;   // this multiply's result faces the camera
extern "C" void MulPre(DWORD* frame) {
    g_faceResult = nullptr;
    if (!g_inDraw) return;
    DWORD ret = frame[2];
    bool viaPush = (ret == ht::mulFromPush);
    if (viaPush) ret = frame[4];
    const Mat* m = reinterpret_cast<const Mat*>(frame[3]);
    const Mat* parent = reinterpret_cast<const Mat*>(frame[0]);
    if (!viaPush && (IsFaceBuilt(m) || IsFaceBuilt(parent)))
        g_faceResult = reinterpret_cast<const Mat*>(frame[1]);   // built from a camera-facing matrix
    frame[3] = (DWORD)Filter(m, ret, 0);
    if (!viaPush) frame[0] = (DWORD)Filter(parent, ret | 0x80000000u, 1);
}
extern "C" void MulPost(const Mat* result) {
    if (g_faceResult && g_faceResult == result) NoteFaceBuilt(result);
    g_faceResult = nullptr;
}

// Called in 0x469470 once the view base is built from the camera.
extern "C" void ViewFilter(DWORD cam) {
    if (!g_inDraw) return;
    Mat& v = G<Mat>(ht::viewBase);
    DWORD t = G<DWORD>(ht::tickCount);
    if (t != g_curTick) NewTick(t);
    unsigned long long base = kViewTag | cam;
    int ord = NextOrdinal(base);
    if (g_recDraw == g_drawId && ord >= 0) Record(base, ord, &v, 0);
    g_viewExact = v; g_viewSaved = true;
    if (!g_blend) return;
    const Mat* p = FindPrev(base, ord, v, true);
    if (!p) return;
    Mat out; Blend(out, *p, v, g_alpha);
    v = out;
}

// ---------------------------------------------------------------------------
// Hooks (asm stubs)
// ---------------------------------------------------------------------------
// 0x4694C0 / 0x469500 start with "mov edx, [0x14507E8] / mov eax, [0x14507E0]"
// (11 bytes) and use ecx only as the matrix; the stub swaps it for the blend.
extern "C" void MulStub();
extern "C" void PushStubB();
extern "C" void FaceStub0();
extern "C" void SpriteStub();
extern "C" void FaceStub1();
extern "C" void FaceStub2();
extern "C" void ViewStub();
extern "C" void DrawStub();
asm(
    ".text\n"
    // 0x4695B0: "mov eax, [esp+4] / push esi" (5 bytes); ecx = result, edx = parent,
    // [esp+4] = the matrix, ret 4. MulPre may swap parent and matrix for blends;
    // the multiply then runs (trampoline) and MulPost looks at its result.
    ".globl _MulStub\n"
    "_MulStub:\n"
    "    movl %edi, _g_callerEdi\n"       // (the push 0x4694C0 doesn't touch edi)
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    movl %esp, %eax\n"               // [eax] = parent, result, ret, matrix, (push's caller)
    "    pushl %eax\n"
    "    call _MulPre\n"
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    pushl %ecx\n"                    // keep the result pointer
    "    pushl 8(%esp)\n"                 // the matrix again, for the multiply
    "    call L_mulTramp\n"               // (pops it)
    "    pushl %eax\n"
    "    pushl 4(%esp)\n"
    "    call _MulPost\n"
    "    addl $4, %esp\n"
    "    popl %eax\n"
    "    addl $4, %esp\n"
    "    ret $4\n"
    "L_mulTramp:\n"
    "    movl 4(%esp), %eax\n"
    "    pushl %esi\n"
    "    pushl $0x4695B5\n"
    "    ret\n"
    // 0x45BD80: "sub esp, 0x38 / fld dword [ecx+0x14]" (6 bytes), ecx = the object
    ".globl _SpriteStub\n"
    "_SpriteStub:\n"
    "    movl %ecx, _g_spriteObj\n"
    "    subl $0x38, %esp\n"
    "    flds 0x14(%ecx)\n"
    "    pushl $0x45BD86\n"
    "    ret\n"
    // 0x4696D0 / 0x469760 / 0x4697F0: "sub esp, 0xC / push esi / mov esi, ecx" (6 bytes),
    // ecx = the matrix to build, plain ret. Run it, then note what it built.
    ".globl _FaceStub0\n"
    "_FaceStub0:\n"
    "    pushl %ecx\n"
    "    call L_faceTramp0\n"
    "    jmp L_faceNote\n"
    "L_faceTramp0:\n"
    "    subl $0xC, %esp\n"
    "    pushl %esi\n"
    "    movl %ecx, %esi\n"
    "    pushl $0x4696D6\n"
    "    ret\n"
    ".globl _FaceStub1\n"
    "_FaceStub1:\n"
    "    pushl %ecx\n"
    "    call L_faceTramp1\n"
    "    jmp L_faceNote\n"
    "L_faceTramp1:\n"
    "    subl $0xC, %esp\n"
    "    pushl %esi\n"
    "    movl %ecx, %esi\n"
    "    pushl $0x469766\n"
    "    ret\n"
    ".globl _FaceStub2\n"
    "_FaceStub2:\n"
    "    pushl %ecx\n"
    "    call L_faceTramp2\n"
    "    jmp L_faceNote\n"
    "L_faceTramp2:\n"
    "    subl $0xC, %esp\n"
    "    pushl %esi\n"
    "    movl %ecx, %esi\n"
    "    pushl $0x4697F6\n"
    "    ret\n"
    "L_faceNote:\n"                       // [esp] = the matrix, [esp+4] = the caller
    "    pushl %eax\n"
    "    pushl 4(%esp)\n"
    "    call _NoteFaceBuilt\n"
    "    addl $4, %esp\n"
    "    popl %eax\n"
    "    addl $4, %esp\n"
    "    ret\n"
    ".globl _PushStubB\n"
    "_PushStubB:\n"
    "    movl %edi, _g_callerEdi\n"
    "    pushl (%esp)\n"
    "    pushl %ecx\n"
    "    call _PushFilter\n"
    "    addl $8, %esp\n"
    "    movl %eax, %ecx\n"
    "    movl 0x14507E8, %edx\n"
    "    movl 0x14507E0, %eax\n"
    "    pushl $0x46950B\n"
    "    ret\n"
    // 0x46947F: "mov ecx, [0x14507E4]" (6 bytes); esi = camera, eax/ecx/edx free
    ".globl _ViewStub\n"
    "_ViewStub:\n"
    "    pushl %esi\n"
    "    call _ViewFilter\n"
    "    addl $4, %esp\n"
    "    movl 0x14507E4, %ecx\n"
    "    pushl $0x469485\n"
    "    ret\n"
    // 0x42F78A: "call [0x14549BC]" (6 bytes), ecx = [0x14549B8]
    ".globl _DrawStub\n"
    "_DrawStub:\n"
    "    pushl %ecx\n"
    "    call _DrawHook\n"
    "    addl $4, %esp\n"
    "    ret\n"
);

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
struct TickSnap { DWORD fn, arg, count, flt, delta, parity, one; bool valid; };
static TickSnap g_last = {};

// kDrawState, saved before the tick's draw (pre) and at the end of the tick
// (post); a frame shown starts from pre and leaves post. Each range is taken
// in whole 64-byte blocks (as tested).
constexpr DWORD BlockLo(const ht::Range& r) { return r.addr & ~63u; }
constexpr DWORD BlockHi(const ht::Range& r) { return (r.addr + r.len + 63u) & ~63u; }
constexpr DWORD StateBytes() {
    DWORD n = 0;
    for (const ht::Range& r : ht::kDrawState) n += BlockHi(r) - BlockLo(r);
    return n;
}
static BYTE g_statePre[StateBytes()], g_statePost[StateBytes()];
static bool g_preValid = false, g_postValid = false;
static void CopyState(BYTE* buf, bool save) {
    size_t at = 0;
    for (const ht::Range& r : ht::kDrawState) {
        BYTE* mem = reinterpret_cast<BYTE*>(BlockLo(r));
        DWORD len = BlockHi(r) - BlockLo(r);
        if (save) memcpy(buf + at, mem, len); else memcpy(mem, buf + at, len);
        at += len;
    }
}

static __attribute__((noinline)) void RunDraw(DWORD fn, DWORD arg, bool blend) {
    volatile DWORD here = 0;
    g_drawBase = (DWORD)&here;                                     // same distance to the draw from both callers
    g_inDraw = true;
    g_blend = blend;
    if (++g_drawId == 0) g_drawId = 1;
    g_viewSaved = false;
    CallFnC(fn, arg);
    g_inDraw = false;
    g_blend = false;
    if (g_viewSaved) G<Mat>(ht::viewBase) = g_viewExact;          // the tick never sees a blended view
}

// The sky scrolls with the camera's angles: the camera code (once a tick)
// calls 0x44D120 (ecx = yaw, edx = pitch, [esp+4] = roll), which keeps them at
// 0x558050 / 0x558054 / 0x558058 and rebuilds the sky's layers (0x44DB80, or
// 0x44DFE0 turned by the roll when it isn't 0).
// A frame shown in between calls it again with the angles blended between the
// last two ticks; all it changes is in kDrawState, put back after the frame.
// Yaw: 0x20000 a turn (0x44D15E scales it by 0.5 into 16 bits); roll: 0x10000
// a turn (0x44DFE0 looks it up in the sine table as it is).
struct SkyAngles { int yaw, pitch, roll; DWORD tick; bool valid; };
static SkyAngles g_skyCur = {}, g_skyPrev = {};
const DWORD kSkySet = 0x44D120;
static void NoteSkyAngles(DWORD tick) {
    SkyAngles a = { G<int>(0x558050), G<int>(0x558054), G<int>(0x558058), tick, true };
    g_skyPrev = (g_skyCur.valid && g_skyCur.tick + 1 == tick) ? g_skyCur : SkyAngles{};
    g_skyCur = a;
}
static inline int AngleDelta(int from, int to, int turn = 0x20000) {
    int d = (to - from) & (turn - 1);
    return d >= turn / 2 ? d - turn : d;
}
static void BlendSky(float alpha) {
    if (!g_skyPrev.valid || !g_skyCur.valid || g_skyCur.tick != g_last.count) return;
    if (!G<DWORD>(0x558010)) return;                               // (no sky)
    int dy = AngleDelta(g_skyPrev.yaw, g_skyCur.yaw), dp = AngleDelta(g_skyPrev.pitch, g_skyCur.pitch);
    int dr = AngleDelta(g_skyPrev.roll, g_skyCur.roll, 0x10000);
    if (!dy && !dp && !dr) return;
    if (dy > 0x4000 || dy < -0x4000 || dp > 0x4000 || dp < -0x4000 || dr > 0x2000 || dr < -0x2000) {
        return;                                                    // a camera cut: as it is
    }
    int yaw = g_skyPrev.yaw + (int)lroundf(dy * alpha), pitch = g_skyPrev.pitch + (int)lroundf(dp * alpha);
    int roll = (g_skyPrev.roll + (int)lroundf(dr * alpha)) & 0xFFFF;
    if (!g_skyCur.roll && !g_skyPrev.roll) roll = 0;
    reinterpret_cast<void (__attribute__((fastcall)) *)(int, int, int)>(kSkySet)(yaw, pitch, roll);
}

// The tick's own draw (from 0x42F700).
extern "C" void DrawHook(DWORD arg) {
    DWORD fn = G<DWORD>(ht::drawFn);
    g_last.fn = fn; g_last.arg = arg;
    g_last.count  = G<DWORD>(ht::tickCount);
    g_last.flt    = G<DWORD>(ht::tickFloat);
    g_last.delta  = G<DWORD>(ht::tickDelta);
    g_last.parity = G<DWORD>(ht::tickParity);
    g_last.one    = G<DWORD>(ht::tickOne);
    g_last.valid  = true;
    g_drawStamp = g_last.count;                                    // a tick draws with the counter itself
    if (fn == ht::drawRace) {
        NoteSkyAngles(g_last.count);
        CopyState(g_statePre, true);
        g_preValid = true;
    }
    g_postValid = false;                                           // set again at the end of a hidden tick
    RunDraw(fn, arg, false);                                       // exact: what it pushes is recorded
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
static bool g_active = false;
static LONGLONG g_nextTick = 0, g_nextFrame = 0;

static float AlphaAt(LONGLONG now) {
    LONGLONG since = now - (g_nextTick - g_tickPeriod);             // time since the last tick was due
    if (since <= 0) return 0.0f;
    if (since >= g_tickPeriod) return 1.0f;
    return (float)(long)since / (float)(long)g_tickPeriod;
}

static void PumpOnly() {
    MSG msg;
    for (int i = 0; i < 10 && PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE); i++) {
        if (msg.message == WM_QUIT) CallFn0(ht::fnQuit);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// Only after a tick drawn hidden, with the state before and after its draw
// saved: after a tick drawn as the original does (pause, options) the draw
// state can't be put back, and drawing again would apply the draw's changes
// twice (the sky's texture coordinates scaled again: a black sky on unpausing).
static bool InterpAllowed() {
    return g_last.valid && g_last.fn == ht::drawRace && !G<DWORD>(ht::screenUp) && g_postValid;
}

// A displayed race frame: the mode's draw again, blended, as the tick's draw
// saw the game (tick counter, kDrawState), with a stamp of its own.
static void DisplayFrame(float alpha, bool tickFrame) {
    G<DWORD>(ht::slowFrame) = 0;
    CallFn0(ht::fnBeginFrame);
    int& cc = G<int>(ht::clearCount);
    if (cc <= 0) { CallFnC(ht::fnClear, 0); cc = 0; }
    else if (tickFrame) cc--;                                      // counts the game's own frames
    DWORD sCount = G<DWORD>(ht::tickCount), sFlt = G<DWORD>(ht::tickFloat), sDelta = G<DWORD>(ht::tickDelta),
          sPar = G<DWORD>(ht::tickParity), sOne = G<DWORD>(ht::tickOne);
    G<DWORD>(ht::tickCount)  = g_last.count;
    G<DWORD>(ht::tickFloat)  = g_last.flt;
    G<DWORD>(ht::tickDelta)  = g_last.delta;
    G<DWORD>(ht::tickParity) = g_last.parity;
    G<DWORD>(ht::tickOne)    = g_last.one;
    // a stamp no tick uses (the counter never reaches bit 31): everything is drawn again
    g_drawStamp = 0x80000000u | (++g_interpSeq & 0x7FFFFFFFu);
    CopyState(g_statePre, false);                                  // the state the tick's draw started from
    BlendSky(alpha);
    G<DWORD>(ht::warnArmed) = 1;                                   // 0x409390: count from 0, no sound
    G<DWORD>(ht::warnCount) = 0;
    g_alpha = alpha;
    g_inDisplay = true;
    RunDraw(g_last.fn, g_last.arg, true);
    g_inDisplay = false;
    CopyState(g_statePost, false);                                 // and the state the tick left
    g_drawStamp = g_last.count;
    if (G<DWORD>(ht::fadeCount)) CallFnC(ht::fnFade, G<DWORD>(ht::fadeArg));
    G<DWORD>(ht::tickCount)  = sCount;
    G<DWORD>(ht::tickFloat)  = sFlt;
    G<DWORD>(ht::tickDelta)  = sDelta;
    G<DWORD>(ht::tickParity) = sPar;
    G<DWORD>(ht::tickOne)    = sOne;
    CallFn0(ht::fnRender);
    CallFn0(ht::fnPresent);
}

static void DoTick(bool render, float alpha) {
    CallFn0(ht::fnPump);
    // In a race the tick's draw is hidden: drawn exact with the game's "skip
    // drawing" flag (polygons dropped, as in its own slow frames) so it only
    // records; the frame shown is drawn after it, blended. 0x42F700 takes the
    // draw function for the tick from [0x51E270].
    // A menu screen over the race (pause, options) is drawn outside the draw
    // function, so then the tick draws as the original does.
    bool hidden = G<DWORD>(ht::pendingDrawFn) == ht::drawRace && !G<DWORD>(ht::screenUp);
    G<DWORD>(ht::slowFrame) = (hidden || !render) ? 1 : 0;
    if (render && !hidden) {
        CallFn0(ht::fnBeginFrame);
        int& cc = G<int>(ht::clearCount);
        if (cc <= 0) { CallFnC(ht::fnClear, 0); cc = 0; } else cc--;
    }
    g_last.valid = false;                                          // set again if this tick draws
    g_drawStamp = G<DWORD>(ht::tickCount);
    CallFn0(ht::fnTick);
    if (hidden) {
        G<DWORD>(ht::slowFrame) = 0;
        if (g_last.valid && g_preValid) { CopyState(g_statePost, true); g_postValid = true; }   // end of tick
        if (render && InterpAllowed()) DisplayFrame(alpha, true);
    } else if (render && !G<DWORD>(ht::slowFrame)) {
        CallFn0(ht::fnRender);
        CallFn0(ht::fnPresent);
    }
    G<DWORD>(ht::lastFrameUs) = 33333;
}

extern "C" void Frame() {
    if (!g_active) { CallFn0(ht::fnFrame); return; }
    if (!g_nextTick) {                                             // first frame, on the game's thread
        timeBeginPeriod(1);                                        // Sleep(1) ~ 1 ms (here, not in DllMain)
        DWORD stackBase;
        asm volatile("movl %%fs:4, %0" : "=r"(stackBase));        // NT_TIB.StackBase
        g_stackHi = stackBase;                                     // matrices "on the stack": 4 MB below its top
        g_stackLo = g_stackHi > 0x400000 ? g_stackHi - 0x400000 : 0;
    }
    UpdateFramePeriod();
    LONGLONG now = Qpc();
    if (!g_nextTick || now - g_nextTick > 8 * g_tickPeriod)       // start, or after a load / a stall
        g_nextTick = now;
    int due = 0;
    while (now - g_nextTick >= 0 && due < 4) { g_nextTick += g_tickPeriod; due++; }
    if (due) {
        if (now - g_nextTick >= 0) g_nextTick = now;               // more than 4 behind: let time go
        float a = AlphaAt(now);
        for (int i = 0; i < due; i++) DoTick(i == due - 1, a);
        g_nextFrame = now + g_framePeriod;
        return;
    }
    bool interp = InterpAllowed();
    if (interp && now - g_nextFrame >= 0) {
        PumpOnly();
        DisplayFrame(AlphaAt(now), false);
        g_nextFrame = (now - g_nextFrame < g_framePeriod) ? g_nextFrame + g_framePeriod : now + g_framePeriod;
        return;
    }
    LONGLONG until = g_nextTick;
    if (interp && g_nextFrame - until < 0) until = g_nextFrame;
    WaitUntil(until);
}

// ---------------------------------------------------------------------------
// Widescreen (Direct3D renderer)
// ---------------------------------------------------------------------------
// The game lays everything out for a 640x480 screen and its Direct3D renderer
// sets that mode. widescreen runs the renderer at any resolution and remaps
// the 640x480 picture where every triangle reaches Direct3D: they are written
// as pretransformed vertices into one buffer (0x636520, slots handed out by
// 0x46D850) and drawn when it is flushed (0x46CE20). Each triangle is tagged
// when its slot is handed out with what is drawing it, and moved at the flush:
//  - 3D (the race draw 0x44C9C0, and the light glows 0x409760 the HUD draws):
//    scaled by height/480 around the middle. The projection is left as it is,
//    so the vertical view is the 4:3 one, and the culling it does against the
//    4:3 screen is widened after the projection setup (0x452530): the clip
//    rectangle the triangles are culled and clipped against, the frustum's side
//    planes (rebuilt from tan(h') = k tan(h), h = half the horizontal FOV) and
//    the half diagonal. The light glows' screen radius reads a wider half
//    width. That is Hor+.
//  - The sky (0x44D1E0, a backdrop on a 4:3 screen quad): stretched across
//    the width, the vertical as the 3D. Its quad can't be widened: it is cut
//    into 256-unit tiles, one sky layer each, with room for 8 (0x5579F0 -
//    0x557FB0); a wider quad needs more and overwrites the sky's texture table.
//  - HUD (0x4316E0, called by the race draw): 4:3 size, each triangle at the
//    left edge, the middle or the right edge by where it is on the 4:3 screen
//    (thirds). Text is queued as glyphs (0x450E40, one call a string) and drawn
//    later (0x451280): a string's glyphs share one anchor, from its extent.
//  - Menus (everything else): 4:3 in the middle, or stretched.
//  - The full-screen fade quad (0x47BAC0) covers the whole screen.
//  - The markers for opponents close behind (the scene draw places them
//    itself, centred: at the 4:3 edges, x = -/+316.25, or along the bottom)
//    are moved to the screen's edges and spread as wide.
// Menu images are copied straight into the locked back buffer (0x416FB0 via
// 0x46BA40): the lock hands out a 640x480 copy instead, made from the frame,
// and the unlock scales the pixels the game changed into the frame.
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

// What a triangle is (its tag), and how it is moved.
enum WsTag : BYTE { kT3D, kTHud, kTMenu, kTLeft, kTCenter, kTRight, kTStretch, kTSky };

static bool  g_wsOn = false;
static int   g_wsW = 640, g_wsH = 480;
static float g_wsS = 1.0f;                // 4:3 scale: min(W/640, H/480)
static float g_wsK = 1.0f;                // how much wider than 4:3: W / (640 s)
static float g_wsCx = 320.0f, g_wsCy = 240.0f;
static float g_wsSx = 1.0f, g_wsSy = 1.0f;   // stretched: W/640, H/480
extern "C" {
    float g_halfW = 320.5f;               // the half width the glows read
    float g_markerSpread = 245.0f;        // the markers along the bottom: their spread
    int   g_wsLayer = kTMenu;             // what is being drawn
    int   g_wsForce = -1;                 // a tag for the next triangles, -1 = none
    DWORD g_wsRetSave = 0;                // (stubs that call 0x47B7F0 in between)
}
static BYTE g_triTag[0x160];
static WORD g_glyphStr[ws::kGlyphMax];    // the string a queued glyph belongs to
static BYTE g_glyphTag[ws::kGlyphMax];    // its anchor
static WORD g_strSeq = 0, g_strStart = 0;
static int  g_strDepth = 0;               // inside 0x450E40 (it may draw its glyphs at once)

static inline bool IsD3D() { return reinterpret_cast<int (*)()>(ws::isD3D)() != 0; }

// The anchor of something spanning lo..hi on the 4:3 screen: by the third it's in.
static inline BYTE AnchorOf(float lo, float hi) {
    float c = (lo + hi) * 0.5f;
    return c < 640.0f / 3.0f ? kTLeft : c > 1280.0f / 3.0f ? kTRight : kTCenter;
}

static bool g_frame3D = false;            // this frame draws the race's 3D (full width)
static bool g_menuFull = false;           // this frame has a full-width menu picture

// 0x46D850 handed out triangle slot [count - 1].
extern "C" void WsTagTri() {
    int n = G<int>(ws::triCount);
    if (n < 1 || n > (int)sizeof(g_triTag)) return;
    BYTE tag = (BYTE)(g_wsForce >= 0 ? g_wsForce : g_wsLayer);
    g_triTag[n - 1] = tag;
    if (tag == kT3D || tag == kTSky) g_frame3D = true;
}

static void ClearBars() {
    if (g_cfg.wsMenus == 2 || !(g_wsK > 1.0001f)) return;
    void* dev = G<void*>(ws::device);
    if (!dev || !G<DWORD>(ws::deviceUp) || G<DWORD>(ws::lockState)) return;
    LONG x0 = (LONG)ceilf(g_wsCx - 320.0f * g_wsS - 0.5f), x1 = (LONG)ceilf(g_wsCx + 320.0f * g_wsS - 0.5f);
    if (x0 <= 0 && x1 >= g_wsW) return;
    D3DRECT r[2] = { { 0, 0, x0, g_wsH }, { x1, 0, g_wsW, g_wsH } };
    typedef HRESULT (__stdcall* ClearFn)(void*, DWORD, const D3DRECT*, DWORD, DWORD, float, DWORD);
    ClearFn clear = reinterpret_cast<ClearFn>((*reinterpret_cast<void***>(dev))[0x28 / 4]);
    clear(dev, 2, r, D3DCLEAR_TARGET, 0, 1.0f, 0);
}

// The frame is about to be shown (0x478040): a menu with a picture across the
// whole 4:3 screen (copied into the frame, or a quad) over a race drawn behind
// it (some menus draw a race scene, full width): at 640x480 the picture hid
// the race, so the bars beside it are cleared to black.
extern "C" void WsPresent() {
    if (g_menuFull) ClearBars();
    g_menuFull = false;
}

// A frame begins (0x478080, after BeginScene): menus are drawn 4:3 and never
// clear the screen (at 640x480 their pictures cover it), so the bars beside
// them would keep whatever was there (a race). Unless the last frame drew a
// race (full width), they are cleared to black.
extern "C" void WsFrameBegin() {
    bool race = g_frame3D;
    g_frame3D = false;
    if (!race) ClearBars();
}

// The triangle buffer is about to be drawn: move every vertex to the screen.
extern "C" void WsFlush() {
    int n = G<int>(ws::triCount);
    if (n > (int)sizeof(g_triTag)) n = sizeof(g_triTag);
    const float left = g_wsCx - 320.0f * g_wsS * g_wsK, right = g_wsCx + 320.0f * g_wsS * g_wsK;
    for (int i = 0; i < n; i++) {
        float* v = reinterpret_cast<float*>(ws::triBuf + i * 0x60);   // x, y at +0, +4; 8 floats a vertex
        int tag = g_triTag[i];
        bool stretch = false;
        switch (tag) {
        case kT3D:      stretch = !g_cfg.wsFixFOV; tag = kTCenter; break;
        case kTMenu:
            stretch = g_cfg.wsMenus == 2; tag = kTCenter;
            if (!g_menuFull) {                                     // a picture across the whole 4:3 screen?
                float lo = v[0], hi = v[0];
                for (int j = 1; j < 3; j++) { float x = v[j * 8]; if (x < lo) lo = x; if (x > hi) hi = x; }
                if (lo <= 1.0f && hi >= 639.0f) g_menuFull = true;
            }
            break;
        case kTStretch: stretch = true; break;
        case kTSky:                                                // stretched across, upright as the 3D
            if (!g_cfg.wsFixFOV) { stretch = true; break; }
            for (int j = 0; j < 3; j++) {
                v[j * 8] = g_wsCx + (v[j * 8] - 320.0f) * g_wsS * g_wsK;
                v[j * 8 + 1] = g_wsCy + (v[j * 8 + 1] - 240.0f) * g_wsS;
            }
            continue;
        default:                                                   // the HUD
            if (g_cfg.wsHud == 0) stretch = true;
            else if (g_cfg.wsHud == 2) tag = kTCenter;
            else if (tag == kTHud) {
                float lo = v[0], hi = v[0];
                for (int j = 1; j < 3; j++) { float x = v[j * 8]; if (x < lo) lo = x; if (x > hi) hi = x; }
                tag = AnchorOf(lo, hi);
            }
            break;
        }
        for (int j = 0; j < 3; j++) {
            float& x = v[j * 8]; float& y = v[j * 8 + 1];
            if (stretch) { x *= g_wsSx; y *= g_wsSy; continue; }
            if (tag == kTLeft)       x = left + x * g_wsS;
            else if (tag == kTRight) x = right - (640.0f - x) * g_wsS;
            else                     x = g_wsCx + (x - 320.0f) * g_wsS;
            y = g_wsCy + (y - 240.0f) * g_wsS;
        }
    }
}

// The markers for opponents behind: at the edges of the screen, and spread
// along the bottom, as wide as the screen. Set when the width changes.
static float g_markerK = 1.0f;
static void SetMarkers(float k) {
    if (k == g_markerK) return;
    g_markerK = k;
    g_markerSpread = 245.0f * k;
    float edge = 320.0f * k - 3.75f;                               // (316.25 at 4:3)
    for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? edge : -edge;
        PatchBytes(ws::kMarkerEdges[i] + 1, &x, 4);
    }
}

// End of the projection setup 0x452530: widen what it built for the 4:3 screen.
extern "C" void WsProjFix() {
    bool on = g_wsOn && g_cfg.wsFixFOV && g_wsK > 1.0001f && IsD3D();
    float k = on ? g_wsK : 1.0f;
    g_halfW = 320.5f * k;
    if (g_wsOn) SetMarkers(k);
    if (!on) return;
    float half = 320.0f * k;                                       // the screen's edges on the 4:3 screen
    G<float>(0x14509A0) = 320.0f - half;                           // clip rectangle: left, right
    G<float>(0x14509A4) = 320.0f + half;
    G<float>(0x14509B8) = 321.0f + half;
    G<float>(0x14509C4) = g_halfW;                                 // half width
    G<float>(0x14508E4) = g_halfW * g_halfW;
    float diag = sqrtf(g_halfW * g_halfW + 240.0f * 240.0f);       // half diagonal (pushed as 320.5^2 + 240^2)
    G<float>(0x14509C8) = diag;
    G<float>(0x14508E8) = diag * diag;
    float focal = G<float>(0x14508C8);
    if (focal > 1e-6f) G<float>(0x1450958) = diag / focal;
    G<float>(0x1450950) *= k;                                      // tan(h)
    // the side planes: normals (cos h, 0, sin h) and (-cos h, 0, sin h), in
    // two sets; the second set's vertical planes use the same angle
    float c = G<float>(0x1450880), s = G<float>(0x1450888);
    if (!(c > 1e-6f)) return;
    float t = s / c * k;
    float c2 = 1.0f / sqrtf(1.0f + t * t), s2 = t * c2;
    G<float>(0x1450880) = c2;  G<float>(0x1450888) = s2;
    G<float>(0x145088C) = -c2; G<float>(0x1450894) = s2;
    G<float>(0x1450900) = c2;  G<float>(0x1450908) = s2;
    G<float>(0x145090C) = -c2; G<float>(0x1450914) = s2;
    G<float>(0x145091C) = c2;  G<float>(0x1450920) = s2;
    G<float>(0x1450928) = -c2; G<float>(0x145092C) = s2;
}

// Text: 0x450E40 queues one string's glyphs at [0x559138] onward (or draws
// them at once through 0x451280); 0x451280 draws a run of glyphs.
extern "C" void WsStrBegin() {
    g_strDepth++;
    g_strStart = G<WORD>(ws::glyphCount);
    if (++g_strSeq == 0) g_strSeq = 1;
}
extern "C" void WsStrEnd() {
    int end = G<WORD>(ws::glyphCount);
    for (int i = g_strStart; i < end && i < ws::kGlyphMax; i++) g_glyphStr[i] = g_strSeq;
    if (g_strDepth > 0) g_strDepth--;
}
// Before a run is drawn: each string's anchor, from the extent of its glyphs
// (record: x +4, width +0xC, scale +0x18).
extern "C" void WsGlyphPrepass(DWORD recs, int n) {
    if (recs < ws::glyphs || (recs - ws::glyphs) % 0x2C) return;
    int i0 = (recs - ws::glyphs) / 0x2C;
    if (n <= 0 || i0 + n > ws::kGlyphMax) return;
    bool one = g_strDepth > 0;                                     // drawn by 0x450E40 itself: one string
    int i = i0;
    while (i < i0 + n) {
        int j = i;
        float lo = 1e9f, hi = -1e9f;
        while (j < i0 + n && (one || j == i || g_glyphStr[j] == g_glyphStr[i])) {
            const BYTE* r = reinterpret_cast<const BYTE*>(ws::glyphs + j * 0x2C);
            float x = *reinterpret_cast<const float*>(r + 4);
            float w = *reinterpret_cast<const float*>(r + 0xC) * *reinterpret_cast<const float*>(r + 0x18);
            if (x < lo) lo = x;
            if (x + w > hi) hi = x + w;
            j++;
        }
        BYTE a = (lo <= hi) ? AnchorOf(lo, hi) : (BYTE)kTCenter;
        for (int m = i; m < j; m++) g_glyphTag[m] = a;
        i = j;
    }
}
// A glyph's quad (esi = its record + 0x24): in the HUD, its string's anchor.
extern "C" void WsGlyphBegin(DWORD esi) {
    if (g_wsLayer != kTHud) return;
    DWORD r = esi - 0x24;
    if (r < ws::glyphs || (r - ws::glyphs) % 0x2C) return;
    int idx = (r - ws::glyphs) / 0x2C;
    if (idx < ws::kGlyphMax) g_wsForce = g_glyphTag[idx];
}
// The light glows and the sun flare (0x409760, drawn by the HUD) place
// themselves from the camera object [0x144FA50] (its matrix, and its position
// at +0x6C), not from the view on the matrix stack. In a blended frame the
// view is blended (ViewFilter) but the camera isn't, so they'd move at 30 fps:
// for their draw the camera gets the blended view, and its position is moved
// by as much as the view's. Widescreen: they are in the world (3D).
static BYTE  g_camSave[0x78];
static DWORD g_camSaved = 0;
static int   g_glowLayer = kTMenu;
extern "C" void GlowPre() {
    g_glowLayer = g_wsLayer;
    g_wsLayer = kT3D;
    g_camSaved = 0;
    if (!g_inDraw || !g_blend || !g_viewSaved) return;
    DWORD cam = G<DWORD>(0x144FA50);
    if (!cam || IsBadWritePtr(reinterpret_cast<void*>(cam), sizeof(g_camSave))) return;
    if (memcmp(reinterpret_cast<const void*>(cam), &g_viewExact, sizeof(Mat))) return;   // not the view that was blended
    const Mat& v = G<Mat>(ht::viewBase);
    memcpy(g_camSave, reinterpret_cast<const void*>(cam), sizeof(g_camSave));
    float* pos = reinterpret_cast<float*>(cam + 0x6C);
    for (int i = 0; i < 3; i++) pos[i] += v.f[21 + i] - g_viewExact.f[21 + i];   // the camera's place: the inverse's translation
    G<Mat>(cam) = v;
    g_camSaved = cam;
}
extern "C" void GlowPost() {
    if (g_camSaved) memcpy(reinterpret_cast<void*>(g_camSaved), g_camSave, sizeof(g_camSave));
    g_camSaved = 0;
    g_wsLayer = g_glowLayer;
}

extern "C" void WsFadeBegin() { if (g_wsLayer != kT3D) g_wsForce = kTStretch; }
extern "C" void WsForceEnd() { g_wsForce = -1; }

// Direct writes to the back buffer (menu images): a 640x480 copy.
static WORD g_shadow[640 * 480], g_snap[640 * 480];
static bool g_shadowLocked = false;
static int  g_colX[8192], g_rowY[8192];   // screen -> 640x480 copy (for the unlock)

static void MenuMap(float& ox, float& oy, float& sx, float& sy) {
    if (g_cfg.wsMenus == 2) { ox = 0.0f; oy = 0.0f; sx = g_wsSx; sy = g_wsSy; return; }
    ox = g_wsCx - 320.0f * g_wsS; oy = g_wsCy - 240.0f * g_wsS; sx = sy = g_wsS;
}

static IDirectDrawSurface7* LockBack(DDSURFACEDESC2& d, DWORD flags) {
    IDirectDrawSurface7* bb = G<IDirectDrawSurface7*>(ws::backBuffer);
    if (!bb) return nullptr;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (FAILED(bb->Lock(nullptr, &d, flags, nullptr))) return nullptr;
    if (d.ddpfPixelFormat.dwRGBBitCount != 16 || !d.lpSurface) { bb->Unlock(nullptr); return nullptr; }
    return bb;
}

// 0x46BA40 (fastcall: ecx = which, edx = &pointer; &pitch, &format on the stack)
extern "C" int __attribute__((fastcall)) WsLock(int which, void** pptr, int* ppitch, int* pfmt) {
    if (!G<DWORD>(ws::deviceUp) || !pptr || !ppitch || !pfmt || G<DWORD>(ws::lockState)) return 0;
    float ox, oy, sx, sy; MenuMap(ox, oy, sx, sy);
    DDSURFACEDESC2 d;
    if (IDirectDrawSurface7* bb = LockBack(d, DDLOCK_WAIT | DDLOCK_READONLY)) {
        int w = (int)d.dwWidth, h = (int)d.dwHeight;
        for (int y = 0; y < 480; y++) {                            // the frame, at 640x480
            int ry = (int)(oy + (y + 0.5f) * sy); if (ry < 0) ry = 0; if (ry >= h) ry = h - 1;
            const WORD* src = reinterpret_cast<const WORD*>(static_cast<const BYTE*>(d.lpSurface) + ry * d.lPitch);
            WORD* dst = g_shadow + y * 640;
            for (int x = 0; x < 640; x++) {
                int rx = (int)(ox + (x + 0.5f) * sx); if (rx < 0) rx = 0; if (rx >= w) rx = w - 1;
                dst[x] = src[rx];
            }
        }
        bb->Unlock(nullptr);
    } else {
        memset(g_shadow, 0, sizeof(g_shadow));
    }
    memcpy(g_snap, g_shadow, sizeof(g_snap));
    *pptr = g_shadow; *ppitch = 640 * 2; *pfmt = G<int>(ws::fmt565);
    G<DWORD>(ws::lockState) = which ? 2 : 1;
    G<DWORD>(ws::lockBack) = 1;
    g_shadowLocked = true;
    return 1;
}

// 0x46BB50: the pixels the game changed in the copy, scaled into the frame.
extern "C" int WsUnlock() {
    if (!g_shadowLocked) return 0;                                 // not ours: the original unlock
    g_shadowLocked = false;
    G<DWORD>(ws::lockState) = 0;
    float ox, oy, sx, sy; MenuMap(ox, oy, sx, sy);
    DDSURFACEDESC2 d;
    IDirectDrawSurface7* bb = LockBack(d, DDLOCK_WAIT | DDLOCK_WRITEONLY);
    if (!bb) return 1;
    int w = (int)d.dwWidth, h = (int)d.dwHeight;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    int x0 = (int)ceilf(ox - 0.5f), x1 = (int)ceilf(ox + 640.0f * sx - 0.5f);
    int y0 = (int)ceilf(oy - 0.5f), y1 = (int)ceilf(oy + 480.0f * sy - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    for (int x = x0; x < x1; x++) { int s = (int)((x + 0.5f - ox) / sx); g_colX[x] = s < 0 ? 0 : s > 639 ? 639 : s; }
    for (int y = y0; y < y1; y++) { int s = (int)((y + 0.5f - oy) / sy); g_rowY[y] = s < 0 ? 0 : s > 479 ? 479 : s; }
    for (int y = 0; y < 480 && !g_menuFull; y++)                  // a picture across the whole 4:3 screen?
        if (g_shadow[y * 640] != g_snap[y * 640])
            for (int y2 = 0; y2 < 480; y2++)
                if (g_shadow[y2 * 640 + 639] != g_snap[y2 * 640 + 639]) { g_menuFull = true; break; }
    for (int y = y0; y < y1; y++) {
        const WORD* sh = g_shadow + g_rowY[y] * 640;
        const WORD* sn = g_snap + g_rowY[y] * 640;
        WORD* dst = reinterpret_cast<WORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch);
        for (int x = x0; x < x1; x++) {
            int c = g_colX[x];
            if (sh[c] != sn[c]) dst[x] = sh[c];
        }
    }
    bb->Unlock(nullptr);
    return 1;
}

extern "C" void WsTagStub();
extern "C" void WsFlushStub();
extern "C" void WsProjStub();
extern "C" void WsRaceStub();
extern "C" void WsHudStub();
extern "C" void WsLastStub();
extern "C" void WsGlowStub();
extern "C" void WsGlyphQuadStub();
extern "C" void WsGlyphDrawStub();
extern "C" void WsStrStub();
extern "C" void WsFadeStub();
extern "C" void WsUnlockStub();
extern "C" void WsBeginStub();
extern "C" void WsSkyStub();
extern "C" void WsFlipStub();
asm(
    ".text\n"
    // 0x46D89F: "lea eax, [eax+0x6364C0] / ret" (the end of 0x46D850)
    ".globl _WsTagStub\n"
    "_WsTagStub:\n"
    "    leal 0x6364C0(%eax), %eax\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    call _WsTagTri\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    ret\n"
    // 0x46CE20: "mov eax, [0x63F550]" (5 bytes)
    ".globl _WsFlushStub\n"
    "_WsFlushStub:\n"
    "    pushal\n"
    "    call _WsFlush\n"
    "    popal\n"
    "    movl 0x63F550, %eax\n"
    "    pushl $0x46CE25\n"
    "    ret\n"
    // 0x4528AF: "add esp, 0x14 / ret 4"
    ".globl _WsProjStub\n"
    "_WsProjStub:\n"
    "    addl $0x14, %esp\n"
    "    pushal\n"
    "    call _WsProjFix\n"
    "    popal\n"
    "    ret $4\n"
    // 0x44C9C0: "sub esp, 0x10 / xor ecx, ecx" (the race draw starts: 3D)
    ".globl _WsRaceStub\n"
    "_WsRaceStub:\n"
    "    movl $0, _g_wsLayer\n"            // kT3D
    "    subl $0x10, %esp\n"
    "    xorl %ecx, %ecx\n"
    "    pushl $0x44C9C5\n"
    "    ret\n"
    // 0x44CB1B: call 0x4316E0 (the HUD)
    ".globl _WsHudStub\n"
    "_WsHudStub:\n"
    "    movl $1, _g_wsLayer\n"            // kTHud
    "    movl $0x4316E0, %eax\n"
    "    call *%eax\n"
    "    movl $0, _g_wsLayer\n"
    "    ret\n"
    // 0x44CB20: call 0x4352D0, the race draw's last call
    ".globl _WsLastStub\n"
    "_WsLastStub:\n"
    "    movl $0x4352D0, %eax\n"
    "    call *%eax\n"
    "    movl $2, _g_wsLayer\n"            // kTMenu
    "    ret\n"
    // 0x409760: "mov eax, [0x4F63E0]" (5 bytes), plain ret: the light glows
    ".globl _WsGlowStub\n"
    "_WsGlowStub:\n"
    "    pushal\n"
    "    call _GlowPre\n"
    "    popal\n"
    "    call L_glowTramp\n"
    "    pushal\n"
    "    call _GlowPost\n"
    "    popal\n"
    "    ret\n"
    "L_glowTramp:\n"
    "    movl 0x4F63E0, %eax\n"
    "    pushl $0x409765\n"
    "    ret\n"
    // 0x4513B2 / 0x47BB59: call 0x47B7F0 (8 arguments pushed, eax free)
    ".globl _WsGlyphQuadStub\n"
    "_WsGlyphQuadStub:\n"
    "    pushal\n"
    "    pushl %esi\n"
    "    call _WsGlyphBegin\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp L_quadForced\n"
    ".globl _WsFadeStub\n"
    "_WsFadeStub:\n"
    "    pushal\n"
    "    call _WsFadeBegin\n"
    "    popal\n"
    "L_quadForced:\n"
    "    popl _g_wsRetSave\n"
    "    movl $0x47B7F0, %eax\n"
    "    call *%eax\n"                     // (ret 0x20)
    "    pushl _g_wsRetSave\n"
    "    pushal\n"
    "    call _WsForceEnd\n"
    "    popal\n"
    "    ret\n"
    // 0x451280: "sub esp, 0x18 / push esi / push edi" (5 bytes); ecx = records, edx = count
    ".globl _WsGlyphDrawStub\n"
    "_WsGlyphDrawStub:\n"
    "    pushal\n"
    "    pushl %edx\n"
    "    pushl %ecx\n"
    "    call _WsGlyphPrepass\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    subl $0x18, %esp\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl $0x451285\n"
    "    ret\n"
    // 0x450E40: "sub esp, 0x30 / mov edx, [esp+0x38]" (7 bytes); ecx = string, 2 arguments, ret 8
    ".globl _WsStrStub\n"
    "_WsStrStub:\n"
    "    pushal\n"
    "    call _WsStrBegin\n"
    "    popal\n"
    "    pushl 8(%esp)\n"
    "    pushl 8(%esp)\n"
    "    call L_strTramp\n"
    "    pushal\n"
    "    call _WsStrEnd\n"
    "    popal\n"
    "    ret $8\n"
    "L_strTramp:\n"
    "    subl $0x30, %esp\n"
    "    movl 0x38(%esp), %edx\n"
    "    pushl $0x450E47\n"
    "    ret\n"
    // 0x478040: call 0x472050 (the flip; eax free)
    ".globl _WsFlipStub\n"
    "_WsFlipStub:\n"
    "    pushal\n"
    "    call _WsPresent\n"
    "    popal\n"
    "    movl $0x472050, %eax\n"
    "    jmp *%eax\n"
    // 0x44CACF: call 0x44D1E0, the race's sky (eax free)
    ".globl _WsSkyStub\n"
    "_WsSkyStub:\n"
    "    movl $7, _g_wsForce\n"           // kTSky
    "    movl $0x44D1E0, %eax\n"
    "    call *%eax\n"
    "    movl $-1, _g_wsForce\n"
    "    ret\n"
    // 0x4780B1: call 0x46CDA0 (BeginScene; eax free after it)
    ".globl _WsBeginStub\n"
    "_WsBeginStub:\n"
    "    movl $0x46CDA0, %eax\n"
    "    call *%eax\n"
    "    pushal\n"
    "    call _WsFrameBegin\n"
    "    popal\n"
    "    ret\n"
    // 0x46BB50: "mov eax, [0x63F51C]" (5 bytes), no arguments
    ".globl _WsUnlockStub\n"
    "_WsUnlockStub:\n"
    "    pushal\n"
    "    call _WsUnlock\n"
    "    testl %eax, %eax\n"
    "    popal\n"
    "    jz L_unlockOrig\n"
    "    ret\n"
    "L_unlockOrig:\n"
    "    movl 0x63F51C, %eax\n"
    "    pushl $0x46BB55\n"
    "    ret\n"
);

static void InstallWidescreen() {
    if (!g_cfg.widescreen) { Logf("widescreen: off (widescreen=0), 640x480\n"); return; }
    if (!SigsMatch(ws::kSigs, sizeof(ws::kSigs) / sizeof(ws::kSigs[0]))) { Logf("widescreen: off\n"); return; }
    int w = g_cfg.wsWidth, h = g_cfg.wsHeight;
    if (w <= 0 || h <= 0) {
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
            w = (int)dm.dmPelsWidth; h = (int)dm.dmPelsHeight;
        } else {
            w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN);
        }
    }
    if (w < 640 || h < 480 || w > 8192 || h > 8192) {
        Logf("widescreen: off (%dx%d is outside 640x480 - 8192x8192)\n", w, h);
        return;
    }
    g_wsW = w; g_wsH = h;
    float sw = w / 640.0f, sh = h / 480.0f;
    g_wsS = sw < sh ? sw : sh;
    g_wsK = w / (640.0f * g_wsS);
    g_wsCx = w * 0.5f; g_wsCy = h * 0.5f;
    g_wsSx = sw; g_wsSy = sh;
    bool ok = true;
    for (const ws::Imm& m : ws::kImm) {
        DWORD v = m.height ? (DWORD)h : (DWORD)w;
        ok = ok && PatchBytes(m.addr + m.off, &v, 4);
    }
    DWORD halfW = reinterpret_cast<DWORD>(&g_halfW);
    for (DWORD a : ws::kHalfWReads) ok = ok && PatchBytes(a + 2, &halfW, 4);
    DWORD spread = reinterpret_cast<DWORD>(&g_markerSpread);
    for (DWORD a : ws::kMarkerSpread) ok = ok && PatchBytes(a + 2, &spread, 4);
    ok = ok && PatchJmp(ws::tagSlot, reinterpret_cast<void*>(&WsTagStub), 7)
            && PatchJmp(ws::flush, reinterpret_cast<void*>(&WsFlushStub), 5)
            && PatchJmp(ws::projEnd, reinterpret_cast<void*>(&WsProjStub), 6)
            && PatchJmp(ws::raceDraw, reinterpret_cast<void*>(&WsRaceStub), 5)
            && PatchCall(ws::callHud, reinterpret_cast<void*>(&WsHudStub), 5)
            && PatchCall(ws::callLast, reinterpret_cast<void*>(&WsLastStub), 5)
            && PatchCall(ws::glyphQuad, reinterpret_cast<void*>(&WsGlyphQuadStub), 5)
            && PatchJmp(ws::glyphDraw, reinterpret_cast<void*>(&WsGlyphDrawStub), 5)
            && PatchJmp(ws::strQueue, reinterpret_cast<void*>(&WsStrStub), 7)
            && PatchCall(ws::fadeQuad, reinterpret_cast<void*>(&WsFadeStub), 5)
            && PatchJmp(ws::lockBB, reinterpret_cast<void*>(&WsLock), 6)
            && PatchJmp(ws::unlockBB, reinterpret_cast<void*>(&WsUnlockStub), 5)
            && PatchCall(ws::callBegin, reinterpret_cast<void*>(&WsBeginStub), 5)
            && PatchCall(ws::callSky, reinterpret_cast<void*>(&WsSkyStub), 5)
            && PatchCall(ws::callFlip, reinterpret_cast<void*>(&WsFlipStub), 5);
    if (!ok) { Logf("widescreen: patching failed\n"); return; }
    g_wsOn = true;
    static const char* const kHud[] = { "stretched", "4:3 at the screen edges", "4:3 in the middle" };
    Logf("widescreen: %dx%d (Direct3D renderer), aspect %.3f; 3D %s, HUD %s, menus %s\n",
         w, h, (double)w / h, g_cfg.wsFixFOV ? "Hor+" : "stretched",
         kHud[g_cfg.wsHud >= 0 && g_cfg.wsHud <= 2 ? g_cfg.wsHud : 1],
         g_cfg.wsMenus == 2 ? "stretched" : "4:3 in the middle");
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
static void Install() {
    if (!g_cfg.interpolate) { Logf("frame rate: original (30 fps, interpolate=0)\n"); return; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart;
    if (g_qpf <= 0) { Logf("frame rate: no performance counter - original kept\n"); return; }
    g_tickPeriod = g_qpf / 30;
    { float c = cosf(g_cfg.maxTurn * 3.14159265f / 180.0f); g_cos2Turn = c * c; }
    g_blendObjects = g_cfg.blendObjects != 0;
    bool ok = PatchJmp(ht::mulInto, reinterpret_cast<void*>(&MulStub), 5)
           && PatchJmp(ht::spriteDraw, reinterpret_cast<void*>(&SpriteStub), 6)
           && PatchJmp(ht::faceBuild[0], reinterpret_cast<void*>(&FaceStub0), 6)
           && PatchJmp(ht::faceBuild[1], reinterpret_cast<void*>(&FaceStub1), 6)
           && PatchJmp(ht::faceBuild[2], reinterpret_cast<void*>(&FaceStub2), 6)
           && PatchJmp(ht::pushMulB, reinterpret_cast<void*>(&PushStubB), 6)
           && PatchJmp(ht::viewSet,  reinterpret_cast<void*>(&ViewStub), 6)
           && PatchCall(ht::callDraw, reinterpret_cast<void*>(&DrawStub), 6)
           && PatchCall(ht::callFrame, reinterpret_cast<void*>(&Frame), 5);
    DWORD stampAddr = reinterpret_cast<DWORD>(&g_drawStamp);
    for (const ht::Sig& s : ht::kStampSites)
        ok = ok && PatchBytes(s.addr + s.len - 4, &stampAddr, 4);
    if (!ok) { Logf("frame rate: patching failed\n"); return; }
    g_active = true;
    Logf("frame rate: interpolation on (30 Hz ticks, races drawn every frame%s)\n",
         g_blendObjects ? "" : ", camera only");
}

// ---------------------------------------------------------------------------
// Arcade sound
// ---------------------------------------------------------------------------
// The PC version's sounds are the arcade's, brought down to 11025 Hz 4-bit
// ADPCM. HTArcade.pak holds the arcade's own (16-bit, 31250 Hz, from the
// arcade hard disk's decoded sound caches), each keyed by where the PC sound
// it replaces sits in Hydro.fsd. Built by tools/HTArcadePak.
//   effects: "h:\sound\N.esf" is opened (0x46ED90) and read whole (0x46EF60)
//     into a DirectSound buffer made by 0x47C3F0, which only takes 11025 or
//     22050 Hz (patched to take any rate). Pitch changes (0x47CB80) set an
//     absolute frequency made for 11025 Hz sounds; it is scaled for these.
//   music: a thread streams "track%02d.esf" through a 2-second buffer, one
//     1-second half at a time (0x46AFC0, decoding the ADPCM read in by
//     0x46AE90). The buffer is made 31250 Hz (2 x 1 s) and the half refill
//     replaced: arcade tracks are played (looped seamlessly), the others are
//     decoded as before and resampled.
namespace snd {
struct PakEnt { DWORD fsdOff, rate, samples, flags, dataOff; };   // flags: 1 music, 2 looped effect
enum : DWORD { kMusic = 1 };
static const BYTE* g_pak = nullptr;
static DWORD g_nEnts = 0;
static const PakEnt* g_ents = nullptr;

static const PakEnt* Find(DWORD off) {
    if (!off) return nullptr;
    DWORD lo = 0, hi = g_nEnts;
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        if (g_ents[mid].fsdOff < off) lo = mid + 1; else hi = mid;
    }
    return lo < g_nEnts && g_ents[lo].fsdOff == off ? &g_ents[lo] : nullptr;
}
static inline const short* Pcm(const PakEnt* e) { return reinterpret_cast<const short*>(g_pak + e->dataOff); }

// archive handle -> offset of the file in Hydro.fsd (0 = loose files / bad handle)
static DWORD FsdOffset(DWORD h) {
    if (!G<DWORD>(0x68F4AC) || h < 1 || h > 0x800) return 0;
    return G<DWORD>(0x6674A8 + (h - 1) * 16);
}

static bool LoadPak(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr);
    HANDLE m = size > 12 ? CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(f);
    if (!m) return false;
    const BYTE* p = static_cast<const BYTE*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(m);
    if (!p) return false;
    DWORD n = *reinterpret_cast<const DWORD*>(p + 8);
    bool ok = !memcmp(p, "HTAP", 4) && *reinterpret_cast<const DWORD*>(p + 4) == 1 && n && n < 0x10000 && 12 + n * 20 <= size;
    const PakEnt* e = reinterpret_cast<const PakEnt*>(p + 12);
    for (DWORD i = 0; ok && i < n; i++)
        ok = e[i].rate >= 4000 && e[i].rate <= 96000 && e[i].samples && e[i].samples < 0x4000000 &&
             e[i].dataOff >= 12 + n * 20 && e[i].dataOff <= size && e[i].samples * 2 <= size - e[i].dataOff &&
             !(e[i].dataOff & 1) && (i == 0 || e[i - 1].fsdOff < e[i].fsdOff);
    if (!ok) { UnmapViewOfFile(p); return false; }
    g_pak = p; g_ents = e; g_nEnts = n;
    return true;
}

// ---- effects
static const PakEnt* g_esf = nullptr;      // the replacement for the ESF open now (main thread)
static float g_ratio[0x400];               // per sound id: replacement rate / the PC sound's rate; 0 = PC sound
static BYTE* g_esfOpenTramp = nullptr;
typedef DWORD (__fastcall *EsfOpenFn)(const char*, int*, int*, int*);

static int SoundId(const char* name) {                             // "h:\sound\123.esf" -> 123
    if (!name) return -1;
    const char* s = strrchr(name, '\\'); s = s ? s + 1 : name;
    if (*s < '0' || *s > '9') return -1;
    int id = atoi(s);
    return id >= 0 && id < 0x400 ? id : -1;
}
static DWORD __fastcall EsfOpenHook(const char* name, int* bits, int* rate, int* loop) {
    DWORD size = reinterpret_cast<EsfOpenFn>(g_esfOpenTramp)(name, bits, rate, loop);
    const PakEnt* e = Find(FsdOffset(G<DWORD>(0x665010)));
    if (e && (e->flags & kMusic)) e = nullptr;
    int id = SoundId(name);
    if (id >= 0) g_ratio[id] = 0.0f;
    g_esf = e;
    if (!e) return size;
    int orig = rate && *rate > 0 ? *rate : 11025;
    if (bits) *bits = 16;
    if (rate) *rate = (int)e->rate;
    if (id >= 0) g_ratio[id] = (float)e->rate / (float)orig;
    return e->samples * 2;
}
static void __fastcall EsfReadAllHook(BYTE* dst) {
    if (g_esf) { memcpy(dst, Pcm(g_esf), g_esf->samples * 2); return; }
    reinterpret_cast<void (__fastcall*)(BYTE*, DWORD)>(0x46EE70)(dst, G<DWORD>(0x66500C) & 0x0FFFFFFF);
}
static void EsfCloseHook() {
    g_esf = nullptr;
    DWORD h = G<DWORD>(0x665010);
    if (h) reinterpret_cast<DWORD (__fastcall*)(DWORD)>(0x470A10)(h);
    G<DWORD>(0x665010) = 0;
}
// 0x47CB80 (cl = channel, edx = Hz), as the game's, with the frequency scaled for a replaced sound
static void __fastcall SetFreqHook(DWORD chReg, DWORD freq) {
    BYTE ch = (BYTE)chReg;
    if (ch >= 0x10) return;
    short id = G<short>(0x13C1958 + ch * 4);
    if (id == -1 || !G<DWORD>(0x13B6958 + id * 44)) return;
    int idx = G<short>(0x13C195A + ch * 4) + id * 11;
    IDirectSoundBuffer* b = G<IDirectSoundBuffer*>(0x13B6958 + idx * 4);
    if (!b) return;
    if (id >= 0 && id < 0x400 && g_ratio[id] > 0.0f) {
        float f = (float)freq * g_ratio[id];
        freq = f > (float)DSBFREQUENCY_MAX ? DSBFREQUENCY_MAX : (DWORD)(f + 0.5f);
    }
    G<HRESULT>(0x13C19D0) = b->SetFrequency(freq);
}

// ---- music
enum : DWORD { kMRate = 31250, kMHalf = kMRate * 2, kMBuf = kMHalf * 2 };   // bytes: 1 s halves, 16-bit mono
static const PakEnt* g_track = nullptr;    // the arcade track playing (music thread)
static short g_pcm[11025];                  // one PC half (1 s at 11025 Hz)
static short g_prevLast = 0;
typedef void (__fastcall *AdpcmFn)(const BYTE*, short*, DWORD, DWORD*);

static void FillHalf(short* out, DWORD cnt) {
    if (g_track) {                                                 // arcade track: looped seamlessly
        const short* s = Pcm(g_track);
        DWORD len = g_track->samples, t = (DWORD)(((unsigned long long)cnt * kMRate) % len);
        for (DWORD i = 0; i < kMRate; i++) { out[i] = s[t]; if (++t == len) t = 0; }
        return;
    }
    DWORD n = G<DWORD>(0x6344EC), idx = n ? cnt % n : 0, rem = G<DWORD>(0x6344F4) % 22050, bytes = 22050;
    if (idx == n - 1 && rem) { memset(g_pcm, 0, sizeof(g_pcm)); bytes = rem; }
    reinterpret_cast<AdpcmFn>(0x46A6B0)(reinterpret_cast<const BYTE*>(0x5916F8 + ((idx * 22050) >> 2)), g_pcm, bytes,
                                       reinterpret_cast<DWORD*>(0x6344F8));
    if (idx == n - 1) G<DWORD>(0x6344F8) = 0;
    if (idx == 0) g_prevLast = g_pcm[0];
    // 11025 -> 31250: output j sits at input j * 441 / 1250; sample -1 is the last half's last one
    for (DWORD j = 0; j < kMRate; j++) {
        DWORD num = j * 441, k = num / 1250, fr = num % 1250;
        int a = k ? g_pcm[k - 1] : g_prevLast, b = g_pcm[k < 11025 ? k : 11024];
        // position k - 1 + fr/1250 between samples k-1 and k (keeps the first output on the last input)
        out[j] = (short)(a + (b - a) * (int)fr / 1250);
    }
    g_prevLast = g_pcm[11024];
}

// replaces 0x46AFC0 (the music thread's refill); the game's logic, at 31250 Hz
static void MusicRefill() {
    if (!G<DWORD>(0x6344DC)) return;
    IDirectSoundBuffer* b = G<IDirectSoundBuffer*>(0x6344C8);
    DWORD cnt = G<DWORD>(0x634508), read = G<DWORD>(0x634504), n = G<DWORD>(0x6344EC);
    if (!n || (read < n && cnt >= read)) return;                   // its chunk is not read in yet
    if (cnt > 0) {
        DWORD play = 0, write = 0;
        if (b) b->GetCurrentPosition(&play, &write);
        if (play / kMHalf != ((cnt - 1) & 1)) return;              // still playing the other half
    } else {
        const PakEnt* e = Find(FsdOffset(G<DWORD>(0x6344FC)));
        g_track = e && (e->flags & kMusic) ? e : nullptr;
        g_prevLast = 0;
    }
    if (!G<DWORD>(0x6344F0) && cnt >= n) { reinterpret_cast<void (*)()>(0x46AC30)(); return; }
    if (!b) { G<DWORD>(0x634508) = cnt + 1; return; }
    DWORD status = 0;
    b->GetStatus(&status);
    void *p1, *p2; DWORD l1, l2;
    if (status & DSBSTATUS_BUFFERLOST) {
        b->Restore();
        if (SUCCEEDED(b->Lock(0, kMBuf, &p1, &l1, &p2, &l2, 0))) {
            if (p1) memset(p1, 0, l1);
            b->Unlock(p1, l1, p2, l2);
        }
    }
    if (SUCCEEDED(b->Lock((cnt & 1) * kMHalf, kMHalf, &p1, &l1, &p2, &l2, 0))) {
        if (p1 && l1 == kMHalf) FillHalf(static_cast<short*>(p1), cnt);
        b->Unlock(p1, l1, p2, l2);
    }
    if (cnt == 0) { b->SetCurrentPosition(0); b->Play(0, 0, DSBPLAY_LOOPING); }
    G<DWORD>(0x634508) = cnt + 1;
}
}  // namespace snd

// The game also sets DirectSound's primary buffer, the format everything is
// mixed into, to 11025 Hz (0x47CDCC, SetFormat). Windows' own DirectSound
// ignores that, but Wine and DirectSound replacements mix at it, which would
// bring the arcade sounds back down to 11 kHz: with the pack, it is 44100 Hz.
static const ht::Sig kPrimarySigs[] = {
    { 0x47CDE4, "\xC7\x44\x24\x14\x11\x2B\x00\x00", 8 },   // nSamplesPerSec = 11025
    { 0x47CDFA, "\xC7\x44\x24\x18\x44\xAC\x00\x00", 8 },   // nAvgBytesPerSec = 44100 (11025 * 4)
};
static const ht::Sig kArcadeSigs[] = {
    { 0x46ED90, "\x56\x57\x8B\xFA\x8B\xF1\xE8", 7 },               // EsfOpen
    { 0x46EF60, "\x8B\x15\x0C\x50\x66\x00\x81\xE2\xFF\xFF\xFF\x0F\xE9", 13 },   // EsfReadAll
    { 0x46EF80, "\x8B\x0D\x10\x50\x66\x00\x85\xC9\x74\x05\xE8", 11 },  // EsfClose
    { 0x47CB80, "\x80\xF9\x10\x56\x73\x4F", 6 },                   // SetFrequency
    { 0x47C42C, "\x3D\x22\x56\x00\x00\x74\x05\xB8\x11\x2B\x00\x00", 12 },   // rate: 22050 or 11025
    { 0x47C460, "\x33\xC9\x3D\x11\x2B\x00\x00\x0F\x95\xC1\x33\xC0\x66\x89\x7C\x24\x28\x8D\x7C\x24\x2C"
                "\x49\x81\xE1\xEF\xD4\xFF\xFF\x81\xC1\x22\x56\x00\x00", 34 },
    { 0x46AA7A, "\x68\x11\x2B\x00\x00\xBA\x10\x00\x00\x00\xB9\x44\xAC\x00\x00", 15 },   // music buffer
    { 0x46AFC0, "\x83\xEC\x14\xA1\xDC\x44\x63\x00", 8 },           // music refill
    { 0x46A7F5, "\xC2\x08\x00", 3 },                               // ADPCM decoder: fastcall, 2 stack args
};
static void InstallArcadeSound() {
    if (!g_cfg.arcadeSound) { Logf("arcade sound: off (arcadeSound=0)\n"); return; }
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTArcade.pak", g_asiDir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { Logf("arcade sound: no HTArcade.pak, PC sounds\n"); return; }
    if (!SigsMatch(kArcadeSigs, sizeof(kArcadeSigs) / sizeof(kArcadeSigs[0]))) { Logf("arcade sound: off\n"); return; }
    if (!snd::LoadPak(path)) { Logf("arcade sound: HTArcade.pak is not valid, PC sounds\n"); return; }
    BYTE* t = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return;
    memcpy(t, "\x56\x57\x8B\xFA\x8B\xF1\xE9", 7);                 // EsfOpen's first 6 bytes, then on to 0x46ED96
    DWORD rel = 0x46ED96 - (reinterpret_cast<DWORD>(t) + 11); memcpy(t + 7, &rel, 4);
    snd::g_esfOpenTramp = t;
    static const BYTE anyRate[] = { 0xEB };                        // 0x47C431: je -> jmp (keep the rate)
    static const BYTE rateFmt[34] = { 0x8B, 0xC8, 0x33, 0xC0, 0x66, 0x89, 0x7C, 0x24, 0x28, 0x8D, 0x7C, 0x24, 0x2C,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    DWORD mRate = snd::kMRate, mBuf = snd::kMBuf;
    unsigned music = 0;
    for (DWORD i = 0; i < snd::g_nEnts; i++) music += (snd::g_ents[i].flags & snd::kMusic) ? 1 : 0;
    bool ok = PatchBytes(0x47C431, anyRate, 1) && PatchBytes(0x47C460, rateFmt, 34) &&   // ecx = rate
              PatchJmp(0x46ED90, reinterpret_cast<void*>(&snd::EsfOpenHook), 6) &&
              PatchJmp(0x46EF60, reinterpret_cast<void*>(&snd::EsfReadAllHook), 6) &&
              PatchJmp(0x46EF80, reinterpret_cast<void*>(&snd::EsfCloseHook), 6) &&
              PatchJmp(0x47CB80, reinterpret_cast<void*>(&snd::SetFreqHook), 6) &&
              PatchBytes(0x46AA7A + 1, &mRate, 4) && PatchBytes(0x46AA84 + 1, &mBuf, 4) &&
              PatchJmp(0x46AFC0, reinterpret_cast<void*>(&snd::MusicRefill), 8);
    Logf("arcade sound: %s - HTArcade.pak, %lu sounds (%u music tracks)\n", ok ? "on" : "patch failed",
         snd::g_nEnts, music);
    DWORD r = 44100, bytes = 44100 * 4;
    if (ok && SigsMatch(kPrimarySigs, sizeof(kPrimarySigs) / sizeof(kPrimarySigs[0])) &&
        PatchBytes(0x47CDE4 + 4, &r, 4) && PatchBytes(0x47CDFA + 4, &bytes, 4))
        Logf("arcade sound: mixed at 44100 Hz (the game asks for 11025)\n");
}


// ---------------------------------------------------------------------------
// Game settings left from the arcade
// ---------------------------------------------------------------------------
// The PC version keeps the arcade's operator settings (0x1455420, loaded from
// Hydro.cfg at start-up by 0x428EC0) without the operator menu: a track
// difficulty per track (+0x50, the checkpoint times: 0x451B60 picks them from a
// table, 0 = most time, 99 = least) and an AI difficulty per track (+0x84). The
// AI's are put back to 50 on every start, the track ones stay at 40. Both are
// set from the ini once the settings are loaded.
// The PC data also still has the arcade's own Hydro Speedway and Lake Powell
// (HETXTR2 / HFTXTR3, with their own skies and water); the PC's track table
// points at reworked ones. Their slots can be pointed back.
static void ApplyDifficulty() {
    for (int t = 0; t < 13; t++) {
        int d = g_cfg.trackDifficulty[t], a = g_cfg.aiDifficulty[t];
        if (d > 0) G<int>(0x1455470 + t * 4) = (d > 100 ? 100 : d) - 1;
        if (a > 0) G<int>(0x14554A4 + t * 4) = a > 100 ? 100 : a;
    }
}
// replaces the call at 0x428ED4 (to 0x402910, a bare ret) at the end of the settings' load
static const char kXtr2[] = "HETXTR2TRH0", kXtr3[] = "HFTXTR3TRH0";
static const ht::Sig kGameSigs[] = {
    { 0x428ED4, "\xE8\x37\x9A\xFD\xFF\xB8\x01\x00\x00\x00", 10 },   // operator settings loaded: call 0x402910
    { 0x402910, "\xC3", 1 },                                         //   (ret)
    { 0x43C8FF, "\x8B\x04\x85\x70\x54\x45\x01", 7 },                // track difficulty read
    { 0x4EF69C, "\xD8\xF8\x4E\x00", 4 },                            // Hydro Speedway's track file
    { 0x4EEE3C, "\x10\xFA\x4E\x00", 4 },                            // Lake Powell's
    { 0x4EF8D8, "HZTLOO2TRH0", 11 },
    { 0x4EFA10, "HPTPWELTRH0", 11 },
};
static void InstallGame() {
    bool diff = false;
    for (int t = 0; t < 13; t++) diff |= g_cfg.trackDifficulty[t] > 0 || g_cfg.aiDifficulty[t] > 0;
    if (!diff && !g_cfg.arcadeSpeedway && !g_cfg.arcadeLakePowell) return;
    if (!SigsMatch(kGameSigs, sizeof(kGameSigs) / sizeof(kGameSigs[0]))) { Logf("arcade settings and tracks: off\n"); return; }
    if (diff && PatchCall(0x428ED4, reinterpret_cast<void*>(&ApplyDifficulty), 5)) {
        char a[80] = "", b[80] = "";
        for (int t = 0; t < 13; t++) {
            snprintf(a + strlen(a), sizeof(a) - strlen(a), "%s%d", t ? "," : "", g_cfg.trackDifficulty[t]);
            snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%d", t ? "," : "", g_cfg.aiDifficulty[t]);
        }
        Logf("difficulty: tracks %s, AI %s (0 = the game's)\n", a, b);
    }
    DWORD p2 = reinterpret_cast<DWORD>(kXtr2), p3 = reinterpret_cast<DWORD>(kXtr3);
    if (g_cfg.arcadeSpeedway && PatchBytes(0x4EF69C, &p2, 4)) Logf("Hydro Speedway: the arcade's\n");
    if (g_cfg.arcadeLakePowell && PatchBytes(0x4EEE3C, &p3, 4)) Logf("Lake Powell: the arcade's\n");
}

// A crash in the game is logged (the first few), with what the mod was doing.
static LONG CALLBACK CrashLog(EXCEPTION_POINTERS* e) {
    static int logged = 0;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    if (logged >= 3) return EXCEPTION_CONTINUE_SEARCH;
    logged++;
    const CONTEXT* c = e->ContextRecord;
    Logf("crash: exception %08lX at %08lX (address %08lX)\n", code, c->Eip,
         e->ExceptionRecord->NumberParameters > 1 ? (DWORD)e->ExceptionRecord->ExceptionInformation[1] : 0);
    Logf("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX\n",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
    if (!IsBadReadPtr(sp, 64))
        Logf("  stack %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
             sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8], sp[9], sp[10], sp[11]);
    Logf("  draw fn %06lX (pending %06lX), tick %lu, in draw %d, shown frame %d, blend %d, pre %d post %d, layer %d\n",
         G<DWORD>(ht::drawFn), G<DWORD>(ht::pendingDrawFn), G<DWORD>(ht::tickCount), (int)g_inDraw,
         (int)g_inDisplay, (int)g_blend, (int)g_preValid, (int)g_postValid, g_wsLayer);
    Logf("  sky: loaded %lu set %lu layers %lu, textures %08lX %08lX %08lX %08lX %08lX %08lX | %08lX %08lX %08lX %08lX %08lX %08lX\n",
         G<DWORD>(0x558010), G<DWORD>(0x5579C8), G<DWORD>(0x5579D8),
         G<DWORD>(0x558014), G<DWORD>(0x558018), G<DWORD>(0x55801C), G<DWORD>(0x558020), G<DWORD>(0x558024), G<DWORD>(0x558028),
         G<DWORD>(0x55802C), G<DWORD>(0x558030), G<DWORD>(0x558034), G<DWORD>(0x558038), G<DWORD>(0x55803C), G<DWORD>(0x558040));
    if (g_preValid) {
        const DWORD* pre = reinterpret_cast<const DWORD*>(g_statePre + (0x558014 - 0x557980));
        Logf("  sky textures saved before the tick's draw: %08lX %08lX %08lX %08lX %08lX %08lX\n",
             pre[0], pre[1], pre[2], pre[3], pre[4], pre[5]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitDirs();
    LoadConfig();
    if (g_cfg.logToFile) {
        char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTFrameFix.log", g_asiDir);
        g_log = fopen(path, "w");
    }
    Logf("HTFrameFix v" HTFF_VERSION "\n");
    if (!CheckSigs()) return TRUE;
    // (Only on the analysed exe: the crash log reads the game's globals.)
    if (g_log) AddVectoredExceptionHandler(0, CrashLog);
    Install();
    InstallWidescreen();
    InstallArcadeSound();
    InstallGame();
    if (g_active || g_wsOn) {
        if (PatchJmp(ws::glows, reinterpret_cast<void*>(&WsGlowStub), 5))
            Logf("light glows: %s\n", g_active ? "blended with the camera" : "widescreen only");
    }
    return TRUE;
}
