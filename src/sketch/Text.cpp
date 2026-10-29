#include "sketch/Text.h"
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#endif

namespace kst::sk {

namespace {

struct Box { double x0, y0, x1, y1; };
struct Glyph { double adv = 0.6; std::vector<Box> boxes; };

Glyph glyphOf(char ch) {
    ch = (char)std::toupper((unsigned char)ch);
    Glyph g;
    auto b = [&](double x0, double y0, double x1, double y1) { g.boxes.push_back({x0, y0, x1, y1}); };
    switch (ch) {
    case ' ': g.adv = 0.35; break;
    case 'I': g.adv = 0.45; b(0.12, 0, 0.32, 1); break;                 // width 0.20
    case 'A': g.adv = 0.7; b(0.05, 0, 0.2, 1); b(0.5, 0, 0.65, 1); b(0.2, 0.75, 0.5, 0.92); b(0.2, 0.38, 0.5, 0.52); break;
    case 'B': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.22, 0.78, 0.55, 0.95); b(0.22, 0.4, 0.55, 0.58); b(0.22, 0.05, 0.55, 0.22); break;
    case 'C': g.adv = 0.7; b(0.08, 0.82, 0.58, 1); b(0.08, 0, 0.58, 0.18); b(0.08, 0.18, 0.26, 0.82); break;
    case 'D': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.22, 0.82, 0.55, 1); b(0.22, 0, 0.55, 0.18); b(0.4, 0.18, 0.58, 0.82); break;
    case 'E': g.adv = 0.65; b(0.05, 0, 0.22, 1); b(0.22, 0.82, 0.55, 1); b(0.22, 0.42, 0.48, 0.58); b(0.22, 0, 0.55, 0.18); break;
    case 'F': g.adv = 0.6; b(0.05, 0, 0.22, 1); b(0.22, 0.82, 0.52, 1); b(0.22, 0.42, 0.45, 0.58); break;
    case 'G': g.adv = 0.7; b(0.08, 0.82, 0.58, 1); b(0.08, 0, 0.58, 0.18); b(0.08, 0.18, 0.26, 0.82); b(0.4, 0.35, 0.58, 0.52); b(0.42, 0.18, 0.58, 0.35); break;
    case 'H': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.48, 0, 0.65, 1); b(0.22, 0.42, 0.48, 0.58); break;
    case 'J': g.adv = 0.6; b(0.3, 0.18, 0.48, 1); b(0.1, 0, 0.48, 0.18); break;
    case 'K': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.28, 0.55, 0.58, 0.72); b(0.28, 0.28, 0.58, 0.45); break;
    case 'L': g.adv = 0.6; b(0.05, 0, 0.22, 1); b(0.22, 0, 0.52, 0.18); break;
    case 'M': g.adv = 0.8; b(0.05, 0, 0.2, 1); b(0.58, 0, 0.73, 1); b(0.2, 0.7, 0.38, 1); b(0.4, 0.7, 0.58, 1); break;
    case 'N': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.48, 0, 0.65, 1); b(0.22, 0.55, 0.48, 0.75); break;
    case 'O': g.adv = 0.7; b(0.08, 0.82, 0.58, 1); b(0.08, 0, 0.58, 0.18); b(0.08, 0.18, 0.26, 0.82); b(0.42, 0.18, 0.6, 0.82); break;
    case 'P': g.adv = 0.65; b(0.05, 0, 0.22, 1); b(0.22, 0.78, 0.55, 0.96); b(0.22, 0.48, 0.55, 0.66); b(0.4, 0.66, 0.55, 0.78); break;
    case 'R': g.adv = 0.7; b(0.05, 0, 0.22, 1); b(0.22, 0.78, 0.55, 0.96); b(0.22, 0.48, 0.5, 0.66); b(0.32, 0, 0.55, 0.42); break;
    case 'S': g.adv = 0.65; b(0.08, 0.82, 0.55, 1); b(0.08, 0.55, 0.26, 0.82); b(0.08, 0.42, 0.55, 0.58); b(0.38, 0.18, 0.55, 0.42); b(0.08, 0, 0.55, 0.18); break;
    case 'T': g.adv = 0.65; b(0.05, 0.82, 0.6, 1); b(0.24, 0, 0.42, 0.82); break;
    case 'U': g.adv = 0.7; b(0.05, 0.18, 0.22, 1); b(0.48, 0.18, 0.65, 1); b(0.05, 0, 0.65, 0.18); break;
    case 'V': g.adv = 0.7; b(0.05, 0.45, 0.22, 1); b(0.48, 0.45, 0.65, 1); b(0.22, 0, 0.48, 0.5); break;
    case 'W': g.adv = 0.85; b(0.02, 0.2, 0.16, 1); b(0.68, 0.2, 0.82, 1); b(0.28, 0.2, 0.42, 1); b(0.16, 0, 0.68, 0.2); break;
    case 'X': g.adv = 0.7; b(0.05, 0.72, 0.28, 1); b(0.42, 0.72, 0.65, 1); b(0.26, 0.42, 0.44, 0.58); b(0.05, 0, 0.28, 0.28); b(0.42, 0, 0.65, 0.28); break;
    case 'Y': g.adv = 0.7; b(0.05, 0.55, 0.22, 1); b(0.48, 0.55, 0.65, 1); b(0.24, 0, 0.42, 0.58); break;
    case 'Z': g.adv = 0.65; b(0.05, 0.82, 0.58, 1); b(0.28, 0.4, 0.48, 0.82); b(0.05, 0, 0.58, 0.18); break;
    case '0': g.adv = 0.7; b(0.08, 0.82, 0.55, 1); b(0.08, 0, 0.55, 0.18); b(0.08, 0.18, 0.26, 0.82); b(0.4, 0.18, 0.58, 0.82); break;
    case '1': g.adv = 0.45; b(0.14, 0, 0.32, 1); break;
    case '2': g.adv = 0.65; b(0.05, 0.82, 0.55, 1); b(0.38, 0.5, 0.55, 0.82); b(0.05, 0.4, 0.55, 0.55); b(0.05, 0.18, 0.22, 0.4); b(0.05, 0, 0.55, 0.18); break;
    case '3': g.adv = 0.65; b(0.05, 0.82, 0.55, 1); b(0.05, 0.42, 0.5, 0.58); b(0.05, 0, 0.55, 0.18); b(0.38, 0.18, 0.55, 0.82); break;
    case '4': g.adv = 0.65; b(0.05, 0.45, 0.22, 1); b(0.05, 0.4, 0.55, 0.55); b(0.38, 0, 0.55, 1); break;
    case '5': g.adv = 0.65; b(0.05, 0.82, 0.55, 1); b(0.05, 0.55, 0.22, 0.82); b(0.05, 0.42, 0.5, 0.58); b(0.35, 0.18, 0.55, 0.42); b(0.05, 0, 0.55, 0.18); break;
    case '6': g.adv = 0.65; b(0.08, 0.18, 0.26, 1); b(0.08, 0, 0.55, 0.18); b(0.08, 0.4, 0.55, 0.55); b(0.38, 0.18, 0.55, 0.4); break;
    case '7': g.adv = 0.65; b(0.05, 0.82, 0.58, 1); b(0.32, 0, 0.5, 0.82); break;
    case '8': g.adv = 0.65; b(0.08, 0.58, 0.5, 1); b(0.08, 0, 0.5, 0.42); b(0.35, 0.42, 0.52, 0.58); break;
    case '9': g.adv = 0.65; b(0.08, 0.82, 0.55, 1); b(0.08, 0.55, 0.26, 0.82); b(0.08, 0.45, 0.55, 0.6); b(0.38, 0, 0.55, 0.82); break;
    default: return {};
    }
    return g;
}

} // namespace

