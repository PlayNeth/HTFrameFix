#include "common.h"
#include "sound.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"

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
static int g_fxVol = 256, g_musicVol = 256;      // arcadeSoundVolume / arcadeMusicVolume, in 256ths
static inline short Vol(int s, int vol) { return static_cast<short>((s * vol) >> 8); }

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
    if (g_esf) {
        if (g_fxVol == 256) { memcpy(dst, Pcm(g_esf), g_esf->samples * 2); return; }
        const short* s = Pcm(g_esf);
        for (DWORD i = 0; i < g_esf->samples; i++) { short v = Vol(s[i], g_fxVol); memcpy(dst + i * 2, &v, 2); }
        return;
    }
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
        for (DWORD i = 0; i < kMRate; i++) { out[i] = Vol(s[t], g_musicVol); if (++t == len) t = 0; }
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
        out[j] = Vol(a + (b - a) * (int)fr / 1250, g_musicVol);
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
void InstallArcadeSound() {
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
    snd::g_fxVol = (g_cfg.arcadeSoundVolume * 256 + 50) / 100;
    snd::g_musicVol = (g_cfg.arcadeMusicVolume * 256 + 50) / 100;
    Logf("arcade sound: %s - HTArcade.pak, %lu sounds (%u music tracks); effects at %d%%, music at %d%%\n",
         ok ? "on" : "patch failed", snd::g_nEnts, music, g_cfg.arcadeSoundVolume, g_cfg.arcadeMusicVolume);
    DWORD r = 44100, bytes = 44100 * 4;
    if (ok && SigsMatch(kPrimarySigs, sizeof(kPrimarySigs) / sizeof(kPrimarySigs[0])) &&
        PatchBytes(0x47CDE4 + 4, &r, 4) && PatchBytes(0x47CDFA + 4, &bytes, 4))
        Logf("arcade sound: mixed at 44100 Hz (the game asks for 11025)\n");
}
