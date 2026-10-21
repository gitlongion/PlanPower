#!/usr/bin/env python3
# ============================================================================
#  PlanPower 任务 XML 结构校验
#
#  输入：xmltest 生成的 /tmp/pp_xml/*.xml 与 lean_*.xml（UTF-16LE + BOM，
#        真实代码产出）
#  做法：用真正的 XML 解析器解析，再按 Task Scheduler 的 schema 逐项断言。
#
#  之所以要这一层：C++ 那边的断言是「字符串包含」级别的，只能证明"写进去了"。
#  这一层证明的是"解析出来是对的"——元素层级、命名空间、取值都对得上，
#  而且 XML 本身是良构的。
#
#  两套定义都要验：
#    · 完整版（01_*.xml）      —— version 1.2 + Principals + 全套 Settings
#    · 精简版（lean_01_*.xml） —— version 1.4 + 无 Principals + 只留语义项
#  精简版是降级链上真正会被用到的那一份，它写坏了等于把"注册失败"换成
#  "注册成一条行为不对的任务"，比直接失败更糟，所以必须同等严格。
# ============================================================================
import os
import sys
import xml.etree.ElementTree as ET

NS = "{http://schemas.microsoft.com/windows/2004/02/mit/task}"
DIR = "/tmp/pp_xml"

# Task Scheduler 的 settingsType 允许的全部子元素。
# 出现不在表里的名字 = 写错了元素名，服务端会整段拒绝（SCHED_E_UNEXPECTEDNODE）。
SETTINGS_ALLOWED = {
    "AllowStartOnDemand", "RestartOnFailure", "MultipleInstancesPolicy",
    "DisallowStartIfOnBatteries", "StopIfGoingOnBatteries", "AllowHardTerminate",
    "StartWhenAvailable", "NetworkProfileName", "RunOnlyIfNetworkAvailable",
    "IdleSettings", "NetworkSettings", "AllowStartIfOnBatteries", "Hidden",
    "RunOnlyIfIdle", "WakeToRun", "ExecutionTimeLimit", "Priority",
    "RestartOnIdle", "Enabled", "UseUnifiedSchedulingEngine",
    "MaintenanceSettings", "Volatile", "DeleteExpiredTaskAfter",
}

# 会改变行为的项：两套定义都必须逐字写出，缺一项就是行为退化。
SETTINGS_SEMANTIC = {
    "DisallowStartIfOnBatteries": "false",
    "StopIfGoingOnBatteries": "false",
    "StartWhenAvailable": "true",
    "WakeToRun": "true",
    "ExecutionTimeLimit": "PT10M",
}

# 与服务端默认值等价的冗余项：精简定义必须一个都不写。
# 每多写一项，就多一处可能被服务端判为非法的取值（0x80041318 的教训）。
SETTINGS_REDUNDANT = {
    "AllowHardTerminate", "RunOnlyIfNetworkAvailable", "IdleSettings",
    "AllowStartOnDemand", "Hidden", "RunOnlyIfIdle", "Priority",
}

passed = 0
failed = 0
failures = []


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        failures.append(what)


def load(name):
    path = os.path.join(DIR, name)
    with open(path, "rb") as f:
        raw = f.read()
    check(raw[:2] == b"\xff\xfe", f"{name}: 应为 UTF-16LE + BOM")
    text = raw.decode("utf-16")
    check(text.startswith('<?xml version="1.0" encoding="UTF-16"?>'),
          f"{name}: XML 声明应与实际编码一致")
    return ET.fromstring(text), text


