// What the other parts use from log.cpp.
#pragma once
#include "common.h"

extern FILE* g_log;
void Logf(const char* fmt, ...);
