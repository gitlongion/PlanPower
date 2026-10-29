// ============================================================================
//  PlanPower - core.cpp  核心层实现
// ============================================================================
#include "core.h"
#include "inifile.h"        // INI 解析 / UTF-16LE 编解码（纯逻辑，可在宿主机上测）
#include "dateutil.h"       // 闰年 / 月份天数（纯头，与 sched_xml 共用同一份实现）

#include <shlobj.h>
#include <powrprof.h>
#include <objbase.h>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <vector>

TaskStore g_store;

// 关机原因码：MAJOR_APPLICATION | FLAG_PLANNED
// （直接用字面量，避免不同 SDK 的常量定义差异）
static const DWORD kShutdownReason = 0x80040000u;

#ifndef SC_MONITORPOWER
#define SC_MONITORPOWER 0xF170
#endif

// ===========================================================================
//  路径与便携性
// ===========================================================================

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) return L".\\";
    // 去掉文件名，保留目录并确保以反斜杠结尾
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *(slash + 1) = L'\0';
    else wcscat(buf, L"\\");
    return buf;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    // 跳过 b 开头的多余反斜杠
    size_t i = 0;
    while (i < b.size() && (b[i] == L'\\' || b[i] == L'/')) ++i;
    r += b.substr(i);
    return r;
}

bool DirIsWritable(const std::wstring& dir) {
    std::wstring probe = JoinPath(dir, L".pp_writetest.tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool IsRemovableDrive(const std::wstring& path) {
    if (path.size() < 3) return false;
    std::wstring root = path.substr(0, 3);      // "E:\"
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOVABLE;
}

bool PortableFlagExists() {
    std::wstring flag = JoinPath(ExeDir(), L"portable.flag");
    return GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ResolveConfigPath(bool* outPortable) {
    std::wstring dir = ExeDir();

    // ① 强制便携：存在 portable.flag，或命令行带 /portable
    bool forced = PortableFlagExists();
    if (!forced) {
        const wchar_t* cmd = GetCommandLineW();
        if (cmd && (wcsstr(cmd, L"/portable") || wcsstr(cmd, L"-portable"))) forced = true;
    }

    // ② 同目录可写 → 便携模式
    if (forced || DirIsWritable(dir)) {
        if (outPortable) *outPortable = true;
        return JoinPath(dir, L"PlanPower.ini");
    }

    // ③ 降级到 %LOCALAPPDATA% 下的 PlanPower 目录
    wchar_t local[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH * 2) > 0) {
        std::wstring d = JoinPath(local, L"PlanPower");
        CreateDirectoryW(d.c_str(), nullptr);
        if (outPortable) *outPortable = false;
        return JoinPath(d, L"PlanPower.ini");
    }

    if (outPortable) *outPortable = false;
    return JoinPath(dir, L"PlanPower.ini");
}

// ===========================================================================
//  配置文件读写
//
//  这里刻意不用 kernel32 的 GetPrivateProfileString / WritePrivateProfileString。
//  那一套 API 会在进程内缓存整个文件的映射，写入先进缓存、延迟落盘。而
//  PlanPower 保存配置后会**立刻**启动提权助手进程去读同一个 INI——助手读到的
//  是旧内容（甚至文件还不存在），于是报「配置里找不到这个任务」，症状还伪装成
//  「注册系统任务失败」。更麻烦的是缓存里可能还留着上一版的陈旧键值，让
//  「先删文件再重写」这个手法失效，已删除任务的段会自己长回来。
//
//  所以改成自己读写：写的时候先写临时文件、FlushFileBuffers，再原子替换。
//  格式仍是 INI，记事本照样能改。解析与编解码在 inifile.cpp 里，是纯逻辑，
//  可以在宿主机上单独跑测试。
// ===========================================================================

// 读取整个文件。文件不存在返回 false（首次运行，不算错误）。
static bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>* out) {
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (64LL << 20)) {
        CloseHandle(h);                  // 大于 64MB 的一定不是我们的配置文件
        return false;
    }
    out->resize((size_t)sz.QuadPart);

    size_t total = 0;
    while (total < out->size()) {
        DWORD got = 0;
        DWORD want = (DWORD)((out->size() - total) > (1u << 20)
                             ? (1u << 20) : (out->size() - total));
        if (!ReadFile(h, out->data() + total, want, &got, nullptr) || got == 0) break;
        total += got;
    }
    out->resize(total);
    CloseHandle(h);
    return true;
}

