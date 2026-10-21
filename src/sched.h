// ============================================================================
//  PlanPower - sched.h
//  系统级持久任务：把任务注册进 Windows 任务计划程序
//
//  设计要点（详见《PlanPower_系统级持久任务设计》）：
//   · 默认不写 —— 全局开关默认关闭，不主动开启就一个字节都不落宿主机
//   · 显式同意 —— 每次写入都弹一次 UAC，用户始终对"正在往系统里写东西"有感知
//   · 可逆     —— 三级清理：任务级 / 机器级 / 应急级（/cleanup-all）
//   · 零子进程 —— 全部走 COM（ITaskService），不调 schtasks.exe
//
//  实现选择：用「生成任务 XML + ITaskFolder::RegisterTask」而不是逐层构造
//  ITaskDefinition 对象模型。原因有二：
//   1) XML 是纯字符串，可以在没有 Windows 的机器上离线校验结构与取值；
//   2) 少一层 COM 属性往返，出错面更小。
// ============================================================================
#pragma once
#include "core.h"
#include "sched_xml.h"

// ---------------------------------------------------------------------------
//  能力查询
// ---------------------------------------------------------------------------
// 任务计划程序服务是否可达（不可达时给出原因）
bool SchedAvailable(std::wstring* err);
// 当前进程是否已提权
bool IsElevated();

// ---------------------------------------------------------------------------
//  命名（实现转发到 sched_xml，自动补上本机介质标识）
// ---------------------------------------------------------------------------
std::wstring SchedTaskLeafName(const PowerTask& t);        // 仅叶子名
std::wstring SchedTaskFullPath(const PowerTask& t);        // 含 \PlanPower\ 前缀

// ---------------------------------------------------------------------------
//  任务 XML 生成（实现转发到 sched_xml，自动补上本机环境）
// ---------------------------------------------------------------------------
bool SchedBuildXml(const PowerTask& t, std::wstring* xml, std::wstring* err);
bool SchedBuildXml(const PowerTask& t, const SchedXmlOpts& opts,
                   std::wstring* xml, std::wstring* err);

// ---------------------------------------------------------------------------
//  注册诊断日志
// ---------------------------------------------------------------------------
// 每一次「写系统任务」的尝试都会追加到这个文件：用到的定义风格、身份写法、
// 返回码、系统说明，失败时还有完整 XML。它在 exe 同目录（不可写时退到配置
// 目录）。真机上再出问题时，把这个文件发出来就能直接定位到是哪个取值不被
// 接受，不用再靠猜。
std::wstring SchedLogPath();
// 追加一段诊断文本（自动补时间戳与分隔线）
void SchedLog(const std::wstring& text);

// ---------------------------------------------------------------------------
//  提权执行
// ---------------------------------------------------------------------------
enum class SchedOp { Add, Delete, SetEnabled, SetAllEnabled, CleanupAll };

// 以管理员身份重启自身完成操作（会弹一次 UAC）。
// taskIdOrName：Add 传任务 ID，其余传系统任务名。
// 返回 false 时 *err 为原因；用户拒绝 UAC 时 *err 为「已取消」。
bool SchedRunElevated(SchedOp op, const std::wstring& taskIdOrName,
                      bool enable, std::wstring* err);

// ---------------------------------------------------------------------------
//  实际执行体（由提权后的进程调用；直接调用则需要进程本身已提权）
// ---------------------------------------------------------------------------
bool SchedDoAdd(const std::wstring& taskId, std::wstring* err);
bool SchedDoDelete(const std::wstring& taskName, std::wstring* err);
bool SchedDoSetEnabled(const std::wstring& taskName, bool on, std::wstring* err);
// 一次提权里把本机归属表记录的所有系统任务设成同一个状态。
// 逐条调 SchedDoSetEnabled 会弹 N 次 UAC，所以批量必须合成一次。
// 只碰归属表里属于本机的那些名字——不按文件夹扫，免得把别的 U 盘的任务也停了。
// 返回实际改动的条数，-1 表示失败。
int  SchedDoSetAllEnabled(bool on, std::wstring* err);
// 删除 \PlanPower 文件夹下的全部任务并删掉文件夹；返回删除条数，-1 表示失败
int  SchedDoCleanupAll(std::wstring* err);

// ---------------------------------------------------------------------------
//  只读查询（无需提权，读取权限即可）
// ---------------------------------------------------------------------------
struct SchedEntry {
    std::wstring leafName;   // 叶子名（不含文件夹前缀）
    bool         enabled = true;
};
// 枚举 \PlanPower 下的全部任务。文件夹不存在时返回 true 且列表为空。
bool SchedEnum(std::vector<SchedEntry>* out, std::wstring* err);

// ---------------------------------------------------------------------------
//  自检（供 /sched-selftest 使用）
// ---------------------------------------------------------------------------
// 对所有已启用任务生成 XML 并做基本校验，把结果追加到 out
void SchedSelfTest(std::vector<std::wstring>* out);
