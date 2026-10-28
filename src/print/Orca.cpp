#include "print/Orca.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Embed.h"
#include "print/Engine.h"
#include "platform/Platform.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace kst::print {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// OrcaSlicer.conf is JSON followed by a "# MD5 checksum" line.
Value parseConf(const std::string& text) {
    size_t end = text.rfind('}');
    if (end == std::string::npos) return {};
    return Value::parse(text.substr(0, end + 1));
}

std::string orcaConfigDir() {
#ifdef _WIN32
    fs::path app = u8path(platform::appDataDir()).parent_path();   // %APPDATA%
    return pathToU8(app / "OrcaSlicer");
#else
    const char* home = std::getenv("HOME");
    std::string h = home ? home : ".";
    std::error_code ec;
    std::string flat = h + "/.var/app/io.github.softfever.OrcaSlicer/config/OrcaSlicer";
    std::string xdg = std::getenv("XDG_CONFIG_HOME") ? std::string(std::getenv("XDG_CONFIG_HOME")) + "/OrcaSlicer" : h + "/.config/OrcaSlicer";
    if (fs::exists(xdg + "/OrcaSlicer.conf", ec)) return xdg;
    if (fs::exists(flat + "/OrcaSlicer.conf", ec)) return flat;
    return xdg;
#endif
}

std::string bedTypeName(const std::string& v) {
    static const char* names[] = {"Default Plate", "Cool Plate", "Engineering Plate", "High Temp Plate", "Textured PEI Plate", "Textured Cool Plate", "Supertack Plate"};
    if (!v.empty() && std::all_of(v.begin(), v.end(), ::isdigit)) {
        int i = std::atoi(v.c_str());
        if (i >= 1 && i <= 6) return names[i];
        return {};
    }
    return v;
}

std::string bambuModel(const std::string& code) {
    static const std::map<std::string, std::string> m = {
        {"N1", "Bambu Lab A1 mini"}, {"N2S", "Bambu Lab A1"}, {"C11", "Bambu Lab P1P"}, {"C12", "Bambu Lab P1S"},
        {"C13", "Bambu Lab X1E"}, {"BL-P001", "Bambu Lab X1 Carbon"}, {"BL-P002", "Bambu Lab X1"}, {"O1D", "Bambu Lab H2D"},
        {"N6", "Bambu Lab P2S"}, {"N7", "Bambu Lab P2S"},
    };
    auto it = m.find(code);
    return it != m.end() ? it->second : code;
}

void scanPresetDir(const fs::path& dir, const std::string& type, bool user, std::vector<Preset>& out, std::set<std::string>* seen) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || toLower(pathToU8(it->path().extension())) != ".json") continue;
        Value v = Value::parse(readFile(it->path()));
        if (!v.isObject()) continue;
        // A vendor's printer model description ("Bambu Lab A1 mini" without a nozzle) sits next to
        // the real printer presets but is not one: OrcaSlicer refuses it as a settings file.
        std::string ft = v["type"].asString();
        if (ft == "machine_model" || (!ft.empty() && ft != type)) continue;
        Preset p;
        p.type = type;
        p.user = user;
        p.path = pathToU8(it->path());
        p.name = v["name"].asString(pathToU8(it->path().stem()));
        if (seen) {
            if (seen->count(type + "|" + p.name)) continue;
            seen->insert(type + "|" + p.name);
        }
        p.inherits = v["inherits"].asString();
        p.printerModel = v["printer_model"].asString();
        for (auto& c : v["compatible_printers"].items()) p.compatible.push_back(c.asString());
        // System base files (fdm_*_common) are building blocks, not presets to pick.
        if (!user && v["instantiation"].asString("true") == "false") p.compatible.push_back("\x01" "base");
        out.push_back(std::move(p));
    }
}

} // namespace

// ---------------------------------------------------------------- zip (reading the sliced 3MF)