// 原子写：临时文件 → 刷盘 → 替换。
// 「刷盘」这一步是关键，不能省——紧接着启动的提权助手进程要从磁盘读它。
static bool WriteAllBytesAtomic(const std::wstring& path,
                                const std::vector<unsigned char>& bytes) {
    std::wstring tmp = path + L".tmp";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    size_t off = 0;
    while (off < bytes.size()) {
        DWORD wrote = 0;
        DWORD want = (DWORD)((bytes.size() - off) > (1u << 20)
                             ? (1u << 20) : (bytes.size() - off));
        if (!WriteFile(h, bytes.data() + off, want, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    if (ok) ok = (FlushFileBuffers(h) != FALSE);
    CloseHandle(h);

    if (!ok) { DeleteFileW(tmp.c_str()); return false; }

    // 原子替换：读者要么看到旧的完整文件，要么看到新的完整文件，
    // 不会撞上写了一半的内容。MOVEFILE_WRITE_THROUGH 保证替换本身也已落盘。
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// 不是 UTF-16 / UTF-8 时，按本地 ANSI 代码页再试一次。
// 覆盖「用户用记事本另存为 ANSI」这种情况，不追求完美还原。
static std::wstring AnsiToWide(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return L"";
    int need = MultiByteToWideChar(CP_ACP, 0,
                                   (const char*)bytes.data(), (int)bytes.size(),
                                   nullptr, 0);
    if (need <= 0) return L"";
    std::wstring s((size_t)need, L'\0');
    MultiByteToWideChar(CP_ACP, 0, (const char*)bytes.data(), (int)bytes.size(),
                        &s[0], need);
    return s;
}

// 读取并解码配置文件
static std::wstring ReadConfigText(const std::wstring& path, bool* exists) {
    std::vector<unsigned char> bytes;
    if (!ReadAllBytes(path, &bytes)) {
        if (exists) *exists = false;
        return L"";
    }
    if (exists) *exists = true;

    std::wstring text;
    if (!TextFromBytes(bytes, &text)) text = AnsiToWide(bytes);
    return text;
}

// ===========================================================================
//  机器与介质标识
// ===========================================================================

// FNV-1a（宽字符），大小写不敏感
static unsigned int FnvHash(const std::wstring& s, bool foldCase = false) {
    unsigned int h = 2166136261u;
    for (wchar_t c : s) {
        wchar_t x = foldCase ? (wchar_t)towlower(c) : c;
        h ^= (unsigned)(unsigned short)x;
        h *= 16777619u;
    }
    return h;
}

// 取某路径所在卷的序列号；失败返回 0
static DWORD VolumeSerialOf(const std::wstring& path) {
    if (path.size() < 3) return 0;
    std::wstring root = path.substr(0, 3);          // "C:\"
    DWORD serial = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial,
                               nullptr, nullptr, nullptr, 0))
        return 0;
    return serial;
}

std::wstring MachineFingerprint() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) name[0] = L'\0';

    wchar_t winDir[MAX_PATH * 2] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH * 2);
    DWORD serial = VolumeSerialOf(winDir);

    wchar_t buf[128];
    swprintf(buf, 128, L"%ls|%08X", name, (unsigned)serial);
    unsigned int h = FnvHash(buf, true);

    wchar_t out[16];
    swprintf(out, 16, L"%08X", h);
    return out;
}

std::wstring MachineShortName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) return L"未知设备";
    return name;
}

std::wstring DriveTag() {
    DWORD serial = VolumeSerialOf(ExeDir());
    unsigned int h;
    if (serial != 0) {
        h = serial;                                  // 卷序列号本身就是稳定标识
    } else {
        h = FnvHash(ExeDir(), true);                 // 网络路径等：退化为目录哈希
    }
    wchar_t out[8];
    swprintf(out, 8, L"%04X", (unsigned)(h & 0xFFFFu));
    return out;
}

// ===========================================================================
//  TaskStore
// ===========================================================================

