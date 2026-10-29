// ============================================================================
//  PlanPower 宿主机测试：任务 XML 生成
//
//  这不是「照着规则再实现一遍」的对照测试——它直接编译 src/sched_xml.cpp，
//  跑的是将来在 Windows 上真正执行的那份代码，只是把时间、系统目录、介质
//  标识、机器指纹、用户 SID 换成了固定值。
//
//  产出两样东西：
//   1) 控制台上的断言结果
//   2) /tmp/pp_xml/*.xml —— 用真实代码生成的 UTF-16LE+BOM 任务定义，
//      交给 check_xml.py 用真正的 XML 解析器再验一遍结构
// ============================================================================
#include "windows.h"
#include "../src/sched_xml.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static std::string ToUtf8(const std::wstring& s) {
    std::string r;
    for (wchar_t c : s) {
        unsigned int cp = (unsigned int)c;
        if (cp < 0x80) r += (char)cp;
        else if (cp < 0x800) {
            r += (char)(0xC0 | (cp >> 6));
            r += (char)(0x80 | (cp & 0x3F));
        } else {
            r += (char)(0xE0 | (cp >> 12));
            r += (char)(0x80 | ((cp >> 6) & 0x3F));
            r += (char)(0x80 | (cp & 0x3F));
        }
    }
    return r;
}

static void Fail(const std::string& what) {
    g_fail++;
    printf("  FAIL  %s\n", what.c_str());
}

static void Ok() { g_pass++; }

static void CheckTrue(bool cond, const std::string& what) {
    if (cond) Ok(); else Fail(what);
}

static void CheckHas(const std::wstring& xml, const wchar_t* needle,
                     const std::string& where) {
    if (xml.find(needle) != std::wstring::npos) Ok();
    else Fail(where + "：缺少 [" + ToUtf8(needle) + "]");
}

static void CheckNotHas(const std::wstring& xml, const wchar_t* needle,
                        const std::string& where) {
    if (xml.find(needle) == std::wstring::npos) Ok();
    else Fail(where + "：不应出现 [" + ToUtf8(needle) + "]");
}

