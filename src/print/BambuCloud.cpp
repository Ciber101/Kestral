#include "print/BambuCloud.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Embed.h"
#include "platform/Platform.h"
#include "print/Engine.h"
#include "print/Orca.h"
#include "print/Zip.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace kst::print {

namespace fs = std::filesystem;
using json::Value;

namespace {

// ===================================================================== the plugin's interface
// Bambu's network plugin (bambu_networking.dll / libbambu_networking.so, installed by OrcaSlicer)
// is a C++ library loaded at run time. Kestral calls a few of its exported functions. Their
// arguments are passed by value, so the two job records below must match the plugin's binary
// layout member for member: same types in the same order. The names here are Kestral's own.

// Callbacks Kestral hands to the plugin.
using CbPrinterUp = std::function<void(std::string topic)>;
using CbServerUp = std::function<void(int result, int reason)>;
using CbMessage = std::function<void(std::string printerId, std::string json)>;
using CbHttpError = std::function<void(unsigned status, std::string body)>;
using CbCountry = std::function<std::string()>;
using CbProgress = std::function<void(int stage, int code, std::string info)>;
using CbCancelled = std::function<bool()>;
using CbWait = std::function<bool(int stage, std::string info)>;
using CbOnMain = std::function<void(std::function<void()>)>;
using CbLocalUp = std::function<void(int status, std::string printerId, std::string info)>;

// A print job, as plugins from before the H2D (two nozzle) printers take it.
struct JobV1 {
    std::string printerId, taskName, projectName, presetName, file, configFile;
    int plate;
    std::string ftpFolder, ftpFile, ftpFileMd5, amsMap, amsMapInfo, connection, notes;
    int profileId = 0;
    int designId = 0;
    std::string modelId, printType, destFile, printerName;
    std::string printerIp;
    bool ftpTls;
    bool mqttTls;
    std::string user, accessCode;
    bool bedLevel, flowCal, vibrationCal, layerCheck, timelapse, useAms;
    std::string bedType, extra;
};

// The same job for newer plugins: nozzle and second AMS maps, and the automatic calibration flags.
struct JobV2 {
    std::string printerId, taskName, projectName, presetName, file, configFile;
    int plate;
    std::string ftpFolder, ftpFile, ftpFileMd5, nozzleMap, amsMap, amsMap2, amsMapInfo, nozzleInfo, connection, notes;
    int profileId = 0;
    int designId = 0;
    std::string modelId, printType, destFile, printerName;
    std::string printerIp;
    bool ftpTls;
    bool mqttTls;
    std::string user, accessCode;
    bool bedLevel, flowCal, vibrationCal, layerCheck, timelapse, useAms;
    std::string bedType, extra;
    int autoBedLevel{0};
    int autoFlowCal{0};
    int autoOffsetCal{0};
    bool extChangeAssist;
    bool emmcPrint;
};

// The exported functions Kestral looks up, by the shape of their arguments.
using PlugDebugCheck = bool (*)(bool);
using PlugVersion = std::string (*)();
using PlugNewAgent = void* (*)(std::string);
using PlugFreeAgent = int (*)(void*);
using PlugCall = int (*)(void*);
using PlugCallStr = int (*)(void*, std::string);
using PlugCert = int (*)(void*, std::string, std::string);
using PlugOnServer = int (*)(void*, CbServerUp);
using PlugOnMessage = int (*)(void*, CbMessage);
using PlugOnHttpError = int (*)(void*, CbHttpError);
using PlugCountry = int (*)(void*, CbCountry);
using PlugOnPrinter = int (*)(void*, CbPrinterUp);
using PlugOnMain = int (*)(void*, CbOnMain);
using PlugAsk = bool (*)(void*);
using PlugText = std::string (*)(void*);
using PlugTracking = int (*)(void*, bool);
using PlugHeaders = int (*)(void*, std::map<std::string, std::string>);
using PlugUserPrints = int (*)(void*, unsigned int*, std::string*);
using PlugPrint = int (*)(void*, JobV2, CbProgress, CbCancelled, CbWait);
using PlugPrintV1 = int (*)(void*, JobV1, CbProgress, CbCancelled, CbWait);
using PlugSend = int (*)(void*, std::string, std::string, int, int);
using PlugSendV1 = int (*)(void*, std::string, std::string, int);
using PlugOnLocal = int (*)(void*, CbLocalUp);
using PlugConnect = int (*)(void*, std::string, std::string, std::string, std::string, bool);
using PlugLocalPrint = int (*)(void*, JobV2, CbProgress, CbCancelled);
using PlugLocalPrintV1 = int (*)(void*, JobV1, CbProgress, CbCancelled);

// ===================================================================== helpers

#ifdef _WIN32
using LibHandle = HMODULE;
#else
using LibHandle = void*;
#endif

struct Plugin {
    LibHandle h = nullptr;
    template <class F> F fn(const char* name) const {
        if (!h) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(h, name)));
#else
        return reinterpret_cast<F>(dlsym(h, name));
#endif
    }
};

