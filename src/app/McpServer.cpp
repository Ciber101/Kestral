// Kestral's MCP server (Model Context Protocol, Streamable HTTP, protocol 2025-06-18 and older)
// and the stdio bridge (Kestral --mcp). See Mcp.h.
//
// Every tool call runs exactly like a step of Kestral's own assistant: on the main thread, as one
// undoable step in the timeline, shown in the assistant chat, and it waits for the design to
// rebuild before answering. Prints still need the user's Print button unless they allowed
// automatic prints.
#include "app/App.h"
#include "app/Mcp.h"
#include "core/Json.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "platform/Sock.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace kst {

using json::Value;

namespace {

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const char* kProtocols[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

const char* kInstructions =
    "You are driving Kestral, a parametric CAD program (Fusion 360 style) that is open on the user's screen. "
    "The user watches every step happen live, and every tool call is one undoable step in their timeline.\n"
    "- Units are millimetres. Z is up; the XY plane is the ground.\n"
    "- Call get_design before changing anything, and refer to bodies by the paths it gives. Big designs come as an "
    "overview of components: pass under (a component path) to list the bodies in one.\n"
    "- Build with sketches (create_sketch, sketch_add, sketch_constrain, then extrude, revolve, sweep or loft) or with "
    "primitives (create_box, create_cylinder and the others), then fillet, chamfer, create_hole, shell, patterns, "
    "mirror and combine. Generators make gears, threads, coils, pipes, wings, blades, dovetails and more.\n"
    "- Each result says which bodies the step made and any timeline step that failed. Fix failures before going on.\n"
    "- Check your work with look_at_design: it returns a picture of the design from the views you ask for.\n"
    "- export_illustration saves a clean picture (exploded, numbered parts) as PNG or SVG.\n"
    "- To print: slice (print_setup lists printers, filaments and profiles), then start_print. Kestral shows the user a "
    "Print button and only prints when they press it, unless they allowed automatic prints.\n"
    "- save_design saves the design. Do not overwrite or delete the user's work unless they asked.\n"
    "- You can also use the whole program like a person: screenshot shows the window, ui_input clicks, drags, scrolls and types "
    "anywhere (menus, dialogs, the browser, the timeline, the Slice tab, Preferences), get_ui_state says what is open, "
    "list_commands and run_command reach every command, and view, undo, redo, design_tabs, workspace, get_settings, set_setting, "
    "assistant, get_log and window cover the rest. Prefer the modeling tools when one does the job; use ui_input for everything else.";

// ---------------------------------------------------------------- HTTP

struct Req {
    std::string method, path;
    std::map<std::string, std::string> h;   // lower case names
    std::string body;
    bool keepAlive = true;
};

// Reads one request. buf keeps what came after it (keep-alive). False when the client is gone.
bool readReq(sock::Sock c, std::string& buf, Req& r, const std::atomic<bool>& run) {
    char tmp[65536];
    double idleSince = nowSec();
    size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        if (!run.load() || buf.size() > 65536 || nowSec() - idleSince > 300) return false;
        int n = sock::recvSome(c, tmp, sizeof tmp, 500);
        if (n < 0) return false;
        if (n > 0) { buf.append(tmp, (size_t)n); idleSince = nowSec(); }
    }
    std::string head = buf.substr(0, he);
    buf.erase(0, he + 4);
    std::istringstream hs(head);
    std::string line;
    std::getline(hs, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream ls(line);
        std::string ver;
        ls >> r.method >> r.path >> ver;
        if (ver == "HTTP/1.0") r.keepAlive = false;
    }
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c2 = line.find(':');
        if (c2 == std::string::npos) continue;
        std::string k = lowerStr(line.substr(0, c2)), v = line.substr(c2 + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        r.h[k] = v;
    }
    if (lowerStr(r.h["connection"]) == "close") r.keepAlive = false;
    size_t len = 0;
    if (r.h.count("content-length")) len = (size_t)std::strtoull(r.h["content-length"].c_str(), nullptr, 10);
    if (len > (size_t)64 << 20) return false;
    while (buf.size() < len) {
        if (!run.load()) return false;
        int n = sock::recvSome(c, tmp, sizeof tmp, 1000);
        if (n < 0) return false;
        if (n > 0) buf.append(tmp, (size_t)n);
    }
    r.body = buf.substr(0, len);
    buf.erase(0, len);
    size_t q = r.path.find('?');
    if (q != std::string::npos) r.path.erase(q);
    return true;
}

const char* reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Error";
    }
}

bool sendResp(sock::Sock c, int code, const std::string& ctype, const std::string& body, const std::string& extra, bool keep) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\n";
    if (!ctype.empty()) h += "Content-Type: " + ctype + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += "Cache-Control: no-store\r\n";
    h += extra;
    h += keep ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    h += "\r\n";
    return sock::sendAll(c, h) && (body.empty() || sock::sendAll(c, body));
}

Value rpcResult(const Value& id, Value result) {
    Value r = Value::object();
    r.set("jsonrpc", "2.0");
    r.set("id", id);
    r.set("result", std::move(result));
    return r;
}

Value rpcError(const Value& id, int code, const std::string& msg) {
    Value r = Value::object();
    r.set("jsonrpc", "2.0");
    r.set("id", id);
    Value e = Value::object();
    e.set("code", code);
    e.set("message", msg);
    r.set("error", e);
    return r;
}

bool hostOk(const std::string& hostHeader) {
    std::string h = lowerStr(hostHeader);
    if (h.empty()) return true;
    for (const char* ok : {"127.0.0.1", "localhost", "[::1]"}) {
        size_t n = std::strlen(ok);
        if (h.compare(0, n, ok) == 0 && (h.size() == n || h[n] == ':')) return true;
    }
    return false;
}

bool originOk(const std::string& origin) {
    if (origin.empty()) return true;   // programs (not web pages) send none
    std::string o = lowerStr(origin);
    for (const char* ok : {"http://127.0.0.1", "http://localhost", "http://[::1]"}) {
        size_t n = std::strlen(ok);
        if (o.compare(0, n, ok) == 0 && (o.size() == n || o[n] == ':' || o[n] == '/')) return true;
    }
    return false;
}