bool TaskStore::Load(const std::wstring& iniPath) {
    path_ = iniPath;
    tasks_.clear();
    hosts_.clear();
    cfg_ = AppConfig();

    bool exists = false;
    std::wstring text = ReadConfigText(path_, &exists);
    if (!exists) return false;           // 首次运行
    if (text.empty()) return true;       // 空文件：全用默认值

    std::vector<IniEntry> doc = IniParse(text);

    cfg_.startWithWindows = IniGetInt(doc, L"General", L"StartWithWindows", 0) != 0;
    cfg_.keepLog          = IniGetInt(doc, L"General", L"KeepLog", 0) != 0;
    cfg_.allowSystemTasks = IniGetInt(doc, L"General", L"AllowSystemTasks", 0) != 0;
    // 缺省值给 1：老配置文件里没有这一项，而升级后的用户正是想要
    // 「开机别再弹窗」的那批人——默认给 false 等于让升级变成空操作。
    cfg_.trayOnAutostart  = IniGetInt(doc, L"General", L"TrayOnAutostart", 1) != 0;
    cfg_.logKeepMonths    = IniGetInt(doc, L"General", L"LogKeepMonths", 6);
    cfg_.quickMinutes[0]  = IniGetInt(doc, L"General", L"Quick1", 30);
    cfg_.quickMinutes[1]  = IniGetInt(doc, L"General", L"Quick2", 60);
    cfg_.quickMinutes[2]  = IniGetInt(doc, L"General", L"Quick3", 120);
    // 夹到合法范围：INI 是可以手改的，菜单里出现「0 分钟后关机」比不给改更糟。
    for (int i = 0; i < 3; ++i) {
        if (cfg_.quickMinutes[i] < 1)       cfg_.quickMinutes[i] = 1;
        if (cfg_.quickMinutes[i] > 24 * 60) cfg_.quickMinutes[i] = 24 * 60;
    }
    if (cfg_.logKeepMonths < 0)    cfg_.logKeepMonths = 0;
    if (cfg_.logKeepMonths > 120)  cfg_.logKeepMonths = 120;

    // 按段名逐个处理，顺序即文件中的出现顺序，任务的排列因此保持稳定
    for (const std::wstring& sec : IniSections(doc)) {
        // ---- 宿主机归属表： [Host.<指纹>] 下是 任务ID=系统任务名 ----
        if (IniStartsWithNoCase(sec, L"Host.")) {
            std::wstring host = sec.substr(5);
            if (host.empty()) continue;
            for (const auto& kv : IniSectionEntries(doc, sec)) {
                if (!kv.first.empty() && !kv.second.empty())
                    hosts_[host][kv.first] = kv.second;
            }
            continue;
        }

        if (!IniStartsWithNoCase(sec, L"Task.")) continue;

        PowerTask t;
        t.id            = IniGet(doc, sec, L"id", L"");
        t.name          = IniGet(doc, sec, L"name", L"未命名任务");
        t.action        = (PowerAction)IniGetInt(doc, sec, L"action", 0);
        t.kind          = (TriggerKind)IniGetInt(doc, sec, L"kind", 0);
        t.delaySeconds  = IniGetInt(doc, sec, L"delay", 1800);
        t.hour          = IniGetInt(doc, sec, L"hour", 23);
        t.minute        = IniGetInt(doc, sec, L"minute", 0);
        t.year          = IniGetInt(doc, sec, L"year", 0);
        t.month         = IniGetInt(doc, sec, L"month", 0);
        t.day           = IniGetInt(doc, sec, L"day", 0);
        t.weekdayMask   = (unsigned)IniGetInt(doc, sec, L"weekdays", 0x1F);
        t.noticeSeconds = IniGetInt(doc, sec, L"notice", 60);
        t.forceClose    = IniGetInt(doc, sec, L"force", 0) != 0;
        t.message       = IniGet(doc, sec, L"message", L"");
        t.enabled       = IniGetInt(doc, sec, L"enabled", 1) != 0;
        t.persistent    = IniGetInt(doc, sec, L"persistent", 0) != 0;

        if (t.id.empty()) t.id = NewGuid();
        if (t.weekdayMask == 0) t.weekdayMask = 0x1F;

        tasks_.push_back(t);
    }
    return true;
}

