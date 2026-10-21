#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PlanPower 布局静态校验
======================
macOS 上跑不了 exe，所以把 ui.cpp 里的布局算术在 Python 里复刻一遍，
再做两类检查：

  A. 几何：四种触发方式下，LayoutEditor 走完的 y 是否等于
     EditorClientHeight(kind)；控件之间是否重叠、是否越界。
  B. 不变量：MakeEditorControls 里创建过的每一个带 ID 的控件，
     是否都在 LayoutEditor 的可见性块里被显式声明过。

B 这一条是上次「提前提醒输入框凭空消失」那个 bug 的直接对策——
当时的成因就是控件创建了但没在任何一处被 ShowWindow。
"""
import re
import sys
from pathlib import Path

UI = Path(sys.argv[1]) if len(sys.argv) > 1 else \
     Path(__file__).resolve().parent.parent / "src" / "ui.cpp"
text = UI.read_text(encoding="utf-8")

fails, checks = [], 0


def ck(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# ---------------------------------------------------------------------------
# 复刻布局算术（单位：逻辑像素，S() 只做整体缩放，不影响相对关系）
# ---------------------------------------------------------------------------
ED_PAD = 24
ED_W_LOGICAL = 480
ED_ROW_GAP = 58
ED_BOX_W = 72
ED_UNIT_W = 40
ED_EDIT_H = 26


def params_top():        return 204
def mode_row_h():        return 92
def mode_mem_w():        return 148
def mode_sys_w():        return 168
def mode_hint_h():       return 38


def editor_client_height(kind):
    h = params_top() + ED_ROW_GAP
    if kind == 1:          # 「指定时刻」多一行日期
        h += ED_ROW_GAP
    if kind == 3:
        h += 36
    h += mode_row_h()
    h += 58 + 34 + 58
    h += 32 + 24
    return h


def layout_editor(kind):
    """返回 (最终 y, {控件名: (x, y, w, h)})"""
    x = ED_PAD
    w = ED_W_LOGICAL - ED_PAD * 2
    boxW, unitW, eh = ED_BOX_W, ED_UNIT_W, ED_EDIT_H
    show_delay = (kind == 0)
    show_at = (kind >= 1)
    show_date = (kind == 1)
    show_wd = (kind == 3)

    rects = {}
    y = params_top()

    if show_delay:
        by = y + 22
        bx = x
        rects["HOURS"] = (bx, by, boxW, eh); bx += boxW + 8
        rects["U_HOUR"] = (bx, by, unitW, eh); bx += unitW + 16
        rects["MINS"] = (bx, by, boxW, eh); bx += boxW + 8
        rects["U_MIN"] = (bx, by, unitW, eh)
        rects["LBL_DELAY"] = (x, y, w, 20)
        y += ED_ROW_GAP

    if show_date:
        by = y + 22
        bx = x
        rects["DATE_Y"] = (bx, by, 88, eh); bx += 88 + 8
        rects["U_DATE_Y"] = (bx, by, 28, eh); bx += 28 + 16
        rects["DATE_M"] = (bx, by, 56, eh); bx += 56 + 8
        rects["U_DATE_M"] = (bx, by, 28, eh); bx += 28 + 16
        rects["DATE_D"] = (bx, by, 56, eh); bx += 56 + 8
        rects["U_DATE_D"] = (bx, by, 28, eh)
        rects["LBL_DATE"] = (x, y, w, 20)
        y += ED_ROW_GAP

    if show_at:
        by = y + 22
        bx = x
        rects["AT_H"] = (bx, by, boxW, eh); bx += boxW + 8
        rects["U_AT_H"] = (bx, by, unitW, eh); bx += unitW + 16
        rects["AT_M"] = (bx, by, boxW, eh); bx += boxW + 8
        rects["U_AT_M"] = (bx, by, unitW, eh)
        rects["LBL_AT"] = (x, y, w, 20)
        y += ED_ROW_GAP

    if show_wd:
        bx = x
        for i in range(7):
            rects["WD%d" % i] = (bx, y, 58, 24)
            bx += 60
        y += 36

    # ---- 执行方式 ----
    rects["LBL_MODE"] = (x, y, w, 20)
    ry = y + 24
    rects["MODE_MEM"] = (x, ry, mode_mem_w(), 22)
    rects["MODE_SYS"] = (x + mode_mem_w() + 8, ry, mode_sys_w(), 22)
    rects["MODE_HINT"] = (x, ry + 26, w, mode_hint_h())
    y += mode_row_h()

    rects["LBL_NOTICE"] = (x, y, w, 20)
    rects["NOTICE"] = (x, y + 22, boxW, eh)
    y += 58

    rects["FORCE"] = (x, y, w, 24)
    y += 34

    rects["LBL_MSG"] = (x, y, w, 20)
    rects["MESSAGE"] = (x, y + 22, w, eh)
    y += 58

    rects["OK"] = (x + w - 200, y, 96, 32)
    rects["CANCEL"] = (x + w - 96, y, 96, 32)
    y += 32

    return y, rects


def overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)


KIND_NAMES = {0: "倒计时", 1: "指定时刻", 2: "每天", 3: "每周"}

for kind in range(4):
    y, rects = layout_editor(kind)
    client = y + 24
    expect = editor_client_height(kind)
    ck(client == expect,
       "kind=%d %s：LayoutEditor 算出客户区高 %d，EditorClientHeight 说 %d"
       % (kind, KIND_NAMES[kind], client, expect))

    # 越界
    for name, (rx, ry, rw, rh) in rects.items():
        ck(rx >= 0 and ry >= 0,
           "kind=%d %s：%s 坐标为负 (%d,%d)" % (kind, KIND_NAMES[kind], name, rx, ry))
        ck(rx + rw <= ED_W_LOGICAL,
           "kind=%d %s：%s 右边界 %d 超出窗口宽 %d"
           % (kind, KIND_NAMES[kind], name, rx + rw, ED_W_LOGICAL))
        ck(ry + rh <= client,
           "kind=%d %s：%s 下边界 %d 超出客户区高 %d"
           % (kind, KIND_NAMES[kind], name, ry + rh, client))

    # 重叠（同排的标签与输入框高度错开，这里只查真正相交的）
    names = sorted(rects)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = names[i], names[j]
            ck(not overlap(rects[a], rects[b]),
               "kind=%d %s：%s 与 %s 重叠 %s %s"
               % (kind, KIND_NAMES[kind], a, b, rects[a], rects[b]))

# ---------------------------------------------------------------------------
# B. 不变量：创建的控件必须被显式声明可见性
# ---------------------------------------------------------------------------
def slice_between(src, start_pat, end_pat):
    m = re.search(start_pat, src)
    if not m:
        return ""
    m2 = re.search(end_pat, src[m.end():])
    return src[m.end(): m.end() + (m2.start() if m2 else len(src))]


def strip_comments(src):
    """剥掉 // 行注释，但保留字符串字面量里的内容。

    做源码级断言前必须先过这一道：本项目的注释习惯是「把元素名写出来解释
    为什么保留 / 为什么省掉」，直接查名字会把注释里的提及当成真代码，
    报出根本不存在的缺陷（踩过一次）。字符串字面量要原样保留，否则
    L"http://schemas..." 这种带 // 的字面量会被截断。
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


make = slice_between(text, r"static void MakeEditorControls\(HWND h\) \{",
                     r"\n// 按触发方式显隐 \+ 重排")
lay = slice_between(text, r"static void LayoutEditor\(HWND h, int kind\) \{",
                    r"\nstatic void ShowEditor")

ck(len(make) > 500, "没能切出 MakeEditorControls 函数体")
ck(len(lay) > 500, "没能切出 LayoutEditor 函数体")

