// ============================================================================
//  PlanPower - sched.cpp
//  系统级持久任务：COM 封装 Windows 任务计划程序 + 提权执行
//
//  纯逻辑（名称、时间边界、任务 XML）在 sched_xml.cpp 里，那部分不碰系统，
//  可以在没有 Windows 的机器上单独编译运行并逐条断言。
//
//  为什么用 XML 而不是对象模型：
//    ITaskDefinition / ITriggerCollection / IActionCollection 那一套要逐层
//    创建对象、逐个 put 属性，链长且每一步都可能失败。改成拼 XML 字符串后，
//    「生成什么」变成纯函数，出错面小得多。ITaskFolder::RegisterTask 接受
//    BSTR 形式的 XML，正是为这条路准备的。
//
//  为什么不用 schtasks.exe：
//    设计约束是「运行时零子进程」。COM 是进程内调用（服务端在 svchost 里），
//    不产生子进程，也不需要在磁盘上落地临时 XML（那会引入 UTF-16LE+BOM 的
//    编码要求，还会在只读介质上失败）。
// ============================================================================
#include "sched.h"
#include "inifile.h"        // TextToUtf16Le：写诊断日志用

#include <taskschd.h>
#include <shellapi.h>
#include <sddl.h>          // ConvertSidToStringSidW
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

// BSTR 的 RAII 包装。MinGW 的 comdef.h 不一定可用，所以自己写一个，
// 避免依赖 _bstr_t。
class Bstr {
public:
    explicit Bstr(const std::wstring& s) : p_(SysAllocStringLen(nullptr, (UINT)s.size())) {
        if (p_ && !s.empty()) memcpy(p_, s.c_str(), s.size() * sizeof(wchar_t));
    }
    ~Bstr() { if (p_) SysFreeString(p_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR get() const { return p_; }
private:
    BSTR p_;
};

static std::wstring IntToStr(int v) {
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    return b;
}

static std::wstring HexErr(HRESULT hr) {
    wchar_t b[32];
    swprintf(b, 32, L"0x%08X", (unsigned)hr);
    return b;
}

// 问系统要一段人话。比在代码里硬编码错误码表准确得多，也不会因为
// 以后新增的错误码而漏掉，而且跟随系统语言（中文系统就是中文）。
static std::wstring FormatOne(DWORD code) {
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&buf, 0, nullptr);
    std::wstring s;
    if (n && buf) {
        s.assign(buf, n);
        while (!s.empty() &&
               (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
            s.pop_back();
    }
    if (buf) LocalFree(buf);
    return s;
}

static std::wstring HrText(HRESULT hr) {
    std::wstring s = FormatOne((DWORD)hr);
    // 0x8007xxxx 是 Win32 错误码套上 HRESULT 外壳。系统消息表里往往没有这个
    // 组合，查不到；再拿低 16 位查一次就能拿到「拒绝访问」这类真正有用的描述。
    if (s.empty() && ((unsigned)hr & 0xFFFF0000u) == 0x80070000u)
        s = FormatOne((DWORD)((unsigned)hr & 0xFFFFu));
    return s;
}

static void SetErr(std::wstring* err, const wchar_t* what, HRESULT hr) {
    if (!err) return;
    *err = std::wstring(what) + L"\n错误码：" + HexErr(hr);
    std::wstring t = HrText(hr);
    if (!t.empty()) *err += L"\n系统说明：" + t;
}

// 完整 exe 路径（不是目录）
static std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH * 2) == 0) return L"";
    return buf;
}

// System32 目录，含结尾反斜杠。
// 特意用 GetWindowsDirectoryW 而不是 GetSystemDirectoryW：x86 版本在 64 位
// 系统上跑时，后者会被 WOW64 重定向到 SysWOW64，那不是我们要的目录。
static std::wstring System32Dir() {
    wchar_t win[MAX_PATH * 2] = {};
    if (GetWindowsDirectoryW(win, MAX_PATH * 2) == 0) return L"";
    std::wstring p = win;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    p += L"System32\\";
    return p;
}

// 当前登录用户的 SID（注销 / 锁定必须在交互式会话里执行）
static std::wstring CurrentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 256);

    std::wstring sid;
    if (GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &len)) {
        TOKEN_USER* tu = (TOKEN_USER*)buf.data();
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &s)) {
            sid = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return sid;
}