bool TaskStore::Save() {
    if (path_.empty()) return false;

    // 整个文件在内存里拼好再一次性写。字段顺序与旧版本一致，
    // 手工改过这个文件的人不会觉得格式变了。
    std::wstring s;
    s.reserve(4096);

    s += L"; PlanPower 配置与任务定义\r\n";
    s += L"; 可以直接用记事本编辑。删掉本文件等同于恢复出厂设置。\r\n";
    s += L"; 本文件由程序整体重写，手工添加的注释不会保留。\r\n";
    s += L"\r\n[General]\r\n";

    auto kv = [&](const wchar_t* k, const std::wstring& v) {
        s += k; s += L'='; s += v; s += L"\r\n";
    };
    auto kvInt = [&](const wchar_t* k, int v) {
        wchar_t b[32];
        swprintf(b, 32, L"%d", v);
        kv(k, b);
    };

    kvInt(L"StartWithWindows", cfg_.startWithWindows ? 1 : 0);
    kvInt(L"KeepLog",          cfg_.keepLog ? 1 : 0);
    kvInt(L"AllowSystemTasks", cfg_.allowSystemTasks ? 1 : 0);
    kvInt(L"TrayOnAutostart",  cfg_.trayOnAutostart ? 1 : 0);
    kvInt(L"LogKeepMonths",    cfg_.logKeepMonths);
    kvInt(L"Quick1",           cfg_.quickMinutes[0]);
    kvInt(L"Quick2",           cfg_.quickMinutes[1]);
    kvInt(L"Quick3",           cfg_.quickMinutes[2]);

    for (size_t i = 0; i < tasks_.size(); ++i) {
        const PowerTask& t = tasks_[i];
        wchar_t sec[32];
        swprintf(sec, 32, L"Task.%u", (unsigned)i);

        s += L"\r\n["; s += sec; s += L"]\r\n";
        kv(L"id",            t.id);
        kv(L"name",          t.name);
        kvInt(L"action",     (int)t.action);
        kvInt(L"kind",       (int)t.kind);
        kvInt(L"delay",      t.delaySeconds);
        kvInt(L"hour",       t.hour);
        kvInt(L"minute",     t.minute);
        kvInt(L"year",       t.year);
        kvInt(L"month",      t.month);
        kvInt(L"day",        t.day);
        kvInt(L"weekdays",   (int)t.weekdayMask);
        kvInt(L"notice",     t.noticeSeconds);
        kvInt(L"force",      t.forceClose ? 1 : 0);
        kv(L"message",       t.message);
        kvInt(L"enabled",    t.enabled ? 1 : 0);
        kvInt(L"persistent", t.persistent ? 1 : 0);
    }

    // 宿主机归属表。一个任务ID都没记的宿主机段会被跳过，不留空段。
    for (const auto& hp : hosts_) {
        if (hp.second.empty()) continue;
        s += L"\r\n[Host."; s += hp.first; s += L"]\r\n";
        for (const auto& e : hp.second) kv(e.first.c_str(), e.second);
    }

    std::vector<unsigned char> bytes;
    TextToUtf16Le(s, &bytes);
    return WriteAllBytesAtomic(path_, bytes);
}

// ---------------------------------------------------------------------------
//  宿主机归属表
// ---------------------------------------------------------------------------

void TaskStore::SetHostTask(const std::wstring& host, const std::wstring& taskId,
                            const std::wstring& sysName) {
    if (host.empty() || taskId.empty()) return;
    if (sysName.empty()) hosts_[host].erase(taskId);
    else                 hosts_[host][taskId] = sysName;
}

void TaskStore::ClearHostTask(const std::wstring& host, const std::wstring& taskId) {
    auto it = hosts_.find(host);
    if (it == hosts_.end()) return;
    it->second.erase(taskId);
    if (it->second.empty()) hosts_.erase(it);
}

std::wstring TaskStore::HostTaskName(const std::wstring& host,
                                     const std::wstring& taskId) const {
    auto it = hosts_.find(host);
    if (it == hosts_.end()) return L"";
    auto jt = it->second.find(taskId);
    return (jt == it->second.end()) ? L"" : jt->second;
}

std::vector<std::wstring> TaskStore::HostTaskNames(const std::wstring& host) const {
    std::vector<std::wstring> v;
    auto it = hosts_.find(host);
    if (it == hosts_.end()) return v;
    for (const auto& kv : it->second) v.push_back(kv.second);
    return v;
}

void TaskStore::ClearHost(const std::wstring& host) { hosts_.erase(host); }

std::vector<std::wstring> TaskStore::Hosts() const {
    std::vector<std::wstring> v;
    for (const auto& hp : hosts_) v.push_back(hp.first);
    return v;
}

bool TaskStore::RemoveTask(const std::wstring& id) {
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i].id == id) {
            tasks_.erase(tasks_.begin() + i);
            return true;
        }
    }
    return false;
}

PowerTask* TaskStore::FindById(const std::wstring& id) {
    for (auto& t : tasks_) if (t.id == id) return &t;
    return nullptr;
}

// ===========================================================================
//  电源动作
// ===========================================================================

bool EnableShutdownPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    BOOL ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    if (ok) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        if (ok && GetLastError() != ERROR_SUCCESS) ok = FALSE;
    }
    CloseHandle(hToken);
    return ok != FALSE;
}

bool DoShutdown(bool reboot, DWORD timeoutSec, const std::wstring& msg, bool force) {
    EnableShutdownPrivilege();

    // 提示文本上限 512 字符
    std::wstring m = msg;
    if (m.size() > 500) m.resize(500);

    BOOL ok = InitiateSystemShutdownExW(
        nullptr,                                   // 本机
        m.empty() ? nullptr : const_cast<wchar_t*>(m.c_str()),
        timeoutSec,
        force ? TRUE : FALSE,
        reboot ? TRUE : FALSE,
        kShutdownReason);
    return ok != FALSE;
}

bool AbortShutdown() {
    EnableShutdownPrivilege();
    return AbortSystemShutdownW(nullptr) != FALSE;
}

