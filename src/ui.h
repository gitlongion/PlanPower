// ============================================================================
//  PlanPower - ui.h  界面层接口
// ============================================================================
#pragma once
#include "core.h"

extern HINSTANCE g_hInst;

// 创建主窗口并启动计时器；成功返回 true
// startHidden = true 时窗口建好但不显示，只留托盘图标（开机自启用）
bool UI_Create(HINSTANCE hInst, int nCmdShow, bool startHidden = false);

// 启动时校正开机自启的 Run 项（把升级前的旧命令行补上 /tray）。
// 只对齐指向本 exe 的值；值不存在或指向别的副本时什么都不做。
void UI_RepairAutostart();

// 由主循环的定时器驱动，每 500ms 调用一次
void UI_Tick();

// 任务数据发生变化后调用：重算计时并重绘
// forceReschedule = true 时强制重置正在计时的任务（编辑保存后用）
void UI_RefreshAll(bool forceReschedule = false);

// 是否有正在倒计时的任务
bool UI_HasRunningTask();

// 取消全部正在倒计时的任务
void UI_CancelAllRunning();

// 取运行时绘制的程序图标（size: 16 或 32）
HICON UI_GetIcon(int size);

// 主窗口句柄
HWND UI_MainWnd();

// 把消息交给编辑窗处理（Tab 导航等）；已被处理返回 true
bool UI_PreTranslate(MSG* msg);
