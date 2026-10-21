// ============================================================================
//  PlanPower - tools/initest.cpp
//  宿主机测试：直接编译真实的 src/inifile.cpp，逐条断言解析与编解码行为。
//
//  存在的理由：inifile.cpp 是纯逻辑（只依赖 <string> / <vector>），而它承担的
//  责任恰恰是上一版出问题的地方——配置写下去、提权助手进程立刻要读到。
//  这一层不出错，才有资格谈「跨进程配置可见性」。
//
//  编译（macOS / Linux）：
//    clang++ -std=c++17 -O1 -I tools/hostshim tools/initest.cpp src/inifile.cpp \
//            -o build/initest && ./build/initest
// ============================================================================
#include "inifile.h"

#include <cstdio>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  断言框架
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void Check(bool cond, const char* what) {
    if (cond) { ++g_pass; return; }
    ++g_fail;
    std::printf("  [FAIL] %s\n", what);
}

static void CheckEq(const std::wstring& got, const std::wstring& want, const char* what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("  [FAIL] %s\n", what);
    std::printf("         期望：[%ls]\n", want.c_str());
    std::printf("         实际：[%ls]\n", got.c_str());
}

static void CheckEqI(int got, int want, const char* what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("  [FAIL] %s  （期望 %d，实际 %d）\n", what, want, got);
}

static void CheckEqS(size_t got, size_t want, const char* what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("  [FAIL] %s  （期望 %zu，实际 %zu）\n", what, want, got);
}

static std::wstring BytesToHex(const std::vector<unsigned char>& b) {
    static const wchar_t* H = L"0123456789ABCDEF";
    std::wstring r;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) r += L' ';
        r += H[(b[i] >> 4) & 0xF];
        r += H[b[i] & 0xF];
    }
    return r;
}

// ---------------------------------------------------------------------------
//  A. 解析基础
// ---------------------------------------------------------------------------
static void TestParseBasics() {
    std::printf("A. 解析基础\n");

    const std::wstring txt =
        L"; PlanPower 配置与任务定义\r\n"
        L"; [NotASection] 注释里的方括号不算段头\r\n"
        L"# 井号也是注释\r\n"
        L"\r\n"
        L"[General]\r\n"
        L"StartWithWindows=0\r\n"
        L"AllowSystemTasks = 1 \r\n"
        L"\r\n"
        L"[Task.0]\r\n"
        L"name=夜间关机\r\n"
        L"message=22:30 关机，注意保存 = 一下\r\n"
        L"delay=1800\r\n";

    std::vector<IniEntry> doc = IniParse(txt);

    CheckEq(IniGet(doc, L"General", L"StartWithWindows", L"?"), L"0",
            "取到 StartWithWindows");
    CheckEq(IniGet(doc, L"general", L"startwithwindows", L"?"), L"0",
            "段名与键名大小写不敏感");
    CheckEq(IniGet(doc, L"GENERAL", L"ALLOWSYSTEMTASKS", L"?"), L"1",
            "行尾空白被裁掉");
    CheckEq(IniGet(doc, L"Task.0", L"name", L"?"), L"夜间关机",
            "取到中文值");
    // 值里第一个 = 之后的 = 必须原样保留：关机提示文本完全可能出现等号
    CheckEq(IniGet(doc, L"Task.0", L"message", L"?"),
            L"22:30 关机，注意保存 = 一下", "值里的等号原样保留");
    CheckEqI(IniGetInt(doc, L"Task.0", L"delay", 0), 1800, "整数取值");

    std::vector<std::wstring> secs = IniSections(doc);
    CheckEqS(secs.size(), 2, "只有 2 个段（注释行里的方括号不算段）");
    if (secs.size() == 2) {
        CheckEq(secs[0], L"General", "段顺序 1 = General");
        CheckEq(secs[1], L"Task.0",  "段顺序 2 = Task.0");
    }

    CheckEq(IniGet(doc, L"Task.0", L"nosuchkey", L"兜底"), L"兜底",
            "键不存在时返回兜底值");
    CheckEq(IniGet(doc, L"NoSuchSection", L"x", L"兜底"), L"兜底",
            "段不存在时返回兜底值");
}