bool DoSleep() {
    // SetSuspendState(bHibernate, bForce, bWakeupEventsDisabled)
    // 注意：若系统已启用休眠，部分机型会实际执行休眠而非睡眠（见设计文档 §7）
    return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
}

bool DoHibernate() {
    return SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
}

bool DoLogoff() {
    return ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, kShutdownReason) != FALSE;
}

bool DoLock() {
    return LockWorkStation() != FALSE;
}

bool DoMonitorOff() {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r);
    return true;
}

bool ExecuteAction(PowerAction a, DWORD timeoutSec, const std::wstring& msg, bool force) {
    switch (a) {
        case PowerAction::Shutdown:   return DoShutdown(false, timeoutSec, msg, force);
        case PowerAction::Restart:    return DoShutdown(true,  timeoutSec, msg, force);
        case PowerAction::Sleep:      return DoSleep();
        case PowerAction::Hibernate:  return DoHibernate();
        case PowerAction::Logoff:     return DoLogoff();
        case PowerAction::Lock:       return DoLock();
        case PowerAction::MonitorOff: return DoMonitorOff();
    }
    return false;
}

// ===========================================================================
//  工具函数
// ===========================================================================

const wchar_t* ActionName(PowerAction a) {
    switch (a) {
        case PowerAction::Shutdown:   return L"关机";
        case PowerAction::Restart:    return L"重启";
        case PowerAction::Sleep:      return L"睡眠";
        case PowerAction::Hibernate:  return L"休眠";
        case PowerAction::Logoff:     return L"注销";
        case PowerAction::Lock:       return L"锁定";
        case PowerAction::MonitorOff: return L"关闭显示器";
    }
    return L"未知";
}

const wchar_t* ActionHint(PowerAction a) {
    switch (a) {
        case PowerAction::Shutdown:   return L"会丢失未保存的内容";
        case PowerAction::Restart:    return L"会丢失未保存的内容";
        case PowerAction::Logoff:     return L"会关闭当前登录会话";
        case PowerAction::Sleep:      return L"低功耗，随时可唤醒";
        case PowerAction::Hibernate:  return L"状态写入磁盘后断电";
        case PowerAction::Lock:       return L"仅锁定屏幕，不影响运行";
        case PowerAction::MonitorOff: return L"仅关闭显示器，不影响运行";
    }
    return L"";
}

const wchar_t* TriggerName(TriggerKind k) {
    switch (k) {
        case TriggerKind::Countdown: return L"倒计时";
        case TriggerKind::OnceAt:    return L"指定时刻";
        case TriggerKind::Daily:     return L"每天";
        case TriggerKind::Weekly:    return L"每周";
    }
    return L"";
}

std::wstring NewGuid() {
    GUID g = {};
    if (CoCreateGuid(&g) != S_OK) {
        // 兜底：用 tick + 栈地址构造一个伪 GUID
        wchar_t buf[48];
        swprintf(buf, 48, L"%08X-%08X-%08X",
                 (unsigned)GetTickCount64(),
                 (unsigned)(ULONG_PTR)&g,
                 (unsigned)GetCurrentProcessId());
        return buf;
    }
    wchar_t buf[48] = {};
    StringFromGUID2(g, buf, 48);
    std::wstring s = buf;
    // 去掉首尾大括号
    if (!s.empty() && s.front() == L'{') s.erase(s.begin());
    if (!s.empty() && s.back()  == L'}') s.pop_back();
    return s;
}

ULONGLONG NowTick() { return GetTickCount64(); }

std::wstring FormatDuration(long long seconds) {
    if (seconds < 0) seconds = 0;
    long long d = seconds / 86400;
    long long h = (seconds % 86400) / 3600;
    long long m = (seconds % 3600) / 60;
    long long s = seconds % 60;

    wchar_t buf[128];
    if (d > 0)      swprintf(buf, 128, L"%lld 天 %lld 小时", d, h);
    else if (h > 0) swprintf(buf, 128, L"%lld 小时 %lld 分", h, m);
    else if (m > 0) swprintf(buf, 128, L"%lld 分钟", m);
    else            swprintf(buf, 128, L"%lld 秒", s);
    return buf;
}

