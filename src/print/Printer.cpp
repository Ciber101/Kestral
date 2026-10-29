#include "print/Printer.h"
#include "print/BambuCloud.h"
#include "print/Orca.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Net.h"
#include "platform/Platform.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace kst::print {

using json::Value;

const char* kindName(PrinterKind k) {
    switch (k) {
    case PrinterKind::BambuOrca: return "Bambu Lab, normal mode (optional: Bambu's network plugin from OrcaSlicer)";
    case PrinterKind::BambuConnect: return "Bambu Lab, through Bambu Connect (you press Print there)";
    case PrinterKind::BambuLan: return "Bambu Lab, LAN only + Developer mode (recommended)";
    case PrinterKind::OctoPrint: return "OctoPrint";
    case PrinterKind::Moonraker: return "Klipper (Moonraker / Mainsail / Fluidd)";
    case PrinterKind::PrusaLink: return "Prusa (PrusaLink)";
    default: return "Not set up";
    }
}

std::string kindId(PrinterKind k) {
    switch (k) {
    case PrinterKind::BambuOrca: return "bambu_orca";
    case PrinterKind::BambuConnect: return "bambu_connect";
    case PrinterKind::BambuLan: return "bambu_lan";
    case PrinterKind::OctoPrint: return "octoprint";
    case PrinterKind::Moonraker: return "moonraker";
    case PrinterKind::PrusaLink: return "prusalink";
    default: return "none";
    }
}

PrinterKind kindFromString(const std::string& s0) {
    std::string s = toLower(s0);
    if (s == "bambu_orca" || s == "orca") return PrinterKind::BambuOrca;
    if (s == "bambu_connect") return PrinterKind::BambuConnect;
    if (s == "bambu_lan" || s == "bambu") return PrinterKind::BambuLan;
    if (s == "octoprint") return PrinterKind::OctoPrint;
    if (s == "moonraker" || s == "klipper" || s == "mainsail" || s == "fluidd") return PrinterKind::Moonraker;
    if (s == "prusalink" || s == "prusa") return PrinterKind::PrusaLink;
    return PrinterKind::None;
}

namespace {

std::string readBinary(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- MQTT 3.1.1 (just what Bambu needs)

std::string mqttLen(size_t n) {
    std::string o;
    do {
        uint8_t b = n % 128;
        n /= 128;
        if (n) b |= 0x80;
        o += (char)b;
    } while (n);
    return o;
}
std::string mqttStr(const std::string& s) {
    std::string o;
    o += (char)(s.size() >> 8);
    o += (char)(s.size() & 0xFF);
    return o + s;
}

struct Mqtt {
    net::TlsConn conn;
    std::string buf;
    int lastFlags = 0;

    bool connect(const PrinterConn& c, std::string& err) {
        if (!conn.connect(c.host, 8883, 6000, err)) return false;
        std::string var = mqttStr("MQTT") + (char)4 + (char)0xC2 + (char)0 + (char)60;
        std::string payload = mqttStr("kestral_" + std::to_string((unsigned)std::chrono::steady_clock::now().time_since_epoch().count() % 1000000u)) +
                              mqttStr("bblp") + mqttStr(c.secret);
        std::string body = var + payload;
        if (!conn.writeAll(std::string(1, (char)0x10) + mqttLen(body.size()) + body)) { err = "Printer closed the connection"; return false; }
        int type = 0;
        std::string pkt;
        if (!readPacket(type, pkt, 6000) || type != 2 || pkt.size() < 2) { err = "The printer did not answer (check the IP address)"; return false; }
        if (pkt[1] != 0) {
            err = (pkt[1] == 4 || pkt[1] == 5) ? "The printer refused the access code (check it on the printer's screen)"
                                                : "The printer refused the connection (code " + std::to_string((int)pkt[1]) + ")";
            return false;
        }
        return true;
    }
    bool subscribe(const std::string& topic) {
        std::string body = std::string("\x00\x01", 2) + mqttStr(topic) + (char)0;
        return conn.writeAll(std::string(1, (char)0x82) + mqttLen(body.size()) + body);
    }
    bool publish(const std::string& topic, const std::string& payload) {
        std::string body = mqttStr(topic) + payload;
        return conn.writeAll(std::string(1, (char)0x30) + mqttLen(body.size()) + body);
    }
    // One whole packet (type in the high nibble), within the time limit.
    bool readPacket(int& type, std::string& body, int timeoutMs) {
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (buf.size() >= 2) {
                size_t len = 0, mul = 1, i = 1;
                bool complete = false;
                for (; i < buf.size() && i < 5; ++i) {
                    len += (size_t)((uint8_t)buf[i] & 0x7F) * mul;
                    mul *= 128;
                    if (!((uint8_t)buf[i] & 0x80)) { complete = true; ++i; break; }
                }
                if (complete && buf.size() >= i + len) {
                    type = (uint8_t)buf[0] >> 4;
                    lastFlags = (uint8_t)buf[0] & 0x0F;
                    body = buf.substr(i, len);
                    buf.erase(0, i + len);
                    return true;
                }
            }
            int left = timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) return false;
            char tmp[16384];
            int n = conn.read(tmp, sizeof tmp, std::min(left, 500));
            if (n < 0) return false;
            if (n > 0) buf.append(tmp, (size_t)n);
        }
    }
    // Next PUBLISH payload (printer reports), JSON parsed.
    bool nextReport(Value& out, int timeoutMs) {
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            int left = timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) return false;
            int type = 0;
            std::string body;
            if (!readPacket(type, body, left)) return false;
            if (type != 3 || body.size() < 2) continue;
            size_t tl = ((uint8_t)body[0] << 8) | (uint8_t)body[1];
            size_t off = 2 + tl;
            if ((lastFlags >> 1) & 3) off += 2;   // QoS 1/2 messages carry a packet id
            if (off > body.size()) continue;
            out = Value::parse(body.substr(off));
            if (out.isObject()) return true;
        }
    }
};

