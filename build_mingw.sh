#!/bin/bash
# ===========================================================================
#  PlanPower - MinGW-w64 交叉编译脚本（macOS / Linux）
#  产物：build/PlanPower.exe —— 单文件、静态链接、零第三方依赖
#
#  支持两类工具链（自动探测）：
#    1) Homebrew 版：  brew install mingw-w64
#    2) LLVM-MinGW：   解压后设置 MINGW_BIN 指向其 bin 目录
#         export MINGW_BIN=/path/to/llvm-mingw-*/bin
# ===========================================================================
set -e
cd "$(dirname "$0")"

# 允许通过 MINGW_BIN 指定工具链目录
if [ -n "$MINGW_BIN" ]; then
    export PATH="$MINGW_BIN:$PATH"
    echo "已加入工具链路径：$MINGW_BIN"
fi

pick() {
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
    done
    return 1
}

# 优先 32 位（单文件通吃 32/64 位 Windows）
if CXX=$(pick i686-w64-mingw32-clang++ i686-w64-mingw32-g++); then
    ARCH=x86
elif CXX=$(pick x86_64-w64-mingw32-clang++ x86_64-w64-mingw32-g++); then
    ARCH=x64
else
    echo "[!] 找不到 mingw-w64 工具链。"
    echo "    方案 A：brew install mingw-w64"
    echo "    方案 B：下载 LLVM-MinGW 后 export MINGW_BIN=<解压目录>/bin"
    exit 1
fi

# 按编译器前缀推导配套工具
PREFIX="${CXX%-clang++}"
PREFIX="${PREFIX%-g++}"
RC=$(pick "${PREFIX}-windres" windres)
OBJDUMP=$(pick "${PREFIX}-objdump" llvm-objdump objdump)
STRIP=$(pick "${PREFIX}-strip" llvm-strip strip)

echo "编译器：$CXX   ($ARCH)"
echo "资源编译器：$RC"
echo

mkdir -p build

echo "[1/3] 编译资源..."
$RC -I src src/planpower.rc -O coff -o build/planpower.res

echo "[2/3] 编译并链接..."
# -static 系列 = 等价于 MSVC 的 /MT，静态链接运行库，这是零依赖的关键
#
# 优化级别用 -Os 而不是 -O2，这不是随手选的：
#   LLVM-MinGW 23.1.3 的 clang 在 -O2 下会把同一个翻译单元里**共享后缀**的
#   宽字符串字面量合并，而合并后的字节是错的——后一条串的开头会盖掉前一条
#   串的结尾。表现不是崩溃，是界面上某句话少几个字、或者两条文案粘在一起。
#   -O0 / -O1 / -Os 都不复现这一条（-O1 起另有一个单独的例外，见
#   tools/check_strings.py 的 KNOWN_OK）。
#   实测：-O2 下 444 条宽字面量里有 4 条被拼坏，-Os 下是 1 条且已确认无害。
#   顺带 -Os 的产物还小 7%。
#
# 构建末尾的 check_strings.py 就是这条的守卫：任何新的文案损坏都会让构建失败。
$CXX -std=c++17 -Os -s \
    -static -static-libgcc -static-libstdc++ \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
    -Wall -Wextra -Wno-unused-parameter \
    src/main.cpp src/core.cpp src/inifile.cpp src/sched_xml.cpp src/sched.cpp src/ui.cpp build/planpower.res \
    -o build/PlanPower.exe \
    -mwindows \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 \
    -lole32 -loleaut32 -lpowrprof -lshlwapi -ltaskschd

echo "[3/3] 检查依赖与体积..."
echo
echo "---- 导入表（应只含系统 DLL）----"
$OBJDUMP -p build/PlanPower.exe | grep -i "DLL Name" | sort -u
echo
echo "---- 体积 ----"
ls -l build/PlanPower.exe | awk '{printf "  %s 字节\n", $5}'
echo

# ---------------------------------------------------------------------------
#  [可选] 宿主端自检
#
#  这套检查存在的理由：macOS 上跑不了 exe，Windows 的运行期行为没法在这里
#  验证。但「配置文件怎么读写」「任务 XML 长什么样」和「控件摆在哪」这三件事
#  都是纯逻辑，可以在宿主机上直接编译真实源码、或者用脚本复刻一遍算术来验。
#  检测到工具就顺手跑，没装就跳过——不影响出包。
#
#  注意 1：这里必须用绝对路径找宿主编译器。LLVM-MinGW 的 bin 目录里也有一个
#  叫 clang++ 的软链（指向它自带的 clang），上面已经把它加进了 PATH，
#  直接 `command -v clang++` 会拿到那个跨平台的，连 <cstdint> 都找不到。
#
#  注意 2：引用 src/ 里的头文件只能用 -iquote，绝不能用 -I src。
#  POSIX 有个系统头就叫 <sched.h>，而项目里也有一个 src/sched.h；
#  一旦把 src 加进 -I，libc++ 内部的 #include <sched.h> 就会命中我们的
#  src/sched.h，把 core.h → <vector> 拖进一个正在展开的 <vector> 里，
#  报出一堆莫名其妙的「no member named 'min' in namespace std」。
#  -iquote 只作用于引号形式的包含，不会污染尖括号查找，正好合适。
# ---------------------------------------------------------------------------
HOSTCXX=""
for c in /usr/bin/clang++ /usr/bin/g++ /usr/local/bin/clang++ \
         /opt/homebrew/bin/clang++ /opt/homebrew/bin/g++; do
    if [ -x "$c" ]; then HOSTCXX="$c"; break; fi
done

if [ -n "$HOSTCXX" ] && [ -f tools/initest.cpp ]; then
    echo "[自检 1/4] 配置文件的解析与编解码（编译真实 src/inifile.cpp）..."
    if $HOSTCXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
         -iquote src tools/initest.cpp src/inifile.cpp -o build/initest; then
        ./build/initest | tail -1 | sed 's/^/    /'
    else
        echo "    （跳过：宿主编译器编译测试桩失败）"
    fi
    echo
fi

if [ -n "$HOSTCXX" ] && [ -f tools/xmltest.cpp ]; then
    echo "[自检 2/4] 任务 XML 生成（编译真实 src/sched_xml.cpp）..."
    if $HOSTCXX -std=c++17 -O1 -I tools/hostshim \
         tools/xmltest.cpp src/sched_xml.cpp -o build/xmltest; then
        ./build/xmltest | tail -2 | sed 's/^/    /'
        if command -v python3 >/dev/null 2>&1 && [ -f tools/check_xml.py ]; then
            python3 tools/check_xml.py | tail -1 | sed 's/^/    /'
        fi
    else
        echo "    （跳过：宿主编译器编译测试桩失败）"
    fi
    echo
fi

if command -v python3 >/dev/null 2>&1 && [ -f tools/check_layout.py ]; then
    echo "[自检 3/4] 界面布局几何与不变量..."
    python3 tools/check_layout.py | sed 's/^/    /'
    echo
fi

# 文案完整性：把源码里的宽字面量逐条拿到二进制里找一遍。
# 这条能抓到编译器看不出来的问题——源码对、二进制错（-O2 的常量合并）。
if command -v python3 >/dev/null 2>&1 && [ -f tools/check_strings.py ]; then
    echo "[自检 4/4] 界面文案完整性..."
    if python3 tools/check_strings.py build/PlanPower.exe | sed 's/^/    /'; then
        :
    else
        echo "    [!] 有文案在二进制里不完整，构建判定为失败。"
        exit 1
    fi
    echo
fi

echo "[OK] 构建完成：build/PlanPower.exe"