std::wstring FormatWeekdays(unsigned mask) {
    static const wchar_t* names[7] = { L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日" };
    // 全选时给出更简洁的说法
    if ((mask & 0x1F) == 0x1F && !(mask & 0x60)) return L"工作日";
    if (mask == 0x7F) return L"每天";

    std::wstring r;
    for (int i = 0; i < 7; ++i) {
        if (mask & (1u << i)) {
            if (!r.empty()) r += L"、";
            r += names[i];
        }
    }
    return r.empty() ? L"未选择" : r;
}

std::wstring FormatRule(const PowerTask& t) {
    wchar_t buf[256];
    switch (t.kind) {
        case TriggerKind::Countdown:
            return FormatDuration(t.delaySeconds) + L"后";
        case TriggerKind::OnceAt:
            if (t.year > 0)
                swprintf(buf, 256, L"%04d-%02d-%02d %02d:%02d",
                         t.year, t.month, t.day, t.hour, t.minute);
            else
                swprintf(buf, 256, L"下一次 %02d:%02d", t.hour, t.minute);
            return buf;
        case TriggerKind::Daily:
            swprintf(buf, 256, L"每天 %02d:%02d", t.hour, t.minute);
            return buf;
        case TriggerKind::Weekly:
            swprintf(buf, 256, L"%ls %02d:%02d", FormatWeekdays(t.weekdayMask).c_str(), t.hour, t.minute);
            return buf;
    }
    return L"";
}

std::wstring FormatCountdown(ULONGLONG remainMs) {
    ULONGLONG sec = remainMs / 1000ULL;
    ULONGLONG d = sec / 86400ULL;
    ULONGLONG h = (sec % 86400ULL) / 3600ULL;
    ULONGLONG m = (sec % 3600ULL) / 60ULL;
    ULONGLONG s = sec % 60ULL;
    wchar_t buf[64];
    if (d > 0) swprintf(buf, 64, L"%llu 天 %02llu:%02llu:%02llu", d, h, m, s);
    else       swprintf(buf, 64, L"%02llu:%02llu:%02llu", h, m, s);
    return buf;
}

std::wstring FormatAbsolute(ULONGLONG tickFromNow) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += (tickFromNow / 1000ULL) * 10000000ULL;   // 秒 → 100ns
    ft.dwLowDateTime  = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    SYSTEMTIME out;
    FileTimeToSystemTime(&ft, &out);

    // 用 FILETIME 精确计算日差（避免月末误差）
    FILETIME fa, fb;
    SystemTimeToFileTime(&st, &fa);
    SystemTimeToFileTime(&out, &fb);
    ULARGE_INTEGER ua, ub;
    ua.LowPart = fa.dwLowDateTime; ua.HighPart = fa.dwHighDateTime;
    ub.LowPart = fb.dwLowDateTime; ub.HighPart = fb.dwHighDateTime;
    long long dayOffset = (long long)((ub.QuadPart / 864000000000ULL) -
                                      (ua.QuadPart / 864000000000ULL));
    if (dayOffset < 0) dayOffset = 0;

    wchar_t buf[64];
    if (dayOffset == 0)      swprintf(buf, 64, L"今天 %02d:%02d", out.wHour, out.wMinute);
    else if (dayOffset == 1) swprintf(buf, 64, L"明天 %02d:%02d", out.wHour, out.wMinute);
    else if (dayOffset == 2) swprintf(buf, 64, L"后天 %02d:%02d", out.wHour, out.wMinute);
    else                     swprintf(buf, 64, L"%02d-%02d %02d:%02d",
                                      out.wMonth, out.wDay, out.wHour, out.wMinute);
    return buf;
}

