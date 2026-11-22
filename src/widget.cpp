// Modified 2026-11-22: taskbar free-gap detection (UI Automation), compact fallback for a full taskbar
// and the device flyout panel. Full list of changes: CHANGES-FORK.md at the repository root.
// Original work: BatteryHub contributors, GPL-3.0-or-later (see LICENSE).
#include "widget.h"

#include "i18n.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light, g_hover;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_stacked;     // icon above the percentage (menu option, PR #525)
bool g_vertical;    // the primary taskbar is docked left or right: one block per device, stacked (PR #321)
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
int g_maxW = 0;  // widest the strip may be on this bar (client px); 0 = unlimited
int g_fillW = 0;  // width the strip should expand to (the empty gap), 0 = size to content
bool g_compressed = false;  // the strip had to leave devices out: a click opens the flyout instead of the menu
bool g_manualStack = false;  // the user picked the layout themselves: never auto-compact on top of it
SIZE g_size{};
RECT g_content{ 0, 0, 0, 0 };  // client rect of the drawn content; outside it the strip is click-through
HINSTANCE g_inst;

// Copies of the strip on secondary-monitor taskbars (Shell_SecondaryTrayWnd), so the levels stay visible
// while a game runs fullscreen on the main screen. Same drawing, same click and tooltip; each one sits
// left of its taskbar's clock. The primary strip above is unchanged, and with one monitor none exist.
struct Mirror {
    HWND bar = nullptr, wnd = nullptr;
    bool hover = false;
    SIZE size{};
    int dpi = 0;
    int lastLeft = INT_MIN;  // client x it was last placed at; INT_MIN = not placed (hidden)
};
std::vector<Mirror> g_mirrors;
// Which taskbars show the strip: "all", "main", or one monitor's id (MonitorId), e.g. only monitor 3.
// With one monitor chosen, the primary strip is hidden; if that monitor is not connected, the primary
// strip is shown instead so the levels never disappear.
std::wstring g_choice = L"all";
bool g_primaryHidden = false;
HWND g_dpiBar;  // while a mirror is painted or placed, Px() uses its taskbar's DPI

const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi() {
    HWND bar = g_dpiBar ? g_dpiBar : g_taskbar;
    UINT d = bar ? GetDpiForWindow(bar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on BatteryHub.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

// Where a Windows 11 secondary taskbar's clock, widgets button and task buttons are. Its XAML has no child
// windows for them, so the same lookup thread reads them with UI Automation (the clock has AutomationId
// "SystemTrayIcon", widgets button "WidgetsButton", task buttons class "Taskbar.TaskListButtonAutomationPeer";
// found by PR #107).
struct Span {
    HWND bar = nullptr;
    bool measured = false;   // UI Automation answered for this taskbar
    bool hasClock = false;   // the clock can be turned off on secondary taskbars
    int clockLeft = 0;       // screen x
    bool hasWidgets = false; // Widgets (weather) button on secondary taskbar
    RECT widgets{};          // bounding rectangle of WidgetsButton (screen coords)
    bool hasTasks = false;
    int tasksRight = 0;      // right edge of the last task button left of the clock (screen x)
};
std::vector<Span> g_spans;   // guarded by g_wbLock
bool g_wbMirrors;            // guarded by g_wbLock: measure secondary taskbars too
int g_tasksRight;            // guarded by g_wbLock: screen x of the last task button's right edge (0 = unknown)
HWND g_tasksRightBar;        // taskbar g_tasksRight was measured on (guarded by g_wbLock)
int g_chevronLeft;           // guarded by g_wbLock: screen x where the tray's overflow arrow starts (0 = unknown)

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    if (v.bstrVal) uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

Span MeasureSpan(IUIAutomation* uia, HWND bar) {
    Span s;
    s.bar = bar;
    RECT br{};
    IUIAutomationElement* root = nullptr;
    if (!GetWindowRect(bar, &br) || FAILED(uia->ElementFromHandle(bar, &root)) || !root) return s;
    s.measured = true;
    auto inBar = [&](const RECT& r) { return r.right > r.left && r.left >= br.left && r.right <= br.right; };
    if (IUIAutomationCondition* clock = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, clock, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {  // leftmost of the clock area's buttons
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.left < s.clockLeft)) {
                    s.clockLeft = r.left;
                    s.hasClock = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        clock->Release();
    }
    if (IUIAutomationCondition* wbCond = StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton")) {
        IUIAutomationElement* wbEl = nullptr;
        RECT r{};
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, wbCond, &wbEl)) && wbEl &&
            SUCCEEDED(wbEl->get_CurrentBoundingRectangle(&r)) && inBar(r)) {
            s.widgets = r;
            s.hasWidgets = true;
        }
        if (wbEl) wbEl->Release();
        wbCond->Release();
    }
    if (IUIAutomationCondition* task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.right <= s.clockLeft) && (!s.hasWidgets || r.right <= s.widgets.left) &&
                    (!s.hasTasks || r.right > s.tasksRight)) {
                    s.tasksRight = r.right;
                    s.hasTasks = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        task->Release();
    }
    root->Release();
    return s;
}

// Right edge of the last task button on this bar (screen x), or 0 when the bar has none.
// Deliberately independent of MeasureSpan(): that one skips buttons right of the Widgets button, which
// only makes sense on secondary bars — on the primary bar the weather widget sits at the far left.
int MeasureTasksRight(IUIAutomation* uia, HWND bar) {
    RECT br{};
    IUIAutomationElement* root = nullptr;
    if (!GetWindowRect(bar, &br) || FAILED(uia->ElementFromHandle(bar, &root)) || !root) return 0;
    int right = 0;
    if (IUIAutomationCondition* task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left && r.left >= br.left && r.right <= br.right && (int)r.right > right)
                    right = (int)r.right;
                if (el) el->Release();
            }
        if (all) all->Release();
        task->Release();
    }
    root->Release();
    return right;
}

