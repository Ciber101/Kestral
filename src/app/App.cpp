#include "app/App.h"
#include "app/Mcp.h"
#include "ui/UiItems.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "kernel/Features.h"
#include "kernel/Kernel.h"
#include "platform/Platform.h"
#include "print/Engine.h"

#define GLFW_INCLUDE_VULKAN
#include <fstream>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <unordered_map>

namespace kst {

namespace {
App* g_app = nullptr;
const char* kVersion = "0.4.0";

void cbCursor(GLFWwindow*, double, double) { if (g_app) g_app->activity(); }
void cbButton(GLFWwindow*, int, int, int) { if (g_app) g_app->activity(); }
void cbScroll(GLFWwindow*, double, double) { if (g_app) g_app->activity(); }
void cbKey(GLFWwindow*, int, int, int, int) { if (g_app) g_app->activity(); }
void cbChar(GLFWwindow*, unsigned int) { if (g_app) g_app->activity(); }
void cbSize(GLFWwindow*, int, int) { if (g_app) g_app->activity(); }
void cbFocus(GLFWwindow*, int) { if (g_app) g_app->activity(); }
void cbDrop(GLFWwindow*, int n, const char** paths) { if (g_app) g_app->dropFiles(n, paths); }
void cbClose(GLFWwindow* w) {
    if (g_app && !g_app->allowClose()) glfwSetWindowShouldClose(w, GLFW_FALSE);
}
void cbGlfwError(int code, const char* msg) { log::error("glfw " + std::to_string(code) + ": " + (msg ? msg : "")); }

#ifdef _WIN32
// While you drag or resize the window, Windows runs its own message loop and our main loop
// is paused inside it, so the viewport would freeze (and a finished import would not show
// until you let go). A timer during that loop keeps drawing frames.
WNDPROC g_prevWndProc = nullptr;
constexpr UINT_PTR kModalTimer = 0x4B53;
LRESULT CALLBACK kstWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ENTERSIZEMOVE: SetTimer(h, kModalTimer, 8, nullptr); break;
    case WM_EXITSIZEMOVE: KillTimer(h, kModalTimer); break;
    case WM_TIMER:
        if (wp == kModalTimer) {
            if (g_app) g_app->modalFrame();
            return 0;
        }
        break;
    default: break;
    }
    return CallWindowProcW(g_prevWndProc, h, msg, wp, lp);
}
#endif
}

const char* kestralVersion() { return kVersion; }

void App::modalFrame() {
    if (inFrame_) return;
    wdBeatMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::string err;
    platform::guarded("frame", [&] { frame(); }, err);
}

// ===================================================================== lifecycle

// Designs used to live in Documents\Kestral Designs. They now live in the Kestral folder (designs). Move
// everything over once, and point recent files and the last folder at the new place.
void App::moveOldDesigns() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string oldS = platform::legacyDesignsDir(), newS = platform::designsDir();
    fs::path oldP = u8path(oldS), newP = u8path(newS);
    if (oldS.empty() || !fs::is_directory(oldP, ec) || fs::equivalent(oldP, newP, ec)) return;
    int moved = 0, kept = 0;
    for (auto& e : fs::directory_iterator(oldP, ec)) {
        fs::path to = newP / e.path().filename();
        if (fs::exists(to, ec)) { ++kept; continue; }   // never overwrite
        std::error_code mec;
        fs::rename(e.path(), to, mec);
        if (mec) {   // another drive: copy, then remove the original only when the copy worked
            std::error_code cec;
            fs::copy(e.path(), to, fs::copy_options::recursive, cec);
            if (cec) { ++kept; log::warn("Could not move " + pathToU8(e.path()) + ": " + cec.message()); continue; }
            fs::remove_all(e.path(), cec);
        }
        ++moved;
    }
    if (!kept) fs::remove(oldP, ec);   // empty now
    auto fix = [&](std::string& p) {
        if (p.size() >= oldS.size() && p.compare(0, oldS.size(), oldS) == 0) p = newS + p.substr(oldS.size());
    };
    {
        // The Fusion export script names its destination: point it at the new folder too.
        fs::path py = newP / "Fusion export script" / "KestralExportAll" / "KestralExportAll.py";
        std::ifstream in(py, std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            bool changed = false;
            for (size_t at; (at = text.find(oldS)) != std::string::npos; changed = true) text.replace(at, oldS.size(), newS);
            if (changed) { std::ofstream o(py, std::ios::binary | std::ios::trunc); o << text; }
        }
    }
    for (auto& r : settings_.recentFiles) fix(r);
    fix(settings_.lastDesignDir);
    settings_.save(settingsPath_);
    if (moved) log::info("Moved " + std::to_string(moved) + " items from " + oldS + " to " + newS);
}

int App::run(int argc, char** argv) {
    g_app = this;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--model-test") modelTest_ = true;
        if (std::string(argv[i]) == "--ui-test") uiTest_ = true;
        // --run-tools / --batch <script.jsonl> <out.kst>: run tool calls from a file, save, quit.
        if ((std::string(argv[i]) == "--run-tools" || std::string(argv[i]) == "--batch") && i + 2 < argc) { toolScript_ = argv[i + 1]; toolScriptOut_ = argv[i + 2]; i += 2; }
        // --hidden: no window on screen. MCP apps (and --batch) drive it; Kestral otherwise works as usual.
        if (std::string(argv[i]) == "--hidden") hiddenWindow_ = true;
    }
    std::string err;
    bool ok = false;
    platform::guarded("startup", [&] { ok = init(err); }, err);
    if (!ok) {
        log::error("Startup failed: " + err);
        platform::messageBox("Kestral", "Kestral could not start:\n\n" + err);
        shutdown();
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--run-tools" || std::string(argv[i]) == "--batch") { i += 2; continue; }
        if (argv[i][0] != '-' || argv[i][1] != '-') importPath(argv[i]);
    }
    // No design on the command line: start on the Start page, like pro CAD apps.
    startOpen_ = toolScript_.empty() && !modelTest_ && !uiTest_ && doc_.timeline.empty() && doc_.bodies.empty();
    // Developer check: KESTRAL_SLICE_PREVIEW=<print file> opens the Slice tab showing that print.
    if (const char* pv = std::getenv("KESTRAL_SLICE_PREVIEW")) {
        enterSliceWorkspace();
        loadSlicePreview(pv);
    }
    // Developer check: KESTRAL_SEARCH_TEST=<words> types into the search box (<words>! also opens the first result).
    if (const char* sq = std::getenv("KESTRAL_SEARCH_TEST")) {
        std::string q = sq;
        gsearchTestGo_ = !q.empty() && q.back() == '!';
        if (gsearchTestGo_) q.pop_back();
        gsearch_.text = q;
        gsearch_.open = true;
        gsearchTest_ = true;
    }
    // Developer check: KESTRAL_AI_TEST=1 opens the assistant with a sample chat (2: empty, 3: with a question card).
    if (const char* at = std::getenv("KESTRAL_AI_TEST")) {
        ai_.open = true;
        int m = std::atoi(at);
        ai_.scroll = true;
        if (const char* pic = std::getenv("KESTRAL_AI_PIC")) aiAttachFile(pic);
        if (const char* say = std::getenv("KESTRAL_AI_SEND")) { ai_.input = say; aiTestSend_ = true; }
        if (m == 4) ai_.input = "This is a very long message that should wrap onto new lines instead of running off the edge of the box and disappearing. Lets see if it grows.\nSecond line.";
        if (m != 2 && m != 4) {
            ai_.lines.push_back({0, "Make a 40 mm cube with 3 mm fillets on the top edges, then make it red."});
            ai_.lines.push_back({4, "A grey L shaped bracket on a white table, two holes.", 3.1});
            ai_.lines.push_back({5, "eyes:7b\nAn **L bracket** made of bent sheet metal.\n- Base plate 50 x 36 mm, 3 mm thick\n- Upright plate 45 mm tall along one long edge\n- One `5 mm` hole in the centre of each plate\n- All outer edges slightly chamfered", 3.1});
            ai_.lines.push_back({4, "The user wants a cube. I will create a box, then fillet the top edges, then color it.", 4.2});
            ai_.lines.push_back({2, "create_box: made Cube (40 x 40 x 40 mm)"});
            ai_.lines.push_back({2, "fillet: 4 edges, 3 mm"});
            ai_.lines.push_back({2, "set_color: Cube is now red"});
            ai_.lines.push_back({1, "## Done\nI made **Cube**, a 40 mm cube:\n- Top edges rounded with a `3 mm` fillet\n- Colored red\n\n1. Open the Slice tab to print it\n2. Or ask me to hollow it out\n```\ncreate_box 40 40 40\n```"});
            ai_.lines.push_back({3, "Could not reach Ollama at http://localhost:11434. Is it running?"});
        }
        if (m == 3) {
            aiChoice_.open = true;
            aiChoice_.title = "The graphics card is busy";
            aiChoice_.text = "Ollama also has llama3:70b loaded (38 GB). With both in memory, your model only partly fits on the card and replies will be slow.";
            aiChoice_.options = {"Unload the other models and continue", "Continue anyway", "Cancel"};
            ai_.busy = true;
            aiLiveStage_ = "Checking the graphics card";
        }
    }
    // Developer check: KESTRAL_ILLUS_TEST=<file without extension> draws a small assembly and saves its illustration.
    if (const char* it = std::getenv("KESTRAL_ILLUS_TEST")) {
        aiRunTool("create_box", R"({"x":0,"y":0,"z":0,"width":60,"depth":40,"height":6,"name":"Base plate"})");
        aiRunTool("create_box", R"({"x":-25,"y":0,"z":18,"width":6,"depth":40,"height":30,"name":"Upright"})");
        aiRunTool("create_cylinder", R"({"x":12,"y":0,"z":6,"diameter":16,"height":20,"name":"Post"})");
        aiRunTool("create_cylinder", R"({"x":12,"y":0,"z":26,"diameter":22,"height":4,"name":"Cap"})");
        illusTest_ = it;
        illus_.open = true;
        illus_.opt.numbers = true;
        illus_.opt.partsList = true;
        illus_.opt.explode = 0.5f;
    }
    // KESTRAL_SLICE_TEST=<printer>|<process>|<filament>: a 30 mm cube sliced from the Slice tab.
    if (const char* st = std::getenv("KESTRAL_SLICE_TEST")) {
        std::string v = st;
        size_t a = v.find('|'), b = v.find('|', a + 1);
        if (a != std::string::npos && b != std::string::npos) {
            aiRunTool("create_box", R"({"x":0,"y":0,"z":0,"width":30,"depth":30,"height":30,"name":"Cube"})");
            if (std::getenv("KESTRAL_SLICE_MULTI"))   // developer check: two colors, two filaments
                aiRunTool("create_box", R"({"x":40,"y":0,"z":0,"width":20,"depth":20,"height":20,"name":"Cube2"})");
            slice_.machine = v.substr(0, a);
            slice_.process = v.substr(a + 1, b - a - 1);
            slice_.filament = v.substr(b + 1);
            enterSliceWorkspace();
            sliceTestPending_ = true;
        }
    }

    lastFrame_ = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window_)) {
        bool lively = redrawFrames_ > 0 || cam_.animating() || renderer_.pickInFlight() || drag_ != Drag::None;
        bool busy = lively || jobs_->active() > 0 || !toasts_.empty();
        if (busy) {
            // Frame cap. Some drivers ignore vsync (an NVIDIA "Vertical sync: Off" setting), which
            // drew 1000+ frames a second and spun the fans up. Moving things: the screen's refresh
            // rate. Only a background job or a message showing: 15. Window in the background: 15.
            static double refreshHz = 0;
            if (refreshHz <= 0) {
                const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
                refreshHz = std::clamp(vm ? (double)vm->refreshRate : 60.0, 30.0, 360.0);
            }
            double fps = lively ? refreshHz : 15.0;
            if (!glfwGetWindowAttrib(window_, GLFW_FOCUSED) && mcpInput_.empty()) fps = std::min(fps, 15.0);
            if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) fps = 4.0;
            double deadline = lastPresentT_ + 1.0 / fps;
            glfwPollEvents();
            for (double now = glfwGetTime(); now < deadline - 0.0005; now = glfwGetTime())
                glfwWaitEventsTimeout(deadline - now);   // input still lands right away, frames wait
            lastPresentT_ = glfwGetTime();
        } else {
            // The slice tab checks the mouse often so focus follows it into OrcaSlicer quickly.
            glfwWaitEventsTimeout(sliceWs_ ? 0.05 : 0.5);
            redrawFrames_ = std::max(redrawFrames_, 1);
        }
        if (redrawFrames_ > 0) redrawFrames_--;

        wdBeatMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());   // idle waits are not hangs
        std::string ferr;
        if (!platform::guarded("frame", [&] { frame(); }, ferr)) {
            // Recover ImGui's internal stacks and keep running. Never exit on a UI error.
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            if (ctx && ctx->WithinFrameScope) {
                ImGui::ErrorRecoveryTryToRecoverState(&ctx->StackSizesInNewFrame);
                ImGui::EndFrame();
            }
            // The same error every frame would flood the toasts and the log: report it once, then
            // again only if it is still happening 10 seconds later.
            static std::string lastErr;
            static double lastErrT = -1e9;
            if (ferr != lastErr || time_ - lastErrT > 10.0) {
                toast("Recovered from an internal error: " + ferr, ToastKind::Warning);
                lastErr = ferr;
                lastErrT = time_;
            }
            if (ferr.find("device lost") != std::string::npos) {
                platform::messageBox("Kestral", "The graphics driver reset. Kestral will save a recovery copy and close the viewport.");
                emergencySave();
                break;
            }
        }
    }
    settings_.save(settingsPath_);
    // Clean exit marker: only autosaves newer than this are offered for recovery.
    try {
        std::error_code ec;
        std::filesystem::create_directories(u8path(platform::appDataDir()), ec);
        FILE* f = std::fopen((platform::appDataDir() + "/clean_exit").c_str(), "w");
        if (f) { std::fprintf(f, "clean"); std::fclose(f); }
    } catch (...) {}
    shutdown();
    return modelTestFail_ ? 1 : 0;
}

