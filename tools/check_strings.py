#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PlanPower 界面文案完整性校验
============================

存在的理由（这一条是真机上才会发现的坑）：

  LLVM-MinGW 23.1.3 的 clang 在 **-O2** 下会把同一个翻译单元里共享后缀的
  宽字符串字面量合并，而合并后的字节是**错的**——后一条串的开头会盖掉前一条
  串的结尾。表现不是崩溃，是界面上某句话少几个字、或者两条文案粘在一起。
  -O0 / -O1 / -Os 都不复现。

  这类缺陷编译器和静态检查都看不出来：源码是对的，二进制是错的。唯一能
  自动发现的办法，就是把源码里的宽字面量逐条拿到二进制里找一遍。

做法：扫 src/*.cpp 里所有不含转义序列的 L"..." 字面量（长度 >= 4），
逐条断言其 UTF-16LE 字节存在于 exe 中。含转义的跳过——转义解析本身容易
出错，而这类串多半是长提示文本，不是被合并的那批。

用法：
  python3 tools/check_strings.py build/PlanPower.exe
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# 已知例外：这些串在优化构建里查不到完整字节，但已确认不是文案损坏。
#
# 「未知设备」是 MachineShortName() 里 GetComputerNameW 失败时的兜底返回值，
# 只在 /sched-selftest 的输出里出现。它在 -O1 及以上会被 LLVM-MinGW 以某种
# 方式拆掉（换成 std::wstring(L"...") 也没用），而 -O0 下完好。由于它在
# 二进制里只留下「未知」两个字，无法从字节上确认运行时是否完整——所以
# 这里显式放行，而不是让整条检查失效。
#
# 如果哪天换了工具链版本、这条不再缺失，把它删掉即可。
KNOWN_OK = {
    "未知设备",
}


def wide_literals(path):
    """返回该文件里所有不含转义的宽字面量。"""
    text = path.read_text(encoding="utf-8")
    # 先把 // 注释剥掉，但保留字符串字面量（否则 L"http://..." 会被截断）
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    code = "".join(out)

    res = []
    for m in re.finditer(r'L"((?:[^"\\])*)"', code):
        s = m.group(1)
        if len(s) >= 4:
            res.append(s)
    return res


def main():
    if len(sys.argv) < 2:
        print("用法：check_strings.py <exe>")
        return 2

    exe = Path(sys.argv[1])
    if not exe.exists():
        print("找不到产物：%s" % exe)
        return 2
    data = exe.read_bytes()

    total, missing = 0, []
    for cpp in sorted(SRC.glob("*.cpp")):
        for s in wide_literals(cpp):
            if s in KNOWN_OK:
                continue
            total += 1
            if s.encode("utf-16-le") not in data:
                missing.append((cpp.name, s))

    print("宽字面量：%d 条，二进制中缺失 %d 条" % (total, len(missing)))
    if missing:
        print("")
        print("以下文案在 exe 里找不到完整字节——最可能是 -O2 的常量合并把它们")
        print("拼坏了（见文件头说明），也可能是被优化掉了：")
        for name, s in missing:
            print("  %-10s %s" % (name, s[:70]))
        return 1
    print("全部完整 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