namespace {

#ifdef _WIN32
Vec2 fromFx(const POINTFX& p) {
    return {p.x.value + p.x.fract / 65536.0, p.y.value + p.y.fract / 65536.0};
}

void sampleQuad(std::vector<Vec2>& out, Vec2 a, Vec2 b, Vec2 c) {
    const int N = 6;
    for (int i = 1; i <= N; ++i) {
        double t = (double)i / N, u = 1.0 - t;
        out.push_back(a * (u * u) + b * (2.0 * u * t) + c * (t * t));
    }
}

// One glyph's contours in font units, plus how far the pen moves.
bool glyphContours(HDC dc, wchar_t ch, std::vector<std::vector<Vec2>>& loops, double& advance) {
    GLYPHMETRICS gm{};
    MAT2 mat{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    DWORD sz = GetGlyphOutlineW(dc, ch, GGO_NATIVE, &gm, 0, nullptr, &mat);
    advance = 0;
    if (sz == GDI_ERROR) return false;
    advance = gm.gmCellIncX;
    if (sz == 0) return true;
    std::vector<char> buf(sz);
    if (GetGlyphOutlineW(dc, ch, GGO_NATIVE, &gm, sz, buf.data(), &mat) == GDI_ERROR) return false;
    const char* p = buf.data();
    const char* end = p + sz;
    while (p + sizeof(TTPOLYGONHEADER) <= end) {
        auto* hdr = reinterpret_cast<const TTPOLYGONHEADER*>(p);
        if (hdr->cb < sizeof(TTPOLYGONHEADER) || p + hdr->cb > end) break;
        std::vector<Vec2> loop;
        Vec2 start = fromFx(hdr->pfxStart);
        loop.push_back(start);
        Vec2 cur = start;
        const char* c = p + sizeof(TTPOLYGONHEADER);
        const char* cend = p + hdr->cb;
        while (c + 4 <= cend) {
            auto* cv = reinterpret_cast<const TTPOLYCURVE*>(c);
            size_t bytes = 4 + sizeof(POINTFX) * cv->cpfx;
            if (cv->cpfx == 0 || c + bytes > cend) break;
            if (cv->wType == TT_PRIM_LINE) {
                for (WORD i = 0; i < cv->cpfx; ++i) {
                    cur = fromFx(cv->apfx[i]);
                    loop.push_back(cur);
                }
            } else if (cv->wType == TT_PRIM_QSPLINE && cv->cpfx >= 2) {
                std::vector<Vec2> pts;
                for (WORD i = 0; i < cv->cpfx; ++i) pts.push_back(fromFx(cv->apfx[i]));
                Vec2 a = cur;
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    Vec2 ctrl = pts[i];
                    Vec2 nxt = (i + 2 == pts.size()) ? pts[i + 1] : (pts[i] + pts[i + 1]) * 0.5;
                    sampleQuad(loop, a, ctrl, nxt);
                    a = nxt;
                }
                cur = pts.back();
            }
            c += bytes;
        }
        if (loop.size() >= 3) {
            if (glm::length(loop.back() - start) > 1e-4) loop.push_back(start);
            loops.push_back(std::move(loop));
        }
        p += hdr->cb;
    }
    return true;
}

std::vector<std::vector<Vec2>> fontLoops(const std::string& font, const std::string& text, double height) {
    std::vector<std::vector<Vec2>> loops;
    if (font.empty() || text.empty() || !(height > 0)) return loops;
    int nw = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (nw <= 1) return loops;
    std::wstring w((size_t)nw, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), nw);
    HDC dc = CreateCompatibleDC(nullptr);
    const int em = 1000;
    HFONT hf = CreateFontA(-em, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, font.c_str());
    if (!hf) { DeleteDC(dc); return loops; }
    HGDIOBJ old = SelectObject(dc, hf);
    double scale = height / (double)em;
    double pen = 0;
    bool any = false;
    for (wchar_t ch : w) {
        if (!ch) break;
        std::vector<std::vector<Vec2>> glyph;
        double adv = 0;
        if (!glyphContours(dc, ch, glyph, adv)) continue;
        any = true;
        for (auto& loop : glyph) {
            for (auto& p : loop) p = Vec2{pen + p.x * scale, p.y * scale};
            loops.push_back(std::move(loop));
        }
        pen += adv * scale;
    }
    SelectObject(dc, old);
    DeleteObject(hf);
    DeleteDC(dc);
    if (!any) loops.clear();
    return loops;
}