# 创建时带 ID 的控件：label(IDC_x, ...)、numEdit(IDC_x, ...)、unit(IDC_x, ...)
created = set(re.findall(r"(?:label|numEdit|unit|edit)\(\s*(IDC_[A-Z0-9_]+)", make))
created |= set(re.findall(r"\(HMENU\)(?:\(INT_PTR\))?(IDC_[A-Z0-9_]+)", make))
# 星期复选框是循环创建的
if "IDC_WD_BASE + i" in make:
    created.add("IDC_WD_BASE")

# LayoutEditor 里显式声明的可见性
declared = set(re.findall(r"vis\(C\((IDC_[A-Z0-9_]+)\)", lay))
if "C(IDC_WD_BASE + i)" in lay:
    declared.add("IDC_WD_BASE")

missing = sorted(created - declared)
ck(not missing,
   "创建了但没在任何一处声明可见性的控件：%s" % ", ".join(missing))

# 反向：声明了但根本没创建的（多半是 ID 写错）
ghost = sorted(declared - created)
ck(not ghost, "声明了可见性但从未创建的控件：%s" % ", ".join(ghost))

# ---------------------------------------------------------------------------
# C. 设置窗几何
# ---------------------------------------------------------------------------
ST_W = 560
ST_PAD = 24


def settings_layout():
    x = ST_PAD
    w = ST_W - ST_PAD * 2
    rects = {}
    y = 14
    rects["SEC1"] = (x, y, w, 18); y += 24
    rects["AUTOSTART"] = (x, y, w, 22); y += 24
    rects["TRAY"] = (x, y, w, 22); y += 24
    rects["HINT1"] = (x, y, w, 50); y += 58

    rects["SEC2"] = (x, y, w, 18); y += 24
    rects["LOG"] = (x, y, w, 22); y += 24
    # 顺序必须和 SettingsLayout 里一致：日志保留 → 日志说明 → 快捷时长。
    # 这里曾经只有 QUICK 一组，加 KEEP 之后如果只改源码不改这里，
    # 复刻出来的高度会比真实窗口少 56，而断言照样「全部通过」。
    rects["KLBL"] = (x, y, w, 20); y += 22
    rects["KEEP"] = (x, y, 80, 26); y += 34
    rects["HINT2"] = (x, y, w, 34); y += 42
    rects["QLBL"] = (x, y, w, 20); y += 22
    rects["QUICK"] = (x, y, 200, 26); y += 34

    rects["SEC3"] = (x, y, w, 18); y += 24
    rects["ALLOW"] = (x, y, w, 22); y += 24
    rects["HINT3"] = (x, y, w, 50); y += 58
    rects["CLEAN"] = (x, y, 148, 30)
    rects["SYSINFO"] = (x + 158, y + 6, w - 158, 18)
    y += 40

    rects["SEC4"] = (x, y, w, 18); y += 24
    rects["HINT4"] = (x, y, w, 38); y += 46
    rects["OPEN"] = (x, y, 148, 30); y += 40

    rects["CLOSE"] = (x + w - 96, y, 96, 32); y += 32
    return y + 18, rects


sh, srects = settings_layout()
for name, (rx, ry, rw, rh) in srects.items():
    ck(rx >= 0 and ry >= 0, "设置窗 %s 坐标为负" % name)
    ck(rx + rw <= ST_W, "设置窗 %s 右边界 %d 超出 %d" % (name, rx + rw, ST_W))
    ck(ry + rh <= sh, "设置窗 %s 下边界 %d 超出客户区 %d" % (name, ry + rh, sh))

snames = sorted(srects)
for i in range(len(snames)):
    for j in range(i + 1, len(snames)):
        a, b = snames[i], snames[j]
        ck(not overlap(srects[a], srects[b]),
           "设置窗 %s 与 %s 重叠 %s %s" % (a, b, srects[a], srects[b]))

# 设置窗控件也要"创建即声明"——这里没有显隐需求，只查创建齐全
screate = slice_between(text, r"static void MakeSettingsControls\(HWND h\) \{",
                        r"\nstatic LRESULT CALLBACK SettingsProc")
ck(len(screate) > 500, "没能切出 MakeSettingsControls 函数体")
made = set(re.findall(r"\(HMENU\)(?:\(INT_PTR\))?(IDC_ST_[A-Z0-9_]+)", screate))
made |= set(re.findall(r"(?:sec|hint|check|button)\(\s*(IDC_ST_[A-Z0-9_]+)", screate))

for need in ["IDC_ST_SEC1", "IDC_ST_SEC2", "IDC_ST_SEC3", "IDC_ST_SEC4",
             "IDC_ST_HINT1", "IDC_ST_HINT2", "IDC_ST_HINT3", "IDC_ST_HINT4",
             "IDC_ST_QUICK", "IDC_ST_QLBL", "IDC_ST_KEEP", "IDC_ST_KLBL",
             "IDC_ST_AUTOSTART", "IDC_ST_TRAY", "IDC_ST_LOG", "IDC_ST_ALLOW",
             "IDC_ST_CLEAN", "IDC_ST_SYSINFO", "IDC_ST_OPEN", "IDC_ST_CLOSE"]:
    ck(need in made, "设置窗控件 %s 没有在 MakeSettingsControls 里创建" % need)

# 反查：SettingsLayout / RefreshSettings 里摆的 ID，必须都真的建出来了
for fn_pat, fn_name in [(r"static int SettingsLayout\(HWND h\) \{", "SettingsLayout"),
                        (r"static void RefreshSettings\(HWND h\) \{", "RefreshSettings")]:
    body = slice_between(text, fn_pat, r"\nstatic ")
    used = set(re.findall(r"(IDC_ST_[A-Z0-9_]+)", body))
    ghost = sorted(used - made)
    ck(not ghost, "%s 里用到但没创建的控件：%s" % (fn_name, ", ".join(ghost)))

# ---------------------------------------------------------------------------
# D. 行为不变量
#
# 这几条都是"看起来能跑、但语义错了"的坑，编译器和肉眼都不容易发现，
# 所以固化成断言。每条后面都跟着它防的那个具体后果。
# ---------------------------------------------------------------------------

# D1 系统持久任务不进内存计时器。
#    否则到点时内存这条用 timeout=0 立刻执行，会把 shutdown.exe /t N
#    争来的宽限窗口掐掉；提醒窗上的「取消本次」也会变成谎话。
sched = slice_between(text, r"static void ScheduleTask\(PowerTask& t, bool force = false\) \{",
                      r"\nvoid UI_RefreshAll")
ck(len(sched) > 200, "没能切出 ScheduleTask 函数体")
i_persist = sched.find("if (t.persistent)")
i_due     = sched.find("ComputeDueTick")
ck(i_persist >= 0 and i_due >= 0 and i_persist < i_due,
   "ScheduleTask 没有在处理持久任务之前早退（持久任务会被重复排期）")

# D2 「降级」必须先摘系统任务再写盘。
#    顺序反了的话，用户在 UAC 上点取消，本地已经写成"内存计时"，
#    系统里那条却还在按时执行——界面上再也看不到它，也就管不着了。
ok_block = slice_between(text, r"if \(id == IDC_OK\) \{",
                         r"if \(id == IDC_CANCEL\) \{ DestroyWindow")
ck(len(ok_block) > 2000, "没能切出 IDC_OK 处理块")
i_unreg = ok_block.find("SysUnregister")
i_save1 = ok_block.find("g_store.Save()")
ck(i_unreg >= 0 and i_save1 >= 0 and i_unreg < i_save1,
   "IDC_OK：SysUnregister 没有排在第一次 g_store.Save() 之前（降级不是原子的）")

