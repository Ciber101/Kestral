#include "platform/Platform.h"
#include "platform/Embed.h"
#include "core/Log.h"
#include "core/Types.h"
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <dbghelp.h>
#include <eh.h>
#include <malloc.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wincrypt.h>
#else
#include <algorithm>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <atomic>
#include <cstdlib>

namespace kst::platform {

namespace {
std::atomic<int> g_nativeDialogs{0};
struct DialogScope {
    DialogScope() { g_nativeDialogs++; }
    ~DialogScope() { g_nativeDialogs--; }
};
} // namespace

bool inNativeDialog() { return g_nativeDialogs.load() > 0; }

namespace {
std::string b64enc(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(t[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
std::string b64dec(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}
} // namespace

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct SehException : std::runtime_error {
    explicit SehException(unsigned code) : std::runtime_error(describe(code)) {}
    static std::string describe(unsigned code) {
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation inside geometry code";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "invalid float operation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        default: {
            char buf[64];
            snprintf(buf, sizeof buf, "hardware exception 0x%08X", code);
            return buf;
        }
        }
    }
};

void seTranslator(unsigned int code, EXCEPTION_POINTERS*) {
    if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
    throw SehException(code);
}

std::function<void()> g_emergencySave;
volatile LONG g_inCrash = 0;

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inCrash, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;
    std::string dir = appDataDir() + "\\crash";
    CreateDirectoryW(widen(appDataDir()).c_str(), nullptr);
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    // Minidump for debugging.
    std::wstring dumpPath = widen(dir + "\\kestral_crash.dmp");
    HANDLE f = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }
    try { if (g_emergencySave) g_emergencySave(); } catch (...) {}
    MessageBoxW(nullptr,
        L"Kestral hit a fatal internal error.\n\nYour work was saved to the recovery folder and a crash report was written to\n%APPDATA%\\Kestral\\crash.",
        L"Kestral", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installProcessCrashGuard(std::function<void()> emergencySave) {
    g_emergencySave = std::move(emergencySave);
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate([] {
        log::error("std::terminate called");
        unhandledFilter(nullptr);
        std::abort();
    });
    // Keep floating point exceptions masked (default), so bad math gives NaN instead of a fault.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

void installThreadGuard() { _set_se_translator(seTranslator); }

std::string appDataDir() {
    PWSTR p = nullptr;
    std::string out = ".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        std::string base = narrow(p);
        out = base + "\\Kestral";
        CoTaskMemFree(p);
        // Renamed from Kestrel: carry settings, shortcuts and recovery files over once.
        static bool migrated = false;
        if (!migrated) {
            migrated = true;
            std::wstring oldW = widen(base + "\\Kestrel"), newW = widen(out);
            if (GetFileAttributesW(newW.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(oldW.c_str()) != INVALID_FILE_ATTRIBUTES)
                MoveFileW(oldW.c_str(), newW.c_str());
        }
    }
    CreateDirectoryW(widen(out).c_str(), nullptr);
    return out;
}

std::string tempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::string out = narrow(std::wstring(buf, n)) + "Kestral";
    CreateDirectoryW(widen(out).c_str(), nullptr);
    return out;
}

std::string fileVersion(const std::string& path) {
    // version.dll loaded on demand (no extra link library).
    HMODULE v = LoadLibraryW(L"version.dll");
    if (!v) return {};
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto sizeFn = (SizeFn)(void*)GetProcAddress(v, "GetFileVersionInfoSizeW");
    auto infoFn = (InfoFn)(void*)GetProcAddress(v, "GetFileVersionInfoW");
    auto queryFn = (QueryFn)(void*)GetProcAddress(v, "VerQueryValueW");
    std::string out;
    if (sizeFn && infoFn && queryFn) {
        std::wstring w = widen(path);
        DWORD dummy = 0, n = sizeFn(w.c_str(), &dummy);
        std::vector<char> buf(n ? n : 1);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (n && infoFn(w.c_str(), 0, n, buf.data()) && queryFn(buf.data(), L"\\", (LPVOID*)&fi, &len) && fi && len) {
            out = std::to_string(HIWORD(fi->dwFileVersionMS)) + "." + std::to_string(LOWORD(fi->dwFileVersionMS)) + "." +
                  std::to_string(HIWORD(fi->dwFileVersionLS));
        }
    }
    FreeLibrary(v);
    return out;
}

std::string selfExe() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n == 0 || n >= buf.size()) return {};
    return narrow(std::wstring(buf.data(), n));
}

namespace {
std::wstring makeFilter(const char* desc, const char* patterns) {
    std::wstring f = widen(desc);
    f.push_back(L'\0');
    f += widen(patterns);
    f.push_back(L'\0');
    f += L"All files";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}
}

std::string legacyDesignsDir() {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) {
        std::string base = narrow(p);
        out = base + "\\Kestral Designs";
        CoTaskMemFree(p);
        std::wstring oldW = widen(base + "\\Kestrel Designs"), newW = widen(out);
        if (GetFileAttributesW(newW.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(oldW.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(oldW.c_str(), newW.c_str());
    } else {
        out = appDataDir() + "\\Designs";
    }
    return out;
}

std::string openFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const std::string& initialDir) {
    DialogScope ds;
    std::wstring wdir = widen(initialDir);
    wchar_t file[4096] = L"";
    std::wstring filter = makeFilter(filterDesc, filterPatterns);
    std::wstring wtitle = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 4096;
    ofn.lpstrTitle = wtitle.c_str();
    if (!wdir.empty()) ofn.lpstrInitialDir = wdir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return narrow(file);
}

namespace {
int CALLBACK folderInit(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}
} // namespace

std::string pickFolderDialog(const char* title, const std::string& initialDir) {
    DialogScope ds;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring wtitle = widen(title), wdir = widen(initialDir);
    BROWSEINFOW bi{};
    bi.hwndOwner = GetActiveWindow();
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = folderInit;
    bi.lParam = wdir.empty() ? 0 : (LPARAM)wdir.c_str();
    std::string out;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH * 4] = L"";
        if (SHGetPathFromIDListW(pidl, path)) out = narrow(path);
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

std::string saveFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const char* defExt, const std::string& defName,
                           const std::string& initialDir) {
    DialogScope ds;
    std::wstring wdir = widen(initialDir);
    wchar_t file[4096] = L"";
    std::wstring dn = widen(defName);
    wcsncpy_s(file, dn.c_str(), _TRUNCATE);
    std::wstring filter = makeFilter(filterDesc, filterPatterns);
    std::wstring wtitle = widen(title), wext = widen(defExt);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 4096;
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = wext.c_str();
    if (!wdir.empty()) ofn.lpstrInitialDir = wdir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return narrow(file);
}

std::string pickExeDialog(const char* title) { return openFileDialog(title, "Programs", "*.exe"); }

void messageBox(const std::string& title, const std::string& text) {
    DialogScope ds;
    MessageBoxW(GetActiveWindow(), widen(text).c_str(), widen(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

bool askYesNo(const std::string& title, const std::string& text) {
    // No owner window: the main window may be the thing that is stuck.
    return MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(),
                       MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
}

bool mouseButtonHeld() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
           (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
}

void hardExit(int code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    std::_Exit(code);
}

bool launch(const std::string& exe, const std::string& fileArg, std::string& error) {
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    if (!fileArg.empty()) cmd += L" \"" + widen(fileArg) + L"\"";
    std::wstring dir = widen(pathToU8(u8path(exe).parent_path()));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        error = "Could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

namespace {
bool g_trayAdded = false;
NOTIFYICONDATAW g_tray{};
}

void notifyUser(void* window, const std::string& title, const std::string& text, bool popup, int sound) {
    HWND hw = (HWND)window;
    if (sound == 2) {
        MessageBeep(MB_ICONASTERISK);
    } else if (sound >= 0) {
        // PlaySound from winmm, loaded when first needed (the sound plays in the background).
        static HMODULE winmm = LoadLibraryW(L"winmm.dll");
        using PlayFn = BOOL(WINAPI*)(LPCWSTR, HMODULE, DWORD);
        static auto play = winmm ? (PlayFn)(void*)GetProcAddress(winmm, "PlaySoundW") : nullptr;
        const std::string& wav = notifySoundWav(sound);
        if (play) play((LPCWSTR)wav.data(), nullptr, 0x0004 /*SND_MEMORY*/ | 0x0001 /*SND_ASYNC*/ | 0x0002 /*SND_NODEFAULT*/);
        else MessageBeep(MB_ICONASTERISK);
    }
    if (hw) {
        FLASHWINFO fi{sizeof fi};
        fi.hwnd = hw;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;   // flashes until Kestral is brought back
        FlashWindowEx(&fi);
    }
    if (!popup || !hw) return;
    if (!g_trayAdded) {
        g_tray.cbSize = sizeof g_tray;
        g_tray.hWnd = hw;
        g_tray.uID = 0x4B53;
        g_tray.uFlags = NIF_ICON | NIF_TIP;
        std::wstring exe = widen(selfExe());
        g_tray.hIcon = exe.empty() ? nullptr : ExtractIconW(GetModuleHandleW(nullptr), exe.c_str(), 0);
        if (!g_tray.hIcon || g_tray.hIcon == (HICON)1) g_tray.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
        wcsncpy_s(g_tray.szTip, L"Kestral", _TRUNCATE);
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
    }
    if (!g_trayAdded) return;
    g_tray.uFlags = NIF_INFO;
    g_tray.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;   // Kestral plays its own sound
    wcsncpy_s(g_tray.szInfoTitle, widen(title.substr(0, 60)).c_str(), _TRUNCATE);
    wcsncpy_s(g_tray.szInfo, widen(text.substr(0, 250)).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void notifyShutdown() {
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_tray);
    g_trayAdded = false;
}

void openUrl(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void setHidden(const std::string& path) {
    std::wstring w = widen(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(w.c_str(), a | FILE_ATTRIBUTE_HIDDEN);
}

void revealInExplorer(const std::string& path) {
    // Explorer only understands backslashes; a folder opens as itself, a file is selected.
    std::string p = path;
    for (auto& c : p) if (c == '/') c = '\\';
    DWORD attr = GetFileAttributesW(widen(p).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteW(nullptr, L"open", widen(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring args = L"/select,\"" + widen(p) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::vector<SlicerInfo> detectSlicers() {
    std::vector<SlicerInfo> out;
    auto pf = [](const wchar_t* var) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
        return n ? narrow(std::wstring(buf, n)) : std::string();
    };
    std::vector<std::string> roots = {pf(L"ProgramFiles"), pf(L"ProgramFiles(x86)"), pf(L"LOCALAPPDATA") + "\\Programs"};
    struct Cand { const char* name; const char* rel; };
    const Cand cands[] = {
        {"OrcaSlicer", "\\OrcaSlicer\\orca-slicer.exe"},
        {"Bambu Studio", "\\Bambu Studio\\bambu-studio.exe"},
        {"PrusaSlicer", "\\Prusa3D\\PrusaSlicer\\prusa-slicer.exe"},
        {"SuperSlicer", "\\SuperSlicer\\superslicer.exe"},
        {"Creality Print", "\\Creality Print\\CrealityPrint.exe"},
    };
    std::error_code ec;
    for (auto& r : roots) {
        if (r.empty()) continue;
        for (auto& c : cands) {
            std::string p = r + c.rel;
            if (std::filesystem::exists(u8path(p), ec)) out.push_back({c.name, p});
        }
        // Cura installs into a versioned folder.
        for (auto& e : std::filesystem::directory_iterator(u8path(r), ec)) {
            std::string dn = pathToU8(e.path().filename());
            if (dn.rfind("UltiMaker Cura", 0) == 0 || dn.rfind("Ultimaker Cura", 0) == 0) {
                auto exe = e.path() / "UltiMaker-Cura.exe";
                if (std::filesystem::exists(exe, ec)) out.push_back({dn, pathToU8(exe)});
            }
        }
    }
    return out;
}


HttpResult httpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body, int timeoutMs, const std::atomic<bool>* cancel, const HttpChunkFn& onChunk,
                       bool keepBody) {
    HttpResult r;
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "Bad URL: " + url; return r; }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET ses = WinHttpOpen(L"Kestral/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"Kestral/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.error = "Could not start the network (error " + std::to_string(GetLastError()) + ")"; return r; }
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    std::wstring wmethod = widen(method);
    HINTERNET req = con ? WinHttpOpenRequest(con, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    auto cleanup = [&] {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
    };
    if (!req) { r.error = "Could not connect to " + url; cleanup(); return r; }
    bool ownType = false;
    for (auto& [k, v] : headers) if (toLower(k) == "content-type") ownType = true;
    std::wstring hdr = body.empty() || ownType ? L"" : L"Content-Type: application/json\r\n";
    for (auto& [k, v] : headers) hdr += widen(k) + L": " + widen(v) + L"\r\n";
    BOOL ok = body.empty()
        ? WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        : WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_CANNOT_CONNECT ? "Could not reach " + url + " (is it running?)"
                : e == ERROR_WINHTTP_TIMEOUT ? "The request timed out"
                : "Network error " + std::to_string(e);
        cleanup();
        return r;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;
    for (;;) {
        if (cancel && cancel->load()) { r.error = "cancelled"; break; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
        if (keepBody || !onChunk) r.body.append(chunk.data(), got);
        if (onChunk) onChunk(chunk.data(), got);
    }
    cleanup();
    return r;
}

ProcResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutMs,
                      const std::atomic<bool>* cancel, const std::string& workDir) {
    ProcResult r;
    // Quote each argument the way the C runtime splits command lines.
    auto quote = [](const std::wstring& a) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring o = L"\"";
        size_t bs = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { ++bs; continue; }
            if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += c; }
            else { o.append(bs, L'\\'); o += c; }
            bs = 0;
        }
        o.append(bs * 2, L'\\');
        return o + L"\"";
    };
    std::wstring cmd = quote(widen(exe));
    for (auto& a : args) cmd += L" " + quote(widen(a));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { r.error = "Could not create a pipe"; return r; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring wdir = widen(workDir.empty() ? pathToU8(u8path(exe).parent_path()) : workDir);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        wdir.empty() ? nullptr : wdir.c_str(), &si, &pi)) {
        r.error = "Could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    ULONGLONG t0 = GetTickCount64();
    char buf[8192];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
            if (r.output.size() < 4u * 1024 * 1024) r.output.append(buf, got);
            avail = 0;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) && got > 0)
                if (r.output.size() < 4u * 1024 * 1024) r.output.append(buf, got);
            break;
        }
        bool stop = cancel && cancel->load();
        bool late = timeoutMs > 0 && GetTickCount64() - t0 > (ULONGLONG)timeoutMs;
        if (stop || late) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            r.cancelled = stop;
            r.timedOut = late;
            r.error = stop ? "cancelled" : "took too long";
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return r;
}

bool urlSchemeRegistered(const std::string& scheme) {
    HKEY k = nullptr;
    std::wstring key = widen(scheme) + L"\\shell\\open\\command";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::string protectSecret(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"Kestral", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return "dp:" + b64enc(s);
}

std::string unprotectSecret(const std::string& stored) {
    if (stored.rfind("dp:", 0) != 0) return {};
    std::string raw = b64dec(stored.substr(3));
    DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}


bool clipboardImage(std::vector<uint8_t>& rgba, int& w, int& h, std::string& filePath) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE hd = IsClipboardFormatAvailable(CF_DIB) ? GetClipboardData(CF_DIB) : nullptr) {
        // A screenshot or a copied picture: a DIB (header, maybe 3 color masks, then rows).
        if (auto* bi = (const BITMAPINFOHEADER*)GlobalLock(hd)) {
            SIZE_T total = GlobalSize(hd);
            int bpp = bi->biBitCount;
            if ((bpp == 32 || bpp == 24) && bi->biWidth > 0 && bi->biHeight != 0 && bi->biWidth < 20000 && std::abs(bi->biHeight) < 20000) {
                w = bi->biWidth;
                h = std::abs(bi->biHeight);
                bool bottomUp = bi->biHeight > 0;
                size_t off = bi->biSize + (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0) + (size_t)bi->biClrUsed * 4;
                size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
                if (off + stride * (size_t)h <= total) {
                    const uint8_t* px = (const uint8_t*)bi + off;
                    rgba.assign((size_t)w * h * 4, 255);
                    bool anyAlpha = false;
                    for (int y = 0; y < h; ++y) {
                        const uint8_t* row = px + stride * (size_t)(bottomUp ? h - 1 - y : y);
                        uint8_t* o = rgba.data() + (size_t)y * w * 4;
                        for (int x = 0; x < w; ++x) {
                            const uint8_t* p = row + (size_t)x * (bpp / 8);
                            o[x * 4 + 0] = p[2];
                            o[x * 4 + 1] = p[1];
                            o[x * 4 + 2] = p[0];
                            if (bpp == 32) { o[x * 4 + 3] = p[3]; anyAlpha |= p[3] != 0; }
                        }
                    }
                    if (bpp == 32 && !anyAlpha)   // most screenshots leave alpha at 0: they are opaque
                        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                    ok = true;
                }
            }
            GlobalUnlock(hd);
        }
    }
    if (!ok && IsClipboardFormatAvailable(CF_HDROP)) {
        // A file copied in Explorer.
        if (HANDLE hd = GetClipboardData(CF_HDROP)) {
            if (HDROP drop = (HDROP)GlobalLock(hd)) {
                wchar_t buf[MAX_PATH * 4];
                if (DragQueryFileW(drop, 0, buf, (UINT)(sizeof buf / sizeof buf[0]))) { filePath = narrow(buf); ok = true; }
                GlobalUnlock(hd);
            }
        }
    }
    CloseClipboard();
    return ok;
}

#else  // Linux (and other POSIX desktops)

namespace {

std::string envOr(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}
std::string homeDir() { return envOr("HOME", "."); }

// Single quotes for /bin/sh, safe for any path.
std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

bool haveCmd(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Runs a command, returns its standard output (trailing newline removed) and exit code.
std::string runCapture(const std::string& cmd, int* exitCode = nullptr) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { if (exitCode) *exitCode = -1; return out; }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = pclose(p);
    if (exitCode) *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

enum class DlgTool { None, Zenity, KDialog };
DlgTool dialogTool() {
    static int t = -1;
    if (t < 0) {
        // KDE desktops look native with kdialog; everything else with zenity (GNOME, XFCE...).
        std::string de = toLower(envOr("XDG_CURRENT_DESKTOP", ""));
        bool kde = de.find("kde") != std::string::npos;
        if (kde && haveCmd("kdialog")) t = 2;
        else if (haveCmd("zenity")) t = 1;
        else if (haveCmd("kdialog")) t = 2;
        else t = 0;
    }
    return (DlgTool)t;
}

std::string patternsFor(const char* filterPatterns) {
    // "*.step;*.stp" -> "*.step *.stp"
    std::string p = filterPatterns ? filterPatterns : "";
    for (auto& c : p) if (c == ';') c = ' ';
    return p;
}

std::function<void()> g_emergencySave;
volatile sig_atomic_t g_inCrash = 0;

void crashHandler(int sig) {
    if (g_inCrash) _exit(128 + sig);
    g_inCrash = 1;
    try { if (g_emergencySave) g_emergencySave(); } catch (...) {}
    log::error("Kestral hit a fatal error (signal " + std::to_string(sig) + "). A recovery copy was saved.");
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void installProcessCrashGuard(std::function<void()> emergencySave) {
    g_emergencySave = std::move(emergencySave);
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        struct sigaction sa {};
        sa.sa_handler = crashHandler;
        sigemptyset(&sa.sa_mask);
        sigaction(s, &sa, nullptr);
    }
    std::set_terminate([] {
        log::error("std::terminate called");
        crashHandler(SIGABRT);
    });
}
void installThreadGuard() {}

std::string appDataDir() {
    // XDG: ~/.config/kestral (an older ~/.kestral folder is moved over once).
    std::string base = envOr("XDG_CONFIG_HOME", homeDir() + "/.config");
    std::string d = base + "/kestral";
    std::error_code ec;
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        std::string old = homeDir() + "/.kestral";
        if (!std::filesystem::exists(d, ec) && std::filesystem::is_directory(old, ec)) {
            std::filesystem::create_directories(base, ec);
            std::filesystem::rename(old, d, ec);
        }
    }
    std::filesystem::create_directories(d, ec);
    return d;
}

std::string tempDir() {
    std::string d = envOr("TMPDIR", "/tmp") + "/kestral-" + envOr("USER", "user");
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

std::string fileVersion(const std::string&) { return {}; }

std::string selfExe() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
}

std::string legacyDesignsDir() {
    static std::string cached;
    if (cached.empty()) {
        std::string docs;
        if (haveCmd("xdg-user-dir")) docs = runCapture("xdg-user-dir DOCUMENTS 2>/dev/null");
        std::error_code ec;
        if (docs.empty() || docs == homeDir() || !std::filesystem::is_directory(docs, ec))
            docs = std::filesystem::is_directory(homeDir() + "/Documents", ec) ? homeDir() + "/Documents" : homeDir();
        cached = docs + "/Kestral Designs";
    }
    return cached;
}

std::string openFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const std::string& initialDir) {
    if (const char* e = std::getenv("KESTRAL_OPEN")) return e;   // automation / tests
    DialogScope ds;
    std::string pats = patternsFor(filterPatterns), dir = initialDir.empty() ? homeDir() : initialDir;
    switch (dialogTool()) {
    case DlgTool::Zenity:
        return runCapture("zenity --file-selection --title=" + shq(title) + " --filename=" + shq(dir + "/") +
                          " --file-filter=" + shq(std::string(filterDesc) + " | " + pats) + " --file-filter='All files | *' 2>/dev/null");
    case DlgTool::KDialog:
        return runCapture("kdialog --title " + shq(title) + " --getopenfilename " + shq(dir) + " " +
                          shq(pats + "|" + filterDesc) + " 2>/dev/null");
    default:
        log::warn("No file dialog: install zenity or kdialog");
        return {};
    }
}

std::string saveFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const char* defExt,
                           const std::string& defName, const std::string& initialDir) {
    if (const char* e = std::getenv("KESTRAL_SAVE")) return e;
    if (std::getenv("KESTRAL_OPEN")) return tempDir() + "/" + defName;   // automation / tests
    DialogScope ds;
    std::string pats = patternsFor(filterPatterns), dir = initialDir.empty() ? homeDir() : initialDir;
    std::string out;
    switch (dialogTool()) {
    case DlgTool::Zenity:
        out = runCapture("zenity --file-selection --save --confirm-overwrite --title=" + shq(title) + " --filename=" +
                         shq(dir + "/" + defName) + " --file-filter=" + shq(std::string(filterDesc) + " | " + pats) + " 2>/dev/null");
        break;
    case DlgTool::KDialog:
        out = runCapture("kdialog --title " + shq(title) + " --getsavefilename " + shq(dir + "/" + defName) + " " +
                         shq(pats + "|" + filterDesc) + " 2>/dev/null");
        break;
    default:
        log::warn("No file dialog: install zenity or kdialog");
        return {};
    }
    // Add the extension when the name has none.
    if (!out.empty() && defExt && *defExt && std::filesystem::path(out).extension().empty()) out += std::string(".") + defExt;
    return out;
}

bool clipboardImage(std::vector<uint8_t>& rgba, int& w, int& h, std::string& filePath) {
    // X11 / Wayland: ask wl-paste or xclip for a PNG and hand back its file.
    (void)rgba; (void)w; (void)h;
    std::string out = tempDir() + "/kestral_clip.png";
    std::string cmd;
    if (haveCmd("wl-paste")) cmd = "wl-paste --type image/png > " + shq(out) + " 2>/dev/null";
    else if (haveCmd("xclip")) cmd = "xclip -selection clipboard -t image/png -o > " + shq(out) + " 2>/dev/null";
    else return false;
    int code = 0;
    runCapture(cmd, &code);
    std::ifstream f(out, std::ios::binary | std::ios::ate);
    if (code != 0 || !f || f.tellg() < 16) return false;
    filePath = out;
    return true;
}

std::string pickExeDialog(const char* title) {
    if (dialogTool() == DlgTool::None) return {};
    DialogScope ds;
    if (dialogTool() == DlgTool::Zenity)
        return runCapture("zenity --file-selection --title=" + shq(title) + " 2>/dev/null");
    return runCapture("kdialog --title " + shq(title) + " --getopenfilename " + shq(homeDir()) + " 2>/dev/null");
}

std::string pickFolderDialog(const char* title, const std::string& initialDir) {
    if (const char* e = std::getenv("KESTRAL_FOLDER")) return e;   // automation / tests
    if (dialogTool() == DlgTool::None) return {};
    DialogScope ds;
    std::string dir = initialDir.empty() ? homeDir() : initialDir;
    if (dialogTool() == DlgTool::Zenity)
        return runCapture("zenity --file-selection --directory --title=" + shq(title) + " --filename=" + shq(dir + "/") + " 2>/dev/null");
    return runCapture("kdialog --title " + shq(title) + " --getexistingdirectory " + shq(dir) + " 2>/dev/null");
}

void messageBox(const std::string& t, const std::string& m) {
    log::info(t + ": " + m);
    if (std::getenv("KESTRAL_OPEN")) return;
    DialogScope ds;
    if (dialogTool() == DlgTool::Zenity) std::system(("zenity --info --no-wrap --title=" + shq(t) + " --text=" + shq(m) + " >/dev/null 2>&1").c_str());
    else if (dialogTool() == DlgTool::KDialog) std::system(("kdialog --title " + shq(t) + " --msgbox " + shq(m) + " >/dev/null 2>&1").c_str());
}

bool askYesNo(const std::string& t, const std::string& m) {
    log::info(t + ": " + m);
    if (std::getenv("KESTRAL_OPEN")) return false;
    int code = -1;
    if (dialogTool() == DlgTool::Zenity) runCapture("zenity --question --no-wrap --title=" + shq(t) + " --text=" + shq(m) + " 2>/dev/null", &code);
    else if (dialogTool() == DlgTool::KDialog) runCapture("kdialog --title " + shq(t) + " --yesno " + shq(m) + " 2>/dev/null", &code);
    return code == 0;
}

void hardExit(int code) { std::_Exit(code); }
bool mouseButtonHeld() { return false; }

HttpResult httpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body, int timeoutMs, const std::atomic<bool>* cancel, const HttpChunkFn& onChunk,
                       bool keepBody) {
    // curl does the networking (installed on every desktop distro). Body and headers go through
    // private temp files, so API keys never show up in the process list.
    HttpResult r;
    static std::atomic<int> seq{0};
    std::string base = tempDir() + "/http_" + std::to_string(getpid()) + "_" + std::to_string(seq++);
    std::string bodyFile = base + ".body", hdrFile = base + ".hdr";
    auto writePrivate = [](const std::string& path, const std::string& data) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
        close(fd);
        return ok;
    };
    std::string hdrs;
    bool ownType = false;
    for (auto& [k, v] : headers) if (toLower(k) == "content-type") ownType = true;
    if (!body.empty() && !ownType) hdrs += "Content-Type: application/json\n";
    for (auto& [k, v] : headers) hdrs += k + ": " + v + "\n";
    if (!writePrivate(bodyFile, body) || !writePrivate(hdrFile, hdrs)) { r.error = "Could not write a temp file"; return r; }
    if (onChunk) {
        // Streamed: run curl directly and pass its output on as it comes. The status line goes to
        // a header file, so the body stays clean.
        std::string respHdr = base + ".resp";
        std::vector<std::string> args = {"curl", "-s", "-L", "-N", "-m", std::to_string(timeoutMs / 1000 + 1), "-D", respHdr,
                                         "-X", method, "-H", "@" + hdrFile};
        if (!body.empty()) { args.push_back("--data-binary"); args.push_back("@" + bodyFile); }
        args.push_back(url);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        int fds[2];
        if (pipe(fds) != 0) { r.error = "Could not create a pipe"; return r; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        pid_t pid = 0;
        int rc = posix_spawnp(&pid, "curl", &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fds[1]);
        if (rc != 0) {
            close(fds[0]);
            std::remove(bodyFile.c_str());
            std::remove(hdrFile.c_str());
            r.error = "curl is not installed";
            return r;
        }
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        char buf[8192];
        int st = 0;
        for (;;) {
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof buf)) > 0) {
                if (keepBody) r.body.append(buf, (size_t)n);
                onChunk(buf, (size_t)n);
            }
            if (waitpid(pid, &st, WNOHANG) == pid) {
                while ((n = read(fds[0], buf, sizeof buf)) > 0) {
                    if (keepBody) r.body.append(buf, (size_t)n);
                    onChunk(buf, (size_t)n);
                }
                break;
            }
            if (cancel && cancel->load()) {
                kill(pid, SIGTERM);
                waitpid(pid, &st, 0);
                r.error = "cancelled";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        close(fds[0]);
        std::remove(bodyFile.c_str());
        std::remove(hdrFile.c_str());
        if (FILE* hf = std::fopen(respHdr.c_str(), "rb")) {
            char line[512];
            while (std::fgets(line, sizeof line, hf))
                if (std::strncmp(line, "HTTP/", 5) == 0) {
                    const char* sp = std::strchr(line, ' ');
                    if (sp) r.status = std::atoi(sp + 1);   // the last status line wins (after a 100 Continue)
                }
            std::fclose(hf);
        }
        std::remove(respHdr.c_str());
        if (r.status == 0 && r.error.empty())
            r.error = WIFEXITED(st) && WEXITSTATUS(st) == 28 ? "The request timed out" : "Could not reach " + url;
        return r;
    }
    std::string cmd = "curl -s -L -m " + std::to_string(timeoutMs / 1000 + 1) + " -w '\\n%{http_code}' -X " + shq(method) +
                      " -H @" + shq(hdrFile);
    if (!body.empty()) cmd += " --data-binary @" + shq(bodyFile);
    cmd += " " + shq(url) + " 2>/dev/null";
    int code = 0;
    r.body = runCapture(cmd, &code);
    std::remove(bodyFile.c_str());
    std::remove(hdrFile.c_str());
    auto nl = r.body.find_last_of('\n');
    if (nl != std::string::npos) { r.status = std::atoi(r.body.c_str() + nl + 1); r.body.resize(nl); }
    else { r.status = std::atoi(r.body.c_str()); r.body.clear(); }
    if (r.status == 0) r.error = code == 127 ? "curl is not installed" : "Could not reach " + url;
    return r;
}

ProcResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutMs,
                      const std::atomic<bool>* cancel, const std::string& workDir) {
    ProcResult r;
    std::vector<std::string> all;
    if (exe.rfind("flatpak:", 0) == 0) {
        all = {"flatpak", "run", "--filesystem=host", exe.substr(8)};
    } else {
        all = {exe};
    }
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) { r.error = "Could not create a pipe"; return r; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::string oldDir;
    if (!workDir.empty()) {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) oldDir = cwd;
        if (chdir(workDir.c_str()) != 0) oldDir.clear();
    }
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    if (!oldDir.empty() && chdir(oldDir.c_str()) != 0) {}
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        r.error = "Could not start " + exe + " (" + std::strerror(rc) + ")";
        return r;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    auto t0 = std::chrono::steady_clock::now();
    char buf[8192];
    int status = 0;
    bool done = false;
    while (!done) {
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0)
            if (r.output.size() < 4u * 1024 * 1024) r.output.append(buf, (size_t)n);
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            while ((n = read(fds[0], buf, sizeof buf)) > 0) r.output.append(buf, (size_t)n);
            done = true;
            break;
        }
        bool stop = cancel && cancel->load();
        bool late = timeoutMs > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs);
        if (stop || late) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 20 && waitpid(pid, &status, WNOHANG) != pid; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            r.cancelled = stop;
            r.timedOut = late;
            r.error = stop ? "cancelled" : "took too long";
            close(fds[0]);
            r.exitCode = -1;
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    close(fds[0]);
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

