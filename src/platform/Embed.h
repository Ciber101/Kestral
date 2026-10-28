#pragma once
// Hosts another program's main window inside Kestral (the OrcaSlicer tab). On Windows the window
// sits inside Kestral; on Linux the program runs as its own window and files are handed to it.
#include <atomic>
#include <string>

namespace kst::platform {

class EmbeddedApp {
public:
    EmbeddedApp();
    ~EmbeddedApp();
    EmbeddedApp(const EmbeddedApp&) = delete;
    EmbeddedApp& operator=(const EmbeddedApp&) = delete;

    static bool supported();
    static bool hostsWindow();   // true when the window is shown inside Kestral (Windows)
    // Starts exe with a file. If a window is already hosted it stays until the new one shows up
    // (then the old one is handed back to the desktop and asked to close).
    // background: start minimized and out of the way (it stays hidden until shown with place()).
    bool start(const std::string& exe, const std::string& file, std::string& error, bool background = false);
    // Hands a file to the hosted program (OrcaSlicer opens it in its running window).
    bool openFile(const std::string& file);
    // Closes the hosted program (it may still ask to save its own project).
    void closeApp();
    // Call every frame: finds the program's main window once it appears and moves it in.
    void poll(void* parentWindow);
    // Where to show it, in the parent's client pixels; hidden when visible is false.
    void place(int x, int y, int w, int h, bool visible);
    // Call every frame: keyboard focus follows the mouse between Kestral and the hosted window, so
    // its shortcuts and wheel zoom work (a hosted window is never "active" on its own).
    void followMouse();
    bool embedded() const;     // a window is hosted right now
    bool starting() const;     // launched, window not found yet
    bool running() const;      // hosted program still alive
    // Gives the window back to the desktop as a normal window (on exit or on request).
    void release();
    // Seconds since the last start (for "still starting" messages).
    double secondsSinceStart() const;
    // The hosted program's process and main window (0 / null when nothing is hosted).
    unsigned long processId() const;
    void* window() const;

private:
    struct Impl;
    Impl* d_;
};

// Clicks a button in another program by its label (OrcaSlicer's "Print plate", "Send"): looks
// through the program's windows (and those under `root`) until the button is there and enabled,
// for up to timeoutMs. `seen` gets the labels it came across (for the log when it fails).
// Windows only; false elsewhere.
bool clickAppButton(unsigned long pid, void* root, const std::string& label, int timeoutMs,
                    const std::atomic<bool>* cancel, std::string& seen);
// Types a shortcut (e.g. Ctrl+Shift+G) into a window of another program. Windows only.
bool sendAppShortcut(void* window, bool ctrl, bool shift, bool alt, int key);

// Full path of an installed OrcaSlicer, empty if none. On Linux may be "flatpak:<app id>".
std::string findOrcaSlicer();
// Folder picker (UTF-8, empty on cancel).
std::string pickFolderDialog(const char* title, const std::string& initialDir);

} // namespace kst::platform