# D3 系统任务摘不掉时不许删本地记录。
#    否则任务从列表里消失、却在后台照常执行，用户连从哪儿关掉都找不到。
del_block = slice_between(text, r"case ELEM_DEL: \{", r"\n        \}\n        \}")
ck(len(del_block) > 400, "没能切出 ELEM_DEL 处理块")
ck("okToDelete = false" in del_block,
   "ELEM_DEL：SysUnregister 失败后没有阻止删除本地记录")
i_guard = del_block.find("if (okToDelete)")
i_rm    = del_block.find("g_store.RemoveTask")
ck(i_guard >= 0 and i_rm > i_guard,
   "ELEM_DEL：RemoveTask 没有被 okToDelete 保护")

# D4 设置窗里改闸门，开着的编辑窗必须立刻跟着变。
#    否则用户关了开关，编辑窗里那个单选按钮还是可点的。
allow_block = slice_between(text, r"if \(id == IDC_ST_ALLOW\) \{",
                            r"if \(id == IDC_ST_CLEAN\) \{")
ck("EditorRefreshMode()" in allow_block,
   "设置窗改了 allowSystemTasks 之后没有刷新编辑窗的执行方式行")

# D5 编辑窗的「执行方式」说明文字必须覆盖三种不可用情形
mode_block = slice_between(text, r"static void EditorRefreshMode\(\) \{",
                           r"\nstatic void EditorUpdateVisibility")
for cond, why in [("!gateOn", "闸门关闭"), ("!actionOk", "动作不支持"),
                  ("sysChecked", "已选系统任务")]:
    ck(cond in mode_block, "EditorRefreshMode 没处理「%s」的情形" % why)
ck("PersistentUnsupportedReason" in mode_block,
   "EditorRefreshMode 没有把动作不支持的原因说给用户")

# D6 提醒行标签随模式变化：系统任务下它变成 shutdown 的倒计时宽限
ck("倒计时宽限" in mode_block,
   "EditorRefreshMode 没有区分「提前提醒」与「倒计时宽限」两种语义")

# ---------------------------------------------------------------------------
# E. 存储层与注册路径（跨文件）
#
# 这一组来自真机上的一次失败：选「系统持久任务」保存时弹「注册系统任务失败」。
# 根因是配置写入用了 kernel32 的 Profile API —— 它写进进程内缓存、延迟落盘，
# 而父进程保存完立刻 ShellExecuteEx(runas) 拉起提权助手，助手从磁盘读到的还是
# 旧内容，FindById 返回空；偏偏所有失败分支又共用同一个标题，症状被完全伪装。
# 这一组断言把那条路彻底封死，并保证下次真出问题时能直接读出原因。
# ---------------------------------------------------------------------------
SRC = UI.parent
ROOT = SRC.parent


def read_src(name):
    p = SRC / name
    return p.read_text(encoding="utf-8") if p.exists() else ""


def read_root(name):
    p = ROOT / name
    return p.read_text(encoding="utf-8") if p.exists() else ""


core_cpp      = read_src("core.cpp")
sched_cpp     = read_src("sched.cpp")
sched_xml_cpp = read_src("sched_xml.cpp")
sched_xml_h   = read_src("sched_xml.h")
main_cpp      = read_src("main.cpp")
inifile_cpp   = read_src("inifile.cpp")
inifile_h     = read_src("inifile.h")

# E1 配置文件不许再用 Profile API。
#    「先删文件再重写」对它也无效——缓存里还留着上一版的键值，删掉的段会自己长回来。
#    注意只查「调用」（带左括号），因为 core.cpp 的说明注释里正是拿这几个名字
#    在讲为什么不能用它们。
for bad in ["WritePrivateProfileString(", "GetPrivateProfileString(",
            "WritePrivateProfileInt(", "GetPrivateProfileInt("]:
    ck(bad not in core_cpp,
       "core.cpp 又用回了 %s（进程内缓存 + 延迟落盘，提权助手读不到刚写的配置）" % bad)

# E2 写盘必须是「临时文件 → 刷盘 → 原子替换」。
#    刷盘不能省：紧接着启动的提权助手进程要从磁盘读它。
#    替换要原子：读者要么看到旧的完整文件，要么看到新的完整文件。
save_body = slice_between(core_cpp, r"bool TaskStore::Save\(\) \{", r"\n// ----")
ck("WriteAllBytesAtomic" in save_body, "TaskStore::Save 没有走原子写")
atomic = slice_between(core_cpp, r"static bool WriteAllBytesAtomic", r"\n// 不是 UTF-16")
ck("FlushFileBuffers" in atomic,
   "WriteAllBytesAtomic 没有 FlushFileBuffers（跨进程可见性就没保障了）")
ck("MoveFileExW" in atomic and "MOVEFILE_REPLACE_EXISTING" in atomic,
   "WriteAllBytesAtomic 没有用 MoveFileExW 做原子替换")
i_flush, i_move = atomic.find("FlushFileBuffers"), atomic.find("MoveFileExW")
ck(i_flush >= 0 and i_move >= 0 and i_flush < i_move,
   "WriteAllBytesAtomic 的顺序不对：必须先刷盘再替换")

# E3 解析与编解码必须真的走 inifile 那一层
ck('#include "inifile.h"' in core_cpp, "core.cpp 没有包含 inifile.h")
for fn in ["IniParse", "IniGet", "IniSections", "IniSectionEntries",
           "IniStartsWithNoCase", "TextToUtf16Le", "TextFromBytes"]:
    ck(fn in core_cpp, "core.cpp 没有用到 %s" % fn)

# E4 归属表的读写前缀必须一致，否则重启后系统任务会「失联」——
#    列表上看不到它，但它还在按时关机，用户找不到地方关掉。
ck('L"\\r\\n[Host."' in save_body, "Save 没有写 [Host.<指纹>] 归属表段")
ck("IniStartsWithNoCase(sec, L\"Host.\")" in core_cpp,
   "Load 识别归属表用的前缀与 Save 写出的不一致")

# E5 RegistrationInfo 里不能出现 <URI>。
#    路径由 RegisterTaskDefinition 的第一个参数决定；手写 URI 一旦与注册路径
#    有出入就以 0x80041318 被整段拒绝，而报错完全指不到这个元素上。
#    只认「双引号字面量里出现 <URI>」——注释里正是拿这个名字在讲为什么不能写它。
ck(re.search(r'"[^"\n]*<URI>', sched_xml_cpp) is None,
   "sched_xml.cpp 又开始往 RegistrationInfo 里写 <URI> 了")

# E6 文档外壳的两处硬性要求
#    version 现在是可变的（降级链会换），所以不能直接查字面量，改为查：
#    · 默认值仍是 1.2（覆盖面最广，Vista 起都认）
#    · 1.4 作为备选存在（Windows 10 的现代写法）
ck('std::wstring  version = L"1.2"' in sched_xml_h,
   "SchedXmlOpts 的默认 version 应该是 1.2")
ck('L"1.4"' in sched_cpp,
   "sched.cpp 的降级链里应该有 version=1.4 的备选")
ck("xmlns=\\\"http://schemas.microsoft.com/windows/2004/02/mit/task\\\"" in sched_xml_cpp,
   "Task 元素缺少任务计划程序命名空间声明")

# E7 注册必须拆成两步：先让服务解析 XML，再注册。
#    一步走的时候，XML 写错、权限不够、文件夹不存在、任务重名都返回同一个失败，
#    报错只能写得很含糊——这正是「注册系统任务失败」查不出原因的原因。
reg = slice_between(sched_cpp, r"static bool RegisterOne\(ITaskService\* svc",
                    r"\n// 把 \*err 里的多行文本缩进后接到日志后面")