// 收集本次生成 XML 所需的全部外部输入
static SchedEnv MakeEnv() {
    SchedEnv env;
    GetLocalTime(&env.now);
    env.systemDir = System32Dir();
    env.driveTag  = DriveTag();
    env.machineFp = MachineFingerprint();
    env.userSid   = CurrentUserSid();
    return env;
}

// RegisterTaskDefinition 的 userId 参数按文档是「用户凭据」，且明确说明
// 「如果任务按 Task Scheduler 1.0 定义，这个参数不要用组名而要用具体用户名」
// ——可见它走的是账户名解析，不是 SID 解析。实测可用的写法（schtasks /ru、
// 各类官方示例）一律是账户名。所以这里给出账户名，SID 作为备选。
static std::wstring AccountNameForUser(bool interactive) {
    if (!interactive) return L"SYSTEM";     // 内置 Local System 的账户名

    // 交互式（注销 / 锁定）：把当前用户的 SID 翻回 SAM 名（DOMAIN\User）
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 256);

    std::wstring name;
    if (GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &len)) {
        TOKEN_USER* tu = (TOKEN_USER*)buf.data();
        wchar_t acc[256] = {};
        wchar_t dom[256] = {};
        DWORD accLen = 256, domLen = 256;
        SID_NAME_USE use;
        if (LookupAccountSidW(nullptr, tu->User.Sid, acc, &accLen, dom, &domLen, &use)) {
            if (dom[0]) { name = dom; name += L'\\'; }
            name += acc;
        }
    }
    CloseHandle(tok);
    return name;
}

// ---------------------------------------------------------------------------
//  诊断日志
// ---------------------------------------------------------------------------

static std::wstring ExeDirPath() {
    std::wstring exe = ExePath();
    size_t p = exe.find_last_of(L'\\');
    if (p == std::wstring::npos) return L"";
    return exe.substr(0, p + 1);
}

std::wstring SchedLogPath() {
    // 首选 exe 同目录——便携版的一切都该落在自己身边。只读介质或没有写权限
    // 时退到临时目录。写不了日志不是错误，绝不能让主流程因此失败。
    std::wstring dir = ExeDirPath();
    if (!dir.empty()) {
        std::wstring p = dir + L"PlanPower_task.log";
        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return p; }
    }
    wchar_t tmp[MAX_PATH * 2] = {};
    DWORD n = GetTempPathW(MAX_PATH * 2, tmp);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    return std::wstring(tmp) + L"PlanPower_task.log";
}

void SchedLog(const std::wstring& text) {
    std::wstring path = SchedLogPath();
    if (path.empty()) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t head[80];
    swprintf(head, 80, L"\r\n===== %04d-%02d-%02d %02d:%02d:%02d =====\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring full = std::wstring(head) + text;
    if (full.empty() || full.back() != L'\n') full += L"\r\n";

    std::vector<unsigned char> bytes;
    TextToUtf16Le(full, &bytes);          // 带 FF FE BOM，记事本能直接打开

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // 只有新建的文件才写 BOM；追加时摘掉，免得中间冒出一个 BOM 把文本切断
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 &&
        bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        bytes.erase(bytes.begin(), bytes.begin() + 2);

    DWORD written = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
//  对外：名称
// ---------------------------------------------------------------------------

std::wstring SchedTaskLeafName(const PowerTask& t) {
    return ::SchedTaskLeafName(t, DriveTag());
}

std::wstring SchedTaskFullPath(const PowerTask& t) {
    return ::SchedTaskFullPath(t, DriveTag());
}

// ---------------------------------------------------------------------------
//  对外：XML 生成（补上环境信息后转交纯逻辑）
// ---------------------------------------------------------------------------

bool SchedBuildXml(const PowerTask& t, std::wstring* xml, std::wstring* err) {
    return ::SchedBuildXml(t, MakeEnv(), xml, err);
}

bool SchedBuildXml(const PowerTask& t, const SchedXmlOpts& opts,
                   std::wstring* xml, std::wstring* err) {
    return ::SchedBuildXml(t, MakeEnv(), opts, xml, err);
}

// ---------------------------------------------------------------------------
//  能力查询
// ---------------------------------------------------------------------------

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;

    TOKEN_ELEVATION el = {};
    DWORD len = sizeof(el);
    bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) != FALSE;
    CloseHandle(tok);
    return ok && el.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
//  COM 基础设施
// ---------------------------------------------------------------------------

static bool ConnectService(ITaskService** out, std::wstring* err) {
    *out = nullptr;

    ITaskService* svc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, (void**)&svc);
    if (FAILED(hr) || !svc) {
        SetErr(err, L"无法创建任务计划程序对象", hr);
        return false;
    }

    VARIANT v;
    VariantInit(&v);
    hr = svc->Connect(v, v, v, v);
    VariantClear(&v);
    if (FAILED(hr)) {
        svc->Release();
        SetErr(err, L"无法连接任务计划程序服务（服务可能被禁用）", hr);
        return false;
    }
    *out = svc;
    return true;
}

