// ============================================================================
//  PlanPower 宿主机测试桩：最小化的 windows.h
//
//  目的只有一个——让 src/sched_xml.cpp 能在 macOS / Linux 上编译并运行，
//  从而在没有 Windows 的机器上验证「任务 XML 生成」这部分逻辑。
//  只实现该模块真正用到的东西：SYSTEMTIME / FILETIME 与两个换算函数。
//
//  时间换算按 Windows 的语义实现（SYSTEMTIME 视为 UTC），与真实系统一致。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

// 注意 DWORD 必须是 32 位。macOS/Linux 是 LP64，unsigned long 有 64 位，
// 若照抄 Windows 的 typedef 会让 ULARGE_INTEGER 这个联合体错位，
// 时间换算随之算错（实测把 1800 秒算成了 82 秒）。
typedef unsigned int        DWORD;
typedef unsigned short      WORD;
typedef unsigned char       BYTE;
typedef int                 BOOL;
typedef void*               HANDLE;
typedef void*               HWND;
typedef void*               HINSTANCE;
typedef void*               HMENU;
typedef void*               HICON;
typedef void*               HFONT;
typedef void*               HDC;
typedef unsigned long long  ULONGLONG;
typedef long long           LONGLONG;
typedef wchar_t*            LPWSTR;
typedef const wchar_t*      LPCWSTR;
typedef DWORD               COLORREF;

#define TRUE  1
#define FALSE 0
#define MAX_PATH 260
#define MAX_COMPUTERNAME_LENGTH 15
#define WINAPI

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

// ---------------------------------------------------------------------------
//  公历 ↔ 天数（Howard Hinnant 的 days_from_civil / civil_from_days）
//  以 1970-01-01 为第 0 天
// ---------------------------------------------------------------------------
inline long long PpDaysFromCivil(long long y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned  yoe = (unsigned)(y - era * 400);
    const unsigned  doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned  doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (long long)doe - 719468LL;
}

inline void PpCivilFromDays(long long z, long long* y, unsigned* m, unsigned* d) {
    z += 719468LL;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned  doe = (unsigned)(z - era * 146097);
    const unsigned  yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const long long yy  = (long long)yoe + era * 400;
    const unsigned  doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned  mp  = (5u * doy + 2u) / 153u;
    const unsigned  dd  = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned  mm  = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = yy + (mm <= 2 ? 1 : 0);
    *m = mm;
    *d = dd;
}

inline BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    if (!st || !ft) return FALSE;
    long long days = PpDaysFromCivil(st->wYear, st->wMonth, st->wDay);
    long long secs = days * 86400LL
                   + (long long)st->wHour * 3600LL
                   + (long long)st->wMinute * 60LL
                   + (long long)st->wSecond;
    unsigned long long ticks = (unsigned long long)secs * 10000000ULL
                             + (unsigned long long)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

inline BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    if (!ft || !st) return FALSE;
    unsigned long long ticks = ((unsigned long long)ft->dwHighDateTime << 32)
                             | (unsigned long long)ft->dwLowDateTime;
    long long secs = (long long)(ticks / 10000000ULL);
    DWORD     ms   = (DWORD)((ticks % 10000000ULL) / 10000ULL);

    long long days = secs / 86400LL;
    long long rem  = secs % 86400LL;
    if (rem < 0) { rem += 86400LL; days -= 1; }

    long long y; unsigned m, d;
    PpCivilFromDays(days, &y, &m, &d);

    st->wYear         = (WORD)y;
    st->wMonth        = (WORD)m;
    st->wDay          = (WORD)d;
    // 1970-01-01 是星期四（Windows 里 0=周日，故为 4）
    st->wDayOfWeek    = (WORD)(((days % 7) + 4 + 7) % 7);
    st->wHour         = (WORD)(rem / 3600);
    st->wMinute       = (WORD)((rem % 3600) / 60);
    st->wSecond       = (WORD)(rem % 60);
    st->wMilliseconds = (WORD)ms;
    return TRUE;
}

// 测试用：按年月日时分秒造一个 SYSTEMTIME，星期几自动算对
inline SYSTEMTIME PpMakeTime(WORD y, WORD mo, WORD d, WORD h, WORD mi, WORD s) {
    SYSTEMTIME st = {};
    st.wYear = y; st.wMonth = mo; st.wDay = d;
    st.wHour = h; st.wMinute = mi; st.wSecond = s;
    FILETIME ft = {};
    SystemTimeToFileTime(&st, &ft);
    SYSTEMTIME back = {};
    FileTimeToSystemTime(&ft, &back);
    st.wDayOfWeek = back.wDayOfWeek;
    return st;
}