ck("put_XmlText" in reg, "RegisterOne 没有先用 put_XmlText 单独校验 XML")
ck("RegisterTaskDefinition" in reg, "RegisterOne 没有用 RegisterTaskDefinition 注册")
i_xml, i_reg = reg.find("put_XmlText"), reg.find("RegisterTaskDefinition")
ck(i_xml >= 0 and i_reg >= 0 and i_xml < i_reg,
   "RegisterOne 的顺序不对：必须先 put_XmlText 再 RegisterTaskDefinition")
ck("put_XmlText" in reg and "RegisterTaskDefinition" in reg,
   "RegisterOne 两个阶段失败时没有区分文案（症状会再次被伪装）")
ck("XML 本身被拒" in reg and "服务端拒绝注册" in reg,
   "RegisterOne 两个阶段失败时没有给出不同的文案（症状会再次被伪装）")

# E8 错误码说明必须问系统要，不能硬编码一张表
ck("FormatMessageW" in sched_cpp and "FORMAT_MESSAGE_FROM_SYSTEM" in sched_cpp,
   "sched.cpp 没有用 FormatMessageW 取系统错误说明")
set_err = slice_between(sched_cpp, r"static void SetErr\(", r"\n// 完整 exe 路径")
ck("HexErr" in set_err and "HrText" in set_err,
   "SetErr 没有同时给出十六进制错误码与可读说明")
ck("0x80070000" in sched_cpp,
   "HrText 没有处理 0x8007xxxx（Win32 码套 HRESULT 外壳，直接查系统表查不到）")

# E9 「找不到任务」必须带上配置文件路径与读到的条数，
#    这两条信息足以判定是路径不对还是父进程没落盘。
add_fn = slice_between(sched_cpp, r"bool SchedDoAdd\(", r"\nbool SchedDoDelete")
ck("g_store.Path()" in add_fn, "SchedDoAdd 报「找不到任务」时没带配置文件路径")
ck("Tasks().size()" in add_fn, "SchedDoAdd 报「找不到任务」时没带读到的任务条数")

# E10 提权助手入口要先确认配置真的读进来了
add_cli = slice_between(main_cpp, r'if \(SameArg\(cmd, L"/sched-add"\)\) \{',
                        r'if \(SameArg\(cmd, L"/sched-del"\)\)')
ck("g_store.Tasks().empty()" in add_cli,
   "/sched-add 没有先检查配置里有没有任务（会把「读不到配置」伪装成「找不到任务」）")

# E11 构建脚本必须把新文件加进编译列表，否则真机上直接链接失败
for name, script in [("build_mingw.sh", read_root("build_mingw.sh")),
                     ("build_msvc.bat", read_root("build_msvc.bat"))]:
    ck("inifile.cpp" in script, "%s 没有把 src/inifile.cpp 加进编译列表" % name)

# E12 宿主自检引用 src 里的头文件只能用 -iquote。
#    POSIX 有个系统头就叫 <sched.h>，项目里也有 src/sched.h；一旦把 src 加进 -I，
#    libc++ 内部的 <sched.h> 会命中我们的 src/sched.h，把 core.h → <vector>
#    拖进一个正在展开的 <vector> 里，报出一堆莫名的「no member named 'min'」。
#    只查宿主自检那一段：资源编译器的 `windres -I src` 是另一回事，它只解析 .rc。
build_sh = read_root("build_mingw.sh")
host_block = build_sh[build_sh.find("HOSTCXX="):] if "HOSTCXX=" in build_sh else ""
ck(bool(host_block), "build_mingw.sh 里找不到宿主自检段（HOSTCXX=）")
ck("-iquote src" in host_block, "build_mingw.sh 的宿主自检应该用 -iquote src")
ck("-I src" not in host_block,
   "build_mingw.sh 的宿主自检里出现了 -I src，会污染尖括号查找")

# E13 严格 UTF-8：非法序列必须返回 false，好让调用方退回 ANSI 代码页。
#    如果这里改成替换成 U+FFFD，记事本另存的 ANSI 文件会被静默误读成乱码。
ck("0xD800" in inifile_cpp, "DecodeUtf8 没有拒绝代理区码点")
ck("0x10FFFF" in inifile_cpp, "DecodeUtf8 没有拒绝超出 Unicode 范围的码点")
ck("if (need == 1 && cp < 0x80) return false;" in inifile_cpp,
   "DecodeUtf8 没有拒绝过长编码（overlong）")
ck("out->clear();\n    return false;" in inifile_cpp,
   "TextFromBytes 解码失败时没有清空输出（调用方会拿到半截内容）")

# E14 inifile 这一层必须保持纯逻辑——一旦它开始碰 Windows API，
#    就再也塞不进宿主机测试，这层回归网也就没了。
for bad in ["windows.h", "CreateFileW", "WriteFile", "MultiByteToWideChar"]:
    ck(bad not in inifile_cpp, "inifile.cpp 引入了 %s，不再是纯逻辑（宿主机测试会失效）" % bad)
    ck(bad not in inifile_h,   "inifile.h 引入了 %s，不再是纯逻辑" % bad)

# ---------------------------------------------------------------------------
#  E15–E18：SCHED_E_INVALIDVALUE（0x80041318）的对策
#
#  真机上服务端以「任务 XML 包含一个格式不正确或超出范围的值」整段拒绝了
#  任务定义。这个错误名下的约束有一部分文档没写、只能实测，在 macOS 上无法
#  复现。对策是：把「都合法但服务端口味不同」的写法按优先级试一遍，同时把
#  每一次的返回码和用到的完整 XML 写进日志。下面几条把这件事钉住——
#  尤其是「降级不能悄悄把行为改坏」这一条。
# ---------------------------------------------------------------------------

# E15 降级链必须存在、有序、且兜底的那一档排在最后。
#     顺序错了会让「行为更差但更容易被接受」的定义被优先采用。
#     逐个解析候选，而不是查关键词在不在——关键词到处都有，查不出顺序错。
chain = slice_between(sched_cpp, r"static std::vector<SchedAttempt> AttemptsFor\(\)",
                      r"// 单次尝试")
entries = re.findall(r"push_back\(\{(.*?)\}\);", chain, re.S)
ck(len(entries) >= 3,
   "AttemptsFor 的候选太少，降级链起不到作用（至少要有三种写法）")

styles, vers, sids = [], [], []
for e in entries:
    styles.append("lean" if "Conservative" in e else ("rich" if "Rich" in e else "?"))
    m = re.search(r'L"(\d\.\d)"', e)
    vers.append(m.group(1) if m else "?")
    sids.append("sid" if re.search(r"true\s*$", e.strip()) else "name")

ck("?" not in styles, "AttemptsFor 里有候选没写清楚用哪套定义")
ck(styles[0] == "rich", "AttemptsFor 的首选必须是完整定义")
ck(styles[-1] == "lean", "AttemptsFor 的兜底必须是精简定义（它是最后一档）")
ck(styles == sorted(styles, key=lambda s: 0 if s == "rich" else 1),
   "AttemptsFor 的顺序不对：精简定义必须全部排在完整定义之后（它是兜底，不是首选）")
ck("name" in sids and "sid" in sids,
   "AttemptsFor 必须同时试账户名与 SID 两种身份写法")
ck(sids[0] == "name",
   "AttemptsFor 应先试账户名（userId 参数按文档是账户名解析，SID 是历史写法）")
ck(("rich", "1.2") in list(zip(styles, vers)),
   "AttemptsFor 里没有 version=1.2 的候选（覆盖面最广的那一档）")
ck(("rich", "1.4") in list(zip(styles, vers)),
   "AttemptsFor 里没有「完整定义 + version=1.4」的候选（1.2 被拒时没有退路）")

