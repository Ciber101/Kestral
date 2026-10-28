#include "core/Settings.h"
#include "core/Types.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace kst {

namespace {
bool toBool(const std::string& v) { return v == "1" || v == "true"; }
}

void Settings::load(const std::string& path) {
    std::ifstream f(u8path(path));
    if (!f) return;
    std::string line;
    std::string oldKey;
    int oldProv = -1;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "slicerName") slicerName = v;
            else if (k == "slicerPath") slicerPath = v;
            else if (k == "reverseZoom") reverseZoom = toBool(v);
            else if (k == "constrainedOrbit") constrainedOrbit = toBool(v);
            else if (k == "navStyle") navStyle = std::stoi(v);
            else if (k == "perspective") perspective = toBool(v);
            else if (k == "showEdges") showEdges = toBool(v);
            else if (k == "groundGrid") showGrid = toBool(v);
            else if (k == "showOrigin") showOrigin = toBool(v);
            else if (k == "showFps") showFps = toBool(v);
            else if (k == "bodyPriority2") bodyPriority = toBool(v);
            else if (k == "uiScale") uiScale = std::stof(v);
            else if (k == "autosaveSec") autosaveSec = std::stof(v);
            else if (k == "autosaveToFile") autosaveToFile = toBool(v);
            else if (k == "recent") recentFiles.push_back(v);
            else if (k == "lastDesignDir") lastDesignDir = v;
            else if (k == "darkMode2") darkMode = toBool(v);
            else if (k == "orcaPrestart") orcaPrestart = toBool(v);
            else if (k == "printerKind") printerKind = v;
            else if (k == "printerName") printerName = v;
            else if (k == "printerHost") printerHost = v;
            else if (k == "printerSerial") printerSerial = v;
            else if (k == "printerSecret") printerSecret = v;
            else if (k == "printerUseAms") printerUseAms = toBool(v);
            else if (k == "aiAutoPrint") aiAutoPrint = toBool(v);
            else if (k == "aiNotify") aiNotify = toBool(v);
            else if (k == "aiNotifySound") aiNotifySound = toBool(v);
            else if (k == "aiNotifySoundKind") aiNotifySoundKind = std::atoi(v.c_str());
            else if (k == "aiNotifyAway") aiNotifyAway = toBool(v);
            else if (k == "printerAmsSlot") printerAmsSlot = std::atoi(v.c_str());
            else if (k == "donateNever") donateNever = toBool(v);
            else if (k == "donateLastShown") donateLastShown = std::stoll(v);
            else if (k == "darkness") darkness = std::stof(v);
            else if (k == "aiProviderId") aiProvider = v;
            else if (k == "aiToolsMode") aiToolsMode = std::atoi(v.c_str());
            else if (k == "aiKeepAliveMin") aiKeepAliveMin = std::atoi(v.c_str());
            else if (k == "aiCompact") aiCompact = std::atoi(v.c_str());
            else if (k == "aiCompactAt") aiCompactAt = std::atoi(v.c_str());
            else if (k == "aiTemperature") aiTemperature = (float)std::atof(v.c_str());
            else if (k == "aiMaxSteps") aiMaxSteps = std::atoi(v.c_str());
            else if (k == "aiGpuCheck") aiGpuCheck = toBool(v);
            else if (k == "aiShowThinking") aiShowThinking = toBool(v);
            else if (k == "aiPageContext") aiPageContext = toBool(v);
            else if (k == "aiMainVision") aiMainVision = std::atoi(v.c_str());
            else if (k == "aiVisionModel") aiVisionModel = v;
            else if (k == "aiCustomPrompt") {
                aiCustomPrompt.clear();
                for (size_t i = 0; i < v.size(); ++i) {
                    if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == 'n') { aiCustomPrompt += '\n'; ++i; }
                    else if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') { aiCustomPrompt += '\\'; ++i; }
                    else aiCustomPrompt += v[i];
                }
            }
            // Older settings files (one Claude / Ollama / OpenAI compatible slot each).
            else if (k == "aiProvider") { int n = std::stoi(v); aiProvider = n == 0 ? "claude" : n == 2 ? "lmstudio" : "ollama"; oldProv = n; }
            else if (k == "aiModelClaude") ai["claude"].model = v;
            else if (k == "aiModelOllama") ai["ollama"].model = v;
            else if (k == "aiModelOpenAI") ai["lmstudio"].model = v;
            else if (k == "aiUrlOllama") ai["ollama"].url = v;
            else if (k == "aiUrlOpenAI") ai["lmstudio"].url = v;
            else if (k == "aiKey") oldKey = v;
            else if (k.rfind("ai.", 0) == 0) {
                auto dot = k.find('.', 3);
                if (dot == std::string::npos) continue;
                std::string id = k.substr(3, dot - 3), field = k.substr(dot + 1);
                AiProvider& p = ai[id];
                if (field == "model") p.model = v;
                else if (field == "url") p.url = v;
                else if (field == "key") p.key = v;
                else if (field == "name") p.name = v;
                else if (field == "format") p.format = std::stoi(v);
                else if (field == "thinking") p.thinking = std::stoi(v);
                else if (field == "ctx") p.ctx = std::stoi(v);
            }
            else if (k.rfind("key.", 0) == 0) keys[k.substr(4)] = v;
        } catch (...) {
            // Ignore malformed lines, never fail on a bad settings file.
        }
    }
    if (!oldKey.empty()) {
        std::string id = oldProv == 2 ? "lmstudio" : "claude";
        if (ai[id].key.empty()) ai[id].key = oldKey;
    }
    darkness = std::clamp(darkness, 0.2f, 1.f);
}

