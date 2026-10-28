#include "platform/Embed.h"
#include "core/Log.h"
#include "core/Types.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cwctype>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "platform/Platform.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kst::platform {

namespace {
double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

#ifdef _WIN32

namespace {

std::wstring wid(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string nar(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
std::wstring fileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return lowerW(p == std::wstring::npos ? path : path.substr(p + 1));
}
std::wstring exeNameOfPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out = fileNameOf(std::wstring(buf, n));
    CloseHandle(h);
    return out;
}

struct FindCtx {
    DWORD pid = 0;
    std::wstring exeName;
    HWND exclude1 = nullptr, exclude2 = nullptr;
    HWND best = nullptr;
    long long bestArea = 0;
};

BOOL CALLBACK findMainWindow(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (h == c->exclude1 || h == c->exclude2 || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    bool match = pid == c->pid;
    if (!match && !c->exeName.empty()) match = exeNameOfPid(pid) == c->exeName;
    if (!match) return TRUE;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if (!(st & WS_CAPTION) && !(st & WS_THICKFRAME)) return TRUE;   // splash screens and tool tips
    RECT r;
    GetWindowRect(h, &r);
    long long w = r.right - r.left, hh = r.bottom - r.top;
    if (w < 400 || hh < 300) return TRUE;
    if (w * hh > c->bestArea) { c->best = h; c->bestArea = w * hh; }
    return TRUE;
}

void moveOff(HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    if (r.left > -20000) SetWindowPos(h, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// While OrcaSlicer starts in the background its splash screen and early windows are moved off
// screen, so nothing pops up over Kestral. Its main window is taken in (and hidden) right after.
BOOL CALLBACK moveAwayStrays(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (h == c->exclude1 || h == c->exclude2 || !IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    bool match = pid == c->pid;
    if (!match && !c->exeName.empty()) match = exeNameOfPid(pid) == c->exeName;
    if (!match) return TRUE;
    moveOff(h);
    return TRUE;
}

// The splash shows (and re centers) between Kestral's frames, so a WinEvent hook catches it the
// moment it appears. Out of context hooks run on the main thread's message pump.
struct StrayHook {
    HWINEVENTHOOK hook = nullptr;
    DWORD pid = 0;
    HWND keep = nullptr;
    bool active = false;          // the slice tab is closed: nothing of OrcaSlicer may show
};
StrayHook g_stray;

void CALLBACK strayEvent(HWINEVENTHOOK, DWORD, HWND h, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_stray.active || !h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (h == g_stray.keep || GetAncestor(h, GA_ROOT) != h || !IsWindowVisible(h)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_stray.pid) return;
    moveOff(h);
}

void hookStrays(DWORD pid) {
    if (g_stray.hook) UnhookWinEvent(g_stray.hook);
    g_stray = StrayHook{};
    g_stray.pid = pid;
    g_stray.active = true;
    g_stray.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, strayEvent, pid, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void unhookStrays() {
    if (g_stray.hook) UnhookWinEvent(g_stray.hook);
    g_stray = StrayHook{};
}

struct Saved { HWND wnd = nullptr; LONG_PTR style = 0, ex = 0; RECT rect{}; };

void giveBack(Saved& s) {
    if (!s.wnd || !IsWindow(s.wnd)) { s = {}; return; }
    SetParent(s.wnd, nullptr);
    SetWindowLongPtrW(s.wnd, GWL_STYLE, s.style);
    SetWindowLongPtrW(s.wnd, GWL_EXSTYLE, s.ex);
    SetWindowPos(s.wnd, HWND_TOP, s.rect.left, s.rect.top, s.rect.right - s.rect.left, s.rect.bottom - s.rect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    s = {};
}

} // namespace

struct EmbeddedApp::Impl {
    HANDLE proc = nullptr;
    DWORD pid = 0;
    std::wstring exeName, exePath;
    Saved cur, retiring;
    HWND parent = nullptr;
    double t0 = 0;
    bool launching = false;
    bool background = false;   // started out of sight (Kestral just opened)
    bool visible = false;
    int x = 0, y = 0, w = 0, h = 0;
    bool placed = false;
    bool hot = false;          // the mouse is over the hosted window (activation already sent)

    void closeProc() {
        if (proc) CloseHandle(proc);
        proc = nullptr;
    }
    bool procAlive() const { return proc && WaitForSingleObject(proc, 0) == WAIT_TIMEOUT; }
    // Keyboard focus inside the hosted window goes back to Kestral (its window, its thread).
    void reclaimFocus() {
        if (!cur.wnd || !IsWindow(cur.wnd) || !parent) return;
        GUITHREADINFO gi{};
        gi.cbSize = sizeof gi;
        DWORD other = GetWindowThreadProcessId(cur.wnd, nullptr);
        if (!GetGUIThreadInfo(other, &gi)) return;
        HWND focus = gi.hwndFocus;
        if (!focus || !(focus == cur.wnd || IsChild(cur.wnd, focus))) return;
        DWORD self = GetCurrentThreadId();
        bool attached = other != self && AttachThreadInput(self, other, TRUE);
        SetFocus(parent);
        if (attached) AttachThreadInput(self, other, FALSE);
        hot = false;
    }
    void apply() {
        if (!cur.wnd || !IsWindow(cur.wnd)) return;
        if (visible && w > 0 && h > 0) {
            SetWindowPos(cur.wnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        } else {
            // Hiding a window that holds the keyboard leaves Kestral deaf to typing (the assistant
            // box stopped taking keys after a visit to the Slice tab): take the focus back first.
            reclaimFocus();
            ShowWindow(cur.wnd, SW_HIDE);
        }
    }
    void adopt(HWND wnd) {
        Saved s;
        s.wnd = wnd;
        if (IsZoomed(wnd) || IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
        s.style = GetWindowLongPtrW(wnd, GWL_STYLE);
        s.ex = GetWindowLongPtrW(wnd, GWL_EXSTYLE);
        GetWindowRect(wnd, &s.rect);
        // The host must not paint over its children.
        SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
        SetParent(wnd, parent);
        LONG_PTR st = (s.style & ~(LONG_PTR)(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_CHILD;
        SetWindowLongPtrW(wnd, GWL_STYLE, st);
        SetWindowLongPtrW(wnd, GWL_EXSTYLE, s.ex & ~(LONG_PTR)(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME));
        SetWindowPos(wnd, HWND_TOP, x, y, std::max(w, 1), std::max(h, 1), SWP_FRAMECHANGED | SWP_NOACTIVATE);
        cur = s;
        apply();
        log::info("Hosting the slicer window inside Kestral");
    }
};

EmbeddedApp::EmbeddedApp() : d_(new Impl) {}
EmbeddedApp::~EmbeddedApp() {
    unhookStrays();
    release();
    d_->closeProc();
    delete d_;
}
bool EmbeddedApp::supported() { return true; }

bool EmbeddedApp::start(const std::string& exe, const std::string& file, std::string& error, bool background) {
    std::wstring wexe = wid(exe);
    std::wstring cmd = L"\"" + wexe + L"\"";
    if (!file.empty()) cmd += L" \"" + wid(file) + L"\"";
    std::wstring dir = wid(pathToU8(u8path(exe).parent_path()));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (background) {
        // Start out of sight; Kestral takes the window in and keeps it hidden until its tab opens.
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINNOACTIVE;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        error = "Could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    d_->closeProc();
    d_->proc = pi.hProcess;
    d_->pid = pi.dwProcessId;
    d_->exeName = fileNameOf(wexe);
    d_->exePath = wexe;
    d_->t0 = nowSec();
    d_->launching = true;
    d_->background = background;
    if (background) hookStrays(pi.dwProcessId);
    else unhookStrays();
    if (d_->cur.wnd) {
        // Keep showing the current one until the new window is up.
        d_->retiring = d_->cur;
    }
    return true;
}

void EmbeddedApp::poll(void* parentWindow) {
    Impl& d = *d_;
    d.parent = (HWND)parentWindow;
    if (d.cur.wnd && !IsWindow(d.cur.wnd)) {
        if (d.retiring.wnd == d.cur.wnd) d.retiring = {};
        d.cur = {};   // closed by the user
    }
    double age = nowSec() - d.t0;
    FindCtx c;
    c.pid = d.pid;
    c.exeName = d.exeName;
    c.exclude1 = d.cur.wnd;
    c.exclude2 = d.retiring.wnd;
    // The splash outlives the main window showing up: keep it off screen for the whole start.
    if (d.background && g_stray.hook) {
        g_stray.keep = d.cur.wnd;
        g_stray.active = !d.visible;
        if (!d.visible) EnumWindows(moveAwayStrays, reinterpret_cast<LPARAM>(&c));
        if (age > 90.0) unhookStrays();
    }
    if (!d.launching || !d.parent) return;
    EnumWindows(findMainWindow, reinterpret_cast<LPARAM>(&c));
    if (c.best) {
        d.launching = false;
        Saved old = d.retiring;
        if (old.wnd) {
            // A second window opened for the new file: host it, hand the old one back and ask it to close.
            d.retiring = {};
            d.cur = {};
            giveBack(old);
            PostMessageW(old.wnd, WM_CLOSE, 0, 0);
        }
        d.adopt(c.best);
        return;
    }
    // Single instance programs pass the file to the running copy and quit: keep what we host.
    if (!d.procAlive() && age > 4.0 && d.cur.wnd) {
        d.launching = false;
        d.retiring = {};
        return;
    }
    if (age > 90.0) {
        d.launching = false;
        d.retiring = {};
        log::warn("The slicer window never appeared");
    }
}

void EmbeddedApp::place(int x, int y, int w, int h, bool visible) {
    Impl& d = *d_;
    bool changed = !d.placed || x != d.x || y != d.y || w != d.w || h != d.h || visible != d.visible;
    d.x = x; d.y = y; d.w = w; d.h = h; d.visible = visible;
    d.placed = true;
    if (changed) d.apply();
}

void EmbeddedApp::followMouse() {
    Impl& d = *d_;
    if (!d.parent || !d.cur.wnd || !IsWindow(d.cur.wnd)) return;
    if (!d.visible) { d.reclaimFocus(); return; }
    if (GetForegroundWindow() != d.parent) return;   // another program, or an Orca dialog / menu is up
    if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0) return;
    GUITHREADINFO gi{};
    gi.cbSize = sizeof gi;
    if (!GetGUIThreadInfo(0, &gi)) return;
    if (gi.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE | GUI_SYSTEMMENUMODE)) return;
    HWND focus = gi.hwndFocus;
    bool focusInside = focus && (focus == d.cur.wnd || IsChild(d.cur.wnd, focus));
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    RECT r;
    GetWindowRect(d.cur.wnd, &r);
    bool over = PtInRect(&r, pt) != 0;
    // A hosted wxWidgets window (Orca) ignores keys, the wheel and several tools until its frame
    // believes it is the active app, which a child window never becomes on its own. Tell it once
    // each time the mouse comes in. Every frame was too much: wx answers WM_ACTIVATE by moving the
    // focus back to its last focused control, and a canvas that loses focus mid click drops the
    // drag (the layer slider only jumped to clicks).
    if (over && !d.hot) {
        SendNotifyMessageW(d.cur.wnd, WM_ACTIVATEAPP, TRUE, 0);
        SendNotifyMessageW(d.cur.wnd, WM_NCACTIVATE, TRUE, 0);
        SendNotifyMessageW(d.cur.wnd, WM_ACTIVATE, WA_CLICKACTIVE, (LPARAM)d.parent);
    }
    d.hot = over;
    HWND target = nullptr;
    if (over && !focusInside) {
        HWND under = WindowFromPoint(pt);
        target = under && (under == d.cur.wnd || IsChild(d.cur.wnd, under)) ? under : d.cur.wnd;
    } else if (!over && focusInside) {
        HWND under = WindowFromPoint(pt);
        if (under == d.parent) target = d.parent;
    }
    if (!target) return;
    DWORD self = GetCurrentThreadId(), other = GetWindowThreadProcessId(d.cur.wnd, nullptr);
    bool attached = other != self && AttachThreadInput(self, other, TRUE);
    SetFocus(target);
    if (attached) AttachThreadInput(self, other, FALSE);
}

bool EmbeddedApp::embedded() const { return d_->cur.wnd && IsWindow(d_->cur.wnd); }
bool EmbeddedApp::hostsWindow() { return true; }
bool EmbeddedApp::starting() const { return d_->launching; }
bool EmbeddedApp::running() const { return embedded() || d_->launching; }
double EmbeddedApp::secondsSinceStart() const { return nowSec() - d_->t0; }

void EmbeddedApp::release() {
    giveBack(d_->retiring);
    giveBack(d_->cur);
    d_->launching = false;
}

void EmbeddedApp::closeApp() {
    Saved cur = d_->cur;
    giveBack(d_->retiring);
    d_->cur = {};
    d_->launching = false;
    if (cur.wnd && IsWindow(cur.wnd)) {
        SetParent(cur.wnd, nullptr);
        PostMessageW(cur.wnd, WM_CLOSE, 0, 0);
    }
}

bool EmbeddedApp::openFile(const std::string& file) {
    // OrcaSlicer (like PrusaSlicer) takes files from a second copy of itself as WM_COPYDATA:
    // id 1, the command line as ';' separated, C style quoted strings.
    if (!embedded()) return false;
    auto quote = [](const std::wstring& s) {
        std::wstring o = L"\"";
        for (wchar_t c : s) {
            if (c == L'\\' || c == L'"') o += L'\\';
            o += c;
        }
        return o + L"\"";
    };
    std::wstring msg = quote(d_->exePath) + L";" + quote(wid(file));
    COPYDATASTRUCT cds{};
    cds.dwData = 1;
    cds.cbData = (DWORD)((msg.size() + 1) * sizeof(wchar_t));
    cds.lpData = (PVOID)msg.c_str();
    DWORD_PTR res = 0;
    return SendMessageTimeoutW(d_->cur.wnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &res) != 0;
}


unsigned long EmbeddedApp::processId() const {
    if (!embedded()) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(d_->cur.wnd, &pid);
    return pid;
}
void* EmbeddedApp::window() const { return embedded() ? (void*)d_->cur.wnd : nullptr; }

namespace {
struct ButtonHunt {
    DWORD pid = 0;
    std::wstring want;
    HWND found = nullptr;
    bool enabled = false;
    std::wstring seen;
};
void huntIn(HWND w, ButtonHunt& h) {
    wchar_t buf[256];
    int n = GetWindowTextW(w, buf, 256);
    if (n > 0) {
        std::wstring t(buf, (size_t)n);
        if (h.seen.size() < 2000 && IsWindowVisible(w)) h.seen += L"[" + t + L"] ";
        if (lowerW(t) == lowerW(h.want) && IsWindowVisible(w)) {
            RECT r;
            GetWindowRect(w, &r);
            if (r.right - r.left > 4 && r.bottom - r.top > 4 && (!h.found || (IsWindowEnabled(w) && !h.enabled))) {
                h.found = w;
                h.enabled = IsWindowEnabled(w) != 0;
            }
        }
    }
}
BOOL CALLBACK huntChild(HWND w, LPARAM lp) {
    huntIn(w, *reinterpret_cast<ButtonHunt*>(lp));
    return TRUE;
}
BOOL CALLBACK huntTop(HWND w, LPARAM lp) {
    auto* h = reinterpret_cast<ButtonHunt*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != h->pid || !IsWindowVisible(w)) return TRUE;
    huntIn(w, *h);
    EnumChildWindows(w, huntChild, lp);
    return TRUE;
}
} // namespace

bool clickAppButton(unsigned long pid, void* root, const std::string& label, int timeoutMs, const std::atomic<bool>* cancel,
                    std::string& seen) {
    double t0 = nowSec();
    ButtonHunt h;
    for (;;) {
        h = ButtonHunt{};
        h.pid = pid;
        h.want = wid(label);
        if (root && IsWindow((HWND)root)) {
            huntIn((HWND)root, h);
            EnumChildWindows((HWND)root, huntChild, (LPARAM)&h);
        }
        if (pid) EnumWindows(huntTop, (LPARAM)&h);
        if (h.found && h.enabled) break;
        if ((cancel && cancel->load()) || (nowSec() - t0) * 1000 > timeoutMs) {
            seen = nar(h.seen);
            return false;
        }
        Sleep(250);
    }
    // A plain mouse click in the middle left of the button (left of any drop down arrow).
    RECT r;
    GetClientRect(h.found, &r);
    LPARAM at = MAKELPARAM((r.right - r.left) * 2 / 5, (r.bottom - r.top) / 2);
    PostMessageW(h.found, WM_MOUSEMOVE, 0, at);
    PostMessageW(h.found, WM_LBUTTONDOWN, MK_LBUTTON, at);
    Sleep(60);
    PostMessageW(h.found, WM_LBUTTONUP, 0, at);
    seen = nar(h.seen);
    return true;
}

bool sendAppShortcut(void* window, bool ctrl, bool shift, bool alt, int key) {
    HWND w = (HWND)window;
    if (!w || !IsWindow(w)) return false;
    DWORD target = GetWindowThreadProcessId(w, nullptr), self = GetCurrentThreadId();
    bool attached = target != self && AttachThreadInput(self, target, TRUE);
    SetFocus(w);
    std::vector<INPUT> in;
    auto key1 = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    if (ctrl) key1(VK_CONTROL, false);
    if (shift) key1(VK_SHIFT, false);
    if (alt) key1(VK_MENU, false);
    key1((WORD)key, false);
    key1((WORD)key, true);
    if (alt) key1(VK_MENU, true);
    if (shift) key1(VK_SHIFT, true);
    if (ctrl) key1(VK_CONTROL, true);
    UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    if (attached) AttachThreadInput(self, target, FALSE);
    return sent == in.size();
}

std::string findOrcaSlicer() {
    // 1. Installed programs list (both registry views, machine and user).
    struct Root { HKEY key; const wchar_t* path; };
    const Root roots[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };
    std::error_code ec;
    for (auto& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(r.key, r.path, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD nl = 256;
            if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY sk;
            if (RegOpenKeyExW(k, name, 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;
            auto readStr = [&](const wchar_t* v) {
                wchar_t buf[1024];
                DWORD sz = sizeof buf, type = 0;
                if (RegQueryValueExW(sk, v, nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
                    return std::wstring();
                return std::wstring(buf, wcsnlen(buf, sz / sizeof(wchar_t)));
            };
            std::wstring dn = lowerW(readStr(L"DisplayName"));
            if (dn.find(L"orca") != std::wstring::npos && dn.find(L"slicer") != std::wstring::npos) {
                std::wstring loc = readStr(L"InstallLocation");
                std::wstring icon = readStr(L"DisplayIcon");
                std::vector<std::wstring> cands;
                if (!loc.empty()) cands.push_back(loc + L"\\orca-slicer.exe");
                if (!icon.empty()) {
                    size_t comma = icon.find(L',');
                    std::wstring ic = icon.substr(0, comma);
                    if (!ic.empty() && ic.front() == L'"') ic = ic.substr(1, ic.size() - 2);
                    cands.push_back(ic);
                }
                for (auto& c : cands) {
                    std::string u = nar(c);
                    if (toLower(u).size() > 4 && toLower(u).rfind(".exe") == u.size() - 4 && std::filesystem::exists(u8path(u), ec)) {
                        RegCloseKey(sk);
                        RegCloseKey(k);
                        return u;
                    }
                }
            }
            RegCloseKey(sk);
        }
        RegCloseKey(k);
    }
    // 2. Usual folders.
    const wchar_t* vars[] = {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"};
    for (auto v : vars) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(v, buf, MAX_PATH);
        if (!n) continue;
        std::string base = nar(std::wstring(buf, n));
        for (const char* rel : {"\\OrcaSlicer\\orca-slicer.exe", "\\Programs\\OrcaSlicer\\orca-slicer.exe"}) {
            std::string p = base + rel;
            if (std::filesystem::exists(u8path(p), ec)) return p;
        }
    }
    return {};
}

#else  // Linux: OrcaSlicer runs as its own window (desktops there do not allow hosting it).

namespace {
pid_t spawnApp(const std::string& exe, const std::string& file, std::string& error) {
    std::vector<std::string> args;
    if (exe.rfind("flatpak:", 0) == 0) {
        args = {"flatpak", "run", "--file-forwarding", exe.substr(8)};
        if (!file.empty()) { args.push_back("@@"); args.push_back(file); args.push_back("@@"); }
    } else {
        args = {exe};
        if (!file.empty()) args.push_back(file);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (rc != 0) { error = "Could not start " + exe + " (error " + std::to_string(rc) + ")"; return 0; }
    return pid;
}
} // namespace

struct EmbeddedApp::Impl { double t0 = 0; pid_t pid = 0; std::string exe; std::vector<pid_t> extra; };
EmbeddedApp::EmbeddedApp() : d_(new Impl) {}
EmbeddedApp::~EmbeddedApp() { delete d_; }
bool EmbeddedApp::supported() { return true; }
bool EmbeddedApp::hostsWindow() { return false; }
bool EmbeddedApp::start(const std::string& exe, const std::string& file, std::string& error, bool) {
    pid_t pid = spawnApp(exe, file, error);
    if (!pid) return false;
    d_->pid = pid;
    d_->exe = exe;
    d_->t0 = nowSec();
    return true;
}
bool EmbeddedApp::openFile(const std::string& file) {
    // OrcaSlicer passes the file to its open window when "single instance" is on in its preferences;
    // otherwise this opens a second window with the file.
    if (d_->exe.empty()) return false;
    std::string err;
    pid_t pid = spawnApp(d_->exe, file, err);
    if (pid) d_->extra.push_back(pid);
    return pid != 0;
}
void EmbeddedApp::closeApp() {}
void EmbeddedApp::poll(void*) {
    // Reap finished processes so none linger as zombies.
    int st;
    if (d_->pid && waitpid(d_->pid, &st, WNOHANG) == d_->pid) d_->pid = 0;
    d_->extra.erase(std::remove_if(d_->extra.begin(), d_->extra.end(), [&](pid_t p) { return waitpid(p, &st, WNOHANG) == p; }),
                    d_->extra.end());
}
void EmbeddedApp::place(int, int, int, int, bool) {}
void EmbeddedApp::followMouse() {}
bool EmbeddedApp::embedded() const { return false; }
bool EmbeddedApp::starting() const { return false; }
bool EmbeddedApp::running() const {
    if (!d_->pid) return false;
    int st;
    if (waitpid(d_->pid, &st, WNOHANG) == d_->pid) { d_->pid = 0; return false; }
    return true;
}
void EmbeddedApp::release() {}
double EmbeddedApp::secondsSinceStart() const { return nowSec() - d_->t0; }
unsigned long EmbeddedApp::processId() const { return 0; }
void* EmbeddedApp::window() const { return nullptr; }
bool clickAppButton(unsigned long, void*, const std::string&, int, const std::atomic<bool>*, std::string&) { return false; }
bool sendAppShortcut(void*, bool, bool, bool, int) { return false; }
std::string findOrcaSlicer() {
    for (auto& s : detectSlicers())
        if (toLower(s.name).find("orca") != std::string::npos) return s.path;
    return {};
}

#endif

} // namespace kst::platform
