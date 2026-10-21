// ============================================================================
//  PlanPower - sched_xml.cpp
//  任务定义的纯逻辑实现（详见 sched_xml.h 的说明）
// ============================================================================
#include "sched_xml.h"
#include "dateutil.h"       // 纯头：闰年 / 月份天数，与 core.cpp 共用同一份
#include <cstdio>

// ---------------------------------------------------------------------------
//  能力查询
// ---------------------------------------------------------------------------

bool ActionSupportsPersistent(PowerAction a) {
    // 关闭显示器没有对应的系统命令；收尾动作、看门狗等也需要 PlanPower
    // 自己运行才能实现，因此都不在支持之列。
    return a != PowerAction::MonitorOff;
}

const wchar_t* PersistentUnsupportedReason(PowerAction a) {
    if (a == PowerAction::MonitorOff)
        return L"Windows 没有「关闭显示器」的命令行入口，"
               L"这项只能由 PlanPower 自己运行时执行，无法做成系统任务。";
    return L"";
}

// ---------------------------------------------------------------------------
//  文本工具
// ---------------------------------------------------------------------------

static std::wstring IntToStr(int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    return b;
}

// 只用于「元素文本内容」，不用于属性值。
// 因此双引号与单引号无需转义——文本内容里它们本来就是合法字符。
// 不转义的好处是导出的 XML 保持可读：shutdown 的
// /c "提示文本" 能原样看到，而不是 /c &quot;提示文本&quot;。
// 如果以后要把这个函数用在属性值上，必须补回转义。
static void XmlEscape(std::wstring* out, const std::wstring& s) {
    for (wchar_t c : s) {
        switch (c) {
            case L'&':  *out += L"&amp;";  break;
            case L'<':  *out += L"&lt;";   break;
            case L'>':  *out += L"&gt;";   break;
            default:    *out += c;
        }
    }
}

static std::wstring XmlEscaped(const std::wstring& s) {
    std::wstring r;
    XmlEscape(&r, s);
    return r;
}

// 嵌进 shutdown.exe 参数里的文本：双引号会拆坏参数引号，换行会截断命令行，
// 一律去掉；长度按 shutdown /c 的 512 上限截断。
static std::wstring SanitizeCmdText(const std::wstring& s, size_t maxLen) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\r' || c == L'\n') continue;
        r += c;
        if (r.size() >= maxLen) break;
    }
    return r;
}

// ---------------------------------------------------------------------------
//  时间
// ---------------------------------------------------------------------------

// 本地时间加减秒数。
// SystemTimeToFileTime / FileTimeToSystemTime 是同一套线性换算，时区偏移在
// 往返中相互抵消，所以把 GetLocalTime 的结果直接当 UTC 做加减是安全的。
static SYSTEMTIME AddSecondsLocal(const SYSTEMTIME& localSt, long long sec) {
    FILETIME ft = {};
    SystemTimeToFileTime(&localSt, &ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)sec * 10000000ULL;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    SYSTEMTIME out = {};
    FileTimeToSystemTime(&ft, &out);
    return out;
}