// 取 \PlanPower 文件夹；create=false 时不存在就返回 false（不算错误）
static bool GetOrCreateFolder(ITaskService* svc, bool create,
                              ITaskFolder** out, std::wstring* err) {
    *out = nullptr;

    Bstr path(SchedFolderPath());
    HRESULT hr = svc->GetFolder(path.get(), out);
    if (SUCCEEDED(hr) && *out) return true;

    if (!create) return false;

    ITaskFolder* root = nullptr;
    Bstr rootPath(L"\\");
    hr = svc->GetFolder(rootPath.get(), &root);
    if (FAILED(hr) || !root) {
        SetErr(err, L"无法打开任务计划程序根目录", hr);
        return false;
    }

    VARIANT sddl;
    VariantInit(&sddl);
    Bstr name(L"PlanPower");
    hr = root->CreateFolder(name.get(), sddl, out);
    VariantClear(&sddl);
    root->Release();
    if (FAILED(hr) || !*out) {
        SetErr(err, L"无法创建 \\PlanPower 文件夹", hr);
        return false;
    }
    return true;
}

bool SchedAvailable(std::wstring* err) {
    ITaskService* svc = nullptr;
    bool ok = ConnectService(&svc, err);
    if (svc) svc->Release();
    return ok;
}

// ---------------------------------------------------------------------------
//  注册 / 删除 / 启停
// ---------------------------------------------------------------------------

// 把任务写进系统。调用方必须已提权。
//
// 刻意拆成「先 put_XmlText 再 RegisterTaskDefinition」两步，而不是一步
// RegisterTask(路径, XML)：一步走的时候，XML 写错、权限不够、文件夹不存在、
// 任务重名，全都返回同一个失败，报错只能写得很含糊。拆开之后，
//   · put_XmlText 失败  → 一定是这段 XML 本身不合法（元素写错 / 取值越界 /
//                          version 与所用元素不匹配），与系统环境无关；
//   · RegisterTaskDefinition 失败 → XML 是好的，问题出在权限、路径或身份。
// 下一次真机上再失败，看标题就知道该查哪一边。

// ---------------------------------------------------------------------------
//  注册：按「定义风格 × 身份写法」的顺序降级重试
// ---------------------------------------------------------------------------
//
// 为什么要有这条链：真机上出现过 SCHED_E_INVALIDVALUE（0x80041318，「任务
// XML 包含一个格式不正确或超出范围的值」）。这个错误名下的约束有一部分是
// 文档没写、只能实测的——它取决于服务端版本、注册身份，以及若干历史遗留的
// 取值口径，在 macOS 上无法复现。
//
// 与其猜，不如把几种「都合法但服务端口味不同」的写法按优先级试一遍：能跑通
// 就是好定义；全都跑不通，就把每一次的返回码和用到的完整 XML 写进日志，下一
// 次直接定位到是哪个取值不被接受。重试全部发生在同一个提权进程里，不会重复
// 弹 UAC。
struct SchedAttempt {
    const wchar_t* label;
    SchedXmlStyle  style;
    const wchar_t* version;    // <Task version="...">
    bool           sidAsUser;  // userId 参数传 SID（true）还是账户名（false）
};