// Left edge (screen x) of the tray's overflow arrow, or 0 when unknown. On Windows 11 the arrow is the
// leftmost SystemTrayIcon element; Windows 10 keeps it as the first button inside TrayNotifyWnd.
int MeasureChevronLeft(IUIAutomation* uia, HWND bar) {
    int left = 0;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(bar, &root)) || !root) return 0;
    RECT br{};
    GetWindowRect(bar, &br);
    if (IUIAutomationCondition* c = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, c, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left && r.left >= br.left && r.right <= br.right && (!left || (int)r.left < left))
                    left = (int)r.left;
                if (el) el->Release();
            }
        if (all) all->Release();
        c->Release();
    }
    root->Release();
    return left;
}

bool IsXamlTaskbar(HWND bar) {
    return FindWindowExW(bar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
}

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockShared(&g_wbLock);
        bool mirrors = g_wbMirrors;
        ReleaseSRWLockShared(&g_wbLock);
        std::vector<Span> spans;
        if (bar && mirrors && uia)
            for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
                 t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
                if (IsXamlTaskbar(t)) spans.push_back(MeasureSpan(uia, t));  // Windows 10 bars have real child windows
        // The empty strip between the task buttons and the tray arrow: where the widget is allowed to live.
        int tasksRight = 0, chevronLeft = 0;
        if (bar && uia) {
            tasksRight = MeasureTasksRight(uia, bar);
            chevronLeft = MeasureChevronLeft(uia, bar);
        }
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        g_spans = std::move(spans);
        g_tasksRight = tasksRight;
        g_chevronLeft = chevronLeft;
        g_tasksRightBar = bar;
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

// Never blocks: screen x of the last task button's right edge on this taskbar (0 = unknown).
int TasksRightEdge(HWND taskbar) {
    AcquireSRWLockShared(&g_wbLock);
    int x = g_tasksRightBar == taskbar ? g_tasksRight : 0;
    ReleaseSRWLockShared(&g_wbLock);
    return x;
}

// Never blocks: screen x where the tray's overflow arrow starts on this taskbar (0 = unknown).
int ChevronLeftEdge(HWND taskbar) {
    AcquireSRWLockShared(&g_wbLock);
    int x = g_tasksRightBar == taskbar ? g_chevronLeft : 0;
    ReleaseSRWLockShared(&g_wbLock);
    return x;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        case DeviceKind::Phone:    return 0xE8EA;  // CellPhone
        case DeviceKind::Microphone: return 0xE720;  // Microphone
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBolt = 0xE945;            // LightningBolt
constexpr wchar_t kCheck = 0xE73E;           // CheckMark, in place of the bolt once fully charged
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// Soft glow around a glyph, like a lit RGB device: the coverage mask padded by r pixels, box-blurred
// twice and brightened so a thin icon outline still gives a visible glow.
Mask Halo(const Mask& m, int r) {
    Mask g;
    g.w = m.w + 2 * r;
    g.h = m.h + 2 * r;
    std::vector<int> a((size_t)g.w * g.h, 0), tmp(a.size(), 0);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) a[(y + r) * g.w + x + r] = m.a[y * m.w + x];
    int n = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (x + k >= 0 && x + k < g.w) s += a[y * g.w + x + k];
                tmp[y * g.w + x] = s / n;
            }
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (y + k >= 0 && y + k < g.h) s += tmp[(y + k) * g.w + x];
                a[y * g.w + x] = s / n;
            }
    }
    g.a.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) g.a[i] = (BYTE)std::min(255, a[i] * 3);
    return g;
}

// One drawn piece of the strip. Normal pieces follow each other from left to right. A piece with an
// `anchor` (index of an earlier piece) is a caption: drawn over that piece, `bx`/`by` from its top-left
// corner, and taking no room of its own. Branded devices add a glow behind the glyph and a level bar.
struct Piece {
    Mask m;
    Rgb color;
    int alpha;
    int gapAfter;
    int anchor = -1;
    int bx = 0, by = 0;
    int barPct = -1;  // >= 0: thin level bar under the text (branded devices)
    Rgb barColor{};
    Mask halo;        // glow drawn behind the glyph (branded devices)
    int haloR = 0, haloAlpha = 0;
    int px = INT_MIN, py = 0;  // fixed top-left position (stacked and vertical layouts); INT_MIN = flow left to right
};