// ---------------------------------------------------------------------------
//  B. 换行与空白兼容
// ---------------------------------------------------------------------------
static void TestNewlines() {
    std::printf("B. 换行与空白兼容\n");

    // 纯 LF
    {
        std::vector<IniEntry> d = IniParse(L"[X]\na=1\nb=2\n");
        CheckEqS(d.size(), 2, "LF 换行：2 条键值");
        CheckEq(IniGet(d, L"X", L"b", L"?"), L"2", "LF 换行：取到 b");
    }
    // 纯 CR（老式 Mac / 某些编辑器）
    {
        std::vector<IniEntry> d = IniParse(L"[X]\ra=1\rb=2\r");
        CheckEqS(d.size(), 2, "CR 换行：2 条键值");
        CheckEq(IniGet(d, L"X", L"b", L"?"), L"2", "CR 换行：取到 b");
    }
    // 最后一行没有换行符
    {
        std::vector<IniEntry> d = IniParse(L"[X]\r\na=1");
        CheckEqS(d.size(), 1, "无结尾换行：仍然解析出最后一条");
        CheckEq(IniGet(d, L"X", L"a", L"?"), L"1", "无结尾换行：取到 a");
    }
    // 连续空行 + 行首缩进
    {
        std::vector<IniEntry> d = IniParse(L"\r\n\r\n   [X]   \r\n\r\n\t a \t=\t 1 \t\r\n\r\n");
        CheckEqS(d.size(), 1, "空行与缩进：只解析出 1 条");
        CheckEq(IniGet(d, L"X", L"a", L"?"), L"1", "缩进被裁掉");
    }
    // CRLF 不能被当成两个换行（否则会多出一堆空段/空行）
    {
        std::vector<IniEntry> d = IniParse(L"[X]\r\na=1\r\n\r\n[Y]\r\nb=2\r\n");
        CheckEqS(d.size(), 2, "CRLF 只切一次，不产生多余条目");
        CheckEqS(IniSections(d).size(), 2, "CRLF：段数正确");
    }
}

// ---------------------------------------------------------------------------
//  C. 段头之前的散落键值对
// ---------------------------------------------------------------------------
static void TestStrayEntries() {
    std::printf("C. 段头之前的散落键值对\n");

    std::vector<IniEntry> d = IniParse(L"stray=1\r\n[X]\r\na=2\r\n");
    CheckEqS(d.size(), 2, "散落键值对也被保留");
    if (d.size() == 2) {
        CheckEq(d[0].section, L"", "散落条目的段名为空");
        CheckEq(d[1].section, L"X", "段内条目的段名为 X");
    }
    CheckEqS(IniSections(d).size(), 1, "段列表里不含空段");
}

// ---------------------------------------------------------------------------
//  D. 畸形行
// ---------------------------------------------------------------------------
static void TestMalformed() {
    std::printf("D. 畸形行\n");

    std::vector<IniEntry> d = IniParse(
        L"[NoClose\r\n"          // 段头没有 ]
        L"keyWithoutEquals\r\n"  // 没有 =
        L"=valueOnly\r\n"        // 键为空
        L"[OK]\r\n"
        L"k=v\r\n");

    CheckEqS(d.size(), 1, "畸形行全部跳过，只留 1 条");
    CheckEq(IniGet(d, L"OK", L"k", L"?"), L"v", "畸形行之后的正常内容仍能取到");
}

// ---------------------------------------------------------------------------
//  E. 重复键 / 重复段
// ---------------------------------------------------------------------------
static void TestDuplicates() {
    std::printf("E. 重复键与重复段\n");

    std::vector<IniEntry> d = IniParse(L"[X]\r\na=1\r\na=2\r\n");
    CheckEq(IniGet(d, L"X", L"a", L"?"), L"2", "同名键以最后一个为准");

    std::vector<IniEntry> d2 = IniParse(L"[X]\r\na=1\r\n[Y]\r\nb=2\r\n[X]\r\nc=3\r\n");
    CheckEq(IniGet(d2, L"X", L"c", L"?"), L"3", "段重现后新键仍属于该段");
    CheckEqS(IniSections(d2).size(), 2, "段列表按首次出现去重");

    std::vector<std::wstring> secs = IniSections(d2);
    if (secs.size() == 2) {
        CheckEq(secs[0], L"X", "去重后顺序保持首次出现");
        CheckEq(secs[1], L"Y", "去重后顺序保持首次出现（2）");
    }

    std::vector<std::pair<std::wstring, std::wstring>> es = IniSectionEntries(d2, L"x");
    CheckEqS(es.size(), 2, "段内条目按出现顺序全部返回（含重复键）");
    if (es.size() == 2) {
        CheckEq(es[0].first,  L"a", "条目顺序 1");
        CheckEq(es[1].first,  L"c", "条目顺序 2");
    }
}

