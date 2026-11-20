// Pictures for the assistant: pasted screenshots, dropped or picked image files. Each one is
// scaled to a size the models read well, stored as a compressed PNG (base64) and gets a small
// preview texture for the chat.
#include "app/App.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "third_party/stb_image.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace kst {

namespace {

void put32(std::string& v, uint32_t x) {
    v.push_back((char)(x >> 24)); v.push_back((char)(x >> 16)); v.push_back((char)(x >> 8)); v.push_back((char)x);
}

void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    put32(png, (uint32_t)crc32(0L, (const Bytef*)td.data(), (uInt)td.size()));
}

// RGB PNG, rows filtered with "Sub" and deflated: a screenshot ends up a few hundred KB.
std::string encodePng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::string raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(1);   // Sub
        const uint8_t* row = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                uint8_t left = x > 0 ? row[(x - 1) * 4 + c] : 0;
                raw.push_back((char)(uint8_t)(row[x * 4 + c] - left));
            }
    }
    uLongf zn = compressBound((uLong)raw.size());
    std::string z(zn, '\0');
    if (compress2((Bytef*)z.data(), &zn, (const Bytef*)raw.data(), (uLong)raw.size(), 6) != Z_OK) return {};
    z.resize(zn);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);   // 8 bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += t[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out += t[n >> 18]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63]; out += '=';
    }
    return out;
}

// Box filter down to fit maxSide (never up).
std::vector<uint8_t> shrink(const uint8_t* rgba, int w, int h, int maxSide, int& ow, int& oh) {
    double s = std::min(1.0, (double)maxSide / std::max(w, h));
    ow = std::max(1, (int)(w * s + 0.5));
    oh = std::max(1, (int)(h * s + 0.5));
    std::vector<uint8_t> out((size_t)ow * oh * 4);
    if (ow == w && oh == h) { std::memcpy(out.data(), rgba, out.size()); return out; }
    for (int y = 0; y < oh; ++y) {
        int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = rgba + ((size_t)yy * w + xx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c) out[((size_t)y * ow + x) * 4 + c] = (uint8_t)(acc[c] / n);
        }
    }
    return out;
}

} // namespace

bool App::isImageFile(const std::string& path) {
    std::string e = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".gif" || e == ".tga";
}

App::AiImageBuilt App::aiBuildImage(const uint8_t* rgba, int w, int h) {
    AiImageBuilt out;
    // Transparent parts become white (models read a flat picture best).
    std::vector<uint8_t> flat(rgba, rgba + (size_t)w * h * 4);
    for (size_t i = 0; i < flat.size(); i += 4) {
        uint32_t a = flat[i + 3];
        if (a == 255) continue;
        for (int c = 0; c < 3; ++c) flat[i + c] = (uint8_t)((flat[i + c] * a + 255 * (255 - a)) / 255);
        flat[i + 3] = 255;
    }
    int sw, sh;
    auto big = shrink(flat.data(), w, h, 1568, sw, sh);   // about what Claude, GPT and Qwen read in full
    auto img = std::make_shared<AiImage>();
    img->mime = "image/png";
    img->b64 = base64(encodePng(big, sw, sh));
    img->w = sw;
    img->h = sh;
    out.small = shrink(flat.data(), w, h, 220, out.tw, out.th);
    out.img = img;
    return out;
}

void App::aiImageTexture(AiImageBuilt& b) {
    if (!b.img || b.img->tex || b.small.empty()) return;
    b.img->tex = makeTexture(b.small.data(), b.tw, b.th);
    aiImageTex_.push_back(b.img->tex);
}

bool App::aiAttachImage(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;
    if (aiAttach_.size() >= 6) { toast("Up to 6 pictures per message", ToastKind::Warning); return false; }
    AiImageBuilt b = aiBuildImage(rgba, w, h);
    aiImageTexture(b);
    aiAttach_.push_back(b.img);
    log::info("Assistant: picture attached, " + std::to_string(b.img->w) + " x " + std::to_string(b.img->h) + ", " + std::to_string(b.img->b64.size() / 1024) + " KB");
    ai_.open = true;
    ai_.focus = true;
    activity();
    return true;
}

bool App::aiAttachFile(const std::string& path) {
    int w = 0, h = 0, n = 0;
    std::string data;
    {
        FILE* f = nullptr;
#ifdef _WIN32
        f = _wfopen(u8path(path).wstring().c_str(), L"rb");
#else
        f = std::fopen(path.c_str(), "rb");
#endif
        if (!f) { toast("Could not open " + path, ToastKind::Error); return false; }
        char buf[65536];
        size_t k;
        while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, k);
        std::fclose(f);
    }
    unsigned char* px = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!px) { toast("That picture could not be read (PNG, JPG, BMP or GIF)", ToastKind::Error); return false; }
    bool ok = aiAttachImage(px, w, h);
    stbi_image_free(px);
    return ok;
}

bool App::aiPasteImage() {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::string file;
    if (!platform::clipboardImage(rgba, w, h, file)) return false;
    if (!rgba.empty()) return aiAttachImage(rgba.data(), w, h);
    if (!file.empty() && isImageFile(file)) return aiAttachFile(file);
    return false;
}

void App::aiClearChat() {
    ai_.lines.clear();
    ai_.history.clear();
    aiActiveTools_.clear();
    aiLastPromptTok_ = aiLastOutTok_ = 0;
    aiAttach_.clear();
    for (auto* t : aiImageTex_) dropTexture(t);
    aiImageTex_.clear();
    aiFollow_ = true;
}

} // namespace kst

namespace kst {

App::AiShotScene App::aiShotScene(const std::vector<std::string>& bodies) const {
    AiShotScene sc;
    std::set<uint32_t> only;
    if (!bodies.empty()) {
        auto ids = aiFindBodies(bodies, sc.missing);
        only.insert(ids.begin(), ids.end());
    }
    for (auto& [id, b] : doc_.bodies) {
        if (only.empty() ? !bodyShown(b) : !only.count(id)) continue;
        auto pit = doc_.parts.find(b.partId);
        if (pit != doc_.parts.end() && pit->second.mesh) sc.items.push_back({pit->second.mesh, b.xform, b.color});
    }
    sc.camFwd = cam_.forward();
    sc.camUp = cam_.rot * glm::vec3(0, 1, 0);
    return sc;
}

} // namespace kst
