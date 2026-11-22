// What the other parts use from timing.cpp.
#pragma once
#include "common.h"

typedef void (*Fn0)();
typedef void (__attribute__((fastcall)) *FnC)(DWORD);
static inline void CallFn0(DWORD a) { reinterpret_cast<Fn0>(a)(); }
static inline void CallFnC(DWORD a, DWORD ecx) { reinterpret_cast<FnC>(a)(ecx); }
extern LONGLONG g_qpf, g_tickPeriod, g_framePeriod;
static inline LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void WaitUntil(LONGLONG t);
void UpdateFramePeriod();