bool App::init(std::string& err) {
    std::string appData = platform::appDataDir();
    log::init(appData + "/kestral.log");
    log::info(std::string("Kestral ") + kVersion + ", OpenCASCADE " + kernel::kernelVersion());
    // A slicer engine downloaded last time becomes the one in use now.
    if (!modelTest_ && !uiTest_ && toolScript_.empty()) print::applyPendingEngine();
    settingsPath_ = appData + "/settings.ini";
    settings_.load(settingsPath_);
    if (!modelTest_ && !uiTest_ && toolScript_.empty()) moveOldDesigns();
    platform::installProcessCrashGuard([this] { emergencySave(); });
    platform::installThreadGuard();

    unsigned hw = std::thread::hardware_concurrency();
    jobs_ = std::make_unique<JobSystem>(hw > 4 ? hw - 2 : 2);

    glfwSetErrorCallback(cbGlfwError);
#ifdef _WIN32
    {
        // Windows 11 slows background apps down ("efficiency mode"). A long import should keep
        // full speed while you watch something else.
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        pt.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    }
#endif
    if (!glfwInit()) { err = "Could not initialize the window system"; return false; }
    if (!glfwVulkanSupported()) { err = "Vulkan is not available. Please update your graphics driver."; return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_MAXIMIZED, hiddenWindow_ ? GLFW_FALSE : GLFW_TRUE);
    if (hiddenWindow_) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifndef _WIN32
    // Lets the desktop match the window to kestral.desktop (icon and name in the dock).
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "kestral");
#endif
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "kestral");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "kestral");
#endif
    // Hidden: a fixed 1920 x 1080 window, so screenshots are the same every time.
    window_ = hiddenWindow_ ? glfwCreateWindow(1920, 1080, "Kestral", nullptr, nullptr) : glfwCreateWindow(1600, 950, "Kestral", nullptr, nullptr);
    if (!window_) { err = "Could not create the main window"; return false; }
    glfwSetCursorPosCallback(window_, cbCursor);
    glfwSetMouseButtonCallback(window_, cbButton);
    glfwSetScrollCallback(window_, cbScroll);
    glfwSetKeyCallback(window_, cbKey);
    glfwSetCharCallback(window_, cbChar);
    glfwSetFramebufferSizeCallback(window_, cbSize);
    glfwSetWindowFocusCallback(window_, cbFocus);
    glfwSetDropCallback(window_, cbDrop);
    glfwSetWindowCloseCallback(window_, cbClose);
#ifdef _WIN32
    if (HWND hw = glfwGetWin32Window(window_))
        g_prevWndProc = (WNDPROC)SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)kstWndProc);
#endif

    const char* val = std::getenv("KESTRAL_VALIDATION");
    if (!vk_.init(window_, val && *val == '1', err)) return false;
    if (!renderer_.init(&vk_, err)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // fixed Fusion style layout, nothing to persist
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    uiScale_ = settings_.uiScale > 0.f ? std::clamp(settings_.uiScale, 0.75f, 3.f) : autoUiScale();
    log::info("UI scale " + std::to_string(uiScale_));
    ui::applyTheme(uiScale_, settings_.darkMode ? settings_.darkness : 0.f);
    fonts_ = ui::loadFonts(uiScale_);
    ImGui_ImplGlfw_InitForVulkan(window_, true);
    uiitems::install();   // every item by name, for MCP apps
    if (!renderer_.initImGui()) { err = "Could not initialize the UI renderer"; return false; }

    cam_.ortho = !settings_.perspective;
    browserWidth_ = 290.f * uiScale_;
    registerTools();
    log::info("Ready");
    checkRecovery();
    startWatchdog();
    if (!modelTest_ && !uiTest_ && toolScript_.empty()) mcpStart();   // other AI apps (MCP)
    return true;
}

void App::notifyAssistantDone(const std::string& reply) {
    if (!settings_.aiNotify && !settings_.aiNotifySound) return;
    if (settings_.aiNotifyAway && window_ && glfwGetWindowAttrib(window_, GLFW_FOCUSED)) return;
    std::string text = reply;
    for (auto& c : text) if (c == '\n' || c == '*' || c == '#') c = c == '\n' ? ' ' : ' ';
    if (text.size() > 200) text = text.substr(0, 197) + "...";
    if (text.empty()) text = "The assistant is done.";
#ifdef _WIN32
    void* hw = window_ ? (void*)glfwGetWin32Window(window_) : nullptr;
#else
    void* hw = nullptr;
#endif
    platform::notifyUser(hw, "Kestral assistant", text, settings_.aiNotify, settings_.aiNotifySound ? settings_.aiNotifySoundKind : -1);
}

void App::shutdown() {
    mcpShutdown();
    platform::notifyShutdown();
    stopWatchdog();
    // "Restart Kestral" (after a slicer engine update): the new Kestral starts as this one ends,
    // with the design that was open.
    if (restartAfterExit_) {
        restartAfterExit_ = false;
        std::string err;
        if (!platform::launch(platform::selfExe(), doc_.path, err)) log::warn("Restart: " + err);
    }
    // A worker can be deep inside a huge file that cannot be interrupted. Never hang on exit for
    // it: everything is saved by now, so give it a moment and then end the process.
    if (jobs_ && jobs_->active() > 0) {
        if (window_) glfwHideWindow(window_);
        for (int i = 0; i < 30 && jobs_->active() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (jobs_->active() > 0) {
            log::info("Closing while a background job is still busy: ending it");
            platform::hardExit(0);
        }
    }
    jobs_.reset();   // joins workers first so no callback touches a dead app
    if (ImGui::GetCurrentContext()) {
        renderer_.shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    } else {
        renderer_.shutdown();
    }
    vk_.shutdown();
    if (window_) glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    g_app = nullptr;
}

void App::emergencySave() {
    // Structure only (no kernel calls): safe to run from the crash handler.
    try {
        std::error_code ec;
        std::filesystem::create_directories(u8path(platform::appDataDir() + "/autosave"), ec);
        saveDocument(doc_, platform::appDataDir() + "/autosave/crash.kst");
    } catch (...) {}
}

// ===================================================================== UI size

float App::autoUiScale() const {
    // The system's own scale (Windows display scaling, desktop scale on Linux), or, when that is
    // 100% on a big high resolution screen (a 4K TV), the screen's resolution: 1440 lines = 1.0.
    float xs = 1.f, ys = 1.f;
    glfwGetWindowContentScale(window_, &xs, &ys);
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window_, &wx, &wy);
    glfwGetWindowSize(window_, &ww, &wh);
    int cx = wx + ww / 2, cy = wy + wh / 2;
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* m = glfwGetVideoMode(mons[i]);
        if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height) { mon = mons[i]; break; }
    }
    const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
    float byRes = mode ? mode->height / 1440.f : 1.f;
    float s = std::max(xs, 1.f);
    if (xs <= 1.01f && byRes > 1.2f) s = byRes;
    return std::clamp(std::round(s * 20.f) / 20.f, 0.75f, 3.f);
}

void App::applyUiScale(float s) {
    if (std::abs(s - uiScale_) < 0.01f) return;
    float old = uiScale_;
    uiScale_ = s;
    ui::applyTheme(uiScale_, settings_.darkMode ? settings_.darkness : 0.f);
    browserWidth_ = browserWidth_ / old * s;
    log::info("UI scale now " + std::to_string(s));
    activity();
}

void App::dropFiles(int count, const char** paths) {
    for (int i = 0; i < count; ++i) {
        // Pictures go to the assistant (it models what they show); everything else is imported.
        if (isImageFile(paths[i])) aiAttachFile(paths[i]);
        else importPath(paths[i]);
    }
    activity();
}