bool urlSchemeRegistered(const std::string& scheme) {
    if (!haveCmd("xdg-mime")) return false;
    return !runCapture("xdg-mime query default x-scheme-handler/" + scheme + " 2>/dev/null").empty();
}

// Secrets: kept in the settings file, which only this user can read (0600). Use the desktop's
// keyring (libsecret) here later if wanted.
std::string protectSecret(const std::string& plain) { return plain.empty() ? std::string() : "b64:" + b64enc(plain); }
std::string unprotectSecret(const std::string& stored) { return stored.rfind("b64:", 0) == 0 ? b64dec(stored.substr(4)) : std::string(); }

bool launch(const std::string& exe, const std::string& arg, std::string& error) {
    // "flatpak:<app id>" runs a Flatpak app; anything else is a program path.
    std::vector<std::string> args;
    if (exe.rfind("flatpak:", 0) == 0) {
        args = {"flatpak", "run", "--file-forwarding", exe.substr(8)};
        if (!arg.empty()) { args.push_back("@@"); args.push_back(arg); args.push_back("@@"); }
    } else {
        args = {exe};
        if (!arg.empty()) args.push_back(arg);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);   // keeps running if Kestral closes
#endif
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = "Could not start " + exe + " (" + std::strerror(rc) + ")";
        return false;
    }
    // Reap it in the background so no zombie stays around.
    std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    return true;
}