// Three arrangements of the same pieces:
//  * normal: "[icon] 30%  [icon] 93%" in one row (unchanged default)
//  * stacked (menu option, from PR #525): each device's icon above its percentage, to save width;
//    used only when the taskbar is tall enough, otherwise the normal row
//  * vertical (left / right taskbar, from PR #321): one device per row block, stacked top to bottom
// `width` and `height` return the strip's size; in the vertical layout the width is the bar's.
std::vector<Piece> Layout(int barW, int barH, int& width, int& height, int maxW, int fillW) {
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    Rgb full = g_light ? Rgb{ 16, 124, 16 } : Rgb{ 108, 203, 95 };  // Windows "success" green
    HFONT icons = MakeFont(Px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT bolt = MakeFont(Px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT text = MakeFont(Px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI", Px(12));
    HFONT caption = MakeFont(std::max(8, Px(7)), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");  // never under 8 px
    g_compressed = false;

    // Gap too narrow for a full slot: keep the most critical device — icon plus percentage when they fit
    // (the icon says whose battery it is), then the percentage alone, then just the icon.
    if (maxW > 0 && maxW < Px(64) && !g_items.empty()) {
        g_compressed = g_items.size() > 1;
        size_t pick = 0;
        for (size_t i = 1; i < g_items.size(); i++)
            if (g_items[i].percent >= 0 && (g_items[pick].percent < 0 || g_items[i].percent < g_items[pick].percent)) pick = i;
        const WidgetItem& it = g_items[pick];
        BrandColor bc = AccentColor(it.accent, g_light);
        Rgb accent{ bc.r, bc.g, bc.b };
        std::wstring txt = !it.text.empty() ? it.text : (it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"");
        Rgb tc = it.low ? red : fg;
        Piece ic{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons, g_light), accent, 255, 0 };
        Piece lb{ RenderMask(txt, text, g_light), tc, 255, 0 };
        std::vector<Piece> one;
        int w = maxW;
        int need = ic.m.w + Px(3) + lb.m.w + Px(10);
        if (!txt.empty() && need <= maxW) {  // icon and number together
            int x = (w - (ic.m.w + Px(3) + lb.m.w)) / 2;
            ic.px = x;
            ic.py = (barH - ic.m.h) / 2;
            lb.px = x + ic.m.w + Px(3);
            lb.py = (barH - lb.m.h) / 2;
            one.push_back(std::move(ic));
            one.push_back(std::move(lb));
        } else if (!txt.empty() && lb.m.w + Px(6) <= maxW) {
            lb.px = (w - lb.m.w) / 2;
            lb.py = (barH - lb.m.h) / 2;
            one.push_back(std::move(lb));
        } else {
            w = std::min(maxW, ic.m.w + Px(8));
            ic.px = (w - ic.m.w) / 2;
            ic.py = (barH - ic.m.h) / 2;
            one.push_back(std::move(ic));
        }
        DeleteObject(icons);
        DeleteObject(bolt);
        DeleteObject(text);
        DeleteObject(caption);
        width = w;
        height = barH;
        return one;
    }

    std::vector<Piece> out;
    struct Slot { int icon, label, bolt; };  // indices of one device's pieces
    std::vector<Slot> slots;
    if (g_items.empty()) out.push_back({ RenderMask(std::wstring(1, kBatteryUnknown), icons, g_light), fg, 110, 0 });
    for (size_t i = 0; i < g_items.size(); i++) {
        const auto& it = g_items[i];
        int a = it.dim ? 120 : 255;
        Rgb tc = it.low ? red : fg;
        bool mark = it.charging || it.full;  // bolt while charging, green check mark once full
        size_t glyph = out.size();
        slots.push_back({ (int)glyph, (int)glyph + 1, mark ? (int)glyph + 2 : -1 });
        bool branded = it.accent != Accent::None;
        BrandColor bc = AccentColor(it.accent, g_light);
        Rgb accent{ bc.r, bc.g, bc.b };
        Piece icon{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons, g_light), branded ? accent : fg, a, Px(5) };
        if (branded && !it.dim) {
            icon.haloR = Px(3);
            icon.halo = Halo(icon.m, icon.haloR);
            icon.haloAlpha = g_light ? 60 : 140;
        }
        out.push_back(std::move(icon));
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"?";
        Piece label{ RenderMask(t, text, g_light), tc, a, mark ? Px(2) : 0 };
        if (branded && it.percent >= 0) {
            label.barPct = it.percent;
            label.barColor = it.low ? red : accent;
        }
        out.push_back(std::move(label));
        if (mark) out.push_back({ RenderMask(std::wstring(1, it.full ? kCheck : kBolt), bolt, g_light), it.full ? full : charge, a, 0 });
        out.back().gapAfter = i + 1 < g_items.size() ? Px(14) : 0;
        if (!it.label.empty()) {  // tiny caption in the bottom-right corner of the icon
            Piece c{ RenderMask(it.label, caption, g_light), fg, a, 0 };
            c.anchor = (int)glyph;
            c.bx = out[glyph].m.w - c.m.w + Px(3);
            c.by = out[glyph].m.h - c.m.h + Px(3);
            out.push_back(c);
        }
    }
    DeleteObject(icons);
    DeleteObject(bolt);
    DeleteObject(text);
    DeleteObject(caption);

    height = barH;
    const int rowGap = Px(2), barRoom = Px(5);  // barRoom: space for a brand level bar under the text
    int iconH = 0, textH = 0;
    for (const Slot& s : slots) {
        iconH = std::max(iconH, out[s.icon].m.h);
        textH = std::max(textH, out[s.label].m.h);
    }
    // Four or more devices no longer fit comfortably next to the tray, so the strip switches to the
    // stacked form on its own (icon above the percentage) instead of running across the taskbar. The
    // same happens for fewer devices when the task buttons leave little room.
    bool tallEnough = !g_vertical && !slots.empty() && barH >= iconH + rowGap + textH + barRoom + Px(6);
    bool tight = maxW > 0 && !slots.empty() && Px(78) * (int)slots.size() + Px(16) > maxW;
    bool stacked = !g_vertical && tallEnough && (g_stacked || (!g_manualStack && (g_items.size() >= 4 || tight)));
    if (g_vertical) {
        // One block per device, centred in the bar's width: icon, then the percentage (and bolt) under it.
        width = barW;
        int y = Px(6);
        if (slots.empty()) {
            // No devices yet (startup, or the last one disconnected): only the placeholder icon. Without this
            // branch the row layout below would keep height = barH, and on a left / right taskbar barH is the
            // whole screen height, so the strip covered the entire taskbar.
            Piece& ic = out[0];
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            height = y + ic.m.h + Px(6);
            return out;
        }
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            y += ic.m.h + rowGap;
            lb.px = (barW - lb.m.w - boltW) / 2;
            lb.py = y;
            if (s.bolt >= 0) {
                out[s.bolt].px = lb.px + lb.m.w + Px(2);
                out[s.bolt].py = y + (lb.m.h - out[s.bolt].m.h) / 2;
            }
            y += lb.m.h + (lb.barPct >= 0 ? barRoom : 0) + Px(8);
        }
        height = y;
        return out;
    }
    if (stacked) {
        // Icon on top (bolt to its right), percentage centred under it; the pair centred in the bar's height.
        // Slot widths come first so a long list can be capped to the taskbar before anything is placed.
        int top = (barH - (iconH + rowGap + textH + barRoom)) / 2;
        std::vector<int> slotW(slots.size());
        for (size_t i = 0; i < slots.size(); i++) {
            const Slot& s = slots[i];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            slotW[i] = std::max(out[s.icon].m.w + boltW, out[s.label].m.w);
        }
        int gap = Px(10);
        auto total = [&](size_t n, int g) {
            int w = Px(16);
            for (size_t i = 0; i < n; i++) w += slotW[i];
            if (n) w += g * (int)(n - 1);
            return w;
        };
        size_t keep = slots.size();
        if (maxW > 0 && total(slots.size(), gap) > maxW) {
            gap = Px(6);
            if (total(slots.size(), gap) > maxW) gap = Px(3);
            keep = 1;
            while (keep < slots.size() && total(keep + 1, gap) + Px(30) <= maxW) keep++;
        }
        int x = Px(8);
        // Spread the slots so the strip fills the gap exactly, instead of leaving empty bar on one side.
        if (fillW > 0 && keep > 1) {
            int natural = total(keep, gap);
            if (fillW > natural) gap += (fillW - natural) / (int)(keep - 1);
        }
        for (size_t i = 0; i < keep; i++) {
            const Slot& s = slots[i];
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            ic.px = x + (slotW[i] - ic.m.w - boltW) / 2;
            ic.py = top + (iconH - ic.m.h) / 2;
            if (s.bolt >= 0) {
                out[s.bolt].px = ic.px + ic.m.w + Px(2);
                out[s.bolt].py = top + (iconH - out[s.bolt].m.h) / 2;
            }
            lb.px = x + (slotW[i] - lb.m.w) / 2;
            lb.py = top + iconH + rowGap;
            x += slotW[i] + gap;
        }
        width = x - gap + Px(8);
        if (keep < slots.size()) {  // the rest become one "+N" chip, but only if it still fits
            size_t rest = slots.size() - keep;
            g_compressed = true;  // devices left out: the panel lists them all
            if (total(keep, gap) + Px(34) > maxW) {  // no room for the chip: show fewer devices, none dropped
                out.erase(out.begin() + slots[keep].icon, out.end());
                slots.resize(keep);
                width = total(keep, gap);
                return out;
            }
            out.erase(out.begin() + slots[keep].icon, out.end());
            Piece plus{ RenderMask(L"+" + std::to_wstring(rest), text, g_light), fg, 255, 0 };
            plus.px = x - gap + Px(6);
            plus.py = top + (iconH + rowGap + textH - plus.m.h) / 2;
            width = plus.px + plus.m.w + Px(6);
            out.push_back(std::move(plus));
        }
        return out;
    }
    // Row layout, also bounded by the taskbar: tighten the gaps first, then fold the tail into "+N".
    auto laid = [&](size_t b) {  // index of the last positioned piece of device block b
        int s = slots[b].icon;
        int e = b + 1 < slots.size() ? slots[b + 1].icon : (int)out.size();
        for (int i = e - 1; i >= s; i--)
            if (out[i].anchor < 0) return i;
        return s;
    };
    auto applyGap = [&](int gap) {
        for (size_t b = 0; b < slots.size(); b++)
            out[laid(b)].gapAfter = b + 1 < slots.size() ? gap : 0;
    };
    auto measure = [&] {
        int w = Px(8) * 2;
        for (const auto& p : out)
            if (p.anchor < 0) w += p.m.w + p.gapAfter;
        return w;
    };
    applyGap(Px(14));
    width = measure();
    if (maxW > 0 && width > maxW) { applyGap(Px(6)); width = measure(); }
    if (maxW > 0 && width > maxW) { applyGap(Px(2)); width = measure(); }
    if (maxW > 0 && width > maxW && slots.size() > 1) {
        const int gap = Px(2), plusRoom = Px(34);
        int acc = Px(8) * 2;
        size_t keep = 0;
        for (; keep < slots.size(); keep++) {
            int bw = 0;
            int s = slots[keep].icon;
            int e = keep + 1 < slots.size() ? slots[keep + 1].icon : (int)out.size();
            for (int i = s; i < e; i++)
                if (out[i].anchor < 0) bw += out[i].m.w + (i == laid(keep) ? gap : out[i].gapAfter);
            bool more = keep + 1 < slots.size();
            if (acc + bw + (more ? plusRoom : 0) > maxW) break;
            acc += bw;
        }
        keep = std::max<size_t>(1, keep);
        if (keep < slots.size()) {
            size_t rest = slots.size() - keep;
            g_compressed = true;
            out.erase(out.begin() + slots[keep].icon, out.end());
            slots.resize(keep);
            out[laid(keep - 1)].gapAfter = Px(6);
            Piece plus{ RenderMask(L"+" + std::to_wstring(rest), text, g_light), fg, 255, 0 };
            out.push_back(std::move(plus));
            width = measure();
        }
    }
    // Row layout: spread the devices over the whole gap so the strip fills it instead of hugging the tray.
    if (fillW > 0 && slots.size() > 1) {
        int natural = measure();
        if (fillW > natural) {
            int add = (fillW - natural) / (int)(slots.size() - 1);
            for (size_t b = 0; b + 1 < slots.size(); b++) out[laid(b)].gapAfter += add;
            width = measure();
        }
    }
    return out;
}

// Draws the strip for one window: the primary one or a mirror on a secondary taskbar.
void PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size) {
    RECT bar;
    GetClientRect(taskbar, &bar);
    int W = 0, H = 0;
    auto pieces = Layout(bar.right - bar.left, bar.bottom - bar.top, W, H, g_maxW, g_fillW);
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    Rgb barColor = g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 };
    if (g_background) roundRect(Px(4), Px(6), barColor, 240);
    if (hover) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    int x = Px(8);
    Rgb track = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    std::vector<int> xs(pieces.size()), ys(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        const Piece& p = pieces[i];
        if (p.anchor < 0) {
            bool fixed = p.px != INT_MIN;  // stacked / vertical layout: Layout() placed it
            xs[i] = fixed ? p.px : x;
            ys[i] = fixed ? p.py : (H - p.m.h) / 2;
            if (p.haloR) cv.Draw(p.halo, xs[i] - p.haloR, ys[i] - p.haloR, p.color, p.haloAlpha * p.alpha / 255);
            if (p.barPct >= 0) {
                // The bar spans the text's width, like Synapse's battery bar; in the row layout text and bar
                // are centred together.
                int bh = std::max(2, Px(2)), gap = Px(3);
                if (!fixed) ys[i] = (H - (p.m.h + gap + bh)) / 2;
                int by = ys[i] + p.m.h + gap;
                int fill = std::max(p.barPct > 0 ? 1 : 0, (p.barPct * p.m.w + 50) / 100);
                for (int yy = by; yy < by + bh; yy++)
                    for (int xx = 0; xx < p.m.w; xx++)
                        cv.Blend(xs[i] + xx, yy, xx < fill ? p.barColor : track, xx < fill ? p.alpha : 45 * p.alpha / 255);
            }
            if (!fixed) x += p.m.w + p.gapAfter;
        } else {  // caption: one-pixel outline in the taskbar's colour so it reads over the icon's lines
            xs[i] = xs[p.anchor] + p.bx;
            ys[i] = ys[p.anchor] + p.by;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if (ox || oy) cv.Draw(p.m, xs[i] + ox, ys[i] + oy, barColor, 255);
        }
        cv.Draw(p.m, xs[i], ys[i], p.color, p.alpha);
    }

    // Remember where the drawn content is: everything outside it answers HTTRANSPARENT, so a stretched
    // strip never swallows clicks meant for the taskbar (a Start menu would otherwise stay open).
    int cl = W, ct = H, cr = 0, cb = 0;
    for (size_t i = 0; i < pieces.size(); i++) {
        cl = std::min(cl, xs[i]);
        ct = std::min(ct, ys[i]);
        cr = std::max(cr, xs[i] + pieces[i].m.w);
        cb = std::max(cb, ys[i] + pieces[i].m.h);
    }
    if (cr > cl) g_content = RECT{ cl, ct, cr, cb };
    else g_content = RECT{ 0, 0, W, H };

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Paint() {
    if (!g_wnd) return;
    PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
    g_lastW = -1;  // force a re-position with the new size
}