void App::frame() {
    inFrame_ = true;
    struct Done { bool& f; ~Done() { f = false; } } done{inFrame_};
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastFrame_).count();
    lastFrame_ = now;
    dt = std::clamp(dt, 0.f, 0.25f);
    time_ += dt;
    if (dt > 0) fps_ = fps_ * 0.9f + (1.f / dt) * 0.1f;

    watchdogBeat("frame start");
    // Follow the screen: moving to another monitor or changing Windows' scale resizes the UI.
    if (time_ - lastScaleCheck_ > 0.5) {
        lastScaleCheck_ = time_;
        applyUiScale(settings_.uiScale > 0.f ? std::clamp(settings_.uiScale, 0.75f, 3.f) : autoUiScale());
    }
    if (jobs_->pumpMain() > 0) activity();
    {
        // Minimized (or squeezed to nothing for a moment on Alt+Tab): skip the interface entirely.
        // An ImGui frame with a zero size display clamps every open window into the top left
        // corner, and they stay there when the window comes back.
        int ww = 0, wh = 0;
        glfwGetWindowSize(window_, &ww, &wh);
        if (ww <= 0 || wh <= 0 || glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) return;
    }
    watchdogBeat("drawing the interface");
    if (cam_.update(dt)) activity();
    updateAutosave();

    // Latest GPU pick result becomes the hover target.
    const PickResult& pk = renderer_.lastPick();
    if (pk.valid) {
        bool over = viewportHovered_ || matDragOverView_;
        uint32_t hb = over ? pk.body : 0;
        uint32_t hf = over ? pk.face : 0;
        if (hb != hoverBody_ || hf != hoverFace_) { hoverBody_ = hb; hoverFace_ = hf; activity(); }
    }

    // KESTRAL_PROFILE=1: every 5 s, log where the frame time goes (interface, scene build, render).
    static const bool profile = std::getenv("KESTRAL_PROFILE") != nullptr;
    static double profUi = 0, profScene = 0, profRender = 0, profT0 = 0;
    static int profN = 0;
    auto profNow = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    double tUi0 = profile ? profNow() : 0;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    mcpFeedInput();   // mouse and keyboard from an MCP app, after the real ones
    uiitems::beginFrame();
    ImGui::NewFrame();
    if (modelTest_) modelTestStep();
    else if (uiTest_) uiTestStep();
    else if (!toolScript_.empty()) toolScriptStep();
    if (sk_.active) {
        auto it = doc_.sketches.find(sk_.sketchId);
        bool gone = it == doc_.sketches.end();
        if (!gone) {
            int fi = doc_.featureIndex(it->second.featureId);
            gone = fi < 0 || fi >= doc_.markerPos;
        }
        if (gone) leaveSketchMode();
    }
    if (!sliceWs_) handleShortcuts();
    drawUI();
    if (!sliceWs_) {
        handleViewportInput();
        drawSketchOverlay();
        drawInteractionOverlay();
    } else {
        handleSliceViewInput();
        if (sliceTestPending_ && !doc_.bodies.empty() && jobs_->active() == 0) {
            sliceTestPending_ = false;
            if (std::getenv("KESTRAL_SLICE_MULTI")) {
                aiRunTool("set_color", R"({"names":["Cube"],"color":"#2060FF"})");
                aiRunTool("set_color", R"({"names":["Cube2"],"color":"#FFD020"})");
            }
            if (std::getenv("KESTRAL_SLICE_PAINT")) {   // developer check: painting and per object settings reach the engine
                plateSync();
                uint32_t cube = 0, cube2 = 0;
                for (auto& [id, o] : slice_.plate) { if (o.name == "Cube") cube = id; if (o.name == "Cube2") cube2 = id; }
                if (cube && cube2) {
                    auto& o = slice_.plate[cube];
                    for (uint32_t f = 0; f < o.mesh->faceCount; ++f) o.paintColor[f] = slice_.plate[cube2].fil + 1;
                    o.settings["wall_loops"] = "5";
                    o.paintSupport[0] = 1;
                }
            }
            if (const char* ct = std::getenv("KESTRAL_CAL_TEST")) sliceStartCalibration(std::atoi(ct), 1);   // developer check
            startSliceJob();
        }
    }
    mcpDrawCursor();
    ImGui::Render();
    double tUi1 = profile ? profNow() : 0;

    if (sceneDirty_ || sceneRev_ != doc_.revision) rebuildScene();
    double tSc1 = profile ? profNow() : 0;

    ImGuiIO& io = ImGui::GetIO();
    VkExtent2D ext = vk_.extent;
    float aspect = ext.height ? (float)ext.width / (float)ext.height : 1.f;
    cam_.ortho = !settings_.perspective;
    FrameParams fp;
    fp.view = cam_.view();
    fp.proj = cam_.proj(aspect, sceneBounds_);
    fp.eye = cam_.eye();
    fp.forward = cam_.forward();
    fp.ortho = cam_.ortho;
    if (doc_.section.on) {
        int code = std::clamp(doc_.section.axis, 0, 2) + 1;
        if (doc_.section.flip) code = -code;
        fp.sectionAxis = (float)code;
        fp.sectionOffset = (float)doc_.section.offset;
    }
    // GPU preselect glow: a face, or a whole body. Edges and profiles are drawn as overlays.
    fp.hoverBody = 0;
    fp.hoverFace = 0;
    fp.hoverMode = 0;
    if (hov_.kind == HoverKind::Face) {
        fp.hoverBody = hov_.body;
        fp.hoverFace = hov_.index + 1;
        fp.hoverMode = 1;
    } else if (hov_.kind == HoverKind::Body) {
        fp.hoverBody = hov_.body;
    }
    if (sk_.choosingPlane) {
        fp.hoverBody = hoverBody_;
        fp.hoverFace = hoverFace_;
        fp.hoverMode = 1;
    }
    if (sliceWs_) {   // the plate: the part under the mouse glows (a face when laying it on one)
        bool prepare = sliceCurTop() == 1;
        fp.hoverBody = prepare && hoverBody_ >= 1000 ? hoverBody_ : 0;
        bool faces = slice_.layMode || slice_.paintMode;
        fp.hoverFace = prepare && faces ? hoverFace_ : 0;
        fp.hoverMode = prepare && faces ? 1 : 0;
    }
    if (matDrag_ >= 0) {   // dragging an appearance: show where it will land
        fp.hoverBody = matDragOverView_ ? hoverBody_ : 0;
        fp.hoverFace = hoverFace_;
        fp.hoverMode = !io.KeyCtrl && hoverFace_ > 0 ? 1 : 0;   // the face it lands on; Ctrl: the whole body
    }
    {
        float t[3], bt[3], g[3];
        ui::viewportColors(t, bt, g);
        fp.bgTop = {t[0], t[1], t[2]};
        fp.bgBottom = {bt[0], bt[1], bt[2]};
        fp.gridColor = {g[0], g[1], g[2]};
    }
    fp.edges = settings_.showEdges;
    fp.grid = settings_.showGrid && !sk_.active && !sliceWs_;
    fp.origin = settings_.showOrigin && !sliceWs_;   // the plate has its own grid
    float hh = cam_.halfHeightAtTarget();
    fp.gridMinor = std::pow(10.f, std::floor(std::log10(std::max(hh * 2.f / 25.f, 1e-3f))));
    fp.gridExtent = std::max(cam_.distance * 6.f, hh * aspect * 4.f) + sceneBounds_.diag();
    fp.pick = (viewportHovered_ && drag_ != Drag::Orbit && drag_ != Drag::Pan &&
               (!sk_.active || sk_.choosingPlane || sk_.tool == "project" || sk_.tool == "sk_intersect" || sk_.tool == "sk_onto_face")) ||
              matDragOverView_;
    fp.pickX = (int)(io.MousePos.x * io.DisplayFramebufferScale.x);
    fp.pickY = (int)(io.MousePos.y * io.DisplayFramebufferScale.y);
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    if (fbw == 0 || fbh == 0) return;   // minimized
    if ((uint32_t)fbw != ext.width || (uint32_t)fbh != ext.height) renderer_.requestResize();
    wdPhase_.store("rendering");
    renderer_.drawFrame(fp, ImGui::GetDrawData());
    if (profile) {
        double t2 = profNow();
        profUi += tUi1 - tUi0; profScene += tSc1 - tUi1; profRender += t2 - tSc1; ++profN;
        if (t2 - profT0 > 5.0 && profN > 0) {
            char b[200];
            std::snprintf(b, sizeof b, "profile: %d frames, interface %.1f ms, scene %.1f ms, render %.1f ms (avg), %zu bodies, %zu folders",
                          profN, profUi / profN * 1000, profScene / profN * 1000, profRender / profN * 1000, doc_.bodies.size(), doc_.components.size());
            log::info(b);
            profUi = profScene = profRender = 0; profN = 0; profT0 = t2;
        }
    }
}

// ===================================================================== scene

bool App::bodyShown(const Body& b) const {
    if (!isolated_.empty() && !isolated_.count(b.id)) return false;
    return doc_.isBodyShown(b);
}

void App::rebuildScene() {
    wdPhase_.store("building the 3D scene");
    if (sliceWs_ && (slice_.bed || (slice_.preview && slice_.paths))) { rebuildSliceScene(); return; }
    // Drop cached edge polylines of shapes no part uses any more.
    {
        std::set<const ShapeHandle*> live;
        for (auto& [pid, p] : doc_.parts) if (p.shape) live.insert(p.shape.get());
        for (auto it = edgeCache_.begin(); it != edgeCache_.end();)
            it = live.count(it->first) ? std::next(it) : edgeCache_.erase(it);
    }
    // Selections that point at bodies that no longer exist go away quietly.
    for (auto it = selFaces_.begin(); it != selFaces_.end();) it = doc_.bodies.count(it->first) ? std::next(it) : selFaces_.erase(it);
    for (auto it = selEdges_.begin(); it != selEdges_.end();) it = doc_.bodies.count(it->first) ? std::next(it) : selEdges_.erase(it);
    for (auto it = selProfiles_.begin(); it != selProfiles_.end();) it = doc_.sketches.count(it->first) ? std::next(it) : selProfiles_.erase(it);
    std::map<uint32_t, std::vector<uint32_t>> facesByBody;
    for (auto& f : selFaces_) facesByBody[f.first].push_back(f.second);
    std::vector<uint32_t> selFaceList;
    std::unordered_map<const MeshData*, size_t> index;
    std::vector<SceneBatch> batches;
    // Appearances, shared: one entry per distinct look, one face table per distinct (mesh, looks).
    std::vector<MaterialGpu> mats;
    std::map<std::string, uint32_t> matIndex;
    auto matOf = [&](const glm::vec3& c, const MatProps& m) -> uint32_t {
        MaterialGpu g;
        g.c0 = glm::vec4(c, std::clamp(m.opacity, 0.02f, 1.f));
        g.c1 = glm::vec4(m.color2, (float)(int)m.pattern);
        g.c2 = glm::vec4(std::clamp(m.metal, 0.f, 1.f), std::clamp(m.rough, 0.f, 1.f), std::max(m.scale, 0.01f), std::max(m.glow, 0.f));
        std::string key(reinterpret_cast<const char*>(&g), sizeof g);
        auto it = matIndex.find(key);
        if (it != matIndex.end()) return it->second;
        uint32_t id = (uint32_t)mats.size();
        mats.push_back(g);
        matIndex.emplace(std::move(key), id);
        return id;
    };
    std::vector<uint32_t> faceMats;
    std::map<std::tuple<const MeshData*, uint32_t, std::string>, uint32_t> tableIndex;
    for (auto& [id, b] : doc_.bodies) {
        // A hidden body hovered in the browser shows as a see-through ghost where it would be.
        bool ghost = false;
        if (!bodyShown(b)) {
            if (browserHover_.count(b.id) && doc_.isBodyAlive(b)) ghost = true;
            else continue;
        }
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end() || !pit->second.mesh || pit->second.mesh->indices.empty()) continue;
        const MeshData* key = pit->second.mesh.get();
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, batches.size()).first;
            batches.push_back({pit->second.mesh, {}});
        }
        RenderInstance ri;
        ri.xform = b.xform;
        ri.color = glm::vec4(b.color, 1.f);
        ri.bodyId = b.id;
        ri.material = matOf(b.color, b.mat);
        ri.transparent = b.mat.opacity < 0.999f;
        {
            const MeshData& md = *pit->second.mesh;
            bool fileColors = b.mat.fileColors && !md.faceColors.empty() && md.faceColors.size() == md.faceCount;
            if ((fileColors || !b.faceLooks.empty()) && md.faceCount > 0) {
                std::string looksKey;
                std::vector<std::pair<uint32_t, uint32_t>> looks;
                for (auto& [face, fl] : b.faceLooks) {
                    if (face >= md.faceCount) continue;
                    uint32_t mi = matOf(fl.color, fl.mat);
                    looks.push_back({face, mi});
                    looksKey += std::to_string(face) + ":" + std::to_string(mi) + ",";
                    if (fl.mat.opacity < 0.999f) ri.transparent = true;
                }
                auto tkey = std::make_tuple(key, ri.material, looksKey);
                auto tit = tableIndex.find(tkey);
                if (tit != tableIndex.end()) {
                    ri.faceTable = tit->second;
                } else {
                    uint32_t start = (uint32_t)faceMats.size();
                    faceMats.resize(faceMats.size() + md.faceCount, 0u);
                    if (fileColors)
                        for (uint32_t fi = 0; fi < md.faceCount; ++fi) {
                            uint32_t fc = md.faceColors[fi];
                            if ((fc >> 24) == 0) continue;
                            glm::vec3 c((fc & 255) / 255.f, ((fc >> 8) & 255) / 255.f, ((fc >> 16) & 255) / 255.f);
                            faceMats[start + fi] = matOf(c, b.mat) + 1;
                        }
                    for (auto& [face, mi] : looks) faceMats[start + face] = mi + 1;
                    ri.faceTable = start + 1;
                    tableIndex.emplace(tkey, ri.faceTable);
                }
            }
        }
        ri.flags = (selection_.count(b.id) ? kInstSelected : 0u) | (browserHover_.count(b.id) ? kInstHover : 0u);
        if (ghost) {
            MatProps gm = b.mat;
            gm.opacity = 0.38f;
            ri.material = matOf(b.color, gm);
            ri.transparent = true;
            ri.faceTable = 0;
        }
        auto fit = facesByBody.find(b.id);
        if (fit != facesByBody.end()) {
            ri.selStart = (uint32_t)selFaceList.size();
            ri.selCount = (uint32_t)std::min<size_t>(fit->second.size(), 255);
            selFaceList.insert(selFaceList.end(), fit->second.begin(), fit->second.begin() + ri.selCount);
        }
        batches[it->second].instances.push_back(ri);
    }
    renderer_.setScene(batches, selFaceList, mats, faceMats);
    sceneBounds_ = Aabb();
    for (auto& [id, b] : doc_.bodies) {
        if (!bodyShown(b)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit != doc_.parts.end() && pit->second.mesh) sceneBounds_.add(pit->second.mesh->bounds.transformed(b.xform));
    }
    sceneDirty_ = false;
    sceneRev_ = doc_.revision;
    activity();
}

