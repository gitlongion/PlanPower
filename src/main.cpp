// ============================================================================
//  PlanPower - main.cpp  程序入口
//
//  命令行接口分两类：
//   1) 用户直接用的： /cleanup-all 应急清理、/sched-selftest 自检、
//      /dump-xml 导出任务定义
//   2) 提权助手用的： /sched-add /sched-del /sched-enable /sched-disable
//      —— 这些由主进程通过 ShellExecuteEx(runas) 拉起，用退出码回传结果。
//
//  注意：命令行分支必须在「单实例互斥体」之前处理。否则提权进程一启动
//  就会撞上主进程已持有的互斥体，被当成"第二次启动"直接退出。
// ============================================================================
#include "core.h"
#include "ui.h"
#include "sched.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#ifdef _MSC_VER
// MSVC 下自动链接；MinGW 由构建脚本通过 -l 指定
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "taskschd.lib")
#endif

static HANDLE g_hMutex = nullptr;

// 按 exe 所在目录生成互斥体名，实现「不同目录的副本互不干扰」
static std::wstring MakeMutexName() {
    std::wstring dir = ExeDir();
    unsigned int h = 2166136261u;               // FNV-1a
    for (wchar_t c : dir) {
        h ^= (unsigned)towlower(c);
        h *= 16777619u;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"Local\\PlanPower_%08X", h);
    return buf;
}

// ---------------------------------------------------------------------------
//  命令行工具的小工具函数
// ---------------------------------------------------------------------------
static std::wstring IntToStr(int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    return b;
}

static bool SameArg(const std::wstring& a, const wchar_t* b) {
    return lstrcmpiW(a.c_str(), b) == 0;
}

// 把若干行写成 UTF-16LE + BOM 的文本文件。
// 带 BOM 是为了让记事本能正确识别中文，同时这个编码也正好是
// schtasks /create /xml 所要求的格式。
static bool WriteTextFileUtf16(const std::wstring& path,
                               const std::vector<std::wstring>& lines) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const wchar_t bom = 0xFEFF;
    DWORD w = 0;
    WriteFile(h, &bom, sizeof(bom), &w, nullptr);

    for (const auto& l : lines) {
        std::wstring s = l + L"\r\n";
        WriteFile(h, s.c_str(), (DWORD)(s.size() * sizeof(wchar_t)), &w, nullptr);
    }
    CloseHandle(h);
    return true;
}

static int CliError(const wchar_t* title, const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), title, MB_ICONERROR | MB_OK);
    return 1;
}

static int CliInfo(const wchar_t* title, const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), title, MB_ICONINFORMATION | MB_OK);
    return 0;
}