// ---------------------------------------------------------------------------
//  F. 整数解析
// ---------------------------------------------------------------------------
static void TestInts() {
    std::printf("F. 整数解析\n");

    // 每行一个用例，键名 k
    struct { const wchar_t* v; int want; const char* what; } cases[] = {
        { L"0",             0,          "0" },
        { L"1800",          1800,       "普通正整数" },
        { L"-5",            -5,         "负整数" },
        { L"+7",            7,          "显式正号" },
        { L"  42  ",        42,         "前后空白" },
        { L"12abc",         12,         "数字前缀后跟垃圾字符，取前缀" },
        { L"2147483647",    2147483647, "INT_MAX" },
        { L"999999999999",  2147483647, "溢出钳到 INT_MAX，不绕回负数" },
    };
    for (const auto& c : cases) {
        std::wstring t = std::wstring(L"[S]\r\nk=") + c.v + L"\r\n";
        std::vector<IniEntry> d = IniParse(t);
        CheckEqI(IniGetInt(d, L"S", L"k", -999), c.want, c.what);
    }

    // 非数字起始 → 返回兜底值，而不是 0。
    // 这正是当初不用 _wtoi 的原因：_wtoi("abc") 也是 0，会把「文件被改坏」
    // 和「用户真的填了 0」混成同一种结果。
    {
        std::vector<IniEntry> d = IniParse(L"[S]\r\nk=abc\r\n");
        CheckEqI(IniGetInt(d, L"S", L"k", -999), -999, "非数字返回兜底值（不是 0）");
    }
    {
        std::vector<IniEntry> d = IniParse(L"[S]\r\nk=\r\n");
        CheckEqI(IniGetInt(d, L"S", L"k", -999), -999, "空值返回兜底值");
    }
    {
        std::vector<IniEntry> d = IniParse(L"[S]\r\nk=-abc\r\n");
        CheckEqI(IniGetInt(d, L"S", L"k", -999), -999, "只有负号也返回兜底值");
    }
    {
        std::vector<IniEntry> d = IniParse(L"[S]\r\n");
        CheckEqI(IniGetInt(d, L"S", L"missing", -999), -999, "键不存在返回兜底值");
    }
}

// ---------------------------------------------------------------------------
//  G. 大小写工具
// ---------------------------------------------------------------------------
static void TestCaseTools() {
    std::printf("G. 大小写工具\n");

    CheckEq(IniLower(L"Task.0AbC"), L"task.0abc", "IniLower 折叠 ASCII 大写");
    CheckEq(IniLower(L"夜间关机"), L"夜间关机", "IniLower 不动非 ASCII");

    Check(IniStartsWithNoCase(L"Host.ABCD1234", L"Host."), "Host. 前缀匹配");
    Check(IniStartsWithNoCase(L"HOST.abcd",     L"Host."), "前缀匹配不区分大小写");
    Check(!IniStartsWithNoCase(L"Hos",          L"Host."), "太短不算匹配");
    Check(!IniStartsWithNoCase(L"Task.0",       L"Host."), "不同前缀不算匹配");
    Check(IniStartsWithNoCase(L"",              L""),      "空前缀恒为真");
    Check(!IniStartsWithNoCase(L"",             L"Host."), "空串不匹配非空前缀");
}