// ===================================================================== tools

void App::registerTools() {
    using ui::Icon;
    auto add = [&](std::string id, std::string label, std::string sc, Icon icon, int ms, std::function<void()> run = {}, std::string tip = {}) {
        tools_.push_back({std::move(id), std::move(label), std::move(sc), icon, ms, std::move(run), std::move(tip)});
        return (int)tools_.size() - 1;
    };
    // CREATE
    int newComp = add("new_component", "New Component", "", Icon::NewComponent, 0, [this] { runNewComponent(); },
                      "Make a component and activate it. New bodies go into the active component");
    int ground = add("ground", "Ground", "", Icon::RigidGroup, 0, [this] { runGround(); },
                     "Lock the selected body's component so a move or joint cannot shift it");
    int sketch = add("sketch", "Create Sketch", "", Icon::Sketch, 0, [this] { beginCreateSketch(); }, "Start a sketch on a plane or flat face");
    int extrude = add("extrude", "Extrude", "E", Icon::Extrude, 0, [this] { runExtrude(); },
                      "Turn a closed sketch profile into a solid (E)");
    int revolve = add("revolve", "Revolve", "", Icon::Revolve, 0, [this] { runRevolve(); },
                      "Spin a closed sketch profile around an axis");
    int sweep = add("sweep", "Sweep", "", Icon::Sweep, 0, [this] { runSweep(); },
                    "A closed profile swept along a path. A guide curve scales the profile as it goes");
    int loft = add("loft", "Loft", "", Icon::Loft, 0, [this] { runLoft(); },
                   "A smooth solid through several sketch profiles");
    int rib = add("rib", "Rib", "", Icon::Rib, 0, [this] { runRib(); },
                  "A thin wall from an open sketch, grown along the sketch normal until it meets a body");
    int web = add("web", "Web", "", Icon::Extrude, 0, [this] { runWeb(); },
                  "The sketch is the side of a wall. Thickness sticks out of the plane, and the wall grows across the plane until it meets a body");
    int emboss = add("emboss", "Emboss", "", Icon::PressPull, 0, [this] { runEmboss(); },
                     "Project a closed sketch onto a body and raise it, or deboss to cut it in");
    int boss = add("boss", "Boss", "", Icon::Cylinder, 0, [this] { runBoss(); },
                   "A round post on a face. Hole 0 is solid");
    int snap = add("snap", "Snap Fit", "", Icon::Joint, 0, [this] { runSnap(); },
                   "A cantilever hook on a face. Catch 0 is a plain beam");
    int rest = add("rest", "Rest", "", Icon::Box, 0, [this] { runRest(); },
                   "A flat pad. The sketch is one face, and the thickness joins a body");
    int lip = add("lip", "Lip", "", Icon::Offset, 0, [this] { runLip(); },
                  "A ridge around a closed sketch. Groove cuts that ridge into the body");
    int sil = add("silhouette", "Silhouette Split", "", Icon::SplitBody, 0, [this] { runSilhouette(); },
                  "Cut a body on the outline seen along X, Y, or Z");
    int coil = add("coil", "Coil", "", Icon::Revolve, 0, [this] { runCoil(); },
                   "A round wire wound into a spring. Diameter is the centerline");
    int pipe = add("pipe", "Pipe", "", Icon::Sweep, 0, [this] { runPipe(); },
                   "A tube along a sketch path, or a straight line. Inside 0 is solid");
    int hole = add("hole", "Hole", "H", Icon::Hole, 0, [this] { runHole(); },
                   "Drill holes through sketch circles. Spotface, drill point, and a tapped thread");
    int thread = add("thread", "Thread", "", Icon::Thread, 0, [this] { runThread(); },
                     "Cut a smooth helical thread into a shaft or hole, or draw a cosmetic one (every standard)");
    int gear = add("gear", "Gear", "", Icon::Gear, 0, [this] { runGear(); },
                   "Spur, helical, herringbone, ring, rack, bevel, worm and worm wheel gears");
    int wing = add("wing", "Wing", "", Icon::Loft, 0, [this] { runWing(); },
                   "Fixed wing from airfoils: panels with sweep, dihedral and twist, optional printable shell");
    int blade = add("blades", "Blades", "", Icon::CircPattern, 0, [this] { runBlade(); },
                    "Propellers, helicopter rotors, ducted fans, wind turbines and boat propellers from airfoils");
    int dovetail = add("dovetail", "Dovetail", "", Icon::Combine, 0, [this] { runDovetail(); },
                       "Dovetail keys, tabs, rails, puzzle joints, woodworking corner joints and machine slides");
    int box = add("box", "Box", "", Icon::Box, 0, [this] { runBox(); },
                  "A 10 mm box on the XY plane. Edit the extrude or the sketch to resize it");
    int cyl = add("cylinder", "Cylinder", "", Icon::Cylinder, 0, [this] { runCylinder(); },
                  "A cylinder, diameter 10 and height 10, on the XY plane");
    int sphere = add("sphere", "Sphere", "", Icon::Sphere, 0, [this] { runSphere(); },
                     "A sphere, diameter 10, centered on the origin");
    int cone = add("cone", "Cone", "", Icon::Cone, 0, [this] { runCone(); },
                   "A cone, bottom diameter 10 and height 10, point up, on the XY plane");
    int torus = add("torus", "Torus", "", Icon::Torus, 0, [this] { runTorus(); },
                    "A torus, centerline diameter 20 and tube diameter 4");
    int wedge = add("wedge", "Wedge", "", Icon::Wedge, 0, [this] { runWedge(); },
                    "A right triangular prism, 10 mm on each side");
    int tube = add("tube", "Tube", "", Icon::Tube, 0, [this] { runTube(); },
                   "A tube, diameter 10, hole 6 and height 10, on the XY plane");
    int rpat = add("rect_pattern", "Rectangular Pattern", "", Icon::RectPattern, 0, [this] { runRectPattern(); },
                   "Copy selected bodies in a grid. Instanced, so 500 copies cost nothing");
    int cpat = add("circ_pattern", "Circular Pattern", "", Icon::CircPattern, 0, [this] { runCircPattern(); },
                   "Copy selected bodies around an axis");
    int ppat = add("path_pattern", "Pattern on Path", "", Icon::Sweep, 0, [this] { runPathPattern(); },
                   "Copy selected bodies along a sketch curve or an edge");
    int mirror = add("solid_mirror", "Mirror", "", Icon::Mirror, 0, [this] { runMirror(); },
                     "Mirrored copies of the selected bodies across a plane");
    // MODIFY
    int press = add("press_pull", "Press Pull", "Q", Icon::PressPull, 0, [this] { runPressPull(); },
                    "Move a face: hover a flat face and press Q");
    int fillet = add("fillet", "Fillet", "F", Icon::Fillet, 0, [this] { runFillet(); },
                     "Round edges, or replace a face with a full round between the two faces beside it");
    int chamfer = add("chamfer", "Chamfer", "Shift+C", Icon::Chamfer, 0, [this] { runChamfer(); },
                      "Bevel edges. A second distance cuts the other face by a different amount");
    int shell = add("shell", "Shell", "", Icon::Shell, 0, [this] { runShell(); },
                    "Hollow the body. Direction is inside, outside, or both");
    int thicken = add("thicken", "Thicken", "", Icon::Shell, 0, [this] { runThicken(); },
                      "Give faces a thickness and make a new solid. The body you picked stays");
    int draft = add("draft", "Draft", "", Icon::Draft, 0, [this] { runDraft(); },
                    "Lean the side faces so a mold can release. The base stays put");
    int scale = add("scale", "Scale", "", Icon::Scale, 0, [this] { runScale(); },
                    "Scale the selected bodies about their centers");
    int combine = add("combine", "Combine", "", Icon::Combine, 0, [this] { runCombine(); },
                      "Join, cut or intersect existing bodies (last clicked = tool)");
    int offFace = add("offset_face", "Offset Face", "", Icon::OffsetFace, 0, [this] { runPressPull(); },
                      "Move faces in or out");
    int replFace = add("replace_face", "Replace Face", "", Icon::OffsetFace, 0, [this] { runReplaceFace(); },
                       "Move flat faces onto an origin plane");
    int split = add("split_body", "Split Body", "", Icon::SplitBody, 0, [this] { runSplitBody(); },
                    "Cut a body into pieces with a plane");
    int splitFace = add("split_face", "Split Face", "", Icon::SplitBody, 0, [this] { runSplitFace(); },
                        "Divide faces where a plane or a sketch curve crosses them. The body stays one piece");
    int move = add("move", "Move/Copy", "M", Icon::MoveCopy, 0, [this] { runMove(); },
                   "Move the selected bodies by X/Y/Z (or copy them)");
    int align = add("align", "Align", "", Icon::Align, 0, [this] { runAlign(); },
                    "Move bodies so a face center, edge middle or corner lands on another");
    int params = add("parameters", "Change Parameters", "", Icon::Parameters, 0, [this] { openParamsDialog(); },
                     "User parameters you can use in any dimension or distance");
    int appear = add("appearance", "Appearance", "A", Icon::Appearance, 0, [this] {
        appearanceOpen_ = !appearanceOpen_;
        appearanceUndoPushed_ = false;
        if (appearanceOpen_ && !selFaces_.empty() && selection_.empty()) matApplyFaces_ = 1;
    }, "Colors, metals, glass, wood, carbon fiber and more. Click one to apply it to the selection, or drag it onto the model");
    // ASSEMBLE
    int joint = add("joint", "Joint", "J", Icon::Joint, 0, [this] { runJoint(); },
                    "Move a body so a point on it lands on a point on another. Revolute turns it. Slider slides it along an axis");
    int asbuilt = add("asbuilt_joint", "As-built Joint", "Shift+J", Icon::AsBuiltJoint, 0, [this] { runAsBuilt(); },
                      "Lock two bodies that are already in place. Neither one moves");
    int rigid = add("rigid_group", "Rigid Group", "", Icon::RigidGroup, 0, [this] { runRigid(); },
                    "Lock bodies together. They stay put, and moving one moves the rest");
    // CONSTRUCT
    int offPlane = add("offset_plane", "Offset Plane", "", Icon::OffsetPlane, 0, [this] { runOffsetPlane(); },
                       "A construction plane parallel to an origin plane, for sketching");
    int anglePlane = add("angle_plane", "Plane at Angle", "", Icon::AnglePlane, 0, [this] { runAnglePlane(); },
                         "A construction plane tilted from an origin plane, around that plane's X or Y");
    int midPlane = add("mid_plane", "Midplane", "", Icon::MidPlane, 0, [this] { runMidPlane(); },
                       "A construction plane halfway between an origin plane and another plane");
    int threePlane = add("plane_3pt", "3 Point Plane", "", Icon::OffsetPlane, 0, [this] { runThreePointPlane(); },
                         "A construction plane through three points. The first point is the origin");
    int tanPlane = add("plane_tangent", "Tangent Plane", "", Icon::AnglePlane, 0, [this] { runTangentPlane(); },
                       "A construction plane tangent to a cylinder. The angle picks which side");
    int axis = add("axis", "Axis", "", Icon::Axis, 0, [this] { runAxis(); },
                   "A construction axis through two points, a straight edge, or a cylinder");
    int point = add("point", "Point", "", Icon::Point, 0, [this] { runPoint(); },
                    "A construction point at coordinates, at the middle of an edge, at the center of a flat face, or on a corner");
    // INSPECT
    int measure = add("measure", "Measure", "I", Icon::Measure, 0, [this] { runMeasure(); },
                      "Distance, angle, radius, area, or the length of a loop");
    int interf = add("interference", "Interference", "", Icon::Interference, 0, [this] { runInterference(); },
                     "Check the selected bodies for overlaps");
    int section = add("section", "Section Analysis", "", Icon::Section, 0, [this] { runSection(); },
                      "Cut the view on an origin plane. The solids stay whole");
    // INSERT
    int insMesh = add("insert_mesh", "Insert Mesh (STL/OBJ)", "", Icon::InsertMesh, 0, [this] { importDialog(); }, "Insert an STL or OBJ mesh as a body");
    int insStep = add("insert_step", "Insert STEP", "Ctrl+I", Icon::InsertStep, 0, [this] { importDialog(); }, "Insert a STEP model (keeps components, names and colors)");
    // SELECT
    int sel = add("select", "Select", "", Icon::Select, 0, [this] { navMode_ = NavMode::None; }, "Normal selection");
    int selPrio = add("select_priority", "Toggle Face / Body Priority", "", Icon::WindowSelect, 0, [this] {
        settings_.bodyPriority = !settings_.bodyPriority;
        clearSelection();
        toast(settings_.bodyPriority ? "Selection: body priority (clicks pick whole bodies)" : "Selection: face priority (clicks pick faces, edges and profiles)");
    }, "Switch between picking faces / edges and whole bodies");
    // 3D PRINT
    int slicer = add("send_to_slicer", "Send to Slicer", "Ctrl+P", Icon::Slicer, 0, [this] { sendToSlicer(); },
                     "Export selected (or all visible) bodies and open them in your slicer");
    int illusT = add("illustration", "Illustration", "", Icon::Illustration, 0, [this] { illus_.open = true; },
                     "A clean picture of the design for instructions, posts or the README: isometric or any view, shaded, "
                     "flat tones or line art, exploded, with numbered parts. Saves PNG or SVG.");
    int stl = add("export_stl", "Export STL", "", Icon::ExportStl, 0, [this] { exportStlDialog(); }, "Save visible or selected bodies as STL");
    int slicerSetup = add("slicer_setup", "Choose Slicer...", "", Icon::Gear, 0, [this] { settingsOpen_ = true; }, "Pick which slicer Send to Slicer opens");
    // Sketch tools
    auto sk = [&](const char* id, const char* label, const char* sc, Icon ic, const char* tip = "") {
        std::string sid = id;
        return add(id, label, sc, ic, 0, [this, sid] { sketchToolCommand(sid); }, tip);
    };
    auto later = [&](const char* id, const char* label, const char* sc, Icon ic) {
        std::string l = label;
        return add(id, label, sc, ic, 0, [this, l] { toast(l + " arrives in the next sketch update (M2b)", ToastKind::Info); });
    };
    int tLine = sk("line", "Line", "L", Icon::Line, "Click points to draw connected lines");
    int tRect = sk("rectangle", "2-Point Rectangle", "R", Icon::Rectangle);
    int tRectC = sk("rect_center", "Center Rectangle", "", Icon::CenterRect);
    int tRect3 = sk("rect_3pt", "3-Point Rectangle", "", Icon::Rectangle, "Two points for one side, then a point for the width");
    int tCircle = sk("circle", "Center Diameter Circle", "C", Icon::Circle);
    int tEll = sk("ellipse", "Ellipse", "", Icon::Circle, "Center, the end of the long axis, then a point on the short axis");
    int tEllArc = sk("ellipse_arc", "Elliptical Arc", "", Icon::Arc, "Center, the long axis, the short axis, then the start and the end");
    int tCirc2 = sk("circle_2pt", "2-Point Circle", "", Icon::Circle);
    int tCirc3 = sk("circle_3pt", "3-Point Circle", "", Icon::Circle);
    int tArc = sk("arc", "3-Point Arc", "", Icon::Arc);
    int tArcC = sk("arc_center", "Center Point Arc", "", Icon::Arc);
    int tPoly = sk("polygon", "Circumscribed Polygon", "", Icon::Polygon, "Center, then the middle of a side. Turn Circumscribed off to put the corners on the circle");
    int tPolyIn = sk("polygon_in", "Inscribed Polygon", "", Icon::Polygon, "Center, then a corner. The polygon sits inside the circle");
    int tSlot = sk("slot", "Center to Center Slot", "", Icon::Slot, "Two center points, then the width");
    int tSlotO = sk("slot_overall", "Overall Slot", "", Icon::Slot, "The two outer ends, then the width. The length includes the rounded caps");
    int tSlotC = sk("slot_center", "Center Point Slot", "", Icon::Slot, "The center, then the center of one cap. The other cap mirrors across");
    int tSlotA = sk("slot_arc", "3-Point Arc Slot", "", Icon::Slot, "Start, a point on the arc, the end, then the width");
    int tSpline = sk("spline", "Fit Point Spline", "", Icon::Spline, "Click fit points. Click the start point to close, or press Enter to finish open");
    int tCtrl = sk("control_spline", "Control Point Spline", "", Icon::Spline, "Click control points. The curve is pulled toward them. Click the start to close, or press Enter to finish open");
    int tText = sk("text", "Text", "", Icon::Dimension, "Type letters, pick a font, then click to place. Click the letters again to edit them.");
    int tPoint = add("sk_point", "Point", "", Icon::Point, 0, [this] { sketchToolCommand("point"); }, "Click to place a sketch point");
    int tCross = sk("cross_point", "Crossing", "", Icon::CrossPoint, "Click where two curves cross. The point stays on both");
    int tSkFillet = sk("sk_fillet", "Fillet", "", Icon::Fillet, "Click a corner where two lines meet, then type the radius");
    int tSkCham = sk("sk_chamfer", "Chamfer", "", Icon::Chamfer, "Click a corner where two lines meet, then type the distance");
    int tTrim = sk("trim", "Trim", "T", Icon::Trim, "Click a curve segment to trim it");
    int tBreak = sk("break", "Break", "", Icon::Break, "Click a curve to split it where other curves cross it");
    int tXform = sk("sk_xform", "Move / Copy", "", Icon::MoveCopy, "Select curves, then move, rotate, or scale them. Copy leaves the originals");
    int tPat = sk("sk_pattern", "Pattern", "", Icon::RectPattern, "Select curves, then make a rectangular or circular pattern. The count includes the original");
    int tExtend = sk("extend", "Extend", "", Icon::Trim, "Click near the end of a line to extend it to the next curve");
    int tOffset = sk("offset", "Offset", "O", Icon::Offset, "Click a curve (its whole chain is used), move to the side, type the distance");
    int tMirrorS = sk("mirror", "Mirror", "", Icon::Mirror, "Select curves, then pick the mirror line");
    int tProject = sk("project", "Project", "P", Icon::Project, "Click body edges or faces to bring them into the sketch");
    int tInter = sk("sk_intersect", "Intersect", "", Icon::Intersect, "Click a body. The outline where it crosses this plane is added to the sketch");
    int tOnto = sk("sk_onto_face", "Onto Face", "", Icon::ProjectFace, "Select curves, then click a face. They drop onto it along the sketch normal");
    int tDim = sk("dimension", "Sketch Dimension", "D", Icon::Dimension, "Click geometry, then place the dimension");
    int tConstr = sk("construction", "Construction", "X", Icon::Construction);
    int cCoin = sk("c_coincident", "Coincident", "", Icon::ConCoincident);
    int cCol = sk("c_collinear", "Collinear", "", Icon::ConCollinear);
    int cConc = sk("c_concentric", "Concentric", "", Icon::ConConcentric);
    int cMid = sk("c_midpoint", "Midpoint", "", Icon::ConMidpoint);
    int cFix = sk("c_fix", "Fix/Unfix", "", Icon::ConFix);
    int cPar = sk("c_parallel", "Parallel", "", Icon::ConParallel);
    int cPerp = sk("c_perpendicular", "Perpendicular", "", Icon::ConPerpendicular);
    int cHV = sk("c_hv", "Horizontal/Vertical", "", Icon::ConHorizontal);
    int cTan = sk("c_tangent", "Tangent", "", Icon::ConTangent);
    int cEq = sk("c_equal", "Equal", "", Icon::ConEqual);
    int cSym = sk("c_symmetric", "Symmetric", "", Icon::ConSymmetric);
    int tFinish = add("finish_sketch", "Finish Sketch", "", Icon::FinishSketch, 0, [this] { finishSketch(); }, "Leave sketch mode");
    sketchGroups_ = {
        {"CREATE", {tLine, tRect, tRectC, tRect3, tCircle, tCirc2, tCirc3, tEll, tEllArc, tArc, tArcC, tPoly, tPolyIn, tSlot, tSlotO, tSlotC, tSlotA, tPoint, tCross, tSpline, tCtrl, tText}, 4},
        {"MODIFY", {tSkFillet, tSkCham, tTrim, tBreak, tXform, tPat, tOffset, tExtend, tMirrorS, tProject, tInter, tOnto}, 3},
        {"CONSTRAINTS", {cCoin, cHV, cPar, cPerp, cTan, cEq, cCol, cConc, cMid, cFix, cSym}, 4},
        {"DIMENSION", {tDim, tConstr}, 2},
        {"FINISH", {tFinish}, 1},
    };
    // Utilities
    add("settings", "Preferences", "", Icon::Settings, 0, [this] { settingsOpen_ = true; }, "Kestral preferences");
    add("fit", "Fit", "F6", Icon::Fit, 0, [this] { fitView(false); }, "Fit the model to the screen");
    add("home_view", "Home View", "Home", Icon::Home, 0, [this] {
        float a = vk_.extent.height ? (float)vk_.extent.width / vk_.extent.height : 1.f;
        cam_.home(sceneBounds_, a);
    }, "Return to the home view");
    add("toggle_visibility", "Toggle Visibility", "V", Icon::Eye, 0, [this] {
        toggleVisibility(std::vector<uint32_t>(selection_.begin(), selection_.end()));
    }, "Hide or show the selected bodies");
    add("isolate", "Isolate", "", Icon::Eye, 0, [this] {
        isolate(std::vector<uint32_t>(selection_.begin(), selection_.end()));
    }, "Show only the selected bodies");
    add("show_all", "Show All", "", Icon::Eye, 0, [this] { showAll(); }, "Make every body visible");

    // Commands with no toolbar button: here so their shortcuts can be changed like any other.
    auto cmd = [&](const char* id, const char* label, const char* sc, Icon ic, std::function<void()> run, const char* tip = "") {
        int i = add(id, label, sc, ic, 0, std::move(run), tip);
        tools_[i].global = true;
        return i;
    };
    cmd("undo", "Undo", "Ctrl+Z", Icon::Undo, [this] { if (dlg_.open || tdlg_.open) commandCancel(); else undo(); });
    cmd("redo", "Redo", "Ctrl+Y", Icon::Redo, [this] { if (!(dlg_.open || tdlg_.open)) redo(); });
    cmd("save", "Save", "Ctrl+S", Icon::Save, [this] { saveDocumentNow(); });
    cmd("save_as", "Save As", "Ctrl+Shift+S", Icon::Save, [this] { saveDocumentAs(); });
    cmd("open", "Open", "Ctrl+O", Icon::File, [this] { openDocumentDialog(); });
    cmd("new_design", "New Design", "Ctrl+N", Icon::File, [this] {
        if (doc_.bodies.empty() && doc_.timeline.empty()) newDocument();
        else confirmNew_ = true;
    });
    cmd("compute_all", "Compute All", "Ctrl+B", Icon::Gear, [this] {
        for (auto& ft : doc_.timeline) ft.chainSig = 0;
        scheduleRecompute(0);
        toast("Recomputing everything");
    }, "Rebuild every feature from scratch");
    cmd("select_all", "Select All", "Ctrl+A", Icon::Select, [this] { selectAll(); });
    cmd("copy", "Copy", "Ctrl+C", Icon::MoveCopy, [this] { copySelection(); }, "Copy the selected bodies");
    cmd("paste", "Paste", "Ctrl+V", Icon::MoveCopy, [this] { pasteClipboard(); }, "Paste copies of the copied bodies and move them");
    cmd("toolbox", "Shortcuts Toolbox", "S", Icon::Palette, [this] { paletteOpen_ = true; paletteBuf_[0] = 0; }, "Search any command");
    cmd("toggle_browser", "Show/Hide Browser", "Ctrl+Alt+B", Icon::Folder, [this] { browserOpen_ = !browserOpen_; });
    cmd("toggle_viewcube", "Show/Hide ViewCube", "Ctrl+Alt+V", Icon::Home, [this] { showViewCube_ = !showViewCube_; });
    cmd("toggle_navbar", "Show/Hide Navigation Bar", "Ctrl+Alt+N", Icon::Orbit, [this] { showNavBar_ = !showNavBar_; });
    cmd("keyboard_shortcuts", "Keyboard Shortcuts", "", Icon::Settings, [this] { keysOpen_ = true; keyFilter_[0] = 0; },
        "See and change every keyboard shortcut");
    cmd("slice_workspace", "Slice in OrcaSlicer", "", Icon::Slicer, [this] { enterSliceWorkspace(); },
        "OrcaSlicer inside Kestral, one click from your model");
    cmd("robot_export", "Robot Export (URDF / USD)", "", Icon::Joint, [this] { robotOpen_ = true; },
        "Links, joints and masses for Isaac Sim / Isaac Lab and ROS");
    cmd("move_from_fusion", "Move from Fusion", "", Icon::ImportFeature, [this] { fusionMoveOpen_ = true; },
        "Bring every Fusion design over in one go");
    cmd("assistant", "Assistant", "Ctrl+Space", Icon::Palette, [this] { ai_.open = !ai_.open; ai_.focus = ai_.open; },
        "AI helper: ask how to do anything, or tell it what to change (Claude or a local model)");
    for (const char* g : {"fit", "home_view", "toggle_visibility", "isolate", "show_all", "measure", "insert_step",
                          "insert_mesh", "send_to_slicer", "settings", "select_priority", "export_stl", "parameters"})
        if (int i = findTool(g); i >= 0) tools_[i].global = true;

    solidGroups_ = {
        {"CREATE", {newComp, sketch, extrude, revolve, sweep, loft, rib, web, emboss, boss, snap, rest, lip, sil, coil, pipe, hole, thread, gear, wing, blade, dovetail, box, cyl, sphere, cone, torus, wedge, tube, rpat, cpat, ppat, mirror}, 3},
        {"MODIFY", {press, fillet, chamfer, shell, thicken, draft, scale, combine, offFace, replFace, split, splitFace, move, align, params, appear}, 3},
        {"ASSEMBLE", {newComp, ground, joint, asbuilt, rigid}, 2},
        {"CONSTRUCT", {offPlane, anglePlane, midPlane, threePlane, tanPlane, axis, point}, 1},
        {"INSPECT", {measure, interf, section}, 2},
        {"INSERT", {insMesh, insStep}, 2},
        {"SELECT", {sel, selPrio}, 1},
        {"3D PRINT", {slicer, stl, slicerSetup}, 1},
    };
    utilGroups_ = {
        {"MAKE", {findTool("slice_workspace"), slicer, stl, slicerSetup}, 2},
        {"SHARE", {illusT}, 1},
        {"SETTINGS", {findTool("settings"), findTool("keyboard_shortcuts")}, 2},
        {"AI", {findTool("assistant")}, 1},
    };
    for (auto& t : tools_) t.defShortcut = t.shortcut;
    applyKeyBindings();
}

