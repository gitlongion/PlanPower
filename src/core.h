// ============================================================================
//  PlanPower - 定时电源管理器（便携版）
//  core.h —— 核心层接口：任务模型、存储、电源动作、时间工具
//
//  设计约束：零第三方依赖、零外部运行时、单文件便携。
//  只使用 Windows 核心系统库（kernel32 / user32 / advapi32 / powrprof 等）。
// ============================================================================
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <map>

// ---------------------------------------------------------------------------
//  电源动作
// ---------------------------------------------------------------------------
enum class PowerAction {
    Shutdown = 0,   // 关机
    Restart,        // 重启
    Sleep,          // 睡眠
    Hibernate,      // 休眠
    Logoff,         // 注销
    Lock,           // 锁定
    MonitorOff      // 关闭显示器
};

// ---------------------------------------------------------------------------
//  触发方式
// ---------------------------------------------------------------------------
enum class TriggerKind {
    Countdown = 0,  // N 秒后执行
    OnceAt,         // 指定时刻执行一次（已过则顺延到明天）
    Daily,          // 每天 HH:MM
    Weekly          // 选定星期几的 HH:MM
};

// ---------------------------------------------------------------------------
//  任务实体
// ---------------------------------------------------------------------------
struct PowerTask {
    std::wstring id;            // GUID 字符串，任务唯一标识
    std::wstring name;          // 用户可见名称
    PowerAction  action = PowerAction::Shutdown;
    TriggerKind  kind   = TriggerKind::Countdown;

    // --- 触发参数 ---
    int  delaySeconds = 1800;   // Countdown：延迟秒数
    int  hour = 23;             // OnceAt / Daily / Weekly：小时
    int  minute = 0;            // 同上：分钟
    unsigned weekdayMask = 0x1F;// Weekly：bit0=周一 ... bit6=周日

    // OnceAt 的具体日期。
    // year == 0 表示「未指定」——旧配置（v0.3.2 及以前）没有这三个键，
    // 沿用当时的语义「今天这个点，过了顺延到明天」。一旦在编辑窗里保存过，
    // 日期就会被具体化，之后按「某个确切时刻执行一次」处理：错过就是错过。
    int  year = 0, month = 0, day = 0;

    // --- 高级选项 ---
    int          noticeSeconds = 60;    // 提前提醒秒数，0 = 不提醒
    bool         forceClose    = false; // 是否强制关闭程序
    std::wstring message;               // 关机提示文本

    bool enabled = true;        // 启用 / 停用

    // --- 系统级持久任务 ---
    // true 表示这条任务同时注册进了本机的 Windows 任务计划程序。
    // 注意：该字段记录的是"用户的意图 + 本机注册结果"，具体任务名还要
    // 结合宿主机归属表（见 TaskStore::HostTaskName）才能定位到系统任务。
    bool persistent = false;

    // --- 运行时状态（不持久化） ---
    bool      running     = false;  // 是否已排入计时器
    ULONGLONG dueTick     = 0;      // 目标时刻（GetTickCount64 基准）
    ULONGLONG totalTicks  = 0;      // 总时长，用于进度条
    bool      noticeFired = false;  // 本次是否已弹过提醒

    PowerTask() = default;
};

// ---------------------------------------------------------------------------
//  应用配置
// ---------------------------------------------------------------------------
struct AppConfig {
    bool startWithWindows = false;  // 开机自启（本机专属，不随盘走）
    bool keepLog          = false;  // 是否记录执行日志
    bool portableMode     = true;   // 当前是否为便携模式（配置写在 exe 同目录）

    // 日志保留月数。0 = 永久保留。默认 6：logs/ 里是按月分的文件，
    // 一个常驻的便携盘跑一年也就 12 个小文件，但「永不清理」意味着
    // 用户永远不需要知道它存在——直到某天发现 U 盘满了。
    int  logKeepMonths    = 6;