// ---------------------------------------------------------------------------
//  H. 编解码
// ---------------------------------------------------------------------------
static void TestCodec() {
    std::printf("H. 编解码\n");

    // ---- 写：UTF-16LE + BOM ----
    {
        std::vector<unsigned char> b;
        TextToUtf16Le(L"中文abc", &b);          // 5 个字符
        CheckEqS(b.size(), 2 + 5 * 2, "UTF-16LE 字节数 = BOM + 字符数 × 2");
        Check(b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE, "带 FF FE BOM（记事本才认）");
        // 第 3 个字符是 'a'，落在偏移 2 + 2×2 = 6
        Check(b.size() >= 8 && b[6] == 0x61 && b[7] == 0x00, "ASCII 按小端存放");
        // 第 1 个字符是「中」U+4E2D，低字节在前
        Check(b.size() >= 4 && b[2] == 0x2D && b[3] == 0x4E, "中文按小端存放");
    }

    // ---- 读：UTF-16LE 往返 ----
    {
        std::vector<unsigned char> b;
        TextToUtf16Le(L"中文abc｜任务 0", &b);
        std::wstring s;
        Check(TextFromBytes(b, &s), "UTF-16LE 解码成功");
        CheckEq(s, L"中文abc｜任务 0", "UTF-16LE 往返一致");
    }

    // ---- 读：UTF-16BE ----
    {
        // "AB" 大端
        std::vector<unsigned char> b = { 0xFE, 0xFF, 0x00, 0x41, 0x00, 0x42 };
        std::wstring s;
        Check(TextFromBytes(b, &s), "UTF-16BE 解码成功");
        CheckEq(s, L"AB", "UTF-16BE 内容正确");
    }

    // ---- 读：UTF-8 带 BOM ----
    {
        std::vector<unsigned char> b = { 0xEF, 0xBB, 0xBF, 'a', 'b' };
        std::wstring s;
        Check(TextFromBytes(b, &s), "UTF-8 + BOM 解码成功");
        CheckEq(s, L"ab", "UTF-8 + BOM：BOM 不进内容");
    }

    // ---- 读：UTF-8 无 BOM（含中文）----
    {
        // 中 = E4 B8 AD，文 = E6 96 87
        std::vector<unsigned char> b = { 0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87 };
        std::wstring s;
        Check(TextFromBytes(b, &s), "UTF-8 无 BOM 解码成功");
        CheckEq(s, L"中文", "UTF-8 无 BOM 内容正确");
    }

    // ---- 读：纯 ASCII 无 BOM ----
    {
        std::vector<unsigned char> b = { 'h', 'e', 'l', 'l', 'o' };
        std::wstring s;
        Check(TextFromBytes(b, &s), "纯 ASCII 无 BOM 解码成功");
        CheckEq(s, L"hello", "纯 ASCII 内容正确");
    }

    // ---- 读：空输入 ----
    {
        std::vector<unsigned char> b;
        std::wstring s;
        Check(TextFromBytes(b, &s), "空输入视为成功");
        CheckEq(s, L"", "空输入产出空串");
    }

    // ---- 读：UTF-16LE 半截字符（字节数为奇数）应被丢弃而不是越界 ----
    {
        std::vector<unsigned char> b = { 0xFF, 0xFE, 0x61 };
        std::wstring s;
        Check(TextFromBytes(b, &s), "奇数长度的 UTF-16LE 不崩");
        CheckEq(s, L"", "奇数长度的 UTF-16LE 丢弃半截字符");
    }

    // ---- 严格 UTF-8：非法序列必须返回 false，好让调用方退回 ANSI ----
    {
        struct { std::vector<unsigned char> b; const char* what; } bad[] = {
            { { 0x80 },                         "孤立的续接字节" },
            { { 0xC0, 0x80 },                   "过长编码 U+0000" },
            { { 0xC1, 0xBF },                   "过长编码 U+007F" },
            { { 0xE0, 0x80, 0x80 },             "过长编码 U+0000（三字节）" },
            { { 0xED, 0xA0, 0x80 },             "UTF-8 里出现代理区码点 U+D800" },
            { { 0xF5, 0x80, 0x80, 0x80 },       "超出 U+10FFFF" },
            { { 0xE4, 0xB8 },                   "三字节序列被截断" },
            { { 0xF0, 0x9F, 0x98 },             "四字节序列被截断" },
            { { 0xC3, 0x28 },                   "第二字节不是续接字节" },
        };
        for (const auto& c : bad) {
            std::wstring s;
            Check(!TextFromBytes(c.b, &s), c.what);
        }
    }

    // ---- 非 BMP 码点：宿主与 Windows 的字节输出必须一致 ----
    if (sizeof(wchar_t) > 2) {
        std::wstring s;
        s += (wchar_t)0x1F600;                       // 😀
        std::vector<unsigned char> b;
        TextToUtf16Le(s, &b);
        // 0x1F600 - 0x10000 = 0xF600 → 高代理 D83D、低代理 DE00
        std::vector<unsigned char> want = { 0xFF, 0xFE, 0x3D, 0xD8, 0x00, 0xDE };
        CheckEq(BytesToHex(b), BytesToHex(want), "非 BMP 码点拆成代理对，字节与 Windows 一致");
    }

    // ---- 非 BMP：UTF-8 四字节解码 ----
    {
        std::vector<unsigned char> b = { 0xF0, 0x9F, 0x98, 0x80 };   // 😀
        std::wstring s;
        Check(TextFromBytes(b, &s), "四字节 UTF-8 解码成功");
        if (sizeof(wchar_t) > 2) {
            CheckEqS(s.size(), 1, "宿主（4 字节 wchar_t）得到 1 个码点");
            if (s.size() == 1) Check(s[0] == (wchar_t)0x1F600, "码点值为 U+1F600");
        } else {
            CheckEqS(s.size(), 2, "Windows（2 字节 wchar_t）得到代理对");
        }
    }
}