bool ComputeDueTick(const PowerTask& t, ULONGLONG now, ULONGLONG& outDue, ULONGLONG& outTotal) {
    if (!t.enabled) return false;

    if (t.kind == TriggerKind::Countdown) {
        ULONGLONG ms = (ULONGLONG)(t.delaySeconds > 0 ? t.delaySeconds : 1) * 1000ULL;
        outTotal = ms;
        outDue   = now + ms;
        return true;
    }

    // 定点类：纯日历算术，不涉及时区换算
    SYSTEMTIME st;
    GetLocalTime(&st);

    long long nowSecOfDay = (long long)st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    long long targetSecOfDay = (long long)t.hour * 3600 + t.minute * 60;

    long long deltaSec = -1;

    // 指定时刻 + 已具体化日期：走真正的日历差，不再「顺延到明天」。
    // 语义是「某个确切的时刻执行一次」——错过就是错过，不是「下一个到这个点」。
    if (t.kind == TriggerKind::OnceAt && t.year > 0) {
        if (t.month < 1 || t.month > 12) return false;
        if (t.day < 1 || t.day > DaysInMonth(t.year, t.month)) return false;

        SYSTEMTIME due = {};
        due.wYear   = (WORD)t.year;
        due.wMonth  = (WORD)t.month;
        due.wDay    = (WORD)t.day;
        due.wHour   = (WORD)t.hour;
        due.wMinute = (WORD)t.minute;
        due.wSecond = 0;

        FILETIME fNow = {}, fDue = {};
        if (!SystemTimeToFileTime(&st, &fNow)) return false;
        if (!SystemTimeToFileTime(&due, &fDue)) return false;

        ULARGE_INTEGER a, b;
        a.LowPart  = fNow.dwLowDateTime;  a.HighPart = fNow.dwHighDateTime;
        b.LowPart  = fDue.dwLowDateTime;  b.HighPart = fDue.dwHighDateTime;
        if (b.QuadPart <= a.QuadPart) return false;          // 已经过了

        ULONGLONG ms = (ULONGLONG)((b.QuadPart - a.QuadPart) / 10000ULL);
        outTotal = ms;
        outDue   = now + ms;
        return true;
    }

    if (t.kind == TriggerKind::OnceAt || t.kind == TriggerKind::Daily) {
        deltaSec = targetSecOfDay - nowSecOfDay;
        if (deltaSec <= 0) deltaSec += 86400;                 // 已过 → 顺延到明天
    } else { // Weekly
        for (int d = 0; d <= 7; ++d) {
            int dow = (st.wDayOfWeek + d) % 7;                 // 0=周日 ... 6=周六
            int bit = (dow + 6) % 7;                           // 换算成 bit0=周一
            if (!(t.weekdayMask & (1u << bit))) continue;

            long long cand = targetSecOfDay - nowSecOfDay + (long long)d * 86400LL;
            if (cand <= 0) continue;
            deltaSec = cand;
            break;
        }
        if (deltaSec < 0) return false;                        // 没有选中的星期
    }

    ULONGLONG ms = (ULONGLONG)deltaSec * 1000ULL;
    outTotal = ms;
    outDue   = now + ms;
    return true;
}

bool OnceAtExpired(const PowerTask& t) {
    if (t.kind != TriggerKind::OnceAt || t.year <= 0) return false;
    if (t.month < 1 || t.month > 12) return true;
    if (t.day < 1 || t.day > DaysInMonth(t.year, t.month)) return true;

    SYSTEMTIME now = {}, due = {};
    GetLocalTime(&now);
    due.wYear   = (WORD)t.year;
    due.wMonth  = (WORD)t.month;
    due.wDay    = (WORD)t.day;
    due.wHour   = (WORD)t.hour;
    due.wMinute = (WORD)t.minute;
    due.wSecond = 0;

    FILETIME fNow = {}, fDue = {};
    if (!SystemTimeToFileTime(&now, &fNow)) return false;
    if (!SystemTimeToFileTime(&due, &fDue)) return true;
    return CompareFileTime(&fNow, &fDue) >= 0;
}

int DaysInMonth(int year, int month) {
    return ppdate::DaysInMonth(year, month);
}

void LocalDatePlusDays(int addDays, int* outY, int* outM, int* outD) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    // 挪到正午再加减：这样跨夏令时也不会因为「少/多一小时」把日期算错一天。
    st.wHour = 12; st.wMinute = 0; st.wSecond = 0; st.wMilliseconds = 0;

    FILETIME ft = {};
    if (!SystemTimeToFileTime(&st, &ft)) {
        if (outY) *outY = st.wYear;
        if (outM) *outM = st.wMonth;
        if (outD) *outD = st.wDay;
        return;
    }

    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (long long)addDays * 86400LL * 10000000LL;   // 100ns 单位
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    SYSTEMTIME utc = {}, loc = {};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);

    if (outY) *outY = loc.wYear;
    if (outM) *outM = loc.wMonth;
    if (outD) *outD = loc.wDay;
}

// ===========================================================================
//  日志
// ===========================================================================

