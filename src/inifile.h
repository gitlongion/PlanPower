// ============================================================================
//  PlanPower - inifile.h
//  INI 文本的解析与编解码（纯逻辑，不碰文件系统，可在宿主机上单独测试）
//
//  为什么不用 kernel32 的 GetPrivateProfileString / WritePrivateProfileString：
//
//  那一套 API 会在进程内缓存整个文件的映射：写入先进缓存、延迟落盘。
//  PlanPower 保存配置后会**立刻**启动提权助手进程去读同一个 INI，
//  助手从磁盘读到的是旧内容（甚至文件还不存在），于是报
//  「配置里找不到这个任务」——症状还伪装成「注册系统任务失败」。
//
//  与其去猜缓存的刷新时机（而且缓存里可能还留着上一个版本的陈旧键值，
//  让「先删文件再重写」这个手法失效），不如自己读写：
//  格式还是 INI，记事本照样能改，而且读写都在同一个调用里落盘。
//
//  这一层刻意只依赖 <string> / <vector>，所以能塞进宿主测试里跑。
// ============================================================================
#pragma once
#include <string>
#include <vector>

struct IniEntry {
    std::wstring section;   // 段名；段头之前出现的键值对 section 为空
    std::wstring key;
    std::wstring value;
};

// 解析 INI 文本。
// 容忍：CRLF / LF / CR 三种换行、行首行尾空白、空行、以 ; 或 # 开头的整行注释。
// 值里第一个 = 之后的内容原样保留——关机提示文本里完全可能出现 = 或 [。
std::vector<IniEntry> IniParse(const std::wstring& text);

// 按段名 + 键名取值，大小写不敏感；找不到返回 def。
std::wstring IniGet(const std::vector<IniEntry>& doc,
                    const std::wstring& section, const std::wstring& key,
                    const std::wstring& def);
int IniGetInt(const std::vector<IniEntry>& doc,
              const std::wstring& section, const std::wstring& key, int def);

// 段名列表，按首次出现顺序，大小写不敏感去重。
std::vector<std::wstring> IniSections(const std::vector<IniEntry>& doc);

// 某个段下的全部键值对，按出现顺序（同名键后者覆盖前者，这里只返回全部原始项）。
std::vector<std::pair<std::wstring, std::wstring>>
IniSectionEntries(const std::vector<IniEntry>& doc, const std::wstring& section);

// 段名/键名的 ASCII 大小写折叠。INI 段名与键名在我们的格式里都是 ASCII，
// 手改文件时也只会是 ASCII，所以不做完整的 Unicode 折叠。
std::wstring IniLower(const std::wstring& s);
bool IniStartsWithNoCase(const std::wstring& s, const std::wstring& prefix);

// ---------------------------------------------------------------------------
//  编解码
// ---------------------------------------------------------------------------
// 文本 → UTF-16LE + BOM 字节。与 Profile API 写出来的一致，记事本可正确识别中文。
void TextToUtf16Le(const std::wstring& text, std::vector<unsigned char>* out);

// 字节 → 文本。按 BOM 识别 UTF-16LE / UTF-16BE / UTF-8；
// 无 BOM 时按严格 UTF-8 解，失败返回 false（调用方可以再退回 ANSI 代码页）。
bool TextFromBytes(const std::vector<unsigned char>& bytes, std::wstring* out);