// ---------------------------------------------------------------- FTPS (implicit TLS, port 990)

struct Ftps {
    net::TlsConn ctrl;
    std::string lastReply;

    int reply(int timeoutMs = 8000) {
        std::string line = ctrl.readLine(timeoutMs);
        if (line.size() < 4) return 0;
        lastReply = line;
        int code = std::atoi(line.substr(0, 3).c_str());
        // Multi line replies ("230-...") end with "230 ...".
        if (line[3] == '-') {
            std::string end = line.substr(0, 3) + " ";
            for (int i = 0; i < 200; ++i) {
                std::string l = ctrl.readLine(timeoutMs);
                if (l.empty()) break;
                lastReply = l;
                if (l.rfind(end, 0) == 0) break;
            }
        }
        return code;
    }
    int cmd(const std::string& c, int timeoutMs = 8000) {
        if (!ctrl.writeAll(c + "\r\n")) return 0;
        return reply(timeoutMs);
    }
    bool upload(const PrinterConn& c, const std::string& remoteName, const std::string& data, std::string& err,
                const std::atomic<bool>* cancel) {
        if (!ctrl.connect(c.host, 990, 8000, err)) return false;
        if (reply() != 220) { err = "The printer's file server did not answer"; return false; }
        if (cmd("USER bblp") != 331 || cmd("PASS " + c.secret) != 230) { err = "The printer refused the access code"; return false; }
        cmd("PBSZ 0");
        if (cmd("PROT P") != 200) { err = "The printer's file server refused a secure transfer"; return false; }
        cmd("TYPE I");
        if (cmd("PASV") != 227) { err = "The printer's file server did not open a transfer"; return false; }
        int h[6] = {};
        size_t lp = lastReply.find('(');
        if (lp == std::string::npos || std::sscanf(lastReply.c_str() + lp + 1, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6) {
            err = "Unexpected answer from the printer: " + lastReply;
            return false;
        }
        int port = h[4] * 256 + h[5];
        if (!ctrl.writeAll("STOR " + remoteName + "\r\n")) { err = "Lost the printer"; return false; }
        net::TlsConn dataConn;
        // The data connection must resume the control connection's TLS session (vsftpd rule).
        if (!dataConn.connect(c.host, port, 8000, err, &ctrl)) return false;
        int code = reply();
        if (code != 150 && code != 125) { err = "The printer did not accept the file: " + lastReply; return false; }
        size_t off = 0;
        const size_t chunk = 256 * 1024;
        while (off < data.size()) {
            if (cancel && cancel->load()) { err = "cancelled"; return false; }
            if (!dataConn.writeAll(data.substr(off, chunk))) { err = "The upload to the printer broke off"; return false; }
            off += chunk;
        }
        dataConn.close();
        code = reply(30000);
        if (code != 226 && code != 250) { err = "The printer did not confirm the upload: " + lastReply; return false; }
        cmd("QUIT", 2000);
        return true;
    }
};

std::string bambuStateName(const std::string& s) {
    std::string l = toLower(s);
    if (l == "running") return "printing";
    if (l == "prepare") return "preparing";
    if (l == "pause") return "paused";
    if (l == "finish") return "finished";
    if (l == "failed") return "failed";
    if (l == "idle") return "idle";
    return l.empty() ? "unknown" : l;
}

PrintStatus bambuStatus(const PrinterConn& c, int timeoutMs) {
    PrintStatus st;
    if (c.host.empty() || c.serial.empty() || c.secret.empty()) {
        st.error = "Kestral needs the printer's IP address, serial number and access code to read its status.";
        return st;
    }
    Mqtt m;
    if (!m.connect(c, st.error)) return st;
    m.subscribe("device/" + c.serial + "/report");
    m.publish("device/" + c.serial + "/request", R"({"pushing":{"sequence_id":"0","command":"pushall"}})");
    auto t0 = std::chrono::steady_clock::now();
    Value r;
    while (m.nextReport(r, std::max(500, timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()))) {
        const Value& p = r["print"];
        if (!p.isObject() || !p.has("gcode_state")) continue;
        st.ok = true;
        st.state = bambuStateName(p["gcode_state"].asString());
        st.percent = (int)p["mc_percent"].asNumber(-1);
        st.minutesLeft = (int)p["mc_remaining_time"].asNumber(-1);
        st.nozzleC = p["nozzle_temper"].asNumber(-1);
        st.bedC = p["bed_temper"].asNumber(-1);
        st.job = p["subtask_name"].asString();
        auto num = [](const Value& v, double d) { return v.isNumber() ? v.asNumber() : v.isString() && !v.asString().empty() ? std::atof(v.asString().c_str()) : d; };
        st.nozzleTarget = num(p["nozzle_target_temper"], -1);
        st.bedTarget = num(p["bed_target_temper"], -1);
        st.chamberC = num(p["chamber_temper"], -1);
        auto fan = [&](const char* k) { double v = num(p[k], -1); return v < 0 ? -1 : (int)std::lround(v / 15.0 * 100.0 / 10.0) * 10; };
        st.partFan = fan("cooling_fan_speed");
        st.auxFan = fan("big_fan1_speed");
        st.chamberFan = fan("big_fan2_speed");
        for (auto& l : p["lights_report"].items())
            if (l["node"].asString() == "chamber_light") { st.lightKnown = true; st.light = l["mode"].asString() != "off"; }
        st.layer = (int)num(p["layer_num"], -1);
        st.totalLayers = (int)num(p["total_layer_num"], -1);
        st.speedLevel = (int)num(p["spd_lvl"], -1);
        st.wifi = p["wifi_signal"].asString();
        st.nozzleType = p["nozzle_type"].asString();
        st.nozzleDia = num(p["nozzle_diameter"], 0);
        st.sdcard = p["sdcard"].isBool() ? p["sdcard"].asBool() : false;
        st.stage = p["mc_print_stage"].asString();
        for (auto& h : p["hms"].items()) {
            long long attr = (long long)num(h["attr"], 0), code = (long long)num(h["code"], 0);
            char b[64];
            std::snprintf(b, sizeof b, "HMS_%04llX_%04llX_%04llX_%04llX", (attr >> 16) & 0xFFFF, attr & 0xFFFF, (code >> 16) & 0xFFFF, code & 0xFFFF);
            st.hms.push_back(b);
        }
        auto trayOf = [&](const Value& t, int ams, int slot) {
            PrintStatus::AmsTray a;
            a.ams = ams;
            a.slot = slot;
            a.type = t["tray_type"].asString();
            std::string c = t["tray_color"].asString();
            a.loaded = !a.type.empty();
            if (c.size() >= 6) a.color = "#" + c.substr(0, 6);
            a.name = t["tray_sub_brands"].asString();
            a.remain = (int)num(t["remain"], -1);
            return a;
        };
        const Value& ams = p["ams"];
        for (auto& unit : ams["ams"].items()) {
            int id = (int)num(unit["id"], 0);
            ++st.amsUnits;
            int hum = (int)num(unit["humidity"], -1);
            for (auto& t : unit["tray"].items()) {
                auto a = trayOf(t, id, (int)num(t["id"], 0));
                a.humidity = hum;
                st.trays.push_back(a);
            }
        }
        if (ams.has("tray_now")) st.trayNow = (int)num(ams["tray_now"], -1);
        if (p["vt_tray"].isObject()) { st.ext = trayOf(p["vt_tray"], 255, 0); st.hasExt = true; }
        break;
    }
    if (!st.ok && st.error.empty()) st.error = "The printer connected but sent no status.";
    return st;
}

SendResult bambuLanSend(const PrinterConn& c, const std::string& file, const std::string& job, const std::atomic<bool>* cancel) {
    SendResult r;
    if (c.host.empty() || c.serial.empty() || c.secret.empty()) {
        r.message = "The Bambu printer needs its IP address, serial number and access code (Preferences > Printer).";
        return r;
    }
    std::string data = readBinary(file);
    if (data.empty()) { r.message = "The sliced file is missing."; return r; }
    std::string remote = job;
    for (auto& ch : remote) if (std::strchr("<>:\"/\\|?* ", ch)) ch = '_';
    remote += ".gcode.3mf";
    std::string err;
    {
        Ftps f;
        if (!f.upload(c, remote, data, err, cancel)) { r.message = "Upload failed: " + err; return r; }
    }
    Mqtt m;
    if (!m.connect(c, err)) { r.message = "The file is on the printer but starting failed: " + err; return r; }
    m.subscribe("device/" + c.serial + "/report");
    Value cmdv = Value::object();
    Value p = Value::object();
    p.set("sequence_id", Value("20000"));
    p.set("command", Value("project_file"));
    p.set("param", Value("Metadata/plate_1.gcode"));
    p.set("project_id", Value("0"));
    p.set("profile_id", Value("0"));
    p.set("task_id", Value("0"));
    p.set("subtask_id", Value("0"));
    p.set("subtask_name", Value(job));
    p.set("file", Value(remote));
    p.set("url", Value("file:///sdcard/" + remote));
    p.set("md5", Value(""));
    p.set("timelapse", Value(false));
    p.set("bed_type", Value("auto"));
    p.set("bed_leveling", Value(true));
    p.set("bed_levelling", Value(true));
    p.set("flow_cali", Value(false));
    p.set("vibration_cali", Value(true));
    p.set("layer_inspect", Value(false));
    p.set("use_ams", Value(c.useAms));
    Value map = Value::array();
    if (c.useAms && !c.amsMapping.empty()) for (int m : c.amsMapping) map.push(Value((double)m));
    else if (c.useAms) map.push(Value(c.amsSlot));
    p.set("ams_mapping", map);
    cmdv.set("print", p);
    if (!m.publish("device/" + c.serial + "/request", cmdv.dump())) { r.message = "Lost the printer while starting the print."; return r; }
    // Confirmation: the printer answers the command, then reports preparing / printing.
    Value rep;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(25)) {
        if (cancel && cancel->load()) break;
        if (!m.nextReport(rep, 1000)) continue;
        const Value& pr = rep["print"];
        if (pr["command"].asString() == "project_file") {
            std::string result = toLower(pr["result"].asString());
            if (result == "fail" || result == "failed") {
                r.message = "The printer refused the print: " + pr["reason"].asString(pr["err_code"].isNumber() ? std::to_string((long long)pr["err_code"].asNumber()) : "no reason given");
                return r;
            }
        }
        std::string state = toLower(pr["gcode_state"].asString());
        if (state == "prepare" || state == "running" || pr["command"].asString() == "project_file") {
            r.ok = true;
            r.message = "The printer took the job and is " + (state == "running" ? std::string("printing") : std::string("getting ready")) + ".";
            return r;
        }
    }
    r.ok = true;
    r.message = "The file was sent and the print was started. The printer has not reported back yet; check its screen.";
    return r;
}