bool loadPlugin(Plugin& p, const std::string& path, std::string& err) {
#ifdef _WIN32
    fs::path fp = u8path(path);
    // Its own helper libraries (BambuSource, agora...) sit next to it.
    SetDllDirectoryW(fp.parent_path().wstring().c_str());
    p.h = LoadLibraryExW(fp.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!p.h) err = "Windows could not load Bambu's plugin (error " + std::to_string(GetLastError()) + ")";
#else
    p.h = dlopen(path.c_str(), RTLD_LAZY);
    if (!p.h) { const char* e = dlerror(); err = std::string("Could not load Bambu's plugin: ") + (e ? e : "unknown"); }
#endif
    return p.h != nullptr;
}

// The plugin calls back "on the main thread": the helper's main thread runs a queue for it while a
// worker does the job.
struct MainQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    void push(std::function<void()> f) {
        { std::lock_guard lk(m); q.push_back(std::move(f)); }
        cv.notify_one();
    }
    void pump(int ms) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock lk(m);
                if (!cv.wait_until(lk, until, [&] { return !q.empty(); })) return;
                f = std::move(q.front());
                q.pop_front();
            }
            try { if (f) f(); } catch (...) {}
        }
    }
};

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string countryCode() {
    std::string region = orcaConfString("region");
    if (region == "CHN" || region == "China") return "CN";
    if (region == "USA" || region == "Europe" || region == "North America") return "US";
    return "Others";
}

// The cloud error codes people actually see.
std::string errorText(int code) {
    switch (code) {
    case -2: case -6010: return "Could not reach the printer";
    case -6020: return "Could not reach Bambu's servers";
    case -17: return "Timed out";
    case -18: return "Stopped";
    case -3010: case -2010: return "Bambu's cloud did not create the print job";
    case -3020: case -2020: return "The upload check failed";
    case -3040: case -3050: case -3060: return "Bambu's cloud did not confirm the job";
    case -3070: case -2100: return "The print file is missing";
    case -3090: case -2040: return "The print file is too big (over 1 GB)";
    case -3100: case -2110: return "Uploading the print to Bambu's cloud failed";
    case -3120: case -2120: return "Bambu's cloud did not accept the print job";
    case -3130: return "The printer did not answer";
    case -3140: return "The printer is not ready for cloud prints yet (try again in a moment)";
    case -26: return "Bambu's plugin refused to sign the request";
    default: return "Bambu's cloud said no (code " + std::to_string(code) + ")";
    }
}

// The engine's certificate folder (OrcaSlicer's resources/cert).
std::string certDir() {
    std::vector<std::string> exes = {managedEngine().exe, platform::findOrcaSlicer()};
    std::error_code ec;
    for (auto& e : exes) {
        if (e.empty() || e.rfind("flatpak:", 0) == 0) continue;
        fs::path x = u8path(e).parent_path();
        for (fs::path c : {x / "resources" / "cert", x.parent_path() / "resources" / "cert"})
            if (fs::exists(c / "slicer_base64.cer", ec)) return pathToU8(c);
    }
    return {};
}