static std::vector<SchedAttempt> AttemptsFor() {
    // 顺序即优先级。前三条语义完全一致，差别只在「服务端可能挑剔的写法」：
    // 先试账户名（文档口径）与 1.2（覆盖面最广），再试 1.4，最后回到 SID。
    // 后两条是兜底：丢掉的是与默认值等价的冗余项，行为承诺不变。
    std::vector<SchedAttempt> v;
    v.push_back({L"完整定义 · 账户名 · 1.2", SchedXmlStyle::Rich,         L"1.2", false});
    v.push_back({L"完整定义 · 账户名 · 1.4", SchedXmlStyle::Rich,         L"1.4", false});
    v.push_back({L"完整定义 · SID · 1.2",    SchedXmlStyle::Rich,         L"1.2", true });
    v.push_back({L"精简定义 · 账户名 · 1.4", SchedXmlStyle::Conservative, L"1.4", false});
    v.push_back({L"精简定义 · SID · 1.4",    SchedXmlStyle::Conservative, L"1.4", true });
    return v;
}

// 单次尝试。失败时 *why 给出「哪一步 + 错误码 + 系统说明」，*xmlOut 回填
// 本次用到的 XML（供日志记录）。
static bool RegisterOne(ITaskService* svc, ITaskFolder* folder,
                        const PowerTask& t, const SchedAttempt& at,
                        const std::wstring& userVal,
                        std::wstring* xmlOut, std::wstring* why) {
    SchedXmlOpts opts;
    opts.style   = at.style;
    opts.version = at.version;

    std::wstring xml;
    if (!::SchedBuildXml(t, MakeEnv(), opts, &xml, why)) return false;
    if (xmlOut) *xmlOut = xml;

    ITaskDefinition* def = nullptr;
    HRESULT hr = svc->NewTask(0, &def);
    if (FAILED(hr) || !def) {
        SetErr(why, L"无法创建任务定义对象", hr);
        return false;
    }

    Bstr bxml(xml);
    hr = def->put_XmlText(bxml.get());
    if (FAILED(hr)) {
        def->Release();
        SetErr(why, L"XML 本身被拒（put_XmlText）", hr);
        return false;
    }

    // logonType 参数决定服务端在注册这一刻用什么身份去校验。
    // TASK_LOGON_SERVICE_ACCOUNT 要求 password 必须是空 VARIANT。
    bool interactive = (t.action == PowerAction::Logoff || t.action == PowerAction::Lock);
    VARIANT user, pass, sddl;
    VariantInit(&user);
    VariantInit(&pass);
    VariantInit(&sddl);
    user.vt = VT_BSTR;
    user.bstrVal = SysAllocString(userVal.c_str());

    TASK_LOGON_TYPE lt = interactive ? TASK_LOGON_INTERACTIVE_TOKEN
                                     : TASK_LOGON_SERVICE_ACCOUNT;

    Bstr leaf(SchedTaskLeafName(t));
    IRegisteredTask* rt = nullptr;
    hr = folder->RegisterTaskDefinition(leaf.get(), def,
                                        TASK_CREATE_OR_UPDATE,
                                        user, pass, lt, sddl, &rt);
    VariantClear(&user);
    VariantClear(&pass);
    VariantClear(&sddl);
    def->Release();
    if (rt) rt->Release();

    if (FAILED(hr)) {
        SetErr(why, L"服务端拒绝注册（RegisterTaskDefinition）", hr);
        return false;
    }
    return true;
}