// 删掉超过保留期的日志。只认严格的 `YYYY-MM.log`——目录里别的文件一律不碰。
//
// 这是整个程序里唯一会删除用户文件的地方，所以边界写死在这里：
//   · 文件名必须正好 11 个字符、第 5 位是 '-'、其余全是数字；
//   · 年 2000–9999、月 1–12；
//   · 只删本程序按 `%04d-%02d.log` 生成的那种名字。
// 宁可漏删一个不认识的文件，也不要误删一个用户自己放进来的东西。
bool IsLogFileName(const std::wstring& n) {
    if (n.size() != 11) return false;            // YYYY-MM.log
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

static void PurgeOldLogs(const std::wstring& dir, int keepMonths) {
    if (keepMonths <= 0) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    long long curIndex = (long long)st.wYear * 12 + (st.wMonth - 1);

    std::wstring pattern = JoinPath(dir, L"*.log");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring n = fd.cFileName;
        if (!IsLogFileName(n)) continue;

        int y = 0, m = 0;
        for (int i = 0; i < 4; ++i) y = y * 10 + (n[i] - L'0');
        for (int i = 5; i < 7; ++i) m = m * 10 + (n[i] - L'0');

        long long idx = (long long)y * 12 + (m - 1);
        if (curIndex - idx >= keepMonths)
            DeleteFileW(JoinPath(dir, n).c_str());
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

// 给老版本写下的日志补 BOM。
//
// v0.4.0 及以前的日志是 UTF-16LE **不带 BOM**。这串字节在记事本里要靠猜，
// 猜错就是整篇乱码；VS Code / Notepad++ 干脆按 UTF-8 或 GBK 解，中文全成
// 问号。而日志里最值得看的恰恰是中文任务名。
//
// 新版本写带 BOM 的，但同一个月的老文件如果不管，用户得等到下个月才能看到
// 正常内容。所以升级后第一次写日志时把无 BOM 的老文件就地补上。
//
// 用原子写入而不是「在开头插 2 字节」：后者要把整段内容后移，中途失败就把
// 日志写坏了。日志本身不金贵，但「为了修显示问题反而弄丢内容」说不过去。
static void EnsureLogBom(const std::wstring& dir) {
    std::wstring pattern = JoinPath(dir, L"*.log");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring n = fd.cFileName;
        if (!IsLogFileName(n)) continue;      // 只碰自己写的文件

        const std::wstring path = JoinPath(dir, n);

        HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (r == INVALID_HANDLE_VALUE) continue;

        LARGE_INTEGER sz = {};
        unsigned char head[2] = {};
        DWORD got = 0;
        bool needFix = false;
        if (GetFileSizeEx(r, &sz) && sz.QuadPart >= 2 &&
            ReadFile(r, head, 2, &got, nullptr) && got == 2)
            needFix = !(head[0] == 0xFF && head[1] == 0xFE);
        CloseHandle(r);

        if (!needFix) continue;
        if (sz.QuadPart > (8 << 20)) continue;    // 异常大的文件不动它

        r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (r == INVALID_HANDLE_VALUE) continue;

        std::vector<unsigned char> bytes;
        bytes.reserve((size_t)sz.QuadPart + 2);
        bytes.push_back(0xFF);                    // UTF-16LE BOM
        bytes.push_back(0xFE);

        bool ok = true;
        unsigned char buf[8192];
        for (;;) {
            DWORD want = 0;
            if (!ReadFile(r, buf, sizeof(buf), &want, nullptr)) { ok = false; break; }
            if (want == 0) break;
            bytes.insert(bytes.end(), buf, buf + want);
        }
        CloseHandle(r);

        if (ok) WriteAllBytesAtomic(path, bytes);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

void LogMaintenance() {
    if (!g_store.Config().keepLog) return;

    // 每次运行只做一次。
    //
    // 挂成独立函数而不是只藏在 LogLine 里，是因为用户升级后可能好几天不触发
    // 任何任务——那期间老日志一直是乱码的，他会以为「修了个寂寞」。UI_Create
    // 里主动调一次，升级后的第一次启动就把文件修好。
    static bool done = false;
    if (done) return;
    done = true;

    std::wstring dir = JoinPath(ExeDir(), L"logs");
    CreateDirectoryW(dir.c_str(), nullptr);
    PurgeOldLogs(dir, g_store.Config().logKeepMonths);
    EnsureLogBom(dir);
}

void LogLine(const wchar_t* text) {
    if (!text || !*text) return;
    if (!g_store.Config().keepLog) return;

    LogMaintenance();

    std::wstring dir = JoinPath(ExeDir(), L"logs");
    CreateDirectoryW(dir.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t file[64];
    swprintf(file, 64, L"%04d-%02d.log", st.wYear, st.wMonth);
    std::wstring path = JoinPath(dir, file);

    wchar_t line[1200];
    swprintf(line, 1200, L"[%04d-%02d-%02d %02d:%02d:%02d] %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             text);

    // 编码是 UTF-16LE **带 BOM**，和 sched.cpp 的诊断日志同一套。
    // 走 TextToUtf16Le 而不是直接把 wchar_t 写盘：那个函数已经在处理
    // 「宿主机 wchar_t 是 32 位、Windows 是 16 位」的差异，两处共用一份。
    std::vector<unsigned char> bytes;
    TextToUtf16Le(line, &bytes);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // 只有新建（或恰好为空）的文件才留 BOM。追加时摘掉，否则 BOM 会落在
    // 文本中间，被当成一个 U+FEFF 显示成方块。
    LARGE_INTEGER sz = {};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 &&
        bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        bytes.erase(bytes.begin(), bytes.begin() + 2);

    DWORD written = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
}

void LogLine(const std::wstring& text) { LogLine(text.c_str()); }
