// ============================================================================
//  PlanPower - sched_xml.h
//  系统任务定义的「纯逻辑」部分：名称净化、时间边界计算、任务 XML 生成
//
//  这里刻意不碰任何系统状态——时间、系统目录、介质标识、机器指纹、用户 SID
//  全部通过 SchedEnv 注入。这样做的好处是这部分逻辑可以脱离 Windows 单独
//  编译运行，从而在没有 Windows 的机器上逐条断言生成的 XML 是否正确。
//  带副作用的 COM 调用留在 sched.cpp。
// ============================================================================
#pragma once
#include "core.h"

// 生成 XML 所需要的全部外部输入
struct SchedEnv {
    SYSTEMTIME   now = {};       // 当前本地时间（决定 StartBoundary）
    std::wstring systemDir;      // System32 目录，含结尾反斜杠
    std::wstring driveTag;       // 本介质标识（4 位十六进制）
    std::wstring machineFp;      // 本机指纹（8 位十六进制）
    std::wstring userSid;        // 当前登录用户 SID（交互式动作需要）
};

// 名称净化：去掉任务计划程序不允许的字符，裁掉首尾空格，限制长度
std::wstring SanitizeTaskName(const std::wstring& s);

// 该动作能否做成系统持久任务。
// 关闭显示器没有对应的系统命令，只能由 PlanPower 自己运行时实现，故不支持。
bool ActionSupportsPersistent(PowerAction a);
// 不支持时给出的原因说明
const wchar_t* PersistentUnsupportedReason(PowerAction a);

// 系统任务名： {可读名称}_{介质标识}_{任务ID前8位}
std::wstring SchedTaskLeafName(const PowerTask& t, const std::wstring& driveTag);
std::wstring SchedTaskFullPath(const PowerTask& t, const std::wstring& driveTag);
std::wstring SchedFolderPath();                    // "\\PlanPower"

// ---------------------------------------------------------------------------
//  定义风格
// ---------------------------------------------------------------------------
//
// 真机上出现过任务计划程序以 SCHED_E_INVALIDVALUE（0x80041318，「任务 XML
// 包含一个格式不正确或超出范围的值」）整段拒绝任务定义的情况。这类拒绝在
// macOS 上无法复现——它取决于服务端版本、注册身份、以及若干只能实测才知道
// 的取值约束。
//
// 因此这里准备两套定义：完整版把全部意图写进 XML，精简版只保留会改变行为的
// 项、其余交给服务端默认值。sched.cpp 按顺序降级重试，并把每一次的结果连同
// 用到的 XML 写进日志，下一次出问题就能直接定位到是哪个取值不被接受。
enum class SchedXmlStyle {
    // 完整：RegistrationInfo + Principals + 全套 Settings
    Rich,
    // 精简：省略 Principals（身份完全由 RegisterTaskDefinition 的
    //       userId / logonType 参数决定）；Settings 只留会改变行为的项。
    //       语义承诺不变——不因电池而跳过、错过补跑、唤醒执行、启停状态、
    //       执行时限都保留；去掉的都是「与默认值等价」的冗余项。
    Conservative,
};

struct SchedXmlOpts {
    SchedXmlStyle style   = SchedXmlStyle::Rich;
    std::wstring  version = L"1.2";   // <Task version="...">
};

// 生成完整任务定义。失败时 *err 说明原因。
bool SchedBuildXml(const PowerTask& t, const SchedEnv& env,
                   const SchedXmlOpts& opts,
                   std::wstring* xml, std::wstring* err);

// 等价于 opts 取默认值（Rich + 1.2）
bool SchedBuildXml(const PowerTask& t, const SchedEnv& env,
                   std::wstring* xml, std::wstring* err);