void PaintMirror(Mirror& m) {
    g_dpiBar = m.bar;
    m.dpi = Dpi();
    RECT b{};
    GetClientRect(m.bar, &b);
    int saved = g_maxW;
    g_maxW = std::max(Px(60), (int)b.right - Px(4));  // a mirror may never run past its own taskbar
    PaintStrip(m.wnd, m.bar, m.hover, m.size);
    g_maxW = saved;
    g_dpiBar = nullptr;
    m.lastLeft = INT_MIN;  // force a re-position with the new size
}

void HideMirror(Mirror& m) {
    if (m.lastLeft != INT_MIN || IsWindowVisible(m.wnd)) ShowWindow(m.wnd, SW_HIDE);
    m.lastLeft = INT_MIN;
}

// Places a mirror left of its taskbar's clock (and left of the Widgets button if present), or hides it
// when that spot is unknown or taken by task buttons: never guess and cover something.
void PositionMirror(Mirror& m) {
    RECT bar{};
    if (!GetWindowRect(m.bar, &bar) || (GetWindowLongW(m.bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)) { HideMirror(m); return; }
    g_dpiBar = m.bar;
    if (Dpi() != m.dpi || m.size.cy != bar.bottom - bar.top) PaintMirror(m), g_dpiBar = m.bar;
    int right = 0, minLeft = bar.left;
    bool known = false;
    // StartAllBack can put a full notification area on each secondary taskbar.
    // Use its left edge, as on the primary taskbar, to clear all of the tray icons.
    HWND tray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (tray && IsWindowVisible(tray) && GetWindowRect(tray, &tr) &&
        tr.left > bar.left && tr.right <= bar.right && tr.right > tr.left &&
        tr.bottom > bar.top && tr.top < bar.bottom) {
        right = tr.left;
        known = true;
    } else if (!IsXamlTaskbar(m.bar)) {  // Windows 10: the clock is a real child window (absent when it is turned off)
        HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
        RECT cr{};
        right = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? cr.left : bar.right - Px(8);
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
            RECT fr{};
            if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && fr.left < right && fr.right > fr.left)
                right = fr.left;
        }
        known = true;
    } else {
        AcquireSRWLockShared(&g_wbLock);
        for (const auto& s : g_spans)
            if (s.bar == m.bar && s.measured) {
                right = s.hasClock ? s.clockLeft : bar.right - Px(8);
                if (s.hasWidgets && s.widgets.left < right && s.widgets.right > right - m.size.cx - Px(2))
                    right = s.widgets.left;
                if (s.hasTasks) minLeft = s.tasksRight + Px(4);
                known = true;
            }
        ReleaseSRWLockShared(&g_wbLock);
    }
    DWORD offset = 0, sz = sizeof(offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"SecondaryTaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    int left = right - m.size.cx - Px(2);
    g_dpiBar = nullptr;
    if (!known || m.size.cx <= 0 || left < minLeft) { HideMirror(m); return; }
    POINT p{ left, bar.top };
    ScreenToClient(m.bar, &p);
    bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && p.x == m.lastLeft) return;
    m.lastLeft = p.x;
    SetWindowPos(m.wnd, HWND_TOP, p.x, 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AddTool(HWND w) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)w;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"BatteryHub" : g_tipText.c_str());
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void DestroyMirror(Mirror& m) {
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)m.wnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    if (m.wnd && IsWindow(m.wnd)) DestroyWindow(m.wnd);
    m.wnd = nullptr;
}