// ---------------------------------------------------------------------------
//  命令行分发
//  返回 true 表示「已作为命令行工具处理完毕」，调用方应直接退出
// ---------------------------------------------------------------------------
static bool RunCliCommand(int argc, wchar_t** argv, int* exitCode) {
    if (argc < 2) return false;

    const std::wstring cmd = argv[1];

    auto argAt = [&](int i) -> std::wstring {
        return (i < argc && argv[i]) ? std::wstring(argv[i]) : std::wstring();
    };

    // ---------------- 应急级清理 ----------------
    // 这一条刻意不读任何配置文件：归属表和 INI 都可能丢失（用户几个月后
    // 换台电脑，想起"以前用 U 盘在旧机器上设过关机任务"），此时唯一还能
    // 依赖的就是「\PlanPower 文件夹」这个约定。
    if (SameArg(cmd, L"/cleanup-all")) {
        std::wstring err;
        int removed = SchedDoCleanupAll(&err);
        if (removed < 0) {
            *exitCode = CliError(L"PlanPower · 清理失败",
                                 L"无法清理系统任务：\n\n" + err +
                                 L"\n\n如果提示权限不足，请右键以管理员身份运行。");
            return true;
        }
        if (removed == 0) {
            *exitCode = CliInfo(L"PlanPower · 清理完成",
                                L"本机没有找到 PlanPower 注册的系统任务，无需清理。");
            return true;
        }
        *exitCode = CliInfo(L"PlanPower · 清理完成",
                            L"已删除 " + IntToStr(removed) +
                            L" 个系统任务，并移除了 \\PlanPower 文件夹。");
        return true;
    }

    // ---------------- 自检 ----------------
    if (SameArg(cmd, L"/sched-selftest")) {
        std::wstring ini = argAt(2);
        if (ini.empty()) {
            bool p = true;
            ini = ResolveConfigPath(&p);
        }
        g_store.Load(ini);

        std::vector<std::wstring> lines;
        SchedSelfTest(&lines);

        std::wstring outPath = JoinPath(ExeDir(), L"PlanPower_selftest.txt");
        bool wrote = WriteTextFileUtf16(outPath, lines);

        // 摘要：只挑几条关键的显示
        std::wstring brief;
        for (const auto& l : lines) {
            if (l.rfind(L"已提权", 0) == 0 ||
                l.rfind(L"任务计划程序服务", 0) == 0 ||
                l.rfind(L"生成成功", 0) == 0 ||
                l.rfind(L"合计 ", 0) == 0) {
                brief += l + L"\n";
            }
        }
        brief += L"\n完整结果已写入：\n" + outPath;
        if (!wrote) brief += L"\n（写入失败，可能介质只读）";

        *exitCode = CliInfo(L"PlanPower · 系统任务自检", brief);
        return true;
    }

    // ---------------- 导出任务 XML ----------------
    // 导出的文件是 UTF-16LE + BOM，可以直接喂给
    //   schtasks /create /xml <文件> /tn "\PlanPower\<名字>"
    // 作为 COM 路径走不通时的兜底手段。
    if (SameArg(cmd, L"/dump-xml")) {
        std::wstring ini = argAt(2);
        std::wstring id  = argAt(3);
        if (ini.empty() || id.empty()) {
            *exitCode = CliError(L"PlanPower",
                L"用法：PlanPower.exe /dump-xml <配置文件路径> <任务ID>");
            return true;
        }
        g_store.Load(ini);

        PowerTask* t = g_store.FindById(id);
        if (!t) {
            *exitCode = CliError(L"PlanPower", L"配置里找不到任务 ID：" + id);
            return true;
        }
        std::wstring xml, err;
        if (!SchedBuildXml(*t, &xml, &err)) {
            *exitCode = CliError(L"PlanPower", L"生成任务定义失败：\n\n" + err);
            return true;
        }

        std::wstring id8 = id.size() >= 8 ? id.substr(0, 8) : id;
        std::wstring outPath = JoinPath(ExeDir(), L"PlanPower_task_" + id8 + L".xml");

        std::vector<std::wstring> lines;
        // 按行拆开写入，行尾统一成 CRLF
        size_t b = 0;
        while (b < xml.size()) {
            size_t e = xml.find(L'\n', b);
            std::wstring line = (e == std::wstring::npos)
                              ? xml.substr(b) : xml.substr(b, e - b);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
                line.pop_back();
            lines.push_back(line);
            if (e == std::wstring::npos) break;
            b = e + 1;
        }

        if (!WriteTextFileUtf16(outPath, lines)) {
            *exitCode = CliError(L"PlanPower", L"写入失败（介质可能只读）：\n" + outPath);
            return true;
        }
        *exitCode = CliInfo(L"PlanPower · 导出任务定义",
            L"已导出到：\n" + outPath +
            L"\n\n系统任务名：\n" + SchedTaskFullPath(*t) +
            L"\n\n该文件为 UTF-16LE + BOM，可直接用\n"
            L"schtasks /create /xml 注册。");
        return true;
    }

    // ---------------- 提权助手：注册 ----------------
    if (SameArg(cmd, L"/sched-add")) {
        std::wstring ini = argAt(2);
        std::wstring id  = argAt(3);
        if (ini.empty() || id.empty()) {
            *exitCode = CliError(L"PlanPower", L"参数不足：/sched-add <配置文件> <任务ID>");
            return true;
        }
        g_store.Load(ini);

        // 先确认配置真的读进来了。读不到就等于后面必然找不到任务，
        // 与其让 SchedDoAdd 报一句含糊的「找不到这个任务」，不如在这里
        // 把「哪个文件、读到几条」直接摊开——这两条信息足以判定是
        // 路径不对，还是父进程的改动还没落盘。
        if (g_store.Tasks().empty()) {
            *exitCode = CliError(L"PlanPower · 注册系统任务失败",
                L"没能从配置文件里读到任何任务，无法注册。\n\n"
                L"配置文件：\n" + ini +
                L"\n\n请确认该文件存在且内容完整。若文件确实存在，"
                L"通常是主程序刚保存完、改动尚未写入磁盘，重试一次即可。");
            return true;
        }

        std::wstring err;
        if (!SchedDoAdd(id, &err)) {
            *exitCode = CliError(L"PlanPower · 注册系统任务失败", err);
            return true;
        }
        // 成功时静默退出：父进程靠退出码判断，不需要再打扰用户一次
        *exitCode = 0;
        return true;
    }

    // ---------------- 提权助手：删除 ----------------
    if (SameArg(cmd, L"/sched-del")) {
        std::wstring name = argAt(2);
        if (name.empty()) {
            *exitCode = CliError(L"PlanPower", L"参数不足：/sched-del <系统任务名>");
            return true;
        }
        std::wstring err;
        if (!SchedDoDelete(name, &err)) {
            *exitCode = CliError(L"PlanPower · 删除系统任务失败", err);
            return true;
        }
        *exitCode = 0;
        return true;
    }

    // ---------------- 提权助手：启用 / 停用 ----------------
    if (SameArg(cmd, L"/sched-enable") || SameArg(cmd, L"/sched-disable")) {
        bool on = SameArg(cmd, L"/sched-enable");
        std::wstring name = argAt(2);
        if (name.empty()) {
            *exitCode = CliError(L"PlanPower", L"参数不足：需要系统任务名");
            return true;
        }
        std::wstring err;
        if (!SchedDoSetEnabled(name, on, &err)) {
            *exitCode = CliError(L"PlanPower · 修改系统任务状态失败", err);
            return true;
        }
        *exitCode = 0;
        return true;
    }

    // ---------------- 提权助手：批量启用 / 停用 ----------------
    // 逐条设置要弹 N 次 UAC，所以「全部暂停」必须合成一次提权。
    if (SameArg(cmd, L"/sched-set-all")) {
        std::wstring ini = argAt(2);
        std::wstring flag = argAt(3);
        if (ini.empty()) {
            *exitCode = CliError(L"PlanPower",
                L"参数不足：/sched-set-all <配置文件> <1|0>");
            return true;
        }
        bool on = (flag == L"1");
        g_store.Load(ini);

        std::wstring err;
        int n = SchedDoSetAllEnabled(on, &err);
        if (n < 0) {
            *exitCode = CliError(L"PlanPower · 批量修改系统任务失败", err);
            return true;
        }
        *exitCode = 0;
        return true;
    }

    return false;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // 让窗口按物理像素绘制，配合代码里的 S() 缩放实现高 DPI 适配
    SetProcessDPIAware();

    // ---- 公共控件 ----
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 命令行 ----
    // trayOnly 单独先解析出来：它决定「窗口建好之后要不要显示」，
    // 而 RunCliCommand 里的分支是「根本不进界面」的另一类。
    bool trayOnly = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            // /tray 由开机自启的 Run 项带过来，是"别弹界面"的唯一信号。
            // 不按"是不是开机时启动的"去猜——猜不准，而且猜错的后果是
            // 用户在登录那一刻被一个窗口挡住。
            for (int i = 1; i < argc; ++i) {
                if (SameArg(argv[i], L"/tray") || SameArg(argv[i], L"-tray"))
                    trayOnly = true;
            }

            int exitCode = 0;
            bool handled = RunCliCommand(argc, argv, &exitCode);
            LocalFree(argv);
            if (handled) {
                CoUninitialize();
                return exitCode;
            }
        }
    }

    // ---- 单实例 ----
    std::wstring mtxName = MakeMutexName();
    g_hMutex = CreateMutexW(nullptr, TRUE, mtxName.c_str());
    if (g_hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"PPMain", nullptr);
        // trayOnly 时**不要**去叫醒已经运行的那一份：开机自启重复触发
        // （比如用户还留着一条旧的手动启动项）不该在登录时弹出一个窗口。
        if (prev && !trayOnly) {
            // 上一份可能正缩在托盘里——用 /tray 启动时窗口从未显示过，
            // 光 SW_RESTORE 不保证能把它带出来，先判一下是不是最小化。
            ShowWindow(prev, IsIconic(prev) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(prev);
        }
        CloseHandle(g_hMutex);
        CoUninitialize();
        return 0;
    }

    // ---- 配置 ----
    bool portable = true;
    std::wstring ini = ResolveConfigPath(&portable);
    g_store.Load(ini);
    g_store.Config().portableMode = portable;

    // 老版本的启动项里没有 /tray，升级后必须对齐一次，否则"开机不弹窗"
    // 这个开关看起来是失效的。只动指向本 exe 的那条，不做别的。
    UI_RepairAutostart();

    // ---- 界面 ----
    if (!UI_Create(hInst, nCmdShow, trayOnly)) {
        MessageBoxW(nullptr, L"窗口创建失败，程序无法启动。", L"PlanPower", MB_ICONERROR);
        CoUninitialize();
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }

    // ---- 消息循环 ----
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (UI_PreTranslate(&msg)) continue;     // 交给编辑窗做 Tab 导航
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    if (g_hMutex) CloseHandle(g_hMutex);
    return (int)msg.wParam;
}