// ---------------------------------------------------------------- HTTP printers

std::string baseUrl(const PrinterConn& c) {
    std::string h = c.host;
    if (h.rfind("http://", 0) != 0 && h.rfind("https://", 0) != 0) h = "http://" + h;
    while (!h.empty() && h.back() == '/') h.pop_back();
    return h;
}

std::string plateGcode(const std::string& gcode3mf) { return readZipEntry(gcode3mf, "Metadata/plate_1.gcode"); }

std::string multipart(const std::string& boundary, const std::string& fileName, const std::string& data,
                      const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string b;
    for (auto& [k, v] : fields) b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + k + "\"\r\n\r\n" + v + "\r\n";
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + fileName +
         "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n--" + boundary + "--\r\n";
    return b;
}

SendResult httpSend(const PrinterConn& c, const std::string& file, const std::string& job, const std::atomic<bool>* cancel) {
    SendResult r;
    if (c.host.empty()) { r.message = "The printer's address is not set (Preferences > Printer)."; return r; }
    std::string g = plateGcode(file);
    if (g.empty()) { r.message = "Could not read the G-code from the sliced file."; return r; }
    std::string name = job;
    for (auto& ch : name) if (std::strchr("<>:\"/\\|?* ", ch)) ch = '_';
    name += ".gcode";
    const std::string boundary = "----KestralBoundary7d3f";
    std::vector<std::pair<std::string, std::string>> hdr = {{"Content-Type", "multipart/form-data; boundary=" + boundary}};
    platform::HttpResult h;
    if (c.kind == PrinterKind::OctoPrint) {
        hdr.push_back({"X-Api-Key", c.secret});
        h = platform::httpRequest("POST", baseUrl(c) + "/api/files/local", hdr, multipart(boundary, name, g, {{"select", "true"}, {"print", "true"}}), 120000, cancel);
    } else if (c.kind == PrinterKind::Moonraker) {
        if (!c.secret.empty()) hdr.push_back({"X-Api-Key", c.secret});
        h = platform::httpRequest("POST", baseUrl(c) + "/server/files/upload", hdr, multipart(boundary, name, g, {{"print", "true"}}), 120000, cancel);
    } else {   // PrusaLink
        std::vector<std::pair<std::string, std::string>> ph = {{"Content-Type", "application/octet-stream"}, {"Print-After-Upload", "?1"}, {"Overwrite", "?1"}};
        if (!c.secret.empty()) ph.push_back({"X-Api-Key", c.secret});
        h = platform::httpRequest("PUT", baseUrl(c) + "/api/v1/files/usb/" + platform::urlEncode(name), ph, g, 120000, cancel);
    }
    if (!h.error.empty()) { r.message = h.error; return r; }
    if (h.status == 401 || h.status == 403) { r.message = "The printer refused the API key."; return r; }
    if (h.status < 200 || h.status >= 300) { r.message = "The printer answered " + std::to_string(h.status) + ": " + h.body.substr(0, 300); return r; }
    r.ok = true;
    r.message = "Sent " + name + " and started the print.";
    return r;
}