static std::wstring FormatIso(const SYSTEMTIME& st) {
    wchar_t b[40];
    swprintf(b, 40, L"%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// 计算 StartBoundary。返回 false 表示无法确定（每周任务一天都没勾选）
static bool ComputeStartBoundary(const PowerTask& t, const SchedEnv& env,
                                 std::wstring* out) {
    const SYSTEMTIME& now = env.now;

    if (t.kind == TriggerKind::Countdown) {
        long long sec = t.delaySeconds > 0 ? t.delaySeconds : 1;
        *out = FormatIso(AddSecondsLocal(now, sec));
        return true;
    }

    long long nowSecOfDay = (long long)now.wHour * 3600 + now.wMinute * 60 + now.wSecond;
    long long tgtSecOfDay = (long long)t.hour * 3600 + (long long)t.minute * 60;

    if (t.kind == TriggerKind::OnceAt && t.year > 0) {
        // 已具体化日期：直接用那一天的日期，不再「顺延到明天」。
        // 日期合法性在这里也要判一次——INI 是可以手改的，不能让一个
        // 2026-02-30 混进任务定义里，让服务端报一句看不懂的 0x80041318。
        if (!ppdate::ValidDate(t.year, t.month, t.day)) return false;
        SYSTEMTIME d = {};
        d.wYear   = (WORD)t.year;
        d.wMonth  = (WORD)t.month;
        d.wDay    = (WORD)t.day;
        d.wHour   = (WORD)t.hour;
        d.wMinute = (WORD)t.minute;
        d.wSecond = 0;
        *out = FormatIso(d);
        return true;
    }

    if (t.kind == TriggerKind::OnceAt || t.kind == TriggerKind::Daily) {
        int dayOffset = (tgtSecOfDay > nowSecOfDay) ? 0 : 1;   // 已过则顺延到明天
        SYSTEMTIME d = AddSecondsLocal(now, (long long)dayOffset * 86400);
        d.wHour   = (WORD)t.hour;
        d.wMinute = (WORD)t.minute;
        d.wSecond = 0;
        *out = FormatIso(d);
        return true;
    }

    // Weekly：从今天起找第一个勾选中的星期，且时刻必须尚未过去。
    // StartBoundary 的日期落在勾选的那一天，任务才会从这一周就开始生效。
    for (int d = 0; d <= 7; ++d) {
        SYSTEMTIME day = AddSecondsLocal(now, (long long)d * 86400);
        int bit = (day.wDayOfWeek + 6) % 7;                    // wDayOfWeek: 0=周日
        if (!(t.weekdayMask & (1u << bit))) continue;
        if (d == 0 && tgtSecOfDay <= nowSecOfDay) continue;     // 今天但已过
        day.wHour   = (WORD)t.hour;
        day.wMinute = (WORD)t.minute;
        day.wSecond = 0;
        *out = FormatIso(day);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  命名
// ---------------------------------------------------------------------------

std::wstring SanitizeTaskName(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"'  || c == L'<' || c == L'>' || c == L'|' || c < 0x20)
            r += L'_';
        else
            r += c;
    }
    // 任务名不允许以空格开头或结尾
    size_t b = r.find_first_not_of(L' ');
    if (b == std::wstring::npos) return L"";
    size_t e = r.find_last_not_of(L' ');
    r = r.substr(b, e - b + 1);

    if (r.size() > 40) r.resize(40);
    return r;
}

std::wstring SchedFolderPath() { return L"\\PlanPower"; }

std::wstring SchedTaskLeafName(const PowerTask& t, const std::wstring& driveTag) {
    std::wstring base = SanitizeTaskName(t.name);
    if (base.empty()) base = L"任务";
    std::wstring id8 = t.id.size() >= 8 ? t.id.substr(0, 8) : t.id;
    if (id8.empty()) id8 = L"00000000";
    // 内嵌介质标识：两支 U 盘插同一台机器时任务名不会撞车，清理也不会误删
    return base + L"_" + driveTag + L"_" + id8;
}

std::wstring SchedTaskFullPath(const PowerTask& t, const std::wstring& driveTag) {
    return SchedFolderPath() + L"\\" + SchedTaskLeafName(t, driveTag);
}

// ---------------------------------------------------------------------------
//  触发器
// ---------------------------------------------------------------------------

static bool BuildTriggerXml(const PowerTask& t, const SchedEnv& env,
                            std::wstring* out, std::wstring* err) {
    std::wstring start;
    if (!ComputeStartBoundary(t, env, &start)) {
        if (err) *err = L"没有选中任何星期，无法确定执行时刻。";
        return false;
    }

    std::wstring s;
    if (t.kind == TriggerKind::Countdown || t.kind == TriggerKind::OnceAt) {
        // 一次性时间触发器：只给 StartBoundary，不给重复，跑完即止
        s  = L"<TimeTrigger><StartBoundary>";
        s += start;
        s += L"</StartBoundary><Enabled>true</Enabled></TimeTrigger>";
    } else if (t.kind == TriggerKind::Daily) {
        s  = L"<CalendarTrigger><StartBoundary>";
        s += start;
        s += L"</StartBoundary><Enabled>true</Enabled>"
             L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>"
             L"</CalendarTrigger>";
    } else {
        static const wchar_t* kDay[7] = {
            L"Monday", L"Tuesday", L"Wednesday", L"Thursday",
            L"Friday", L"Saturday", L"Sunday"
        };
        s  = L"<CalendarTrigger><StartBoundary>";
        s += start;
        s += L"</StartBoundary><Enabled>true</Enabled>"
             L"<ScheduleByWeek><WeeksInterval>1</WeeksInterval><DaysOfWeek>";
        for (int i = 0; i < 7; ++i) {
            if (t.weekdayMask & (1u << i)) {
                s += L"<";
                s += kDay[i];
                s += L"/>";
            }
        }
        s += L"</DaysOfWeek></ScheduleByWeek></CalendarTrigger>";
    }
    *out = s;
    return true;
}

// ---------------------------------------------------------------------------
//  动作
// ---------------------------------------------------------------------------

static bool BuildActionXml(const PowerTask& t, const SchedEnv& env,
                           std::wstring* out, std::wstring* err) {
    if (!ActionSupportsPersistent(t.action)) {
        if (err) *err = PersistentUnsupportedReason(t.action);
        return false;
    }

    std::wstring command, arguments;
    const std::wstring& sys = env.systemDir;

    switch (t.action) {
    case PowerAction::Shutdown:
    case PowerAction::Restart: {
        command   = sys + L"shutdown.exe";
        arguments = (t.action == PowerAction::Shutdown) ? L"/s" : L"/r";
        // 用 shutdown 自带的 /t 倒计时顶替"提前提醒"：
        // 持久任务执行时 PlanPower 并不在运行，没法弹自己的提醒窗，但 Windows
        // 会弹出官方的关机倒计时对话框，且用户可以用 shutdown /a 撤销——
        // 比静默关机好得多。这也是持久模式下"提前提醒"的等价物。
        int n = t.noticeSeconds > 0 ? t.noticeSeconds : 0;
        if (n > 315360000) n = 315360000;
        arguments += L" /t ";
        arguments += IntToStr(n);
        if (t.forceClose) arguments += L" /f";
        // /c 只在 /t > 0 时才会被显示出来，否则不必带上
        if (n > 0 && !t.message.empty()) {
            arguments += L" /c \"";
            arguments += SanitizeCmdText(t.message, 500);
            arguments += L"\"";
        }
        break;
    }
    case PowerAction::Logoff:
        command   = sys + L"shutdown.exe";
        arguments = t.forceClose ? L"/l /f" : L"/l";
        break;
    case PowerAction::Hibernate:
        // /t 与 /c 对 /h 无效，只保留 /f
        command   = sys + L"shutdown.exe";
        arguments = t.forceClose ? L"/h /f" : L"/h";
        break;
    case PowerAction::Sleep:
        // rundll32 是唯一不需要第三方程序就能进入睡眠的系统入口
        command   = sys + L"rundll32.exe";
        arguments = L"powrprof.dll,SetSuspendState 0,1,0";
        break;
    case PowerAction::Lock:
        command   = sys + L"rundll32.exe";
        arguments = L"user32.dll,LockWorkStation";
        break;
    case PowerAction::MonitorOff:
        if (err) *err = PersistentUnsupportedReason(t.action);
        return false;
    }

    std::wstring s = L"<Exec><Command>";
    s += XmlEscaped(command);
    s += L"</Command>";
    if (!arguments.empty()) {
        s += L"<Arguments>";
        s += XmlEscaped(arguments);
        s += L"</Arguments>";
    }
    s += L"</Exec>";
    *out = s;
    return true;
}

// ---------------------------------------------------------------------------
//  组装
// ---------------------------------------------------------------------------

bool SchedBuildXml(const PowerTask& t, const SchedEnv& env,
                   const SchedXmlOpts& opts,
                   std::wstring* xml, std::wstring* err) {
    if (!xml) return false;
    if (err) err->clear();

    std::wstring trigger;
    if (!BuildTriggerXml(t, env, &trigger, err)) return false;

    std::wstring action;
    if (!BuildActionXml(t, env, &action, err)) return false;

    // 注销 / 锁定必须落在交互式会话里：以 SYSTEM 身份执行会作用于会话 0，
    // 而不是用户当前登录的那个桌面。关机 / 重启 / 睡眠 / 休眠则相反——
    // 用 SYSTEM 才能在锁屏或无人登录时也照常执行。
    bool interactive = (t.action == PowerAction::Logoff || t.action == PowerAction::Lock);
    std::wstring userId, logonType;
    if (interactive) {
        userId = env.userSid;
        if (userId.empty()) {
            if (err) *err = L"无法取得当前用户标识，注销 / 锁定无法做成系统任务。";
            return false;
        }
        logonType = L"InteractiveToken";
    } else {
        userId    = L"S-1-5-18";           // Local System
        logonType = L"ServiceAccount";
    }

    std::wstring desc = L"由 PlanPower 便携版创建｜介质 ";
    desc += env.driveTag;
    desc += L"｜机器 ";
    desc += env.machineFp;
    desc += L"｜任务 ";
    desc += t.id;
    desc += L"｜如需清除，可在 PlanPower 中用「清理本机系统任务」，"
            L"或运行 PlanPower.exe /cleanup-all";

    const bool rich = (opts.style == SchedXmlStyle::Rich);
    const std::wstring& ver = opts.version.empty() ? std::wstring(L"1.2") : opts.version;

    std::wstring s;
    s += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    // version 不能低于所用元素引入的版本：AllowHardTerminate / Enabled /
    // Hidden / RunOnlyIfIdle / WakeToRun / ExecutionTimeLimit / Priority /
    // RestartOnIdle 都是 1.2 才引入的，写成 1.0 会被服务端判为非法。
    // 1.4 是 Windows 10 起的版本，sched.cpp 会按 1.2 → 1.4 的顺序试。
    s += L"<Task version=\"";
    s += ver;
    s += L"\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    // RegistrationInfo 里只放 Author 与 Description。
    // 刻意不写 <URI>：任务的路径由 RegisterTaskDefinition 的第一个参数决定，
    // 服务端会自己推导。手写一个以反斜杠开头的 URI 一旦与注册路径有任何出入，
    // 就会以 0x80041318（XML 取值非法）被拒，而报错完全指不到这个元素上。
    s += L"  <RegistrationInfo>\r\n";
    s += L"    <Author>PlanPower</Author>\r\n";
    s += L"    <Description>";
    s += XmlEscaped(desc);
    s += L"</Description>\r\n";
    s += L"  </RegistrationInfo>\r\n";

    s += L"  <Triggers>\r\n    ";
    s += trigger;
    s += L"\r\n  </Triggers>\r\n";

    // 精简风格整段省略 <Principals>：身份改由 RegisterTaskDefinition 的
    // userId / logonType 参数确定，那本来就是文档规定的正路（参数「优先于
    // 任务定义对象里的凭据」）。少一段就少一处可能被判非法的取值。
    // RunLevel 的默认值 LeastPrivilege 对我们没有影响：SYSTEM 身份下
    // RunLevel 本来就被忽略，而注销 / 锁定也不需要提权。
    if (rich) {
        s += L"  <Principals>\r\n    <Principal id=\"Author\">\r\n";
        s += L"      <UserId>";
        s += XmlEscaped(userId);
        s += L"</UserId>\r\n      <LogonType>";
        s += logonType;
        s += L"</LogonType>\r\n      <RunLevel>HighestAvailable</RunLevel>\r\n";
        s += L"    </Principal>\r\n  </Principals>\r\n";
    }

    s += L"  <Settings>\r\n";
    // 下面这几项是「会改变行为」的，两套风格都必须写死：
    //  · DisallowStartIfOnBatteries / StopIfGoingOnBatteries 默认是 true，
    //    会让笔记本在电池供电时静默不执行且毫无报错；
    //  · StartWhenAvailable 实现「错过补跑」；
    //  · WakeToRun 让任务能唤醒休眠中的机器；
    //  · Enabled 承载用户按下的启停开关；
    //  · ExecutionTimeLimit 覆盖默认的 PT72H，免得卡住的实例在系统里挂三天。
    s += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    s += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    s += L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n";
    s += L"    <Enabled>";
    s += t.enabled ? L"true" : L"false";
    s += L"</Enabled>\r\n";
    s += L"    <WakeToRun>true</WakeToRun>\r\n";
    s += L"    <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>\r\n";

    if (rich) {
        // 其余各项在精简风格里整段省掉：它们要么本来就等于默认值
        // （AllowHardTerminate=true、Hidden=false、RunOnlyIfIdle=false、
        //   RunOnlyIfNetworkAvailable=false、AllowStartOnDemand=true、
        //   Priority=7），要么只在 RunOnlyIfIdle=true 时才有意义
        // （IdleSettings）。写出来只是把「默认值」重复一遍，却每一个
        // 都是一处可能被服务端判为非法的取值。
        s += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
        s += L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n";
        s += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
        s += L"    <IdleSettings>\r\n";
        s += L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n";
        s += L"      <RestartOnIdle>false</RestartOnIdle>\r\n";
        s += L"    </IdleSettings>\r\n";
        s += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
        s += L"    <Hidden>false</Hidden>\r\n";
        s += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
        s += L"    <Priority>7</Priority>\r\n";
    }
    s += L"  </Settings>\r\n";

    s += L"  <Actions Context=\"Author\">\r\n    ";
    s += action;
    s += L"\r\n  </Actions>\r\n";
    s += L"</Task>\r\n";

    *xml = s;
    return true;
}

bool SchedBuildXml(const PowerTask& t, const SchedEnv& env,
                   std::wstring* xml, std::wstring* err) {
    return SchedBuildXml(t, env, SchedXmlOpts(), xml, err);
}