// ---------------------------------------------------------------------------
//  I. 模拟真实配置文件：写完再读回来
// ---------------------------------------------------------------------------
static void TestRealisticRoundTrip() {
    std::printf("I. 真实配置往返\n");

    // 这段文本刻意照抄 core.cpp 的 TaskStore::Save() 输出格式，
    // 包括头部三行 ; 注释、段之间的空行、字段顺序。
    const std::wstring text =
        L"; PlanPower 配置与任务定义\r\n"
        L"; 可以直接用记事本编辑。删掉本文件等同于恢复出厂设置。\r\n"
        L"; 本文件由程序整体重写，手工添加的注释不会保留。\r\n"
        L"\r\n"
        L"[General]\r\n"
        L"StartWithWindows=0\r\n"
        L"KeepLog=0\r\n"
        L"AllowSystemTasks=1\r\n"
        L"\r\n"
        L"[Task.0]\r\n"
        L"id=9F2C1A4B7E8D0356\r\n"
        L"name=夜间关机\r\n"
        L"action=0\r\n"
        L"kind=1\r\n"
        L"delay=1800\r\n"
        L"hour=23\r\n"
        L"minute=30\r\n"
        L"weekdays=31\r\n"
        L"notice=60\r\n"
        L"force=1\r\n"
        L"message=系统即将关机；请及时保存（含 = 号的文本）\r\n"
        L"enabled=1\r\n"
        L"persistent=1\r\n"
        L"\r\n"
        L"[Task.1]\r\n"
        L"id=1122334455667788\r\n"
        L"name=每周重启\r\n"
        L"action=1\r\n"
        L"kind=3\r\n"
        L"delay=300\r\n"
        L"hour=4\r\n"
        L"minute=0\r\n"
        L"weekdays=85\r\n"
        L"notice=120\r\n"
        L"force=0\r\n"
        L"message=\r\n"
        L"enabled=0\r\n"
        L"persistent=0\r\n"
        L"\r\n"
        L"[Host.A1B2C3D4]\r\n"
        L"9F2C1A4B7E8D0356=夜间关机_1A2B_9F2C1A4B\r\n";

    // 先把文本编码成字节再解回来，模拟真实的「写盘 → 另一进程读盘」链路
    std::vector<unsigned char> bytes;
    TextToUtf16Le(text, &bytes);
    std::wstring back;
    Check(TextFromBytes(bytes, &back), "配置文本编解码往返成功");
    CheckEq(back, text, "配置文本逐字符一致");

    std::vector<IniEntry> doc = IniParse(back);

    CheckEqI(IniGetInt(doc, L"General", L"AllowSystemTasks", 0), 1, "总开关读回 1");
    CheckEqI(IniGetInt(doc, L"General", L"KeepLog", 9), 0, "KeepLog 读回 0");

    CheckEq(IniGet(doc, L"Task.0", L"id", L""), L"9F2C1A4B7E8D0356", "Task.0 id");
    CheckEq(IniGet(doc, L"Task.0", L"name", L""), L"夜间关机", "Task.0 中文名");
    CheckEqI(IniGetInt(doc, L"Task.0", L"hour", -1), 23, "Task.0 hour");
    CheckEqI(IniGetInt(doc, L"Task.0", L"minute", -1), 30, "Task.0 minute");
    CheckEqI(IniGetInt(doc, L"Task.0", L"persistent", -1), 1, "Task.0 persistent");
    // 提示文本里的分号与等号都必须活着回来
    CheckEq(IniGet(doc, L"Task.0", L"message", L""),
            L"系统即将关机；请及时保存（含 = 号的文本）", "Task.0 message 完整保留");

    CheckEq(IniGet(doc, L"Task.1", L"name", L""), L"每周重启", "Task.1 名称");
    CheckEqI(IniGetInt(doc, L"Task.1", L"enabled", -1), 0, "Task.1 已停用");
    CheckEq(IniGet(doc, L"Task.1", L"message", L"非空"), L"",
            "Task.1 message 是空值（空值不吞掉）");

    std::vector<std::wstring> secs = IniSections(doc);
    CheckEqS(secs.size(), 4, "段数 = General + 2 个任务 + 1 张归属表");
    if (secs.size() == 4) {
        CheckEq(secs[0], L"General",    "段顺序 1");
        CheckEq(secs[1], L"Task.0",     "段顺序 2（任务顺序必须稳定）");
        CheckEq(secs[2], L"Task.1",     "段顺序 3");
        CheckEq(secs[3], L"Host.A1B2C3D4", "段顺序 4（归属表在最后）");
    }

    // 归属表：任务ID → 系统任务名
    std::vector<std::pair<std::wstring, std::wstring>> hosts =
        IniSectionEntries(doc, L"Host.A1B2C3D4");
    CheckEqS(hosts.size(), 1, "归属表 1 条");
    if (hosts.size() == 1) {
        CheckEq(hosts[0].first,  L"9F2C1A4B7E8D0356", "归属表键 = 任务ID");
        CheckEq(hosts[0].second, L"夜间关机_1A2B_9F2C1A4B", "归属表值 = 系统任务名");
    }

    // 段名匹配必须大小写不敏感：Save() 写的是 "Host."，Load() 查的也是 "Host."，
    // 但用户手改成 "host." 之后仍要能读出来。
    Check(IniStartsWithNoCase(L"Host.A1B2C3D4", L"Host."), "归属表段名可被识别");
    Check(IniStartsWithNoCase(secs.empty() ? L"" : secs[3], L"Host."),
          "识别出的归属表段名确实以 Host. 开头");
}