void notifyUser(void*, const std::string& title, const std::string& text, bool popup, int sound) {
    // notify-send and the desktop's sound player, when they are there; never waits on them.
    std::string wavPath;
    if (sound == 0 || sound == 1) {
        wavPath = tempDir() + "/notify" + std::to_string(sound) + ".wav";
        std::ofstream f(wavPath, std::ios::binary | std::ios::trunc);
        const std::string& w = notifySoundWav(sound);
        f.write(w.data(), (std::streamsize)w.size());
    }
    std::thread([title, text, popup, sound, wavPath] {
        if (popup) runProcess("notify-send", {"-a", "Kestral", title, text.substr(0, 250)}, 5000);
        if (!wavPath.empty()) {
            ProcResult r = runProcess("paplay", {wavPath}, 5000);
            if (r.exitCode != 0) runProcess("aplay", {"-q", wavPath}, 5000);
        } else if (sound == 2) {
            ProcResult r = runProcess("canberra-gtk-play", {"-i", "complete"}, 5000);
            if (r.exitCode != 0) runProcess("paplay", {"/usr/share/sounds/freedesktop/stereo/complete.oga"}, 5000);
        }
    }).detach();
}

void notifyShutdown() {}

void setHidden(const std::string&) {}

void revealInExplorer(const std::string& path) {
    std::error_code ec;
    bool dir = std::filesystem::is_directory(path, ec);
    // File managers that speak the freedesktop FileManager1 interface select the file.
    if (!dir && haveCmd("gdbus")) {
        std::string uri = "file://" + path;
        std::string cmd = "gdbus call --session --dest org.freedesktop.FileManager1 --object-path /org/freedesktop/FileManager1 "
                          "--method org.freedesktop.FileManager1.ShowItems \"['" + uri + "']\" '' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) return;
    }
    std::string target = dir ? path : std::filesystem::path(path).parent_path().string();
    std::string err;
    launch("xdg-open", target, err);
}

