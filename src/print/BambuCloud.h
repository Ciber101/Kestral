// Bambu Lab printers in normal (cloud) mode, the way OrcaSlicer does it: through Bambu's own network
// plugin (bambu_networking, a closed library from Bambu). Kestral uses the plugin and the Bambu login
// that OrcaSlicer already has on this computer.
//
// The plugin never runs inside Kestral itself: Kestral starts itself as a hidden helper
// ("--bambu-agent"), which loads the plugin, does one job and ends. If the plugin crashes, only the
// helper goes.
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace kst::print {

struct BambuDevice {
    std::string id;        // serial
    std::string name;
    std::string model;     // e.g. "N1" (A1 mini)
    bool online = false;
    std::string state;     // print status the cloud reports, when it does
};

struct BambuCloudStatus {
    bool pluginFound = false;   // Bambu's plugin is installed (OrcaSlicer downloads it)
    bool connected = false;     // reached Bambu's servers
    bool loggedIn = false;
    std::string pluginVersion, user, error;
    std::string detail;         // what the plugin reported (for troubleshooting)
    std::vector<BambuDevice> devices;   // printers bound to the account
};

struct BambuPrintJob {
    std::string devId;
    std::string gcode3mf;
    std::string name;
    bool useAms = false;
    int amsSlot = 0;            // 0 based
    std::vector<int> amsMapping;   // per filament in the print: AMS tray (ams*4+slot), 254 external spool, -1 none
    bool bedLeveling = true, flowCali = false, timelapse = false;
    // For sending over the home network (no Bambu login needed): the printer's IP and access code.
    std::string ip, accessCode;
};

struct BambuPrintResult {
    bool ok = false;
    int code = 0;
    std::string message;
};

// Where the plugin is, empty when OrcaSlicer never downloaded it.
std::string bambuPluginPath(std::string* version = nullptr);
// Login and printers (starts the helper; a few seconds).
BambuCloudStatus bambuCloudStatus(const std::atomic<bool>* cancel);
// Sends a sliced print to a printer through Bambu's cloud (starts the helper; up to a few minutes).
BambuPrintResult bambuCloudPrint(const BambuPrintJob& job, const std::atomic<bool>* cancel);

// Pause, resume or stop the printer's current job ("pause", "resume", "stop"), the way OrcaSlicer's
// Device tab does (through the plugin, so newer firmware accepts it).
BambuPrintResult bambuCloudControl(const BambuPrintJob& printer, const std::string& action, const std::atomic<bool>* cancel);

// The helper side: Kestral --bambu-agent <status|print> <request.json> <result.json>
int runBambuAgent(int argc, char** argv);

} // namespace kst::print