// 把 wstring 按 UTF-16LE + BOM 写盘（我们的字符串全在 BMP 内）
static void WriteUtf16Le(const std::string& path, const std::wstring& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { Fail("无法写入 " + path); return; }
    unsigned char bom[2] = { 0xFF, 0xFE };
    fwrite(bom, 1, 2, f);
    for (wchar_t c : s) {
        unsigned int cp = (unsigned int)c;
        unsigned char b[2] = { (unsigned char)(cp & 0xFF),
                               (unsigned char)((cp >> 8) & 0xFF) };
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

static PowerTask MakeTask(PowerAction a, TriggerKind k,
                          const wchar_t* name, const wchar_t* id) {
    PowerTask t;
    t.id     = id;
    t.name   = name;
    t.action = a;
    t.kind   = k;
    return t;
}

int main() {
    // 自己建输出目录，不指望外部先建好。
    //
    // /tmp 会被系统按天清理，目录一没，后面每个用例的写文件都失败，控制台
    // 上就是十几条「失败」——看起来像代码坏了，其实只是环境被清了。
    // 这类假失败比真失败更费时间：它会把注意力引到完全无关的地方。
    mkdir("/tmp/pp_xml", 0755);

    SchedEnv env;
    // 2026-10-13 是星期二，20:00
    env.now       = PpMakeTime(2026, 10, 13, 20, 0, 0);
    env.systemDir = L"C:\\Windows\\System32\\";
    env.driveTag  = L"1A2B";
    env.machineFp = L"DEADBEEF";
    env.userSid   = L"S-1-5-21-1111111111-2222222222-3333333333-1001";

    printf("=== 环境自检 ===\n");
    CheckTrue(env.now.wDayOfWeek == 2, "2026-10-13 应为星期二（wDayOfWeek=2）");
    printf("  基准时间 2026-10-13 20:00，星期值 %u\n\n", (unsigned)env.now.wDayOfWeek);

    // =====================================================================
    //  1. StartBoundary 计算
    // =====================================================================
    printf("=== StartBoundary 计算 ===\n");
    {
        struct {
            const char*   label;
            PowerTask     t;
            const wchar_t* want;
        } cases[] = {
            { "倒计时 1800s",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Countdown,
                                         L"c", L"aaaaaaaa-0000-0000-0000-000000000001");
                  t.delaySeconds = 1800; return t; }(),
              L"<StartBoundary>2026-10-13T20:30:00</StartBoundary>" },

            { "指定时刻 23:00（今天未过）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::OnceAt,
                                         L"o", L"aaaaaaaa-0000-0000-0000-000000000002");
                  t.hour = 23; t.minute = 0; return t; }(),
              L"<StartBoundary>2026-10-13T23:00:00</StartBoundary>" },

            { "每天 18:30（今天已过 → 顺延明天）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                                         L"d", L"aaaaaaaa-0000-0000-0000-000000000003");
                  t.hour = 18; t.minute = 30; return t; }(),
              L"<StartBoundary>2026-10-14T18:30:00</StartBoundary>" },

            { "每周一三五 09:00（周二 → 下一个是周三）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Weekly,
                                         L"w", L"aaaaaaaa-0000-0000-0000-000000000004");
                  t.hour = 9; t.minute = 0; t.weekdayMask = 0x15; return t; }(),
              L"<StartBoundary>2026-10-14T09:00:00</StartBoundary>" },

            { "每周每天 23:00（今天未过 → 今天）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Weekly,
                                         L"w", L"aaaaaaaa-0000-0000-0000-000000000005");
                  t.hour = 23; t.minute = 0; t.weekdayMask = 0x7F; return t; }(),
              L"<StartBoundary>2026-10-13T23:00:00</StartBoundary>" },

            { "每天 20:00（与基准同一秒 → 视为已过）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                                         L"d", L"aaaaaaaa-0000-0000-0000-000000000006");
                  t.hour = 20; t.minute = 0; return t; }(),
              L"<StartBoundary>2026-10-14T20:00:00</StartBoundary>" },
        };

        for (auto& c : cases) {
            std::wstring xml, err;
            if (!SchedBuildXml(c.t, env, &xml, &err)) {
                Fail(std::string(c.label) + "：生成失败 " + ToUtf8(err));
                continue;
            }
            CheckHas(xml, c.want, c.label);
            printf("  %-40s → %s\n", c.label, ToUtf8(c.want).c_str());
        }
    }
    printf("\n");

    // =====================================================================
    //  2. 触发器元素
    // =====================================================================
    printf("=== 触发器元素 ===\n");
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Countdown,
                               L"t", L"bbbbbbbb-0000-0000-0000-000000000001");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<TimeTrigger>", "倒计时用 TimeTrigger");
        CheckNotHas(xml, L"<ScheduleByDay>", "倒计时不带 ScheduleByDay");
        CheckNotHas(xml, L"Repetition", "倒计时不带重复");
    }
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"t", L"bbbbbbbb-0000-0000-0000-000000000002");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<CalendarTrigger>", "每天用 CalendarTrigger");
        CheckHas(xml, L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>",
                 "每天 DaysInterval=1");
    }
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Weekly,
                               L"t", L"bbbbbbbb-0000-0000-0000-000000000003");
        t.weekdayMask = 0x15;   // 一、三、五
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<ScheduleByWeek>", "每周用 ScheduleByWeek");
        CheckHas(xml, L"<WeeksInterval>1</WeeksInterval>", "每周 WeeksInterval=1");
        CheckHas(xml, L"<Monday/><Wednesday/><Friday/>", "每周一二三五的 DaysOfWeek 顺序");
        CheckNotHas(xml, L"<Tuesday/>", "未勾选的星期二不应出现");
    }
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Weekly,
                               L"t", L"bbbbbbbb-0000-0000-0000-000000000004");
        t.weekdayMask = 0x7F;   // 每天
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<Monday/><Tuesday/><Wednesday/><Thursday/><Friday/><Saturday/><Sunday/>",
                 "全选时七天齐全");
    }
    printf("\n");

    // =====================================================================
    //  3. 动作命令与参数
    // =====================================================================
    printf("=== 动作命令与参数 ===\n");
    {
        struct { const char* label; PowerTask t; std::vector<const wchar_t*> want;
                 std::vector<const wchar_t*> notWant; } cs[] = {
            { "关机 / 提前提醒 60s / 提示文本",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000001");
                  t.noticeSeconds = 60; t.message = L"系统即将关机，请保存工作"; return t; }(),
              { L"<Command>C:\\Windows\\System32\\shutdown.exe</Command>",
                L"<Arguments>/s /t 60 /c \"系统即将关机，请保存工作\"</Arguments>" },
              { L"/r", L"/f" } },

            { "重启 / 不提醒 / 强制",
              []{ PowerTask t = MakeTask(PowerAction::Restart, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000002");
                  t.noticeSeconds = 0; t.forceClose = true; return t; }(),
              { L"<Arguments>/r /t 0 /f</Arguments>" },
              { L"/c ", L"<Arguments>/s" } },

            { "注销 / 强制",
              []{ PowerTask t = MakeTask(PowerAction::Logoff, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000003");
                  t.forceClose = true; return t; }(),
              { L"<Arguments>/l /f</Arguments>" },
              { L"/t ", L"/c " } },

            { "休眠 / 不强制（/h 不接受 /t 与 /c）",
              []{ PowerTask t = MakeTask(PowerAction::Hibernate, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000004");
                  return t; }(),
              { L"<Arguments>/h</Arguments>" },
              { L"/t ", L"/c ", L"/f" } },

            { "睡眠走 rundll32",
              []{ PowerTask t = MakeTask(PowerAction::Sleep, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000005");
                  return t; }(),
              { L"<Command>C:\\Windows\\System32\\rundll32.exe</Command>",
                L"<Arguments>powrprof.dll,SetSuspendState 0,1,0</Arguments>" },
              { L"shutdown.exe" } },

            { "锁定走 rundll32",
              []{ PowerTask t = MakeTask(PowerAction::Lock, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000006");
                  return t; }(),
              { L"<Command>C:\\Windows\\System32\\rundll32.exe</Command>",
                L"<Arguments>user32.dll,LockWorkStation</Arguments>" },
              { L"shutdown.exe" } },

            { "提示文本里的双引号会被去掉（否则拆坏参数引号）",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                                         L"x", L"cccccccc-0000-0000-0000-000000000007");
                  t.noticeSeconds = 30; t.message = L"他说\"马上关机\""; return t; }(),
              { L"<Arguments>/s /t 30 /c \"他说马上关机\"</Arguments>" },
              { L"&quot;" } },
        };

        for (auto& c : cs) {
            std::wstring xml, err;
            if (!SchedBuildXml(c.t, env, &xml, &err)) {
                Fail(std::string(c.label) + "：生成失败 " + ToUtf8(err));
                continue;
            }
            for (auto w : c.want)    CheckHas(xml, w, c.label);
            for (auto w : c.notWant) CheckNotHas(xml, w, c.label);
            printf("  %s\n", c.label);
        }
    }
    printf("\n");

    // =====================================================================
    //  4. 执行身份：关机用 SYSTEM，注销/锁定用当前用户
    // =====================================================================
    printf("=== 执行身份 ===\n");
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"x", L"dddddddd-0000-0000-0000-000000000001");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<UserId>S-1-5-18</UserId>", "关机用 Local System");
        CheckHas(xml, L"<LogonType>ServiceAccount</LogonType>", "关机用 ServiceAccount");
        CheckHas(xml, L"<RunLevel>HighestAvailable</RunLevel>", "RunLevel 最高权限");
    }
    {
        PowerTask t = MakeTask(PowerAction::Lock, TriggerKind::Daily,
                               L"x", L"dddddddd-0000-0000-0000-000000000002");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<UserId>S-1-5-21-1111111111-2222222222-3333333333-1001</UserId>",
                 "锁定用当前用户 SID");
        CheckHas(xml, L"<LogonType>InteractiveToken</LogonType>",
                 "锁定用 InteractiveToken（必须在交互式会话里执行）");
        CheckNotHas(xml, L"S-1-5-18", "锁定不应以 SYSTEM 身份执行");
    }
    {
        PowerTask t = MakeTask(PowerAction::Logoff, TriggerKind::Daily,
                               L"x", L"dddddddd-0000-0000-0000-000000000003");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<LogonType>InteractiveToken</LogonType>", "注销用 InteractiveToken");
    }
    printf("\n");

    // =====================================================================
    //  4.5 文档外壳（声明 / 命名空间 / RegistrationInfo）
    //
    //  这几条是「注册系统任务失败」那次的直接教训：外壳写错时服务端只回一个
    //  0x80041318，看不出是哪个元素的问题，只能在源头堵住。
    // =====================================================================
    printf("=== 文档外壳 ===\n");
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"外壳", L"cccccccc-0000-0000-0000-000000000001");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);

        CheckHas(xml, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>",
                 "XML 声明必须声明 UTF-16（与 BSTR / 落盘编码一致）");
        CheckHas(xml, L"<Task version=\"1.2\"", "version 必须是 1.2");
        // 用到的 AllowHardTerminate / Enabled / Hidden / RunOnlyIfIdle /
        // WakeToRun / ExecutionTimeLimit / Priority / RestartOnIdle 都是 1.2
        // 才引入的元素，写成 1.0 反而会被判为非法。这条断言防止有人「降版本」。
        CheckNotHas(xml, L"<Task version=\"1.0\"", "Task 的 version 不能降成 1.0");
        CheckHas(xml, L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"",
                 "必须声明任务计划程序命名空间");
        CheckHas(xml, L"<RegistrationInfo>", "应有 RegistrationInfo");
        CheckHas(xml, L"<Author>PlanPower</Author>", "Author 固定为 PlanPower");
        // 任务的路径由注册参数决定，服务端自己推导。手写 URI 一旦与注册路径
        // 不一致就会整段被拒，且报错指不到这个元素上。
        CheckNotHas(xml, L"<URI>", "RegistrationInfo 里不能出现 <URI>");
        // Description 里要留下任务 ID 与介质标识，日后清理时能对得上号
        CheckHas(xml, L"cccccccc-0000-0000-0000-000000000001", "Description 应带任务 ID");
        CheckHas(xml, L"/cleanup-all", "Description 应告知应急清理方式");
        // 说明文本里出现的用户输入不能破坏 XML：& < > 必须转义
        CheckHas(xml, L"<Description>由 PlanPower", "Description 应被正确包裹");
    }
    {
        // 名称里带 & < > 时，Description 里的任务名不会出现（XML 里不带名字），
        // 但提示文本会进 <Arguments>，那里的 & 必须转义，否则 XML 直接良构性失败。
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Countdown,
                               L"x", L"cccccccc-0000-0000-0000-000000000002");
        t.message = L"A & B < C > D";
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"A &amp; B &lt; C &gt; D", "提示文本里的 & < > 必须转义");
        CheckNotHas(xml, L"A & B", "不能留下未转义的裸 &");
    }
    printf("\n");

    // =====================================================================
    //  5. Settings 关键项（电池 / 补跑 / 唤醒）
    // =====================================================================
    printf("=== Settings 关键项 ===\n");
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"x", L"eeeeeeee-0000-0000-0000-000000000001");
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>",
                 "必须覆盖电池默认值（否则笔记本静默不执行）");
        CheckHas(xml, L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>",
                 "必须覆盖「转电池就停止」");
        CheckHas(xml, L"<StartWhenAvailable>true</StartWhenAvailable>", "错过补跑");
        CheckHas(xml, L"<WakeToRun>true</WakeToRun>", "唤醒执行");
        CheckHas(xml, L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>",
                 "不重复执行");
        CheckHas(xml, L"<Enabled>true</Enabled>", "启用状态");
    }
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"x", L"eeeeeeee-0000-0000-0000-000000000002");
        t.enabled = false;
        std::wstring xml, err;
        SchedBuildXml(t, env, &xml, &err);
        CheckHas(xml, L"<Enabled>false</Enabled>", "停用状态要如实写进 XML");
    }
    printf("\n");

    // =====================================================================
    //  6. 名称净化与任务名规范
    // =====================================================================
    printf("=== 名称净化与任务名规范 ===\n");
    {
        CheckTrue(SanitizeTaskName(L"关机/测试:任务*?") == L"关机_测试_任务__",
                  "非法字符替换为下划线");
        CheckTrue(SanitizeTaskName(L"  前后空格  ") == L"前后空格", "裁掉首尾空格");
        CheckTrue(SanitizeTaskName(L"   ") == L"", "全空格 → 空串");
        CheckTrue(SanitizeTaskName(std::wstring(60, L'字')).size() == 40, "长度截到 40");
        CheckTrue(SanitizeTaskName(L"正常名称") == L"正常名称", "正常名称原样保留");

        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"夜间关机", L"a1b2c3d4-1111-2222-3333-444444444444");
        CheckTrue(SchedTaskLeafName(t, L"1A2B") == L"夜间关机_1A2B_a1b2c3d4",
                  "任务名 = 名称_介质标识_任务ID前8位");
        CheckTrue(SchedTaskFullPath(t, L"1A2B") == L"\\PlanPower\\夜间关机_1A2B_a1b2c3d4",
                  "完整路径带 \\PlanPower 前缀");

        PowerTask e = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                               L"   ", L"a1b2c3d4-1111-2222-3333-444444444444");
        CheckTrue(SchedTaskLeafName(e, L"1A2B") == L"任务_1A2B_a1b2c3d4",
                  "名称为空时回落到「任务」");
    }
    printf("\n");

    // =====================================================================
    //  7. 不支持的动作与非法参数
    // =====================================================================
    printf("=== 不支持的动作与非法参数 ===\n");
    {
        PowerTask t = MakeTask(PowerAction::MonitorOff, TriggerKind::Daily,
                               L"x", L"ffffffff-0000-0000-0000-000000000001");
        std::wstring xml, err;
        bool ok = SchedBuildXml(t, env, &xml, &err);
        CheckTrue(!ok, "关闭显示器应被拒绝");
        CheckTrue(!err.empty(), "拒绝时要有原因说明");
        printf("  关闭显示器 → %s\n", ToUtf8(err).c_str());
    }
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Weekly,
                               L"x", L"ffffffff-0000-0000-0000-000000000002");
        t.weekdayMask = 0;
        std::wstring xml, err;
        bool ok = SchedBuildXml(t, env, &xml, &err);
        CheckTrue(!ok, "每周但一天都没勾选应被拒绝");
        printf("  每周未勾选 → %s\n", ToUtf8(err).c_str());
    }
    {
        PowerTask t = MakeTask(PowerAction::Lock, TriggerKind::Daily,
                               L"x", L"ffffffff-0000-0000-0000-000000000003");
        SchedEnv noSid = env;
        noSid.userSid.clear();
        std::wstring xml, err;
        bool ok = SchedBuildXml(t, noSid, &xml, &err);
        CheckTrue(!ok, "取不到用户 SID 时锁定应被拒绝");
    }
    printf("\n");

    // =====================================================================
    //  8. 定义风格：完整版 / 精简版
    //
    //     真机上出现过服务端以 SCHED_E_INVALIDVALUE（0x80041318）整段拒绝
    //     任务定义。sched.cpp 的对策是按顺序降级重试，所以两套定义都必须
    //     自洽：语义项一个不少，冗余项一个不多。任何一套写坏了，降级链就
    //     等于把用户从「注册失败」换成「注册成一条行为不对的任务」。
    // =====================================================================
    printf("=== 定义风格（完整 / 精简）===\n");
    {
        PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Countdown,
                               L"风格对比", L"88888888-8888-8888-8888-888888888888");
        t.delaySeconds  = 600;
        t.noticeSeconds = 30;
        t.message       = L"风格对比";

        SchedXmlOpts rich;
        rich.style   = SchedXmlStyle::Rich;
        rich.version = L"1.2";

        SchedXmlOpts lean;
        lean.style   = SchedXmlStyle::Conservative;
        lean.version = L"1.4";

        std::wstring rx, lx, err;
        CheckTrue(SchedBuildXml(t, env, rich, &rx, &err), "完整定义应能生成");
        CheckTrue(SchedBuildXml(t, env, lean, &lx, &err), "精简定义应能生成");

        // 只取 <Settings> 段。触发器自己也有 <Enabled>true</Enabled>，拿整份
        // XML 去查 <Enabled> 会误报——这条断言第一版就是这么写错的。
        auto SettingsBlock = [](const std::wstring& x) -> std::wstring {
            size_t a = x.find(L"<Settings>");
            size_t b = x.find(L"</Settings>");
            if (a == std::wstring::npos || b == std::wstring::npos) return std::wstring();
            return x.substr(a, b - a);
        };

        // version 要如实反映到属性上——降级链靠它区分两种写法
        CheckHas(rx, L"<Task version=\"1.2\"", "完整定义用 1.2");
        CheckHas(lx, L"<Task version=\"1.4\"", "精简定义用 1.4");

        // 完整定义保留 Principals；精简定义整段省掉，身份交给注册参数
        CheckHas(rx, L"<Principals>", "完整定义应写 Principals");
        CheckHas(rx, L"<RunLevel>HighestAvailable</RunLevel>", "完整定义应写 RunLevel");
        CheckNotHas(lx, L"<Principals>", "精简定义不应写 Principals");
        CheckNotHas(lx, L"<RunLevel>", "精简定义不应写 RunLevel");
        CheckNotHas(lx, L"<LogonType>", "精简定义不应写 LogonType");

        // 两套定义都必须保住「会改变行为」的项
        for (int i = 0; i < 2; ++i) {
            const std::wstring& x = i ? lx : rx;
            std::string who = i ? "精简" : "完整";
            CheckHas(x, L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>",
                     who + "定义必须解除电池限制");
            CheckHas(x, L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>",
                     who + "定义必须解除电池中断");
            CheckHas(x, L"<StartWhenAvailable>true</StartWhenAvailable>",
                     who + "定义必须支持错过补跑");
            CheckHas(x, L"<WakeToRun>true</WakeToRun>",
                     who + "定义必须支持唤醒执行");
            CheckHas(x, L"<ExecutionTimeLimit>PT10M</ExecutionTimeLimit>",
                     who + "定义必须显式给出执行时限");
            CheckHas(SettingsBlock(x), L"<Enabled>true</Enabled>",
                     who + "定义必须写出启停状态");
            CheckHas(x, L"<Actions Context=\"Author\">",
                     who + "定义必须有 Actions");
            CheckNotHas(x, L"<URI>", who + "定义不得写 <URI>");
        }

        // 精简定义去掉的必须都是「等于默认值」的冗余项
        CheckNotHas(lx, L"<IdleSettings>",
                    "精简定义不应写 IdleSettings（RunOnlyIfIdle=false 时无意义）");
        CheckNotHas(lx, L"<AllowStartOnDemand>",
                    "精简定义不应写 AllowStartOnDemand（默认 true）");
        CheckNotHas(lx, L"<Priority>", "精简定义不应写 Priority（默认 7）");
        CheckNotHas(lx, L"<Hidden>", "精简定义不应写 Hidden（默认 false）");
        CheckNotHas(lx, L"<RunOnlyIfIdle>", "精简定义不应写 RunOnlyIfIdle（默认 false）");
        CheckNotHas(lx, L"<RunOnlyIfNetworkAvailable>",
                    "精简定义不应写 RunOnlyIfNetworkAvailable（默认 false）");
        CheckNotHas(lx, L"<AllowHardTerminate>",
                    "精简定义不应写 AllowHardTerminate（默认 true）");

        // 完整定义仍保留它们，用于排查服务端口味
        CheckHas(rx, L"<IdleSettings>", "完整定义保留 IdleSettings");
        CheckHas(rx, L"<Priority>7</Priority>", "完整定义保留 Priority");

        // 启停状态在精简定义里也必须如实反映
        PowerTask off = t;
        off.enabled = false;
        std::wstring ox, oerr;
        CheckTrue(SchedBuildXml(off, env, lean, &ox, &oerr), "停用任务的精简定义应能生成");
        CheckHas(SettingsBlock(ox), L"<Enabled>false</Enabled>", "停用状态必须写进精简定义");
        CheckNotHas(SettingsBlock(ox), L"<Enabled>true</Enabled>", "停用任务不得写成 true");
        CheckHas(SettingsBlock(lx), L"<Enabled>true</Enabled>", "启用状态必须写进精简定义");

        // 精简定义也要覆盖三种触发方式
        for (int k = 0; k < 3; ++k) {
            PowerTask q = t;
            q.kind = (TriggerKind)k;
            q.hour = 9;
            q.minute = 15;
            q.weekdayMask = 0x3F;
            std::wstring qx, qerr;
            CheckTrue(SchedBuildXml(q, env, lean, &qx, &qerr),
                      "精简定义应支持第 " + std::to_string(k) + " 种触发方式");
        }

        // 精简定义下的动作覆盖：睡眠 / 锁定走 rundll32，也必须有完整 Exec
        for (int a = 0; a <= (int)PowerAction::MonitorOff; ++a) {
            PowerAction act = (PowerAction)a;
            if (!ActionSupportsPersistent(act)) continue;
            PowerTask q = t;
            q.action = act;
            q.forceClose = true;
            std::wstring qx, qerr;
            CheckTrue(SchedBuildXml(q, env, lean, &qx, &qerr),
                      "精简定义应支持动作 " + std::to_string(a));
            CheckHas(qx, L"<Exec><Command>", "精简定义的动作必须有 Command");
        }
    }
    printf("\n");

    // =====================================================================
    //  9. 落盘，交给 Python 用真正的 XML 解析器再验一遍
    // =====================================================================
    printf("=== 导出样本供 XML 解析器校验 ===\n");
    {
        struct { const char* file; PowerTask t; } samples[] = {
            { "01_countdown_shutdown.xml",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Countdown,
                                         L"30 分钟后关机", L"11111111-1111-1111-1111-111111111111");
                  t.delaySeconds = 1800; t.noticeSeconds = 60;
                  t.message = L"系统即将关机，请保存工作"; return t; }() },
            { "02_onceat_restart.xml",
              []{ PowerTask t = MakeTask(PowerAction::Restart, TriggerKind::OnceAt,
                                         L"凌晨重启", L"22222222-2222-2222-2222-222222222222");
                  t.hour = 3; t.minute = 30; t.noticeSeconds = 300; return t; }() },
            { "03_daily_logoff.xml",
              []{ PowerTask t = MakeTask(PowerAction::Logoff, TriggerKind::Daily,
                                         L"下班注销", L"33333333-3333-3333-3333-333333333333");
                  t.hour = 18; t.minute = 30; t.forceClose = true; return t; }() },
            { "04_weekly_sleep.xml",
              []{ PowerTask t = MakeTask(PowerAction::Sleep, TriggerKind::Weekly,
                                         L"工作日午休", L"44444444-4444-4444-4444-444444444444");
                  t.hour = 12; t.minute = 30; t.weekdayMask = 0x1F; return t; }() },
            { "05_daily_hibernate.xml",
              []{ PowerTask t = MakeTask(PowerAction::Hibernate, TriggerKind::Daily,
                                         L"夜间休眠", L"55555555-5555-5555-5555-555555555555");
                  t.hour = 2; t.minute = 0; return t; }() },
            { "06_weekly_lock.xml",
              []{ PowerTask t = MakeTask(PowerAction::Lock, TriggerKind::Weekly,
                                         L"下班锁定", L"66666666-6666-6666-6666-666666666666");
                  t.hour = 19; t.minute = 0; t.weekdayMask = 0x7F; return t; }() },
            { "07_name_with_symbols.xml",
              []{ PowerTask t = MakeTask(PowerAction::Shutdown, TriggerKind::Daily,
                                         L"关机/测试:任务*?", L"77777777-7777-7777-7777-777777777777");
                  t.hour = 23; t.minute = 0; t.enabled = false; return t; }() },
        };

        for (auto& s : samples) {
            std::wstring xml, err;
            if (!SchedBuildXml(s.t, env, &xml, &err)) {
                Fail(std::string("样本生成失败 ") + s.file + "：" + ToUtf8(err));
                continue;
            }
            WriteUtf16Le(std::string("/tmp/pp_xml/") + s.file, xml);
            printf("  已写出 %s（%u 字符）\n", s.file, (unsigned)xml.size());

            // 同一批任务再导一份精简定义（文件名前缀 lean_）。
            // 降级链上真正会被用到的就是这一份，必须同样经得起解析器校验。
            SchedXmlOpts lean;
            lean.style   = SchedXmlStyle::Conservative;
            lean.version = L"1.4";
            std::wstring lx, lerr;
            if (!SchedBuildXml(s.t, env, lean, &lx, &lerr)) {
                Fail(std::string("精简样本生成失败 ") + s.file + "：" + ToUtf8(lerr));
                continue;
            }
            WriteUtf16Le(std::string("/tmp/pp_xml/lean_") + s.file, lx);
            printf("  已写出 lean_%s（%u 字符）\n", s.file, (unsigned)lx.size());
        }
    }

    printf("\n========================================\n");
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