void openUrl(const std::string& url) {
    if (std::getenv("KESTRAL_OPEN")) { log::info("open " + url); return; }
    std::string err;
    if (!launch("xdg-open", url, err)) log::warn(err);
}

std::vector<SlicerInfo> detectSlicers() {
    std::vector<SlicerInfo> out;
    auto which = [](const char* name) { return runCapture(std::string("command -v ") + name + " 2>/dev/null"); };
    struct Cand { const char* name; std::vector<const char*> cmds; const char* flatpak; };
    const Cand cands[] = {
        {"OrcaSlicer", {"orca-slicer", "OrcaSlicer", "orcaslicer"}, "io.github.softfever.OrcaSlicer"},
        {"Bambu Studio", {"bambu-studio", "BambuStudio"}, "com.bambulab.BambuStudio"},
        {"PrusaSlicer", {"prusa-slicer", "PrusaSlicer"}, "com.prusa3d.PrusaSlicer"},
        {"SuperSlicer", {"superslicer", "SuperSlicer"}, nullptr},
        {"UltiMaker Cura", {"cura", "UltiMaker-Cura"}, "com.ultimaker.cura"},
    };
    std::string flatpaks = haveCmd("flatpak") ? runCapture("flatpak list --app --columns=application 2>/dev/null") : std::string();
    std::error_code ec;
    for (auto& c : cands) {
        std::string found;
        for (auto cmd : c.cmds) {
            found = which(cmd);
            if (!found.empty()) break;
        }
        if (found.empty() && c.flatpak && flatpaks.find(c.flatpak) != std::string::npos) found = std::string("flatpak:") + c.flatpak;
        if (found.empty()) {
            // AppImages people keep in the usual places.
            std::string key = toLower(c.name);
            key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
            for (const std::string& dir : {homeDir() + "/Applications", homeDir() + "/.local/bin", homeDir() + "/Downloads", std::string("/opt")}) {
                for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    std::string n = toLower(pathToU8(e.path().filename()));
                    std::string nn = n;
                    nn.erase(std::remove(nn.begin(), nn.end(), '_'), nn.end());
                    nn.erase(std::remove(nn.begin(), nn.end(), '-'), nn.end());
                    if (nn.find(key) != std::string::npos && n.size() > 9 && n.substr(n.size() - 9) == ".appimage") { found = pathToU8(e.path()); break; }
                }
                if (!found.empty()) break;
            }
        }
        if (!found.empty()) out.push_back({c.name, found});
    }
    return out;
}