PrintStatus httpStatus(const PrinterConn& c, int timeoutMs) {
    PrintStatus st;
    if (c.host.empty()) { st.error = "The printer's address is not set."; return st; }
    std::vector<std::pair<std::string, std::string>> hdr;
    if (!c.secret.empty()) hdr.push_back({"X-Api-Key", c.secret});
    if (c.kind == PrinterKind::OctoPrint) {
        auto j = platform::httpGet(baseUrl(c) + "/api/job", hdr, timeoutMs, nullptr);
        if (!j.error.empty() || j.status != 200) { st.error = j.error.empty() ? "OctoPrint answered " + std::to_string(j.status) : j.error; return st; }
        Value v = Value::parse(j.body);
        st.ok = true;
        st.state = toLower(v["state"].asString());
        st.percent = (int)v["progress"]["completion"].asNumber(-1);
        double left = v["progress"]["printTimeLeft"].asNumber(-1);
        st.minutesLeft = left >= 0 ? (int)(left / 60) : -1;
        st.job = v["job"]["file"]["name"].asString();
        auto t = platform::httpGet(baseUrl(c) + "/api/printer", hdr, timeoutMs, nullptr);
        Value tv = Value::parse(t.body);
        st.nozzleC = tv["temperature"]["tool0"]["actual"].asNumber(-1);
        st.bedC = tv["temperature"]["bed"]["actual"].asNumber(-1);
    } else if (c.kind == PrinterKind::Moonraker) {
        auto j = platform::httpGet(baseUrl(c) + "/printer/objects/query?print_stats&display_status&extruder&heater_bed", hdr, timeoutMs, nullptr);
        if (!j.error.empty() || j.status != 200) { st.error = j.error.empty() ? "Moonraker answered " + std::to_string(j.status) : j.error; return st; }
        const Value& s = Value::parse(j.body)["result"]["status"];
        st.ok = true;
        st.state = s["print_stats"]["state"].asString();
        st.job = s["print_stats"]["filename"].asString();
        double prog = s["display_status"]["progress"].asNumber(-1);
        st.percent = prog >= 0 ? (int)(prog * 100) : -1;
        st.nozzleC = s["extruder"]["temperature"].asNumber(-1);
        st.bedC = s["heater_bed"]["temperature"].asNumber(-1);
    } else {
        auto j = platform::httpGet(baseUrl(c) + "/api/v1/status", hdr, timeoutMs, nullptr);
        if (!j.error.empty() || j.status != 200) { st.error = j.error.empty() ? "PrusaLink answered " + std::to_string(j.status) : j.error; return st; }
        Value v = Value::parse(j.body);
        st.ok = true;
        st.state = toLower(v["printer"]["state"].asString());
        st.nozzleC = v["printer"]["temp_nozzle"].asNumber(-1);
        st.bedC = v["printer"]["temp_bed"].asNumber(-1);
        st.percent = (int)v["job"]["progress"].asNumber(-1);
        double left = v["job"]["time_remaining"].asNumber(-1);
        st.minutesLeft = left >= 0 ? (int)(left / 60) : -1;
    }
    return st;
}

} // namespace