#else

void sampleQuad(std::vector<Vec2>& out, Vec2 a, Vec2 b, Vec2 c) {
    const int N = 6;
    for (int i = 1; i <= N; ++i) {
        double t = (double)i / N, u = 1.0 - t;
        out.push_back(a * (u * u) + b * (2.0 * u * t) + c * (t * t));
    }
}

void sampleCubic(std::vector<Vec2>& out, Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const int N = 8;
    for (int i = 1; i <= N; ++i) {
        double t = (double)i / N, u = 1.0 - t;
        out.push_back(a * (u * u * u) + b * (3.0 * u * u * t) + c * (3.0 * u * t * t) + d * (t * t * t));
    }
}

std::string squash(const std::string& s) {
    std::string r;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) r += (char)std::tolower((unsigned char)c);
    return r;
}

// A font file for a family name. Windows names map to the usual Linux look-alikes, and anything
// unknown falls back to a plain sans font so text never silently disappears.
std::string findFontFileScan(const std::string& family);

// Fonts are looked up once per family name (scanning the font folders takes a moment).
std::string findFontFile(const std::string& family) {
    static std::mutex mx;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(mx);
        auto it = cache.find(family);
        if (it != cache.end()) return it->second;
    }
    std::string f = findFontFileScan(family);
    std::lock_guard<std::mutex> lk(mx);
    cache[family] = f;
    return f;
}

