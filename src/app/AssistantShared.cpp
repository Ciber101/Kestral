// Pieces of the assistant's tools that run off the main thread and are shared by the built in
// assistant and the MCP server: pictures of the design (look_at_design) and illustrations.
#include "app/App.h"
#include "app/Illustration.h"
#include "app/Thumbnails.h"
#include "core/Json.h"
#include "platform/Platform.h"
#include "third_party/stb_image.h"

#include <algorithm>
#include <ctime>
#include <thread>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>

namespace kst {

using json::Value;

namespace {
std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

bool App::designPicture(const std::string& argsJson, const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel,
                        AiImageBuilt& built, std::string& head, std::string& order, std::string& err) {
    Value a = Value::parse(argsJson);
    std::vector<std::string> views;
    for (auto& v : a["views"].items()) views.push_back(lowerAscii(v.asString()));
    if (views.empty()) views = {"iso", "front", "top", "right"};
    if (views.size() > 8) views.resize(8);
    std::vector<std::string> bodies;
    for (auto& b : a["bodies"].items()) bodies.push_back(b.asString());
    auto prom = std::make_shared<std::promise<AiShotScene>>();
    auto fut = prom->get_future();
    post([this, prom, bodies] { prom->set_value(aiShotScene(bodies)); });
    AiShotScene sc;
    for (;;) {
        if (cancel->load()) { err = "Stopped."; return false; }
        if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) { sc = fut.get(); break; }
    }
    if (sc.items.empty()) { err = "Nothing to look at: no visible bodies" + (sc.missing.empty() ? std::string(".") : " (not found: " + sc.missing + ")."); return false; }
    // Views and their directions (from the model toward the camera) and screen up.
    auto dirFor = [&](const std::string& v, glm::vec3& eye, glm::vec3& up) {
        up = glm::vec3(0, 0, 1);
        if (v == "front") eye = {0, -1, 0};
        else if (v == "back") eye = {0, 1, 0};
        else if (v == "right") eye = {1, 0, 0};
        else if (v == "left") eye = {-1, 0, 0};
        else if (v == "top") { eye = {0, 0, 1}; up = {0, 1, 0}; }
        else if (v == "bottom") { eye = {0, 0, -1}; up = {0, 1, 0}; }
        else if (v == "current") { eye = -sc.camFwd; up = sc.camUp; }
        else eye = glm::normalize(glm::vec3(1, -1, 0.8f));   // iso, the home view
    };
    int n = (int)views.size();
    int cols = n == 1 ? 1 : n <= 4 ? 2 : n <= 6 ? 3 : 4, rows = (n + cols - 1) / cols;
    int pw = n == 1 ? 960 : 560, ph = n == 1 ? 720 : 480;
    int W = cols * pw + (cols - 1) * 4, H = rows * ph + (rows - 1) * 4;
    std::vector<uint8_t> sheet((size_t)W * H * 4, 255);
    for (size_t i = 0; i < sheet.size(); i += 4) { sheet[i] = 60; sheet[i + 1] = 62; sheet[i + 2] = 68; }   // gaps between panels
    for (int k = 0; k < n; ++k) {
        glm::vec3 eye, up;
        dirFor(views[(size_t)k], eye, up);
        auto px = thumb::renderView(sc.items, pw, ph, eye, up);
        int ox = (k % cols) * (pw + 4), oy = (k / cols) * (ph + 4);
        for (int y = 0; y < ph; ++y)
            std::memcpy(&sheet[((size_t)(oy + y) * W + ox) * 4], &px[(size_t)y * pw * 4], (size_t)pw * 4);
    }
    if (const char* dump = std::getenv("KESTRAL_SHOT_DUMP")) thumb::writePng(dump, sheet, W, H);   // developer check
    built = aiBuildImage(sheet.data(), W, H);
    order.clear();
    for (auto& v : views) order += (order.empty() ? "" : ", ") + v;
    // Size of what is shown, so the picture has a scale.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (auto& it : sc.items) {
        const auto& v = it.mesh->verts;
        for (size_t i = 0; i + 5 < v.size(); i += 6) {
            glm::vec3 p = glm::vec3(it.xform * glm::vec4(v[i], v[i + 1], v[i + 2], 1.f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    char box[160];
    std::snprintf(box, sizeof box, "%d bodies, overall %.1f x %.1f x %.1f mm (X x Y x Z)", (int)sc.items.size(), hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
    head = "Picture of the design: " + order + " (panels left to right, then top to bottom). " + box + ".";
    return true;
}

std::string App::illustrationTool(const std::string& argsJson, const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel,
                                  AiImageBuilt* outImg) {
    Value a = Value::parse(argsJson);
    std::vector<std::string> bodies;
    for (auto& b : a["bodies"].items()) bodies.push_back(b.asString());
    struct Prep { std::vector<illus::Part> parts; illus::Options opt; std::string defPath; };
    auto prom = std::make_shared<std::promise<Prep>>();
    auto fut = prom->get_future();
    post([this, prom, bodies] {
        Prep pr;
        pr.parts = illusParts(false, bodies);
        pr.opt = illusOptions(1920, 1080);
        pr.defPath = platform::designsDir() + "/" + (doc_.name.empty() ? std::string("Kestral") : doc_.name) + " illustration.png";
        prom->set_value(pr);
    });
    Prep pr;
    bool got = false;
    while (!cancel->load())
        if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) { pr = fut.get(); got = true; break; }
    if (!got) return "Stopped.";
    illus::Options o = pr.opt;
    std::string v = lowerAscii(a["view"].asString()), st = lowerAscii(a["style"].asString()), bgs = lowerAscii(a["background"].asString());
    o.view = v == "front" ? illus::View::Front : v == "top" ? illus::View::Top : v == "right" ? illus::View::Right : v == "left" ? illus::View::Left
           : v == "back" ? illus::View::Back : v == "bottom" ? illus::View::Bottom : v == "current" ? illus::View::Current : illus::View::Iso;
    o.style = st == "shaded" ? illus::Style::Shaded : st == "line_art" ? illus::Style::LineArt : st == "shaded_no_lines" ? illus::Style::ShadedNoLines
                                                                                                             : illus::Style::Toon;
    o.background = bgs == "transparent" ? illus::Background::Transparent : bgs == "dark" ? illus::Background::Dark : illus::Background::White;
    o.explode = std::clamp((float)a["explode"].asNumber(0), 0.f, 3.f);
    o.numbers = a["numbers"].asBool(false);
    o.partsList = a["parts_list"].asBool(o.numbers);
    o.width = std::clamp((int)a["width"].asNumber(1920), 256, 8192);
    o.height = std::clamp((int)a["height"].asNumber(1080), 256, 8192);
    std::string path = a["path"].asString().empty() ? pr.defPath : a["path"].asString();
    bool svg = lowerAscii(path).size() > 4 && lowerAscii(path).substr(lowerAscii(path).size() - 4) == ".svg";
    std::string result;
    if (pr.parts.empty()) {
        result = "Nothing to draw: no visible bodies (or none of the named ones).";
    } else {
        illus::Result r = illus::render(pr.parts, o, svg);
        bool ok;
        if (svg) {
            std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
            f << r.svg;
            ok = (bool)f;
        } else {
            ok = illus::writePng(path, r.rgba, r.width, r.height);
        }
        result = ok ? "Saved the illustration to " + path + " (" + std::to_string(r.width) + " x " + std::to_string(r.height) +
                          (r.partsNumbered ? ", " + std::to_string(r.partsNumbered) + " parts numbered" : "") + ")."
                    : "Could not write " + path + ".";
        if (ok) {
            AiImageBuilt built = aiBuildImage(r.rgba.data(), r.width, r.height);
            if (outImg) *outImg = built;
            post([this, built, path]() mutable {
                aiImageTexture(built);
                AiLine ln{6, "Saved an illustration: " + path};
                ln.images = {built.img};
                ai_.lines.push_back(ln);
                ai_.scroll = true;
            });
        }
    }
    return result;
}

bool App::cameraPicture(const PostFn& post, const std::shared_ptr<std::atomic<bool>>& cancel, AiImageBuilt& built, std::string& msg) {
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut = prom->get_future();
    post([this, prom] {
        print::PrinterConn c = printerConn();
        bool bambu = c.kind == print::PrinterKind::BambuOrca || c.kind == print::PrinterKind::BambuLan || c.kind == print::PrinterKind::BambuConnect;
        if (c.kind == print::PrinterKind::None) { prom->set_value("No printer is set up in Kestral yet."); return; }
        if (!bambu) { prom->set_value("This printer has no camera Kestral can show (only Bambu Lab A1, A1 mini and P1 cameras)."); return; }
        camOn_ = true;
        if (!camera_.running()) camera_.start(c);
        prom->set_value("");
    });
    std::string why;
    for (;;) {
        if (cancel->load()) { msg = "Stopped."; return false; }
        if (fut.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) { why = fut.get(); break; }
    }
    if (!why.empty()) { msg = why; return false; }
    // The camera sends a picture about once a second; the first one can take a few seconds.
    std::string jpeg;
    for (int i = 0; i < 150 && !cancel->load(); ++i) {   // up to 15 s
        uint64_t seq = 0;
        if (camera_.latest(jpeg, seq) && !jpeg.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (cancel->load()) { msg = "Stopped."; return false; }
    if (jpeg.empty()) { msg = "The printer's camera sent no picture (" + camera_.status() + "). Is the printer on and on the network?"; return false; }
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(jpeg.data()), (int)jpeg.size(), &w, &h, &n, 4);
    if (!px) { msg = "The camera picture could not be read."; return false; }
    built = aiBuildImage(px, w, h);
    stbi_image_free(px);
    std::time_t t = std::time(nullptr);
    char when[16];
    std::strftime(when, sizeof when, "%H:%M:%S", std::localtime(&t));
    msg = std::string("Picture from the printer's camera at ") + when + " (" + std::to_string(w) + " x " + std::to_string(h) + ").";
    return true;
}

} // namespace kst
