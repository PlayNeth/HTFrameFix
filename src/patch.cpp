#include "common.h"
#include "patch.h"
#include "game.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------
bool PatchBytes(DWORD at, const void* bytes, size_t len) {
    void* p = reinterpret_cast<void*>(at); DWORD old;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, len);
    DWORD tmp; VirtualProtect(p, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}
bool PatchCall(DWORD at, const void* target, int len) {     // E8 rel32, rest NOP
    BYTE b[8] = { 0xE8 };
    DWORD rel = reinterpret_cast<DWORD>(target) - (at + 5); memcpy(b + 1, &rel, 4);
    for (int i = 5; i < len; i++) b[i] = 0x90;
    return PatchBytes(at, b, len);
}
bool PatchJmp(DWORD at, const void* target, int len) {      // E9 rel32, rest NOP
    BYTE b[8] = { 0xE9 };
    DWORD rel = reinterpret_cast<DWORD>(target) - (at + 5); memcpy(b + 1, &rel, 4);
    for (int i = 5; i < len; i++) b[i] = 0x90;
    return PatchBytes(at, b, len);
}
bool SigsMatch(const ht::Sig* sigs, size_t n) {
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
bool CheckSigs() {
    if (!SigsMatch(ht::kSigs, sizeof(ht::kSigs) / sizeof(ht::kSigs[0])) ||
        !SigsMatch(ht::kStampSites, sizeof(ht::kStampSites) / sizeof(ht::kStampSites[0]))) {
        Logf("exe check: not the analysed Hydro.exe - mod off\n");
        return false;
    }
    Logf("exe check: Hydro.exe matches\n");
    return true;
}
