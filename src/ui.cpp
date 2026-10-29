// ============================================================================
//  PlanPower - ui.cpp  界面层
//  主窗口（自绘任务列表）/ 托盘 / 任务编辑窗 / 执行前提醒窗
//  全部使用 GDI 自绘，不依赖任何第三方 UI 库
// ============================================================================
#include "ui.h"
#include "sched.h"
#include "dateutil.h"      // ppdate::ValidDate —— 日期合法性只有这一份实现

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

// ---------------------------------------------------------------------------
//  全局
// ---------------------------------------------------------------------------
HINSTANCE g_hInst = nullptr;

static HWND   g_hMain   = nullptr;
static HWND   g_hEditor = nullptr;
static HWND   g_hNotice = nullptr;
static HWND   g_hSet    = nullptr;   // 设置窗
static HWND   g_hConfirm = nullptr;  // 「启用系统级持久任务」确认窗

static int    g_dpi    = 96;
static int    g_scroll = 0;
static int    g_filter = 0;          // 0=全部 1=运行中 2=已停用
static int    g_hoverRow  = -1;
static int    g_hoverElem = 0;
// 键盘选中的行（-1 = 没有选中）。和 hover 分开：鼠标划过不代表选中，
// 而方向键需要一个「上一次停在哪」的记忆。
static int    g_selRow    = -1;

static HFONT  g_fTitle = nullptr, g_fBody = nullptr, g_fBodyB = nullptr;
static HFONT  g_fSmall = nullptr, g_fSmallB = nullptr, g_fIcon = nullptr, g_fMono = nullptr, g_fBig = nullptr;
static HICON  g_icLarge = nullptr, g_icSmall = nullptr;
static NOTIFYICONDATAW g_nid = {};
static bool   g_trayAdded = false;
static bool   g_iconFontOk = true;

// 正在提醒中的任务（用于提醒窗）
static std::wstring g_noticeTaskId;

// 执行历史窗（定义在设置窗之后，托盘菜单要用，先声明）
static void ShowHistory(HWND owner);


#define WM_TRAYMSG (WM_APP + 1)
#define IDT_TICK   1

// ---------------------------------------------------------------------------
//  缩放与颜色
// ---------------------------------------------------------------------------
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }

#define C_BG         RGB(0xF5,0xF6,0xF8)
#define C_CARD       RGB(0xFF,0xFF,0xFF)
#define C_HOVER      RGB(0xF8,0xFA,0xFC)
#define C_LINE       RGB(0xE5,0xE7,0xEB)
#define C_LINE2      RGB(0xF1,0xF3,0xF6)
#define C_INK        RGB(0x1F,0x29,0x37)
#define C_INK2       RGB(0x4B,0x55,0x63)
#define C_INK3       RGB(0x8B,0x95,0xA5)
#define C_BRAND      RGB(0x25,0x63,0xEB)
#define C_BRAND_S    RGB(0xEE,0xF3,0xFF)
#define C_TEAL       RGB(0x0D,0x94,0x88)
#define C_TEAL_S     RGB(0xE9,0xF9,0xF6)
#define C_WARN       RGB(0xC2,0x41,0x0C)
#define C_WARN_S     RGB(0xFF,0xF5,0xED)
#define C_DANGER     RGB(0xDC,0x26,0x26)
#define C_DANGER_S   RGB(0xFE,0xF2,0xF2)
#define C_DISABLED   RGB(0xCB,0xD5,0xE1)
#define C_DISABLED_S RGB(0xF1,0xF5,0xF9)