// Prepares the two files the cloud wants: the print, with the printer model filled in (the
// command line leaves it empty), and the same without G-code ("config" file for the task record).
bool prepareFiles(const std::string& gcode3mf, const std::string& model, const std::string& dir, std::string& printFile,
                  std::string& configFile, std::string& bedType, std::string& err) {
    std::vector<ZipEntry> zs;
    if (!readZip(gcode3mf, zs, err)) return false;
    for (auto& e : zs) {
        if (e.name == "Metadata/slice_info.config" && !model.empty()) {
            const std::string empty = "key=\"printer_model_id\" value=\"\"";
            size_t at = e.data.find(empty);
            if (at != std::string::npos) e.data.replace(at, empty.size(), "key=\"printer_model_id\" value=\"" + model + "\"");
        }
        if (e.name == "Metadata/plate_1.json") bedType = Value::parse(e.data)["bed_type"].asString();
    }
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    printFile = dir + "/" + pathToU8(u8path(gcode3mf).filename());
    configFile = dir + "/config.3mf";
    if (!writeZip(printFile, zs, err)) return false;
    std::vector<ZipEntry> cfg;
    for (auto& e : zs) {
        bool gcode = e.name.rfind("Metadata/plate_", 0) == 0 &&
                     (e.name.find(".gcode") != std::string::npos);
        if (!gcode) cfg.push_back(e);
    }
    return writeZip(configFile, cfg, err);
}

} // namespace

// ===================================================================== client side

std::string bambuPluginPath(std::string* version) {
    fs::path dir = u8path(orcaDataDir()) / "plugins";
    std::string v = orcaConfString("network_plugin_version");
    if (version) *version = v;
    std::error_code ec;
#ifdef _WIN32
    std::string pre = "bambu_networking", ext = ".dll";
#else
    std::string pre = "libbambu_networking", ext = ".so";
#endif
    if (!v.empty() && fs::exists(dir / u8path(pre + "_" + v + ext), ec)) return pathToU8(dir / u8path(pre + "_" + v + ext));
    if (fs::exists(dir / u8path(pre + ext), ec)) return pathToU8(dir / u8path(pre + ext));
    return {};
}

namespace {

// Runs the helper and reads what it wrote.
Value runHelper(const std::string& cmd, const Value& req, int timeoutMs, const std::atomic<bool>* cancel, std::string& err) {
    std::string self = platform::selfExe();
    if (self.empty()) { err = "Kestral could not find itself to start the helper"; return {}; }
    std::string dir = platform::tempDir() + "/bambu";
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    std::string reqFile = dir + "/" + cmd + "_request.json", resFile = dir + "/" + cmd + "_result.json";
    fs::remove(u8path(resFile), ec);
    {
        std::ofstream o(u8path(reqFile), std::ios::binary | std::ios::trunc);
        o << req.dump();
    }
    platform::ProcResult pr = platform::runProcess(self, {"--bambu-agent", cmd, reqFile, resFile}, timeoutMs, cancel, dir);
    fs::remove(u8path(reqFile), ec);
    Value res = Value::parse(readText(u8path(resFile)));
    fs::remove(u8path(resFile), ec);
    if (pr.cancelled) { err = "Stopped"; return {}; }
    if (!res.isObject()) {
        err = pr.timedOut ? "Bambu's plugin did not finish in time" : "Bambu's plugin stopped unexpectedly (exit " + std::to_string(pr.exitCode) + ")";
        return {};
    }
    for (auto& l : res["log"].items()) log::info("Bambu plugin: " + l.asString());
    return res;
}

} // namespace

BambuCloudStatus bambuCloudStatus(const std::atomic<bool>* cancel) {
    BambuCloudStatus st;
    st.pluginFound = !bambuPluginPath(&st.pluginVersion).empty();
    if (!st.pluginFound) {
        st.error = "Bambu's network plugin is not on this computer. Install OrcaSlicer, sign in to Bambu there once, and let it "
                   "install the Bambu network plugin.";
        return st;
    }
    std::string err;
    Value r = runHelper("status", Value::object(), 60000, cancel, err);
    if (!err.empty()) { st.error = err; return st; }
    st.connected = r["connected"].asBool();
    st.loggedIn = r["logged_in"].asBool();
    st.user = r["user"].asString();
    st.error = r["error"].asString();
    for (auto& l : r["log"].items()) st.detail += (st.detail.empty() ? "" : "; ") + l.asString();
    if (!r["plugin_version"].asString().empty()) st.pluginVersion = r["plugin_version"].asString();
    for (auto& d : r["devices"].items())
        st.devices.push_back({d["id"].asString(), d["name"].asString(), d["model"].asString(), d["online"].asBool(), d["state"].asString()});
    return st;
}

