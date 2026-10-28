#pragma once
#include <map>
#include <string>
#include <vector>

namespace kst {

struct Settings {
    // Slicer
    std::string slicerName;     // e.g. "OrcaSlicer"
    std::string slicerPath;     // full path to the exe
    // Navigation
    bool reverseZoom = false;   // false: wheel forward zooms in
    bool constrainedOrbit = false;
    int navStyle = 0;           // 0 Fusion, 1 SolidWorks, 2 Onshape, 3 Blender, 4 Tinkercad, 5 Maya / Alias
    bool perspective = true;
    // Display
    bool showEdges = true;
    bool showGrid = false;      // ground grid outside sketches (sketches always have their own grid)
    bool showOrigin = true;
    bool showFps = true;
    float uiScale = 0.f;        // 0 = follow Windows DPI
    // Selection
    bool bodyPriority = false;  // false (Fusion default): clicks pick faces, edges and profiles
    float autosaveSec = 30.f;   // seconds between autosaves, 0 = off
    bool autosaveToFile = true; // a saved design also autosaves into its own .kst (not just recovery)
    std::vector<std::string> recentFiles;
    std::string lastDesignDir;  // folder of the last saved / opened design
    // Support the project (Ko-fi). The note can be snoozed or turned off for good.
    bool donateNever = false;
    long long donateLastShown = 0;   // unix seconds of the last time the note popped up
    bool orcaPrestart = true;   // start OrcaSlicer hidden with Kestral so the Slice tab is instant
    // 3D printer the assistant sends prints to (see print/Printer.h for the kinds).
    std::string printerKind = "none";   // none, bambu_orca, bambu_connect, bambu_lan, octoprint, moonraker, prusalink
    std::string printerName, printerHost, printerSerial;
    std::string printerSecret;          // access code or API key, protected like AI keys
    bool printerUseAms = false;
    bool aiAutoPrint = false;
    bool aiNotify = true;        // a notification when the assistant finishes a reply
    bool aiNotifySound = true;   // and a sound
    int aiNotifySoundKind = 0;   // 0 chime, 1 pop, 2 the Windows sound
    bool aiNotifyAway = true;    // only while Kestral is not the window in front   // the assistant may start prints without the Print / Cancel card
    int printerAmsSlot = 0;
    // Look
    bool darkMode = true;
    float darkness = 0.85f;     // how dark the dark theme is (0.2 .. 1)
    // AI assistant: the chosen provider and each provider's own model / address / key.
    // Built in ids: claude, openai, gemini, grok, deepseek, mistral, openrouter, groq, ollama, lmstudio.
    // User added ones are custom1, custom2... with their own name and API format.
    struct AiProvider {
        std::string model, url, key;   // key is protected (DPAPI), empty = none
        std::string name;              // custom providers only
        int format = 0;                // custom providers only: 0 OpenAI compatible, 1 Anthropic, 2 Ollama
        int thinking = 0;              // 0 model default, 1 off, 2 low, 3 medium, 4 high
        int ctx = 0;                   // context window in tokens, 0 = automatic
    };
    // Assistant behaviour (Preferences > Assistant)
    int aiToolsMode = 0;          // 0 automatic (smart for local models, all for cloud ones), 1 smart, 2 all
    int aiKeepAliveMin = 30;      // Ollama: minutes the model stays loaded after a reply (-1 always)
    int aiCompact = 0;            // long chats: 0 summarize the older part, 1 drop the oldest, 2 off
    int aiCompactAt = 75;         // percent of the window that starts compaction
    float aiTemperature = -1.f;   // < 0: the model's default
    int aiMaxSteps = 0;           // tool steps per request (0: no limit)
    bool aiGpuCheck = true;       // look for other programs on the graphics card first
    bool aiShowThinking = true;
    bool aiPageContext = true;    // tell the model which page and selection the user has
    std::string aiCustomPrompt;   // extra instructions for every chat
    int aiMainVision = 0;         // the chat model sees pictures: 0 find out, 1 yes, 2 no (a helper describes them)
    std::string aiVisionModel;    // helper that describes pictures for a model that cannot see (same provider)
    std::string aiProvider = "ollama";
    std::map<std::string, AiProvider> ai;
    // Keyboard shortcut overrides: command id -> combo ("" = unassigned). Defaults are Fusion's.
    std::map<std::string, std::string> keys;

    void load(const std::string& path);
    void save(const std::string& path) const;
    void addRecent(const std::string& file);
};

} // namespace kst
