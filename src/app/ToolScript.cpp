// Developer check: Kestral --run-tools <script.jsonl> <design.kst> runs assistant tool calls from a file,
// one per line, exactly as the assistant would, then saves the design and quits.
//   {"t":"create_sketch","a":{"plane":"XY"},"as":"s1"}   "as" keeps the id from the result
//   {"t":"extrude","a":{"sketch_id":"$s1","distance":5}}  "$s1" becomes that id
// Lines starting with # are skipped. Every result goes to <script>.out.txt.
#include "app/App.h"
#include "core/Log.h"
#include "core/SaveLoad.h"
#include "core/Json.h"
#include "core/Types.h"

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace kst {

void App::toolScriptStep() {
    static std::vector<std::string> lines;
    static size_t idx = 0;
    static bool loaded = false, waiting = false;
    static double t0 = 0;
    static std::map<std::string, std::string> vars;
    static std::ofstream out;
    static int fails = 0;
    activity();
    if (!loaded) {
        loaded = true;
        std::ifstream f(toolScript_);
        std::string l;
        while (std::getline(f, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (!l.empty() && l[0] != '#') lines.push_back(l);
        }
        out.open(toolScript_ + ".out.txt", std::ios::binary);
        out << lines.size() << " calls\n";
    }
    auto busy = [&] { return recomputeRunning_ || recomputeQueuedFrom_ >= 0 || (jobs_ && jobs_->active() > 0); };
    if (waiting) {
        if (busy() && time_ - t0 < 1800) return;
        waiting = false;
    }
    if (idx >= lines.size()) {
        std::string failed;
        for (auto& ft : doc_.timeline)
            if (ft.failed) failed += "  " + ft.name + ": " + ft.error + "\n";
        out << "features that failed:\n" << (failed.empty() ? "  none\n" : failed);
        out << "design:\n" << aiRunTool("get_design", "{}") << "\n";
        bool ok = saveDocument(doc_, toolScriptOut_);
        {
            // The shown bodies as OBJ (world placed, color in a comment) for looking at the result outside Kestral.
            std::ofstream m(toolScriptOut_ + ".obj", std::ios::binary);
            size_t base = 1;
            for (auto& [id, b] : doc_.bodies) {
                if (!doc_.isBodyAlive(b) || !bodyShown(b)) continue;
                auto pit = doc_.parts.find(b.partId);
                if (pit == doc_.parts.end() || !pit->second.mesh) continue;
                const MeshData& md = *pit->second.mesh;
                m << "o " << aiBodyPath(id) << "\n# color " << b.color.r << " " << b.color.g << " " << b.color.b << "\n";
                for (size_t i = 0; i < md.vertexCount(); ++i) {
                    glm::vec4 p = b.xform * glm::vec4(md.verts[i * 6], md.verts[i * 6 + 1], md.verts[i * 6 + 2], 1.f);
                    m << "v " << p.x << " " << p.y << " " << p.z << "\n";
                }
                for (size_t t = 0; t < md.triCount(); ++t)
                    m << "f " << base + md.indices[t * 3] << " " << base + md.indices[t * 3 + 1] << " " << base + md.indices[t * 3 + 2] << "\n";
                base += md.vertexCount();
            }
        }
        out << (ok ? "saved " : "SAVE FAILED ") << toolScriptOut_ << "\n";
        out.close();
        log::info("tool script done, " + std::to_string(fails) + " errors");
        dirty_ = false;
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
        idx = (size_t)-1 / 2;
        return;
    }
    std::string line = lines[idx++];
    std::string perr;
    json::Value v = json::Value::parse(line, &perr);
    if (!perr.empty() || !v.isObject()) {
        out << "#" << idx << " BAD LINE " << perr << ": " << line << "\n";
        ++fails;
        return;
    }
    std::string args = v["a"].isObject() ? v["a"].dump() : "{}";
    for (auto& [k, val] : vars) {
        std::string key = "\"$" + k + "\"";
        for (size_t p; (p = args.find(key)) != std::string::npos;) args.replace(p, key.size(), val);
    }
    std::string res;
    if (v["t"].asString() == "_open") {   // developer: open a saved design (it rebuilds from its timeline)
        dirty_ = false;
        openDocumentPath(json::Value::parse(args)["path"].asString());
        res = "opened";
    } else {
        res = aiRunTool(v["t"].asString(), args);
    }
    std::string as = v["as"].asString();
    if (!as.empty()) {
        std::string val;
        for (const char* key : {"sketch_id=", "\"sketch_id\":", "id="}) {
            size_t p = res.find(key);
            if (p == std::string::npos) continue;
            p += std::strlen(key);
            while (p < res.size() && res[p] == ' ') ++p;
            size_t e = p;
            while (e < res.size() && (std::isdigit((unsigned char)res[e]) || res[e] == '-')) ++e;
            val = res.substr(p, e - p);
            break;
        }
        vars[as] = val.empty() ? "0" : val;
    }
    std::string low = res;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    bool bad = low.find("error") != std::string::npos || low.find("failed") != std::string::npos || low.find("cannot") != std::string::npos ||
               low.find("unknown") != std::string::npos || low.find("not found") != std::string::npos;
    if (bad) ++fails;
    char tbuf[32];
    std::snprintf(tbuf, sizeof tbuf, " [%.1f s]", time_);
    out << "#" << idx << tbuf << (bad ? " ?? " : " ") << v["t"].asString() << " " << args.substr(0, 300) << "\n   -> " << res.substr(0, 20000) << "\n";
    out.flush();
    waiting = true;
    t0 = time_;
}

} // namespace kst
