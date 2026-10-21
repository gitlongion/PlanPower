// ============================================================================
//  PlanPower - dateutil.h  纯日期算术
//
//  只依赖标准库、不碰 Win32。
//  存在的理由：闰年与月份天数必须只有一份实现。sched_xml.cpp 是纯逻辑模块
//  （宿主机上单独编译，不链接 core.cpp），core.cpp 是 Win32 层——两边都要
//  判断「这个日期合不合法」，各写一套迟早会漂移。
// ============================================================================
#pragma once

namespace ppdate {

// 某年某月的天数；月份非法返回 0。
inline int DaysInMonth(int year, int month) {
    static const int days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

// 年/月/日是否构成一个真实存在的日期。
// 年份下限取 1601（FILETIME 的起点），上限取 9999。
inline bool ValidDate(int year, int month, int day) {
    if (year < 1601 || year > 9999) return false;
    if (day < 1) return false;
    return day <= DaysInMonth(year, month);
}

}  // namespace ppdate