int App::findTool(const std::string& id) const {
    for (int i = 0; i < (int)tools_.size(); ++i)
        if (tools_[i].id == id) return i;
    return -1;
}

void App::notYet(const Tool& t) {
    toast(t.label + " arrives in milestone M" + std::to_string(t.milestone) + ".", ToastKind::Info);
}

void App::runTool(const std::string& id) {
    int i = findTool(id);
    if (i < 0) return;
    const Tool& t = tools_[i];
    lastToolId_ = id;
    if (!t.available()) { notYet(t); return; }
    std::string err;
    platform::guarded(t.label.c_str(), t.run, err);
    if (!err.empty()) toast(t.label + " failed: " + err, ToastKind::Error);
}

// ===================================================================== document ops

void App::pushUndo(const std::string& label) {
    undo_.push(doc_, label);
    dirty_ = true;
}

void App::undo() {
    if (!undo_.canUndo()) return;
    std::string label = undo_.undoLabel();
    undo_.undo(doc_);
    sketchAfterHistory();
    selection_.clear();
    selOrder_.clear();
    selFaces_.clear();
    selEdges_.clear();
    selProfiles_.clear();
    markSceneDirty();
    (void)label;
}

void App::redo() {
    if (!undo_.canRedo()) return;
    std::string label = undo_.redoLabel();
    undo_.redo(doc_);
    sketchAfterHistory();
    selection_.clear();
    selOrder_.clear();
    selFaces_.clear();
    selEdges_.clear();
    selProfiles_.clear();
    markSceneDirty();
    (void)label;
}