BambuPrintResult bambuCloudPrint(const BambuPrintJob& job, const std::atomic<bool>* cancel) {
    BambuPrintResult res;
    if (bambuPluginPath().empty()) {
        res.message = "Bambu's network plugin is not on this computer (OrcaSlicer installs it).";
        return res;
    }
    Value req = Value::object();
    req.set("dev_id", Value(job.devId));
    req.set("file", Value(pathToU8(fs::absolute(u8path(job.gcode3mf)))));
    req.set("name", Value(job.name));
    req.set("use_ams", Value(job.useAms));
    req.set("ams_slot", Value(job.amsSlot));
    Value amap = Value::array();
    for (int m : job.amsMapping) amap.push(Value((double)m));
    req.set("ams_map", amap);
    req.set("bed_leveling", Value(job.bedLeveling));
    req.set("flow_cali", Value(job.flowCali));
    req.set("timelapse", Value(job.timelapse));
    req.set("ip", Value(job.ip));
    req.set("access_code", Value(job.accessCode));   // only in the helper's request file, removed right after
    std::string err;
    Value r = runHelper("print", req, 6 * 60 * 1000, cancel, err);
    if (!err.empty()) { res.message = err; return res; }
    res.ok = r["ok"].asBool();
    res.code = (int)r["code"].asNumber(0);
    res.message = r["message"].asString();
    return res;
}

BambuPrintResult bambuCloudControl(const BambuPrintJob& job, const std::string& action, const std::atomic<bool>* cancel) {
    BambuPrintResult res;
    Value req = Value::object();
    req.set("dev_id", Value(job.devId));
    req.set("action", Value(action));
    req.set("ip", Value(job.ip));
    req.set("access_code", Value(job.accessCode));
    std::string err;
    Value r = runHelper("control", req, 90000, cancel, err);
    if (!err.empty()) { res.message = err; return res; }
    res.ok = r["ok"].asBool();
    res.code = (int)r["code"].asNumber(0);
    res.message = r["message"].asString();
    return res;
}

// ===================================================================== helper side