bool tokenEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

} // namespace

// ---------------------------------------------------------------- server state

struct App::McpServer {
    App* app = nullptr;
    std::thread accepter;
    std::atomic<bool> run{false};
    sock::Sock listener = sock::kBad;
    int port = 0;
    std::string token, readToken, session;
    bool other = false;   // another Kestral window serves MCP
    std::atomic<int> conns{0};
    std::shared_ptr<std::atomic<bool>> stopAll = std::make_shared<std::atomic<bool>>(false);
    std::mutex callMx;   // one tool call at a time, from any client

    std::mutex mx;   // guards everything below
    std::string lastClient, runningTool, error;
    double lastUse = -1e9;
    int calls = 0;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> inFlight;   // request id -> cancel
    std::map<std::string, std::string> clients;   // session -> client name
};

std::string mcp::infoPath() { return platform::appDataDir() + "/mcp.json"; }

namespace {

void writeInfo(bool enabled, int port, const std::string& token, const std::string& error, const std::string& readToken = "") {
    Value v = Value::object();
    v.set("enabled", enabled);
    v.set("pid", sock::selfPid());
    v.set("version", kestralVersion());
    if (enabled && port > 0) {
        v.set("port", port);
        v.set("url", "http://127.0.0.1:" + std::to_string(port) + "/mcp");
        v.set("token", token);
        if (!readToken.empty()) v.set("read_token", readToken);
    }
    if (!error.empty()) v.set("error", error);
    std::error_code ec;
    std::filesystem::create_directories(u8path(platform::appDataDir()), ec);
    std::string tmp = mcp::infoPath() + ".tmp";
    {
        std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
        f << v.dump();
    }
    std::filesystem::rename(u8path(tmp), u8path(mcp::infoPath()), ec);
    if (ec) {
        std::ofstream f(u8path(mcp::infoPath()), std::ios::binary | std::ios::trunc);
        f << v.dump();
    }
}

} // namespace

// ---------------------------------------------------------------- tool calls