void App::stampActiveComponent(uint32_t featureId) {
    if (!featureId) return;
    uint32_t id = doc_.activeComponent;
    if (!id || id == doc_.rootId || !doc_.components.count(id)) return;
    doc_.featureFolder[featureId] = id;
}

void App::rememberComponentName(uint32_t cid) {
    // Folders a feature makes (imports) are made again on every rebuild: keep the name the user gave.
    auto it = doc_.components.find(cid);
    if (it == doc_.components.end() || !it->second.featureId) return;
    uint32_t slot = 0;
    for (auto& [id, c] : doc_.components) {
        if (c.featureId != it->second.featureId) continue;
        if (id == cid) break;
        ++slot;
    }
    doc_.compNames[Document::overrideKey(it->second.featureId, slot)] = it->second.name;
}

void App::activateComponent(uint32_t id) {
    if (!doc_.components.count(id)) id = doc_.rootId;
    doc_.activeComponent = id;
    doc_.touch();
    dirty_ = true;
    toast("Activated " + doc_.components[id].name);
}

void App::groundComponent(uint32_t id, bool on) {
    auto it = doc_.components.find(id);
    if (it == doc_.components.end() || id == doc_.rootId) {
        toast("The root design cannot be grounded");
        return;
    }
    pushUndo(on ? "Ground" : "Unground");
    it->second.grounded = on;
    doc_.touch();
    dirty_ = true;
    toast((on ? "Grounded " : "Ungrounded ") + it->second.name);
}

void App::runNewComponent() {
    pushUndo("New Component");
    Component c;
    c.id = doc_.newId();
    auto taken = [&](const std::string& name) {
        for (auto& [id, ex] : doc_.components)
            if (toLower(ex.name) == toLower(name)) return true;
        return false;
    };
    for (int n = 1;; ++n) {
        std::string name = "Component" + std::to_string(n);
        if (!taken(name)) { c.name = name; break; }
    }
    uint32_t parent = doc_.activeComponent && doc_.components.count(doc_.activeComponent) ? doc_.activeComponent : doc_.rootId;
    if (parent == c.id) parent = doc_.rootId;
    c.parentId = parent == 0 ? doc_.rootId : parent;
    doc_.components[c.id] = c;
    doc_.activeComponent = c.id;
    doc_.touch();
    dirty_ = true;
    markSceneDirty();
    toast("Activated " + c.name);
}

void App::runGround() {
    std::vector<uint32_t> comps;
    for (uint32_t b : selectedBodies()) {
        auto it = doc_.bodies.find(b);
        if (it == doc_.bodies.end()) continue;
        uint32_t id = it->second.componentId;
        if (!id || id == doc_.rootId || !doc_.components.count(id)) continue;
        if (std::find(comps.begin(), comps.end(), id) == comps.end()) comps.push_back(id);
    }
    if (comps.empty()) {
        toast("Select a body that is inside a component");
        return;
    }
    pushUndo("Ground");
    std::string names;
    for (uint32_t id : comps) {
        doc_.components[id].grounded = true;
        if (!names.empty()) names += ", ";
        names += doc_.components[id].name;
    }
    doc_.touch();
    dirty_ = true;
    toast("Grounded " + names);
}