int runBambuAgent(int argc, char** argv) {
    if (argc < 5) return 64;
    std::string cmd = argv[2], reqFile = argv[3], resFile = argv[4];
    Value req = Value::parse(readText(u8path(reqFile)));
    Value out = Value::object();
    Value logs = Value::array();
    auto note = [&](const std::string& s) { logs.push(Value(s)); };
    auto finish = [&]() {
        out.set("log", logs);
        std::ofstream o(u8path(resFile), std::ios::binary | std::ios::trunc);
        o << out.dump();
        o.close();
        return 0;
    };
    auto fail = [&](const std::string& msg) {
        out.set("ok", Value(false));
        out.set("error", Value(msg));
        out.set("message", Value(msg));
        return finish();
    };

    std::string version;
    std::string lib = bambuPluginPath(&version);
    if (lib.empty()) return fail("Bambu's network plugin is not installed");
    Plugin p;
    std::string err;
    if (!loadPlugin(p, lib, err)) return fail(err);
    if (auto check = p.fn<PlugDebugCheck>("bambu_network_check_debug_consistent"); check && !check(false))
        note("the plugin says this is not a matching release build");
    std::string loaded = version;
    if (auto gv = p.fn<PlugVersion>("bambu_network_get_version")) loaded = gv();
    out.set("plugin_version", Value(loaded));
    bool legacy = loaded.rfind("01.", 0) == 0;

    auto create = p.fn<PlugNewAgent>("bambu_network_create_agent");
    if (!create) return fail("This Bambu plugin is not one Kestral knows");
    std::string dataDir = orcaDataDir();
    void* agent = create(dataDir);
    if (!agent) return fail("Bambu's plugin did not start");

    MainQueue mq;
    std::atomic<bool> serverUp{false};
    std::string cc = countryCode();
    if (auto f = p.fn<PlugCallStr>("bambu_network_set_config_dir")) f(agent, dataDir);
    if (auto f = p.fn<PlugCall>("bambu_network_init_log")) f(agent);
    std::string cert = certDir();
    if (cert.empty()) note("certificate folder not found (resources/cert)");
    else if (auto f = p.fn<PlugCert>("bambu_network_set_cert_file")) f(agent, cert, "slicer_base64.cer");
    if (auto f = p.fn<PlugCallStr>("bambu_network_set_country_code")) f(agent, cc);
    if (auto f = p.fn<PlugOnServer>("bambu_network_set_on_server_connected_fn"))
        f(agent, [&](int rc, int reason) {
            if (rc == 0) serverUp = true;
            mq.push([&, rc, reason] { note("server connection: " + std::to_string(rc) + " reason " + std::to_string(reason)); });
        });
    if (auto f = p.fn<PlugOnHttpError>("bambu_network_set_on_http_error_fn"))
        f(agent, [&](unsigned code, std::string body) {
            std::string b = body.substr(0, 200);
            mq.push([&, code, b] { note("http error " + std::to_string(code) + ": " + b); });
        });
    if (auto f = p.fn<PlugCountry>("bambu_network_set_get_country_code_fn")) f(agent, [cc] { return cc; });
    if (auto f = p.fn<PlugOnMain>("bambu_network_set_queue_on_main_fn")) f(agent, [&](std::function<void()> fn) { mq.push(std::move(fn)); });
    std::mutex msgM;
    std::map<std::string, std::string> lastMsg;   // printer -> its last report
    if (auto f = p.fn<PlugOnMessage>("bambu_network_set_on_message_fn"))
        f(agent, [&](std::string dev, std::string msg) { std::lock_guard lk(msgM); lastMsg[dev] = msg; });
    if (auto f = p.fn<PlugOnPrinter>("bambu_network_set_on_printer_connected_fn")) f(agent, [](std::string) {});
    std::atomic<int> localState{-1};
    std::string localMsg;
    std::mutex localM;
    if (auto f = p.fn<PlugOnLocal>("bambu_network_set_on_local_connect_fn"))
        f(agent, [&](int st, std::string, std::string msg) {
            { std::lock_guard lk(localM); localMsg = msg; }
            localState = st;
        });
    // No usage tracking (OrcaSlicer turns it off the same way).
    if (auto f = p.fn<PlugTracking>("bambu_network_track_enable")) f(agent, false);
    if (auto f = p.fn<PlugCall>("bambu_network_track_remove_files")) f(agent);
    {
        std::map<std::string, std::string> h;
        h["X-BBL-Client-Type"] = "slicer";
        h["X-BBL-Client-Name"] = "BambuStudio";   // what OrcaSlicer sends
        h["X-BBL-Client-Version"] = p.fn<void*>("bambu_network_get_my_token") ? std::string() : "01.10.01.50";
#ifdef _WIN32
        h["X-BBL-OS-Type"] = "windows";
#else
        h["X-BBL-OS-Type"] = "linux";
#endif
        h["X-BBL-Device-ID"] = orcaConfString("slicer_uuid");
        h["X-BBL-Language"] = "en";
        if (auto f = p.fn<PlugHeaders>("bambu_network_set_extra_http_header")) f(agent, h);
    }
    if (auto f = p.fn<PlugCall>("bambu_network_start")) f(agent);
    if (auto f = p.fn<PlugCall>("bambu_network_connect_server")) f(agent);
    auto isUp = p.fn<PlugAsk>("bambu_network_is_server_connected");
    auto t0 = std::chrono::steady_clock::now();
    auto isLogin = p.fn<PlugAsk>("bambu_network_is_user_login");
    // The login comes back from the plugin's saved session right after start; the live connection to
    // Bambu's message server can take longer and is not needed to send a print (that goes over https).
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(cmd == "status" ? 25 : 12)) {
        mq.pump(200);
        if (serverUp || (isUp && isUp(agent))) break;
        // Not signed in: the saved session would be back by now; nothing to wait for.
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(4) && !(isLogin && isLogin(agent))) break;
    }
    bool connected = serverUp || (isUp && isUp(agent));
    bool login = isLogin && isLogin(agent);
    note(std::string("plugin ") + loaded + (legacy ? " (older interface)" : "") + ", signed in: " + (login ? "yes" : "no") +
         ", message server: " + (connected ? "connected" : "not connected") + ", region " + cc + (cert.empty() ? ", no certificate" : ""));
    out.set("connected", Value(connected));
    out.set("logged_in", Value(login));
    if (auto f = p.fn<PlugText>("bambu_network_get_user_name"); f && login) out.set("user", Value(f(agent)));

    // Printers bound to the account (never their access codes).
    Value devices = Value::array();
    std::string modelOf;
    if (login) {
        if (auto f = p.fn<PlugUserPrints>("bambu_network_get_user_print_info")) {
            unsigned code = 0;
            std::string body;
            f(agent, &code, &body);
            Value j = Value::parse(body);
            for (auto& d : j["devices"].items()) {
                Value o = Value::object();
                o.set("id", Value(d["dev_id"].asString()));
                o.set("name", Value(d["dev_name"].asString()));
                o.set("model", Value(d["dev_model_name"].asString()));
                o.set("online", Value(d["dev_online"].asBool()));
                o.set("state", Value(d["print_status"].asString()));
                if (d["dev_id"].asString() == req["dev_id"].asString()) modelOf = d["dev_model_name"].asString();
                devices.push(o);
            }
            if (code != 200 && code != 0) note("printer list: http " + std::to_string(code) + " " + body.substr(0, 200));
            note("printers on the account: " + std::to_string(j["devices"].size()));
        }
    }
    out.set("devices", devices);

    if (cmd == "status") {
        if (!login)
            out.set("error", Value(std::string("Not signed in to a Bambu account. That is fine: like OrcaSlicer, Kestral then sends straight to the "
                                               "printer over the home network with its IP and access code (Preferences > Printer).")));
        out.set("ok", Value(true));
        if (auto f = p.fn<PlugFreeAgent>("bambu_network_destroy_agent")) f(agent);
        return finish();
    }

    if (cmd == "print") {
        std::string devId = req["dev_id"].asString();
        std::string ip = req["ip"].asString(), code = req["access_code"].asString();
        // Two ways, like OrcaSlicer: through Bambu's cloud when signed in, otherwise straight to the
        // printer on the home network (its IP and access code; the plugin signs the job).
        bool lan = !login && !ip.empty() && !code.empty();
        if (!login && !lan)
            return fail("Not signed in to Bambu, and Kestral does not have the printer's IP and access code. Add them in "
                        "Preferences > Printer (or use the OrcaSlicer button there).");
        if (login) {
            bool known = false;
            for (auto& d : devices.items()) known |= d["id"].asString() == devId;
            if (!known && !devices.items().empty()) return fail("The printer " + devId + " is not on this Bambu account");
        }
        if (modelOf.empty()) {
            // The printer's model from OrcaSlicer's own list of printers it has seen (e.g. "N1").
            std::string t = readText(u8path(orcaDataDir()) / "OrcaSlicer.conf");
            size_t e = t.rfind('}');
            Value conf = e == std::string::npos ? Value() : Value::parse(t.substr(0, e + 1));
            modelOf = conf["local_machines"][devId]["printer_type"].asString();
        }
        if (lan) {
            auto conn = p.fn<PlugConnect>("bambu_network_connect_printer");
            if (!conn) return fail("This Bambu plugin cannot connect to printers directly");
            conn(agent, devId, ip, "bblp", code, true);
            auto c0 = std::chrono::steady_clock::now();
            while (localState.load() == -1 && std::chrono::steady_clock::now() - c0 < std::chrono::seconds(20)) mq.pump(200);
            std::string lm;
            { std::lock_guard lk(localM); lm = localMsg; }
            note("printer connection: " + std::to_string(localState.load()) + (lm.empty() ? "" : " (" + lm + ")"));
            if (localState.load() != 0) {
                if (lm == "5") return fail("The printer turned down the access code. Check it on the printer (Settings > Network) and in Preferences > Printer.");
                return fail(localState.load() == -1 ? "The printer did not answer at " + ip + " (is it on, and on the same network?)"
                                                    : "Could not connect to the printer at " + ip);
            }
        }
        std::string printFile, configFile, bedType;
        std::string dir = platform::tempDir() + "/bambu/job";
        if (!prepareFiles(req["file"].asString(), modelOf, dir, printFile, configFile, bedType, err)) return fail(err);
        std::string name = req["name"].asString("Kestral print");
        bool useAms = req["use_ams"].asBool();
        int slot = (int)req["ams_slot"].asNumber(0);
        std::string amsMap = useAms ? "[" + std::to_string(slot) + "]" : std::string();
        std::string amsMap2 = useAms ? "[{\"ams_id\":0,\"slot_id\":" + std::to_string(slot) + "}]" : std::string();
        if (useAms && req["ams_map"].isArray() && req["ams_map"].size() > 0) {
            // One AMS tray per filament of the print (Orca's filament mapping).
            amsMap = "[";
            amsMap2 = "[";
            for (size_t i = 0; i < req["ams_map"].size(); ++i) {
                int m = (int)req["ams_map"][i].asNumber(-1);
                if (i) { amsMap += ","; amsMap2 += ","; }
                amsMap += std::to_string(m);
                if (m == 254) amsMap2 += "{\"ams_id\":255,\"slot_id\":0}";
                else if (m < 0) amsMap2 += "{\"ams_id\":255,\"slot_id\":255}";
                else amsMap2 += "{\"ams_id\":" + std::to_string(m / 4) + ",\"slot_id\":" + std::to_string(m % 4) + "}";
            }
            amsMap += "]";
            amsMap2 += "]";
        }
        std::atomic<int> result{-1};
        std::atomic<bool> done{false};
        std::string lastStage;
        std::mutex stageM;
        auto update = [&](int stage, int code, std::string info) {
            std::lock_guard lk(stageM);
            lastStage = "stage " + std::to_string(stage) + " code " + std::to_string(code) + (info.empty() ? "" : " " + info);
        };
        auto cancelFn = [] { return false; };
        auto waitFn = [](int, std::string) { return true; };
        std::thread worker([&] {
            try {
                if (legacy) {
                    auto f = p.fn<PlugPrintV1>("bambu_network_start_print");
                    if (!f) { result = -1; done = true; return; }
                    JobV1 pp{};
                    pp.printerId = devId; pp.taskName = name; pp.projectName = name; pp.presetName = name + "_plate_1";
                    pp.file = printFile; pp.configFile = configFile; pp.plate = 1;
                    pp.ftpFolder = "sdcard/"; pp.amsMap = amsMap; pp.connection = lan ? "lan" : "cloud";
                    pp.printerIp = ip; pp.accessCode = code;
                    pp.printType = "from_normal"; pp.user = "bblp"; pp.ftpTls = true; pp.mqttTls = true;
                    pp.bedLevel = req["bed_leveling"].asBool(true); pp.flowCal = req["flow_cali"].asBool(false);
                    pp.vibrationCal = false; pp.layerCheck = false; pp.timelapse = req["timelapse"].asBool(false);
                    pp.useAms = useAms; pp.bedType = bedType.empty() ? "auto" : bedType;
                    if (lan) {
                        auto lf = p.fn<PlugLocalPrintV1>("bambu_network_start_local_print");
                        result = lf ? lf(agent, pp, update, cancelFn) : -1;
                    } else {
                        result = f(agent, pp, update, cancelFn, waitFn);
                    }
                } else {
                    auto f = p.fn<PlugPrint>("bambu_network_start_print");
                    if (!f) { result = -1; done = true; return; }
                    JobV2 pp{};
                    pp.printerId = devId; pp.taskName = name; pp.projectName = name; pp.presetName = name + "_plate_1";
                    pp.file = printFile; pp.configFile = configFile; pp.plate = 1;
                    pp.ftpFolder = "sdcard/"; pp.amsMap = amsMap; pp.amsMap2 = amsMap2; pp.connection = lan ? "lan" : "cloud";
                    pp.printerIp = ip; pp.accessCode = code;
                    pp.printType = "from_normal"; pp.user = "bblp"; pp.ftpTls = true; pp.mqttTls = true;
                    pp.bedLevel = req["bed_leveling"].asBool(true); pp.flowCal = req["flow_cali"].asBool(false);
                    pp.vibrationCal = false; pp.layerCheck = false; pp.timelapse = req["timelapse"].asBool(false);
                    pp.useAms = useAms; pp.bedType = bedType.empty() ? "auto" : bedType;
                    pp.autoBedLevel = pp.bedLevel ? 1 : 0;
                    pp.autoFlowCal = pp.flowCal ? 1 : 0;
                    pp.extChangeAssist = false;
                    pp.emmcPrint = false;
                    if (lan) {
                        auto lf = p.fn<PlugLocalPrint>("bambu_network_start_local_print");
                        result = lf ? lf(agent, pp, update, cancelFn) : -1;
                    } else {
                        result = f(agent, pp, update, cancelFn, waitFn);
                    }
                }
            } catch (...) {
                result = -100;
            }
            done = true;
        });
        auto s0 = std::chrono::steady_clock::now();
        while (!done && std::chrono::steady_clock::now() - s0 < std::chrono::minutes(5)) mq.pump(200);
        if (!done) {
            worker.detach();
            { std::lock_guard lk(stageM); note("last: " + lastStage); }
            return fail("Bambu's cloud did not finish sending within 5 minutes");
        }
        worker.join();
        { std::lock_guard lk(stageM); if (!lastStage.empty()) note("last: " + lastStage); }
        int r = result;
        out.set("code", Value(r));
        out.set("ok", Value(r == 0));
        out.set("message", Value(r == 0 ? std::string(lan ? "Sent straight to the printer over the home network" : "Sent to the printer through Bambu's cloud")
                                        : errorText(r)));
        if (lan) if (auto f = p.fn<PlugCall>("bambu_network_disconnect_printer")) f(agent);
        if (auto f = p.fn<PlugFreeAgent>("bambu_network_destroy_agent")) f(agent);
        return finish();
    }
    if (cmd == "control") {
        std::string devId = req["dev_id"].asString(), action = req["action"].asString();
        std::string ip = req["ip"].asString(), code = req["access_code"].asString();
        if (action != "stop" && action != "pause" && action != "resume") return fail("Unknown printer command " + action);
        std::string msg = "{\"print\":{\"sequence_id\":\"0\",\"command\":\"" + action + "\",\"param\":\"\"}}";
        int r = -1;
        bool lan = !ip.empty() && !code.empty();
        if (lan) {
            // Straight to the printer, like OrcaSlicer's Device tab on the home network.
            auto conn = p.fn<PlugConnect>("bambu_network_connect_printer");
            if (!conn) return fail("This Bambu plugin cannot connect to printers directly");
            conn(agent, devId, ip, "bblp", code, true);
            auto c0 = std::chrono::steady_clock::now();
            while (localState.load() == -1 && std::chrono::steady_clock::now() - c0 < std::chrono::seconds(20)) mq.pump(200);
            if (localState.load() != 0) return fail(localState.load() == -1 ? "The printer did not answer at " + ip : "Could not connect to the printer");
            if (legacy) { if (auto f = p.fn<PlugSendV1>("bambu_network_send_message_to_printer")) r = f(agent, devId, msg, 1); }
            else if (auto f = p.fn<PlugSend>("bambu_network_send_message_to_printer")) r = f(agent, devId, msg, 1, 1);
        } else if (login) {
            if (legacy) { if (auto f = p.fn<PlugSendV1>("bambu_network_send_message")) r = f(agent, devId, msg, 1); }
            else if (auto f = p.fn<PlugSend>("bambu_network_send_message")) r = f(agent, devId, msg, 1, 1);
        } else {
            return fail("Kestral needs the printer's IP and access code (or a Bambu login) to control it.");
        }
        mq.pump(1500);   // let it go out before the helper ends
        if (lan) if (auto f = p.fn<PlugCall>("bambu_network_disconnect_printer")) f(agent);
        out.set("code", Value(r));
        out.set("ok", Value(r == 0));
        out.set("message", Value(r == 0 ? std::string("Sent ") + action + " to the printer" : "The printer did not take the command (" + std::to_string(r) + ")"));
        if (auto f = p.fn<PlugFreeAgent>("bambu_network_destroy_agent")) f(agent);
        return finish();
    }
    return fail("Unknown helper command " + cmd);
}

} // namespace kst::print
