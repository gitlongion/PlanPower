// ============================================================================
//  PlanPower - inifile.cpp
//  纯逻辑实现，见 inifile.h 的说明
// ============================================================================
#include "inifile.h"

// ---------------------------------------------------------------------------
//  文本工具
// ---------------------------------------------------------------------------
static bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v';
}

static std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring IniLower(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return r;
}

bool IniStartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = s[i], b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  解析
// ---------------------------------------------------------------------------
std::vector<IniEntry> IniParse(const std::wstring& text) {
    std::vector<IniEntry> out;
    std::wstring cur;                 // 当前段名
    size_t i = 0;

    while (i < text.size()) {
        // 切出一行（不含行尾）
        size_t e = i;
        while (e < text.size() && text[e] != L'\n' && text[e] != L'\r') ++e;
        std::wstring line = text.substr(i, e - i);

        // 跳过行尾符。\r\n 要一次吃掉两个字符，否则会多出一堆空行。
        if (e < text.size()) {
            if (text[e] == L'\r' && e + 1 < text.size() && text[e + 1] == L'\n') i = e + 2;
            else i = e + 1;
        } else {
            i = e;
        }

        line = Trim(line);
        if (line.empty()) continue;
        if (line[0] == L';' || line[0] == L'#') continue;   // 整行注释

        if (line[0] == L'[') {
            size_t r = line.find(L']');
            if (r == std::wstring::npos) continue;          // 畸形段头，跳过
            cur = Trim(line.substr(1, r - 1));
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;             // 没有 =，不是键值对

        IniEntry en;
        en.section = cur;
        en.key     = Trim(line.substr(0, eq));
        en.value   = Trim(line.substr(eq + 1));
        if (en.key.empty()) continue;
        out.push_back(en);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------
std::wstring IniGet(const std::vector<IniEntry>& doc,
                    const std::wstring& section, const std::wstring& key,
                    const std::wstring& def) {
    const std::wstring want = IniLower(section);
    const std::wstring wk   = IniLower(key);
    // 倒着找：同名键以最后一个为准，与 INI 的通常语义一致
    for (size_t i = doc.size(); i-- > 0; ) {
        if (IniLower(doc[i].section) == want && IniLower(doc[i].key) == wk)
            return doc[i].value;
    }
    return def;
}

int IniGetInt(const std::vector<IniEntry>& doc,
              const std::wstring& section, const std::wstring& key, int def) {
    std::wstring v = IniGet(doc, section, key, L"");
    if (v.empty()) return def;

    // 手写解析而不用 _wtoi：_wtoi 在遇到非数字时返回 0，会把
    // "abc" 和 "0" 混为一谈，看不出文件被改坏了。
    size_t i = 0;
    bool neg = false;
    while (i < v.size() && IsSpace(v[i])) ++i;
    if (i < v.size() && (v[i] == L'-' || v[i] == L'+')) { neg = (v[i] == L'-'); ++i; }
    if (i >= v.size() || v[i] < L'0' || v[i] > L'9') return def;

    long long n = 0;
    while (i < v.size() && v[i] >= L'0' && v[i] <= L'9') {
        n = n * 10 + (v[i] - L'0');
        if (n > 0x7FFFFFFFLL) { n = 0x7FFFFFFFLL; break; }
        ++i;
    }
    return (int)(neg ? -n : n);
}

std::vector<std::wstring> IniSections(const std::vector<IniEntry>& doc) {
    std::vector<std::wstring> out;
    std::vector<std::wstring> seen;
    for (const auto& e : doc) {
        if (e.section.empty()) continue;
        std::wstring low = IniLower(e.section);
        bool dup = false;
        for (const auto& s : seen) if (s == low) { dup = true; break; }
        if (dup) continue;
        seen.push_back(low);
        out.push_back(e.section);
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>>
IniSectionEntries(const std::vector<IniEntry>& doc, const std::wstring& section) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const std::wstring want = IniLower(section);
    for (const auto& e : doc) {
        if (IniLower(e.section) == want) out.push_back({ e.key, e.value });
    }
    return out;
}

// ---------------------------------------------------------------------------
//  编码
// ---------------------------------------------------------------------------
static void Push16(std::vector<unsigned char>* out, unsigned int c) {
    out->push_back((unsigned char)(c & 0xFF));
    out->push_back((unsigned char)((c >> 8) & 0xFF));
}

void TextToUtf16Le(const std::wstring& text, std::vector<unsigned char>* out) {
    out->clear();
    out->reserve(2 + text.size() * 2);
    out->push_back(0xFF);                 // BOM：让记事本认出这是 UTF-16LE
    out->push_back(0xFE);

    for (size_t i = 0; i < text.size(); ++i) {
        unsigned int c = (unsigned int)text[i];
        if (sizeof(wchar_t) > 2 && c > 0xFFFF) {
            // 宿主机上 wchar_t 是 32 位，一个元素就是一个完整码点；
            // Windows 上 wchar_t 是 16 位，非 BMP 字符本来就以代理对存放。
            // 这里把前者拆成代理对，让两种平台的字节输出完全一致——
            // 否则宿主测试通过的编解码，到了 Windows 上对不上。
            c -= 0x10000;
            Push16(out, 0xD800u + (c >> 10));
            Push16(out, 0xDC00u + (c & 0x3FFu));
        } else {
            Push16(out, c);
        }
    }
}

// ---------------------------------------------------------------------------
//  解码
// ---------------------------------------------------------------------------
// 严格 UTF-8：遇到非法序列返回 false，而不是替换成 U+FFFD。
// 这样调用方才能区分「这是 UTF-8」和「这大概是 ANSI，得换代码页」。
static bool DecodeUtf8(const unsigned char* p, size_t n, std::wstring* out) {
    out->clear();
    size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        unsigned int cp = 0;
        size_t need = 0;

        if (b < 0x80)             { cp = b;         need = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; need = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; need = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; need = 3; }
        else return false;                       // 0x80..0xBF 开头，或 0xF8+

        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; ++k) {
            unsigned char c = p[i + k];
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        i += need + 1;

        // 过长编码与代理区码点都不是合法 UTF-8
        if (need == 1 && cp < 0x80) return false;
        if (need == 2 && cp < 0x800) return false;
        if (need == 3 && cp < 0x10000) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;

        if (cp > 0xFFFF) {
            if (sizeof(wchar_t) > 2) {
                out->push_back((wchar_t)cp);
            } else {
                cp -= 0x10000;
                out->push_back((wchar_t)(0xD800u + (cp >> 10)));
                out->push_back((wchar_t)(0xDC00u + (cp & 0x3FFu)));
            }
        } else {
            out->push_back((wchar_t)cp);
        }
    }
    return true;
}

bool TextFromBytes(const std::vector<unsigned char>& bytes, std::wstring* out) {
    out->clear();
    const size_t n = bytes.size();
    if (n == 0) return true;
    const unsigned char* p = bytes.data();

    // ---- UTF-16LE ----
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        for (size_t i = 2; i + 1 < n; i += 2)
            out->push_back((wchar_t)((unsigned)p[i] | ((unsigned)p[i + 1] << 8)));
        return true;
    }

    // ---- UTF-16BE ----
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        for (size_t i = 2; i + 1 < n; i += 2)
            out->push_back((wchar_t)(((unsigned)p[i] << 8) | (unsigned)p[i + 1]));
        return true;
    }

    // ---- UTF-8（可能带 BOM）----
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return DecodeUtf8(p + 3, n - 3, out);

    // ---- 无 BOM：先当 UTF-8 试 ----
    // 注意纯 ASCII 也走这条路，而且必然成功——ANSI 文件里只要没有非 ASCII
    // 字节，两种解释本来就一样。
    if (DecodeUtf8(p, n, out)) return true;

    out->clear();
    return false;
}