// 把 *err 里的多行文本缩进后接到日志后面
static void LogIndent(std::wstring* log, const std::wstring& text) {
    for (size_t p = 0; p < text.size(); ) {
        size_t e = text.find(L'\n', p);
        size_t n = (e == std::wstring::npos) ? std::wstring::npos : e - p;
        *log += L"         " + text.substr(p, n) + L"\r\n";
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
}

static bool RegisterWithFallback(ITaskService* svc, ITaskFolder* folder,
                                 const PowerTask& t, std::wstring* err) {
    const bool interactive = (t.action == PowerAction::Logoff ||
                              t.action == PowerAction::Lock);
    const std::wstring sid  = interactive ? CurrentUserSid() : std::wstring(L"S-1-5-18");
    const std::wstring acct = AccountNameForUser(interactive);
    const std::vector<SchedAttempt> attempts = AttemptsFor();

    std::wstring log = L"注册系统任务：" + SchedTaskLeafName(t) + L"\r\n";
    log += L"  执行身份：" + std::wstring(interactive ? L"交互式（当前用户）"
                                                      : L"Local System") + L"\r\n";
    log += L"  账户名参数：" + (acct.empty() ? std::wstring(L"（取不到）") : acct) + L"\r\n";
    log += L"  SID 参数：" + sid + L"\r\n";

    std::wstring allErrs;
    std::wstring lastXml;
    int tried = 0;

    for (size_t i = 0; i < attempts.size(); ++i) {
        const SchedAttempt& at = attempts[i];
        std::wstring userVal = at.sidAsUser ? sid : acct;
        if (userVal.empty()) {
            log += L"  [跳过] " + std::wstring(at.label) + L"：身份值为空\r\n";
            continue;
        }
        ++tried;

        std::wstring why, xml;
        if (RegisterOne(svc, folder, t, at, userVal, &xml, &why)) {
            log += L"  [成功] " + std::wstring(at.label) + L"\r\n";
            SchedLog(log);
            if (err) err->clear();
            return true;
        }
        if (!xml.empty()) lastXml = xml;

        log += L"  [失败] " + std::wstring(at.label) + L"\r\n";
        LogIndent(&log, why);

        if (!allErrs.empty()) allErrs += L"\n\n";
        allErrs += std::wstring(at.label) + L"：\n" + why;
    }

    log += L"  以上 " + IntToStr(tried) + L" 种写法全部被拒绝。最后一次尝试的 XML：\r\n";
    log += lastXml + L"\r\n";
    SchedLog(log);

    if (err) {
        *err = L"系统拒绝了全部 " + IntToStr(tried) + L" 种任务定义写法。\n\n" + allErrs;
        std::wstring lp = SchedLogPath();
        if (!lp.empty())
            *err += L"\n\n诊断日志（含每次尝试的完整 XML）：\n" + lp;
    }
    return false;
}

bool SchedDoAdd(const std::wstring& taskId, std::wstring* err) {
    if (err) err->clear();

    PowerTask* t = g_store.FindById(taskId);
    if (!t) {
        // 这条以前只写「配置里找不到这个任务」，很难查。补上配置路径和
        // 实际读到的任务条数——提权助手进程读的是磁盘上的文件，只要这里
        // 报出条数不对，就说明是父进程还没把改动落盘，而不是任务真的没了。
        if (err) {
            *err = L"配置里找不到这个任务。\n任务 ID：" + taskId +
                   L"\n配置文件：" + g_store.Path() +
                   L"\n当前读到的任务数：" +
                   IntToStr((int)g_store.Tasks().size());
        }
        return false;
    }
    if (!ActionSupportsPersistent(t->action)) {
        if (err) *err = PersistentUnsupportedReason(t->action);
        return false;
    }

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return false;

    ITaskFolder* folder = nullptr;
    bool ok = GetOrCreateFolder(svc, true, &folder, err);
    if (ok && folder) ok = RegisterWithFallback(svc, folder, *t, err);
    if (folder) folder->Release();
    svc->Release();
    return ok;
}

bool SchedDoDelete(const std::wstring& taskName, std::wstring* err) {
    if (err) err->clear();
    if (taskName.empty()) { if (err) *err = L"任务名为空。"; return false; }

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return false;

    ITaskFolder* folder = nullptr;
    if (!GetOrCreateFolder(svc, false, &folder, err)) {
        svc->Release();
        return true;                     // 文件夹都不存在 = 本来就没有，视为成功
    }

    Bstr leaf(taskName);
    HRESULT hr = folder->DeleteTask(leaf.get(), 0);
    folder->Release();
    svc->Release();

    // 已经不在了也当作成功——删除是幂等操作
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        SetErr(err, L"删除系统任务失败", hr);
        return false;
    }
    return true;
}

bool SchedDoSetEnabled(const std::wstring& taskName, bool on, std::wstring* err) {
    if (err) err->clear();
    if (taskName.empty()) { if (err) *err = L"任务名为空。"; return false; }

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return false;

    ITaskFolder* folder = nullptr;
    if (!GetOrCreateFolder(svc, false, &folder, err)) {
        svc->Release();
        return true;
    }

    Bstr leaf(taskName);
    IRegisteredTask* rt = nullptr;
    HRESULT hr = folder->GetTask(leaf.get(), &rt);
    if (SUCCEEDED(hr) && rt) {
        hr = rt->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE);
        rt->Release();
    }
    folder->Release();
    svc->Release();

    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
        SetErr(err, L"修改系统任务状态失败", hr);
        return false;
    }
    return true;
}

