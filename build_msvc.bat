@echo off
REM ===========================================================================
REM  PlanPower - MSVC 构建脚本（Windows）
REM  在 "x64 Native Tools Command Prompt for VS" 或 "Developer Command Prompt" 中运行
REM  产物：build\PlanPower.exe —— 单文件、静态链接 CRT、零第三方依赖
REM ===========================================================================
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo [!] 找不到 MSVC 编译器 cl.exe
    echo     请在 "x64 Native Tools Command Prompt for VS" 中运行本脚本，
    echo     或先执行 vcvarsall.bat 初始化环境。
    echo.
    exit /b 1
)

if not exist build mkdir build

echo [1/3] 编译资源...
rc /nologo /fo build\planpower.res src\planpower.rc
if errorlevel 1 goto :fail

echo [2/3] 编译并链接...
REM  /MT  = 静态链接 CRT（零依赖的关键，绝不可改成 /MD）
REM  /utf-8 = 源码与执行字符集均为 UTF-8
REM  taskschd.lib 是 ITaskService（系统级持久任务）需要的导入库
cl /nologo /std:c++17 /O2 /MT /GS /GL /utf-8 /EHsc /W3 ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 ^
   src\main.cpp src\core.cpp src\inifile.cpp src\sched_xml.cpp src\sched.cpp src\ui.cpp ^
   /Fe:build\PlanPower.exe /Fo:build\ ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
   /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
   build\planpower.res ^
   kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ^
   comctl32.lib ole32.lib oleaut32.lib powrprof.lib taskschd.lib
if errorlevel 1 goto :fail

echo [3/3] 检查依赖与体积...
echo.
echo ---- 导入表（应只含系统 DLL）----
dumpbin /dependents build\PlanPower.exe | findstr /i ".dll"
echo.
echo ---- 体积 ----
for %%F in (build\PlanPower.exe) do echo   %%~zF 字节

echo.
echo [OK] 构建完成：build\PlanPower.exe
exit /b 0

:fail
echo.
echo [X] 构建失败。
exit /b 1