SendResult sendPrint(const PrinterConn& c, const std::string& gcode3mf, const std::string& jobName, const std::atomic<bool>* cancel) {
    SendResult r;
    std::error_code ec;
    if (!std::filesystem::exists(u8path(gcode3mf), ec)) { r.message = "Slice the design first."; return r; }
    switch (c.kind) {
    case PrinterKind::BambuOrca: {
        if (bambuPluginPath().empty()) {
            // No Bambu plugin on this computer: OrcaSlicer's own window sends it.
            r.ok = true;
            r.needsClick = true;
            r.message = "Opened in OrcaSlicer: press \"Print plate\" there to send it to the printer.";
            return r;
        }
        if (c.serial.empty()) {
            r.message = "Kestral needs to know which printer: Preferences > Printer > Find my Bambu printers.";
            return r;
        }
        BambuPrintJob j;
        j.devId = c.serial;
        j.gcode3mf = gcode3mf;
        j.name = jobName;
        j.useAms = c.useAms;
        j.amsSlot = c.amsSlot;
        j.amsMapping = c.amsMapping;
        j.bedLeveling = c.bedLeveling;
        j.flowCali = c.flowCali;
        j.timelapse = c.timelapse;
        j.ip = c.host;
        j.accessCode = c.secret;
        BambuPrintResult br = bambuCloudPrint(j, cancel);
        r.ok = br.ok;
        r.message = br.ok ? "Sent to " + (c.name.empty() ? std::string("the printer") : c.name) + ". " + br.message + "." : br.message;
        return r;
    }
    case PrinterKind::BambuConnect: {
        if (!platform::urlSchemeRegistered("bambu-connect")) {
            r.message = "Bambu Connect is not installed. Get it free from bambulab.com (Software > Bambu Connect), or use OrcaSlicer to send.";
            return r;
        }
        std::string url = "bambu-connect://import-file?path=" + platform::urlEncode(pathToU8(std::filesystem::absolute(u8path(gcode3mf), ec))) +
                          "&name=" + platform::urlEncode(jobName) + "&version=1.0.0";
        platform::openUrl(url);
        r.ok = true;
        r.needsClick = true;
        r.message = "Opened in Bambu Connect: press Print there.";
        return r;
    }
    case PrinterKind::BambuLan: return bambuLanSend(c, gcode3mf, jobName, cancel);
    case PrinterKind::OctoPrint:
    case PrinterKind::Moonraker:
    case PrinterKind::PrusaLink: return httpSend(c, gcode3mf, jobName, cancel);
    default:
        r.message = "No printer is set up yet (Preferences > Printer, or tell the assistant your printer).";
        return r;
    }
}