# E16 userId 参数不能只认 SID。
#     文档把 userId 描述成「用户凭据」并要求用具体用户名，实测可用的写法
#     （schtasks /ru、各类官方示例）一律是账户名。只传 SID 是上次失败的可疑点。
ck("AccountNameForUser" in sched_cpp,
   "sched.cpp 没有提供账户名（AccountNameForUser）")
ck('return L"SYSTEM";' in sched_cpp,
   "AccountNameForUser 没有为 Local System 返回账户名 SYSTEM")
ck("LookupAccountSidW" in sched_cpp,
   "AccountNameForUser 没有把当前用户 SID 翻回 SAM 名")
ck("LookupAccountSidW" in sched_cpp and "secur32" not in sched_cpp,
   "AccountNameForUser 引入了 secur32（LookupAccountSidW 在 advapi32 里，不必新增依赖）")

# E17 每一次尝试都必须留痕：日志要写盘，失败文案要给出日志路径。
#     没有这一条，下次再失败还是只能靠猜。
ck("SchedLogPath" in sched_cpp and "SchedLog" in sched_cpp,
   "sched.cpp 没有实现诊断日志")
log_fn = slice_between(sched_cpp, r"void SchedLog\(const std::wstring& text\)",
                       r"\n// 对外：名称")
ck("FILE_APPEND_DATA" in log_fn,
   "SchedLog 不是以追加方式写日志（每次都会覆盖上一次的诊断信息）")
ck("GetFileSizeEx" in log_fn,
   "SchedLog 没有判断文件是否已存在（追加时会把 BOM 塞进文本中间）")
fb = slice_between(sched_cpp, r"static bool RegisterWithFallback\(",
                   r"bool SchedDoAdd")
ck("SchedLog(log)" in fb,
   "RegisterWithFallback 没有把尝试过程写进日志")
ck("SchedLog(log)" in fb and fb.count("SchedLog(log)") >= 2,
   "RegisterWithFallback 只在失败时写日志，成功时也应记下「哪种写法被接受」")
ck("SchedLogPath()" in fb,
   "全部写法失败时的文案里没有给出日志路径（用户拿不到可发回来的东西）")
ck("[成功]" in fb and "[失败]" in fb and "[跳过]" in fb,
   "日志没有区分每次尝试的结果")
# 降级重试必须在同一个提权进程里完成，不能每次重试都重新提权
ck("ShellExecute" not in fb and "SchedRunElevated" not in fb,
   "RegisterWithFallback 内部又去提权了（会重复弹 UAC）")
ck("RegisterWithFallback(svc, folder" in sched_cpp,
   "SchedDoAdd 没有走降级链（还停在单次注册）")

# E18 降级不能悄悄改坏行为。
#     精简定义去掉的必须都是「等于服务端默认值」的冗余项；只要还留着
#     电池、补跑、唤醒、启停、时限这五项，降级就是行为等价的。
#     做法：把 Settings 段切成「公共部分」和「只在完整版里输出的部分」，
#     然后在公共部分上断言。这样无论冗余项被塞在 if (rich) 之前还是之后
#     都能查到。
#
#     注意：断言前必须先剥掉注释。源码的说明注释里正好写着 WakeToRun、
#     IdleSettings 这些元素名（在解释为什么保留 / 为什么省掉），不剥就会
#     把「注释里提到过」当成「代码里写了」——这类假阳性踩过一次。
settings_src = sched_xml_cpp[sched_xml_cpp.find('s += L"  <Settings>'):]
settings_src = settings_src[:settings_src.find('s += L"  </Settings>')]
i_rich = settings_src.find("if (rich) {")
j_end  = settings_src.rfind("\n    }")
ck(i_rich >= 0 and j_end > i_rich, "sched_xml.cpp 的 Settings 段找不到 if (rich) 分支")
common = strip_comments(settings_src[:i_rich] + settings_src[j_end:])
rich_only = strip_comments(settings_src[i_rich:j_end])

for must in ["DisallowStartIfOnBatteries", "StopIfGoingOnBatteries",
             "StartWhenAvailable", "WakeToRun", "ExecutionTimeLimit", "Enabled"]:
    ck(must in common,
       "精简定义丢掉了 %s —— 降级会悄悄改坏行为（比直接失败更糟）" % must)
for redundant in ["IdleSettings", "AllowStartOnDemand", "Priority",
                  "Hidden", "RunOnlyIfIdle", "RunOnlyIfNetworkAvailable",
                  "AllowHardTerminate", "MultipleInstancesPolicy"]:
    ck(redundant not in common,
       "精简定义的公共段里出现了冗余项 %s（每多一项就多一处可能被判非法的取值）"
       % redundant)
    ck(redundant in rich_only,
       "完整定义里应保留 %s（它是排查服务端口味的依据）" % redundant)
ck("Principals" in sched_xml_cpp and sched_xml_cpp.count("if (rich)") >= 2,
   "sched_xml.cpp 没有把 Principals 放进风格分支（精简版必须省掉它）")
ck("SchedXmlStyle::Rich" in sched_xml_cpp and "if (rich)" in sched_xml_cpp,
   "sched_xml.cpp 没有按风格分流")

# E19 自检必须两套定义都跑。只验一套等于放任另一套悄悄写坏。
selftest = slice_between(sched_cpp, r"void SchedSelfTest\(", r"\n    out->push_back\(L\"\"\);\n    out->push_back\(L\"=== 本机已注册")
ck("SchedXmlStyle::Conservative" in selftest,
   "/sched-selftest 只验了完整定义，没验降级时会用到的精简定义")
ck("SchedLogPath()" in selftest,
   "/sched-selftest 没有报出诊断日志的位置")

# ---------------------------------------------------------------------------
# E20 「开机静默启动」必须靠显式命令行开关，不能靠猜。
#
#     所有"猜"的写法都不可靠：判断父进程是谁（explorer 也可能来自用户双击）、
#     判断启动耗时（SSD 上跟手动启动没区别）、看有没有登录脚本（计划任务也能
#     拉起程序）。而猜错的两个方向都是"看起来像 bug"：
#       · 该静默时弹了窗 —— 用户在登录那一刻被挡住；
#       · 该显示时只给托盘 —— 用户双击了图标，却什么都没出现。
#
#     所以判定依据只有一个：自启动的 Run 项里有没有 /tray。手动双击永远没有。
# ---------------------------------------------------------------------------
ui_src   = strip_comments(text)
main_src = strip_comments(main_cpp)
core_src = strip_comments(core_cpp)

# (1) /tray 要被解析出来，并且一路传到 UI_Create
ck("/tray" in main_src and "-tray" in main_src,
   "main.cpp 没有解析 /tray 命令行开关（开机静默启动就无从触发）")
ck("trayOnly = true" in main_src,
   "main.cpp 解析到 /tray 之后没有置位 trayOnly")
ck("UI_Create(hInst, nCmdShow, trayOnly)" in main_src,
   "main.cpp 没有把 trayOnly 传给 UI_Create，开关等于没接上")

# (2) 单实例分支：静默启动时不许去叫醒已经在运行的那一份。
#     否则用户还留着一条旧的手动启动项时，登录那一刻照样会弹窗。
inst = slice_between(main_src,
                     r"if \(g_hMutex && GetLastError\(\) == ERROR_ALREADY_EXISTS\)",
                     r"\n    // ---- 配置")
ck(len(inst) > 100, "没能切出单实例分支")
ck("trayOnly" in inst,
   "单实例分支没考虑 trayOnly：开机自启重复触发时仍然会把窗口叫出来")
ck("IsIconic" in inst,
   "单实例分支没有用 IsIconic 区分「最小化」与「从未显示」——"
   "托盘里缩着的窗口用 SW_RESTORE 未必带得出来")