def common_checks(name, root, text, style):
    """所有任务定义都该满足的结构要求。style: "rich" | "lean"。
    返回 (Exec 元素, Principal 元素或 None)。"""
    lean = (style == "lean")

    check(root.tag == NS + "Task", f"{name}: 根元素应为 Task（含命名空间）")
    want_ver = "1.4" if lean else "1.2"
    check(root.get("version") == want_ver,
          f"{name}: {style} 的 Task version 应为 {want_ver}，实际 {root.get('version')}")
    # xmlns 是命名空间声明，解析后不会留在属性表里，只能查原文
    check('xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task"' in text,
          f"{name}: 缺少 Task Scheduler 命名空间声明")

    ri = root.find(NS + "RegistrationInfo")
    check(ri is not None, f"{name}: 缺少 RegistrationInfo")
    check(ri.find(NS + "Author").text == "PlanPower", f"{name}: Author 应为 PlanPower")
    desc = ri.find(NS + "Description").text
    check("/cleanup-all" in desc, f"{name}: Description 应告知应急清理方式")
    # RegistrationInfo 里绝不能出现 <URI>。
    # 任务的路径由 RegisterTaskDefinition 的第一个参数决定，服务端自己推导；
    # 手写一个以反斜杠开头的 URI，一旦与注册路径有任何出入，就会以
    # 0x80041318（XML 取值非法）被整段拒绝，而报错完全指不到这个元素上。
    # 这条断言就是防止以后有人「顺手」把它加回来。
    check(ri.find(NS + "URI") is None,
          f"{name}: RegistrationInfo 里不应出现 <URI>（路径由注册参数决定）")
    kids = sorted(c.tag[len(NS):] for c in ri)
    check(kids == ["Author", "Description"],
          f"{name}: RegistrationInfo 应只含 Author 与 Description，实际 {kids}")

    trigs = root.find(NS + "Triggers")
    check(trigs is not None and len(list(trigs)) == 1, f"{name}: 应恰好有一个触发器")

    # Principals：完整版必须有且取值明确；精简版必须整段不出现
    # ——身份交给 RegisterTaskDefinition 的 userId / logonType 参数。
    prin = None
    ps = root.find(NS + "Principals")
    if lean:
        check(ps is None, f"{name}: 精简定义不应出现 Principals")
    else:
        check(ps is not None, f"{name}: 完整定义应有 Principals")
        if ps is not None:
            prin = ps.find(NS + "Principal")
            check(prin is not None and prin.get("id") == "Author",
                  f"{name}: Principal id 应为 Author")
            if prin is not None:
                check(prin.find(NS + "UserId") is not None, f"{name}: 缺少 UserId")
                check(prin.find(NS + "LogonType") is not None, f"{name}: 缺少 LogonType")
                check(prin.find(NS + "RunLevel").text == "HighestAvailable",
                      f"{name}: RunLevel 应为 HighestAvailable")

    st = root.find(NS + "Settings")
    check(st is not None, f"{name}: 缺少 Settings")
    kids = [c.tag[len(NS):] for c in st]

    # 元素名必须全在 schema 允许集内——写错一个字母就是整段被拒
    unknown = [k for k in kids if k not in SETTINGS_ALLOWED]
    check(not unknown, f"{name}: Settings 里出现了非法子元素 {unknown}")

    # 会改变行为的项，逐字对上
    for key, want in SETTINGS_SEMANTIC.items():
        node = st.find(NS + key)
        check(node is not None and node.text == want,
              f"{name}: Settings.{key} 应为 {want}")

    check(st.find(NS + "Enabled") is not None, f"{name}: Settings 应写出 Enabled")

    if lean:
        extra = [k for k in kids if k in SETTINGS_REDUNDANT]
        check(not extra,
              f"{name}: 精简定义的 Settings 里不该出现冗余项 {extra}（它们都等于默认值）")
    else:
        # 完整版保留这些，用于排查服务端口味；至少要有几项标志性的
        for key in ("IdleSettings", "Priority", "MultipleInstancesPolicy"):
            check(st.find(NS + key) is not None,
                  f"{name}: 完整定义的 Settings 应含 {key}")

    acts = root.find(NS + "Actions")
    check(acts is not None and acts.get("Context") == "Author",
          f"{name}: Actions Context 应为 Author")
    ex = acts.find(NS + "Exec")
    check(ex is not None, f"{name}: 缺少 Exec 动作")
    check(ex.find(NS + "Command") is not None, f"{name}: 缺少 Command")

    return ex, prin


def logon_of(prin):
    return None if prin is None else prin.find(NS + "LogonType").text


# 每个样本：文件、附加断言（rich 用）、附加断言（lean 用）
def extra_01(root, ex, name):
    tt = root.find(NS + "Triggers").find(NS + "TimeTrigger")
    check(tt is not None, f"{name}: 倒计时应用 TimeTrigger")
    check(tt.find(NS + "StartBoundary").text == "2026-10-13T20:30:00",
          f"{name}: 基准 20:00 + 1800 秒 = 20:30")
    check(ex.find(NS + "Command").text == r"C:\Windows\System32\shutdown.exe",
          f"{name}: 命令应为 shutdown.exe 的绝对路径")
    check(ex.find(NS + "Arguments").text == '/s /t 60 /c "系统即将关机，请保存工作"',
          f"{name}: 提前提醒 60 秒应变成 /t 60，提示文本应进 /c")


def extra_02(root, ex, name):
    tt = root.find(NS + "Triggers").find(NS + "TimeTrigger")
    check(tt is not None, f"{name}: 指定时刻应用 TimeTrigger")
    check(tt.find(NS + "StartBoundary").text == "2026-10-14T03:30:00",
          f"{name}: 03:30 已过基准 20:00，应顺延到次日")
    check(ex.find(NS + "Arguments").text == "/r /t 300",
          f"{name}: 重启应为 /r /t 300")
    # 一次性触发器不能有重复定义，否则会变成每天跑
    check(tt.find(NS + "Repetition") is None, f"{name}: 一次性触发器不应带 Repetition")