void SetMeasureMirrors(bool on) {
    AcquireSRWLockExclusive(&g_wbLock);
    bool changed = g_wbMirrors != on;
    g_wbMirrors = on;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (changed && on && g_wbWake) SetEvent(g_wbWake);  // measure the new taskbar now, not in 5 s
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        RECT r{};
        if (IsWindowVisible(t) && GetWindowRect(t, &r) && r.right - r.left > r.bottom - r.top) out.push_back(t);  // horizontal only
    }
    return out;
}

// Keeps one mirror per secondary taskbar: adds them for new monitors, removes them for gone ones.
// Called from Reposition() about once a second; with one monitor it is a single FindWindowEx call.
// A monitor's lasting identity: its device interface name, which survives reboots, Explorer restarts and
// Windows renumbering the displays (falls back to "\\.\DISPLAYn").
std::wstring MonitorId(HWND bar) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return dd.DeviceID;
    return mi.szDevice;
}

// "Display 3 (right)": the number Windows gives the display and where it sits next to the main one.
std::wstring MonitorLabel(HWND bar) {
    MONITORINFOEXW mi{}, pm{};
    mi.cbSize = pm.cbSize = sizeof(MONITORINFOEXW);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return T(L"Other monitor");
    std::wstring dev = mi.szDevice;
    size_t d = dev.find(L"DISPLAY");
    std::wstring label = d != std::wstring::npos ? TF(L"Display {number}", { { L"number", dev.substr(d + 7) } }) : T(L"Other monitor");
    POINT origin{ 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm)) {
        int dx = (mi.rcMonitor.left + mi.rcMonitor.right - pm.rcMonitor.left - pm.rcMonitor.right) / 2;
        int dy = (mi.rcMonitor.top + mi.rcMonitor.bottom - pm.rcMonitor.top - pm.rcMonitor.bottom) / 2;
        label += std::wstring(L" (") + T(abs(dx) >= abs(dy) ? (dx < 0 ? L"left" : L"right") : (dy < 0 ? L"above" : L"below")) + L")";
    }
    return label;
}