#endif

HttpResult httpPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& body, int timeoutMs, const std::atomic<bool>* cancel, const HttpChunkFn& onChunk) {
    return httpRequest("POST", url, headers, body, timeoutMs, cancel, onChunk);
}
HttpResult httpGet(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs,
                   const std::atomic<bool>* cancel) {
    return httpRequest("GET", url, headers, std::string(), timeoutMs, cancel);
}

HttpResult httpDownload(const std::string& url, const std::string& path, int timeoutMs, const std::atomic<bool>* cancel,
                        const std::function<void(long long, long long)>& progress) {
    HttpResult r;
    std::error_code ec;
    std::filesystem::create_directories(u8path(path).parent_path(), ec);
    FILE* f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, u8path(path).wstring().c_str(), L"wb");
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) { r.error = "Could not write " + path; return r; }
    long long got = 0;
    bool writeFailed = false;
    r = httpRequest("GET", url, {}, std::string(), timeoutMs, cancel, [&](const char* d, size_t n) {
        if (std::fwrite(d, 1, n, f) != n) writeFailed = true;
        got += (long long)n;
        if (progress) progress(got, 0);
    }, false);
    std::fclose(f);
    if (writeFailed && r.error.empty()) r.error = "The disk is full or not writable";
    if (r.error.empty() && (r.status < 200 || r.status >= 300)) r.error = "The download failed (HTTP " + std::to_string(r.status) + ")";
    if (!r.error.empty()) std::filesystem::remove(u8path(path), ec);
    return r;
}