void Settings::save(const std::string& path) const {
    std::error_code ec;
    std::filesystem::create_directories(u8path(path).parent_path(), ec);
    std::ofstream f(u8path(path), std::ios::trunc);
    if (!f) return;
    f << "slicerName=" << slicerName << "\n";
    f << "slicerPath=" << slicerPath << "\n";
    f << "reverseZoom=" << reverseZoom << "\n";
    f << "constrainedOrbit=" << constrainedOrbit << "\n";
    f << "navStyle=" << navStyle << "\n";
    f << "perspective=" << perspective << "\n";
    f << "showEdges=" << showEdges << "\n";
    f << "groundGrid=" << showGrid << "\n";
    f << "showOrigin=" << showOrigin << "\n";
    f << "showFps=" << showFps << "\n";
    f << "bodyPriority2=" << bodyPriority << "\n";
    f << "uiScale=" << uiScale << "\n";
    f << "autosaveSec=" << autosaveSec << "\n";
    f << "autosaveToFile=" << autosaveToFile << "\n";
    for (auto& r : recentFiles) f << "recent=" << r << "\n";
    f << "lastDesignDir=" << lastDesignDir << "\n";
    f << "darkMode2=" << darkMode << "\n";
    f << "orcaPrestart=" << orcaPrestart << "\n";
    f << "printerKind=" << printerKind << "\n";
    f << "printerName=" << printerName << "\n";
    f << "printerHost=" << printerHost << "\n";
    f << "printerSerial=" << printerSerial << "\n";
    f << "printerSecret=" << printerSecret << "\n";
    f << "printerUseAms=" << printerUseAms << "\n";
    f << "aiAutoPrint=" << aiAutoPrint << "\n";
    f << "aiNotify=" << aiNotify << "\n";
    f << "aiNotifySound=" << aiNotifySound << "\n";
    f << "aiNotifySoundKind=" << aiNotifySoundKind << "\n";
    f << "aiNotifyAway=" << aiNotifyAway << "\n";
    f << "printerAmsSlot=" << printerAmsSlot << "\n";
    f << "donateNever=" << donateNever << "\n";
    f << "donateLastShown=" << donateLastShown << "\n";
    f << "darkness=" << darkness << "\n";
    f << "aiProviderId=" << aiProvider << "\n";
    f << "aiToolsMode=" << aiToolsMode << "\n";
    f << "aiKeepAliveMin=" << aiKeepAliveMin << "\n";
    f << "aiCompact=" << aiCompact << "\n";
    f << "aiCompactAt=" << aiCompactAt << "\n";
    f << "aiTemperature=" << aiTemperature << "\n";
    f << "aiMaxSteps=" << aiMaxSteps << "\n";
    f << "aiGpuCheck=" << aiGpuCheck << "\n";
    f << "aiShowThinking=" << aiShowThinking << "\n";
    f << "aiPageContext=" << aiPageContext << "\n";
    f << "aiMainVision=" << aiMainVision << "\n";
    f << "aiVisionModel=" << aiVisionModel << "\n";
    {
        std::string esc;
        for (char c : aiCustomPrompt) {
            if (c == '\n') esc += "\\n";
            else if (c == '\\') esc += "\\\\";
            else if (c != '\r') esc += c;
        }
        f << "aiCustomPrompt=" << esc << "\n";
    }
    for (auto& [id, p] : ai) {
        if (!p.model.empty()) f << "ai." << id << ".model=" << p.model << "\n";
        if (!p.url.empty()) f << "ai." << id << ".url=" << p.url << "\n";
        if (!p.key.empty()) f << "ai." << id << ".key=" << p.key << "\n";
        if (!p.name.empty()) f << "ai." << id << ".name=" << p.name << "\n";
        if (id.rfind("custom", 0) == 0) f << "ai." << id << ".format=" << p.format << "\n";
        if (p.thinking) f << "ai." << id << ".thinking=" << p.thinking << "\n";
        if (p.ctx) f << "ai." << id << ".ctx=" << p.ctx << "\n";
    }
    for (auto& [id, combo] : keys) f << "key." << id << "=" << combo << "\n";
    f.close();
    // API keys live here: only this user may read the file.
    std::filesystem::permissions(u8path(path), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
}

void Settings::addRecent(const std::string& file) {
    std::erase(recentFiles, file);
    recentFiles.insert(recentFiles.begin(), file);
    if (recentFiles.size() > 10) recentFiles.resize(10);
}

} // namespace kst