PrintStatus printerStatus(const PrinterConn& c, int timeoutMs) {
    switch (c.kind) {
    case PrinterKind::BambuOrca:
    case PrinterKind::BambuConnect:
    case PrinterKind::BambuLan: return bambuStatus(c, timeoutMs);
    case PrinterKind::OctoPrint:
    case PrinterKind::Moonraker:
    case PrinterKind::PrusaLink: return httpStatus(c, timeoutMs);
    default: {
        PrintStatus s;
        s.error = "No printer is set up yet.";
        return s;
    }
    }
}

SendResult printerControl(const PrinterConn& c, const std::string& action, const std::atomic<bool>* cancel) {
    SendResult r;
    if (action != "pause" && action != "resume" && action != "stop") { r.message = "Unknown printer command " + action; return r; }
    std::string done = action == "stop" ? "Stopped the print." : action == "pause" ? "Paused the print." : "Resumed the print.";
    auto mqttCommand = [&]() {
        if (c.host.empty() || c.serial.empty() || c.secret.empty()) { r.message = "Kestral needs the printer's IP, serial and access code."; return; }
        Mqtt m;
        if (!m.connect(c, r.message)) return;
        m.publish("device/" + c.serial + "/request", "{\"print\":{\"sequence_id\":\"0\",\"command\":\"" + action + "\",\"param\":\"\"}}");
        r.ok = true;
        r.message = done;
    };
    std::vector<std::pair<std::string, std::string>> hdr;
    if (!c.secret.empty()) hdr.push_back({"X-Api-Key", c.secret});
    hdr.push_back({"Content-Type", "application/json"});
    auto http = [&](const std::string& method, const std::string& path, const std::string& body) {
        auto h = platform::httpRequest(method, baseUrl(c) + path, hdr, body, 15000, cancel);
        if (!h.error.empty() || h.status >= 300) { r.message = h.error.empty() ? "The printer answered " + std::to_string(h.status) : h.error; return; }
        r.ok = true;
        r.message = done;
    };
    switch (c.kind) {
    case PrinterKind::BambuOrca: {
        if (bambuPluginPath().empty()) { mqttCommand(); break; }
        BambuPrintJob j;
        j.devId = c.serial;
        j.ip = c.host;
        j.accessCode = c.secret;
        BambuPrintResult br = bambuCloudControl(j, action, cancel);
        if (!br.ok) { mqttCommand(); if (!r.ok) r.message = br.message; break; }   // the plain way as a second try
        r.ok = true;
        r.message = done;
        break;
    }
    case PrinterKind::BambuLan: mqttCommand(); break;
    case PrinterKind::OctoPrint:
        http("POST", "/api/job", action == "stop" ? R"({"command":"cancel"})"
                                                   : std::string(R"({"command":"pause","action":")") + action + "\"}");
        break;
    case PrinterKind::Moonraker: http("POST", "/printer/print/" + std::string(action == "stop" ? "cancel" : action), ""); break;
    case PrinterKind::PrusaLink: {
        auto s = platform::httpGet(baseUrl(c) + "/api/v1/status", hdr, 10000, cancel);
        long long id = (long long)Value::parse(s.body)["job"]["id"].asNumber(-1);
        if (id < 0) { r.message = "No job is running on the printer."; break; }
        std::string base = "/api/v1/job/" + std::to_string(id);
        if (action == "stop") http("DELETE", base, "");
        else http("PUT", base + "/" + action, "");
        break;
    }
    case PrinterKind::BambuConnect: r.message = "Bambu Connect has no remote control: use the printer's screen or Bambu Handy."; break;
    default: r.message = "No printer is set up yet."; break;
    }
    return r;
}