std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

bool guarded(const char* what, const std::function<void()>& fn, std::string& error) {
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown internal error";
    }
    log::error(std::string(what) + " failed: " + error);
    return false;
}

std::string programDir() {
    // The Kestral folder: the one holding run.bat / run.sh (Kestral.exe lives in its bin folder), else the
    // program's own folder.
    std::error_code ec;
    std::string exe = selfExe();
    if (exe.empty()) return {};
    std::filesystem::path d = u8path(exe).parent_path();
    std::filesystem::path p = d;
    for (int up = 0; up < 3 && !p.empty(); ++up, p = p.parent_path()) {
        if (std::filesystem::exists(p / "run.bat", ec) || std::filesystem::exists(p / "run.sh", ec)) return pathToU8(p);
        if (p == p.parent_path()) break;
    }
    return pathToU8(d);
}

std::string designsDir() {
    static std::string cached;
    if (cached.empty()) {
        std::error_code ec;
        std::string root = programDir();
        if (!root.empty()) {
            std::filesystem::path d = u8path(root) / "designs";
            std::filesystem::create_directories(d, ec);
            // Only when Kestral may write there (not an install under Program Files).
            std::filesystem::path probe = d / ".kestral_write_test";
            {
                std::ofstream t(probe, std::ios::binary);
                if (t) { t << "ok"; t.close(); if (std::filesystem::exists(probe, ec)) cached = pathToU8(d); }
            }
            std::filesystem::remove(probe, ec);
        }
        if (cached.empty()) cached = legacyDesignsDir();
    }
    std::error_code ec;
    std::filesystem::create_directories(u8path(cached), ec);
    return cached;
}