    // 托盘菜单里那三个快捷关机的时长（分钟）。
    // 原来是写死的 30 / 60 / 120，但「临时关一下」这个动作的常用档位
    // 因人而异——写死就意味着每次都要新建一条任务。
    int  quickMinutes[3]  = { 30, 60, 120 };

    // 开机自启时是否只显示托盘图标（不弹出主界面）。
    // 默认 true：开机那一刻用户正在登录，没有人在等这个界面。
    // 注意它只影响「由自启动拉起的那一次」——手动双击 exe 永远有界面，
    // 这个区分是靠命令行 /tray 显式传递的，不靠猜。
    bool trayOnAutostart  = true;

    // 全局闸门：是否允许把任务写入本机任务计划程序。
    // 默认 false —— 不主动开启，程序就一个字节都不往宿主机写。
    bool allowSystemTasks = false;
};

// ---------------------------------------------------------------------------
//  任务存储（INI 格式，用 kernel32 的 Profile API，零依赖）
//
//  除任务本身外，还维护一张"宿主机归属表"：
//  便携盘会插到很多台机器上，配置跟着盘走，但系统任务留在各台机器里。
//  归属表记录「本盘在哪台机器上注册过哪些系统任务」，写入 INI 的
//  [Host.<机器指纹>] 段。它只是优化项——真正的兜底是任务名前缀和
//  \PlanPower 文件夹，所以即使归属表丢了也能靠 /cleanup-all 清干净。
// ---------------------------------------------------------------------------
class TaskStore {
public:
    // 加载；文件不存在时返回 false 但不报错（首次运行）
    bool Load(const std::wstring& iniPath);
    // 保存全部任务与配置
    bool Save();

    std::vector<PowerTask>& Tasks() { return tasks_; }
    AppConfig&              Config() { return cfg_; }
    const std::wstring&     Path() const { return path_; }

    // 增删
    void AddTask(const PowerTask& t)   { tasks_.push_back(t); }
    bool RemoveTask(const std::wstring& id);
    PowerTask* FindById(const std::wstring& id);

    // --- 宿主机归属表 ---
    // 记录：在 host 这台机器上，taskId 对应的系统任务名是 sysName
    void SetHostTask(const std::wstring& host, const std::wstring& taskId,
                     const std::wstring& sysName);
    // 清除一条记录（系统任务被删掉后调用）
    void ClearHostTask(const std::wstring& host, const std::wstring& taskId);
    // 查询；未记录返回空串
    std::wstring HostTaskName(const std::wstring& host, const std::wstring& taskId) const;
    // 本机上记录的全部系统任务名（用于与系统实际状态比对）
    std::vector<std::wstring> HostTaskNames(const std::wstring& host) const;
    // 丢弃整段归属记录（清理本机系统任务后调用）
    void ClearHost(const std::wstring& host);
    // 已知的全部宿主机指纹
    std::vector<std::wstring> Hosts() const;

private:
    std::wstring           path_;
    std::vector<PowerTask> tasks_;
    AppConfig              cfg_;
    // 指纹 → (任务ID → 系统任务名)
    std::map<std::wstring, std::map<std::wstring, std::wstring>> hosts_;
};

// ---------------------------------------------------------------------------
//  路径与便携性
// ---------------------------------------------------------------------------
std::wstring ExeDir();                                     // exe 所在目录（带结尾反斜杠）
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool         DirIsWritable(const std::wstring& dir);        // 探测目录可写性
bool         IsRemovableDrive(const std::wstring& path);    // 是否位于可移动介质
// 决定配置文件的落盘位置（便携决策树）
std::wstring ResolveConfigPath(bool* outPortable);
// 便携标记文件是否存在
bool PortableFlagExists();

// ---------------------------------------------------------------------------
//  机器与介质标识
// ---------------------------------------------------------------------------
// 本机指纹：计算机名 + 系统盘卷序列号 → 8 位十六进制。
// 用途是回答"这台机器是不是我上次插的那台"，不追求防伪。
std::wstring MachineFingerprint();
// 本机计算机名（显示用，最多 15 字符）
std::wstring MachineShortName();
// 本介质标识：exe 所在卷的序列号 → 4 位十六进制。
// 卷序列号不可得时（网络路径等）退化为目录路径的哈希。
// 用途是让两支 U 盘的任务名互不冲突。
std::wstring DriveTag();