SendResult printerMqtt(const PrinterConn& c, const std::string& payload) {
    SendResult r;
    bool bambu = c.kind == PrinterKind::BambuOrca || c.kind == PrinterKind::BambuLan || c.kind == PrinterKind::BambuConnect;
    if (!bambu) { r.message = "Only Bambu Lab printers take these commands."; return r; }
    if (c.host.empty() || c.serial.empty() || c.secret.empty()) { r.message = "Kestral needs the printer's IP, serial and access code (Preferences > Printer)."; return r; }
    Mqtt m;
    if (!m.connect(c, r.message)) return r;
    if (!m.publish("device/" + c.serial + "/request", payload)) { r.message = "Lost the printer."; return r; }
    r.ok = true;
    r.message = "Sent.";
    return r;
}

SendResult printerGcode(const PrinterConn& c, const std::string& gcode) {
    Value p = Value::object();
    p.set("sequence_id", Value("0"));
    p.set("command", Value("gcode_line"));
    p.set("param", Value(gcode + "\n"));
    Value root = Value::object();
    root.set("print", p);
    if (c.kind == PrinterKind::OctoPrint || c.kind == PrinterKind::Moonraker) {
        SendResult r;
        std::vector<std::pair<std::string, std::string>> hdr = {{"Content-Type", "application/json"}};
        if (!c.secret.empty()) hdr.push_back({"X-Api-Key", c.secret});
        platform::HttpResult h;
        if (c.kind == PrinterKind::OctoPrint) {
            Value b = Value::object();
            Value cmds = Value::array();
            size_t st = 0;
            while (st < gcode.size()) {
                size_t e = gcode.find('\n', st);
                cmds.push(Value(gcode.substr(st, e == std::string::npos ? std::string::npos : e - st)));
                if (e == std::string::npos) break;
                st = e + 1;
            }
            b.set("commands", cmds);
            h = platform::httpRequest("POST", baseUrl(c) + "/api/printer/command", hdr, b.dump(), 15000, nullptr);
        } else {
            h = platform::httpRequest("POST", baseUrl(c) + "/printer/gcode/script?script=" + platform::urlEncode(gcode), hdr, "", 15000, nullptr);
        }
        r.ok = h.error.empty() && h.status < 300;
        r.message = r.ok ? "Sent." : (h.error.empty() ? "The printer answered " + std::to_string(h.status) : h.error);
        return r;
    }
    return printerMqtt(c, root.dump());
}

PrinterCamera::~PrinterCamera() { stop(); }

void PrinterCamera::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
}

std::string PrinterCamera::status() {
    std::lock_guard lk(m_);
    return status_;
}

