// Kestral's slicer engine: OrcaSlicer's own slicing program, run hidden by Kestral (never a window).
// Kestral keeps its own copy in %APPDATA%\Kestral\engine\<version> (Linux: ~/.local/share/kestral),
// downloaded from OrcaSlicer's official releases, so slicing works without installing OrcaSlicer and
// updates install from the Slice tab. An installed OrcaSlicer is used when Kestral has no copy yet.
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace kst::print {

struct Engine {
    std::string exe;        // program to run (empty: none)
    std::string version;    // e.g. "2.4.2" (empty: unknown)
    bool managed = false;   // Kestral's own copy (false: the OrcaSlicer installed on this computer)
};

struct EngineRelease {
    std::string version;    // "2.5.0"
    std::string tag;        // "v2.5.0"
    std::string assetName, url, sha256;
    long long size = 0;
    std::string notesUrl;   // the release page
    std::string error;      // empty: found
};

std::string engineRoot();
// Kestral's own engine, after switching to one installed last time (call once at start).
Engine managedEngine();
void applyPendingEngine();
// The newest OrcaSlicer release for this system (GitHub). Blocking, worker thread only.
EngineRelease latestEngineRelease(const std::atomic<bool>* cancel);
// Downloads, checks and unpacks a release. It becomes the engine at the next start of Kestral.
// progress(stage text, 0..1 or -1).
bool installEngine(const EngineRelease& r, const std::function<void(const std::string&, double)>& progress,
                   const std::atomic<bool>* cancel, std::string& error);
// The version an installed OrcaSlicer program reports (asks it; about a second; cached per path).
std::string engineVersionOf(const std::string& exe);
// <0, 0, >0 like strcmp, for "2.4.2" style versions (a "-beta" part sorts before the plain one).
int compareVersions(const std::string& a, const std::string& b);
// Version installed and waiting for a restart of Kestral (empty: none).
std::string pendingEngineVersion();

// Unpacks a whole zip archive into a folder.
bool unzipAll(const std::string& zipPath, const std::string& destDir, std::string& error,
              const std::function<void(double)>& progress = {});

} // namespace kst::print
