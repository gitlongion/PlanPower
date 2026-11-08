// ============================================================================
//  PlanPower - logname.h  日志文件名判定（纯头，宿主机可编译、可真跑）
//
//  只有一份实现，三处用：
//    · PurgeOldLogs   决定「能删哪些」
//    · EnsureLogBom   决定「能给哪些补 BOM」
//    · BuildHistoryText 决定「能读哪些」
//  各写一套的话迟早会漂移，而漂移的方向恰好是「清理删得比读取宽」——把用户
//  自己放进 logs 目录的东西删掉。宁可漏认，也不要误认。
//
//  放在纯头里不是为了复用方便，是为了能在宿主机上**真跑**：v0.4.1 的这个函数
//  把 `.log` 后缀也要求成数字，对任何真实文件名都返回 false，于是三个功能
//  一起静默失效（执行历史永远空白、过期日志从不清理、老日志 BOM 从没补过）。
//  当时的静态自检只查「三处有没有走共用判断」，不查判断本身对不对，1500 多项
//  全绿。tools/lognametest.cpp 直接编译这个头，逐条验证，并带旧实现对照。
// ============================================================================
#ifndef PLANPOWER_LOGNAME_H
#define PLANPOWER_LOGNAME_H

#include <string>

// 判断一个文件名是不是本程序按 `%04d-%02d.log` 写下的 `YYYY-MM.log`。
inline bool IsLogFileName(const std::wstring& n) {
    if (n.size() != 11) return false;              // YYYY-MM.log
    if (n[4] != L'-' || n[7] != L'.') return false;

    // 扩展名按大小写不敏感认：Windows 文件系统本身不区分大小写，用户手动
    // 改成 .LOG 不该让这个文件从执行历史里消失。
    const wchar_t ext[3] = { L'l', L'o', L'g' };
    for (int i = 0; i < 3; ++i) {
        wchar_t c = n[8 + i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (c != ext[i]) return false;
    }

    // 只有前 7 位（`YYYY-MM`）要求数字，第 5 位是 '-'。
    // v0.4.1 这里把循环上界写成了 11，于是 '.' 和 "log" 也被要求成数字，
    // 函数对任何真实文件名恒为 false。
    for (int i = 0; i < 7; ++i) {
        if (i == 4) continue;
        if (n[i] < L'0' || n[i] > L'9') return false;
    }

    int y = 0, m = 0;
    for (int i = 0; i < 4; ++i) y = y * 10 + (n[i] - L'0');
    for (int i = 5; i < 7; ++i) m = m * 10 + (n[i] - L'0');
    return y >= 2000 && m >= 1 && m <= 12;
}

#endif  // PLANPOWER_LOGNAME_H