App::McpCall App::mcpRunTool(const std::string& client, const std::string& name, const std::string& args,
                             const std::shared_ptr<std::atomic<bool>>& cancel) {
    PostFn post = [this](std::function<void()> fn) {
        jobs_->post(std::move(fn));
        glfwPostEmptyEvent();   // the main loop may be idle: wake it
    };
    auto waitFuture = [&](auto& fut) {
        while (!cancel->load())
            if (fut.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) return true;
        return false;
    };
    auto waitIdle = [&] {
        for (int k = 0; k < 6000 && !cancel->load(); ++k) {
            auto p = std::make_shared<std::promise<bool>>();
            auto f = p->get_future();
            post([this, p] { p->set_value(recomputeQueuedFrom_ < 0 && !recomputeRunning_ && !recomputeResults_); });
            if (!waitFuture(f)) return;
            if (f.get()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    const std::string who = client.empty() ? std::string("MCP") : client;
    auto chat = [&](int kind, const std::string& text) {
        post([this, kind, text] {
            ai_.lines.push_back({kind, text});
            ai_.scroll = true;
        });
    };
    McpCall out;
    if (mcpIsUiTool(name)) {
        Value pa = Value::parse(args.empty() ? "{}" : args);
        out = mcpRunUiTool(client, name, pa.isObject() ? pa : Value::object(), cancel);
        if (out.text.rfind("Error", 0) == 0 || out.text.rfind("Unknown", 0) == 0) out.error = true;
        post([this, who, name, t = out.text] { aiLog("mcp " + who + " " + name, t); });
        return out;
    }
    if (!mcpKnownTool(name)) {
        out.text = "Unknown tool: " + name + ". tools/list has every tool Kestral offers.";
        out.error = true;
        return out;
    }
    Value parsed = Value::parse(args.empty() ? "{}" : args);
    std::string a = parsed.isObject() ? parsed.dump() : "{}";

    if (name == "printer_camera") {
        AiImageBuilt built;
        std::string msg;
        if (cameraPicture(post, cancel, built, msg)) {
            out.text = msg;
            out.images = {built.img};
            post([this, built, who]() mutable {
                aiImageTexture(built);
                AiLine ln{6, who + " looked through the printer's camera"};
                ln.images = {built.img};
                ai_.lines.push_back(ln);
                ai_.scroll = true;
            });
        } else {
            out.text = msg;
            out.error = true;
        }
        post([this, who, name, t = out.text] { aiLog("mcp " + who + " " + name, t); });
        return out;
    }
    if (name == "look_at_design") {
        waitIdle();
        AiImageBuilt built;
        std::string head, order, err;
        if (cancel->load()) { out.text = "Stopped."; out.error = true; return out; }
        if (!designPicture(a, post, cancel, built, head, order, err)) {
            out.text = err;
            out.error = err != "Nothing to look at: no visible bodies.";
        } else {
            out.text = head + " Compare it with what you meant to build.";
            out.images = {built.img};
            post([this, built, order, who]() mutable {
                aiImageTexture(built);
                AiLine ln{6, who + " looked at the design: " + order};
                ln.images = {built.img};
                ai_.lines.push_back(ln);
                ai_.scroll = true;
            });
        }
    } else if (name == "export_illustration") {
        waitIdle();
        AiImageBuilt built;
        out.text = illustrationTool(a, post, cancel, &built);
        if (built.img) out.images = {built.img};
        out.error = out.text.rfind("Saved", 0) != 0;
    } else if (aiIsPrintTool(name)) {
        std::string perr;
        if (!platform::guarded("MCP print tool", [&] { out.text = aiPrintTool(name, a, post, cancel); }, perr)) out.text = "Error: " + perr;
        if (name != "print_setup" && name != "list_print_profiles") chat(2, who + ": " + out.text);
    } else {
        // Document tools: on the main thread, then wait for the rebuild and report what it made.
        waitIdle();
        if (cancel->load()) { out.text = "Stopped."; out.error = true; return out; }
        auto prom = std::make_shared<std::promise<std::string>>();
        auto fut = prom->get_future();
        auto before = std::make_shared<std::set<uint32_t>>();
        post([this, prom, name, a, before, who] {
            for (auto& [id, b] : doc_.bodies) if (doc_.isBodyAlive(b)) before->insert(id);
            aiCaptured_.clear();
            aiCapture_ = true;
            std::string res, perr;
            if (!platform::guarded("MCP tool", [&] { res = aiRunTool(name, a); }, perr)) res = "Error: " + perr;
            if (name != "get_design") ai_.lines.push_back({2, who + ": " + res});
            ai_.scroll = true;
            activity();
            prom->set_value(res);
        });
        if (!waitFuture(fut)) { out.text = "Stopped."; out.error = true; return out; }
        out.text = fut.get();
        if (name != "get_design" && name != "select") {
            waitIdle();
            auto p2 = std::make_shared<std::promise<std::string>>();
            auto f2 = p2->get_future();
            post([this, p2, before] {
                std::string made, failed, said;
                for (auto& [id, b] : doc_.bodies)
                    if (doc_.isBodyAlive(b) && !before->count(id)) made += (made.empty() ? "" : ", ") + aiBodyPath(id);
                for (auto& f : doc_.timeline)
                    if (f.failed && !f.error.empty()) failed += (failed.empty() ? "" : "; ") + f.name + ": " + f.error;
                for (auto& m : aiCaptured_) said += (said.empty() ? "" : " | ") + m;
                aiCapture_ = false;
                aiCaptured_.clear();
                std::string o;
                if (!made.empty()) o += " Bodies now in the design from this step: " + made + ".";
                if (!said.empty()) o += " Kestral showed the user: \"" + said + "\".";
                if (!failed.empty()) o += " Timeline steps with errors: " + failed + ".";
                p2->set_value(o);
            });
            if (waitFuture(f2)) out.text += f2.get();
        } else {
            post([this] { aiCapture_ = false; aiCaptured_.clear(); });
        }
    }
    std::string low = lowerStr(out.text.substr(0, 200));
    if (low.rfind("error", 0) == 0 || low.rfind("unknown", 0) == 0 || low.rfind("not found", 0) == 0 || low.rfind("stopped", 0) == 0)
        out.error = true;
    post([this, who, name, t = out.text] { aiLog("mcp " + who + " " + name, t); });
    return out;
}

// ---------------------------------------------------------------- the action log and the read-only key

bool App::mcpReadOnlyTool(const std::string& n) {
    static const char* kRead[] = {"get_design", "get_body", "get_sketch", "look_at_design", "list_designs", "measure", "interference",
                                  "list_print_profiles", "printer_status", "printer_camera", "screenshot", "get_ui_state", "list_commands", "ui_find",
                                  "get_settings", "get_log", "wait_for"};
    for (const char* k : kRead) if (n == k) return true;
    return false;
}

void App::mcpAudit(const std::string& client, const std::string& tool, const std::string& args, const McpCall& r, double ms, bool readOnly) {
    // One JSON line per call in <app data>/mcp_log.jsonl: when, who, what, how it went.
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    std::string path = platform::appDataDir() + "/mcp_log.jsonl";
    std::error_code ec;
    if (std::filesystem::exists(u8path(path), ec) && std::filesystem::file_size(u8path(path), ec) > (20u << 20)) {
        std::filesystem::rename(u8path(path), u8path(path + ".1"), ec);   // keeps one older file
    }
    std::time_t t = std::time(nullptr);
    char when[40];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    Value v = Value::object();
    v.set("time", when);
    v.set("client", client.empty() ? std::string("MCP client") : client);
    v.set("tool", tool);
    v.set("args", args.size() > 2000 ? args.substr(0, 2000) + "..." : args);
    v.set("ok", !r.error);
    v.set("result", r.text.size() > 400 ? r.text.substr(0, 400) + "..." : r.text);
    if (!r.images.empty()) v.set("pictures", (int)r.images.size());
    v.set("ms", (int)ms);
    if (readOnly) v.set("read_only_key", true);
    std::ofstream f(u8path(path), std::ios::binary | std::ios::app);
    f << v.dump() << "\n";
}

// ---------------------------------------------------------------- one connection

void App::mcpServeConnection(std::shared_ptr<McpServer> srv, intptr_t csock) {
    sock::Sock c = (sock::Sock)csock;
    struct Count {
        std::shared_ptr<McpServer> s;
        sock::Sock c;
        ~Count() { sock::close(c); s->conns--; }
    } count{srv, c};
    std::string buf;
    while (srv->run.load()) {
        Req r;
        if (!readReq(c, buf, r, srv->run)) return;
        bool keep = r.keepAlive;
        if (!hostOk(r.h["host"]) || !originOk(r.h["origin"])) {
            sendResp(c, 403, "text/plain", "Kestral only answers programs on this computer.", "", false);
            return;
        }
        if (r.path.rfind("/.well-known/", 0) == 0) { sendResp(c, 404, "text/plain", "Not found", "", keep); if (!keep) return; continue; }
        if (r.path != "/mcp" && r.path != "/mcp/" && r.path != "/") { sendResp(c, 404, "text/plain", "Kestral's MCP server is at /mcp", "", keep); if (!keep) return; continue; }
        std::string auth = r.h["authorization"];
        bool readOnly = !srv->readToken.empty() && tokenEq(auth, "Bearer " + srv->readToken);
        if (!readOnly && !tokenEq(auth, "Bearer " + srv->token)) {
            Value e = rpcError(Value(), -32001, "Kestral needs its MCP key: send the header Authorization: Bearer <key>. The key is in "
                                                "Kestral, Preferences > Assistant > MCP server (or use Kestral --mcp, which finds it).");
            sendResp(c, 401, "application/json", e.dump(), "WWW-Authenticate: Bearer realm=\"kestral\"\r\n", keep);
            if (!keep) return;
            continue;
        }
        if (r.method == "GET") {   // no server to client stream: everything comes back on the POST
            sendResp(c, 405, "text/plain", "Use POST", "Allow: POST, DELETE\r\n", keep);
            if (!keep) return;
            continue;
        }
        if (r.method == "DELETE") {
            {
                std::lock_guard<std::mutex> lk(srv->mx);
                srv->clients.erase(r.h["mcp-session-id"]);
            }
            sendResp(c, 200, "", "", "", keep);
            if (!keep) return;
            continue;
        }
        if (r.method == "OPTIONS") { sendResp(c, 204, "", "", "Allow: POST, DELETE\r\n", keep); if (!keep) return; continue; }
        if (r.method != "POST") { sendResp(c, 405, "text/plain", "Use POST", "Allow: POST, DELETE\r\n", keep); if (!keep) return; continue; }

        std::string perr;
        Value msg = Value::parse(r.body, &perr);
        if (!perr.empty()) {
            sendResp(c, 400, "application/json", rpcError(Value(), -32700, "Parse error: " + perr).dump(), "", keep);
            if (!keep) return;
            continue;
        }
        std::string sess = r.h["mcp-session-id"];
        bool sse = r.h["accept"].find("text/event-stream") != std::string::npos;
        std::string client;
        {
            std::lock_guard<std::mutex> lk(srv->mx);
            auto it = srv->clients.find(sess);
            client = it != srv->clients.end() ? it->second : srv->lastClient;
        }

        // Answers one JSON-RPC request (not tools/call when streaming). Null: a notification.
        std::string newSession;
        auto handle = [&](const Value& m, bool& isCall) -> Value {
            isCall = false;
            if (!m.isObject()) return rpcError(Value(), -32600, "Invalid request");
            if (!m.has("method")) return Value();   // a response or ack from the client
            std::string method = m["method"].asString();
            const Value& params = m["params"];
            if (!m.has("id")) {   // notification
                if (method == "notifications/cancelled") {
                    std::string rid = params["requestId"].dump();
                    std::lock_guard<std::mutex> lk(srv->mx);
                    auto it = srv->inFlight.find(rid);
                    if (it != srv->inFlight.end()) it->second->store(true);
                }
                return Value();
            }
            const Value& id = m["id"];
            if (method == "initialize") {
                std::string want = params["protocolVersion"].asString();
                std::string ver = kProtocols[0];
                for (const char* p : kProtocols) if (want == p) ver = p;
                std::string name = params["clientInfo"]["title"].asString();
                if (name.empty()) name = params["clientInfo"]["name"].asString();
                if (name.empty()) name = "MCP client";
                newSession = sock::randomHex(16);
                {
                    std::lock_guard<std::mutex> lk(srv->mx);
                    srv->clients[newSession] = name;
                    srv->lastClient = name;
                    srv->lastUse = nowSec();
                }
                client = name;
                srv->app->jobs_->post([app = srv->app, name] {
                    app->toast(name + " connected to Kestral (MCP)", ToastKind::Info);
                    log::info("MCP: " + name + " connected");
                });
                glfwPostEmptyEvent();
                Value res = Value::object();
                res.set("protocolVersion", ver);
                Value caps = Value::object();
                Value tools = Value::object();
                tools.set("listChanged", false);
                caps.set("tools", tools);
                caps.set("logging", Value::object());
                res.set("capabilities", caps);
                Value info = Value::object();
                info.set("name", "kestral");
                info.set("title", "Kestral CAD");
                info.set("version", kestralVersion());
                res.set("serverInfo", info);
                res.set("instructions", readOnly ? std::string(kInstructions) + "\nThis connection uses Kestral's read-only key: you can look at "
                                                     "the design and the screen, but not change anything." : std::string(kInstructions));
                return rpcResult(id, res);
            }
            if (method == "ping" || method == "logging/setLevel") return rpcResult(id, Value::object());
            if (method == "tools/list") {
                Value res = Value::object();
                Value all = srv->app->mcpToolList();
                Value ui = srv->app->mcpUiToolList();
                for (auto& t : ui.items()) all.push(t);
                if (readOnly) {   // the read-only key only sees the tools that look
                    Value some = Value::array();
                    for (auto& t : all.items()) if (mcpReadOnlyTool(t["name"].asString())) some.push(t);
                    all = some;
                }
                res.set("tools", all);
                return rpcResult(id, res);
            }
            if (method == "resources/list") { Value res = Value::object(); res.set("resources", Value::array()); return rpcResult(id, res); }
            if (method == "resources/templates/list") { Value res = Value::object(); res.set("resourceTemplates", Value::array()); return rpcResult(id, res); }
            if (method == "prompts/list") { Value res = Value::object(); res.set("prompts", Value::array()); return rpcResult(id, res); }
            if (method == "tools/call") { isCall = true; return Value(); }
            return rpcError(id, -32601, "Method not found: " + method);
        };

        // Runs a tools/call on its own thread so pings, progress and cancels keep flowing.
        auto runCall = [&](const Value& m, auto&& onTick) -> Value {
            const Value& id = m["id"];
            const Value& params = m["params"];
            std::string tool = params["name"].asString();
            std::string args = params["arguments"].isObject() ? params["arguments"].dump() : "{}";
            auto cancel = std::make_shared<std::atomic<bool>>(false);
            std::string rid = id.dump();
            {
                std::lock_guard<std::mutex> lk(srv->mx);
                srv->inFlight[rid] = cancel;
            }
            struct Shared {
                std::mutex m;
                bool done = false;
                McpCall res;
            };
            auto sh = std::make_shared<Shared>();
            std::shared_ptr<std::atomic<bool>> stopAll = srv->stopAll;
            std::thread([srv, sh, tool, args, cancel, client, readOnly] {
                // One call at a time, except waiting: wait_for has to let the calls it waits for run.
                std::unique_lock<std::mutex> one(srv->callMx, std::defer_lock);
                if (tool != "wait_for" && tool != "get_log") one.lock();
                {
                    std::lock_guard<std::mutex> lk(srv->mx);
                    srv->runningTool = tool;
                    srv->lastUse = nowSec();
                    if (!client.empty()) srv->lastClient = client;
                }
                McpCall r;
                double t0 = nowSec();
                if (readOnly && !mcpReadOnlyTool(tool)) {
                    r.text = "Error: this connection uses Kestral's read-only key, so " + tool + " is not allowed. The user can give you the full key.";
                    r.error = true;
                } else if (!cancel->load() && !srv->stopAll->load()) {
                    std::string perr;
                    if (!platform::guarded("MCP call", [&] { r = srv->app->mcpRunTool(client, tool, args, cancel); }, perr)) {
                        r.text = "Error: " + perr;
                        r.error = true;
                    }
                } else {
                    r.text = "Stopped.";
                    r.error = true;
                }
                {
                    std::lock_guard<std::mutex> lk(srv->mx);
                    srv->runningTool.clear();
                    srv->lastUse = nowSec();
                    srv->calls++;
                }
                srv->app->mcpAudit(client, tool, args, r, (nowSec() - t0) * 1000.0, readOnly);
                std::lock_guard<std::mutex> lk(sh->m);
                sh->res = std::move(r);
                sh->done = true;
            }).detach();
            double t0 = nowSec(), lastTick = t0;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lk(sh->m);
                    if (sh->done) break;
                }
                if (stopAll->load()) cancel->store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (nowSec() - lastTick >= 5.0) {
                    lastTick = nowSec();
                    if (!onTick(nowSec() - t0)) cancel->store(true);   // the client hung up
                }
            }
            {
                std::lock_guard<std::mutex> lk(srv->mx);
                srv->inFlight.erase(rid);
            }
            McpCall r;
            {
                std::lock_guard<std::mutex> lk(sh->m);
                r = sh->res;
            }
            Value content = Value::array();
            Value t = Value::object();
            t.set("type", "text");
            t.set("text", r.text);
            content.push(t);
            for (auto& im : r.images) {
                if (!im || im->b64.empty()) continue;
                Value iv = Value::object();
                iv.set("type", "image");
                iv.set("data", im->b64);
                iv.set("mimeType", im->mime.empty() ? std::string("image/png") : im->mime);
                content.push(iv);
            }
            Value res = Value::object();
            res.set("content", content);
            res.set("isError", r.error);
            return rpcResult(id, res);
        };

        if (msg.isArray()) {   // a batch (2025-03-26): answered together as JSON
            Value outArr = Value::array();
            for (auto& m : msg.items()) {
                bool isCall = false;
                Value v = handle(m, isCall);
                if (isCall) v = runCall(m, [](double) { return true; });
                if (!v.isNull()) outArr.push(v);
            }
            std::string extra = newSession.empty() ? "" : "Mcp-Session-Id: " + newSession + "\r\n";
            if (outArr.size() == 0) sendResp(c, 202, "", "", extra, keep);
            else sendResp(c, 200, "application/json", outArr.dump(), extra, keep);
            if (!keep) return;
            continue;
        }
        bool isCall = false;
        Value v = handle(msg, isCall);
        if (!isCall) {
            std::string extra = newSession.empty() ? "" : "Mcp-Session-Id: " + newSession + "\r\n";
            if (v.isNull()) sendResp(c, 202, "", "", extra, keep);
            else sendResp(c, 200, "application/json", v.dump(), extra, keep);
            if (!keep) return;
            continue;
        }
        if (!sse) {
            Value res = runCall(msg, [](double) { return true; });
            sendResp(c, 200, "application/json", res.dump(), "", keep);
            if (!keep) return;
            continue;
        }
        // Streamed answer: progress (when asked for) and keep-alive comments while the tool works,
        // then the result. The stream ends the connection.
        std::string h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        if (!sock::sendAll(c, h)) return;
        Value progressToken = msg["params"]["_meta"]["progressToken"];
        int ticks = 0;
        Value res = runCall(msg, [&](double secs) {
            std::string ev;
            if (!progressToken.isNull()) {
                Value n = Value::object();
                n.set("jsonrpc", "2.0");
                n.set("method", "notifications/progress");
                Value p = Value::object();
                p.set("progressToken", progressToken);
                p.set("progress", ++ticks);
                std::string tool = msg["params"]["name"].asString();
                p.set("message", "Kestral is running " + tool + " (" + std::to_string((int)secs) + " s)");
                n.set("params", p);
                ev = "event: message\ndata: " + n.dump() + "\n\n";
            } else {
                ev = ": working\n\n";
            }
            return sock::sendAll(c, ev);
        });
        sock::sendAll(c, "event: message\ndata: " + res.dump() + "\n\n");
        return;
    }
}

// ---------------------------------------------------------------- start / stop

void App::mcpStart() {
    if (mcp_) return;
    sock::init();
    if (!settings_.mcpEnabled) {
        writeInfo(false, 0, "", "");
        return;
    }
    if (settings_.mcpToken.size() < 32 || settings_.mcpReadToken.size() < 32) {
        if (settings_.mcpToken.size() < 32) settings_.mcpToken = sock::randomHex(24);
        if (settings_.mcpReadToken.size() < 32) settings_.mcpReadToken = sock::randomHex(24);
        settings_.save(settingsPath_);
    }
    auto srv = std::make_shared<McpServer>();
    srv->app = this;
    srv->token = settings_.mcpToken;
    srv->readToken = settings_.mcpReadToken;
    {
        // Another Kestral window opened first already answers MCP apps: leave it to that one.
        std::ifstream f(u8path(mcp::infoPath()), std::ios::binary);
        std::stringstream ss;
        if (f) ss << f.rdbuf();
        Value v = Value::parse(ss.str());
        int pid = (int)v["pid"].asNumber(0), port = (int)v["port"].asNumber(0);
        if (v["enabled"].asBool(false) && pid != sock::selfPid() && sock::processAlive(pid) && port > 0) {
            sock::Sock probe = sock::connectLocal(port, 300);
            if (probe != sock::kBad) {
                sock::close(probe);
                srv->error = "Another Kestral window (the one opened first) is serving MCP apps. Close it to serve them from this one.";
                srv->other = true;
                log::info("MCP: " + srv->error);
                mcp_ = srv;
                return;
            }
        }
    }
    int base = std::clamp(settings_.mcpPort, 1024, 65000);
    for (int p = base; p < base + 10; ++p) {
        srv->listener = sock::listenLocal(p);
        if (srv->listener != sock::kBad) { srv->port = p; break; }
    }
    if (srv->listener == sock::kBad) {
        srv->error = "Ports " + std::to_string(base) + " to " + std::to_string(base + 9) + " are all in use. Pick another port.";
        log::warn("MCP: " + srv->error);
        writeInfo(false, 0, "", srv->error);
        mcp_ = srv;
        return;
    }
    srv->run = true;
    srv->accepter = std::thread([this, srv] {
        while (srv->run.load()) {
            sock::Sock c = sock::acceptWait(srv->listener, 300);
            if (c == sock::kBad) continue;
            if (!srv->run.load()) { sock::close(c); break; }
            srv->conns++;
            std::thread([this, srv, c] {
                std::string perr;
                if (!platform::guarded("MCP connection", [&] { mcpServeConnection(srv, (intptr_t)c); }, perr))
                    log::warn("MCP connection: " + perr);
            }).detach();
        }
    });
    writeInfo(true, srv->port, srv->token, "", srv->readToken);
    log::info("MCP server at http://127.0.0.1:" + std::to_string(srv->port) + "/mcp");
    mcp_ = srv;
}

void App::mcpStop() {
    if (!mcp_) return;
    auto srv = mcp_;
    mcp_.reset();
    srv->run = false;
    srv->stopAll->store(true);
    {
        std::lock_guard<std::mutex> lk(srv->mx);
        for (auto& [k, c] : srv->inFlight) c->store(true);
    }
    if (srv->accepter.joinable()) srv->accepter.join();
    sock::close(srv->listener);
    srv->listener = sock::kBad;
    // Connections end within a moment (they poll the stop flag); tool calls waiting on the main
    // thread are cancelled. Keep the main loop's queue moving while they finish.
    for (int i = 0; i < 40 && srv->conns.load() > 0; ++i) {
        if (jobs_) jobs_->pumpMain();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void App::mcpRestart() {
    mcpStop();
    mcpStart();
}

void App::mcpShutdown() {
    bool other = mcp_ && mcp_->other;
    mcpStop();
    if (other) return;   // the file belongs to the other Kestral window
    std::error_code ec;
    std::filesystem::remove(u8path(mcp::infoPath()), ec);
}

App::McpStatus App::mcpStatus() const {
    McpStatus s;
    if (!mcp_) return s;
    std::lock_guard<std::mutex> lk(mcp_->mx);
    s.running = mcp_->run.load();
    s.port = mcp_->port;
    s.error = mcp_->error;
    s.client = mcp_->lastClient;
    s.tool = mcp_->runningTool;
    s.calls = mcp_->calls;
    s.idleSecs = mcp_->lastUse > -1e8 ? nowSec() - mcp_->lastUse : -1;
    return s;
}

// ---------------------------------------------------------------- preferences

namespace {

std::string jsonEsc(const std::string& s) {
    Value v(s);
    return v.dump();
}

// Indents compact JSON (Claude Desktop's settings file stays readable for people).
std::string prettyJson(const std::string& j) {
    std::string o;
    int depth = 0;
    bool str = false, esc = false;
    auto nl = [&] { o += '\n'; o.append((size_t)depth * 2, ' '); };
    for (size_t i = 0; i < j.size(); ++i) {
        char c = j[i];
        if (str) {
            o += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') { str = true; o += c; }
        else if (c == '{' || c == '[') {
            o += c;
            bool empty = i + 1 < j.size() && (j[i + 1] == '}' || j[i + 1] == ']');
            if (!empty) { depth++; nl(); }
        } else if (c == '}' || c == ']') {
            if (!(i > 0 && (j[i - 1] == '{' || j[i - 1] == '['))) { depth--; nl(); }
            o += c;
        } else if (c == ',') { o += c; nl(); }
        else if (c == ':') o += ": ";
        else o += c;
    }
    return o + "\n";
}

std::string claudeDesktopConfigPath() {
#ifdef _WIN32
    return pathToU8(u8path(platform::appDataDir()).parent_path() / "Claude" / "claude_desktop_config.json");
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/Claude/claude_desktop_config.json";
#endif
}

} // namespace

void App::drawMcpPrefs() {
    const float S = uiScale_;
    ImGui::Spacing();
    ImGui::PushFont(fonts_.bold, 0.f);
    ImGui::TextUnformatted("MCP server (other AI apps)");
    ImGui::PopFont();
    flashItem("pref:MCP server (other AI apps)");
    ImGui::Separator();
    if (ImGui::Checkbox("Let other AI apps use Kestral's tools (MCP)", &settings_.mcpEnabled)) {
        settings_.save(settingsPath_);
        mcpRestart();
    }
    flashItem("pref:Let other AI apps use Kestral's tools (MCP)");
    ImGui::TextDisabled("Claude Desktop, Claude Code, Cursor and any other MCP app can then build, check, slice and print here, live.");
    ImGui::TextDisabled("Only programs on this computer can connect, and only with Kestral's key.");
    McpStatus st = mcpStatus();
    std::string exe = platform::selfExe();
    std::string url = "http://127.0.0.1:" + std::to_string(st.port) + "/mcp";
    if (!settings_.mcpEnabled) {
        ImGui::TextDisabled("Off.");
    } else if (mcp_ && mcp_->other) {
        ImGui::TextDisabled("%s", st.error.c_str());
    } else if (!st.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.f), "%s", st.error.c_str());
    } else if (st.running) {
        ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.5f, 1.f), "Running at %s", url.c_str());
        if (!st.tool.empty())
            ImGui::Text("%s is running %s", st.client.c_str(), st.tool.c_str());
        else if (st.idleSecs >= 0)
            ImGui::TextDisabled("Last used by %s %s ago (%d tool calls since Kestral started)", st.client.c_str(),
                                st.idleSecs < 90 ? (std::to_string((int)st.idleSecs) + " s").c_str() : (std::to_string((int)(st.idleSecs / 60)) + " min").c_str(), st.calls);
        else
            ImGui::TextDisabled("No app has connected yet.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine(190 * S);
    ImGui::SetNextItemWidth(140 * S);
    static int portEdit = -1;
    if (portEdit < 0) portEdit = settings_.mcpPort;
    ImGui::InputInt("##mcpport", &portEdit, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        portEdit = std::clamp(portEdit, 1024, 65000);
        if (portEdit != settings_.mcpPort) {
            settings_.mcpPort = portEdit;
            settings_.save(settingsPath_);
            mcpRestart();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(the next free one is used if it is taken)");

    if (!settings_.mcpEnabled) return;
    ImGui::Spacing();
    ImGui::TextUnformatted("Set up an app");
    std::string desktopJson = "{\n  \"mcpServers\": {\n    \"kestral\": {\n      \"command\": " + jsonEsc(exe) + ",\n      \"args\": [\"--mcp\"]\n    }\n  }\n}";
    if (ImGui::Button("Add Kestral to Claude Desktop")) {
        std::string path = claudeDesktopConfigPath(), perr;
        Value cfg = Value::object();
        bool ok = true;
        {
            std::ifstream f(u8path(path), std::ios::binary);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                std::string text = ss.str();
                if (text.find_first_not_of(" \r\n\t") != std::string::npos) {
                    cfg = Value::parse(text, &perr);
                    ok = perr.empty() && cfg.isObject();
                }
            }
        }
        if (!ok) {
            toast("Claude Desktop's settings file could not be read, so Kestral left it alone. Use Copy setup instead.", ToastKind::Warning);
        } else {
            Value servers = cfg["mcpServers"].isObject() ? cfg["mcpServers"] : Value::object();
            Value k = Value::object();
            k.set("command", exe);
            Value args = Value::array();
            args.push("--mcp");
            k.set("args", args);
            servers.set("kestral", k);
            cfg.set("mcpServers", servers);
            std::error_code ec;
            std::filesystem::create_directories(u8path(path).parent_path(), ec);
            if (std::filesystem::exists(u8path(path), ec)) std::filesystem::copy_file(u8path(path), u8path(path + ".bak"), std::filesystem::copy_options::overwrite_existing, ec);
            std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
            f << prettyJson(cfg.dump());
            if (f) toast("Added. Restart Claude Desktop (quit it from the tray) and Kestral shows up in its tools.", ToastKind::Info);
            else toast("Could not write " + path, ToastKind::Error);
        }
    }
    ImGui::SetItemTooltip("Writes Kestral into %s (a backup is kept as .bak).", claudeDesktopConfigPath().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy setup (Claude Desktop, Cursor, others)")) {
        ImGui::SetClipboardText(desktopJson.c_str());
        toast("Copied. Paste it into the app's MCP settings file.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("%s", desktopJson.c_str());
    std::string codeCmd = "claude mcp add --scope user kestral -- \"" + exe + "\" --mcp";
    if (ImGui::Button("Copy command for Claude Code")) {
        ImGui::SetClipboardText(codeCmd.c_str());
        toast("Copied. Run it in a terminal.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("%s", codeCmd.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy address and key (HTTP)")) {
        std::string t = "URL: " + url + "\nHeader: Authorization: Bearer " + settings_.mcpToken;
        ImGui::SetClipboardText(t.c_str());
        toast("Copied the address and key.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("For apps that connect to an MCP server by address (Streamable HTTP).");
    std::string roCmd = "claude mcp add --scope user kestral-view -- \"" + exe + "\" --mcp --read-only";
    if (ImGui::Button("Copy read-only command for Claude Code")) {
        ImGui::SetClipboardText(roCmd.c_str());
        toast("Copied. An app set up this way can look at designs and the screen but change nothing.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("%s", roCmd.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy read-only address and key")) {
        std::string t = "URL: " + url + "\nHeader: Authorization: Bearer " + settings_.mcpReadToken;
        ImGui::SetClipboardText(t.c_str());
        toast("Copied the read-only address and key.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("This key can read the design, take pictures and wait for things, never change anything.");
    if (ImGui::Button("Show what MCP apps did")) {
        std::string lp = platform::appDataDir() + "/mcp_log.jsonl";
        std::error_code ec;
        if (!std::filesystem::exists(u8path(lp), ec)) { std::ofstream(u8path(lp), std::ios::app); }
        platform::revealInExplorer(lp);
    }
    ImGui::SetItemTooltip("Every call an MCP app made, one line each: when, which app, the tool, and the result (mcp_log.jsonl).");
    ImGui::SameLine();
    if (ImGui::Button("New keys")) {
        settings_.mcpReadToken = sock::randomHex(24);
        settings_.mcpToken = sock::randomHex(24);
        settings_.save(settingsPath_);
        mcpRestart();
        toast("New keys made. Apps set up with Kestral --mcp pick them up by themselves.", ToastKind::Info);
    }
    ImGui::SetItemTooltip("Apps that use the old key by address stop working until you give them the new one.");
    ImGui::TextDisabled("Prints still wait for you to press Print, unless \"Let it start prints without asking\" is on.");
}

// ---------------------------------------------------------------- the stdio bridge

namespace mcp {

namespace {

struct Info {
    bool ok = false, enabled = false;
    int port = 0, pid = 0;
    std::string token, readToken, error;
};

bool gReadOnly = false;

Info readInfo() {
    Info i;
    std::ifstream f(u8path(infoPath()), std::ios::binary);
    if (!f) return i;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    Value v = Value::parse(ss.str(), &err);
    if (!err.empty() || !v.isObject()) return i;
    i.ok = true;
    i.enabled = v["enabled"].asBool(false);
    i.port = (int)v["port"].asNumber(0);
    i.pid = (int)v["pid"].asNumber(0);
    i.token = v["token"].asString();
    i.readToken = v["read_token"].asString();
    i.error = v["error"].asString();
    return i;
}

bool answers(int port) {
    if (port <= 0) return false;
    sock::Sock s = sock::connectLocal(port, 400);
    if (s == sock::kBad) return false;
    sock::close(s);
    return true;
}

std::mutex gLaunchMx;
bool gLaunched = false;
std::mutex gSessMx;
std::string gSession;

// Makes sure a Kestral with its server on is there. Starts one when none is open.
bool ensureServer(Info& info, std::string& err) {
    std::lock_guard<std::mutex> lk(gLaunchMx);
    info = readInfo();
    if (info.ok && info.enabled && answers(info.port)) return true;
    bool alive = info.ok && sock::processAlive(info.pid);
    if (alive && !info.enabled) {
        err = info.error.empty() ? "Kestral is open but its MCP server is off. Turn it on in Kestral: Preferences > Assistant > MCP server."
                                 : "Kestral's MCP server could not start: " + info.error;
        return false;
    }
    if (!alive && !gLaunched) {
        std::string lerr;
        gLaunched = true;
        if (!platform::launch(platform::selfExe(), "", lerr)) { err = "Could not start Kestral: " + lerr; return false; }
    }
    for (int i = 0; i < 240; ++i) {   // up to a minute: the first start loads a lot
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        info = readInfo();
        if (info.ok && info.enabled && answers(info.port)) return true;
        if (info.ok && !info.enabled && sock::processAlive(info.pid)) {
            err = info.error.empty() ? "Kestral is open but its MCP server is off. Turn it on in Kestral: Preferences > Assistant > MCP server."
                                     : "Kestral's MCP server could not start: " + info.error;
            return false;
        }
    }
    err = "Kestral did not start its MCP server in time. Open Kestral and check Preferences > Assistant > MCP server.";
    return false;
}

void replyError(const Value& id, const std::string& msg) {
    if (id.isNull()) return;
    sock::writeOut(rpcError(id, -32000, msg).dump() + "\n");
}

// Sends one message to Kestral and writes whatever comes back to stdout, one JSON message per line.
void forward(const std::string& line, const Value& id) {
    Info info;
    std::string err;
    if (!ensureServer(info, err)) { replyError(id, err); return; }
    sock::Sock s = sock::connectLocal(info.port, 2000);
    if (s == sock::kBad) { replyError(id, "Kestral did not answer on port " + std::to_string(info.port) + "."); return; }
    std::string sess;
    {
        std::lock_guard<std::mutex> lk(gSessMx);
        sess = gSession;
    }
    std::string req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(info.port) +
                      "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nAuthorization: Bearer " + (gReadOnly ? info.readToken : info.token) +
                      "\r\n" + (sess.empty() ? std::string() : "Mcp-Session-Id: " + sess + "\r\n") +
                      "Content-Length: " + std::to_string(line.size()) + "\r\nConnection: close\r\n\r\n" + line;
    if (!sock::sendAll(s, req)) { sock::close(s); replyError(id, "Lost the connection to Kestral."); return; }
    std::string buf;
    char tmp[65536];
    size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        int n = sock::recvSome(s, tmp, sizeof tmp, 1000);
        if (n < 0) { sock::close(s); replyError(id, "Kestral closed the connection."); return; }
        if (n > 0) buf.append(tmp, (size_t)n);
    }
    std::string head = buf.substr(0, he);
    buf.erase(0, he + 4);
    int code = 0;
    std::sscanf(head.c_str(), "HTTP/%*s %d", &code);
    std::map<std::string, std::string> h;
    {
        std::istringstream hs(head);
        std::string l;
        std::getline(hs, l);
        while (std::getline(hs, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            size_t c = l.find(':');
            if (c == std::string::npos) continue;
            std::string k = lowerStr(l.substr(0, c)), v = l.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            h[k] = v;
        }
    }
    if (h.count("mcp-session-id")) {
        std::lock_guard<std::mutex> lk(gSessMx);
        gSession = h["mcp-session-id"];
    }
    if (code == 202 || code == 204) { sock::close(s); return; }
    bool sse = h["content-type"].find("text/event-stream") != std::string::npos;
    if (sse) {
        std::string data;
        size_t pos = 0;
        auto drain = [&] {
            for (;;) {
                size_t nl = buf.find('\n', pos);
                if (nl == std::string::npos) break;
                std::string l = buf.substr(pos, nl - pos);
                pos = nl + 1;
                if (!l.empty() && l.back() == '\r') l.pop_back();
                if (l.empty()) {
                    if (!data.empty()) sock::writeOut(data + "\n");
                    data.clear();
                } else if (l.rfind("data:", 0) == 0) {
                    std::string d = l.substr(5);
                    if (!d.empty() && d.front() == ' ') d.erase(0, 1);
                    data += d;
                }
            }
            buf.erase(0, pos);
            pos = 0;
        };
        drain();
        for (;;) {
            int n = sock::recvSome(s, tmp, sizeof tmp, 1000);
            if (n < 0) break;
            if (n > 0) { buf.append(tmp, (size_t)n); drain(); }
        }
        if (!data.empty()) sock::writeOut(data + "\n");
        sock::close(s);
        return;
    }
    size_t len = h.count("content-length") ? (size_t)std::strtoull(h["content-length"].c_str(), nullptr, 10) : (size_t)-1;
    while (buf.size() < len) {
        int n = sock::recvSome(s, tmp, sizeof tmp, 1000);
        if (n < 0) break;
        if (n > 0) buf.append(tmp, (size_t)n);
    }
    sock::close(s);
    if (len != (size_t)-1 && buf.size() > len) buf.resize(len);
    if (code == 200 && !buf.empty()) {
        for (auto& ch : buf) if (ch == '\n' || ch == '\r') ch = ' ';
        sock::writeOut(buf + "\n");
        return;
    }
    std::string why = buf;
    Value ev = Value::parse(buf);
    if (ev["error"]["message"].isString()) why = ev["error"]["message"].asString();
    replyError(id, "Kestral answered " + std::to_string(code) + ": " + why);
}

} // namespace

int runBridge(bool readOnly) {
    gReadOnly = readOnly;
    sock::init();
    std::vector<std::thread> work;
    std::string line;
    while (sock::readLine(line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::string perr;
        Value m = Value::parse(line, &perr);
        if (!perr.empty()) {
            sock::writeOut(rpcError(Value(), -32700, "Parse error: " + perr).dump() + "\n");
            continue;
        }
        Value id = m.isObject() ? m["id"] : Value();
        // initialize goes first and alone, so the session is set before anything else is sent.
        bool first = m.isObject() && m["method"].asString() == "initialize";
        if (first) {
            forward(line, id);
            continue;
        }
        work.emplace_back([line, id] { forward(line, id); });
        // Finished threads are joined as we go so a long session does not pile them up.
        if (work.size() > 64) {
            for (auto& t : work) if (t.joinable()) t.join();
            work.clear();
        }
    }
    for (auto& t : work) if (t.joinable()) t.join();
    return 0;
}

} // namespace mcp

} // namespace kst