// ---------------------------------------------------------------------------
//  绘图辅助
// ---------------------------------------------------------------------------
static void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void RoundFill(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void DrawTextC(HDC dc, const std::wstring& s, RECT r, HFONT f,
                      COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    int obk = SetBkMode(dc, TRANSPARENT);
    COLORREF oc = SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
    SetTextColor(dc, oc);
    SetBkMode(dc, obk);
    SelectObject(dc, of);
}

static int TextWidth(HDC dc, const std::wstring& s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

// 圆角胶囊标签
static int DrawPill(HDC dc, int x, int y, const std::wstring& text,
                    COLORREF bg, COLORREF fg, HFONT f) {
    int tw = TextWidth(dc, text, f);
    int w  = tw + S(16);
    int h  = S(19);
    RECT r = { x, y, x + w, y + h };
    RoundFill(dc, r, h / 2, bg, bg);
    RECT tr = { x, y - S(1), x + w, y + h - S(1) };
    DrawTextC(dc, text, tr, f, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
}

// ---------------------------------------------------------------------------
//  字体
// ---------------------------------------------------------------------------
static HFONT MakeFont(int pt, bool bold, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// 安全的宽字符串拷贝（不依赖 wcsncpy_s，跨 MSVC / MinGW 一致）
static void CopyStr(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = L'\0';
}

// 字体枚举回调必须是 CALLBACK（__stdcall），否则 32 位下会破坏栈
static int CALLBACK EnumFontCb(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) {
    *(bool*)p = true;
    return 0;
}

static bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, 32);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, EnumFontCb, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static void CreateFonts() {
    const wchar_t* ui = L"Microsoft YaHei UI";
    if (!FontExists(ui)) ui = L"Microsoft YaHei";
    if (!FontExists(ui)) ui = L"Segoe UI";

    g_iconFontOk = FontExists(L"Segoe MDL2 Assets");
    const wchar_t* ic = g_iconFontOk ? L"Segoe MDL2 Assets" : L"Segoe UI Symbol";

    g_fTitle    = MakeFont(17, true,  ui);
    g_fBody     = MakeFont(11, false, ui);
    g_fBodyB    = MakeFont(11, true,  ui);
    g_fSmall    = MakeFont(9,  false, ui);
    g_fSmallB   = MakeFont(9,  true,  ui);
    g_fBig      = MakeFont(28, true,  ui);
    g_fIcon     = MakeFont(11, false, ic);
    g_fMono     = MakeFont(13, true,  L"Consolas");
}

static void DestroyFonts() {
    HFONT* all[] = { &g_fTitle, &g_fBody, &g_fBodyB, &g_fSmall, &g_fSmallB,
                     &g_fBig, &g_fIcon, &g_fMono };
    for (HFONT* f : all) if (*f) { DeleteObject(*f); *f = nullptr; }
}

// ---------------------------------------------------------------------------
//  图标（运行时用 GDI 绘制，避免引入 .ico 资源）
// ---------------------------------------------------------------------------
static HICON MakeIcon(int size) {
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);

    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(BITMAPV5HEADER);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;          // top-down
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    if (!color) { DeleteDC(mem); ReleaseDC(nullptr, hdc); return nullptr; }

    HGDIOBJ oldBmp = SelectObject(mem, color);

    // 清为全透明
    memset(bits, 0, (size_t)size * size * 4);

    // 圆角方块 + 竖向渐变（手工逐行绘制，避免依赖 msimg32.dll）
    int pad = size >= 32 ? 1 : 0;
    RECT r = { pad, pad, size - pad, size - pad };
    int rad = (size >= 32) ? 7 : 3;

    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectClipRgn(mem, rgn);

    int span = r.bottom - r.top;
    for (int yy = r.top; yy < r.bottom; ++yy) {
        double k = (span > 0) ? (double)(yy - r.top) / (double)span : 0.0;
        int rr = (int)(0x2A + (0x0D - 0x2A) * k);
        int gg = (int)(0x5A + (0x94 - 0x5A) * k);
        int bb = (int)(0xF0 + (0x88 - 0xF0) * k);
        RECT line = { r.left, yy, r.right, yy + 1 };
        FillSolid(mem, line, RGB(rr, gg, bb));
    }

    SelectClipRgn(mem, nullptr);
    DeleteObject(rgn);

    // 白色 "P"
    if (size >= 32) {
        HFONT f = CreateFontW(-MulDiv(20, size, 32), 0, 0, 0, FW_BOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ of = SelectObject(mem, f);
        int obk = SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        RECT tr = { 0, 0, size, size };
        DrawTextW(mem, L"P", 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetBkMode(mem, obk);
        SelectObject(mem, of);
        DeleteObject(f);
    }

    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);

    // GDI 不会写 alpha 通道，手工补上
    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < size * size; ++i) {
        if ((px[i] & 0x00FFFFFF) != 0) px[i] |= 0xFF000000;
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

HICON UI_GetIcon(int size) {
    return size >= 32 ? g_icLarge : g_icSmall;
}

// ---------------------------------------------------------------------------
//  过滤后的任务列表
// ---------------------------------------------------------------------------
// 任务是否「生效中」——注意这里不能直接看 running。
//
// running 的含义是「已排进本进程的内存计时器」。它对两类任务不成立：
//   · 每天 / 每周：一旦排上，执行完立刻重排，running 恒为真；
//   · 系统持久任务：执行者是 Windows，本进程根本不排期，running 恒为假。
// 而「启用 / 停用」的语义字段是 enabled。两者混用会让开关的显示与点击
// 都指向错误的东西，所以这里显式分开。
static bool TaskActive(const PowerTask& t) {
    if (!t.enabled) return false;
    return t.persistent ? true : t.running;
}

static bool TaskMatchesFilter(const PowerTask& t) {
    switch (g_filter) {
        case 1: return TaskActive(t);
        case 2: return !t.enabled;
        default: return true;
    }
}

static std::vector<PowerTask*> VisibleTasks() {
    std::vector<PowerTask*> v;
    for (auto& t : g_store.Tasks())
        if (TaskMatchesFilter(t)) v.push_back(&t);
    return v;
}

static int CountFilter(int f) {
    int n = 0;
    for (auto& t : g_store.Tasks()) {
        switch (f) {
            case 0: n++; break;
            case 1: if (TaskActive(t)) n++; break;
            case 2: if (!t.enabled) n++; break;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
//  布局
// ---------------------------------------------------------------------------
static const int HEADER_H = 78;
static const int FILTER_H = 46;
static const int STATUS_H = 30;
static const int ROW_H    = 80;

struct Layout {
    RECT rcHeader, rcFilter, rcList, rcStatus;
    RECT rcNewBtn, rcSetBtn, rcHistBtn;
    RECT rcChip[3];
};

static Layout ComputeLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    Layout L = {};
    L.rcHeader = { 0, 0, W, S(HEADER_H) };
    L.rcFilter = { 0, S(HEADER_H), W, S(HEADER_H + FILTER_H) };
    L.rcList   = { 0, S(HEADER_H + FILTER_H), W, H - S(STATUS_H) };
    L.rcStatus = { 0, H - S(STATUS_H), W, H };

    int bh = S(34);
    int by = S(22);
    L.rcNewBtn = { W - S(22) - S(126), by, W - S(22), by + bh };
    L.rcSetBtn = { L.rcNewBtn.left - S(8) - S(34), by, L.rcNewBtn.left - S(8), by + bh };
    // 历史按钮排在设置左边，同宽同高，只换图标
    L.rcHistBtn = { L.rcSetBtn.left - S(8) - S(34), by, L.rcSetBtn.left - S(8), by + bh };

    static const wchar_t* names[3] = { L"全部", L"运行中", L"已停用" };
    int x = S(22);
    HDC dc = GetDC(hwnd);
    for (int i = 0; i < 3; ++i) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%ls  %d", names[i], CountFilter(i));
        int w = TextWidth(dc, buf, g_fSmall) + S(24);
        L.rcChip[i] = { x, S(HEADER_H + 9), x + w, S(HEADER_H + 37) };
        x += w + S(4);
    }
    ReleaseDC(hwnd, dc);
    return L;
}

static RECT RowRect(const Layout& L, int i) {
    int top = L.rcList.top + S(6) + i * S(ROW_H) - g_scroll;
    RECT r = { S(12), top, L.rcList.right - S(12), top + S(ROW_H) - S(6) };
    return r;
}

// 行内元素
enum RowElem { ELEM_NONE = 0, ELEM_TOGGLE, ELEM_RUN, ELEM_EDIT, ELEM_DEL };

static void RowElementRects(const Layout& L, int i,
                            RECT* toggle, RECT* run, RECT* edit, RECT* del) {
    RECT r = RowRect(L, i);
    int cy = (r.top + r.bottom) / 2;
    int W  = L.rcList.right;

    RECT d = { W - S(22) - S(28), cy - S(14), W - S(22), cy + S(14) };
    *del = d;
    RECT e = { d.left - S(30), d.top, d.left - S(2), d.bottom };
    *edit = e;
    RECT p = { e.left - S(30), e.top, e.left - S(2), e.bottom };
    *run = p;

    int tw = S(42), th = S(22);
    RECT t = { p.left - S(22) - tw, cy - th / 2, p.left - S(22), cy + th / 2 };
    *toggle = t;
}

static int HitTestRow(HWND hwnd, POINT pt) {
    Layout L = ComputeLayout(hwnd);
    if (pt.x < L.rcList.left || pt.x >= L.rcList.right) return -1;
    if (pt.y < L.rcList.top  || pt.y >= L.rcList.bottom) return -1;

    auto vis = VisibleTasks();
    for (size_t i = 0; i < vis.size(); ++i) {
        RECT r = RowRect(L, (int)i);
        if (pt.y >= r.top && pt.y < r.bottom) return (int)i;
    }
    return -1;
}

static int HitTestElem(HWND hwnd, POINT pt, int row) {
    if (row < 0) return ELEM_NONE;
    Layout L = ComputeLayout(hwnd);
    RECT tg, rn, ed, dl;
    RowElementRects(L, row, &tg, &rn, &ed, &dl);
    if (PtInRect(&tg, pt)) return ELEM_TOGGLE;
    if (PtInRect(&rn, pt)) return ELEM_RUN;
    if (PtInRect(&ed, pt)) return ELEM_EDIT;
    if (PtInRect(&dl, pt)) return ELEM_DEL;
    return ELEM_NONE;
}

// ---------------------------------------------------------------------------
//  绘制主窗口
// ---------------------------------------------------------------------------
static void DrawHeader(HDC dc, const Layout& L) {
    FillSolid(dc, L.rcHeader, C_CARD);

    RECT rt = { S(22), S(16), S(500), S(44) };
    DrawTextC(dc, L"PlanPower", rt, g_fTitle, C_INK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 副标题：统计 + 下次执行
    // 只数**启用中**的任务。停用的系统任务照样占着 persistent 字段，
    // 把它算进「由 Windows 调度」会让一句停用过的任务看起来还在跑。
    int memActive = 0, sysActive = 0;
    ULONGLONG soonest = 0;
    bool hasSoon = false;
    for (auto& t : g_store.Tasks()) {
        if (!t.enabled) continue;
        if (t.persistent) { sysActive++; continue; }
        if (!t.running) continue;
        memActive++;
        if (!hasSoon || t.dueTick < soonest) { soonest = t.dueTick; hasSoon = true; }
    }
    wchar_t sub[256];
    ULONGLONG now = NowTick();
    if (memActive == 0 && sysActive == 0) {
        swprintf(sub, 256, L"暂无启用的任务");
    } else if (memActive == 0) {
        swprintf(sub, 256, L"%d 个系统任务由 Windows 调度，本程序无需保持运行", sysActive);
    } else if (hasSoon && soonest > now) {
        std::wstring abs = FormatAbsolute(soonest - now);
        swprintf(sub, 256, L"%d 个任务运行中 · 最近一次 %ls", memActive, abs.c_str());
    } else {
        swprintf(sub, 256, L"%d 个任务运行中", memActive);
    }
    RECT rs = { S(22), S(46), S(600), S(66) };
    DrawTextC(dc, sub, rs, g_fSmall, C_INK3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 设置按钮
    bool setHover = (g_hoverElem == 900);
    RoundFill(dc, L.rcSetBtn, S(8), setHover ? C_HOVER : C_CARD, C_LINE);
    if (g_iconFontOk) {
        RECT r = L.rcSetBtn;
        DrawTextC(dc, L"\uE713", r, g_fIcon, C_INK2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT r = L.rcSetBtn;
        DrawTextC(dc, L"设置", r, g_fSmall, C_INK2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 历史按钮
    bool histHover = (g_hoverElem == 902);
    RoundFill(dc, L.rcHistBtn, S(8), histHover ? C_HOVER : C_CARD, C_LINE);
    {
        RECT r = L.rcHistBtn;
        DrawTextC(dc, g_iconFontOk ? L"\uE81C" : L"历史", r,
                  g_iconFontOk ? g_fIcon : g_fSmall,
                  C_INK2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 新建按钮
    bool newHover = (g_hoverElem == 901);
    RoundFill(dc, L.rcNewBtn, S(8), newHover ? RGB(0x1D,0x4E,0xD8) : C_BRAND, C_BRAND);
    RECT rn = L.rcNewBtn;
    std::wstring label = g_iconFontOk ? L"\uE710  新建任务" : L"+ 新建任务";
    DrawTextC(dc, label, rn, g_fBody, RGB(255,255,255),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 分隔线
    RECT line = { 0, L.rcHeader.bottom - 1, L.rcHeader.right, L.rcHeader.bottom };
    FillSolid(dc, line, C_LINE);
}

static void DrawFilterBar(HDC dc, const Layout& L) {
    FillSolid(dc, L.rcFilter, C_CARD);

    for (int i = 0; i < 3; ++i) {
        bool on = (g_filter == i);
        if (on) RoundFill(dc, L.rcChip[i], S(7), C_BRAND_S, C_BRAND_S);
    }

    // 底部分隔线
    RECT line = { 0, L.rcFilter.bottom - 1, L.rcFilter.right, L.rcFilter.bottom };
    FillSolid(dc, line, C_LINE2);
}

static void DrawChipsText(HDC dc, const Layout& L) {
    static const wchar_t* names[3] = { L"全部", L"运行中", L"已停用" };
    for (int i = 0; i < 3; ++i) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%ls  %d", names[i], CountFilter(i));
        RECT r = L.rcChip[i];
        DrawTextC(dc, buf, r, g_fSmall, (g_filter == i) ? C_BRAND : C_INK3,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 画一个动作图标按钮
static void DrawIconBtn(HDC dc, const RECT& r, const wchar_t* glyph,
                        const wchar_t* fallback, bool hover, bool danger) {
    if (hover) RoundFill(dc, r, S(6), danger ? C_DANGER_S : C_DISABLED_S,
                         danger ? C_DANGER_S : C_DISABLED_S);
    COLORREF c = hover ? (danger ? C_DANGER : C_INK) : C_INK3;
    RECT rr = r;
    if (g_iconFontOk)
        DrawTextC(dc, glyph, rr, g_fIcon, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else
        DrawTextC(dc, fallback, rr, g_fSmall, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// on 只表示「是否启用」。不传第二个「是否可交互」参数：停用的任务必须能点回来，
// 所以开关在任何状态下都是可点的，画成禁用态会让人以为点不了。
static void DrawToggle(HDC dc, const RECT& r, bool on) {
    int h = r.bottom - r.top;
    int rad = h / 2;
    COLORREF bg = on ? C_BRAND : C_DISABLED;
    RoundFill(dc, r, rad, bg, bg);

    int kr = rad - S(2);
    int kcx = on ? (r.right - rad) : (r.left + rad);
    int kcy = (r.top + r.bottom) / 2;

    HBRUSH b = CreateSolidBrush(RGB(255,255,255));
    HPEN   p = CreatePen(PS_SOLID, 1, RGB(255,255,255));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, kcx - kr, kcy - kr, kcx + kr, kcy + kr);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void DrawTaskRow(HDC dc, const Layout& L, int i, PowerTask* t, bool hover) {
    RECT r = RowRect(L, i);
    if (r.bottom < L.rcList.top || r.top > L.rcList.bottom) return;

    // 卡片背景
    bool sel = (g_selRow == i);
    RoundFill(dc, r, S(10), hover ? C_HOVER : C_CARD, sel ? C_BRAND : C_LINE2);

    bool warm = (t->action == PowerAction::Shutdown ||
                 t->action == PowerAction::Restart ||
                 t->action == PowerAction::Logoff);

    // 状态点
    int cy = (r.top + r.bottom) / 2;
    bool active = TaskActive(*t);
    COLORREF dot = active ? (warm ? C_WARN : C_TEAL) : C_DISABLED;
    COLORREF dotSoft = active ? (warm ? C_WARN_S : C_TEAL_S) : C_DISABLED_S;
    {
        int R = S(4);
        HBRUSH b = CreateSolidBrush(dotSoft);
        HPEN   p = CreatePen(PS_SOLID, 1, dotSoft);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        Ellipse(dc, S(24) - R - S(3), cy - R - S(3), S(24) + R + S(3), cy + R + S(3));
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(p);

        HBRUSH b2 = CreateSolidBrush(dot);
        HPEN   p2 = CreatePen(PS_SOLID, 1, dot);
        ob = SelectObject(dc, b2); op = SelectObject(dc, p2);
        Ellipse(dc, S(24) - R, cy - R, S(24) + R, cy + R);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b2); DeleteObject(p2);
    }

    // 名称 + 标签（标签超出可用宽度时自动省略，避免压到右侧区域）
    int x = S(42);
    int nameY = r.top + S(16);
    int pillLimit = L.rcList.right - S(400);
    RECT rn = { x, nameY, pillLimit - S(8), nameY + S(22) };
    DrawTextC(dc, t->name, rn, g_fBodyB,
              t->enabled ? C_INK : C_INK3,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int pillX = x + TextWidth(dc, t->name, g_fBodyB) + S(10);
    if (pillX > pillLimit) pillX = pillLimit;

    auto addPill = [&](const wchar_t* text, COLORREF bg, COLORREF fg) {
        int w = TextWidth(dc, text, g_fSmall) + S(16);
        if (pillX + w > pillLimit) return;
        DrawPill(dc, pillX, nameY + S(1), text, bg, fg, g_fSmall);
        pillX += w + S(6);
    };
    addPill(TriggerName(t->kind), RGB(0xF1,0xF5,0xF9), C_INK3);
    if (t->persistent)     addPill(L"系统任务", C_BRAND_S, C_BRAND);
    if (active)            addPill(L"运行中", C_TEAL_S, C_TEAL);
    else if (!t->enabled)  addPill(L"已停用", RGB(0xF1,0xF5,0xF9), C_INK3);
    if (warm)              addPill(L"会丢数据", C_WARN_S, C_WARN);

    // 元信息行
    std::wstring meta = std::wstring(ActionName(t->action)) + L" · " + FormatRule(*t);
    if (t->noticeSeconds > 0) {
        wchar_t nb[64];
        swprintf(nb, 64, L" · 提前 %d 秒提醒", t->noticeSeconds);
        meta += nb;
    }
    if (t->forceClose) meta += L" · 强制关闭程序";
    if (t->persistent) meta += L" · 已写入 Windows 任务计划程序";
    RECT rm = { x, nameY + S(24), L.rcList.right - S(380), nameY + S(24) + S(18) };
    DrawTextC(dc, meta, rm, g_fSmall, C_INK3,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 右侧元素
    RECT tg, rn2, ed, dl;
    RowElementRects(L, i, &tg, &rn2, &ed, &dl);

    // 下次执行 / 剩余时间
    RECT rnext = { tg.left - S(178), r.top, tg.left - S(14), r.bottom };
    if (t->persistent) {
        // 系统任务的执行者是 Windows，本程序不参与计时，所以这里不给倒计时。
        // 倒计时意味着"我们正在盯着它"，而实际上我们并没有。
        RECT r1 = { rnext.left, r.top + S(24), rnext.right, r.top + S(44) };
        DrawTextC(dc, L"系统任务 · 下次执行", r1, g_fSmall, C_INK3,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        std::wstring when;
        if (!t->enabled) {
            when = L"已停用";
        } else if (t->kind == TriggerKind::Daily || t->kind == TriggerKind::Weekly) {
            // 只有重复型任务才有"下一次"可言，推算出来的时刻是可靠的
            ULONGLONG now = NowTick(), due = 0, total = 0;
            when = ComputeDueTick(*t, now, due, total)
                 ? FormatAbsolute(due > now ? due - now : 0)
                 : L"待排期";
        } else {
            // 倒计时 / 指定时刻都是一次性的：注册完就已经交给系统了，
            // 再按"当前时间 + 延迟"算一次只会显示成永远差 30 分钟。
            when = L"一次性 · 已交给系统";
        }
        RECT r2 = { rnext.left, r.top + S(44), rnext.right, r.top + S(64) };
        DrawTextC(dc, when, r2, g_fBodyB, t->enabled ? C_INK2 : C_INK3,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (t->running) {
        ULONGLONG now = NowTick();
        ULONGLONG remain = (t->dueTick > now) ? (t->dueTick - now) : 0;
        RECT r1 = { rnext.left, r.top + S(20), rnext.right, r.top + S(42) };
        DrawTextC(dc, FormatCountdown(remain), r1, g_fMono, C_BRAND,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT r2 = { rnext.left, r.top + S(44), rnext.right, r.top + S(62) };
        DrawTextC(dc, FormatAbsolute(remain) + L" 执行", r2, g_fSmall, C_INK3,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (t->enabled) {
        // 「指定时刻」错过了它自己的日期时，不能显示成「待排期」——
        // 那会让用户以为它在等一个时刻，实际上它永远不会执行。
        bool expired = OnceAtExpired(*t);
        RECT r1 = { rnext.left, r.top + S(24), rnext.right, r.top + S(44) };
        DrawTextC(dc, expired ? L"已过期" : L"下次执行", r1, g_fSmall,
                  expired ? C_WARN : C_INK3,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT r2 = { rnext.left, r.top + S(44), rnext.right, r.top + S(64) };
        DrawTextC(dc, expired ? L"不会执行" : L"待排期", r2, g_fBodyB,
                  expired ? C_WARN : C_INK2,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT r1 = { rnext.left, r.top + S(24), rnext.right, r.top + S(44) };
        DrawTextC(dc, L"任务已停用", r1, g_fSmall, C_INK3,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawToggle(dc, tg, t->enabled);

    bool hov = (g_hoverRow == i);
    DrawIconBtn(dc, rn2, L"\uE768", L"跑", hov && g_hoverElem == ELEM_RUN, false);
    DrawIconBtn(dc, ed,  L"\uE70F", L"改", hov && g_hoverElem == ELEM_EDIT, false);
    DrawIconBtn(dc, dl,  L"\uE74D", L"删", hov && g_hoverElem == ELEM_DEL, true);
}

static void DrawEmptyState(HDC dc, const Layout& L) {
    RECT r = L.rcList;
    int cy = (r.top + r.bottom) / 2;

    RECT r1 = { r.left, cy - S(60), r.right, cy - S(30) };
    DrawTextC(dc, L"还没有任务", r1, g_fTitle, C_INK2,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT r2 = { r.left, cy - S(30), r.right, cy - S(4) };
    DrawTextC(dc, L"点右上角「新建任务」，或从托盘菜单快速设置一个倒计时关机",
              r2, g_fBody, C_INK3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawStatusBar(HDC dc, const Layout& L) {
    FillSolid(dc, L.rcStatus, RGB(0xFA,0xFB,0xFC));
    RECT line = { 0, L.rcStatus.top, L.rcStatus.right, L.rcStatus.top + 1 };
    FillSolid(dc, line, C_LINE);

    wchar_t buf[512];
    int n = (int)g_store.Tasks().size();
    int active = 0, sysN = 0;
    for (auto& t : g_store.Tasks()) {
        if (TaskActive(t)) active++;
        if (t.persistent) sysN++;
    }

    swprintf(buf, 512, L"共 %d 个任务 · 启用 %d 个 · 系统任务 %d 个 · 存储位置：%ls",
             n, active, sysN, g_store.Path().c_str());
    RECT rl = { S(22), L.rcStatus.top, L.rcStatus.right - S(220), L.rcStatus.bottom };
    DrawTextC(dc, buf, rl, g_fSmall, C_INK3,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const wchar_t* mode = g_store.Config().portableMode ? L"便携模式" : L"用户目录模式";
    RECT rr = { L.rcStatus.right - S(220), L.rcStatus.top, L.rcStatus.right - S(22), L.rcStatus.bottom };
    DrawTextC(dc, mode, rr, g_fSmall,
              g_store.Config().portableMode ? C_TEAL : C_WARN,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void PaintMain(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc0 = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    // 双缓冲，避免闪烁
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);

    Layout L = ComputeLayout(hwnd);

    FillSolid(dc, rc, C_BG);

    // 列表区裁剪
    HRGN clip = CreateRectRgn(L.rcList.left, L.rcList.top, L.rcList.right, L.rcList.bottom);
    SelectClipRgn(dc, clip);
    FillSolid(dc, L.rcList, C_BG);

    auto vis = VisibleTasks();
    if (vis.empty()) {
        SelectClipRgn(dc, nullptr);
        DrawEmptyState(dc, L);
    } else {
        for (size_t i = 0; i < vis.size(); ++i) {
            DrawTaskRow(dc, L, (int)i, vis[i], g_hoverRow == (int)i);
        }
        SelectClipRgn(dc, nullptr);
    }
    DeleteObject(clip);

    DrawHeader(dc, L);
    DrawFilterBar(dc, L);
    DrawChipsText(dc, L);
    DrawStatusBar(dc, L);

    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);

    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
//  引擎：排期 / 触发
// ---------------------------------------------------------------------------
static void ScheduleTask(PowerTask& t, bool force = false) {
    // 系统持久任务不进内存计时器——它的执行者是 Windows，不是我们。
    //
    // 一开始我是两边都排的，想着"多执行一次关机也无害"。实际不是：
    // 内存这条到点用的是 timeout=0（立刻执行），会把 shutdown.exe /t N
    // 好不容易争来的那几十秒宽限窗口当场掐掉，用户根本没机会看到
    // Windows 的倒计时对话框。更糟的是提醒窗上的「取消本次」会变成谎话——
    // 它取消了内存这条，系统那条照样会执行。
    //
    // 所以这里直接跳过，由任务计划程序全权负责；界面改用它推算出的
    // 「下次执行」时刻来展示，只当计划看，不当进度看。
    if (t.persistent) {
        t.running     = false;
        t.noticeFired = false;
        return;
    }

    // 已在计时且未要求重置 —— 保持原有进度，不要打断
    if (t.running && !force) return;

    if (!t.enabled) { t.running = false; return; }

    ULONGLONG now = NowTick();
    ULONGLONG due = 0, total = 0;
    if (ComputeDueTick(t, now, due, total)) {
        t.running     = true;
        t.dueTick     = due;
        t.totalTicks  = total;
        t.noticeFired = false;
    } else {
        t.running = false;
    }
}

void UI_RefreshAll(bool forceReschedule) {
    for (auto& t : g_store.Tasks()) ScheduleTask(t, forceReschedule);
    g_store.Save();
    if (g_hMain) {
        Layout L = ComputeLayout(g_hMain);
        InvalidateRect(g_hMain, &L.rcList, FALSE);
        InvalidateRect(g_hMain, &L.rcHeader, FALSE);
        InvalidateRect(g_hMain, &L.rcFilter, FALSE);
        InvalidateRect(g_hMain, &L.rcStatus, FALSE);
    }
}

bool UI_HasRunningTask() {
    for (auto& t : g_store.Tasks()) if (t.running) return true;
    return false;
}

void UI_CancelAllRunning() {
    for (auto& t : g_store.Tasks()) {
        if (!t.running) continue;
        t.running = false;
        if (t.kind == TriggerKind::Countdown) t.enabled = false;
        else ScheduleTask(t);
    }
    AbortShutdown();
    LogLine(L"用户取消了全部进行中的任务");
    UI_RefreshAll();
}

// 提醒窗 / 执行
static void ShowNoticeWindow(PowerTask* t, bool isNotice);
static void ExecuteTask(PowerTask* t);

void UI_Tick() {
    if (!g_hMain) return;

    ULONGLONG now = NowTick();
    bool needRepaint = false;

    for (auto& t : g_store.Tasks()) {
        if (!t.running) continue;

        if (t.dueTick <= now) {
            ExecuteTask(&t);
            needRepaint = true;
            continue;
        }

        ULONGLONG remain = t.dueTick - now;
        if (t.noticeSeconds > 0 && !t.noticeFired &&
            remain <= (ULONGLONG)t.noticeSeconds * 1000ULL) {
            t.noticeFired = true;
            ShowNoticeWindow(&t, true);
            LogLine(std::wstring(L"提醒：") + t.name);
        }
        needRepaint = true;
    }

    // 提醒窗倒计时刷新
    if (g_hNotice && IsWindowVisible(g_hNotice)) {
        InvalidateRect(g_hNotice, nullptr, FALSE);
    }

    if (needRepaint) {
        Layout L = ComputeLayout(g_hMain);
        InvalidateRect(g_hMain, &L.rcList, FALSE);
        InvalidateRect(g_hMain, &L.rcHeader, FALSE);
    }
}

static void ExecuteTask(PowerTask* t) {
    LogLine(std::wstring(L"执行：") + t->name + L" → " + ActionName(t->action));

    DWORD timeout = 0;   // 由我们自己的提醒窗负责倒计时
    bool ok = ExecuteAction(t->action, timeout, t->message, t->forceClose);

    wchar_t buf[256];
    swprintf(buf, 256, L"执行结果：%ls", ok ? L"成功" : L"失败");
    LogLine(buf);

    t->running = false;
    t->noticeFired = false;

    if (t->kind == TriggerKind::Countdown || t->kind == TriggerKind::OnceAt) {
        t->enabled = false;          // 一次性任务执行完自动停用
    } else {
        ScheduleTask(*t);            // 重复任务重算下次
    }
    g_store.Save();

    // 关闭提醒窗
    if (g_hNotice) { DestroyWindow(g_hNotice); g_hNotice = nullptr; }
}

// ---------------------------------------------------------------------------
//  托盘
// ---------------------------------------------------------------------------
static void TrayAdd(HWND hwnd) {
    if (g_trayAdded) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon            = g_icSmall;
    CopyStr(g_nid.szTip, 128, L"PlanPower · 定时电源管理");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = true;
}

static void TrayRemove() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

// 托盘气泡提示。
//
// 只在「开机静默启动」那一次用：用户看不到主界面，而 Win10 默认又把新出现
// 的托盘图标收进溢出区，不给一句提示的话，从用户视角看就是"什么都没发生"。
// 用局部副本加 NIF_INFO，避免把气泡标志永久留在 g_nid 上——留着的话之后
// 每次 NIM_MODIFY（比如刷新提示文字）都会再弹一次气泡。
static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags |= NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    CopyStr(nid.szInfoTitle, 64, title);
    CopyStr(nid.szInfo, 256, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static std::wstring g_lastTip;

static void TrayUpdateTip() {
    if (!g_trayAdded) return;
    int memActive = 0, sysActive = 0;
    ULONGLONG soonest = 0;
    bool has = false;
    ULONGLONG now = NowTick();
    for (auto& t : g_store.Tasks()) {
        if (!t.enabled) continue;          // 停用的不算「已交给 Windows」
        if (t.persistent) { sysActive++; continue; }
        if (!t.running) continue;
        memActive++;
        if (!has || t.dueTick < soonest) { soonest = t.dueTick; has = true; }
    }

    std::wstring s;
    if (memActive == 0 && sysActive == 0) {
        s = L"PlanPower · 暂无启用的任务";
    } else if (memActive == 0) {
        wchar_t b[64];
        swprintf(b, 64, L"PlanPower · %d 个系统任务已交给 Windows", sysActive);
        s = b;
    } else if (has && soonest > now) {
        s = L"PlanPower · " + FormatCountdown(soonest - now) + L" 后执行";
    } else {
        s = L"PlanPower · 任务即将执行";
    }

    if (s == g_lastTip) return;      // 内容没变就不惊动外壳
    g_lastTip = s;
    CopyStr(g_nid.szTip, 128, s.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// 退出前确认：内存计时任务只活在进程里，进程一没就永远不会执行。
//
// 不提示的话，用户点「退出」时以为关机还在等着，实际什么都没发生——
// 这是本程序里唯一一个「操作成功、结果却相反」的地方，必须拦一道。
static bool ConfirmExitWithRunningTasks(HWND hwnd) {
    std::vector<PowerTask*> live;
    for (auto& t : g_store.Tasks())
        if (t.running) live.push_back(&t);
    if (live.empty()) return true;

    ULONGLONG now = NowTick();
    std::wstring q = L"以下任务正在计时，退出程序后不会执行：\n\n";
    int shown = 0;
    for (auto* t : live) {
        if (shown++ >= 6) { q += L"  ……\n"; break; }
        ULONGLONG remain = (t->dueTick > now) ? (t->dueTick - now) : 0;
        q += L"  · " + t->name + L"（" + FormatCountdown(remain) + L" 后）\n";
    }
    q += L"\n要让它们在关掉界面后仍然执行，请改用「系统持久任务」。";

    return MessageBoxW(hwnd, q.c_str(), L"PlanPower · 还有任务在计时",
                       MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

// 快速创建一条倒计时关机任务
static void QuickCountdown(int minutes) {
    // 已经有一条同样时长的倒计时关机在跑时，重置它的计时，而不是再建一条。
    // 连点两次「30 分钟后关机」的直觉是「从现在重新数 30 分钟」，
    // 而不是留下两条任务、到点各执行一次。
    for (auto& t : g_store.Tasks()) {
        if (t.kind != TriggerKind::Countdown) continue;
        if (t.action != PowerAction::Shutdown) continue;
        if (t.persistent || !t.enabled) continue;
        if (t.delaySeconds != minutes * 60) continue;

        ScheduleTask(t, true);
        g_store.Save();
        UI_RefreshAll();
        TrayUpdateTip();
        LogLine(std::wstring(L"快捷重置：") + t.name);
        return;
    }

    PowerTask t;
    t.id            = NewGuid();
    t.action        = PowerAction::Shutdown;
    t.kind          = TriggerKind::Countdown;
    t.delaySeconds  = minutes * 60;
    t.noticeSeconds = minutes >= 2 ? 60 : 15;
    t.message       = L"系统即将关机，请保存工作";

    wchar_t name[64];
    swprintf(name, 64, L"%d 分钟后关机", minutes);
    t.name = name;

    g_store.AddTask(t);
    UI_RefreshAll();
    TrayUpdateTip();
    LogLine(std::wstring(L"快捷创建：") + t.name);
}

// 全部启用 / 全部暂停。
//
// 系统任务必须**合成一次提权**：逐条走 SysSetEnabled 会连弹 N 次 UAC，
// 而用户点的是一个「全部」按钮，不是 N 个。
static void SetAllEnabled(HWND hwnd, bool on) {
    bool anySys = false;
    int  changed = 0;
    for (auto& t : g_store.Tasks()) {
        if (t.persistent) anySys = true;
        if (t.enabled == on) continue;
        t.enabled = on;
        if (!t.persistent) {
            if (on) ScheduleTask(t, true);
            else    t.running = false;
        }
        changed++;
    }
    if (changed == 0) return;

    if (anySys) {
        std::wstring err;
        if (!SchedRunElevated(SchedOp::SetAllEnabled, g_store.Path(), on, &err)) {
            MessageBoxW(hwnd,
                (L"系统任务状态没能同步：\n\n" + err +
                 L"\n\n本地任务已经改好了，但任务计划程序里的那几条可能仍是旧状态。").c_str(),
                L"PlanPower · 系统任务未同步", MB_ICONWARNING);
        }
    }

    g_store.Save();
    UI_RefreshAll();
    TrayUpdateTip();
    LogLine(on ? L"全部启用" : L"全部暂停");
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    // 三个档位来自设置（默认 30 / 60 / 120），不再写死。
    for (int i = 0; i < 3; ++i) {
        wchar_t b[64];
        swprintf(b, 64, L"%d 分钟后关机", g_store.Config().quickMinutes[i]);
        AppendMenuW(m, MF_STRING, 2001 + i, b);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    UINT cancelFlags = UI_HasRunningTask() ? MF_STRING : (MF_STRING | MF_GRAYED);
    AppendMenuW(m, cancelFlags, 2004, L"取消全部进行中的任务");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2009, L"暂停全部任务");
    AppendMenuW(m, MF_STRING, 2010, L"启用全部任务");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2005, L"立即睡眠");
    AppendMenuW(m, MF_STRING, 2006, L"锁定工作站");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2011, L"执行历史…");
    AppendMenuW(m, MF_STRING, 2007, L"打开主界面");
    AppendMenuW(m, MF_STRING, 2008, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------
//  任务编辑窗
// ---------------------------------------------------------------------------
#define ED_W_LOGICAL 480          // 窗口逻辑宽度
#define ED_PAD       24           // 左右内边距
#define ED_ROW_GAP   58           // 一行「标题 + 输入框」占用的高度
#define ED_EDIT_H    26           // 输入框高度
#define ED_BOX_W     72           // 数字输入框宽度
#define ED_UNIT_W    40           // 单位标签宽度

#define IDC_NAME     1001
#define IDC_ACTION   1002
#define IDC_TRIGGER  1003
#define IDC_HOURS    1004
#define IDC_MINS     1005
#define IDC_AT_H     1006
#define IDC_AT_M     1007
#define IDC_NOTICE   1008
#define IDC_FORCE    1009
// 行标题与单位标签（需要随触发方式显隐、随参数区重排，因此必须带 ID）
#define IDC_LBL_DELAY 1010     // "延迟时长（小时 / 分钟）"
#define IDC_LBL_AT    1011     // "执行时刻（24 小时制）"
#define IDC_U_HOUR    1012     // "小时"
#define IDC_U_MIN     1013     // "分钟"
#define IDC_U_AT_H    1014     // "时"
#define IDC_U_AT_M    1015     // "分"
#define IDC_MESSAGE  1017
#define IDC_LBL_NOTICE 1018    // "提前提醒（秒，0 = 不提醒）"
#define IDC_LBL_MSG   1019     // "关机提示文本（可选）"
#define IDC_WD_BASE  1020      // 1020..1026 = 周一..周日
#define IDC_MODE_MEM  1027     // "内存计时"
#define IDC_MODE_SYS  1028     // "系统持久任务"
#define IDC_MODE_HINT 1029     // 执行方式下方的说明文字
#define IDC_LBL_MODE  1030     // "执行方式" 行标题
// 指定时刻的日期行（年 / 月 / 日）。只在这一种触发方式下显示。
#define IDC_LBL_DATE  1031     // "执行日期"
#define IDC_DATE_Y    1032
#define IDC_DATE_M    1033
#define IDC_DATE_D    1034
#define IDC_U_DATE_Y  1035     // "年"
#define IDC_U_DATE_M  1036     // "月"
#define IDC_U_DATE_D  1037     // "日"
#define IDC_OK       1
#define IDC_CANCEL   2

static std::wstring g_editTaskId;   // 空 = 新建

static HWND E(int id) { return GetDlgItem(g_hEditor, id); }

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::vector<wchar_t> buf(n + 2, 0);
    GetWindowTextW(h, buf.data(), n + 1);
    return buf.data();
}

static int GetInt(HWND h, int def) {
    std::wstring s = GetText(h);
    if (s.empty()) return def;
    int v = _wtoi(s.c_str());
    return v;
}

// ---------------------------------------------------------------------------
//  系统任务的三个同步动作
//
//  全部走 SchedRunElevated —— 也就是说每一次都会弹一次 UAC。这是有意为之：
//  用户在「正在往系统里写东西」这件事上应该始终有感知。刻意没有做
//  「一次提权、全程免打扰」，那会让后台悄悄多出几条开机就执行的任务。
//
//  失败时 *err 里是人话，直接可以塞进 MessageBox。
// ---------------------------------------------------------------------------
static std::wstring SysNameOf(const PowerTask& t) {
    // 归属表是权威来源：任务改过名之后，只有表里还记着旧名字。
    // 表里没有（首次注册，或归属表被清掉了）才按当前名字现算。
    std::wstring n = g_store.HostTaskName(MachineFingerprint(), t.id);
    if (n.empty()) n = SchedTaskLeafName(t);
    return n;
}

static bool SysRegister(const PowerTask& t, std::wstring* err) {
    if (!SchedRunElevated(SchedOp::Add, t.id, true, err)) return false;
    g_store.SetHostTask(MachineFingerprint(), t.id, SchedTaskLeafName(t));
    return true;
}

static bool SysUnregister(const PowerTask& t, std::wstring* err) {
    if (!SchedRunElevated(SchedOp::Delete, SysNameOf(t), false, err)) return false;
    g_store.ClearHostTask(MachineFingerprint(), t.id);
    return true;
}

static bool SysSetEnabled(const PowerTask& t, bool on, std::wstring* err) {
    return SchedRunElevated(SchedOp::SetEnabled, SysNameOf(t), on, err);
}

// ---------------------------------------------------------------------------
//  按当前触发方式重排参数区（含窗口高度自适应）。
//  定义在 MakeEditorControls 之后，此处先声明。
// ---------------------------------------------------------------------------
static void LayoutEditor(HWND h, int kind);
static int  EditorClientHeight(int kind);

// ---------------------------------------------------------------------------
//  「执行方式」行的状态刷新
//
//  只改文字与可用性，不动位置——所以它可以被 EditorUpdateVisibility 反复调用
//  而不会和 LayoutEditor 抢布局。
//
//  三种不可用情形按优先级显示，一次只说一条，避免用户同时读到三段解释：
//    ① 全局闸门关闭   ② 当前动作不支持   ③ 正常
// ---------------------------------------------------------------------------
static void EditorRefreshMode() {
    if (!g_hEditor) return;

    HWND hMem  = E(IDC_MODE_MEM);
    HWND hSys  = E(IDC_MODE_SYS);
    HWND hHint = E(IDC_MODE_HINT);
    if (!hMem || !hSys || !hHint) return;

    int act = (int)SendMessageW(E(IDC_ACTION), CB_GETCURSEL, 0, 0);
    if (act < 0) act = 0;
    PowerAction a = (PowerAction)act;

    const bool gateOn    = g_store.Config().allowSystemTasks;
    const bool actionOk  = ActionSupportsPersistent(a);
    const bool canSys    = gateOn && actionOk;

    // 闸门关着、或动作不支持时，把选择强制拉回内存计时。
    // 强制而不是保留用户的勾选：留着会在保存时被静默改掉，那才是真正的意外。
    bool sysChecked = (SendMessageW(hSys, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!canSys && sysChecked) {
        SendMessageW(hSys, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(hMem, BM_SETCHECK, BST_CHECKED, 0);
        sysChecked = false;
    }

    EnableWindow(hSys, canSys ? TRUE : FALSE);
    EnableWindow(hMem, TRUE);

    std::wstring hint;
    if (!gateOn) {
        hint = L"系统持久任务已在「设置」中关闭。关闭状态下只使用内存计时，"
               L"不会向本机写入任何东西。";
    } else if (!actionOk) {
        hint = std::wstring(PersistentUnsupportedReason(a)) + L"这项只能用内存计时。";
    } else if (sysChecked) {
        hint = L"写入 Windows 任务计划程序，由系统调度：PlanPower 关掉、U 盘拔掉、"
               L"没人登录也会执行。本程序不再参与计时，保存时需授权一次。";
    } else {
        hint = L"仅在 PlanPower 运行期间有效，退出程序后不再执行。"
               L"不想让程序常驻时可改用系统持久任务。";
    }
    SetWindowTextW(hHint, hint.c_str());

    // 「提前提醒」这一项在两种模式下含义完全不同，标签必须跟着变：
    //  · 内存计时：到点前 N 秒弹本程序自己的提醒窗；
    //  · 系统持久任务：PlanPower 不在场，只能把它塞进 shutdown 的 /t N，
    //    变成 Windows 官方的关机倒计时宽限。而且只有关机/重启认这个参数。
    if (sysChecked) {
        bool hasT = (a == PowerAction::Shutdown || a == PowerAction::Restart);
        SetWindowTextW(E(IDC_LBL_NOTICE), hasT
            ? L"倒计时宽限（秒，0 = 立即执行）"
            : L"提前提醒（秒，此动作的系统任务不支持）");
    } else {
        SetWindowTextW(E(IDC_LBL_NOTICE), L"提前提醒（秒，0 = 不提醒）");
    }
}

static void EditorUpdateVisibility() {
    if (!g_hEditor) return;
    int kind = (int)SendMessageW(E(IDC_TRIGGER), CB_GETCURSEL, 0, 0);
    if (kind < 0) kind = 0;

    // 触发方式决定客户区高度：指定时刻多一行日期，每周多一行星期。
    // 不跟着改窗口大小的话，切到更高的那一种会把底部按钮挤出客户区——
    // 用户看到的现象是「保存按钮不见了」，而不是「窗口该变高」。
    RECT rc = {};
    GetWindowRect(g_hEditor, &rc);
    RECT adj = { 0, 0, S(ED_W_LOGICAL), EditorClientHeight(kind) };
    AdjustWindowRectEx(&adj, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                       FALSE, WS_EX_DLGMODALFRAME);
    SetWindowPos(g_hEditor, nullptr, rc.left, rc.top,
                 adj.right - adj.left, adj.bottom - adj.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // 显隐 + 重排必须一起做：只显隐会让单位标签悬空、输入框错位
    LayoutEditor(g_hEditor, kind);
    EditorRefreshMode();
    InvalidateRect(g_hEditor, nullptr, TRUE);
}

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDC_TRIGGER && code == CBN_SELCHANGE) {
            EditorUpdateVisibility();
            return 0;
        }
        if (id == IDC_ACTION && code == CBN_SELCHANGE) {
            // 动作决定「系统持久任务」是否可选（关闭显示器没有命令行入口）
            EditorRefreshMode();
            return 0;
        }
        if (id == IDC_MODE_MEM || id == IDC_MODE_SYS) {
            // 显式互斥，不依赖 BS_AUTORADIOBUTTON 的分组推断——那套规则要靠
            // 控件创建顺序与 WS_GROUP 边界来推，日后有人在两者之间插入一个
            // 控件就会静默失效，症状是「两个都能选中」。
            bool mem = (SendMessageW(E(IDC_MODE_MEM), BM_GETCHECK, 0, 0) == BST_CHECKED);
            bool sys = (SendMessageW(E(IDC_MODE_SYS), BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (mem && sys) {
                HWND other = (id == IDC_MODE_MEM) ? E(IDC_MODE_SYS) : E(IDC_MODE_MEM);
                SendMessageW(other, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            EditorRefreshMode();
            return 0;
        }
        if (id == IDC_OK) {
            // ---- 收集并校验 ----
            std::wstring name = GetText(E(IDC_NAME));
            if (name.empty()) {
                MessageBoxW(hwnd, L"请填写任务名称。", L"PlanPower", MB_ICONINFORMATION);
                SetFocus(E(IDC_NAME));
                return 0;
            }
            PowerTask t;
            bool wasPersistent = false;
            std::wstring oldSysName;
            if (!g_editTaskId.empty()) {
                PowerTask* old = g_store.FindById(g_editTaskId);
                if (old) {
                    t = *old;
                    wasPersistent = old->persistent;
                    // 必须在覆盖 t.name 之前取。系统任务名里含任务名称，
                    // 所以改名等于换了一条系统任务，旧的那条要单独删掉。
                    if (wasPersistent) oldSysName = SchedTaskLeafName(*old);
                }
            }
            if (t.id.empty()) t.id = NewGuid();

            t.name    = name;
            t.action  = (PowerAction)SendMessageW(E(IDC_ACTION), CB_GETCURSEL, 0, 0);
            t.kind    = (TriggerKind)SendMessageW(E(IDC_TRIGGER), CB_GETCURSEL, 0, 0);
            if (g_editTaskId.empty()) t.enabled = true;   // 新建默认启用；编辑保留原状态

            // 同名任务在列表里长得一模一样，而「编辑 / 删除」是按行点的——
            // 两条都叫「晚上关机」时，用户没有第二个线索能分辨。
            // 不禁止（有人确实需要两条同名但不同时间的），只提醒一次。
            for (auto& other : g_store.Tasks()) {
                if (other.id == t.id) continue;
                if (other.name != name) continue;
                std::wstring q = L"已经有一条叫「" + name + L"」的任务。\n\n"
                                 L"两条同名任务在列表里无法区分，仍要保存吗？";
                if (MessageBoxW(hwnd, q.c_str(), L"PlanPower · 任务重名",
                                MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                    return 0;
                break;
            }

            if (t.kind == TriggerKind::Countdown) {
                int h = GetInt(E(IDC_HOURS), 0);
                int m = GetInt(E(IDC_MINS), 30);
                if (h < 0) h = 0; if (m < 0) m = 0;
                t.delaySeconds = h * 3600 + m * 60;
                if (t.delaySeconds <= 0) t.delaySeconds = 60;
            } else {
                int hh = GetInt(E(IDC_AT_H), 23);
                int mm = GetInt(E(IDC_AT_M), 0);
                if (hh < 0 || hh > 23) hh = 23;
                if (mm < 0 || mm > 59) mm = 0;
                t.hour = hh;
                t.minute = mm;

                if (t.kind == TriggerKind::OnceAt) {
                    int yy = GetInt(E(IDC_DATE_Y), 0);
                    int mo = GetInt(E(IDC_DATE_M), 0);
                    int dd = GetInt(E(IDC_DATE_D), 0);
                    // 走 ppdate::ValidDate，不在这里重写一遍「年 1601–9999、
                    // 日不超过当月天数」——那一份判断已经在 dateutil.h 里，
                    // 两处各写一份的话，某天改了下限就会出现「编辑窗放行、
                    // 写 XML 时拒绝」这种前后不一致。
                    if (!ppdate::ValidDate(yy, mo, dd)) {
                        MessageBoxW(hwnd,
                            L"执行日期不是一个真实存在的日期，请检查年、月、日。",
                            L"PlanPower · 日期不合法", MB_ICONWARNING);
                        SetFocus(E(IDC_DATE_Y));
                        return 0;
                    }

                    // 存一个已经过去的时刻没有任何意义：它永远不会执行，
                    // 界面上只会写「已过期」。不如当场拦下来。
                    SYSTEMTIME now = {}, due = {};
                    FILETIME fNow = {}, fDue = {};
                    GetLocalTime(&now);
                    due.wYear   = (WORD)yy;
                    due.wMonth  = (WORD)mo;
                    due.wDay    = (WORD)dd;
                    due.wHour   = (WORD)hh;
                    due.wMinute = (WORD)mm;
                    if (SystemTimeToFileTime(&now, &fNow) &&
                        SystemTimeToFileTime(&due, &fDue) &&
                        CompareFileTime(&fDue, &fNow) <= 0) {
                        MessageBoxW(hwnd,
                            L"这个日期和时刻已经过去了，任务不会执行。\n\n"
                            L"请选一个将来的时间。",
                            L"PlanPower · 时间已过", MB_ICONWARNING);
                        SetFocus(E(IDC_DATE_Y));
                        return 0;
                    }

                    t.year = yy; t.month = mo; t.day = dd;
                }

                if (t.kind == TriggerKind::Weekly) {
                    unsigned mask = 0;
                    for (int i = 0; i < 7; ++i) {
                        if (SendMessageW(E(IDC_WD_BASE + i), BM_GETCHECK, 0, 0) == BST_CHECKED)
                            mask |= (1u << i);
                    }
                    if (mask == 0) {
                        MessageBoxW(hwnd, L"请至少选择一个星期几。", L"PlanPower", MB_ICONINFORMATION);
                        return 0;
                    }
                    t.weekdayMask = mask;
                }
            }

            t.noticeSeconds = GetInt(E(IDC_NOTICE), 60);
            if (t.noticeSeconds < 0) t.noticeSeconds = 0;
            if (t.noticeSeconds > 3600) t.noticeSeconds = 3600;

            t.forceClose = (SendMessageW(E(IDC_FORCE), BM_GETCHECK, 0, 0) == BST_CHECKED);
            t.message    = GetText(E(IDC_MESSAGE));

            // ---- 执行方式 ----
            // 界面上已经把不可用的组合禁掉了，这里再算一次是因为它是唯一
            // 决定"要不要往系统里写"的地方，不能只靠界面状态。
            bool wantSys = (SendMessageW(E(IDC_MODE_SYS), BM_GETCHECK, 0, 0) == BST_CHECKED);
            bool canSys  = g_store.Config().allowSystemTasks &&
                           ActionSupportsPersistent(t.action);
            t.persistent = wantSys && canSys;

            // ---- 第一步：把"从系统里摘掉"这类操作做完 ----
            // 刻意放在写盘之前，是为了让"降级"这一步保持原子性。如果用户
            // 在 UAC 弹窗上点了取消，就整体中止保存、什么都不改；否则会留下
            // 一条本地写着「内存计时」、系统里却还在按时执行的任务——那是
            // 最糟糕的一种状态，因为界面上再也看不到它、也就管不着它了。
            if (wasPersistent && !t.persistent) {
                std::wstring err;
                if (!SysUnregister(t, &err)) {
                    MessageBoxW(hwnd,
                        (L"没能把这条任务从 Windows 任务计划程序里移除，"
                         L"所以这次保存没有生效，任务保持原样。\n\n原因：" + err +
                         L"\n\n可以稍后重试，或者到「设置」里用"
                         L"「清理本机系统任务」一次性清干净。").c_str(),
                        L"PlanPower · 保存已取消", MB_ICONWARNING);
                    return 0;                  // 编辑窗留着，让用户能改回去
                }
                wasPersistent = false;         // 系统侧已经摘干净了
            }

            // ---- 第二步：落盘 ----
            // 提权助手进程要按 ID 从 INI 里读这条任务，所以必须先写文件，
            // 再启动提权进程。顺序反了助手会报「配置里找不到这个任务」。
            if (g_editTaskId.empty()) {
                g_store.AddTask(t);
            } else {
                PowerTask* dst = g_store.FindById(g_editTaskId);
                if (dst) *dst = t;
            }
            g_store.Save();

            // ---- 第三步：注册 / 更新系统任务 ----
            // 这一步成功之后，ScheduleTask 会把这条任务从内存计时器里摘出去
            // （见 ScheduleTask 开头的早退），执行权完全交给 Windows。
            if (t.persistent) {
                std::wstring err;
                if (!SysRegister(t, &err)) {
                    // 注册失败就降级成内存计时：任务本身不该因为提权没成功
                    // 而丢掉，用户辛苦填的参数要保住。
                    t.persistent = false;
                    PowerTask* dst = g_store.FindById(t.id);
                    if (dst) dst->persistent = false;
                    g_store.Save();
                    MessageBoxW(hwnd,
                        (L"这条任务没能注册成系统任务，已按「内存计时」保存。\n\n"
                         L"原因：" + err +
                         L"\n\n本地任务已经保存好了，随时可以再编辑重试。").c_str(),
                        L"PlanPower · 系统任务未注册", MB_ICONWARNING);
                } else if (wasPersistent && oldSysName != SchedTaskLeafName(t)) {
                    // 改了名字 = 换了一条系统任务。新的已经注册成功，再删旧的；
                    // 删失败只提示残留，不回滚——宁可按新名字跑起来，
                    // 也不要为了删一条旧任务把刚建好的新任务撤掉。
                    std::wstring e2;
                    if (!SchedRunElevated(SchedOp::Delete, oldSysName, false, &e2)) {
                        MessageBoxW(hwnd,
                            (L"新的系统任务已经注册好了，但旧名字的那条没能删掉。\n\n"
                             L"原因：" + e2 +
                             L"\n\n它可能仍会在后台执行。可以到「设置」里用"
                             L"「清理本机系统任务」清干净。").c_str(),
                            L"PlanPower · 旧系统任务残留", MB_ICONWARNING);
                    }
                }
            }

            UI_RefreshAll(true);
            TrayUpdateTip();
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_CANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hEditor = nullptr;
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  编辑窗控件与布局
//
//  三处必须一起处理的排版问题：
//   1) 「延迟时长」与「执行时刻」两行参数互斥，只显示当前触发方式适用的那一行；
//   2) 单位标签（小时/分钟/时/分）必须紧跟各自的输入框。若把两个标签统一排在
//      两个输入框之后，界面会显示成 "0    30    小时   分钟"，语义完全错位；
//   3) 显隐与重排必须同时进行。只做 ShowWindow 而不重排，隐藏输入框后单位
//      标签会留在原地悬空，下方的行也不会补位。
// ---------------------------------------------------------------------------
static int EditorParamsTop() { return S(204); }   // 参数区起点（"触发方式"之后）
static int EditorModeRowH()  { return S(92); }    // 「执行方式」行占用的高度
static int EditorModeMemW()  { return S(148); }   // 单选按钮宽度
static int EditorModeSysW()  { return S(168); }
static int EditorModeHintH() { return S(38); }    // 说明文字（最多两行）

static int EditorClientHeight(int kind) {
    int h = EditorParamsTop() + S(ED_ROW_GAP);    // 参数行（四选一，等高）
    if (kind == 1) h += S(ED_ROW_GAP);            // 日期行（仅「指定时刻」）
    if (kind == 3) h += S(36);                    // 星期行
    h += EditorModeRowH();                        // 执行方式
    h += S(58) + S(34) + S(58);                   // 提醒 / 强制关闭 / 提示文本
    h += S(32) + S(24);                           // 按钮 + 底部边距
    return h;
}

static void MakeEditorControls(HWND h) {
    const int x = S(ED_PAD);
    const int w = S(ED_W_LOGICAL - ED_PAD * 2);
    HFONT f = g_fBody;

    // 普通行标题 / 输入框：一律 WS_VISIBLE 创建，位置固定
    auto label = [&](int id, const wchar_t* s, int yy) {
        HWND c = CreateWindowExW(0, L"STATIC", s, WS_CHILD | WS_VISIBLE,
                                 x, yy, w, S(20), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
        return c;
    };
    auto edit = [&](int id, const wchar_t* init, int yy, int ww) {
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", init,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 x, yy, ww, S(ED_EDIT_H), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    // 数字输入框：位置由 LayoutEditor 统一排。
    // 一律带 WS_VISIBLE 创建，不适用的行由 LayoutEditor 显式隐藏——
    // 万一漏改，症状是"多出一个字段"（一眼可见），而不是"输入框凭空消失"。
    auto numEdit = [&](int id, const wchar_t* init) {
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", init,
                                 WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                 x, 0, S(ED_BOX_W), S(ED_EDIT_H), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    // 单位标签：SS_CENTERIMAGE + 与输入框等高，保证与输入框垂直居中对齐
    auto unit = [&](int id, const wchar_t* s) {
        HWND c = CreateWindowExW(0, L"STATIC", s,
                                 WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT,
                                 x, 0, S(ED_UNIT_W), S(ED_EDIT_H), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
        return c;
    };

    int y = S(18);

    // ---- 任务名称 ----
    label(0, L"任务名称", y);  y += S(22);
    edit(IDC_NAME, L"", y, w); y += S(40);

    // ---- 执行动作 ----
    label(0, L"执行动作", y);  y += S(22);
    {
        HWND cbA = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, S(200), h, (HMENU)IDC_ACTION, g_hInst, nullptr);
        SendMessageW(cbA, WM_SETFONT, (WPARAM)f, TRUE);
        for (int i = 0; i <= (int)PowerAction::MonitorOff; ++i) {
            std::wstring s = std::wstring(ActionName((PowerAction)i)) + L"　—　" + ActionHint((PowerAction)i);
            SendMessageW(cbA, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        }
        SendMessageW(cbA, CB_SETCURSEL, 0, 0);
    }
    y += S(40);

    // ---- 触发方式 ----
    label(0, L"触发方式", y);  y += S(22);
    {
        HWND cbT = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, S(200), h, (HMENU)IDC_TRIGGER, g_hInst, nullptr);
        SendMessageW(cbT, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(cbT, CB_ADDSTRING, 0, (LPARAM)L"倒计时　—　N 小时 N 分钟后执行");
        SendMessageW(cbT, CB_ADDSTRING, 0, (LPARAM)L"指定时刻　—　今天的某个时间点，已过则顺延");
        SendMessageW(cbT, CB_ADDSTRING, 0, (LPARAM)L"每天　—　每日固定时间");
        SendMessageW(cbT, CB_ADDSTRING, 0, (LPARAM)L"每周　—　选定星期几的固定时间");
        SendMessageW(cbT, CB_SETCURSEL, 0, 0);
    }
    y += S(40);
    // 此处 y == EditorParamsTop()

    // ---- 参数区：两行互斥，位置与显隐全部交给 LayoutEditor ----
    label(IDC_LBL_DELAY, L"延迟时长（小时 / 分钟）", y);
    numEdit(IDC_HOURS, L"0");
    numEdit(IDC_MINS,  L"30");
    unit(IDC_U_HOUR, L"小时");
    unit(IDC_U_MIN,  L"分钟");

    label(IDC_LBL_AT, L"执行时刻（24 小时制）", y);
    numEdit(IDC_AT_H, L"23");
    numEdit(IDC_AT_M, L"00");
    unit(IDC_U_AT_H, L"时");
    unit(IDC_U_AT_M, L"分");

    // ---- 执行日期（仅「指定时刻」）----
    // 用三个数字框而不是 DateTimePicker：与「小时 / 分钟」完全同一种交互，
    // 布局算术也能在这里算准（DTP 的高度由系统决定，窗口高度会跟着变）。
    label(IDC_LBL_DATE, L"执行日期（年 / 月 / 日）", y);
    {
        // 默认今天。新建「指定时刻」时用户多半想的是「今天/明天几点」，
        // 让日期框停在某个写死的年份上等于逼他先改一遍。
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        wchar_t yb[8], mb[8], db[8];
        swprintf(yb, 8, L"%04d", now.wYear);
        swprintf(mb, 8, L"%02d", now.wMonth);
        swprintf(db, 8, L"%02d", now.wDay);
        numEdit(IDC_DATE_Y, yb);
        numEdit(IDC_DATE_M, mb);
        numEdit(IDC_DATE_D, db);
    }
    unit(IDC_U_DATE_Y, L"年");
    unit(IDC_U_DATE_M, L"月");
    unit(IDC_U_DATE_D, L"日");

    // ---- 星期 ----
    {
        static const wchar_t* wd[7] = { L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日" };
        for (int i = 0; i < 7; ++i) {
            HWND c = CreateWindowExW(0, L"BUTTON", wd[i],
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, 0, S(58), S(24), h, (HMENU)(INT_PTR)(IDC_WD_BASE + i), g_hInst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
            if (i < 5) SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
        }
    }

    // ---- 执行方式（内存计时 / 系统持久任务）----
    // 两个单选按钮用 WS_GROUP 划成一组。但真正的互斥不依赖它的自动行为——
    // 见 EditorRefreshMode：每次点击都显式把另一个置为未选中。理由是在
    // 纯 CreateWindow 出来的普通窗口里，自动单选的分组规则要靠控件创建顺序
    // 与样式推断，一旦以后有人在两者之间插入控件就会静默失效。
    label(IDC_LBL_MODE, L"执行方式", y);
    {
        HWND m1 = CreateWindowExW(0, L"BUTTON", L"内存计时",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
            x, 0, S(148), S(22), h, (HMENU)IDC_MODE_MEM, g_hInst, nullptr);
        HWND m2 = CreateWindowExW(0, L"BUTTON", L"系统持久任务",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP,
            x, 0, S(168), S(22), h, (HMENU)IDC_MODE_SYS, g_hInst, nullptr);
        SendMessageW(m1, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(m2, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(m1, BM_SETCHECK, BST_CHECKED, 0);
    }
    {
        // 说明文字：内容随「全局闸门 / 动作 / 当前选择」变化，由 EditorRefreshMode 填
        HWND c = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, 0, w, S(38), h, (HMENU)IDC_MODE_HINT, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    }

    // ---- 提醒秒数 ----
    label(IDC_LBL_NOTICE, L"提前提醒（秒，0 = 不提醒）", y);
    numEdit(IDC_NOTICE, L"60");

    // ---- 强制关闭 ----
    {
        HWND c = CreateWindowExW(0, L"BUTTON", L"强制关闭仍在运行的程序（会丢失未保存内容）",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, 0, w, S(24), h, (HMENU)IDC_FORCE, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    }

    // ---- 提示文本 ----
    label(IDC_LBL_MSG, L"关机提示文本（可选）", y);
    edit(IDC_MESSAGE, L"系统即将关机，请保存工作", y, w);

    // ---- 按钮 ----
    {
        HWND ok = CreateWindowExW(0, L"BUTTON", L"保存任务",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            x, 0, S(96), S(32), h, (HMENU)IDC_OK, g_hInst, nullptr);
        HWND cc = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE,
            x, 0, S(96), S(32), h, (HMENU)IDC_CANCEL, g_hInst, nullptr);
        SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessageW(cc, WM_SETFONT, (WPARAM)f, TRUE);
    }
}

// 按触发方式显隐 + 重排参数区，并自适应窗口高度
static void LayoutEditor(HWND h, int kind) {
    if (!h) return;

    const int x     = S(ED_PAD);
    const int w     = S(ED_W_LOGICAL - ED_PAD * 2);
    const int boxW  = S(ED_BOX_W);
    const int unitW = S(ED_UNIT_W);
    const int eh    = S(ED_EDIT_H);

    const bool showDelay = (kind == 0);
    const bool showAt    = (kind >= 1);
    const bool showDate  = (kind == 1);
    const bool showWd    = (kind == 3);

    auto C = [&](int id) { return GetDlgItem(h, id); };
    auto put = [](HWND c, int cx, int cy, int cw, int ch) {
        if (c) SetWindowPos(c, nullptr, cx, cy, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto vis = [](HWND c, bool on) {
        if (c) ShowWindow(c, on ? SW_SHOW : SW_HIDE);
    };

    int y = EditorParamsTop();

    // ---- 可见性：全部控件集中在此决定 ----
    // 控件一律带 WS_VISIBLE 创建，这里显式隐藏当前触发方式不适用的行。
    // 集中列出而非按行分散，是为了能一眼看全"谁该显示"——之前 IDC_NOTICE
    // 因为创建时不可见又没被任何一处显示，导致"提前提醒"整行消失。
    vis(C(IDC_LBL_DELAY), showDelay);
    vis(C(IDC_HOURS),     showDelay);
    vis(C(IDC_MINS),      showDelay);
    vis(C(IDC_U_HOUR),    showDelay);
    vis(C(IDC_U_MIN),     showDelay);

    vis(C(IDC_LBL_AT),    showAt);
    vis(C(IDC_AT_H),      showAt);
    vis(C(IDC_AT_M),      showAt);
    vis(C(IDC_U_AT_H),    showAt);
    vis(C(IDC_U_AT_M),    showAt);

    vis(C(IDC_LBL_DATE),  showDate);
    vis(C(IDC_DATE_Y),    showDate);
    vis(C(IDC_DATE_M),    showDate);
    vis(C(IDC_DATE_D),    showDate);
    vis(C(IDC_U_DATE_Y),  showDate);
    vis(C(IDC_U_DATE_M),  showDate);
    vis(C(IDC_U_DATE_D),  showDate);

    for (int i = 0; i < 7; ++i) vis(C(IDC_WD_BASE + i), showWd);

    // 常显控件：显式再确认一次，避免日后改动走漏
    vis(C(IDC_NAME),       true);
    vis(C(IDC_ACTION),     true);
    vis(C(IDC_TRIGGER),    true);
    vis(C(IDC_LBL_MODE),   true);
    vis(C(IDC_MODE_MEM),   true);
    vis(C(IDC_MODE_SYS),   true);
    vis(C(IDC_MODE_HINT),  true);
    vis(C(IDC_LBL_NOTICE), true);
    vis(C(IDC_NOTICE),     true);
    vis(C(IDC_FORCE),      true);
    vis(C(IDC_LBL_MSG),    true);
    vis(C(IDC_MESSAGE),    true);
    vis(C(IDC_OK),         true);
    vis(C(IDC_CANCEL),     true);

    // ---- 行 1：延迟时长（仅「倒计时」）----
    if (showDelay) {
        int by = y + S(22);
        int bx = x;
        put(C(IDC_HOURS),  bx, by, boxW,  eh);  bx += boxW  + S(8);
        put(C(IDC_U_HOUR), bx, by, unitW, eh);  bx += unitW + S(16);
        put(C(IDC_MINS),   bx, by, boxW,  eh);  bx += boxW  + S(8);
        put(C(IDC_U_MIN),  bx, by, unitW, eh);
        put(C(IDC_LBL_DELAY), x, y, w, S(20));
        y += S(ED_ROW_GAP);
    }

    // ---- 行 1.5：执行日期（仅「指定时刻」）----
    if (showDate) {
        int by = y + S(22);
        int bx = x;
        put(C(IDC_DATE_Y),   bx, by, S(88), eh);  bx += S(88) + S(8);
        put(C(IDC_U_DATE_Y), bx, by, S(28), eh);  bx += S(28) + S(16);
        put(C(IDC_DATE_M),   bx, by, S(56), eh);  bx += S(56) + S(8);
        put(C(IDC_U_DATE_M), bx, by, S(28), eh);  bx += S(28) + S(16);
        put(C(IDC_DATE_D),   bx, by, S(56), eh);  bx += S(56) + S(8);
        put(C(IDC_U_DATE_D), bx, by, S(28), eh);
        put(C(IDC_LBL_DATE), x, y, w, S(20));
        y += S(ED_ROW_GAP);
    }

    // ---- 行 2：执行时刻（指定时刻 / 每天 / 每周）----
    if (showAt) {
        int by = y + S(22);
        int bx = x;
        put(C(IDC_AT_H),   bx, by, boxW,  eh);  bx += boxW  + S(8);
        put(C(IDC_U_AT_H), bx, by, unitW, eh);  bx += unitW + S(16);
        put(C(IDC_AT_M),   bx, by, boxW,  eh);  bx += boxW  + S(8);
        put(C(IDC_U_AT_M), bx, by, unitW, eh);
        put(C(IDC_LBL_AT), x, y, w, S(20));
        y += S(ED_ROW_GAP);
    }

    // ---- 行 3：星期（仅「每周」）----
    if (showWd) {
        int bx = x;
        for (int i = 0; i < 7; ++i) {
            put(C(IDC_WD_BASE + i), bx, y, S(58), S(24));
            bx += S(60);
        }
        y += S(36);
    }

    // ---- 行 4：执行方式（两个单选按钮 + 一行说明）----
    {
        put(C(IDC_LBL_MODE), x, y, w, S(20));
        int ry = y + S(24);
        put(C(IDC_MODE_MEM), x, ry, EditorModeMemW(), S(22));
        put(C(IDC_MODE_SYS), x + EditorModeMemW() + S(8), ry,
            EditorModeSysW(), S(22));
        // 说明文字紧贴单选按钮下方。高度固定为两行——高度随内容变会让整个
        // 窗口在切换单选按钮时上下跳动，比多留一点空白更烦人。
        put(C(IDC_MODE_HINT), x, ry + S(26), w, EditorModeHintH());
        y += EditorModeRowH();
    }

    // ---- 提醒秒数 ----
    put(C(IDC_LBL_NOTICE), x, y, w, S(20));
    put(C(IDC_NOTICE), x, y + S(22), boxW, eh);
    y += S(58);

    // ---- 强制关闭 ----
    put(C(IDC_FORCE), x, y, w, S(24));
    y += S(34);

    // ---- 提示文本 ----
    put(C(IDC_LBL_MSG), x, y, w, S(20));
    put(C(IDC_MESSAGE), x, y + S(22), w, eh);
    y += S(58);

    // ---- 按钮 ----
    put(C(IDC_OK),     x + w - S(200), y, S(96), S(32));
    put(C(IDC_CANCEL), x + w - S(96),  y, S(96), S(32));
    y += S(32);

    // ---- 窗口高度自适应（避免隐藏一行后底部留下大片空白）----
    RECT wr = {}, cr = {};
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    int ncH  = (wr.bottom - wr.top) - (cr.bottom - cr.top);   // 标题栏 + 边框
    int newW = S(ED_W_LOGICAL);
    int newH = y + S(24) + ncH;
    if ((wr.right - wr.left) != newW || (wr.bottom - wr.top) != newH) {
        SetWindowPos(h, nullptr, 0, 0, newW, newH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void ShowEditor(HWND owner, PowerTask* existing) {
    if (g_hEditor) { SetForegroundWindow(g_hEditor); return; }

    g_editTaskId = existing ? existing->id : L"";

    // 按触发方式算出实际客户区高度，再换算成窗口外框尺寸，保证一次居中到位
    int kind = existing ? (int)existing->kind : 0;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    RECT adj = { 0, 0, S(ED_W_LOGICAL), EditorClientHeight(kind) };
    AdjustWindowRectEx(&adj, style, FALSE, WS_EX_DLGMODALFRAME);
    int W = adj.right - adj.left;
    int H = adj.bottom - adj.top;

    RECT ro;
    GetWindowRect(owner, &ro);
    int x = ro.left + ((ro.right - ro.left) - W) / 2;
    int y = ro.top + ((ro.bottom - ro.top) - H) / 2;

    g_hEditor = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PPEditor",
        existing ? L"编辑任务 — PlanPower" : L"新建任务 — PlanPower",
        style, x, y, W, H, owner, nullptr, g_hInst, nullptr);
    if (!g_hEditor) return;

    MakeEditorControls(g_hEditor);

    if (existing) {
        SetWindowTextW(E(IDC_NAME), existing->name.c_str());
        SendMessageW(E(IDC_ACTION), CB_SETCURSEL, (WPARAM)existing->action, 0);
        SendMessageW(E(IDC_TRIGGER), CB_SETCURSEL, (WPARAM)existing->kind, 0);

        wchar_t b[16];
        swprintf(b, 16, L"%d", existing->delaySeconds / 3600);
        SetWindowTextW(E(IDC_HOURS), b);
        swprintf(b, 16, L"%d", (existing->delaySeconds % 3600) / 60);
        SetWindowTextW(E(IDC_MINS), b);

        swprintf(b, 16, L"%d", existing->hour);
        SetWindowTextW(E(IDC_AT_H), b);
        swprintf(b, 16, L"%02d", existing->minute);
        SetWindowTextW(E(IDC_AT_M), b);

        // 指定时刻的日期。旧任务没有日期（year==0），按它当时的规则具体化成
        // 「今天」或「明天」——用户在界面上看到的是一个确切日期，而不是一个
        // 他自己也说不清的「下一次到这个点」。
        if (existing->kind == TriggerKind::OnceAt) {
            int yy = existing->year, mo = existing->month, dd = existing->day;
            if (yy <= 0) {
                SYSTEMTIME now = {};
                GetLocalTime(&now);
                bool passed = (existing->hour * 60 + existing->minute) <=
                              (now.wHour * 60 + now.wMinute);
                LocalDatePlusDays(passed ? 1 : 0, &yy, &mo, &dd);
            }
            swprintf(b, 16, L"%04d", yy); SetWindowTextW(E(IDC_DATE_Y), b);
            swprintf(b, 16, L"%02d", mo); SetWindowTextW(E(IDC_DATE_M), b);
            swprintf(b, 16, L"%02d", dd); SetWindowTextW(E(IDC_DATE_D), b);
        }

        for (int i = 0; i < 7; ++i) {
            SendMessageW(E(IDC_WD_BASE + i), BM_SETCHECK,
                (existing->weekdayMask & (1u << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        swprintf(b, 16, L"%d", existing->noticeSeconds);
        SetWindowTextW(E(IDC_NOTICE), b);
        SendMessageW(E(IDC_FORCE), BM_SETCHECK,
            existing->forceClose ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(E(IDC_MESSAGE), existing->message.c_str());

        // 执行方式。只在真的注册过时才回填「系统持久任务」——如果用户后来
        // 关掉了全局开关，这条任务仍是持久任务，但界面会显示成内存计时，
        // 保存时就会把它降级。这是有意的：开关关掉之后，不应该再有新的
        // 提权弹窗从编辑窗里冒出来。
        bool sys = existing->persistent && g_store.Config().allowSystemTasks &&
                   ActionSupportsPersistent(existing->action);
        SendMessageW(E(IDC_MODE_MEM), BM_SETCHECK,
            sys ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(E(IDC_MODE_SYS), BM_SETCHECK,
            sys ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    EditorUpdateVisibility();
    ShowWindow(g_hEditor, SW_SHOW);
    SetForegroundWindow(g_hEditor);
}

// ---------------------------------------------------------------------------
//  提醒窗
// ---------------------------------------------------------------------------
#define IDC_NOTICE_CANCEL 3001
#define IDC_NOTICE_NOW    3002

static LRESULT CALLBACK NoticeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_NOTICE_CANCEL) {
            PowerTask* t = g_store.FindById(g_noticeTaskId);
            if (t) {
                t->noticeFired = false;
                if (t->kind == TriggerKind::Countdown || t->kind == TriggerKind::OnceAt) {
                    t->running = false;
                    t->enabled = false;
                    LogLine(std::wstring(L"用户取消了：") + t->name);
                } else {
                    // 重复任务：跳过本次，顺延到下一个周期
                    ScheduleTask(*t);
                    LogLine(std::wstring(L"跳过本次：") + t->name);
                }
            }
            AbortShutdown();
            g_store.Save();
            UI_RefreshAll();
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_NOTICE_NOW) {
            PowerTask* t = g_store.FindById(g_noticeTaskId);
            if (t) { t->dueTick = NowTick(); }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        // 点关闭 = 取消本次
        SendMessageW(hwnd, WM_COMMAND, IDC_NOTICE_CANCEL, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        FillSolid(dc, rc, C_CARD);

        PowerTask* t = g_store.FindById(g_noticeTaskId);
        ULONGLONG now = NowTick();
        ULONGLONG remain = (t && t->dueTick > now) ? (t->dueTick - now) : 0;
        ULONGLONG total = (t && t->totalTicks > 0) ? t->totalTicks : 1;

        // 标题
        RECT r1 = { S(22), S(18), rc.right - S(22), S(42) };
        DrawTextC(dc, L"即将执行", r1, g_fSmall, C_INK3, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 大号倒计时
        RECT r2 = { S(22), S(40), rc.right - S(22), S(92) };
        DrawTextC(dc, FormatCountdown(remain), r2, g_fBig, C_BRAND,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 任务名
        wchar_t line[256];
        swprintf(line, 256, L"「%ls」将在 %llu 秒后%ls",
                 t ? t->name.c_str() : L"任务",
                 (unsigned long long)(remain / 1000),
                 t ? ActionName(t->action) : L"执行");
        RECT r3 = { S(22), S(96), rc.right - S(22), S(120) };
        DrawTextC(dc, line, r3, g_fSmall, C_INK2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 进度条
        RECT rb = { S(22), S(126), rc.right - S(22), S(132) };
        FillSolid(dc, rb, RGB(0xE8,0xED,0xF4));
        ULONGLONG done = (total > remain) ? (total - remain) : 0;
        int pw = (int)((double)done / (double)total * (rb.right - rb.left));
        if (pw < 2) pw = 2;
        RECT rf = { rb.left, rb.top, rb.left + pw, rb.bottom };
        FillSolid(dc, rf, C_BRAND);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_hNotice = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowNoticeWindow(PowerTask* t, bool /*isNotice*/) {
    g_noticeTaskId = t->id;

    if (g_hNotice) { SetForegroundWindow(g_hNotice); return; }

    int W = S(400), H = S(196);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - W - S(20);
    int y = wa.bottom - H - S(20);

    g_hNotice = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"PPNotice", L"",
        WS_POPUP | WS_BORDER,
        x, y, W, H, nullptr, nullptr, g_hInst, nullptr);
    if (!g_hNotice) return;

    CreateWindowExW(0, L"BUTTON", L"取消本次",
        WS_CHILD | WS_VISIBLE, S(22), S(148), S(110), S(32),
        g_hNotice, (HMENU)IDC_NOTICE_CANCEL, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"立即执行",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, S(140), S(148), S(110), S(32),
        g_hNotice, (HMENU)IDC_NOTICE_NOW, g_hInst, nullptr);

    for (int id : { IDC_NOTICE_CANCEL, IDC_NOTICE_NOW }) {
        SendMessageW(GetDlgItem(g_hNotice, id), WM_SETFONT, (WPARAM)g_fBody, TRUE);
    }

    ShowWindow(g_hNotice, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hNotice, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE);
}

// ===========================================================================
//  设置窗
//
//  设计口径（见《PlanPower_系统级持久任务设计》第一道闸门）：
//  「系统级持久任务」的总开关默认关闭。首次开启时必须先看一遍好处和代价，
//  并且要主动勾选「我已了解」才允许点「启用」——不能一路回车跳过。
// ===========================================================================
#define IDC_ST_AUTOSTART 5001
#define IDC_ST_LOG       5002
#define IDC_ST_ALLOW     5003
#define IDC_ST_CLEAN     5004
#define IDC_ST_OPEN      5005
#define IDC_ST_CLOSE     5006
// 5007 = 「开机启动时只显示托盘图标」复选框
#define IDC_ST_TRAY      5007
#define IDC_ST_SEC1      5010
#define IDC_ST_SEC2      5011
#define IDC_ST_SEC3      5012
#define IDC_ST_SEC4      5013
#define IDC_ST_HINT1     5020
#define IDC_ST_HINT2     5021
#define IDC_ST_HINT3     5022
#define IDC_ST_HINT4     5023
#define IDC_ST_SYSINFO   5024
#define IDC_ST_QUICK     5025   // 托盘快捷关机的三个时长（"30,60,120"）
#define IDC_ST_QLBL      5026   // 上面那个输入框的标题
#define IDC_ST_KEEP      5027   // 日志保留月数
#define IDC_ST_KLBL      5028   // 上面那个输入框的标题

#define ST_W_LOGICAL 560
#define ST_PAD       24

static std::wstring ExeFullPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

// 开机自启：写 HKCU 的 Run 项。
// 这是整个程序里唯一会碰注册表的地方，而且只在用户显式勾选时才发生。
// 之所以不做成"往启动文件夹放快捷方式"，是因为那会真的在磁盘上留下文件，
// 而注册表值删起来是干净的、可逆的。

// 自启动时要写进 Run 项的命令行。
//
// /tray 是"开机启动别弹界面"的**唯一信号**：它由自启动这一条命令行带过来，
// 手动双击 exe 永远不会有它。这样"这次该不该显示窗口"是显式传进来的，
// 不需要去猜（父进程是谁、启动耗时多少、有没有登录脚本……都猜不准）。
static std::wstring AutostartCommand() {
    // 带上 /portable：即使 exe 放在只读介质上，也强制配置写在同目录，
    // 避免"开机自启的那一份"和"手动点开的那一份"读到不同的配置。
    std::wstring cmd = L"\"" + ExeFullPath() + L"\" /portable";
    if (g_store.Config().trayOnAutostart) cmd += L" /tray";
    return cmd;
}

static bool ApplyAutostart(bool on, std::wstring* err) {
    HKEY k = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (st != ERROR_SUCCESS) {
        if (err) *err = L"无法打开当前用户的启动项注册表键，可能是权限受限。";
        return false;
    }

    if (on) {
        std::wstring cmd = AutostartCommand();
        st = RegSetValueExW(k, L"PlanPower", 0, REG_SZ,
                            (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        if (st != ERROR_SUCCESS && err) *err = L"写入启动项失败。";
    } else {
        st = RegDeleteValueW(k, L"PlanPower");
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
        if (st != ERROR_SUCCESS && err) *err = L"删除启动项失败。";
    }
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

// 读当前 Run 项的值。不存在（或读不出来）返回 false。
static bool ReadAutostartValue(std::wstring* out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;

    wchar_t buf[1024] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type  = 0;
    LSTATUS st = RegQueryValueExW(k, L"PlanPower", nullptr, &type,
                                  (LPBYTE)buf, &bytes);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || type != REG_SZ) return false;
    *out = buf;
    return true;
}

// 启动时校正 Run 项。
//
// 存在的理由：v0.2.2 之前写进去的启动项没有 /tray，光升级 exe 不会让
// "开机不弹窗"生效——用户还得手动把开关关掉再打开一次。这里在启动时
// 把它对齐到当前配置。
//
// 两条边界，都是刻意的：
//   · 值被删掉了就**不复活**。用户可能在任务管理器的"启动"页里禁用了它，
//     程序自己加回去是最招人烦的行为之一。
//   · 值指向别的副本就**不抢**。同一台机器上可能放着两支 U 盘，各自都开着
//     自启，抢同一个值名没有意义，谁也别改谁。
void UI_RepairAutostart() {
    if (!g_store.Config().startWithWindows) return;

    std::wstring cur;
    if (!ReadAutostartValue(&cur)) return;

    if (cur.find(ExeFullPath()) == std::wstring::npos) return;
    if (cur == AutostartCommand()) return;

    std::wstring err;
    ApplyAutostart(true, &err);
}

// ---------------------------------------------------------------------------
//  「启用系统级持久任务」确认窗
// ---------------------------------------------------------------------------
#define IDC_CFM_CHECK  5101
#define IDC_CFM_OK     5102
#define IDC_CFM_CANCEL 5103

static bool g_confirmOk = false;
static RECT g_cfmBody = {};          // 正文绘制区（在 WM_PAINT 里用）

static const wchar_t* kConfirmBody =
    L"开启后，新建任务时可以选择把任务交给 Windows 自己调度。\n"
    L"\n"
    L"好处\n"
    L"    · PlanPower 关掉、U 盘拔掉、甚至没人登录，任务照样按时执行\n"
    L"    · 由 Windows 的任务计划程序负责，不受 PlanPower 崩溃或误关影响\n"
    L"    · 任务名统一带 PlanPower 前缀，集中放在 \\PlanPower 文件夹里\n"
    L"\n"
    L"代价\n"
    L"    · 会在本机留下系统任务，不再「退出即无残留」\n"
    L"    · 任务由 %SystemRoot%\\System32\\shutdown.exe 执行，而不是 PlanPower，\n"
    L"      所以提前提醒会换成 Windows 自己的关机倒计时对话框\n"
    L"    · 每次创建 / 删除 / 启停这类任务，都会弹一次管理员授权\n"
    L"    · 关闭这个开关不会自动删掉已注册的任务，需要单独点「清理本机系统任务」\n"
    L"\n"
    L"这些任务任何时候都能一键清干净：设置里的「清理本机系统任务」，\n"
    L"或者在任意副本上执行  PlanPower.exe /cleanup-all  。";

static int MeasureTextHeight(HDC dc, HFONT f, const std::wstring& s, int w) {
    HGDIOBJ of = SelectObject(dc, f);
    RECT r = { 0, 0, w, 0 };
    DrawTextW(dc, s.c_str(), (int)s.size(), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, of);
    return r.bottom - r.top;
}

static LRESULT CALLBACK ConfirmProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_CFM_CHECK) {
            // 「启用」按钮在勾选之前一直是灰的——这是设计里明确要求的一步，
            // 目的就是让用户没法闭着眼回车把这个开关打开。
            bool on = (SendMessageW(GetDlgItem(hwnd, IDC_CFM_CHECK),
                                     BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_CFM_OK), on ? TRUE : FALSE);
            return 0;
        }
        if (id == IDC_CFM_OK)     { g_confirmOk = true;  DestroyWindow(hwnd); return 0; }
        // IDCANCEL 是 IsDialogMessageW 在按 Esc 时发过来的，专门接一下：
        // 我们的按钮 ID 不是标准的 IDCANCEL，光靠系统转发接不到。
        if (id == IDC_CFM_CANCEL || id == IDCANCEL) {
            g_confirmOk = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillSolid(dc, rc, C_CARD);

        RECT rt = { S(24), S(16), rc.right - S(24), S(16) + S(30) };
        DrawTextC(dc, L"启用系统级持久任务", rt, g_fTitle, C_INK,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        DrawTextC(dc, kConfirmBody, g_cfmBody, g_fBody, C_INK2,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, C_CARD);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CLOSE:
        g_confirmOk = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hConfirm = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 弹确认窗；用户点了「启用」返回 true
static bool ConfirmEnableSystemTasks(HWND owner) {
    if (g_hConfirm) { SetForegroundWindow(g_hConfirm); return false; }
    g_confirmOk = false;

    const int W = S(560);
    const int padX = S(24);
    const int bodyW = W - padX * 2;

    // 正文高度按实际排版量出来，而不是估一个数——中文在不同 DPI 下的
    // 折行位置不一样，写死高度在 150% 缩放下会把最后几行切掉。
    HDC dc = GetDC(owner ? owner : nullptr);
    int bodyH = MeasureTextHeight(dc, g_fBody, kConfirmBody, bodyW);
    ReleaseDC(owner ? owner : nullptr, dc);

    int titleH = S(30);
    int cbH    = S(24);
    int btnH   = S(32);

    int bodyTop = S(16) + titleH + S(10);
    g_cfmBody = { padX, bodyTop, padX + bodyW, bodyTop + bodyH };
    int cbTop  = bodyTop + bodyH + S(16);
    int btnTop = cbTop + cbH + S(14);
    int clientH = btnTop + btnH + S(18);

    // 刻意不带 WS_VISIBLE：先把控件建好、摆好、填好内容再一次性显示出来。
    // 带 WS_VISIBLE 的话会先闪一个空白框，而后面 RefreshSettings 里那次
    // SchedEnum 要连任务计划程序服务，肉眼能看出停顿。
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT adj = { 0, 0, W, clientH };
    AdjustWindowRectEx(&adj, style, FALSE, WS_EX_DLGMODALFRAME);
    int WW = adj.right - adj.left, HH = adj.bottom - adj.top;

    RECT ro = {};
    if (owner) GetWindowRect(owner, &ro);
    else       SystemParametersInfoW(SPI_GETWORKAREA, 0, &ro, 0);
    int x = ro.left + ((ro.right - ro.left) - WW) / 2;
    int y = ro.top  + ((ro.bottom - ro.top) - HH) / 2;

    g_hConfirm = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PPConfirm",
        L"启用系统级持久任务 — PlanPower", style,
        x, y, WW, HH, owner, nullptr, g_hInst, nullptr);
    if (!g_hConfirm) return false;

    HWND cb = CreateWindowExW(0, L"BUTTON", L"我已了解以上内容，并确认启用",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        padX, cbTop, bodyW, cbH, g_hConfirm, (HMENU)IDC_CFM_CHECK, g_hInst, nullptr);
    SendMessageW(cb, WM_SETFONT, (WPARAM)g_fBody, TRUE);

    HWND ok = CreateWindowExW(0, L"BUTTON", L"启用",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
        W - padX - S(96), btnTop, S(96), btnH,
        g_hConfirm, (HMENU)IDC_CFM_OK, g_hInst, nullptr);
    HWND no = CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        W - padX - S(96) * 2 - S(8), btnTop, S(96), btnH,
        g_hConfirm, (HMENU)IDC_CFM_CANCEL, g_hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)g_fBody, TRUE);
    SendMessageW(no, WM_SETFONT, (WPARAM)g_fBody, TRUE);

    ShowWindow(g_hConfirm, SW_SHOW);
    SetForegroundWindow(g_hConfirm);

    // 把设置窗冻住：否则用户能在确认窗开着的时候再去点那个勾选框，
    // 走一遍完全并行的分支，最后两个窗口的状态互相打架。
    if (owner) EnableWindow(owner, FALSE);

    // 模态：只在本窗口和它的子控件之间转消息。
    // 用 IsDialogMessageW 是为了拿到 Tab 键导航和回车触发默认按钮。
    MSG m;
    while (g_hConfirm && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hConfirm, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return g_confirmOk;
}

// ---------------------------------------------------------------------------
//  设置窗本体
// ---------------------------------------------------------------------------
static void RefreshSettings(HWND h) {
    if (!h) return;
    auto C = [&](int id) { return GetDlgItem(h, id); };
    auto chk = [&](int id, bool on) {
        SendMessageW(C(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    const AppConfig& c = g_store.Config();

    chk(IDC_ST_AUTOSTART, c.startWithWindows);
    chk(IDC_ST_TRAY,      c.trayOnAutostart);
    chk(IDC_ST_LOG,       c.keepLog);
    {
        wchar_t qb[64];
        swprintf(qb, 64, L"%d,%d,%d",
                 c.quickMinutes[0], c.quickMinutes[1], c.quickMinutes[2]);
        SetWindowTextW(C(IDC_ST_QUICK), qb);
    }
    {
        wchar_t kb[16];
        swprintf(kb, 16, L"%d", c.logKeepMonths);
        SetWindowTextW(C(IDC_ST_KEEP), kb);
    }
    chk(IDC_ST_ALLOW,     c.allowSystemTasks);

    // 没开自启时，「只显示托盘图标」这一项无从谈起——置灰而不是隐藏，
    // 这样用户仍看得见它的存在和当前取值，不会以为程序偷偷记了别的状态。
    EnableWindow(C(IDC_ST_TRAY), c.startWithWindows ? TRUE : FALSE);

    // 系统任务那一段的说明文字随开关变化。开着的时候要特别点明"关掉开关
    // 不会自动删任务"——这是最容易让人以为已经清干净的一句话。
    if (c.allowSystemTasks) {
        SetWindowTextW(C(IDC_ST_HINT3),
            L"已开启。新建或编辑任务时可以选择「系统持久任务」——那类任务由 "
            L"Windows 自己调度，PlanPower 没运行也会执行。\n"
            L"代价是每次写入都要过一次管理员授权。关闭本开关不会删除已经注册的"
            L"系统任务，需要点下面的「清理本机系统任务」。");
    } else {
        SetWindowTextW(C(IDC_ST_HINT3),
            L"默认关闭。关闭状态下程序不会往这台电脑里写任何东西，"
            L"所有任务都只在 PlanPower 运行期间有效。\n"
            L"开启后可以创建「系统持久任务」，代价是会留下痕迹、且每次写入都要"
            L"过一次管理员授权。");
    }

    // 本机实际注册数量：以任务计划程序为准，不是数我们自己的标记。
    // 两者不一致（比如别处删了任务）时，这里显示的是真实值。
    std::wstring info;
    std::vector<SchedEntry> list;
    std::wstring err;
    if (SchedEnum(&list, &err)) {
        wchar_t b[128];
        swprintf(b, 128, L"本机当前注册了 %d 个 PlanPower 系统任务。", (int)list.size());
        info = b;
    } else {
        info = L"无法读取本机任务计划程序：" + err;
    }
    SetWindowTextW(C(IDC_ST_SYSINFO), info.c_str());

    std::wstring pathLine = L"配置与任务：" + g_store.Path();
    std::wstring modeLine = c.portableMode
        ? L"便携模式 —— 配置写在 exe 同目录，换台电脑只带走这个文件。"
        : L"用户目录模式 —— exe 所在目录不可写，配置改存到本机用户目录。";
    SetWindowTextW(C(IDC_ST_HINT4), (pathLine + L"\r\n" + modeLine).c_str());
}

static int SettingsLayout(HWND h) {
    const int x = S(ST_PAD);
    const int w = S(ST_W_LOGICAL - ST_PAD * 2);
    auto C = [&](int id) { return GetDlgItem(h, id); };
    auto put = [](HWND c, int cx, int cy, int cw, int ch) {
        if (c) SetWindowPos(c, nullptr, cx, cy, cw, ch,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    };

    int y = S(14);
    // ---- 启动 ----
    put(C(IDC_ST_SEC1),   x, y, w, S(18)); y += S(24);
    put(C(IDC_ST_AUTOSTART), x, y, w, S(22)); y += S(24);
    put(C(IDC_ST_TRAY),   x, y, w, S(22)); y += S(24);
    // 说明文字要讲三件事（写在哪、只对本机、手动双击仍有界面），
    // 小字号下正好三行——给 S(34) 会把最后一行裁掉。
    put(C(IDC_ST_HINT1),  x, y, w, S(50)); y += S(58);

    // ---- 运行 ----
    put(C(IDC_ST_SEC2),   x, y, w, S(18)); y += S(24);
    put(C(IDC_ST_LOG),    x, y, w, S(22)); y += S(24);
    put(C(IDC_ST_KLBL),   x, y, w, S(20)); y += S(22);
    put(C(IDC_ST_KEEP),   x, y, S(80), S(26)); y += S(34);
    put(C(IDC_ST_HINT2),  x, y, w, S(34)); y += S(42);
    put(C(IDC_ST_QLBL),   x, y, w, S(20)); y += S(22);
    put(C(IDC_ST_QUICK),  x, y, S(200), S(26)); y += S(34);
    // ---- 系统级持久任务 ----
    put(C(IDC_ST_SEC3),   x, y, w, S(18)); y += S(24);
    put(C(IDC_ST_ALLOW),  x, y, w, S(22)); y += S(24);
    put(C(IDC_ST_HINT3),  x, y, w, S(50)); y += S(58);
    put(C(IDC_ST_CLEAN),  x, y, S(148), S(30));
    put(C(IDC_ST_SYSINFO), x + S(158), y + S(6), w - S(158), S(18));
    y += S(40);

    // ---- 存储 ----
    put(C(IDC_ST_SEC4),   x, y, w, S(18)); y += S(24);
    put(C(IDC_ST_HINT4),  x, y, w, S(38)); y += S(46);
    put(C(IDC_ST_OPEN),   x, y, S(148), S(30)); y += S(40);

    // ---- 关闭 ----
    put(C(IDC_ST_CLOSE),  x + w - S(96), y, S(96), S(32));
    y += S(32);

    return y + S(18);
}

static void MakeSettingsControls(HWND h) {
    const int x = S(ST_PAD);
    const int w = S(ST_W_LOGICAL - ST_PAD * 2);

    auto sec = [&](int id, const wchar_t* s) {
        HWND c = CreateWindowExW(0, L"STATIC", s, WS_CHILD | WS_VISIBLE,
                                 x, 0, w, S(18), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmallB, TRUE);
    };
    auto hint = [&](int id) {
        HWND c = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 x, 0, w, S(34), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    };
    auto check = [&](int id, const wchar_t* s) {
        HWND c = CreateWindowExW(0, L"BUTTON", s,
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                                 x, 0, w, S(22), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fBody, TRUE);
    };
    auto button = [&](int id, const wchar_t* s) {
        HWND c = CreateWindowExW(0, L"BUTTON", s, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 x, 0, S(148), S(30), h, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    };

    sec(IDC_ST_SEC1, L"启动");
    check(IDC_ST_AUTOSTART, L"开机时自动启动 PlanPower");
    check(IDC_ST_TRAY, L"开机启动时只显示托盘图标，不弹出主界面");
    hint(IDC_ST_HINT1);
    SetWindowTextW(GetDlgItem(h, IDC_ST_HINT1),
        L"勾选后会在当前用户的启动项里写一条记录（注册表 Run 项），"
        L"只对这台电脑生效，不随 U 盘走；取消勾选会把它删掉。"
        L"手动双击运行永远会打开主界面。");

    sec(IDC_ST_SEC2, L"运行");
    check(IDC_ST_LOG, L"记录执行日志");

    {
        HWND lb = CreateWindowExW(0, L"STATIC", L"日志保留月数（0 = 不自动清理）",
            WS_CHILD | WS_VISIBLE, x, 0, w, S(20), h,
            (HMENU)(INT_PTR)IDC_ST_KLBL, g_hInst, nullptr);
        SendMessageW(lb, WM_SETFONT, (WPARAM)g_fSmall, TRUE);

        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
            x, 0, S(80), S(26), h,
            (HMENU)(INT_PTR)IDC_ST_KEEP, g_hInst, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_fBody, TRUE);
    }

    hint(IDC_ST_HINT2);
    SetWindowTextW(GetDlgItem(h, IDC_ST_HINT2),
        L"把任务的创建、执行、取消记到 exe 同目录的 logs\\ 文件夹，按月分文件。"
        L"「执行历史」里能看最近的记录。只在需要排查问题时才建议打开。");

    // 托盘快捷关机的三个时长。一个输入框写三个数，比三行输入框省地方，
    // 也比写死三个档位灵活——「临时关一下」的常用档位因人而异。
    {
        HWND lb = CreateWindowExW(0, L"STATIC", L"托盘快捷关机时长（分钟，逗号分隔）",
            WS_CHILD | WS_VISIBLE, x, 0, w, S(20), h,
            (HMENU)(INT_PTR)IDC_ST_QLBL, g_hInst, nullptr);
        SendMessageW(lb, WM_SETFONT, (WPARAM)g_fSmall, TRUE);

        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            x, 0, S(200), S(26), h,
            (HMENU)(INT_PTR)IDC_ST_QUICK, g_hInst, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_fBody, TRUE);
    }

    sec(IDC_ST_SEC3, L"系统级持久任务");
    check(IDC_ST_ALLOW, L"允许创建系统持久任务（写入 Windows 任务计划程序）");
    hint(IDC_ST_HINT3);
    button(IDC_ST_CLEAN, L"清理本机系统任务");
    {
        HWND c = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 0, S(18), h, (HMENU)IDC_ST_SYSINFO, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fSmall, TRUE);
    }

    sec(IDC_ST_SEC4, L"存储");
    hint(IDC_ST_HINT4);
    button(IDC_ST_OPEN, L"打开所在文件夹");
    {
        HWND c = CreateWindowExW(0, L"BUTTON", L"关闭",
                                 WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                 0, 0, S(96), S(32), h, (HMENU)IDC_ST_CLOSE, g_hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fBody, TRUE);
    }
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        HWND c = GetDlgItem(hwnd, id);
        auto checked = [&]() {
            return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
        };

        // IDCANCEL：Esc 键经 IsDialogMessageW 转发过来
        if (id == IDC_ST_CLOSE || id == IDCANCEL) { DestroyWindow(hwnd); return 0; }

        // 快捷关机时长：失焦时才解析，边打边存会把「30,6」这种中间状态也写进去。
        if (id == IDC_ST_QUICK && HIWORD(wp) == EN_KILLFOCUS) {
            wchar_t buf[64] = {};
            GetWindowTextW(c, buf, 64);
            int v[3] = { 0, 0, 0 };
            if (swscanf(buf, L"%d,%d,%d", &v[0], &v[1], &v[2]) != 3) {
                MessageBoxW(hwnd,
                    L"请按「30,60,120」的格式填写三个分钟数。",
                    L"PlanPower · 格式不对", MB_ICONWARNING);
                RefreshSettings(hwnd);
                return 0;
            }
            for (int i = 0; i < 3; ++i) {
                if (v[i] < 1)        v[i] = 1;
                if (v[i] > 24 * 60)  v[i] = 24 * 60;
                g_store.Config().quickMinutes[i] = v[i];
            }
            g_store.Save();
            RefreshSettings(hwnd);      // 把夹取后的值写回输入框
            return 0;
        }

        // 日志保留月数：同样失焦才解析。清空输入框不算错——直接当 0（不自动清理），
        // 而不是弹一个「格式不对」再把它还原成旧值：用户清空往往就是想关掉清理。
        if (id == IDC_ST_KEEP && HIWORD(wp) == EN_KILLFOCUS) {
            wchar_t buf[16] = {};
            GetWindowTextW(c, buf, 16);
            int v = 0;
            if (buf[0] && swscanf(buf, L"%d", &v) != 1) {
                MessageBoxW(hwnd,
                    L"请填一个 0 到 120 之间的整数（0 表示不自动清理）。",
                    L"PlanPower · 格式不对", MB_ICONWARNING);
                RefreshSettings(hwnd);
                return 0;
            }
            if (v < 0)   v = 0;
            if (v > 120) v = 120;
            g_store.Config().logKeepMonths = v;
            g_store.Save();
            RefreshSettings(hwnd);      // 把夹取后的值写回输入框
            return 0;
        }

        if (id == IDC_ST_AUTOSTART) {            bool on = checked();
            std::wstring err;
            if (!ApplyAutostart(on, &err)) {
                MessageBoxW(hwnd, err.c_str(), L"PlanPower", MB_ICONWARNING);
                SendMessageW(c, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
                return 0;
            }
            g_store.Config().startWithWindows = on;
            g_store.Save();
            // 托盘那一项的有效性跟着自启开关走
            EnableWindow(GetDlgItem(hwnd, IDC_ST_TRAY), on ? TRUE : FALSE);
            return 0;
        }

        if (id == IDC_ST_TRAY) {
            // 改的是命令行里有没有 /tray，所以必须**立刻重写 Run 项**——
            // 只存配置不重写的话，下次开机仍然是旧命令行，开关看起来没生效。
            g_store.Config().trayOnAutostart = checked();
            g_store.Save();
            if (g_store.Config().startWithWindows) {
                std::wstring err;
                if (!ApplyAutostart(true, &err))
                    MessageBoxW(hwnd, err.c_str(), L"PlanPower", MB_ICONWARNING);
            }
            return 0;
        }

        if (id == IDC_ST_LOG) {
            g_store.Config().keepLog = checked();
            g_store.Save();
            return 0;
        }

        if (id == IDC_ST_ALLOW) {
            bool on = checked();
            // 开闸门必须先过确认窗。用户没确认就把勾去掉——不静默当成同意。
            if (on && !ConfirmEnableSystemTasks(hwnd)) {
                SendMessageW(c, BM_SETCHECK, BST_UNCHECKED, 0);
                return 0;
            }
            g_store.Config().allowSystemTasks = on;
            g_store.Save();
            RefreshSettings(hwnd);
            // 编辑窗开着的时候，那个「系统持久任务」单选按钮的可用性
            // 和说明文字都要跟着变——闸门就是这个开关的唯一真值来源。
            if (g_hEditor) EditorRefreshMode();
            UI_RefreshAll();
            return 0;
        }

        if (id == IDC_ST_CLEAN) {
            std::wstring q =
                L"将删除本机 \\PlanPower 文件夹下的全部系统任务。\n\n"
                L"注意：这是按文件夹清理的，所以其他 PlanPower 副本（比如另一支 U 盘）"
                L"在这台电脑上注册的任务也会一并删掉。\n\n"
                L"需要管理员授权。确定继续吗？";
            if (MessageBoxW(hwnd, q.c_str(), L"清理本机系统任务",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return 0;

            std::wstring err;
            if (!SchedRunElevated(SchedOp::CleanupAll, L"", false, &err)) {
                MessageBoxW(hwnd, (L"清理未完成：\n\n" + err).c_str(),
                            L"PlanPower", MB_ICONERROR);
                return 0;
            }

            // 系统侧清空了，本地标记也要跟着复位，否则列表上会挂着一堆
            // 名不副实的「系统任务」标签。
            g_store.ClearHost(MachineFingerprint());
            for (auto& t : g_store.Tasks()) t.persistent = false;
            g_store.Save();

            RefreshSettings(hwnd);
            UI_RefreshAll();
            TrayUpdateTip();
            MessageBoxW(hwnd, L"已清理本机全部 PlanPower 系统任务。",
                        L"PlanPower", MB_ICONINFORMATION);
            return 0;
        }

        if (id == IDC_ST_OPEN) {
            ShellExecuteW(hwnd, L"open", ExeDir().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, C_CARD);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hSet = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettings(HWND owner) {
    if (g_hSet) { SetForegroundWindow(g_hSet); return; }

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT adj = { 0, 0, S(ST_W_LOGICAL), S(560) };   // 先用估算高度建窗
    AdjustWindowRectEx(&adj, style, FALSE, WS_EX_DLGMODALFRAME);
    int W = adj.right - adj.left;
    int H = adj.bottom - adj.top;

    RECT ro;
    GetWindowRect(owner, &ro);
    int x = ro.left + ((ro.right - ro.left) - W) / 2;
    int y = ro.top  + ((ro.bottom - ro.top) - H) / 2;

    g_hSet = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PPSettings",
        L"设置 — PlanPower", style, x, y, W, H, owner, nullptr, g_hInst, nullptr);
    if (!g_hSet) return;

    MakeSettingsControls(g_hSet);

    // 控件都建好之后再问一次真实高度，把窗口收到刚好
    int clientH = SettingsLayout(g_hSet);
    RECT adj2 = { 0, 0, S(ST_W_LOGICAL), clientH };
    AdjustWindowRectEx(&adj2, style, FALSE, WS_EX_DLGMODALFRAME);
    int W2 = adj2.right - adj2.left, H2 = adj2.bottom - adj2.top;
    int x2 = ro.left + ((ro.right - ro.left) - W2) / 2;
    int y2 = ro.top  + ((ro.bottom - ro.top) - H2) / 2;
    SetWindowPos(g_hSet, nullptr, x2, y2, W2, H2,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    RefreshSettings(g_hSet);
    ShowWindow(g_hSet, SW_SHOW);
    SetForegroundWindow(g_hSet);
}

// ---------------------------------------------------------------------------
//  执行历史
// ---------------------------------------------------------------------------
// 数据来源就是 LogLine 写的那批文件：`logs\YYYY-MM.log`，UTF-16LE（带 BOM，
// v0.4.0 及以前的老文件没有 BOM，读取端两种都认），一行一条，行首是
// `[YYYY-MM-DD HH:MM:SS]`。
//
// 不另建一个「历史」数据结构，理由和日志清理共用 IsLogFileName 是同一条：
// 同一个事实只能有一份记录。多一份就可能和日志对不上，而用户看到对不上的
// 时候不知道该信哪个。
#define IDC_HIST_TEXT  6001
#define IDC_HIST_OPEN  6002
#define IDC_HIST_CLOSE 6003
#define IDC_HIST_TIP   6004

static HWND g_hHist = nullptr;

// 一次最多显示多少条。日志按月切，一条几十字节，300 条足够看清最近发生了什么，
// 又不至于让只读编辑框滚动起来卡顿。
static const int HIST_MAX_LINES = 300;

static std::wstring ReadWholeFileUtf16(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();

    std::wstring out;
    LARGE_INTEGER sz = {};
    // 上限 8MB：按月切分之后正常远小于这个数。真碰到超大文件宁可不读，
    // 也不要把整个文件拖进内存再分页。
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (8 << 20)) {
        DWORD n = (DWORD)sz.QuadPart;
        std::vector<unsigned char> raw(n);
        DWORD got = 0;
        if (ReadFile(h, raw.data(), n, &got, nullptr) && got == n) {
            // 跳过 UTF-16LE 的 BOM。v0.4.0 及以前写的文件没有 BOM，此时
            // 从 0 开始——两种都要能读，升级不该要求用户清空 logs。
            size_t off = (n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? 2 : 0;
            size_t chars = (n - off) / sizeof(wchar_t);   // 奇数字节截掉，免得读到半截字符
            out.resize(chars);
            // 用 memcpy 而不是把 raw 强转成 wchar_t*：off 是 2 时指针对不齐，
            // 在 ARM 上会是未定义行为。x86 上碰巧没事，但不该依赖这一点。
            if (chars) memcpy(&out[0], raw.data() + off, chars * sizeof(wchar_t));
        }
    }
    CloseHandle(h);
    return out;
}

static std::wstring BuildHistoryText() {
    std::wstring dir = JoinPath(ExeDir(), L"logs");

    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            if (IsLogFileName(n)) names.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 文件名就是 YYYY-MM，字典序即时间序。从最新的月份往前攒。
    std::sort(names.begin(), names.end());
    std::reverse(names.begin(), names.end());

    std::vector<std::wstring> lines;     // 先按「新 → 旧」攒
    for (size_t i = 0; i < names.size(); ++i) {
        if ((int)lines.size() >= HIST_MAX_LINES) break;

        std::wstring text = ReadWholeFileUtf16(JoinPath(dir, names[i]));

        // 拆行。CRLF 和裸 LF 都认（文件是本程序自己写的，但用户可能
        // 用编辑器打开改过，编辑器存回去的行尾不一定还是 CRLF）。
        std::vector<std::wstring> chunk;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = text.size();
            size_t end = nl;
            if (end > pos && text[end - 1] == L'\r') end--;
            if (end > pos) chunk.push_back(text.substr(pos, end - pos));
            pos = nl + 1;
        }

        // 同一个文件里行是「旧 → 新」，所以从尾部往前取，才和「新 → 旧」一致
        for (int k = (int)chunk.size() - 1; k >= 0; --k) {
            if ((int)lines.size() >= HIST_MAX_LINES) break;
            lines.push_back(chunk[k]);
        }
    }

    if (lines.empty()) return std::wstring();

    std::reverse(lines.begin(), lines.end());   // 翻回正序显示

    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        out += L"\r\n";
    }
    return out;
}

static void HistLayout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad  = S(16);
    int btnH = S(30);
    int rowY = rc.bottom - pad - btnH;

    int closeW = S(84), openW = S(136);
    int closeX = rc.right - pad - closeW;
    int openX  = closeX - S(8) - openW;

    MoveWindow(GetDlgItem(h, IDC_HIST_CLOSE), closeX, rowY, closeW, btnH, TRUE);
    MoveWindow(GetDlgItem(h, IDC_HIST_OPEN),  openX,  rowY, openW,  btnH, TRUE);
    MoveWindow(GetDlgItem(h, IDC_HIST_TIP),   pad,    rowY, openX - S(12) - pad, btnH, TRUE);

    MoveWindow(GetDlgItem(h, IDC_HIST_TEXT), pad, pad,
               rc.right - pad * 2,
               rowY - S(10) - pad, TRUE);
}

static LRESULT CALLBACK HistoryProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) HistLayout(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HIST_OPEN: {
            // 用系统资源管理器打开日志目录，而不是自己做文件管理。
            // 用户要看的是「原始文件」，给他一个记事本能打开的地方就够了。
            std::wstring dir = JoinPath(ExeDir(), L"logs");
            CreateDirectoryW(dir.c_str(), nullptr);
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDC_HIST_CLOSE:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_hHist = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHistory(HWND owner) {
    if (g_hHist) { SetForegroundWindow(g_hHist); return; }

    const int CW = S(660), CH = S(520);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    RECT adj = { 0, 0, CW, CH };
    AdjustWindowRectEx(&adj, style, FALSE, WS_EX_DLGMODALFRAME);
    int W = adj.right - adj.left, H = adj.bottom - adj.top;

    RECT ro = { 0, 0, 0, 0 };
    if (owner) GetWindowRect(owner, &ro);
    int x = ro.left + ((ro.right - ro.left) - W) / 2;
    int y = ro.top  + ((ro.bottom - ro.top) - H) / 2;

    g_hHist = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PPHistory",
        L"执行历史 — PlanPower", style, x, y, W, H,
        owner, nullptr, g_hInst, nullptr);
    if (!g_hHist) return;

    // 只读多行编辑框：滚动、选中、Ctrl+C 全都是系统给的，不用自己写
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, g_hHist, (HMENU)(INT_PTR)IDC_HIST_TEXT, g_hInst, nullptr);
    if (ed) {
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        std::wstring text = BuildHistoryText();
        if (text.empty())
            text = L"还没有执行记录。\r\n\r\n"
                   L"任务真正执行、被暂停/启用、被取消时才会写日志；"
                   L"如果日志功能是关的，这里会一直是空的（可在「设置」里打开）。";
        SetWindowTextW(ed, text.c_str());
        // 光标挪到末尾，打开就看到最近一条
        SendMessageW(ed, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
        SendMessageW(ed, EM_SCROLLCARET, 0, 0);
    }

    wchar_t tip[128];
    swprintf(tip, 128, L"日志保留 %d 个月，可在「设置」里调整",
             g_store.Config().logKeepMonths);
    CreateWindowExW(0, L"STATIC", tip,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, g_hHist, (HMENU)(INT_PTR)IDC_HIST_TIP, g_hInst, nullptr);
    SendMessageW(GetDlgItem(g_hHist, IDC_HIST_TIP), WM_SETFONT, (WPARAM)g_fSmall, TRUE);

    CreateWindowExW(0, L"BUTTON", L"打开日志文件夹",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, g_hHist, (HMENU)(INT_PTR)IDC_HIST_OPEN, g_hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"关闭",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, g_hHist, (HMENU)(INT_PTR)IDC_HIST_CLOSE, g_hInst, nullptr);
    SendMessageW(GetDlgItem(g_hHist, IDC_HIST_OPEN),  WM_SETFONT, (WPARAM)g_fBody, TRUE);
    SendMessageW(GetDlgItem(g_hHist, IDC_HIST_CLOSE), WM_SETFONT, (WPARAM)g_fBody, TRUE);

    HistLayout(g_hHist);
    ShowWindow(g_hHist, SW_SHOW);
    SetForegroundWindow(g_hHist);
}

// ---------------------------------------------------------------------------
//  主窗口过程
// ---------------------------------------------------------------------------
// 激活某一行上的某个元素。
//
// 鼠标点击和键盘操作共用这一份：两份实现迟早会分叉，而分叉的表现是
// 「键盘删任务没删掉系统任务」——一个只有用键盘的人才会遇到的 bug。
static void ActivateRowElem(HWND hwnd, int row, int elem) {
    auto vis = VisibleTasks();
    if (row < 0 || row >= (int)vis.size()) return;
    PowerTask* t = vis[row];

    switch (elem) {

        case ELEM_TOGGLE: {
            // 开关翻的是 enabled，不是 running。
            //
            // 之前这里按 running 翻转，结果是：每天/每周任务执行完立刻重排，
            // running 恒为真，于是「关」分支只清掉 running、enabled 仍是真，
            // 紧接着 UI_RefreshAll 重新排期又把它设回 true——开关点了马上弹回，
            // 看起来就是「只有倒计时能停」。倒计时能停只是巧合：那个分支
            // 顺手把 enabled 也置了假。
            PowerTask copy = *t;
            bool turnOn = !t->enabled;
            t->enabled = turnOn;
            if (turnOn) {
                ScheduleTask(*t, true);   // 强制重算，丢掉停用前的旧 dueTick
            } else {
                t->running = false;
            }
            // 系统任务跟着一起启停。不同步的话，界面上写着"已停用"，
            // 任务计划程序里那条却还在按时跑——这是最坏的一类不一致。
            if (copy.persistent) {
                std::wstring err;
                if (!SysSetEnabled(copy, turnOn, &err)) {
                    MessageBoxW(hwnd,
                        (L"系统任务状态没能同步：\n\n" + err +
                         L"\n\n本机任务计划程序里的那条可能仍是旧状态。").c_str(),
                        L"PlanPower · 系统任务未同步", MB_ICONWARNING);
                }
            }
            g_store.Save();
            UI_RefreshAll();
            TrayUpdateTip();
            break;
        }

        case ELEM_RUN:
            // 立即执行一次，不改动原有排期
            ExecuteAction(t->action, 0, t->message, t->forceClose);
            LogLine(std::wstring(L"手动执行：") + t->name);
            break;

        case ELEM_EDIT:
            ShowEditor(hwnd, t);
            break;

        case ELEM_DEL: {
            PowerTask copy = *t;          // RemoveTask 之后 t 就悬空了
            std::wstring q = L"确定要删除任务「" + copy.name + L"」吗？\n\n此操作不可撤销。";
            if (copy.persistent) {
                q += L"\n\n这条任务已注册为系统任务，删除时会一并从 Windows "
                     L"任务计划程序中移除（需管理员授权一次）。";
            }
            if (MessageBoxW(hwnd, q.c_str(), L"删除任务",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
                bool okToDelete = true;
                if (copy.persistent) {
                    std::wstring err;
                    if (!SysUnregister(copy, &err)) {
                        // 系统任务没摘掉就不删本地记录。删了的话，界面上再也
                        // 看不到这条任务，而它还会在后台按时执行——用户连
                        // 从哪儿关掉它都找不到。
                        okToDelete = false;
                        MessageBoxW(hwnd,
                            (L"没能从 Windows 任务计划程序里移除这条系统任务，"
                             L"所以没有删除。\n\n原因：" + err +
                             L"\n\n可以稍后重试，或者到「设置」里用"
                             L"「清理本机系统任务」一次性清干净。").c_str(),
                            L"PlanPower · 删除已取消", MB_ICONWARNING);
                    }
                }
                if (okToDelete) {
                    g_store.RemoveTask(copy.id);
                    UI_RefreshAll();
                    TrayUpdateTip();
                    LogLine(L"删除任务：" + copy.id);
                }
            }
            break;
        }
        }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        PaintMain(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        // 防止窗口被拖得过小导致布局错乱
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(720);
        mmi->ptMinTrackSize.y = S(430);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_TICK) {
            UI_Tick();
            TrayUpdateTip();
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Layout L = ComputeLayout(hwnd);
        int newRow = -1, newElem = 0;

        if (PtInRect(&L.rcNewBtn, pt))      newElem = 901;
        else if (PtInRect(&L.rcSetBtn, pt)) newElem = 900;
        else if (PtInRect(&L.rcHistBtn, pt)) newElem = 902;
        else {
            newRow = HitTestRow(hwnd, pt);
            if (newRow >= 0) newElem = HitTestElem(hwnd, pt, newRow);
        }

        if (newRow != g_hoverRow || newElem != g_hoverElem) {
            g_hoverRow = newRow;
            g_hoverElem = newElem;
            InvalidateRect(hwnd, &L.rcList, FALSE);
            InvalidateRect(hwnd, &L.rcHeader, FALSE);

            // 手型光标
            LPCWSTR cur = (newElem != 0) ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, cur));
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hoverElem != 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_MOUSEWHEEL: {
        Layout L = ComputeLayout(hwnd);
        int contentH = (int)VisibleTasks().size() * S(ROW_H) + S(12);
        int viewH    = L.rcList.bottom - L.rcList.top;
        int maxScroll = contentH > viewH ? contentH - viewH : 0;

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_scroll -= (delta / 120) * S(60);
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > maxScroll) g_scroll = maxScroll;

        InvalidateRect(hwnd, &L.rcList, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Layout L = ComputeLayout(hwnd);

        if (PtInRect(&L.rcNewBtn, pt)) { ShowEditor(hwnd, nullptr); return 0; }
        if (PtInRect(&L.rcSetBtn, pt)) { ShowSettings(hwnd); return 0; }
        if (PtInRect(&L.rcHistBtn, pt)) { ShowHistory(hwnd); return 0; }

        // 筛选
        for (int i = 0; i < 3; ++i) {
            if (PtInRect(&L.rcChip[i], pt)) {
                g_filter = i;
                g_scroll = 0;
                g_selRow = -1;      // 换了筛选，原来的选中行已经没有意义
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        int row = HitTestRow(hwnd, pt);
        if (row < 0) return 0;

        // 点一下就把选中行移过来，之后方向键从这里继续。
        g_selRow = row;
        int elem = HitTestElem(hwnd, pt, row);
        ActivateRowElem(hwnd, row, elem);
        InvalidateRect(hwnd, &L.rcList, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        // 列表是自绘的，没有原生焦点项，所以「选中」只能由这里维护。
        // ↑↓ 选行、空格切开关、回车编辑、Delete 删除。
        auto vis = VisibleTasks();
        int n = (int)vis.size();
        if (n == 0) { g_selRow = -1; return 0; }
        if (g_selRow >= n) g_selRow = n - 1;      // 上一轮可能删掉了一行

        Layout L = ComputeLayout(hwnd);
        int before = g_selRow;

        switch (wp) {
        case VK_DOWN: g_selRow = (g_selRow < 0) ? 0 : (g_selRow + 1) % n; break;
        case VK_UP:   g_selRow = (g_selRow < 0) ? n - 1 : (g_selRow - 1 + n) % n; break;
        case VK_HOME: g_selRow = 0; break;
        case VK_END:  g_selRow = n - 1; break;
        case VK_SPACE:
            if (g_selRow >= 0) ActivateRowElem(hwnd, g_selRow, ELEM_TOGGLE);
            return 0;
        case VK_RETURN:
            if (g_selRow >= 0) ActivateRowElem(hwnd, g_selRow, ELEM_EDIT);
            return 0;
        case VK_DELETE:
            if (g_selRow >= 0) {
                ActivateRowElem(hwnd, g_selRow, ELEM_DEL);
                auto after = VisibleTasks();
                if (g_selRow >= (int)after.size()) g_selRow = (int)after.size() - 1;
                InvalidateRect(hwnd, &L.rcList, FALSE);
            }
            return 0;
        default:
            return 0;
        }

        if (g_selRow != before) InvalidateRect(hwnd, &L.rcList, FALSE);
        return 0;
    }

    case WM_TRAYMSG:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case 2001: QuickCountdown(g_store.Config().quickMinutes[0]); return 0;
        case 2002: QuickCountdown(g_store.Config().quickMinutes[1]); return 0;
        case 2003: QuickCountdown(g_store.Config().quickMinutes[2]); return 0;
        case 2004: UI_CancelAllRunning(); return 0;
        case 2005: DoSleep();   return 0;
        case 2006: DoLock();    return 0;
        case 2007:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case 2008:
            if (!ConfirmExitWithRunningTasks(hwnd)) return 0;
            DestroyWindow(hwnd);
            return 0;
        // 批量启停。系统持久任务要走一次提权，所以整批合成一次 UAC，
        // 而不是在 SetAllEnabled 里逐条调用 SchedRunElevated。
        case 2009: SetAllEnabled(hwnd, false); return 0;
        case 2010: SetAllEnabled(hwnd, true);  return 0;
        case 2011: ShowHistory(hwnd);          return 0;
        }
        break;
    }

    case WM_CLOSE:
        // 关闭 = 最小化到托盘
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_TICK);
        TrayRemove();
        if (g_icLarge) { DestroyIcon(g_icLarge); g_icLarge = nullptr; }
        if (g_icSmall) { DestroyIcon(g_icSmall); g_icSmall = nullptr; }
        DestroyFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
static void RegisterClasses() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PPMain";
    wc.hIcon         = g_icLarge;
    wc.hIconSm       = g_icSmall;
    RegisterClassExW(&wc);

    WNDCLASSEXW we = {};
    we.cbSize        = sizeof(we);
    we.lpfnWndProc   = EditorProc;
    we.hInstance     = g_hInst;
    we.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    we.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    we.lpszClassName = L"PPEditor";
    RegisterClassExW(&we);

    WNDCLASSEXW wn = {};
    wn.cbSize        = sizeof(wn);
    wn.lpfnWndProc   = NoticeProc;
    wn.hInstance     = g_hInst;
    wn.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wn.hbrBackground = nullptr;
    wn.lpszClassName = L"PPNotice";
    RegisterClassExW(&wn);

    WNDCLASSEXW ws = {};
    ws.cbSize        = sizeof(ws);
    ws.lpfnWndProc   = SettingsProc;
    ws.hInstance     = g_hInst;
    ws.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // 设置窗与确认窗都是白底 + 子控件，背景刷直接给白，省掉自绘背景
    ws.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    ws.lpszClassName = L"PPSettings";
    RegisterClassExW(&ws);

    WNDCLASSEXW wc2 = {};
    wc2.cbSize        = sizeof(wc2);
    wc2.lpfnWndProc   = ConfirmProc;
    wc2.hInstance     = g_hInst;
    wc2.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc2.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc2.lpszClassName = L"PPConfirm";
    RegisterClassExW(&wc2);

    WNDCLASSEXW wh = {};
    wh.cbSize        = sizeof(wh);
    wh.lpfnWndProc   = HistoryProc;
    wh.hInstance     = g_hInst;
    wh.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wh.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wh.lpszClassName = L"PPHistory";
    RegisterClassExW(&wh);
}

bool UI_Create(HINSTANCE hInst, int nCmdShow, bool startHidden) {
    g_hInst = hInst;

    // 日志维护放在最前面：清过期文件、给老版本写的无 BOM 文件补 BOM。
    // 早一点做，用户打开日志文件夹时就已经是好的。keepLog 关闭时是空操作。
    LogMaintenance();

    // DPI
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (g_dpi < 96) g_dpi = 96;

    CreateFonts();
    g_icLarge = MakeIcon(32);
    g_icSmall = MakeIcon(16);

    RegisterClasses();

    int W = S(880), H = S(660);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;

    g_hMain = CreateWindowExW(0, L"PPMain", L"PlanPower — 定时电源管理",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        x, y, W, H, nullptr, nullptr, hInst, nullptr);
    if (!g_hMain) return false;

    // 开机自启（命令行带 /tray）：窗口照建，但不显示。
    //
    // 窗口必须真的建出来——托盘图标要挂在它上面收消息，计时器也挂在它上面。
    // 不能因为"不显示"就跳过后面的 TrayAdd / SetTimer。
    //
    // 用 SW_HIDE 而不是"干脆不调 ShowWindow"：SW_HIDE 会把窗口的可见状态
    // 明确置成隐藏，之后 ShowWindow(SW_SHOW) 才是确定能显示出来的。依赖
    // CreateWindowExW 的默认隐藏状态，会碰到"第一次 SW_SHOW 没反应、
    // 要点两次"这类只在某些 shell 版本上出现的怪毛病。
    if (startHidden) {
        ShowWindow(g_hMain, SW_HIDE);
    } else {
        ShowWindow(g_hMain, nCmdShow);
        UpdateWindow(g_hMain);
    }

    TrayAdd(g_hMain);
    SetTimer(g_hMain, IDT_TICK, 500, nullptr);

    // 初始排期
    for (auto& t : g_store.Tasks()) ScheduleTask(t);

    // 静默启动时给一次气泡提示。用户看不到主界面，而 Win10 默认把新出现的
    // 托盘图标收进溢出区——不提示的话，从用户视角看就是"什么都没发生"。
    if (startHidden) {
        TrayBalloon(L"PlanPower 已在后台运行",
                    L"双击托盘图标可打开主界面；右键可快捷关机。");
    }

    return true;
}

HWND UI_MainWnd() { return g_hMain; }

bool UI_PreTranslate(MSG* msg) {
    // 有子控件的窗口都要过一遍 IsDialogMessageW，否则 Tab 键在控件之间
    // 跳不过去、回车也点不到默认按钮。确认窗自己跑模态循环，不在这里处理。
    if (g_hEditor && IsWindow(g_hEditor) && IsDialogMessageW(g_hEditor, msg))
        return true;
    if (g_hSet && IsWindow(g_hSet) && IsDialogMessageW(g_hSet, msg))
        return true;
    return false;
}
