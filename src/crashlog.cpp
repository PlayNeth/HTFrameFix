#include "common.h"
#include "crashlog.h"
#include "game.h"
#include "log.h"
#include "patch.h"
#include "interp.h"
#include "widescreen.h"

// ---------------------------------------------------------------------------
// Crash log
// ---------------------------------------------------------------------------
// A crash in the game is logged (the first few), with what the mod was doing.
LONG CALLBACK CrashLog(EXCEPTION_POINTERS* e) {
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