bool PrinterCamera::latest(std::string& jpeg, uint64_t& seq) {
    std::lock_guard lk(m_);
    if (seq_ == seq || jpeg_.empty()) return false;
    jpeg = jpeg_;
    seq = seq_;
    return true;
}

void PrinterCamera::start(const PrinterConn& c) {
    stop();
    run_ = true;
    th_ = std::thread([this, c] {
        auto setStatus = [&](const std::string& s) { std::lock_guard lk(m_); status_ = s; };
        while (run_) {
            if (c.host.empty() || c.secret.empty()) { setStatus("The camera needs the printer's IP and access code (Preferences > Printer)."); return; }
            {
                std::lock_guard lk(m_);
                if (status_.empty() && jpeg_.empty()) status_ = "Connecting to the camera...";   // keeps an earlier reason while retrying
            }
            net::TlsConn conn;
            std::string err;
            if (!conn.connect(c.host, 6000, 6000, err)) {
                setStatus("No camera picture: " + err + " (A1, A1 mini and P1 send it on port 6000; X1 printers are not supported yet)");
                for (int i = 0; i < 30 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            // The login: 0x40, 0x3000, then user and access code in 32 bytes each.
            std::string auth(80, '\0');
            auto put32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) auth[at + k] = (char)((v >> (8 * k)) & 255); };
            put32(0, 0x40);
            put32(4, 0x3000);
            std::memcpy(&auth[16], "bblp", 4);
            std::memcpy(&auth[48], c.secret.data(), std::min<size_t>(c.secret.size(), 32));
            conn.writeAll(auth);
            std::string buf;
            auto lastPic = std::chrono::steady_clock::now();
            while (run_) {
                char tmp[65536];
                int n = conn.read(tmp, sizeof tmp, 500);
                if (n < 0) { setStatus("The camera stream ended; trying again..."); break; }
                if (n > 0) buf.append(tmp, (size_t)n);
                while (buf.size() >= 16) {
                    uint32_t len = (uint8_t)buf[0] | ((uint8_t)buf[1] << 8) | ((uint8_t)buf[2] << 16) | ((uint32_t)(uint8_t)buf[3] << 24);
                    if (len > 8u * 1024u * 1024u) { buf.clear(); break; }
                    if (buf.size() < 16 + len) break;
                    std::string pic = buf.substr(16, len);
                    buf.erase(0, 16 + len);
                    if (pic.size() > 4 && (uint8_t)pic[0] == 0xFF && (uint8_t)pic[1] == 0xD8) {
                        std::lock_guard lk(m_);
                        jpeg_ = std::move(pic);
                        ++seq_;
                        status_.clear();
                        lastPic = std::chrono::steady_clock::now();
                    }
                }
                if (std::chrono::steady_clock::now() - lastPic > std::chrono::seconds(12)) { setStatus("The printer accepts the camera connection but sends no pictures. In LAN only mode, turn on \"LAN only liveview\" on the printer (its settings menu), and check that the camera is not switched off there."); break; }
            }
            conn.close();
            for (int i = 0; i < 10 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

std::string testConnection(const PrinterConn& c) {
    if (c.kind == PrinterKind::None) return "No printer is set up.";
    if (c.kind == PrinterKind::BambuOrca && !bambuPluginPath().empty()) {
        BambuCloudStatus st = bambuCloudStatus(nullptr);
        if (!st.loggedIn) {
            if (!c.host.empty() && !c.secret.empty())
                return "Ready: not signed in to Bambu, so prints go straight to the printer at " + c.host + " over the home network (like OrcaSlicer).";
            return st.error.empty() ? "Not signed in to Bambu." : st.error;
        }
        for (auto& d : st.devices)
            if (d.id == c.serial)
                return "Ready: signed in to Bambu as " + st.user + ". " + d.name + " is " + (d.online ? "online." : "offline right now.");
        return "Signed in to Bambu as " + st.user + ", but this printer is not on the account. Use Find my Bambu printers.";
    }
    if ((c.kind == PrinterKind::BambuOrca || c.kind == PrinterKind::BambuConnect) && (c.host.empty() || c.secret.empty()))
        return "Ready: prints go through " + std::string(c.kind == PrinterKind::BambuOrca ? "OrcaSlicer" : "Bambu Connect") +
               ". (Add the printer's IP, serial and access code to also see its status.)";
    PrintStatus s = printerStatus(c, 8000);
    if (!s.ok) return "Not reachable: " + s.error;
    std::string out = "Connected. The printer is " + s.state;
    if (s.percent >= 0 && s.state == "printing") out += " (" + std::to_string(s.percent) + "%)";
    return out + ".";
}

} // namespace kst::print