std::string findFontFileScan(const std::string& family) {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* h = std::getenv("HOME")) {
        dirs.push_back(fs::path(h) / ".local/share/fonts");
        dirs.push_back(fs::path(h) / ".fonts");
    }
    std::vector<std::pair<std::string, std::string>> files;   // squashed stem, path
    for (auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::string ext = squash(it->path().extension().string());
            if (ext == "ttf" || ext == "otf") files.push_back({squash(it->path().stem().string()), it->path().string()});
        }
    }
    std::sort(files.begin(), files.end());
    auto pick = [&](const std::string& want) -> std::string {
        if (want.empty()) return {};
        for (auto& f : files) if (f.first == want || f.first == want + "regular") return f.second;
        for (auto& f : files) if (f.first.rfind(want, 0) == 0) return f.second;
        return {};
    };
    std::string want = squash(family);
    if (auto f = pick(want); !f.empty()) return f;
    static const std::pair<const char*, const char*> alias[] = {
        {"arial", "liberationsans"}, {"helvetica", "liberationsans"}, {"segoeui", "dejavusans"},
        {"timesnewroman", "liberationserif"}, {"times", "liberationserif"}, {"couriernew", "liberationmono"},
        {"consolas", "dejavusansmono"}, {"calibri", "carlito"}, {"cambria", "caladea"}};
    for (auto& [w, l] : alias)
        if (want == w)
            if (auto f = pick(l); !f.empty()) return f;
    for (const char* f : {"dejavusans", "liberationsans", "notosans", "freesans"})
        if (auto p = pick(f); !p.empty()) return p;
    return files.empty() ? std::string() : files.front().second;
}

struct OutlineCtx {
    std::vector<std::vector<Vec2>>* loops;
    std::vector<Vec2> cur;
    void flush() {
        if (cur.size() >= 3) {
            if (glm::length(cur.back() - cur.front()) > 1e-4) cur.push_back(cur.front());
            loops->push_back(std::move(cur));
        }
        cur.clear();
    }
};
Vec2 ftv(const FT_Vector* v) { return {v->x / 64.0, v->y / 64.0}; }
int ftMove(const FT_Vector* to, void* u) { auto* c = (OutlineCtx*)u; c->flush(); c->cur.push_back(ftv(to)); return 0; }
int ftLine(const FT_Vector* to, void* u) { ((OutlineCtx*)u)->cur.push_back(ftv(to)); return 0; }
int ftConic(const FT_Vector* k, const FT_Vector* to, void* u) {
    auto* c = (OutlineCtx*)u;
    if (c->cur.empty()) return 1;
    sampleQuad(c->cur, c->cur.back(), ftv(k), ftv(to));
    return 0;
}
int ftCubic(const FT_Vector* k1, const FT_Vector* k2, const FT_Vector* to, void* u) {
    auto* c = (OutlineCtx*)u;
    if (c->cur.empty()) return 1;
    sampleCubic(c->cur, c->cur.back(), ftv(k1), ftv(k2), ftv(to));
    return 0;
}

std::u32string utf8to32(const std::string& s) {
    std::u32string r;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        r.push_back(cp);
        i += n;
    }
    return r;
}

std::vector<std::vector<Vec2>> fontLoops(const std::string& font, const std::string& text, double height) {
    std::vector<std::vector<Vec2>> loops;
    if (font.empty() || text.empty() || !(height > 0)) return loops;
    std::string file = findFontFile(font);
    if (file.empty()) return loops;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) return loops;
    FT_Face face = nullptr;
    if (FT_New_Face(lib, file.c_str(), 0, &face)) { FT_Done_FreeType(lib); return loops; }
    const int em = 1000;
    FT_Set_Char_Size(face, 0, em * 64, 72, 72);
    double scale = height / (double)em;
    double pen = 0;
    bool any = false;
    FT_Outline_Funcs fn{};
    fn.move_to = ftMove;
    fn.line_to = ftLine;
    fn.conic_to = ftConic;
    fn.cubic_to = ftCubic;
    for (char32_t ch : utf8to32(text)) {
        if (FT_Load_Char(face, ch, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)) continue;
        any = true;
        std::vector<std::vector<Vec2>> glyph;
        OutlineCtx ctx{&glyph, {}};
        if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline_Decompose(&face->glyph->outline, &fn, &ctx);
            ctx.flush();
        }
        for (auto& loop : glyph) {
            for (auto& p : loop) p = Vec2{pen + p.x * scale, p.y * scale};
            loops.push_back(std::move(loop));
        }
        pen += face->glyph->advance.x / 64.0 * scale;
    }
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    if (!any) loops.clear();
    return loops;
}