// ---------------------------------------------------------------------------
//  电源动作执行
// ---------------------------------------------------------------------------
bool EnableShutdownPrivilege();      // 提升 SeShutdownPrivilege
bool DoShutdown(bool reboot, DWORD timeoutSec, const std::wstring& msg, bool force);
bool AbortShutdown();
bool DoSleep();
bool DoHibernate();
bool DoLogoff();
bool DoLock();
bool DoMonitorOff();
// 统一入口：执行指定动作
bool ExecuteAction(PowerAction a, DWORD timeoutSec, const std::wstring& msg, bool force);

// ---------------------------------------------------------------------------
//  工具函数
// ---------------------------------------------------------------------------
const wchar_t* ActionName(PowerAction a);        // "关机"
const wchar_t* ActionHint(PowerAction a);        // "会丢失未保存的内容"
const wchar_t* TriggerName(TriggerKind k);       // "倒计时"
std::wstring   NewGuid();
std::wstring   FormatDuration(long long seconds);         // "1 小时 30 分"
std::wstring   FormatRule(const PowerTask& t);            // "每周一、三、五 23:00"
std::wstring   FormatWeekdays(unsigned mask);             // "周一、周三"
ULONGLONG      NowTick();                                 // GetTickCount64
// 计算任务的下次执行时刻；返回 false 表示无法计算（如未启用、日期已过）
bool ComputeDueTick(const PowerTask& t, ULONGLONG now, ULONGLONG& outDue, ULONGLONG& outTotal);
// 指定时刻任务是否已经错过了它自己的日期（只有 OnceAt 且已具体化日期时才有意义）
bool OnceAtExpired(const PowerTask& t);
// 今天往后 n 天的本地日期（n 可为 0）。用来把「今天/明天」具体化成日期。
void LocalDatePlusDays(int addDays, int* outY, int* outM, int* outD);
// 某年某月的天数；月份非法返回 0。闰年在这里判，别让编辑窗自己算。
int  DaysInMonth(int year, int month);
// 把 tick 差值格式化为 "00:23:41" 或 "1 天 03:20:11"
std::wstring   FormatCountdown(ULONGLONG remainMs);
// 把绝对时间格式化为 "今天 23:00" / "明天 08:30" / "10-15 09:00"
std::wstring   FormatAbsolute(ULONGLONG tickFromNow);

// 简单的日志（可关闭）
//
// 两个重载，指针版是原语，wstring 版转发过去。加指针版不是为了省事：
//   `LogLine(L"全部暂停")` 只有 wstring 版时，clang 会把 4 个字符直接编成
//   `movl $imm` 写进 libc++ 的短串缓冲区（SSO），rodata 里那份字面量成了
//   死数据被删掉。功能没问题，但 tools/check_strings.py 靠「在 exe 里找得到
//   这条串的 UTF-16 字节」来判断文案有没有被 -O2 的常量合并拼坏——串不在
//   rodata 里，这道防线就看不见它了。
//   有指针版之后，字面量以指针形式传给另一个翻译单元的函数，编译器必须把它
//   留在 rodata，检查照常有效；顺带每行日志少一次临时 std::wstring 构造。
void LogLine(const wchar_t* text);
void LogLine(const std::wstring& text);

// 判断一个文件名是不是本程序自己写的 `YYYY-MM.log`。
//
// 只有一份实现，两个地方用：PurgeOldLogs 决定「能删哪些」，
// 执行历史窗决定「能读哪些」。两边各写一套的话迟早会漂移，
// 而漂移的方向恰好是「清理删得比读取宽」——把用户自己放进 logs
// 目录的东西删掉。宁可漏删，也不要误删。
bool IsLogFileName(const std::wstring& name);

extern TaskStore g_store;