const std::string& notifySoundWav(int kind) {
    // Soft sine tones with a gentle attack and a long fade, a quiet octave above added for shimmer.
    static std::string cache[2];
    kind = kind == 1 ? 1 : 0;
    if (!cache[kind].empty()) return cache[kind];
    const int rate = 44100;
    struct Note { double start, freq, len, gain; };
    std::vector<Note> notes = kind == 0 ? std::vector<Note>{{0.00, 880.00, 0.9, 0.40}, {0.11, 1318.51, 1.1, 0.34}}   // A5 then E6
                                        : std::vector<Note>{{0.00, 783.99, 0.22, 0.50}, {0.13, 1174.66, 0.30, 0.46}};  // pop pop, G5 then D6
    double total = 0;
    for (auto& n : notes) total = std::max(total, n.start + n.len);
    size_t count = (size_t)(total * rate);
    std::vector<int16_t> pcm(count);
    const double tau = 6.283185307179586;
    for (size_t i = 0; i < count; ++i) {
        double t = (double)i / rate, v = 0;
        for (auto& n : notes) {
            double u = t - n.start;
            if (u < 0 || u > n.len) continue;
            double attack = std::min(1.0, u / (kind == 1 ? 0.004 : 0.008));
            double decay = std::exp(-u * (kind == 0 ? 4.2 : 16.0));
            double tail = std::min(1.0, (n.len - u) / 0.05);
            double tone;
            if (kind == 1) {
                // Bubbly pop: the pitch rises quickly into the note (phase is the integral of the glide).
                const double k = 45.0, d = 0.22;
                double phase = tau * n.freq * (u - d * (1.0 - std::exp(-u * k)) / k);
                tone = std::sin(phase) + 0.12 * std::sin(2.0 * phase) * std::exp(-u * 25.0);
            } else {
                tone = std::sin(tau * n.freq * u) + 0.18 * std::sin(tau * n.freq * 2.0 * u) * std::exp(-u * 6.0);
            }
            v += n.gain * attack * decay * tail * tone;
        }
        pcm[i] = (int16_t)std::lround(std::clamp(v * 0.75, -1.0, 1.0) * 32767);
    }
    std::string& w = cache[kind];
    auto u32 = [&](uint32_t x) { for (int b = 0; b < 4; ++b) w.push_back((char)((x >> (8 * b)) & 255)); };
    auto u16 = [&](uint16_t x) { w.push_back((char)(x & 255)); w.push_back((char)(x >> 8)); };
    uint32_t bytes = (uint32_t)(count * 2);
    w += "RIFF"; u32(36 + bytes); w += "WAVE";
    w += "fmt "; u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    w += "data"; u32(bytes);
    w.append(reinterpret_cast<const char*>(pcm.data()), bytes);
    return w;
}

} // namespace kst::platform