int SchedDoSetAllEnabled(bool on, std::wstring* err) {
    if (err) err->clear();

    // 名单来自归属表，而不是「文件夹里有什么」。
    // 按文件夹扫会把别的 U 盘在这台机器上注册的任务一起停掉——那些任务的
    // 主人在另一支盘上，看不到任何提示。
    std::vector<std::wstring> names = g_store.HostTaskNames(MachineFingerprint());
    if (names.empty()) return 0;

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return -1;

    ITaskFolder* folder = nullptr;
    if (!GetOrCreateFolder(svc, false, &folder, err)) {
        svc->Release();
        return 0;
    }

    int changed = 0;
    for (const auto& nm : names) {
        Bstr leaf(nm);
        IRegisteredTask* rt = nullptr;
        if (SUCCEEDED(folder->GetTask(leaf.get(), &rt)) && rt) {
            if (SUCCEEDED(rt->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE)))
                changed++;
            rt->Release();
        }
    }

    folder->Release();
    svc->Release();
    return changed;
}

int SchedDoCleanupAll(std::wstring* err) {
    if (err) err->clear();

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return -1;

    ITaskFolder* folder = nullptr;
    if (!GetOrCreateFolder(svc, false, &folder, err)) {
        svc->Release();
        return 0;                        // 本来就没有
    }

    // 先收集再删除：边枚举边删会让集合索引失效
    std::vector<std::wstring> names;
    IRegisteredTaskCollection* col = nullptr;
    // 必须带 TASK_ENUM_HIDDEN，否则会漏掉隐藏任务，清理不干净
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &col)) && col) {
        LONG count = 0;
        col->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;

            IRegisteredTask* rt = nullptr;
            if (SUCCEEDED(col->get_Item(idx, &rt)) && rt) {
                BSTR nm = nullptr;
                if (SUCCEEDED(rt->get_Name(&nm)) && nm) {
                    names.push_back(nm);
                    SysFreeString(nm);
                }
                rt->Release();
            }
            VariantClear(&idx);
        }
        col->Release();
    }

    int removed = 0;
    for (const auto& n : names) {
        Bstr leaf(n);
        if (SUCCEEDED(folder->DeleteTask(leaf.get(), 0))) removed++;
    }
    folder->Release();

    // 顺手删掉空文件夹，做到零残留
    ITaskFolder* root = nullptr;
    Bstr rootPath(L"\\");
    if (SUCCEEDED(svc->GetFolder(rootPath.get(), &root)) && root) {
        Bstr name(L"PlanPower");
        root->DeleteFolder(name.get(), 0);
        root->Release();
    }
    svc->Release();
    return removed;
}

// ---------------------------------------------------------------------------
//  只读枚举
// ---------------------------------------------------------------------------

bool SchedEnum(std::vector<SchedEntry>* out, std::wstring* err) {
    if (err) err->clear();
    if (!out) return false;
    out->clear();

    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return false;

    ITaskFolder* folder = nullptr;
    if (!GetOrCreateFolder(svc, false, &folder, err)) {
        svc->Release();
        return true;                     // 文件夹不存在 = 空列表
    }

    IRegisteredTaskCollection* col = nullptr;
    HRESULT hr = folder->GetTasks(TASK_ENUM_HIDDEN, &col);
    if (FAILED(hr)) {
        folder->Release();
        svc->Release();
        SetErr(err, L"读取系统任务列表失败", hr);
        return false;
    }

    if (col) {
        LONG count = 0;
        col->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;

            IRegisteredTask* rt = nullptr;
            if (SUCCEEDED(col->get_Item(idx, &rt)) && rt) {
                SchedEntry e;
                BSTR nm = nullptr;
                if (SUCCEEDED(rt->get_Name(&nm)) && nm) {
                    e.leafName = nm;
                    SysFreeString(nm);
                }
                VARIANT_BOOL en = VARIANT_TRUE;
                if (SUCCEEDED(rt->get_Enabled(&en))) e.enabled = (en != VARIANT_FALSE);
                if (!e.leafName.empty()) out->push_back(e);
                rt->Release();
            }
            VariantClear(&idx);
        }
        col->Release();
    }
    folder->Release();
    svc->Release();
    return true;
}

// ---------------------------------------------------------------------------
//  提权执行
// ---------------------------------------------------------------------------

// 给命令行参数加引号（Windows 规则：内层双引号用反斜杠转义）
static std::wstring QuoteArg(const std::wstring& s) {
    std::wstring r = L"\"";
    int backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { backslashes++; r += c; continue; }
        if (c == L'"') {
            r.append((size_t)backslashes + 1, L'\\');
            r += L'"';
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        r += c;
    }
    r.append((size_t)backslashes, L'\\');
    r += L'"';
    return r;
}

