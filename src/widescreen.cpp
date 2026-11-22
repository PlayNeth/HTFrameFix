#include "common.h"
#include "widescreen.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "interp.h"

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

// What a triangle is (its tag), and how it is moved.
enum WsTag : BYTE { kT3D, kTHud, kTMenu, kTLeft, kTCenter, kTRight, kTStretch, kTSky };

bool  g_wsOn = false;
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

void InstallWidescreen() {
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
