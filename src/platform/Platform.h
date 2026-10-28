#pragma once
#include <atomic>
#include <functional>
#include <utility>
#include <string>
#include <cstdint>
#include <vector>

namespace kst::platform {

// Crash safety
void installProcessCrashGuard(std::function<void()> emergencySave);
void installThreadGuard();   // converts hardware faults on this thread into C++ exceptions
// Runs fn and turns any exception or fault into a false return with an error message.
bool guarded(const char* what, const std::function<void()>& fn, std::string& error);

// Folders (UTF-8)
std::string appDataDir();    // %APPDATA%\Kestral
std::string tempDir();       // %TEMP%\Kestral
std::string designsDir();    // <Kestral folder>\designs (created on first use): where designs are saved.
                             // Falls back to Documents\Kestral Designs when the program folder is read only.
std::string legacyDesignsDir();   // Documents\Kestral Designs, where designs were saved before (not created)
std::string programDir();    // the Kestral folder (holds run.bat / run.sh), else the program's folder

// Dialogs (UTF-8 paths, empty on cancel)
std::string openFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const std::string& initialDir = std::string());
std::string saveFileDialog(const char* title, const char* filterDesc, const char* filterPatterns, const char* defExt, const std::string& defName,
                           const std::string& initialDir = std::string());
std::string pickExeDialog(const char* title);
// A picture on the clipboard (a screenshot, a copied image) as RGBA, or a copied image file's path.
bool clipboardImage(std::vector<uint8_t>& rgba, int& w, int& h, std::string& filePath);
void messageBox(const std::string& title, const std::string& text);
// Yes / No box that works from any thread (the hang watchdog uses it). False on other platforms.
bool askYesNo(const std::string& title, const std::string& text);
// True while a native dialog (file picker, message box) is open on the main thread: the main loop
// is legitimately blocked then, so the hang watchdog ignores it.
bool inNativeDialog();
// A mouse button physically held (Windows' window move / resize loop pauses the main loop).
bool mouseButtonHeld();
// Ends the process at once without running destructors (used after the user asks to close a
// hung Kestral; the recovery copy is already on disk).
[[noreturn]] void hardExit(int code);

// Processes
std::string selfExe();   // full path of the running Kestral executable (empty if unknown)
// "2.4.2" from a Windows program's version resource (empty elsewhere or when it has none).
std::string fileVersion(const std::string& path);
bool launch(const std::string& exe, const std::string& fileArg, std::string& error);   // empty fileArg: none
void revealInExplorer(const std::string& path);
// Windows: mark a file or folder hidden (Explorer hides it). Elsewhere a dot name already does.
void setHidden(const std::string& path);
void openUrl(const std::string& url);   // default browser
// A desktop notification (Windows: from the tray icon; Linux: notify-send), a sound, and the taskbar
// button flashing until the window is used. window: the app's native window (HWND), may be null.
// sound: -1 none, 0 Kestral's chime, 1 pop, 2 the system's own sound.
void notifyUser(void* window, const std::string& title, const std::string& text, bool popup, int sound);
// Kestral's own notification sounds as a WAV file in memory (made, not recorded): 0 chime, 1 pop.
const std::string& notifySoundWav(int kind);
void notifyShutdown();   // removes the tray icon

// Runs a program to completion and collects what it prints (stdout and stderr together). No
// window shows. Stops it on timeout or when cancel turns true. "flatpak:<app id>" runs a Flatpak.
struct ProcResult { int exitCode = -1; std::string output; std::string error; bool timedOut = false, cancelled = false; };
ProcResult runProcess(const std::string& exe, const std::vector<std::string>& args, int timeoutMs,
                      const std::atomic<bool>* cancel = nullptr, const std::string& workDir = std::string());
// True when an app registered this URL scheme (e.g. "bambu-connect").
bool urlSchemeRegistered(const std::string& scheme);
std::string urlEncode(const std::string& s);

// HTTP POST (https or http). Blocking: call from a worker thread. cancel may be null.
// onChunk (optional) sees the body as it arrives, for streamed replies. The whole body is still
// returned in HttpResult. A streamed request also notices cancel between chunks.
struct HttpResult { int status = 0; std::string body; std::string error; };
using HttpChunkFn = std::function<void(const char* data, size_t n)>;
HttpResult httpRequest(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body, int timeoutMs, const std::atomic<bool>* cancel, const HttpChunkFn& onChunk = {},
                       bool keepBody = true);
HttpResult httpPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& body, int timeoutMs, const std::atomic<bool>* cancel, const HttpChunkFn& onChunk = {});
HttpResult httpGet(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs,
                   const std::atomic<bool>* cancel);
// Downloads to a file (redirects followed). progress(bytes so far, total or 0) may be empty.
HttpResult httpDownload(const std::string& url, const std::string& path, int timeoutMs, const std::atomic<bool>* cancel,
                        const std::function<void(long long, long long)>& progress = {});
// Secrets (API keys) at rest: encrypted to this Windows user (DPAPI), base64 text.
std::string protectSecret(const std::string& plain);
std::string unprotectSecret(const std::string& stored);

struct SlicerInfo { std::string name; std::string path; };
std::vector<SlicerInfo> detectSlicers();

} // namespace kst::platform