// ---------------------------------------------------------------------------
//  J. 旧版文件兼容（Prof* API 写出来的样子）
// ---------------------------------------------------------------------------
static void TestLegacyCompat() {
    std::printf("J. 旧版文件兼容\n");

    // WritePrivateProfileString 写出来的文件也是 UTF-16LE + BOM，
    // 但结尾常常多一个空行，且键名大小写是原样保留的。必须照样能读。
    const std::wstring text =
        L"[General]\r\n"
        L"StartWithWindows=0\r\n"
        L"\r\n"
        L"[Task.0]\r\n"
        L"ID=ABCDEF0123456789\r\n"      // 注意是大写 ID
        L"Name=旧文件\r\n"
        L"\r\n";

    std::vector<IniEntry> doc = IniParse(text);
    CheckEq(IniGet(doc, L"Task.0", L"id", L""), L"ABCDEF0123456789",
            "键名大小写不同也能读到（ID vs id）");
    CheckEq(IniGet(doc, L"Task.0", L"name", L""), L"旧文件", "旧文件的中文值");
    CheckEqS(IniSections(doc).size(), 2, "旧文件段数正确");
}

// ---------------------------------------------------------------------------
int main() {
    std::printf("PlanPower · INI 解析与编解码测试\n");
    std::printf("================================\n");

    TestParseBasics();
    TestNewlines();
    TestStrayEntries();
    TestMalformed();
    TestDuplicates();
    TestInts();
    TestCaseTools();
    TestCodec();
    TestRealisticRoundTrip();
    TestLegacyCompat();

    std::printf("================================\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