// Hides or shows the strip on the main taskbar (it keeps running, just out of sight).
void SetPrimaryHidden(bool hide) {
    if (hide == g_primaryHidden || !g_wnd) return;
    g_primaryHidden = hide;
    if (hide) {
        ShowWindow(g_wnd, SW_HIDE);
        if (g_rebar && IsWindow(g_rebar)) {  // Windows 10: give the task buttons their full width back
            RECT r;
            GetClientRect(g_taskbar, &r);
            PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
        }
    }
    g_lastW = -1;  // re-place (and show) on the next pass
}

void SyncMirrors() {
    std::vector<HWND> all = g_wnd ? SecondaryTaskbars() : std::vector<HWND>{}, bars;
    bool chosenFound = false;
    if (g_choice == L"all") {
        bars = all;
    } else if (g_choice != L"main") {
        for (HWND b : all)
            if (MonitorId(b) == g_choice) {
                bars.push_back(b);
                chosenFound = true;
                break;
            }
    }
    SetPrimaryHidden(chosenFound);
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (std::find(bars.begin(), bars.end(), it->bar) == bars.end() || !IsWindow(it->wnd)) {
            DestroyMirror(*it);
            it = g_mirrors.erase(it);
        } else {
            ++it;
        }
    }
    for (HWND b : bars) {
        if (std::any_of(g_mirrors.begin(), g_mirrors.end(), [b](const Mirror& m) { return m.bar == b; })) continue;
        Mirror m;
        m.bar = b;
        m.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, b, nullptr,
                                g_inst, nullptr);
        if (!m.wnd) continue;
        AddTool(m.wnd);
        g_mirrors.push_back(m);
        PaintMirror(g_mirrors.back());
    }
    SetMeasureMirrors(!g_mirrors.empty());
    for (auto& m : g_mirrors) PositionMirror(m);
}

void PaintAll() {
    Paint();
    for (auto& m : g_mirrors) PaintMirror(m);
}

// Hover highlight for the strip under the mouse: the primary one or a mirror.
void SetHover(HWND h, bool on) {
    if (h == g_wnd) {
        if (g_hover == on) return;
        g_hover = on;
        Paint();
        widget::Reposition();
        return;
    }
    for (auto& m : g_mirrors)
        if (m.wnd == h && m.hover != on) {
            m.hover = on;
            PaintMirror(m);
            PositionMirror(m);
        }
}

// ---------------------------------------------------------------- flyout panel
// Shown when the strip had to leave devices out: a taskbar-style flyout above the strip listing every
// device with its level and state. Expands upward with a short ease-out animation, closes on a second
// click on the strip, on Esc, or when it loses activation (click anywhere else).
HWND g_panel;
int g_panelProg;        // 0 (hidden) .. 100 (fully open)
bool g_panelClosing;
int g_panelW, g_panelH, g_panelX, g_panelBottom;
const UINT_PTR kPanelTimer = 3;
const int kPanelTicks = 26;  // ~260 ms at 10 ms per tick

std::wstring PanelState(const WidgetItem& it) {
    if (it.text == L"\u00D7") return T(L"Not connected");
    if (it.charging) return T(L"Charging");
    if (it.full) return T(L"Fully charged");
    if (it.dim) return T(L"Asleep");
    if (it.percent < 0) return T(L"Unknown");
    return T(L"In use");
}

// Trims `s` with an ellipsis until it fits `maxW` at font `f`.
std::wstring FitText(const std::wstring& s, HFONT f, int maxW) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, f);
    auto width = [&](const std::wstring& t) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        return (int)sz.cx;
    };
    std::wstring out = s;
    while (!out.empty() && width(out) > maxW) out.pop_back();
    if (out.size() != s.size()) {
        while (!out.empty() && width(out + L"\u2026") > maxW) out.pop_back();
        out += L"\u2026";
    }
    SelectObject(dc, of);
    DeleteDC(dc);
    return out;
}