#endif

std::vector<uint32_t> addLoops(Sketch& s, const std::vector<std::vector<Vec2>>& loops, Vec2 origin, bool construction) {
    std::vector<uint32_t> lines;
    std::map<std::pair<long long, long long>, uint32_t> pts;
    auto point = [&](Vec2 p) {
        p += origin;
        auto k = std::make_pair((long long)std::llround(p.x * 1e6), (long long)std::llround(p.y * 1e6));
        auto it = pts.find(k);
        if (it != pts.end()) return it->second;
        uint32_t id = s.addPoint(p);
        pts[k] = id;
        return id;
    };
    for (const auto& loop : loops) {
        if (loop.size() < 3) continue;
        uint32_t first = point(loop.front());
        uint32_t prev = first;
        for (size_t i = 1; i < loop.size(); ++i) {
            uint32_t id = point(loop[i]);
            if (id == prev) continue;
            lines.push_back(s.addLine(prev, id, construction));
            prev = id;
        }
        if (prev != first) lines.push_back(s.addLine(prev, first, construction));
    }
    return lines;
}

} // namespace

std::vector<std::vector<Vec2>> textOutlines(const std::string& font, const std::string& text, double height) {
    return fontLoops(font, text, height);
}

std::vector<uint32_t> addSketchText(Sketch& s, const std::string& text, Vec2 origin, double height, bool construction,
                                    const std::string& font, uint32_t* noteId) {
    if (noteId) *noteId = 0;
    if (!(height > 1e-9) || text.empty()) return {};
    if (!font.empty()) {
        auto loops = fontLoops(font, text, height);
        auto lines = addLoops(s, loops, origin, construction);
        if (lines.empty()) return {};
        Sketch::TextNote n;
        n.id = s.nextNote++;
        n.text = text;
        n.font = font;
        n.height = height;
        n.origin = origin;
        n.construction = construction;
        n.ents = lines;
        s.notes.push_back(std::move(n));
        if (noteId) *noteId = s.notes.back().id;
        return lines;
    }
    std::vector<uint32_t> lines;
    std::map<std::pair<long long, long long>, uint32_t> pts;
    auto point = [&](Vec2 p) {
        auto k = std::make_pair((long long)std::llround(p.x * 1e6), (long long)std::llround(p.y * 1e6));
        auto it = pts.find(k);
        if (it != pts.end()) return it->second;
        uint32_t id = s.addPoint(p);
        pts[k] = id;
        return id;
    };
    double x = origin.x;
    for (char ch : text) {
        Glyph g = glyphOf(ch);
        if (g.boxes.empty() && g.adv <= 0) continue;
        for (const Box& box : g.boxes) {
            Vec2 p0{x + box.x0 * height, origin.y + box.y0 * height};
            Vec2 p1{x + box.x1 * height, origin.y + box.y0 * height};
            Vec2 p2{x + box.x1 * height, origin.y + box.y1 * height};
            Vec2 p3{x + box.x0 * height, origin.y + box.y1 * height};
            uint32_t a = point(p0), b = point(p1), c = point(p2), d = point(p3);
            lines.push_back(s.addLine(a, b, construction));
            lines.push_back(s.addLine(b, c, construction));
            lines.push_back(s.addLine(c, d, construction));
            lines.push_back(s.addLine(d, a, construction));
        }
        x += g.adv * height;
    }
    return lines;
}

bool editSketchText(Sketch& s, uint32_t noteId, const std::string& text, double height, const std::string& font) {
    if (!(height > 1e-9) || text.empty() || font.empty()) return false;
    Sketch::TextNote* n = nullptr;
    for (auto& note : s.notes)
        if (note.id == noteId) n = &note;
    if (!n) return false;
    auto loops = fontLoops(font, text, height);
    auto lines = addLoops(s, loops, n->origin, n->construction);
    if (lines.empty()) return false;
    std::set<uint32_t> old(n->ents.begin(), n->ents.end());
    s.deleteEntities(old);
    n->text = text;
    n->font = font;
    n->height = height;
    n->ents = lines;
    return true;
}

} // namespace kst::sk