# (3) UI_Create 必须照常建窗口、挂托盘、开计时器，只是不显示。
#     「不显示」只影响 ShowWindow 这一个动作，不能连带跳过功能初始化。
uicreate = slice_between(ui_src,
                         r"bool UI_Create\(HINSTANCE hInst, int nCmdShow, bool startHidden\)",
                         r"\nHWND UI_MainWnd")
ck(len(uicreate) > 300, "没能切出 UI_Create 函数体")
i_hide  = uicreate.find("ShowWindow(g_hMain, SW_HIDE)")
i_tray  = uicreate.find("TrayAdd(g_hMain)")
i_timer = uicreate.find("SetTimer(g_hMain, IDT_TICK")
ck(i_hide >= 0,
   "UI_Create 在 startHidden 时没有把主窗口隐藏（开机启动照样弹窗）")
ck(i_tray > i_hide and i_timer > i_hide,
   "startHidden 时跳过了 TrayAdd / SetTimer —— 窗口不显示不等于功能不启动，"
   "托盘图标和计时器都挂在这个窗口上")

# (4) 自启动命令行里有没有 /tray，只由配置决定；而且只允许一处拼接。
auto_cmd = slice_between(ui_src, r"static std::wstring AutostartCommand\(\)",
                         r"\nstatic bool ApplyAutostart")
ck(len(auto_cmd) > 50, "没能切出 AutostartCommand")
ck('L" /tray"' in auto_cmd,
   "AutostartCommand 没有给自启动命令行加 /tray")
ck("trayOnAutostart" in auto_cmd,
   "AutostartCommand 里的 /tray 没有受 trayOnAutostart 控制（开关是假的）")
apply_body = slice_between(ui_src, r"static bool ApplyAutostart\(bool on",
                           r"\n// 读当前 Run 项的值")
ck("AutostartCommand()" in apply_body,
   "ApplyAutostart 没有走 AutostartCommand，自己又拼了一份命令行"
   "（两处各拼一份，迟早不一致）")

# (5) 升级自愈的两条边界：不复活、不抢。
repair = slice_between(ui_src, r"void UI_RepairAutostart\(\)", r"\n}\n")
ck(len(repair) > 200, "没能切出 UI_RepairAutostart")
ck("ReadAutostartValue" in repair,
   "UI_RepairAutostart 没有先读现值——不读就写等于每次启动都重写注册表")
ck("if (!ReadAutostartValue(&cur)) return;" in repair,
   "UI_RepairAutostart 会把被删掉的启动项**复活**。用户在任务管理器的"
   "「启动」页里禁用它之后，程序自己加回去是最招人烦的行为之一")
ck("ExeFullPath()" in repair and "npos" in repair,
   "UI_RepairAutostart 会抢别的副本的启动项。同一台机器可能放着两支 U 盘，"
   "各自都开着自启，谁也别改谁")
ck("AutostartCommand()" in repair,
   "UI_RepairAutostart 没有把现值与 AutostartCommand() 比对，无法判断是否要修")
ck("UI_RepairAutostart();" in main_src,
   "main.cpp 没有在启动时调用 UI_RepairAutostart —— 老版本的启动项永远不会补上 /tray")

# (6) 设置窗里要有这一项，且没开自启时置灰。
ck("IDC_ST_TRAY" in ui_src, "设置窗没有「开机启动时只显示托盘图标」这一项")
refresh = slice_between(ui_src, r"static void RefreshSettings\(HWND h\)",
                        r"\nstatic int SettingsLayout")
ck("EnableWindow(C(IDC_ST_TRAY)" in refresh,
   "没开自启时托盘选项没有置灰——一个不生效的勾选框比没有更糟")
ck("id == IDC_ST_TRAY" in ui_src,
   "托盘选项没有接处理函数（点了没反应）")
ck("ApplyAutostart(true" in slice_between(ui_src, r"if \(id == IDC_ST_TRAY\)", r"\n        if \(id == IDC_ST_LOG\)"),
   "改托盘选项时没有立刻重写 Run 项——下次开机仍然是旧命令行，开关看起来失效")

# (7) 配置要真的持久化，且缺省值是 1。
ck(core_src.count('L"TrayOnAutostart"') >= 2,
   "TrayOnAutostart 只在读或只在写里出现（保存后重启就丢了）")
ck('L"TrayOnAutostart", 1' in core_src,
   "TrayOnAutostart 的缺省值不是 1：老配置文件升级后开机仍然会弹窗，"
   "而升级的用户正是最想要「别再弹」的那批人")

# ---------------------------------------------------------------------------
# E21 任务行的开关 = 「启用 / 停用」，不是「是否已排进内存计时器」。
#
# running 与 enabled 只在倒计时任务上恰好同步。按 running 翻转的两个症状
# 是同一个根因：每天/每周点了开关立刻弹回（执行完立即重排，running 恒真），
# 系统任务的开关则永远显示为关（执行者是 Windows，本进程不排期）。
# ---------------------------------------------------------------------------
ui_code = strip_comments(ui_src)

# (1) 语义函数存在，且系统任务按 enabled 判。
ta = slice_between(ui_code, r"static bool TaskActive\(const PowerTask& t\)", r"\n}")
ck("t.persistent ? true : t.running" in ta,
   "TaskActive 没有区分系统任务：系统任务的执行者是 Windows、本进程不排期，"
   "按 running 判会把它永远算成停用")
ck("if (!t.enabled) return false;" in ta,
   "TaskActive 没有先看 enabled —— 停用的任务会被算成生效中")

# (2) 筛选、计数、列表必须同源，否则数字和列表会对不上。
tmf = slice_between(ui_code, r"static bool TaskMatchesFilter", r"\n}")
ck("case 1: return TaskActive(t);" in tmf,
   "「运行中」筛选没有用 TaskActive")
ck("case 2: return !t.enabled;" in tmf,
   "「已停用」筛选没有用 enabled")
ck("running" not in tmf,
   "筛选里又直接看 running 了 —— 与 TaskActive 两套判定必然漂移")

cf = slice_between(ui_code, r"static int CountFilter", r"\n}")
ck("if (TaskActive(t)) n++;" in cf and "if (!t.enabled) n++;" in cf,
   "CountFilter 与 TaskMatchesFilter 判定不一致（标签上的数字会和列表对不上）")
ck("running" not in cf, "CountFilter 又直接看 running 了")

# (3) 开关画的是 enabled，且不再有「是否可交互」参数。
dt = slice_between(ui_code, r"static void DrawToggle", r"\n}")
ck("bool enabled" not in dt,
   "DrawToggle 还留着第二个「是否可交互」参数：停用的任务必须能点回来，"
   "画成禁用态会让人以为点不了")
ck("COLORREF bg = on ? C_BRAND : C_DISABLED;" in dt,
   "开关底色没有只由 on 决定")
ck("DrawToggle(dc, tg, t->enabled);" in ui_code,
   "任务行的开关没有绑 enabled —— 这就是「点了马上弹回」的直接原因")
ck("DrawToggle(dc, tg, t->running" not in ui_code, "开关又绑回 running 了")

# (4) 点击翻 enabled；停用要清 running；启用要强制重算。
tg = slice_between(ui_code, r"case ELEM_TOGGLE:", r"case ELEM_RUN:")
ck("bool turnOn = !t->enabled;" in tg, "开关点击没有以 enabled 为准")
ck("bool turnOn = !t->running;" not in tg, "开关点击又按 running 翻转了")
ck("t->running = false;" in tg, "停用时没有清掉 running —— 内存计时器还在跑")
ck("ScheduleTask(*t, true);" in tg,
   "启用时没有强制重算排期，可能沿用停用前的旧 dueTick")