void App::newDocument() {
    if (docInUse() && !designTabs_.empty()) newDesignTab();   // a new design opens in its own tab
    doc_ = Document();
    undo_.clear();
    selection_.clear();
    isolated_.clear();
    doc_.featureFolder.clear();
    dirty_ = false;
    markSceneDirty();
}

void App::importDialog() {
    std::string p = platform::openFileDialog("Insert", "CAD and mesh files (*.step;*.stp;*.stl;*.obj)", "*.step;*.stp;*.STEP;*.STP;*.stl;*.STL;*.obj;*.OBJ");
    if (!p.empty()) importPath(p);
}

void App::importPath(const std::string& path) {
    std::string ext = toLower(pathToU8(u8path(path).extension()));
    if (ext == ".kst") {
        openDocumentPath(path);
        return;
    }
    std::string name = pathToU8(u8path(path).filename());
    auto cancel = std::make_shared<CancelToken>();
    jobs_->setStatus("Importing " + name);
    toast("Importing " + name + " in the background...");
    JobSystem* jobs = jobs_.get();
    jobs_->submit("import " + name,
        [this, jobs, path, cancel, name] {
            auto res = std::make_shared<kernel::ImportResult>(
                kernel::importFile(path, *cancel, [jobs, name](const std::string& s) { jobs->setStatus(name + ": " + s); }));
            jobs->post([this, res, path, jobs] {
                jobs->setStatus("");
                applyImport(std::move(*res), path);
            });
        },
        [this, jobs, name](const std::string& err) {
            jobs->setStatus("");
            toast("Could not import " + name + ":\n" + err, ToastKind::Error);
        });
}

// Shared by the import dialog and the recompute engine: creates parts, components and bodies
// for one import result under an existing timeline feature. Slot order matches creation order,
// so body overrides (name / color / visibility) survive recomputes.
void App::addImportToDoc(kernel::ImportResult& r, uint32_t featureId, const std::string& path) {
    std::string fname = pathToU8(u8path(path).filename());
    std::string ext = toLower(pathToU8(u8path(path).extension()));

    // An import lands in the component that was active when it was made (kept with the feature).
    uint32_t importComp = doc_.rootId;
    if (auto ff = doc_.featureFolder.find(featureId); ff != doc_.featureFolder.end() && doc_.components.count(ff->second))
        importComp = ff->second;
    uint32_t importParent = importComp;
    bool isStep = ext == ".step" || ext == ".stp";
    uint32_t compSlot = 0;
    auto nameFor = [&](const std::string& def) {
        auto it = doc_.compNames.find(Document::overrideKey(featureId, compSlot++));
        return it != doc_.compNames.end() ? it->second : def;
    };
    if (isStep) {
        Component c;
        c.id = doc_.newId();
        c.name = nameFor(r.rootName.empty() ? pathToU8(u8path(path).stem()) : r.rootName);
        c.parentId = importParent;
        c.featureId = featureId;
        doc_.components[c.id] = c;
        importComp = c.id;
    }
    std::vector<uint32_t> compIds(r.comps.size(), importComp);
    for (size_t i = 0; i < r.comps.size(); ++i) {
        if (i == 0 && r.firstCompIsRoot) {
            if (auto it = doc_.components.find(importComp); it != doc_.components.end() && importComp != doc_.rootId && isStep) {
                auto nm = doc_.compNames.find(Document::overrideKey(featureId, 0));
                it->second.name = nm != doc_.compNames.end() ? nm->second : r.comps[0].name;
            }
            compIds[0] = importComp;
            continue;
        }
        Component c;
        c.id = doc_.newId();
        c.name = nameFor(r.comps[i].name);
        c.parentId = r.comps[i].parent < 0 ? importComp : compIds[r.comps[i].parent];
        c.featureId = featureId;
        doc_.components[c.id] = c;
        compIds[i] = c.id;
    }
    {   // folders the user hid stay hidden through rebuilds
        uint32_t slot = 0;
        for (auto& [id, c] : doc_.components)
            if (c.featureId == featureId && doc_.compHidden.count(Document::overrideKey(featureId, slot++))) c.visible = false;
    }
    std::vector<uint32_t> partIds(r.parts.size());
    for (size_t i = 0; i < r.parts.size(); ++i) {
        Part p;
        p.id = doc_.newId();
        p.name = r.parts[i].name;
        p.shape = r.parts[i].shape;
        p.mesh = r.parts[i].mesh;
        p.isMesh = r.parts[i].isMesh;
        doc_.parts[p.id] = p;
        partIds[i] = p.id;
    }
    uint32_t slot = 0;
    static const bool dbgSlots = std::getenv("KESTRAL_DEBUG_SLOTS") != nullptr;
    for (auto& bo : r.bodies) {
        if (bo.part < 0 || bo.part >= (int)partIds.size()) { ++slot; continue; }
        Body b;
        b.id = doc_.newId();
        b.name = bo.name;
        b.partId = partIds[bo.part];
        b.componentId = bo.comp < 0 ? importComp : compIds[bo.comp];
        b.featureId = featureId;
        b.xform = bo.xform;
        if (bo.hasColor) b.color = bo.color;
        if (const BodyOverride* o = doc_.findOverride(featureId, slot)) {
            if (!o->name.empty()) b.name = o->name;
            if (o->hasColor) {
                b.color = o->color;
                b.mat = o->mat;
                b.faceLooks = o->faceLooks;
            }
            b.visible = o->visible;
        }
        if (dbgSlots)
            fprintf(stderr, "[slots] import %u slot %u body %u '%s' vis %d (override '%s' vis %d)\n",
                    featureId, slot, b.id, b.name.c_str(), (int)b.visible,
                    [&] { auto* o = doc_.findOverride(featureId, slot); return o ? o->name.c_str() : "-"; }(),
                    [&] { auto* o = doc_.findOverride(featureId, slot); return o ? (int)o->visible : -1; }());
        doc_.bodies[b.id] = b;
        ++slot;
    }
}

void App::applyImport(kernel::ImportResult&& r, const std::string& path) {
    std::string fname = pathToU8(u8path(path).filename());
    bool wasEmpty = doc_.bodies.empty();
    pushUndo("Import " + fname);

    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::Import;
    f.name = "Import " + fname;
    f.importPath = path;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;

    int count = (int)r.bodies.size();
    stampActiveComponent(f.id);
    addImportToDoc(r, f.id, path);
    refreshSignatures();   // geometry is already up to date; don't re-import on the next pass

    doc_.touch();
    markSceneDirty();
    settings_.addRecent(path);
    for (auto& w : r.warnings) toast(w, ToastKind::Warning);
    toast("Imported " + std::to_string(count) + (count == 1 ? " body" : " bodies") + " from " + fname, ToastKind::Success);
    if (wasEmpty) {
        rebuildScene();
        float a = vk_.extent.height ? (float)vk_.extent.width / (float)vk_.extent.height : 1.f;
        cam_.home(sceneBounds_, a);
    }
}

void App::startBreakCheck(const std::string& what) {
    breakCheck_ = BreakCheck{};
    breakCheck_.pending = true;
    breakCheck_.what = what;
    for (auto& f : doc_.timeline)
        if (f.failed) breakCheck_.failedBefore.insert(f.id);
}

// Once the recompute after a delete is done: if steps that worked before now fail, say which and
// offer to undo, instead of leaving bodies quietly gone (or consumed ones quietly back).
void App::drawBreakCheck() {
    if (breakCheck_.pending && !recomputeRunning_ && recomputeQueuedFrom_ < 0 && !recomputeResults_) {
        breakCheck_.pending = false;
        breakCheck_.broken.clear();
        for (auto& f : doc_.timeline)
            if (f.failed && !breakCheck_.failedBefore.count(f.id)) breakCheck_.broken.push_back(f.name);
        breakCheck_.show = !breakCheck_.broken.empty();
    }
    if (!breakCheck_.show) return;
    const float S = uiScale_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.35f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("That delete broke some steps", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string list;
        for (size_t i = 0; i < breakCheck_.broken.size() && i < 8; ++i) list += (i ? ", " : "") + breakCheck_.broken[i];
        if (breakCheck_.broken.size() > 8) list += " and " + std::to_string(breakCheck_.broken.size() - 8) + " more";
        ImGui::PushTextWrapPos(420 * S);
        ImGui::TextWrapped("Deleting %s made %zu later step%s fail: %s.", breakCheck_.what.c_str(), breakCheck_.broken.size(),
                           breakCheck_.broken.size() == 1 ? "" : "s", list.c_str());
        ImGui::TextDisabled("Their bodies are gone, and bodies they had used up (joined, cut) can show up again.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Undo the delete", ImVec2(150 * S, 28 * S))) { breakCheck_.show = false; undo(); }
        ImGui::SameLine();
        if (ImGui::Button("Keep it", ImVec2(110 * S, 28 * S))) breakCheck_.show = false;
    }
    ImGui::End();
}

void App::deleteSelection() {
    // Sketches and planes picked in the browser go too, in the same undo step as any bodies.
    bool pushed = false;
    if (!selSketches_.empty() || !selPlanes_.empty()) {
        std::vector<uint32_t> fids;
        std::string what;
        int n = 0;
        for (auto& f : doc_.timeline) {
            bool pick = (f.type == FeatureType::Sketch && selSketches_.count(f.sketchId)) || selPlanes_.count(f.id);
            if (!pick) continue;
            fids.push_back(f.id);
            if (n++ < 3) what += (what.empty() ? "" : ", ") + f.name;
        }
        if (n > 3) what += " and " + std::to_string(n - 3) + " more";
        bool bodiesToo = !selection_.empty();
        if (!fids.empty()) {
            pushUndo("Delete " + what);
            startBreakCheck(what);
            pushed = true;
            for (uint32_t fid : fids) deleteFeature(doc_.featureIndex(fid), false);
        }
        selSketches_.clear();
        selPlanes_.clear();
        if (!bodiesToo) return;
    }
    // Fusion: deleting a body adds a Remove step to the timeline, so it stays parametric.
    std::vector<TargetRef> refs;
    for (uint32_t id : selection_) {
        TargetRef r = refOfBody(id);
        if (r.first) refs.push_back(r);
    }
    if (refs.empty() && !selFaces_.empty()) {
        // Fusion: Delete on faces of a solid removes them and heals the body (Delete Face).
        auto m = std::make_shared<ModifyParams>();
        m->kind = 4;
        for (auto& [b, fc] : selFaces_) {
            TargetRef r = refOfBody(b);
            if (!r.first) continue;
            auto it = std::find(m->targets.begin(), m->targets.end(), r);
            uint32_t ti = (uint32_t)(it - m->targets.begin());
            if (it == m->targets.end()) m->targets.push_back(r);
            m->faces.push_back({ti, fc});
        }
        if (m->targets.empty()) return;
        if (m->targets.size() == 1) {
            auto bit = doc_.bodies.find(selFaces_.begin()->first);
            if (bit != doc_.bodies.end()) {
                auto pit = doc_.parts.find(bit->second.partId);
                if (pit != doc_.parts.end() && pit->second.shape) m->edgeTotal = kernel::edgeCount(pit->second.shape);
            }
        }
        pushUndo("Delete Face");
        Feature f;
        f.id = doc_.newId();
        f.type = FeatureType::DeleteFace;
        int n = 0;
        for (auto& ft : doc_.timeline)
            if (ft.type == FeatureType::DeleteFace) ++n;
        f.name = "DeleteFace" + std::to_string(n + 1);
        f.modify = m;
        int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
        doc_.timeline.insert(doc_.timeline.begin() + pos, f);
        doc_.markerPos = pos + 1;
        clearSelection();
        doc_.touch();
        markSceneDirty();
        dirty_ = true;
        scheduleRecompute(pos);
        return;
    }
    if (refs.empty()) {
        if (!selEdges_.empty()) toast("Edges cannot be deleted on their own. Select faces or a body.");
        return;
    }
    if (!pushed) {
        pushUndo("Delete");
        startBreakCheck(refs.size() == 1 ? "that body" : "those bodies");
    }
    Feature f;
    f.id = doc_.newId();
    f.type = FeatureType::Remove;
    int n = 0;
    for (auto& ft : doc_.timeline)
        if (ft.type == FeatureType::Remove) ++n;
    f.name = "Remove" + std::to_string(n + 1);
    auto rp = std::make_shared<RemoveParams>();
    rp->targets = refs;
    f.remove = rp;
    int pos = std::clamp(doc_.markerPos, 0, (int)doc_.timeline.size());
    doc_.timeline.insert(doc_.timeline.begin() + pos, f);
    doc_.markerPos = pos + 1;
    // Hide right away; the recompute makes it official a moment later.
    for (uint32_t id : selection_) {
        auto it = doc_.bodies.find(id);
        if (it != doc_.bodies.end()) it->second.consumedBy = f.id;
    }
    clearSelection();
    doc_.touch();
    markSceneDirty();
    dirty_ = true;
    scheduleRecompute(pos);
}

