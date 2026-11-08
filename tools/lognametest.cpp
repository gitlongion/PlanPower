// ============================================================================
//  PlanPower - tools/lognametest.cpp
//  宿主机测试：直接编译真实的 src/logname.h，逐条验证日志文件名判定。
//
//  这一层存在的理由不是「多测一个函数」，而是 v0.4.1 的教训：
//    那个函数把 `.log` 后缀也要求成数字，对任何真实文件名恒为 false，
//    导致执行历史空白、过期日志不清理、老日志 BOM 不补，三处一起静默失效。
//    而当时的 1500 多项静态自检只检查「三处有没有调用共用的判断」，
//    没有一处检查判断**本身**对不对，所以全绿。
//  静态断言能证明「代码写了」，证明不了「代码算对了」。纯逻辑就抽成纯头，
//  在宿主机上真跑一遍。
//
//  编译（macOS / Linux）：
//    clang++ -std=c++17 -O1 -iquote src tools/lognametest.cpp -o build/lognametest
//    ./build/lognametest              # 正常：必须全部通过
//    ./build/lognametest --old        # 反向验证：换成有缺陷的旧实现，必须失败
// ============================================================================
#include "logname.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  有缺陷的旧实现（v0.4.1），只用于反向验证。
//
//  保留它而不是写个「一眼就错」的假实现，是为了证明这组用例真的能抓住
//  当初那个具体缺陷：循环上界写成 11，把 '.' 和 "log" 也要求成数字。
// ---------------------------------------------------------------------------
static bool OldIsLogFileName(const std::wstring& n) {
    if (n.size() != 11) return false;
    if (n[4] != L'-') return false;
    for (int i = 0; i < 11; ++i) {
        if (i == 4) continue;
        if (n[i] < L'0' || n[i] > L'9') return false;
    }
    int y = 0, m = 0;
    for (int i = 0; i < 4; ++i) y = y * 10 + (n[i] - L'0');
    for (int i = 5; i < 7; ++i) m = m * 10 + (n[i] - L'0');
    return y >= 2000 && m >= 1 && m <= 12;
}

struct Case {
    const wchar_t* name;
    bool want;
    const char* why;
};

static const Case kCases[] = {
    // ---- 本程序按 %04d-%02d.log 生成的文件，必须认 ----
    { L"2026-11.log",  true,  "当月文件名（v0.4.1 就漏在这一条）" },
    { L"2026-01.log",  true,  "月份补零" },
    { L"2000-01.log",  true,  "年份下界" },
    { L"9999-12.log",  true,  "年份上界" },
    { L"2026-11.LOG",  true,  "扩展名大写：Windows 文件系统不区分大小写" },
    { L"2026-11.Log",  true,  "扩展名混合大小写" },

    // ---- 长度不对 ----
    { L"",                false, "空串" },
    { L"2026-11",         false, "少了扩展名" },
    { L"2026-1.log",      false, "月份没补零" },
    { L"2026-011.log",    false, "多一位" },
    { L"2026-11.log.bak", false, "用户备份文件，不能被清理删掉" },
    { L"2026-11.log ",    false, "尾部空格" },

    // ---- 扩展名 / 分隔符不对 ----
    { L"2026-11.txt",  false, "别的扩展名" },
    { L"2026-11.logg", false, "扩展名多一位" },
    { L"2026_11.log",  false, "分隔符是下划线" },
    { L"2026/11.log",  false, "分隔符是斜杠" },
    { L"2026.11.log",  false, "分隔符是点" },
    { L"notes.log",    false, "用户自己放的文件，不能被清理删掉" },
    { L"PlanPower.ini",false, "配置文件，不能被清理删掉" },

    // ---- 数字位 / 范围 ----
    { L"20x6-11.log",  false, "年份里有字母" },
    { L"2026-x1.log",  false, "月份里有字母" },
    { L"2026-1x.log",  false, "月份第二位有字母" },
    { L"YYYY-MM.log",  false, "纯占位符" },
    { L"1999-12.log",  false, "年份小于 2000" },
    { L"2026-00.log",  false, "月份 0" },
    { L"2026-13.log",  false, "月份 13" },
    { L"2026-99.log",  false, "月份 99" },
    { L"２０２６-11.log", false, "全角数字（size 仍是 11，必须按字符值拒绝）" },
};

typedef bool (*CheckFn)(const std::wstring&);

int main(int argc, char** argv) {
    CheckFn fn = IsLogFileName;
    bool oldMode = (argc > 1 && std::strcmp(argv[1], "--old") == 0);
    if (oldMode) fn = OldIsLogFileName;

    int pass = 0, fail = 0;
    for (const Case& c : kCases) {
        bool got = fn(c.name);
        if (got == c.want) { ++pass; continue; }
        ++fail;
        std::printf("  [FAIL] %s：期望 %s，实际 %s\n", c.why,
                    c.want ? "认" : "不认", got ? "认" : "不认");
    }

    int oldMiss = 0;
    for (const Case& c : kCases)
        if (c.want && !OldIsLogFileName(c.name)) ++oldMiss;

    // 反向验证：旧实现必须至少漏掉一个合法文件名。
    // 只在正常模式下做——如果这组用例连「恒 false」都抓不出来，
    // 那它就是在给自己发合格证。
    if (!oldMode) {
        if (oldMiss > 0) ++pass;
        else { ++fail; std::printf("  [FAIL] 反向验证失效：旧实现竟然认出了全部合法文件名\n"); }
    }

    std::printf("日志文件名判定：通过 %d 项，失败 %d 项%s\n",
                pass, fail, oldMode ? "（旧实现模式，预期失败）" : "");
    if (!oldMode) std::printf("  反向验证：旧实现漏掉 %d 个合法文件名 ✓\n", oldMiss);
    return fail ? 1 : 0;
}
