#pragma once
// Slicing through OrcaSlicer: finds the profiles you already made in OrcaSlicer (printer,
// filament, process), applies changes asked for ("walls 3, infill 25%"), and slices in the
// background with OrcaSlicer's command line. The result is a print ready .gcode.3mf.
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace kst::print {

struct Preset {
    std::string name;
    std::string type;       // "machine", "filament", "process"
    std::string path;       // its .json
    bool user = false;      // made by you (false: built into OrcaSlicer)
    std::string inherits;
    std::string printerModel;              // machine: e.g. "Bambu Lab A1 mini"
    std::vector<std::string> compatible;   // filament/process: printers it is made for
};

struct OrcaSetup {
    std::string exe;            // orca-slicer program (empty: not installed)
    std::string configDir;      // where OrcaSlicer keeps its settings
    std::vector<Preset> presets;
    // What OrcaSlicer has selected right now (your usual setup).
    std::string machine, process, filament;
    std::string bedType;        // e.g. "Textured PEI Plate"
    // Printers OrcaSlicer already knows on your network (Bambu Lab): serial -> name / ip / model.
    struct KnownPrinter { std::string serial, name, ip, model; };
    std::vector<KnownPrinter> knownPrinters;
    std::string problem;        // why slicing is not ready (empty: ready)
};

// Looks everything up (fast: reads small files). orcaExe may be empty (then it searches).
OrcaSetup findOrca(const std::string& orcaExe);

const Preset* findPreset(const OrcaSetup& s, const std::string& type, const std::string& name);
// Best matching presets for a free text query (e.g. "a1 mini", "petg"), user ones first.
std::vector<const Preset*> searchPresets(const OrcaSetup& s, const std::string& type, const std::string& query, size_t max = 25);

// The full settings of a preset (its parents merged in), as JSON text. systemName (optional) gets the
// first built in profile in its chain (the preset itself when it is built in).
std::string resolvePreset(const OrcaSetup& s, const Preset& p, std::string& err, std::string* systemName = nullptr);

// The built in profile a preset is based on (itself when built in; nullptr when its chain has none).
const Preset* systemBase(const OrcaSetup& s, const Preset& p);
// What the Slice tab offers: printers (type "machine"), or the filament / process profiles that fit a
// printer, yours first, then built in ones by name. Base profiles OrcaSlicer never shows are left out.
std::vector<const Preset*> presetChoices(const OrcaSetup& s, const std::string& type, const std::string& machine);

struct SliceRequest {
    std::string model3mf;        // what to slice (Kestral writes it)
    std::string machine, process, filament;   // preset names (empty: OrcaSlicer's current)
    std::map<std::string, std::string> overrides;   // Orca setting key -> value
    std::string bedType;         // empty: OrcaSlicer's current
    std::vector<std::string> filamentColors;   // more than one: that many filaments, these colors (#RRGGBB)
    std::vector<std::string> filamentPresets;  // profile of each of those filaments ("" or missing: `filament`)
    bool arrange = true;
    bool orient = false;         // let OrcaSlicer turn parts to a good printing side
    std::string outDir;
    std::string name;            // job name
};

struct SliceResult {
    bool ok = false;
    std::string error;
    std::string gcode3mf;        // print ready file
    double seconds = 0;          // estimated print time
    double grams = 0;            // filament weight
    double meters = 0;
    int plates = 0;
    std::string machine, process, filament, bedType;
    std::vector<std::string> warnings;
    std::string log;             // OrcaSlicer's own output (last part), for troubleshooting
};

SliceResult slice(const OrcaSetup& s, const SliceRequest& r, const std::atomic<bool>* cancel);
// Every setting the engine would use with these profiles (its own defaults filled in), as JSON text.
// Runs the engine for a moment. workDir gets the profile files.
std::string fullSettings(const OrcaSetup& s, const std::string& machine, const std::string& process, const std::string& filament,
                         const std::string& workDir, const std::atomic<bool>* cancel, std::string& error);

// Friendly names for common settings ("wall loops" -> "wall_loops") and checks the value.
std::string normalizeSettingKey(const std::string& key);
// A short description of the settings people change most, for the assistant.
std::string commonSettingsHelp();

std::string formatDuration(double seconds);

// The LAN access code OrcaSlicer saved for a Bambu printer (by serial), empty if none. Only for
// filling in Kestral's own printer settings: never shown or sent anywhere else.
std::string orcaSavedAccessCode(const OrcaSetup& s, const std::string& serial);

// OrcaSlicer's settings folder (%APPDATA%\OrcaSlicer), and one plain text value from its OrcaSlicer.conf
// ("app" section first). Never used for access codes.
std::string orcaDataDir();
std::string orcaConfString(const std::string& key);

// One file from a zip (3MF) archive, unpacked. Empty when missing.
std::string readZipEntry(const std::string& zipPath, const std::string& entry);

// Developer check: Kestral --slice-test <model.3mf> [orca program]. Slices with the profiles OrcaSlicer
// has selected and prints the result. No window.
int sliceSelfTest(const std::string& model3mf, const std::string& orcaExe);

} // namespace kst::print
