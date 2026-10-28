#pragma once
// Sending a sliced print to a printer, and asking how it is doing.
//   Bambu Lab, normal (cloud) mode : through OrcaSlicer (you press Print there) or Bambu Connect
//   Bambu Lab, LAN only + Developer mode : straight to the printer (MQTT + FTPS), fully automatic
//   OctoPrint, Klipper (Moonraker), PrusaLink : over HTTP with an API key
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kst::print {

enum class PrinterKind { None = 0, BambuOrca, BambuConnect, BambuLan, OctoPrint, Moonraker, PrusaLink };

struct PrinterConn {
    PrinterKind kind = PrinterKind::None;
    std::string name;        // shown name, e.g. "A1 mini"
    std::string host;        // IP or host name (LAN kinds)
    std::string serial;      // Bambu: printer serial number
    std::string secret;      // Bambu: access code; OctoPrint/PrusaLink: API key / password
    bool useAms = false;     // Bambu: feed from the AMS
    int amsSlot = 0;         // Bambu: AMS slot (0 based)
    std::vector<int> amsMapping;   // Bambu: AMS tray per filament of the print (ams*4+slot, 254 external, -1 none)
    bool bedLeveling = true, flowCali = false, timelapse = false;   // Bambu: print options (Send print dialog)
};

const char* kindName(PrinterKind k);
PrinterKind kindFromString(const std::string& s);
std::string kindId(PrinterKind k);

struct PrintStatus {
    bool ok = false;
    std::string error;
    std::string state;       // e.g. "printing", "idle", "finished", "paused", "error"
    int percent = -1;
    int minutesLeft = -1;
    double nozzleC = -1, bedC = -1;
    std::string job;
    // Bambu Lab: what Orca's Device tab shows.
    double nozzleTarget = -1, bedTarget = -1, chamberC = -1;
    int partFan = -1, auxFan = -1, chamberFan = -1;   // percent
    bool lightKnown = false, light = false;
    int layer = -1, totalLayers = -1;
    int speedLevel = -1;                              // 1 silent, 2 standard, 3 sport, 4 ludicrous
    std::string wifi, nozzleType, stage;
    double nozzleDia = 0;
    bool sdcard = false;
    std::vector<std::string> hms;                     // HMS messages (codes)
    struct AmsTray {
        int ams = 0, slot = 0;                        // tray id = ams * 4 + slot (254: external spool)
        bool loaded = false;                          // a spool is in it
        std::string type, color, name;               // "PLA", "#RRGGBB", "PLA Basic"
        int remain = -1;                              // percent, -1 unknown
        int humidity = -1;
    };
    std::vector<AmsTray> trays;                       // every AMS slot
    int amsUnits = 0;
    AmsTray ext;                                      // the spool holder outside the AMS
    bool hasExt = false;
    int trayNow = -1;                                 // loaded now (255 none, 254 external)
};



struct SendResult {
    bool ok = false;
    bool needsClick = false; // the print waits for a click in OrcaSlicer / Bambu Connect
    std::string message;
};

// Bambu printers: any MQTT command (payload JSON), and G-code lines. For the Device tab.
SendResult printerMqtt(const PrinterConn& c, const std::string& payload);
SendResult printerGcode(const PrinterConn& c, const std::string& gcode);

// The camera of A1, A1 mini and P1 printers (JPEG stream on port 6000, over TLS). Runs on its own thread.
class PrinterCamera {
public:
    ~PrinterCamera();
    void start(const PrinterConn& c);
    void stop();
    bool running() const { return run_; }
    // The newest picture (JPEG) when it is newer than `seq`.
    bool latest(std::string& jpeg, uint64_t& seq);
    std::string status();

private:
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex m_;
    std::string jpeg_, status_;
    uint64_t seq_ = 0;
};

// Sends gcode3mf (a sliced .gcode.3mf) and starts it where the connection allows.
// openInOrca: called for BambuOrca (Kestral opens the file in its Slice tab).
SendResult sendPrint(const PrinterConn& c, const std::string& gcode3mf, const std::string& jobName,
                     const std::atomic<bool>* cancel);
PrintStatus printerStatus(const PrinterConn& c, int timeoutMs = 8000);
// The job on the printer: action "pause", "resume" or "stop" (cancels the print).
SendResult printerControl(const PrinterConn& c, const std::string& action, const std::atomic<bool>* cancel = nullptr);
// Quick reachability check with a human readable answer.
std::string testConnection(const PrinterConn& c);

} // namespace kst::print