// Paints the whole panel into an ARGB canvas, revealed upward from the bottom: everything above
// `revealedH` stays fully transparent, the top edge is rounded while it grows.
void PanelRender(Canvas& cv, int revealedH) {
    Rgb bg = g_light ? Rgb{ 249, 249, 249 } : Rgb{ 43, 43, 43 };
    Rgb fg = g_light ? Rgb{ 27, 27, 27 } : Rgb{ 255, 255, 255 };
    Rgb sub = g_light ? Rgb{ 96, 94, 90 } : Rgb{ 176, 174, 168 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    int y0 = std::clamp(cv.h - revealedH, 0, cv.h);
    int r = Px(12);
    for (int y = y0; y < cv.h; y++)
        for (int x = 0; x < cv.w; x++) {
            int dx = x < r ? r - x : (x >= cv.w - r ? x - (cv.w - r - 1) : 0);
            int dy = y < y0 + r ? y0 + r - y : (y >= cv.h - r ? y - (cv.h - r - 1) : 0);
            if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, bg, 255);
        }
    auto draw = [&](const Mask& m, int x, int y, Rgb c, int a) {
        for (int j = 0; j < m.h; j++) {
            int yy = y + j;
            if (yy < y0 || yy >= cv.h) continue;
            for (int i = 0; i < m.w; i++) cv.Blend(x + i, yy, c, m.a[(size_t)j * m.w + i] * a / 255);
        }
    };
    int pad = Px(14), rowH = Px(42), y = pad;
    HFONT fName = MakeFont(Px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI", Px(12));
    HFONT fSub = MakeFont(Px(11), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT fIcon = MakeFont(Px(15), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT fPct = MakeFont(Px(14), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI", Px(12));
    int nameW = cv.w - pad - Px(26) - pad - Px(52);
    if (g_items.empty()) {
        std::wstring none = T(L"No devices with battery info found");
        draw(RenderMask(FitText(none, fName, nameW), fName, g_light), pad, y + Px(4), sub, 255);
    } else
    for (const auto& it : g_items) {
        BrandColor bc = AccentColor(it.accent, g_light);
        draw(RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), fIcon, g_light), pad, y + Px(2), Rgb{ bc.r, bc.g, bc.b },
             255);
        draw(RenderMask(FitText(it.name, fName, nameW), fName, g_light), pad + Px(26), y - Px(4), fg, 255);
        draw(RenderMask(FitText(PanelState(it), fSub, nameW), fSub, g_light), pad + Px(26), y + Px(14), sub, 255);
        std::wstring pct = !it.text.empty() ? it.text : (it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"");
        int pw = RenderMask(pct, fPct, g_light).w;
        draw(RenderMask(pct, fPct, g_light), cv.w - pad - pw, y - Px(2), it.low ? red : fg, 255);
        y += rowH;
    }
    DeleteObject(fName);
    DeleteObject(fSub);
    DeleteObject(fIcon);
    DeleteObject(fPct);
}

// Pushes one frame. A layered window takes the whole bitmap in one call, so the motion never tears.
void PanelPush(int revealedH, int alpha) {
    if (!g_panel || !IsWindow(g_panel)) return;
    Canvas cv(g_panelW, g_panelH);
    PanelRender(cv, revealedH);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), g_panelW, -g_panelH, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    memcpy(bits, cv.px.data(), (size_t)g_panelW * g_panelH * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    SIZE sz{ g_panelW, g_panelH };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_panel, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// One animation frame. Ease-out cubic on both the reveal and the fade, so it starts brisk and settles.
void PanelApplyFrame() {
    if (!g_panel || !IsWindow(g_panel)) return;
    int t = std::clamp(g_panelProg, 0, 100);
    int rest = 100 - t;
    int eased = 100 - rest * rest * rest / 10000;
    int h = std::max(Px(2), g_panelH * eased / 100);
    int a = std::min(255, t * 255 / 55);
    PanelPush(h, a);
}

void PanelHide() {
    if (!g_panel || !IsWindow(g_panel) || g_panelClosing) return;
    g_panelClosing = true;
    SetTimer(g_panel, kPanelTimer, 10, nullptr);
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {  // every frame is pushed with UpdateLayeredWindow; nothing to paint here
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp != kPanelTimer) break;
            g_panelProg += g_panelClosing ? -(100 / kPanelTicks + 1) : (100 / kPanelTicks + 1);
            if (g_panelProg >= 100) g_panelProg = 100;
            if (g_panelProg <= 0) {
                g_panelProg = 0;
                KillTimer(h, kPanelTimer);
                g_panelClosing = false;
                g_panel = nullptr;  // clear first: DestroyWindow sends WM_CAPTURECHANGED back to us
                DestroyWindow(h);
                return 0;
            }
            PanelApplyFrame();
            if (g_panelProg >= 100) KillTimer(h, kPanelTimer);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) PanelHide();  // click anywhere else
            return 0;
        case WM_LBUTTONDOWN: {  // mouse capture is held while open: a press outside the panel closes it
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT rc{};
            GetClientRect(h, &rc);
            if (!PtInRect(&rc, pt)) {
                PanelHide();
                ReleaseCapture();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            PanelHide();  // any other window took the mouse: behave like a click outside
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { PanelHide(); return 0; }
            break;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void PanelShow() {
    if (!g_inst) return;
    RECT bar{}, wnd{};
    if (!GetWindowRect(g_taskbar, &bar) || !g_wnd || !GetWindowRect(g_wnd, &wnd)) return;
    int rows = (int)std::max<size_t>(1, g_items.size());
    g_panelW = Px(316);
    g_panelH = Px(14) * 2 + Px(42) * rows;
    g_panelH = std::min(g_panelH, (int)bar.top - Px(24));
    g_panelX = std::max(Px(8), (int)wnd.right - g_panelW);
    g_panelBottom = (int)bar.top - Px(6);
    if (!g_panel) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = PanelProc;
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.lpszClassName = L"WirelessBatteryPanel";
            wc.hbrBackground = nullptr;  // nothing is ever painted by the class: no white flash before the first frame
            registered = RegisterClassExW(&wc) != 0;
        }
        // Created at its final size and already topmost / non-activating, so showing it never resizes or
        // activates anything — that is what made the windows underneath flicker for a frame.
        g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                  L"WirelessBatteryPanel", L"BatteryHub", WS_POPUP, g_panelX, g_panelBottom - g_panelH,
                                  g_panelW, g_panelH, nullptr, nullptr, g_inst, nullptr);
        if (!g_panel) return;
        // The first bitmap has to be in place before the window becomes visible, otherwise it flashes empty.
        g_panelProg = 0;
        g_panelClosing = false;
        PanelApplyFrame();
        ShowWindow(g_panel, SW_SHOWNOACTIVATE);
    } else {
        g_panelProg = 0;
        g_panelClosing = false;
        SetWindowPos(g_panel, HWND_TOPMOST, g_panelX, g_panelBottom - g_panelH, g_panelW, g_panelH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PanelApplyFrame();
    }
    SetCapture(g_panel);  // no focus stealing; the first press anywhere else closes the panel
    SetTimer(g_panel, kPanelTimer, 10, nullptr);
}

void PanelDestroy() {
    if (g_panel) {
        HWND p = g_panel;
        g_panel = nullptr;
        if (GetCapture() == p) ReleaseCapture();
        KillTimer(p, kPanelTimer);
        DestroyWindow(p);
    }
    g_panelProg = 0;
    g_panelClosing = false;
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {    switch (msg) {
        case WM_NCHITTEST: {  // a compressed strip is tiny: the whole box opens the flyout
            if (g_compressed) return HTCLIENT;
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &pt);
            RECT r = g_content;
            r.left -= Px(3);
            r.top -= Px(3);
            r.right += Px(3);
            r.bottom += Px(3);
            return PtInRect(&r, pt) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEMOVE: {
            if (g_compressed) return 0;  // no hover plate on the compact strip: it only added a repaint per move
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            SetHover(h, true);
            return 0;
        }
        case WM_MOUSELEAVE:
            SetHover(h, false);
            return 0;
        case WM_LBUTTONUP:
            if (g_panel) { PanelHide(); return 0; }                       // second click closes
            if (g_compressed && !g_items.empty()) { PanelShow(); return 0; }  // compressed: show the rest
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    g_vertical = tb.bottom - tb.top > tb.right - tb.left;  // left / right taskbar: devices stacked in a column
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    g_inst = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(L"BatteryHub");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    if (!g_primaryHidden) ShowWindow(g_wnd, SW_SHOWNOACTIVATE);  // hidden when only another monitor is chosen
    return true;
}

void Destroy() {
    PanelDestroy();
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    for (auto& m : g_mirrors) DestroyMirror(m);
    g_mirrors.clear();
    SetMeasureMirrors(false);
    g_primaryHidden = false;  // the next Create() decides again
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hover = false;
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        ti.uId = (UINT_PTR)g_wnd;
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        for (const auto& m : g_mirrors) {
            ti.uId = (UINT_PTR)m.wnd;
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    PaintAll();
    Reposition();
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    PaintAll();
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

void SetStacked(bool on, bool user) {
    g_stacked = on;
    if (user) g_manualStack = true;  // an explicit choice wins over the automatic compaction
    PaintAll();
    if (g_wnd) Reposition();
}

void SetMonitorChoice(const std::wstring& choice) {
    g_choice = choice.empty() ? L"all" : choice;
    SyncMirrors();  // adds, removes or hides strips right away
    if (g_wnd) Reposition();
}

std::vector<MonitorOption> OtherMonitors() {
    std::vector<MonitorOption> out;
    for (HWND b : SecondaryTaskbars()) out.push_back({ MonitorId(b), MonitorLabel(b) });
    std::sort(out.begin(), out.end(), [](const MonitorOption& a, const MonitorOption& b) { return a.label < b.label; });
    return out;
}

bool Reposition() {
    if (!Alive()) return false;
    SyncMirrors();
    if (g_primaryHidden) return true;  // only another monitor's taskbar shows the strip
    if (Dpi() != g_lastDpi) { g_lastDpi = Dpi(); Paint(); }
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr, bar;
    GetWindowRect(tray, &tr);
    GetWindowRect(g_taskbar, &bar);
    // The taskbar can be moved between a horizontal and a vertical edge while the app runs.
    bool vertical = bar.bottom - bar.top > bar.right - bar.left;
    if (vertical != g_vertical) {
        g_vertical = vertical;
        Paint();
    }
    if (g_vertical) {
        // Left or right taskbar (PR #321): the column sits just above the notification area, centred in
        // the bar's width.
        RECT client{};
        GetClientRect(g_taskbar, &client);
        POINT anchor{ bar.left, tr.top };
        ScreenToClient(g_taskbar, &anchor);
        int top = anchor.y - g_size.cy - Px(2);
        DWORD offset = 0, osz = sizeof(offset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset,
                         &osz) == ERROR_SUCCESS)
            top -= Px((int)offset);
        top = std::clamp(top, 0, (int)std::max<LONG>(0, client.bottom - g_size.cy));
        int left = (int)std::max<LONG>(0, (client.right - g_size.cx) / 2);
        // Windows 10: shorten the task-button area when it would reach behind the column.
        if (g_rebar && IsWindow(g_rebar)) {
            RECT rr{};
            GetWindowRect(g_rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(g_taskbar, &tl);
            if (tl.y < top && tl.y + (rr.bottom - rr.top) > top)
                MoveWindow(g_rebar, tl.x, tl.y, rr.right - rr.left, top - tl.y, TRUE);
        }
        bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
        if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && g_lastW == g_size.cy && g_lastLeft == top)
            return true;
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cy;
        g_lastLeft = top;
        SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
    // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
    // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
    // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
    // width, so the widget follows it.
    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    // Step past the Widgets button when it sits where we would go.
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    // Free span between the last task button and the tray's overflow arrow, in taskbar client px.
    // Recomputed on every pass (1 s) from the values the UIA thread refreshes every 5 s, so it follows new
    // windows, a resized bar, and a tray that gains or loses icons.
    int spanL = 0, spanR = 0;
    bool haveSpan = false;
    if (!rtl) {
        int tasksRight = TasksRightEdge(g_taskbar);
        int chevron = ChevronLeftEdge(g_taskbar);
        // Whichever comes first wins: the measured arrow, or the tray window's own left edge. The UIA
        // value lags a few seconds behind tray changes, the window rect does not.
        int rightScreen = (int)tr.left;
        if (chevron > 0 && chevron < rightScreen) rightScreen = chevron;
        if (tasksRight > 0 && rightScreen > tasksRight) {
            POINT a{ tasksRight, 0 }, b{ rightScreen, 0 };
            ScreenToClient(g_taskbar, &a);
            ScreenToClient(g_taskbar, &b);
            int l = (int)a.x + Px(12), r = (int)b.x - Px(12);
            if (r - l >= Px(16)) { spanL = l; spanR = r; haveSpan = true; }  // Layout() degrades when tiny
        }
    }
    int maxW = haveSpan ? spanR - spanL : (rtl ? (int)(bar.right - bar.left) - Px(4) : (int)p.x - Px(4));
    // Inside a measured gap the strip also stretches to it, so the space is covered end to end.
    int fillW = haveSpan ? spanR - spanL : 0;
    if (!rtl) {
        // Opt-in tolerance, in DIPs, for people who would rather see every device than stay inside the gap.
        DWORD extra = 0;
        DWORD sz = sizeof(extra);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"WidgetExtraWidth", RRF_RT_REG_DWORD, nullptr, &extra, &sz) ==
            ERROR_SUCCESS) {
            maxW += Px((int)extra);
            fillW += Px((int)extra);
        }
    }
    maxW = std::max(Px(60), maxW);
    if (maxW != g_maxW || fillW != g_fillW) {
        g_maxW = maxW;
        g_fillW = fillW;
        Paint();  // re-lay out under the new cap; Paint() invalidates the cached size
    }
    // Left edge of the gap when one was measured; the content is spread across its whole width.
    int left = haveSpan ? spanL : p.x - g_size.cx - Px(2);
    if (!rtl && left < Px(2)) left = Px(2);


    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && EqualRect(&wb, &g_lastWidgets) &&
        g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

bool Compressed() {
    return g_compressed;
}

}  // namespace widget