ck("TriggerKind::Countdown" not in tg,
   "点击处理里还留着「只有倒计时才置 enabled=false」的特例 —— 那正是当初的巧合")

# (5) 状态点与「运行中」标签同源。
ck("bool active = TaskActive(*t);" in ui_code, "状态点没有用 TaskActive")
ck(re.search(r'if \(active\)\s+addPill\(L"运行中"', ui_code) is not None,
   "「运行中」标签没有用 active —— 系统任务的 pill 会一直缺席")

# ---------------------------------------------------------------------------
# E22 三个「静默失效」的口子
#
# 都不是崩溃，而是「操作成功、结果却相反」：
#   ① 退出程序 = 悄悄取消所有内存计时任务；
#   ② 统计口径把停用的系统任务算成「已交给 Windows」；
#   ③ 连点两次「30 分钟后关机」留下两条任务。
# ---------------------------------------------------------------------------
core_h = strip_comments(read_src("core.h"))

# (1) 退出前必须确认，且确认必须在销毁窗口之前。
m = re.search(r"case 2008:(.{0,220})", ui_code, re.S)
exit_blk = m.group(1) if m else ""
ck("ConfirmExitWithRunningTasks" in exit_blk,
   "托盘「退出」没有确认就直接关：内存计时任务会随进程一起消失，"
   "而用户以为自己只是关掉了界面")
ck(exit_blk.find("ConfirmExitWithRunningTasks") < exit_blk.find("DestroyWindow")
   if "DestroyWindow" in exit_blk else False,
   "确认放在了 DestroyWindow 之后 —— 窗口都没了，弹不出来")
cf = slice_between(ui_code, r"static bool ConfirmExitWithRunningTasks", r"\n}")
ck("if (live.empty()) return true;" in cf,
   "没有任务在计时时也要弹一次确认？这个函数必须能静默返回 true")
ck("FormatCountdown(remain)" in cf,
   "确认框没有列出剩余时间 —— 用户没法判断「还能不能等」")

# (2) 统计口径只数启用的任务。
sb = slice_between(ui_code, r"static void DrawStatusBar", r"\n}")
ck("if (TaskActive(t)) active++;" in sb,
   "状态栏还在数 running —— 停用的系统任务会被算成运行中")
ck("t.running" not in sb, "状态栏又直接看 running 了")

tip = slice_between(ui_code, r"static void TrayUpdateTip", r"\n}")
ck("if (!t.enabled) continue;" in tip,
   "托盘提示把停用的任务也算进「已交给 Windows」")
ck("memActive" in tip and "sysActive" in tip,
   "托盘提示没有区分内存计时与系统任务")

# (3) 快捷关机复用同参数任务，而不是再建一条。
qc = slice_between(ui_code, r"static void QuickCountdown", r"\n}")
ck("t.delaySeconds != minutes * 60" in qc,
   "快捷关机没有按「同样时长」匹配已有任务 —— 连点两次会留下两条，到点各关一次")
ck("ScheduleTask(t, true);" in qc, "复用已有任务时没有重置它的计时")
ck(qc.find("AddTask") > qc.find("ScheduleTask"),
   "新增任务的代码排在了复用逻辑之前 —— 复用永远不会被执行")

# (4) 死字段必须清掉：声明了「用于防止重复触发」却一次都没读，
#     比没有这个字段更危险，下一个读代码的人会以为机制已经存在。
ck("firedTick" not in core_h,
   "core.h 里还有 firedTick —— 它从没被读写过，留着会让人误以为有防重复触发")

# ---------------------------------------------------------------------------
# E23 第二梯队：指定时刻的日期 / 快捷时长 / 日志清理 / 全部启停 / 执行历史
#
# 这一批的共同点是「新增了一条数据通路」，而数据通路最容易漏掉的是**某一端**：
# 加了字段忘了写盘、加了菜单项忘了接命令、加了配置项忘了夹取。逐段查，
# 不查「这个功能在不在」，查「这条通路的每一节都通着」。
# ---------------------------------------------------------------------------
core_h  = strip_comments(read_src("core.h"))
core_c  = strip_comments(core_cpp)
xml_c   = strip_comments(sched_xml_cpp)
sched_c = strip_comments(sched_cpp)
date_h  = strip_comments(read_src("dateutil.h"))

# (1) 指定时刻带日期：字段 → 写盘 → 读盘 → 显示 → 算 due → 写 XML，六节都要通
ck(re.search(r"int\s+year\s*=\s*0\s*,\s*month\s*=\s*0\s*,\s*day\s*=\s*0;", core_h) is not None,
   "PowerTask 没有 year/month/day —— 「指定时刻」还是只能指到分钟")
# 双语义的关键在默认值：读不到这三个键时必须落到 0，才能沿用旧规则。
ck('L"year", 0' in core_c,
   "读配置时 year 的默认值不是 0 —— 旧配置会被当成「指定了 0000 年」")

ini_w = slice_between(core_c, r"bool TaskStore::Save\(\)", r"\nbool ")
ini_r = slice_between(core_c, r"bool TaskStore::Load\(", r"\nbool ")
ck(len(ini_w) > 200, "没能切出 TaskStore::Save 函数体")
ck(len(ini_r) > 200, "没能切出 TaskStore::Load 函数体")
for key in ["year", "month", "day"]:
    ck(key in ini_w, "保存配置时没写 %s —— 指定时刻的日期存不下来" % key)
    ck(key in ini_r, "读配置时没读 %s —— 存了也白存" % key)

fr = slice_between(core_c, r"std::wstring FormatRule", r"\n}")
ck("t.year > 0" in fr,
   "FormatRule 没区分「有日期」和「没日期」—— 旧配置会显示成一个并不存在的日期")
ck("L\"下一次 %02d:%02d\"" in fr,
   "year == 0 的旧配置没有沿用「下一次 HH:MM」的旧文案")

# due 计算：有日期时必须按绝对时刻比，且过期要能返回 false
cd = slice_between(core_c, r"bool ComputeDueTick", r"\n}")
ck("SystemTimeToFileTime" in cd, "ComputeDueTick 没有按绝对日期算 —— 指定日期形同虚设")
ck("QuadPart <=" in cd,
   "ComputeDueTick 没判「已经过去」—— 指定日期在过去时会算出一个负数倒计时")
ck("OnceAtExpired" in core_c,
   "没有 OnceAtExpired —— 界面没法把「这个任务永远不会执行」标出来")

# 写 XML 时不能再「过了就顺延到明天」：指定了日期就是那一个时刻
cb = slice_between(xml_c, r"ComputeStartBoundary", r"\n}")
ck("ValidDate" in cb, "写系统任务时没校验日期 —— 2 月 30 号会直接写进 XML")
ck("ppdate::" in cb,
   "sched_xml 自己算天数了 —— 闰年必须只有 dateutil.h 一份实现")

ck("inline int DaysInMonth" in date_h,
   "dateutil.h 里没有 DaysInMonth —— 闰年判断又散回各处了")
ck("dateutil.h" not in slice_between(core_c, r"int DaysInMonth", r"\n}"),
   "core.cpp 的 DaysInMonth 没有转发到 ppdate —— 两份实现会漂移")
ck("DaysInMonth" not in slice_between(xml_c, r"int DaysInMonth", r"\n}"),
   "sched_xml.cpp 自己实现了一份 DaysInMonth")

# 编辑窗：保存前必须拦「时间已过去」，否则用户以为排上了，其实永远不会执行
ed_body = slice_between(ui_code, r"static LRESULT CALLBACK EditorProc", r"\nstatic ")
ck(len(ed_body) > 3000, "没能切出 EditorProc 函数体")
ck("CompareFileTime" in ed_body,
   "编辑窗保存时没比时间 —— 选一个过去的时刻会「保存成功」但永不执行")
