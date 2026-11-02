// HTFrameFix.asi  v0.1
// Frame-rate unlock with interpolation for Hydro Thunder (PC, Eurocom 2000)
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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>


#define HTFF_VERSION "0.1"

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

static void LoadConfig() {
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sHTFrameFix.ini", g_asiDir);
    FILE* f = fopen(path, "r");
    if (!f) {
        if ((f = fopen(path, "w"))) { fputs(kIniText, f); fclose(f); }
        return;
    }
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
    }
    fclose(f);
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
            Logf("exe check: bytes at 0x%06lX differ - not the analysed Hydro.exe, mod off\n", s.addr);
            return false;
        }
    }
    return true;
}
static bool CheckSigs() {
    if (!SigsMatch(ht::kSigs, sizeof(ht::kSigs) / sizeof(ht::kSigs[0])) ||
        !SigsMatch(ht::kStampSites, sizeof(ht::kStampSites) / sizeof(ht::kStampSites[0]))) return false;
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
    G<DWORD>(ht::warnArmed) = 1;                                   // 0x409390: count from 0, no sound
    G<DWORD>(ht::warnCount) = 0;
    g_alpha = alpha;
    RunDraw(g_last.fn, g_last.arg, true);
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
    Install();
    return TRUE;
}