std::string readZipEntry(const std::string& zipPath, const std::string& entry) {
    std::string z = readFile(u8path(zipPath));
    if (z.size() < 22) return {};
    auto u16 = [&](size_t o) { return (uint32_t)(uint8_t)z[o] | (uint32_t)(uint8_t)z[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    // End of central directory.
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22; i + 1 > 0 && z.size() - i < 70000; --i) {
        if (u32(i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return {};
    size_t count = u16(eocd + 10), cd = u32(eocd + 16);
    size_t p = cd;
    for (size_t k = 0; k < count && p + 46 <= z.size(); ++k) {
        if (u32(p) != 0x02014b50) break;
        uint32_t method = u16(p + 10), csize = u32(p + 20), usize = u32(p + 24);
        size_t nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32);
        uint32_t local = u32(p + 42);
        std::string name = z.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (name != entry) continue;
        if (local + 30 > z.size()) return {};
        size_t data = local + 30 + u16(local + 26) + u16(local + 28);
        if (data + csize > z.size()) return {};
        if (method == 0) return z.substr(data, csize);
        if (method != 8) return {};
        std::string out(usize ? usize : csize * 4, '\0');
        z_stream s{};
        s.next_in = (Bytef*)&z[data];
        s.avail_in = csize;
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return {};
        int rc;
        do {
            if (s.total_out >= out.size()) out.resize(out.size() * 2);
            s.next_out = (Bytef*)&out[s.total_out];
            s.avail_out = (uInt)(out.size() - s.total_out);
            rc = inflate(&s, Z_NO_FLUSH);
        } while (rc == Z_OK);
        out.resize(s.total_out);
        inflateEnd(&s);
        return rc == Z_STREAM_END ? out : std::string();
    }
    return {};
}

namespace {

std::string xmlAttr(const std::string& tag, const std::string& attr) {
    std::string key = attr + "=\"";
    size_t a = tag.find(key);
    if (a == std::string::npos) return {};
    a += key.size();
    size_t b = tag.find('"', a);
    return b == std::string::npos ? std::string() : tag.substr(a, b - a);
}

} // namespace

std::string orcaDataDir() { return orcaConfigDir(); }

std::string orcaConfString(const std::string& key) {
    Value conf = parseConf(readFile(u8path(orcaConfigDir()) / "OrcaSlicer.conf"));
    if (!conf.isObject()) return {};
    const Value& a = conf["app"][key];
    if (a.isString()) return a.asString();
    return conf[key].isString() ? conf[key].asString() : std::string();
}

// ===================================================================== setup

OrcaSetup findOrca(const std::string& orcaExe) {
    OrcaSetup s;
    std::error_code ec;
    s.exe = orcaExe;
    if (s.exe.empty() || (s.exe.rfind("flatpak:", 0) != 0 && !fs::exists(u8path(s.exe), ec))) {
        s.exe = managedEngine().exe;   // Kestral's own copy first, then an installed OrcaSlicer
        if (s.exe.empty()) s.exe = platform::findOrcaSlicer();
    }
    s.configDir = orcaConfigDir();
    fs::path cfg = u8path(s.configDir);
    std::set<std::string> seen;
    Value conf = parseConf(readFile(cfg / "OrcaSlicer.conf"));
    // Your own presets first (they win over built in ones with the same name). OrcaSlicer can keep
    // several user folders (one per login); the one it uses now ("preset_folder") comes first, so an
    // older copy of a preset with the same name never wins.
    std::vector<fs::path> userDirs;
    std::string active = conf.isObject() ? conf["app"]["preset_folder"].asString(conf["preset_folder"].asString()) : std::string();
    if (!active.empty() && fs::is_directory(cfg / "user" / u8path(active), ec)) userDirs.push_back(cfg / "user" / u8path(active));
    for (auto& acc : fs::directory_iterator(cfg / "user", ec))
        if (acc.is_directory(ec) && (userDirs.empty() || acc.path() != userDirs.front())) userDirs.push_back(acc.path());
    for (auto& [sub, type] : std::vector<std::pair<std::string, std::string>>{{"machine", "machine"}, {"process", "process"}, {"filament", "filament"}})
        for (auto& dir : userDirs) scanPresetDir(dir / sub, type, true, s.presets, &seen);
    // Built in presets: OrcaSlicer copies the vendors you use into its settings folder; the
    // installed program has all of them (next to the exe on Windows, one folder up on Linux).
    std::vector<fs::path> roots = {cfg / "system"};
    if (!s.exe.empty() && s.exe.rfind("flatpak:", 0) != 0) {
        roots.push_back(u8path(s.exe).parent_path() / "resources" / "profiles");
        roots.push_back(u8path(s.exe).parent_path().parent_path() / "resources" / "profiles");
    }
    for (auto& root : roots)
        for (auto& vendor : fs::directory_iterator(root, ec)) {
            if (!vendor.is_directory(ec)) continue;
            for (auto& type : {"machine", "process", "filament"}) scanPresetDir(vendor.path() / type, type, false, s.presets, &seen);
        }

    if (conf.isObject()) {
        s.machine = conf["presets"]["machine"].asString();
        for (auto& op : conf["orca_presets"].items()) {
            if (op["machine"].asString() != s.machine) continue;
            s.process = op["process"].asString();
            s.filament = op["filament"].asString();
            s.bedType = bedTypeName(op["curr_bed_type"].asString());
        }
        if (s.process.empty()) s.process = conf["presets"]["process"].asString(conf["presets"]["print"].asString());
        if (s.filament.empty()) s.filament = conf["presets"]["filament"].asString();
        if (s.bedType.empty()) s.bedType = bedTypeName(conf["app"]["curr_bed_type"].asString());
        for (auto& [serial, m] : conf["local_machines"].members())
            s.knownPrinters.push_back({serial, m["dev_name"].asString(), m["dev_ip"].asString(), bambuModel(m["printer_type"].asString())});
    }
    if (s.exe.empty()) s.problem = "The slicer engine is not installed yet: press Install slicer engine in Kestral's Slice tab.";
    else if (s.presets.empty()) s.problem = "No printer profiles were found.";
    else if (s.machine.empty()) s.problem = "No printer chosen yet: pick your printer in Kestral's Slice tab.";
    return s;
}

std::string orcaSavedAccessCode(const OrcaSetup& s, const std::string& serial) {
    Value conf = parseConf(readFile(u8path(s.configDir) / "OrcaSlicer.conf"));
    return conf["user_access_code"][serial].asString();
}

const Preset* findPreset(const OrcaSetup& s, const std::string& type, const std::string& name) {
    for (auto& p : s.presets)
        if (p.type == type && p.name == name) return &p;
    std::string ln = toLower(name);
    for (auto& p : s.presets)
        if (p.type == type && toLower(p.name) == ln) return &p;
    return nullptr;
}

std::vector<const Preset*> searchPresets(const OrcaSetup& s, const std::string& type, const std::string& query, size_t max) {
    std::vector<std::string> words;
    std::istringstream ss(toLower(query));
    for (std::string w; ss >> w;) words.push_back(w);
    std::vector<std::pair<int, const Preset*>> hits;
    for (auto& p : s.presets) {
        if (!type.empty() && p.type != type) continue;
        if (!p.user && std::find(p.compatible.begin(), p.compatible.end(), "\x01" "base") != p.compatible.end()) continue;
        std::string nm = toLower(p.name), extra = toLower(p.printerModel + " " + p.inherits);
        int score = 0;
        bool all = true;
        for (auto& w : words) {
            if (nm.find(w) != std::string::npos) score += 10;          // in its own name counts most
            else if (extra.find(w) != std::string::npos) score += 3;   // only in what it is based on
            else { all = false; break; }
        }
        if (!all) continue;
        if (p.user) score += 5;
        hits.push_back({score, &p});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<const Preset*> out;
    for (auto& h : hits) {
        if (out.size() >= max) break;
        out.push_back(h.second);
    }
    return out;
}

const Preset* systemBase(const OrcaSetup& s, const Preset& p) {
    const Preset* cur = &p;
    for (int guard = 0; cur && guard < 16; ++guard) {
        if (!cur->user) return cur;
        if (cur->inherits.empty()) return nullptr;
        cur = findPreset(s, cur->type, cur->inherits);
    }
    return nullptr;
}

std::vector<const Preset*> presetChoices(const OrcaSetup& s, const std::string& type, const std::string& machine) {
    std::vector<const Preset*> out;
    auto hidden = [](const Preset& p) {
        return !p.user && std::find(p.compatible.begin(), p.compatible.end(), "\x01" "base") != p.compatible.end();
    };
    // A vendor's folder (".../profiles/BBL/filament/x.json" -> ".../profiles/BBL").
    auto vendorDir = [](const Preset& p) { return pathToU8(u8path(p.path).parent_path().parent_path()); };
    const Preset* mp = type == "machine" ? nullptr : findPreset(s, "machine", machine);
    const Preset* mbase = mp ? systemBase(s, *mp) : nullptr;
    std::string base = mbase ? mbase->name : machine, vendor = mbase ? vendorDir(*mbase) : std::string();
    for (auto& p : s.presets) {
        if (p.type != type || hidden(p)) continue;
        if (type != "machine" && !machine.empty()) {
            // A profile of yours fits when it names this printer or, naming none, its base does.
            const Preset* ref = &p;
            if (p.user && p.compatible.empty()) ref = systemBase(s, p);
            bool fits;
            if (!ref) fits = true;
            else if (ref->compatible.empty()) fits = !ref->user ? vendorDir(*ref) == vendor : true;
            else fits = std::find(ref->compatible.begin(), ref->compatible.end(), base) != ref->compatible.end() ||
                        std::find(ref->compatible.begin(), ref->compatible.end(), machine) != ref->compatible.end();
            if (!fits) continue;
        }
        out.push_back(&p);
    }
    std::stable_sort(out.begin(), out.end(), [](const Preset* a, const Preset* b) {
        if (a->user != b->user) return a->user;
        return toLower(a->name) < toLower(b->name);
    });
    return out;
}

std::string resolvePreset(const OrcaSetup& s, const Preset& p, std::string& err, std::string* systemName) {
    // Follow "inherits" up to the root, then lay each level over its parent.
    if (systemName) systemName->clear();
    std::vector<Value> chain;
    std::set<std::string> visited;
    const Preset* cur = &p;
    while (cur) {
        if (visited.count(cur->path)) { err = "Profile loop at " + cur->name; return {}; }
        visited.insert(cur->path);
        if (systemName && systemName->empty() && !cur->user) *systemName = cur->name;   // the first built in level
        Value v = Value::parse(readFile(u8path(cur->path)));
        if (!v.isObject()) { err = "Could not read profile " + cur->path; return {}; }
        chain.push_back(v);
        std::string parent = v["inherits"].asString();
        if (parent.empty()) break;
        const Preset* next = nullptr;
        // A user preset's parent is a built in one; prefer that over a same named user copy.
        for (auto& q : s.presets)
            if (q.type == cur->type && q.name == parent && !q.user) { next = &q; break; }
        if (!next) next = findPreset(s, cur->type, parent);
        if (!next) { err = "The base profile \"" + parent + "\" of \"" + cur->name + "\" was not found in OrcaSlicer's profiles."; return {}; }
        cur = next;
    }
    Value merged = Value::object();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (auto& [k, v] : it->members()) merged.set(k, v);
    merged.set("inherits", Value(std::string()));
    merged.set("name", Value(p.name));
    if (merged["type"].asString().empty()) merged.set("type", Value(p.type));
    merged.set("from", Value(std::string(p.user ? "User" : "system")));   // the CLI refuses any other value
    return merged.dump();
}

// ===================================================================== settings

std::string normalizeSettingKey(const std::string& key) {
    std::string k = toLower(key);
    for (auto& c : k) if (c == ' ' || c == '-') c = '_';
    static const std::map<std::string, std::string> alias = {
        {"walls", "wall_loops"}, {"wall_count", "wall_loops"}, {"perimeters", "wall_loops"}, {"wall_loop", "wall_loops"},
        {"infill", "sparse_infill_density"}, {"infill_density", "sparse_infill_density"}, {"infill_percent", "sparse_infill_density"},
        {"infill_pattern", "sparse_infill_pattern"}, {"pattern", "sparse_infill_pattern"},
        {"layer", "layer_height"}, {"layer_heights", "layer_height"},
        {"top_layers", "top_shell_layers"}, {"bottom_layers", "bottom_shell_layers"},
        {"supports", "enable_support"}, {"support", "enable_support"}, {"support_type", "support_type"},
        {"brim", "brim_type"}, {"brim_width", "brim_width"},
        {"nozzle_temp", "nozzle_temperature"}, {"nozzle_temperature", "nozzle_temperature"},
        {"bed_temp", "hot_plate_temp"}, {"bed_temperature", "hot_plate_temp"},
        {"speed", "outer_wall_speed"}, {"ironing", "ironing_type"},
    };
    auto it = alias.find(k);
    return it != alias.end() ? it->second : k;
}

std::string commonSettingsHelp() {
    return "wall_loops (walls, e.g. \"3\"), sparse_infill_density (e.g. \"25%\"), sparse_infill_pattern (grid, gyroid, cubic, "
           "honeycomb, line, rectilinear...), layer_height (mm, e.g. \"0.2\"), top_shell_layers, bottom_shell_layers, "
           "enable_support (\"1\"/\"0\"), support_type (\"normal(auto)\", \"tree(auto)\"), brim_type (\"auto_brim\", \"outer_only\", "
           "\"no_brim\"), brim_width (mm), ironing_type (\"no ironing\", \"top\"), outer_wall_speed / inner_wall_speed (mm/s), "
           "nozzle_temperature and nozzle_temperature_initial_layer (filament, C), hot_plate_temp / textured_plate_temp (bed, C), "
           "seam_position (aligned, nearest, back, random). Any OrcaSlicer setting key works.";
}

std::string formatDuration(double sec) {
    int s = (int)std::lround(sec);
    int h = s / 3600, m = (s % 3600) / 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    return std::to_string(std::max(m, 1)) + "m";
}

// ===================================================================== slicing

namespace {

// The three profiles as the engine's command line wants them, with the changes asked for, written
// to outDir as machine.json, process.json and filament.json.
bool prepareProfiles(const OrcaSetup& s, const std::string& machine, const std::string& process, const std::string& filament,
                     const std::map<std::string, std::string>& overrides, const std::string& outDir, std::string& error,
                     const std::vector<std::string>& colors = {}, std::string* filamentList = nullptr,
                     const std::vector<std::string>& filPresets = {}) {
    const Preset* mp = findPreset(s, "machine", machine);
    const Preset* pp = process.empty() ? nullptr : findPreset(s, "process", process);
    const Preset* fp = filament.empty() ? nullptr : findPreset(s, "filament", filament);
    if (!mp) { error = "Printer profile \"" + machine + "\" not found."; return false; }
    if (!pp) { error = "Process profile \"" + process + "\" not found (pick one with list_print_profiles)."; return false; }
    if (!fp) { error = "Filament profile \"" + filament + "\" not found (pick one with list_print_profiles)."; return false; }

    std::string err, machineSystem;
    Value m = Value::parse(resolvePreset(s, *mp, err, &machineSystem));
    Value p = err.empty() ? Value::parse(resolvePreset(s, *pp, err)) : Value();
    Value f = err.empty() ? Value::parse(resolvePreset(s, *fp, err)) : Value();
    if (!err.empty()) { error = err; return false; }
    if (machineSystem.empty()) machineSystem = mp->name;

    // The CLI only slices when the process names the printer in "compatible_printers", and it
    // knows a printer by its built in base ("Bambu Lab A1 mini 0.4 nozzle", not "my PETG copy").
    // A flattened user profile has no base any more, so the printer goes in as that base: same
    // settings, the name the CLI expects. The process then lists exactly that printer. (OrcaSlicer's
    // own window does this matching through the profile's parent.)
    m.set("name", Value(machineSystem));
    m.set("from", Value(std::string("system")));
    m.set("printer_settings_id", Value(machineSystem));
    Value only = Value::array();
    only.push(Value(machineSystem));
    p.set("compatible_printers", only);
    p.set("compatible_printers_condition", Value(std::string()));
    f.set("compatible_printers", Value::array());
    f.set("compatible_printers_condition", Value(std::string()));
    for (Value* v : {&p, &f}) {
        v->set("compatible_prints", Value::array());
        v->set("compatible_prints_condition", Value(std::string()));
    }

    // The other filaments (several colors): their own profiles, flattened the same way.
    std::vector<Value> fils;
    if (colors.size() > 1) {
        std::map<std::string, Value> cache;
        for (size_t i = 0; i < colors.size(); ++i) {
            std::string nm = i < filPresets.size() ? filPresets[i] : std::string();
            if (nm.empty() || nm == filament) { fils.push_back(f); continue; }
            auto c = cache.find(nm);
            if (c == cache.end()) {
                const Preset* op = findPreset(s, "filament", nm);
                Value fv = f;
                if (op) {
                    std::string e2;
                    Value got = Value::parse(resolvePreset(s, *op, e2));
                    if (e2.empty() && got.isObject()) {
                        fv = got;
                        fv.set("compatible_printers", Value::array());
                        fv.set("compatible_printers_condition", Value(std::string()));
                        fv.set("compatible_prints", Value::array());
                        fv.set("compatible_prints_condition", Value(std::string()));
                    }
                }
                c = cache.emplace(nm, fv).first;
            }
            fils.push_back(c->second);
        }
    }

    // Changes asked for: each goes to whichever profile has that setting (process by default).
    auto setOne = [](Value& target, const std::string& key, const std::string& v) {
        const Value& old = target[key];
        bool listKey = key == "flush_volumes_matrix" || key == "flush_volumes_vector";
        if (listKey) {
            // A whole list: "a,b,c".
            Value arr = Value::array();
            size_t st = 0;
            const char sep = ',';
            while (st <= v.size()) {
                size_t c = v.find(sep, st);
                arr.push(Value(v.substr(st, c == std::string::npos ? std::string::npos : c - st)));
                if (c == std::string::npos) break;
                st = c + 1;
            }
            target.set(key, arr);
        } else if (old.isArray()) {
            Value arr = Value::array();
            size_t n = std::max<size_t>(old.size(), 1);
            for (size_t i = 0; i < n; ++i) arr.push(Value(v));
            target.set(key, arr);
        } else {
            target.set(key, Value(v));
        }
    };
    for (auto& [rawKey, value] : overrides) {
        std::string key = normalizeSettingKey(rawKey);
        std::string v = value;
        if (key == "sparse_infill_density" && !v.empty() && v.back() != '%') v += "%";
        if (key == "enable_support") v = (toLower(v) == "true" || v == "1" || toLower(v) == "yes" || toLower(v) == "on") ? "1" : "0";
        Value* target = &p;
        if (!p.has(key)) {
            if (f.has(key) || key.rfind("filament_", 0) == 0) target = &f;
            else if (m.has(key) || key.rfind("machine_", 0) == 0 || key.rfind("printer_", 0) == 0) target = &m;
        }
        setOne(*target, key, v);
        if (target == &f)
            for (auto& fi : fils) setOne(fi, key, v);   // every filament gets the change
    }

    if (colors.size() == 1 && !colors[0].empty()) {   // one filament: its color (the preview and the AMS match use it)
        Value c = Value::array();
        c.push(Value(colors[0]));
        f.set("filament_colour", c);
    }
    std::error_code ec;
    fs::create_directories(u8path(outDir), ec);
    auto write = [](const std::string& path, const std::string& text) {
        std::ofstream o(u8path(path), std::ios::binary | std::ios::trunc);
        o << text;
        return (bool)o;
    };
    if (!write(outDir + "/machine.json", m.dump()) || !write(outDir + "/process.json", p.dump()) || !write(outDir + "/filament.json", f.dump())) {
        error = "Could not write to " + outDir;
        return false;
    }
    if (filamentList) *filamentList = outDir + "/filament.json";
    if (colors.size() > 1 && filamentList) {
        // One filament per color: the same profile, each with its own color (the parts' colors).
        std::string list;
        for (size_t i = 0; i < colors.size(); ++i) {
            Value fi = i < fils.size() ? fils[i] : f;
            Value c = Value::array();
            c.push(Value(colors[i]));
            fi.set("filament_colour", c);
            std::string pth = outDir + "/filament_" + std::to_string(i + 1) + ".json";
            if (!write(pth, fi.dump())) { error = "Could not write to " + outDir; return false; }
            list += (list.empty() ? "" : ";") + pth;
        }
        *filamentList = list;
    }
    return true;
}

} // namespace

std::string fullSettings(const OrcaSetup& s, const std::string& machine, const std::string& process, const std::string& filament,
                         const std::string& workDir, const std::atomic<bool>* cancel, std::string& error) {
    if (s.exe.empty()) { error = "No slicer engine"; return {}; }
    if (!prepareProfiles(s, machine, process, filament, {}, workDir, error)) return {};
    std::string out = workDir + "/all_settings.json";
    std::error_code ec;
    fs::remove(u8path(out), ec);
    platform::ProcResult pr = platform::runProcess(s.exe, {"--load-settings", workDir + "/machine.json;" + workDir + "/process.json",
                                                           "--load-filaments", workDir + "/filament.json", "--export-settings", out},
                                                   120000, cancel, workDir);
    std::string text = readFile(u8path(out));
    if (text.empty()) error = "The slicer engine did not list its settings" + (pr.error.empty() ? std::string() : ": " + pr.error);
    return text;
}

SliceResult slice(const OrcaSetup& s, const SliceRequest& r, const std::atomic<bool>* cancel) {
    SliceResult res;
    if (s.exe.empty()) { res.error = s.problem.empty() ? "OrcaSlicer is not installed." : s.problem; return res; }
    res.machine = r.machine.empty() ? s.machine : r.machine;
    res.process = r.process.empty() ? s.process : r.process;
    res.filament = r.filament.empty() ? s.filament : r.filament;
    res.bedType = r.bedType.empty() ? s.bedType : r.bedType;
    if (res.machine.empty()) { res.error = "No printer profile chosen (OrcaSlicer has none selected)."; return res; }
    std::string fPath;
    if (!prepareProfiles(s, res.machine, res.process, res.filament, r.overrides, r.outDir, res.error, r.filamentColors, &fPath, r.filamentPresets)) return res;
    std::error_code ec;
    std::string mPath = r.outDir + "/machine.json", pPath = r.outDir + "/process.json";

    std::string safe = r.name.empty() ? "Kestral" : r.name;
    for (auto& c : safe) if (std::strchr("<>:\"/\\|?* ", c)) c = '_';
    std::string outName = safe + ".gcode.3mf";
    fs::remove(u8path(r.outDir + "/" + outName), ec);
    std::vector<std::string> args = {"--slice", "0", "--arrange", r.arrange ? "1" : "0", "--orient", r.orient ? "1" : "0",
                                     "--load-settings", mPath + ";" + pPath, "--load-filaments", fPath,
                                     "--min-save", "--outputdir", r.outDir, "--export-3mf", outName};
    if (!res.bedType.empty()) args.push_back("--curr-bed-type=" + res.bedType);
    args.push_back(r.model3mf);
    log::info("Slicing with OrcaSlicer: " + res.machine + " | " + res.process + " | " + res.filament);
    platform::ProcResult pr = platform::runProcess(s.exe, args, 15 * 60 * 1000, cancel, r.outDir);
    res.log = pr.output.size() > 4000 ? pr.output.substr(pr.output.size() - 4000) : pr.output;
    if (pr.cancelled) { res.error = "Slicing was stopped."; return res; }
    if (pr.timedOut) { res.error = "Slicing took longer than 15 minutes and was stopped."; return res; }
    res.gcode3mf = r.outDir + "/" + outName;
    if (pr.exitCode != 0 || !fs::exists(u8path(res.gcode3mf), ec)) {
        static const std::map<int, std::string> codes = {
            {-2, "OrcaSlicer did not accept the settings."}, {-3, "OrcaSlicer could not find the model file."},
            {-5, "One of the profiles could not be read by OrcaSlicer."}, {-6, "OrcaSlicer could not read the model."},
            {-14, "OrcaSlicer ran out of memory (the model may be too detailed)."}, {-17, "The process profile does not fit this printer."},
            {-20, "The print profile is for a bigger bed than this printer has."}, {-21, "The parts could not be arranged on the plate."},
            {-50, "Nothing to slice: the plate is empty or the part does not fit the printer."},
            {-51, "Some settings are invalid for this print."}, {-52, "Part of the model is outside the printable area."},
            {-61, "This filament does not work with the chosen bed plate."}, {-65, "Some settings do not work with vase mode."},
            {-100, "Slicing failed."},
        };
        auto it = codes.find(pr.exitCode);
        res.error = it != codes.end() ? it->second : (pr.error.empty() ? "OrcaSlicer stopped with code " + std::to_string(pr.exitCode) : pr.error);
        // Its own last words help (on Linux it prints the reason).
        std::string tail = res.log.size() > 600 ? res.log.substr(res.log.size() - 600) : res.log;
        if (!tail.empty()) res.error += " OrcaSlicer said: " + tail;
        return res;
    }
    // Time, weight and warnings from the sliced file.
    std::string info = readZipEntry(res.gcode3mf, "Metadata/slice_info.config");
    size_t pos = 0;
    while ((pos = info.find("<plate>", pos)) != std::string::npos) {
        ++res.plates;
        size_t end = info.find("</plate>", pos);
        std::string plate = info.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        for (size_t q = 0; (q = plate.find("<metadata", q)) != std::string::npos; ++q) {
            std::string tag = plate.substr(q, plate.find('>', q) - q);
            std::string key = xmlAttr(tag, "key"), val = xmlAttr(tag, "value");
            if (key == "prediction") res.seconds += std::atof(val.c_str());
            else if (key == "weight") res.grams += std::atof(val.c_str());
        }
        for (size_t q = 0; (q = plate.find("<filament", q)) != std::string::npos; ++q) {
            std::string tag = plate.substr(q, plate.find('>', q) - q);
            res.meters += std::atof(xmlAttr(tag, "used_m").c_str());
        }
        for (size_t q = 0; (q = plate.find("<warning", q)) != std::string::npos; ++q) {
            std::string tag = plate.substr(q, plate.find('>', q) - q);
            std::string msg = xmlAttr(tag, "msg");
            if (!msg.empty()) res.warnings.push_back(msg);
        }
        pos = end == std::string::npos ? info.size() : end;
    }
    res.ok = true;
    return res;
}

int sliceSelfTest(const std::string& model3mf, const std::string& orcaExe) {
    OrcaSetup s = findOrca(orcaExe);
    std::printf("OrcaSlicer: %s\nprofiles: %zu\nselected: %s | %s | %s | bed %s\n", s.exe.empty() ? "(not found)" : s.exe.c_str(),
                s.presets.size(), s.machine.c_str(), s.process.c_str(), s.filament.c_str(), s.bedType.c_str());
    if (!s.problem.empty()) { std::printf("problem: %s\n", s.problem.c_str()); return 2; }
    SliceRequest r;
    r.model3mf = model3mf;
    r.outDir = (fs::temp_directory_path() / "kestral-slice-test").string();
    r.name = "slice-test";
    SliceResult res = slice(s, r, nullptr);
    if (!res.error.empty()) { std::printf("FAILED: %s\n", res.error.c_str()); return 1; }
    std::printf("OK: %s\ntime %s, %.1f g, %.2f m\n", res.gcode3mf.c_str(), formatDuration(res.seconds).c_str(), res.grams, res.meters);
    return 0;
}

} // namespace kst::print