void App::toggleVisibility(const std::vector<uint32_t>& bodies) {
    if (bodies.empty()) return;
    bool anyVisible = false;
    for (uint32_t id : bodies) {
        auto it = doc_.bodies.find(id);
        if (it != doc_.bodies.end() && it->second.visible) anyVisible = true;
    }
    pushUndo(anyVisible ? "Hide" : "Show");
    for (uint32_t id : bodies) {
        auto it = doc_.bodies.find(id);
        if (it != doc_.bodies.end()) {
            it->second.visible = !anyVisible;
            syncBodyOverride(id);
        }
    }
    doc_.touch();
    dirty_ = true;
}

void App::setComponentVisible(uint32_t comp, bool vis) {
    auto it = doc_.components.find(comp);
    if (it == doc_.components.end()) return;
    if (it->second.visible != vis) pushUndo(vis ? "Show" : "Hide");
    it->second.visible = vis;
    if (it->second.featureId) {
        uint32_t slot = 0;
        for (auto& [id, c] : doc_.components) {
            if (c.featureId != it->second.featureId) continue;
            if (id == comp) break;
            ++slot;
        }
        auto key = Document::overrideKey(it->second.featureId, slot);
        if (vis) doc_.compHidden.erase(key); else doc_.compHidden.insert(key);
    }
    doc_.touch();
    dirty_ = true;
}

void App::isolate(const std::vector<uint32_t>& bodies) {
    if (bodies.empty()) { toast("Select something to isolate"); return; }
    isolated_ = std::set<uint32_t>(bodies.begin(), bodies.end());
    markSceneDirty();
}

void App::exitIsolate() {
    isolated_.clear();
    markSceneDirty();
}

void App::showAll() {
    for (auto& [id, b] : doc_.bodies) b.visible = true;
    for (auto& [id, c] : doc_.components) c.visible = true;
    isolated_.clear();
    doc_.touch();
}

void App::selectBodies(const std::vector<uint32_t>& ids, bool additive) {
    if (!additive) {
        selection_.clear();
        selOrder_.clear();
        selFaces_.clear();
        selEdges_.clear();
        selProfiles_.clear();
    }
    for (uint32_t id : ids) {
        if (!selection_.count(id)) selOrder_.push_back(id);
        selection_.insert(id);
    }
    markSceneDirty();
    selectionChanged();
}

std::string App::componentPath(uint32_t componentId) const {
    std::vector<std::string> chain;
    uint32_t c = componentId;
    for (int guard = 0; c && c != doc_.rootId && guard < 64; ++guard) {
        auto cit = doc_.components.find(c);
        if (cit == doc_.components.end()) break;
        chain.push_back(cit->second.name);
        c = cit->second.parentId;
    }
    std::string path;
    for (int i = (int)chain.size() - 1; i >= 0; --i) {
        if (!path.empty()) path += "/";
        path += chain[i];
    }
    return path;
}

std::vector<kernel::ExportItem> App::exportItems() const {
    std::vector<kernel::ExportItem> items;
    auto addBody = [&](const Body& b) {
        auto pit = doc_.parts.find(b.partId);
        if (pit == doc_.parts.end()) return;
        items.push_back({pit->second.shape, pit->second.mesh, b.xform, b.name, b.color, componentPath(b.componentId)});
    };
    if (!selection_.empty()) {
        for (uint32_t id : selection_) {
            auto it = doc_.bodies.find(id);
            if (it != doc_.bodies.end() && bodyShown(it->second)) addBody(it->second);
        }
    } else {
        for (auto& [id, b] : doc_.bodies)
            if (bodyShown(b)) addBody(b);
    }
    return items;
}

void App::sendToSlicer() {
    std::error_code ec;
    if (settings_.slicerPath.empty() || !std::filesystem::exists(u8path(settings_.slicerPath), ec)) {
        auto found = platform::detectSlicers();
        if (found.size() >= 1) {
            // Prefer OrcaSlicer when installed.
            auto pick = found[0];
            for (auto& s : found) if (s.name == "OrcaSlicer") pick = s;
            settings_.slicerName = pick.name;
            settings_.slicerPath = pick.path;
            settings_.save(settingsPath_);
            toast("Using " + pick.name + " (change it in Preferences)");
        } else {
            settingsOpen_ = true;
            pendingSendAfterSetup_ = true;
            toast("Choose your slicer first", ToastKind::Warning);
            return;
        }
    }
    auto items = exportItems();
    if (items.empty()) { toast("Nothing visible to send", ToastKind::Warning); return; }
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
    std::string safe = doc_.name;
    for (auto& ch : safe) if (ch == ' ' || ch == '/' || ch == '\\' || ch == ':') ch = '_';
    std::string out = platform::tempDir() + "/" + safe + "_" + stamp + ".stl";
    std::string exe = settings_.slicerPath, sname = settings_.slicerName.empty() ? "slicer" : settings_.slicerName;
    size_t n = items.size();
    auto cancel = std::make_shared<CancelToken>();
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Preparing " + std::to_string(n) + " bodies for " + sname);
    jobs_->submit("send to slicer",
        [this, jobs, items, out, exe, sname, cancel, n] {
            kernel::exportStl(items, out, *cancel);
            std::string err;
            bool ok = platform::launch(exe, out, err);
            jobs->post([this, jobs, ok, err, sname, n] {
                jobs->setStatus("");
                if (ok) toast("Sent " + std::to_string(n) + (n == 1 ? " body to " : " bodies to ") + sname, ToastKind::Success);
                else toast(err, ToastKind::Error);
            });
        },
        [this, jobs](const std::string& err) {
            jobs->setStatus("");
            toast("Send to slicer failed: " + err, ToastKind::Error);
        });
}

void App::exportStlDialog() {
    auto items = exportItems();
    if (items.empty()) { toast("Nothing visible to export", ToastKind::Warning); return; }
    std::string p = platform::saveFileDialog("Export STL", "STL files (*.stl)", "*.stl", "stl", doc_.name + ".stl");
    if (p.empty()) return;
    auto cancel = std::make_shared<CancelToken>();
    JobSystem* jobs = jobs_.get();
    jobs_->setStatus("Exporting STL");
    jobs_->submit("export stl",
        [this, jobs, items, p, cancel] {
            kernel::exportStl(items, p, *cancel);
            jobs->post([this, jobs, p] { jobs->setStatus(""); toast("Saved " + p, ToastKind::Success); });
        },
        [this, jobs](const std::string& err) { jobs->setStatus(""); toast("Export failed: " + err, ToastKind::Error); });
}

void App::setMarker(int pos) {
    pos = std::clamp(pos, 0, (int)doc_.timeline.size());
    if (pos == doc_.markerPos) return;
    int old = doc_.markerPos;
    doc_.markerPos = pos;
    doc_.touch();
    if (pos > old) scheduleRecompute(old);   // newly activated features need their geometry
}

void App::toggleSuppress(int i) {
    if (i < 0 || i >= (int)doc_.timeline.size()) return;
    pushUndo(doc_.timeline[i].suppressed ? "Unsuppress" : "Suppress");
    doc_.timeline[i].suppressed = !doc_.timeline[i].suppressed;
    doc_.touch();
    if (!doc_.timeline[i].suppressed && i < doc_.markerPos) scheduleRecompute(i);
}

void App::deleteFeature(int i, bool undoStep) {
    if (i < 0 || i >= (int)doc_.timeline.size()) return;
    if (undoStep) {
        pushUndo("Delete " + doc_.timeline[i].name);
        startBreakCheck(doc_.timeline[i].name);
    }
    uint32_t fid = doc_.timeline[i].id;
    if (doc_.timeline[i].type == FeatureType::Sketch) {
        if (sk_.active && sk_.sketchId == doc_.timeline[i].sketchId) leaveSketchMode();
        doc_.sketches.erase(doc_.timeline[i].sketchId);
    }
    std::set<uint32_t> ids;
    for (auto& [id, b] : doc_.bodies) if (b.featureId == fid) ids.insert(id);
    doc_.removeBodies(ids);
    // Remove now empty components created by the import.
    bool removed = true;
    while (removed) {
        removed = false;
        for (auto& [cid, c] : doc_.components) {
            if (cid == doc_.rootId) continue;
            if (doc_.bodiesIn(cid).empty() && doc_.childComponents(cid).empty()) {
                doc_.components.erase(cid);
                removed = true;
                break;
            }
        }
    }
    doc_.timeline.erase(doc_.timeline.begin() + i);
    if (doc_.markerPos > i) doc_.markerPos--;
    for (auto id : ids) selection_.erase(id);
    doc_.touch();
    scheduleRecompute(0);   // downstream features may reference what this feature made
}

void App::fitView(bool selectionOnly) {
    Aabb box;
    if (selectionOnly && !selection_.empty()) {
        for (uint32_t id : selection_) {
            auto it = doc_.bodies.find(id);
            if (it == doc_.bodies.end()) continue;
            auto pit = doc_.parts.find(it->second.partId);
            if (pit != doc_.parts.end() && pit->second.mesh) box.add(pit->second.mesh->bounds.transformed(it->second.xform));
        }
    } else {
        box = sceneBounds_;
    }
    if (!box.valid()) box.add(glm::vec3(-50, -50, 0)), box.add(glm::vec3(50, 50, 50));
    float a = vk_.extent.height ? (float)vk_.extent.width / vk_.extent.height : 1.f;
    cam_.fit(box, a);
}

} // namespace kst

namespace kst {
void App::sketchAfterHistory() {
    sk_.selEnt.clear(); sk_.selCon.clear(); sk_.clicks.clear(); sk_.picks.clear();
    sk_.hoverEnt = 0; sk_.hoverCon = 0;
    sk_.dimPlacing = false; sk_.dimEdit = 0; sk_.step = 0; sk_.chainStart = 0;
    sk_.dragging = false; sk_.pressed = false; sk_.boxSel = false;
    sk_.profSketch = 0;
    if (sk_.active) {
        auto it = doc_.sketches.find(sk_.sketchId);
        if (it != doc_.sketches.end()) sk::computeDof(it->second);
    }
}
} // namespace kst