bool SchedRunElevated(SchedOp op, const std::wstring& arg,
                      bool enable, std::wstring* err) {
    if (err) err->clear();

    std::wstring exe = ExePath();
    std::wstring dir = ExeDir();
    if (exe.empty()) {
        if (err) *err = L"无法确定程序路径。";
        return false;
    }

    std::wstring params;
    switch (op) {
    case SchedOp::Add:
        // 连 INI 路径一起传过去：提权进程如果重新推导，遇到
        // 「/portable + 目录不可写」这类组合可能与父进程推导出不同的位置
        params = L"/sched-add " + QuoteArg(g_store.Path()) + L" " + QuoteArg(arg);
        break;
    case SchedOp::Delete:
        params = L"/sched-del " + QuoteArg(arg);
        break;
    case SchedOp::SetEnabled:
        params = (enable ? L"/sched-enable " : L"/sched-disable ") + QuoteArg(arg);
        break;
    case SchedOp::SetAllEnabled:
        // 把配置文件路径一起传过去：提权进程要读归属表才知道「本机的任务」是哪几条。
        params = L"/sched-set-all " + QuoteArg(arg) + (enable ? L" 1" : L" 0");
        break;
    case SchedOp::CleanupAll:
        params = L"/cleanup-all";
        break;
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";                 // 触发 UAC
    sei.lpFile       = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.lpDirectory  = dir.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (err) {
            *err = (e == ERROR_CANCELLED)
                 ? L"已取消：未获得管理员授权，系统任务未做任何改动。"
                 : L"无法启动管理员进程（错误 " + IntToStr((int)e) + L"）。";
        }
        return false;
    }

    if (!sei.hProcess) return true;

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    if (code != 0) {
        if (err) *err = L"操作未完成（管理员进程返回 " + IntToStr((int)code) + L"）。";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  自检
// ---------------------------------------------------------------------------

// 让任务计划程序自己解析一遍 XML。
//
// 以前走的是 RegisterTask + TASK_VALIDATE_ONLY。那个路子有个副作用：校验时
// 还要一并提供执行身份，而身份是否被接受与「XML 本身合不合法」是两件事，
// 容易把身份问题误报成 XML 问题。改用 NewTask + put_XmlText 后，这一步
// 只做纯粹的 XML 解析与取值域检查，正是自检想确认的东西。
static bool ValidateXmlWithService(const std::wstring& xml, std::wstring* err) {
    ITaskService* svc = nullptr;
    if (!ConnectService(&svc, err)) return false;

    ITaskDefinition* def = nullptr;
    HRESULT hr = svc->NewTask(0, &def);
    if (FAILED(hr) || !def) {
        svc->Release();
        SetErr(err, L"无法创建任务定义对象", hr);
        return false;
    }

    Bstr bxml(xml);
    hr = def->put_XmlText(bxml.get());
    def->Release();
    svc->Release();

    if (FAILED(hr)) {
        SetErr(err, L"任务计划程序不接受这段 XML", hr);
        return false;
    }
    return true;
}

void SchedSelfTest(std::vector<std::wstring>* out) {
    if (!out) return;

    out->push_back(L"=== 环境 ===");
    out->push_back(L"已提权：" + std::wstring(IsElevated() ? L"是" : L"否"));
    out->push_back(L"机器指纹：" + MachineFingerprint() + L"（" + MachineShortName() + L"）");
    out->push_back(L"介质标识：" + DriveTag());
    out->push_back(L"System32：" + System32Dir());
    out->push_back(L"配置文件：" + g_store.Path());
    out->push_back(L"诊断日志：" + SchedLogPath());
    out->push_back(L"系统任务总开关：" +
                   std::wstring(g_store.Config().allowSystemTasks ? L"已开启" : L"关闭"));

    std::wstring serr;
    bool avail = SchedAvailable(&serr);
    out->push_back(L"任务计划程序服务：" + std::wstring(avail ? L"可达" : L"不可达"));
    if (!avail) out->push_back(L"  原因：" + serr);

    out->push_back(L"");
    out->push_back(L"=== 任务 XML 生成 ===");
    int nOk = 0, nBad = 0;
    for (auto& t : g_store.Tasks()) {
        std::wstring head = L"[" + t.name + L"] ";

        if (!ActionSupportsPersistent(t.action)) {
            out->push_back(head + L"跳过：动作「" + ActionName(t.action) + L"」不支持系统任务");
            continue;
        }

        // 两套定义都要过：注册时先试完整定义，被拒就退到精简定义。
        // 只检查其中一套，等于放任另一套悄悄写坏。
        struct Variant {
            const wchar_t* label;
            SchedXmlStyle  style;
            const wchar_t* version;
            bool           wantsPrincipals;
        };
        const Variant variants[2] = {
            {L"完整", SchedXmlStyle::Rich,         L"1.2", true },
            {L"精简", SchedXmlStyle::Conservative, L"1.4", false},
        };

        for (int vi = 0; vi < 2; ++vi) {
            const Variant& v = variants[vi];
            SchedXmlOpts opts;
            opts.style   = v.style;
            opts.version = v.version;

            std::wstring xml, err;
            if (!::SchedBuildXml(t, MakeEnv(), opts, &xml, &err)) {
                out->push_back(head + v.label + L"定义生成失败：" + err);
                nBad++;
                continue;
            }

            bool hasVer   = xml.find(L"<Task version=\"" + std::wstring(v.version) + L"\"")
                            != std::wstring::npos;
            bool hasTrig  = xml.find(L"<Triggers>") != std::wstring::npos;
            bool hasPrin  = xml.find(L"<Principals>") != std::wstring::npos;
            bool hasSet   = xml.find(L"<WakeToRun>true</WakeToRun>") != std::wstring::npos;
            bool hasBatt  = xml.find(L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>")
                            != std::wstring::npos;
            bool hasAction = xml.find(L"<Actions Context=\"Author\">") != std::wstring::npos;
            // RegistrationInfo 里不能出现 <URI>：路径由注册时的参数决定，手写
            // URI 一旦与注册路径不一致就会以 0x80041318 被拒。这条断言防止
            // 以后有人「顺手」把它加回来。
            bool noUri    = xml.find(L"<URI>") == std::wstring::npos;
            bool prinOk   = (hasPrin == v.wantsPrincipals);
            // 精简定义必须只保留「会改变行为」的项；IdleSettings 这类只在
            // RunOnlyIfIdle=true 时才有意义的元素不该出现。
            bool leanOk   = v.wantsPrincipals ||
                            (xml.find(L"<IdleSettings>") == std::wstring::npos &&
                             xml.find(L"<AllowStartOnDemand>") == std::wstring::npos &&
                             xml.find(L"<Priority>") == std::wstring::npos);

            std::wstring line = head + v.label + L"定义 " +
                                IntToStr((int)xml.size()) + L" 字符";
            if (hasVer && hasTrig && hasSet && hasBatt && hasAction &&
                noUri && prinOk && leanOk) {
                line += L"，结构完整";
                nOk++;
            } else {
                line += L"，结构缺项";
                if (!hasVer)   line += L"（version 不对）";
                if (!noUri)    line += L"（多出了 <URI>）";
                if (!prinOk)   line += L"（Principals 有无不对）";
                if (!leanOk)   line += L"（精简定义不够精简）";
                nBad++;
            }

            if (avail) {
                std::wstring verr;
                if (ValidateXmlWithService(xml, &verr)) line += L"，服务端校验通过";
                else                                    line += L"，服务端校验失败：" + verr;
            }
            out->push_back(line);
        }
        out->push_back(L"    系统任务名：" + SchedTaskFullPath(t));
    }
    out->push_back(L"");
    out->push_back(L"生成成功 " + IntToStr(nOk) + L" 条，失败 " + IntToStr(nBad) + L" 条");

    out->push_back(L"");
    out->push_back(L"=== 本机已注册的 PlanPower 系统任务 ===");
    std::vector<SchedEntry> list;
    std::wstring eerr;
    if (SchedEnum(&list, &eerr)) {
        if (list.empty()) out->push_back(L"（无）");
        for (auto& e : list)
            out->push_back(L"  " + e.leafName + (e.enabled ? L"  [已启用]" : L"  [已停用]"));
        out->push_back(L"合计 " + IntToStr((int)list.size()) + L" 条");
    } else {
        out->push_back(L"枚举失败：" + eerr);
    }
}