def extra_03(root, ex, name):
    ct = root.find(NS + "Triggers").find(NS + "CalendarTrigger")
    check(ct is not None, f"{name}: 每天应用 CalendarTrigger")
    check(ct.find(NS + "ScheduleByDay") is not None, f"{name}: 应有 ScheduleByDay")
    check(ct.find(NS + "ScheduleByDay").find(NS + "DaysInterval").text == "1",
          f"{name}: DaysInterval 应为 1")
    check(ex.find(NS + "Arguments").text == "/l /f", f"{name}: 注销应为 /l /f")


def extra_04(root, ex, name):
    ct = root.find(NS + "Triggers").find(NS + "CalendarTrigger")
    wk = ct.find(NS + "ScheduleByWeek")
    check(wk is not None, f"{name}: 每周应用 ScheduleByWeek")
    check(wk.find(NS + "WeeksInterval").text == "1", f"{name}: WeeksInterval 应为 1")
    days = [d.tag[len(NS):] for d in wk.find(NS + "DaysOfWeek")]
    check(days == ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday"],
          f"{name}: 工作日应为周一到周五，实际 {days}")
    check(ex.find(NS + "Command").text == r"C:\Windows\System32\rundll32.exe",
          f"{name}: 睡眠应走 rundll32")
    check(ex.find(NS + "Arguments").text == "powrprof.dll,SetSuspendState 0,1,0",
          f"{name}: 睡眠参数应为 SetSuspendState 0,1,0")


def extra_05(root, ex, name):
    check(ex.find(NS + "Command").text == r"C:\Windows\System32\shutdown.exe",
          f"{name}: 休眠应走 shutdown.exe")
    check(ex.find(NS + "Arguments").text == "/h",
          f"{name}: 休眠应为 /h（/h 不接受 /t 与 /c）")


def extra_06(root, ex, name):
    wk = root.find(NS + "Triggers").find(NS + "CalendarTrigger").find(NS + "ScheduleByWeek")
    days = [d.tag[len(NS):] for d in wk.find(NS + "DaysOfWeek")]
    check(days == ["Monday", "Tuesday", "Wednesday", "Thursday",
                   "Friday", "Saturday", "Sunday"],
          f"{name}: 全选应为七天，实际 {days}")
    check(ex.find(NS + "Command").text == r"C:\Windows\System32\rundll32.exe",
          f"{name}: 锁定应走 rundll32")
    check(ex.find(NS + "Arguments").text == "user32.dll,LockWorkStation",
          f"{name}: 锁定参数应为 LockWorkStation")


def extra_07(root, ex, name):
    # 任务名的净化规则（非法字符替换、介质标识、ID 前 8 位）由 C++
    # 侧的 SchedTaskLeafName 断言负责，XML 里不再携带这个名字。
    # 这里改为验证 Description 里的归属信息确实指向这个任务。
    desc = root.find(NS + "RegistrationInfo").find(NS + "Description").text
    check("77777777-7777-7777-7777-777777777777" in desc,
          f"{name}: Description 应带上任务 ID 便于日后认领，实际 {desc}")
    check(root.find(NS + "Settings").find(NS + "Enabled").text == "false",
          f"{name}: 停用状态应如实写进 XML")


SAMPLES = [
    ("01_countdown_shutdown.xml", extra_01, "01"),
    ("02_onceat_restart.xml",     extra_02, "02"),
    ("03_daily_logoff.xml",       extra_03, "03"),
    ("04_weekly_sleep.xml",       extra_04, "04"),
    ("05_daily_hibernate.xml",    extra_05, "05"),
    ("06_weekly_lock.xml",        extra_06, "06"),
    ("07_name_with_symbols.xml",  extra_07, "07"),
]


def main():
    if not os.path.isdir(DIR):
        print(f"目录不存在：{DIR}")
        return 1

    # 同一批任务的两套定义：完整版（Principals + 全套 Settings）与精简版
    # （无 Principals + 只留语义项）。两者都必须过同一组结构断言，
    # 再加各自特有的断言。
    for fname, extra, tag in SAMPLES:
        for prefix, style in (("", "rich"), ("lean_", "lean")):
            path = prefix + fname
            name = ("精简 " if style == "lean" else "完整 ") + tag
            root, text = load(path)
            ex, prin = common_checks(name, root, text, style)
            extra(root, ex, name)
            # 身份只在完整版里显式写；精简版由注册参数决定
            # 01 关机 / 02 重启 / 04 睡眠 / 05 休眠 / 07 关机（名字含符号）
            #   → SYSTEM，锁屏或无人登录时也要能执行
            # 03 注销 / 06 锁定 → 当前用户，必须在交互式会话里执行
            if style == "rich":
                if tag in ("03", "06"):
                    check(logon_of(prin) == "InteractiveToken",
                          f"{name}: 注销/锁定必须在交互式会话里执行")
                else:
                    check(logon_of(prin) == "ServiceAccount",
                          f"{name}: 关机/重启/睡眠/休眠应以 SYSTEM 身份执行")

    print(f"XML 解析与结构校验：通过 {passed} 项，失败 {failed} 项")
    for f in failures:
        print("  FAIL ", f)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
