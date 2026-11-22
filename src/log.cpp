#include "common.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
FILE* g_log = nullptr;
void Logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}