ck("ValidDate" in ed_body, "编辑窗保存时没校验日期合法性")

# (2) 快捷关机时长可配置：INI 两端 + 菜单读配置 + 夹取
for key in ["Quick1", "Quick2", "Quick3"]:
    ck(key in core_c, "配置里没有 %s —— 托盘三个档位还是写死的" % key)
ck("24 * 60" in core_c,
   "快捷时长没夹到 24 小时以内 —— INI 手改成 99999 会出一个「99999 分钟后关机」")
menu = slice_between(ui_code, r"static void ShowTrayMenu", r"\n}")
ck("quickMinutes[i]" in menu,
   "托盘菜单没读配置里的时长 —— 设置里改了也不生效")

# (3) 日志清理：只认严格命名的文件，而且「能删」与「能读」共用同一份判断
ck("bool IsLogFileName" in core_h and "bool IsLogFileName" in core_c,
   "没有 IsLogFileName —— 清理和读取各写一套判断，迟早一个删得比另一个宽")
purge = slice_between(core_c, r"static void PurgeOldLogs", r"\n}")
ck("IsLogFileName(n)" in purge, "PurgeOldLogs 没有走共用的命名判断")
hist = slice_between(ui_code, r"static std::wstring BuildHistoryText", r"\nstatic void HistLayout")
ck("IsLogFileName(n)" in hist,
   "执行历史没有走共用的命名判断 —— 用户自己放进 logs 的 .log 会被当成本程序记录读进来")
ck("keepMonths" in purge and "curIndex - idx >= keepMonths" in purge,
   "PurgeOldLogs 没有按保留月数比较 —— 要么不删，要么删光")
ck("static bool purged" in core_c,
   "日志清理不是每次运行只做一次 —— 每写一行都扫一遍目录")
ck("logKeepMonths" in ui_code,
   "设置窗没有日志保留月数的入口 —— 功能在，但用户改不了")

# (4) 全部启停：菜单项必须接上，且系统任务只提权一次
for cmd in ["2009", "2010"]:
    ck(re.search(r"case %s:\s*SetAllEnabled" % cmd, ui_code) is not None,
       "托盘菜单 %s 没有接上处理函数 —— 点了没反应" % cmd)
sa = slice_between(ui_code, r"static void SetAllEnabled", r"\nstatic void ShowTrayMenu")
ck(len(sa) > 300, "没能切出 SetAllEnabled 函数体")
ck(sa.count("SchedRunElevated") == 1,
   "SetAllEnabled 里 SchedRunElevated 出现 %d 次 —— 系统任务会被逐条提权，弹 N 次 UAC"
   % sa.count("SchedRunElevated"))
# 「只调一次」还不够：唯一那次调用必须直接躺在 if (anySys) 块里（函数体 4 空格
# + if 体 4 空格 = 8），不能嵌在更深的循环里——嵌进 for 就是逐条提权，
# 而调用次数仍然是 1，光数次数看不出来。
run_lines = [ln for ln in sa.splitlines() if "SchedRunElevated(SchedOp::SetAllEnabled" in ln]
ck(len(run_lines) == 1,
   "SetAllEnabled 里找不到唯一的那次提权调用（找到 %d 处）" % len(run_lines))
if run_lines:
    indent = len(run_lines[0]) - len(run_lines[0].lstrip())
    ck(indent == 8,
       "提权调用缩进 %d 个空格（应为 8）—— 它被嵌进了循环或额外分支，"
       "「全部暂停」会逐条弹 UAC" % indent)
ck("SchedOp::SetAllEnabled" in sched_cpp,
   "sched 层没有 SetAllEnabled 这个操作")
ck("/sched-set-all" in sched_cpp and "/sched-set-all" in main_cpp,
   "命令行入口没接上 —— 提权后的那个进程不知道要做什么")
allnames = slice_between(sched_cpp, r"SchedDoSetAllEnabled", r"\n}")
ck("HostTaskNames" in allnames,
   "批量操作按文件夹扫任务 —— 会把别的程序注册的任务也一起改掉；"
   "必须只动归属表里记着的那些")

# (5) 执行历史：窗口要注册，防重入，空状态要有话说
ck("L\"PPHistory\"" in ui_code, "执行历史窗口类没注册 —— CreateWindowExW 会直接失败")
ck("RegisterClassExW(&wh)" in ui_code, "PPHistory 的 RegisterClassExW 漏了")
ck("newElem = 902" in ui_code,
   "历史按钮没有悬停反馈 —— 鼠标移上去不变色，用户不知道它可点")
ck(re.search(r"PtInRect\(&L\.rcHistBtn, pt\)\)\s*\{\s*ShowHistory\(hwnd\);", ui_code)
   is not None,
   "历史按钮点了没反应")
show_hist = slice_between(ui_code, r"static void ShowHistory", r"\n// ---")
ck("if (g_hHist) { SetForegroundWindow(g_hHist); return; }" in show_hist,
   "ShowHistory 没防重入 —— 点两次会开出两个窗口，关掉一个另一个还在")
ck("HIST_MAX_LINES" in show_hist or "HIST_MAX_LINES" in hist,
   "历史没有行数上限 —— 日志攒几个月之后打开会卡")
ck("text.empty()" in show_hist,
   "没有日志时窗口是空的 —— 用户分不清「没执行过」和「读不到文件」")
ck("EM_SETSEL" in show_hist,
   "打开历史没把光标移到末尾 —— 用户得自己滚到底才能看到最近一条")

# (6) 键盘与鼠标必须共用一份激活逻辑
mb = slice_between(ui_code, r"case WM_LBUTTONDOWN", r"case WM_MOUSEMOVE")
kb = slice_between(ui_code, r"case WM_KEYDOWN", r"\n    case ")
ck("ActivateRowElem(hwnd" in mb, "鼠标点击没走 ActivateRowElem")
ck("ActivateRowElem(hwnd" in kb,
   "键盘操作没走 ActivateRowElem —— 两份实现迟早分叉，"
   "表现是「键盘删任务没删掉系统任务」")
# 光查「出现过一次」不够：删掉其中一两个动作照样过。三个动作都要在。
for elem, label in [("ELEM_TOGGLE", "空格切换开关"),
                    ("ELEM_EDIT", "回车编辑"),
                    ("ELEM_DEL", "Delete 删除")]:
    ck(elem in kb, "键盘的「%s」没接上 —— 这个键按了没反应" % label)
ck("g_selRow" in ui_code and "g_hoverRow" in ui_code,
   "键盘选中行与鼠标悬停行没有分开 —— 鼠标一移开选中就丢了")

# (7) 日志写入的指针重载：不加它，短字面量会被 SSO 折叠成立即数，
#     check_strings.py 就再也看不见它们（见 core.h 的说明）。
ck("void LogLine(const wchar_t* text);" in core_h,
   "core.h 没有 LogLine(const wchar_t*) 重载 —— 4 个字的日志文案会被编成立即数，"
   "文案自检看不见它")
ck("void LogLine(const std::wstring& text) { LogLine(text.c_str()); }" in core_c,
   "wstring 版没有转发到指针版 —— 两套写盘逻辑")

# ---------------------------------------------------------------------------
print("检查项：%d" % checks)
if fails:
    print("失败 %d 项：" % len(fails))
    for f in fails:
        print("  ✗ " + f)
    sys.exit(1)
print("全部通过 ✓")
print("  编辑器客户区高度：%s" % {KIND_NAMES[k]: editor_client_height(k) for k in range(4)})
print("  设置窗客户区高度：%d" % sh)
