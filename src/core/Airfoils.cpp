#include "core/Airfoils.h"
#include "core/Types.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kst::airfoils {
namespace {

const double kPi = 3.14159265358979323846;

const char* kPresets[] = {"NACA 0009", "NACA 0012", "NACA 0015", "NACA 0018", "NACA 2412", "NACA 2415", "NACA 4412",
                          "NACA 4415", "NACA 6412", "NACA 23012", "NACA 23015", "NACA 23018", "Clark Y"};

// Clark Y (percent chord), the classic table.
const double kClarkX[] = {0, 1.25, 2.5, 5, 7.5, 10, 15, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};
const double kClarkU[] = {3.5, 5.45, 6.5, 7.9, 8.85, 9.6, 10.68, 11.36, 11.7, 11.4, 10.52, 9.15, 7.35, 5.22, 2.8, 1.49, 0.12};
const double kClarkL[] = {3.5, 1.93, 1.47, 0.93, 0.63, 0.42, 0.15, 0.03, 0, 0, 0, 0, 0, 0, 0, 0, 0};

struct Surfaces { std::vector<glm::dvec2> upper, lower; };   // both from the leading edge to the trailing edge

// NACA 4 digit (MPXX) or 5 digit (LPQXX, Q = 0) at cosine spaced stations.
Surfaces naca(const std::string& digits, int n) {
    Surfaces s;
    double t = std::stoi(digits.substr(digits.size() - 2)) / 100.0;
    if (t <= 0) throw std::runtime_error("NACA " + digits + " has zero thickness");
    bool five = digits.size() == 5;
    double m = 0, p = 0, k1 = 0, r = 0, scale = 1;
    if (five) {
        int L = digits[0] - '0', P = digits[1] - '0', Q = digits[2] - '0';
        if (Q != 0) throw std::runtime_error("Reflexed NACA 5 digit sections are not supported yet");
        static const double rs[] = {0, 0.0580, 0.1260, 0.2025, 0.2900, 0.3910};
        static const double ks[] = {0, 361.4, 51.64, 15.957, 6.643, 3.230};
        if (P < 1 || P > 5) throw std::runtime_error("NACA " + digits + ": the second digit must be 1 to 5");
        r = rs[P];
        k1 = ks[P];
        scale = L / 2.0;   // tables are for a design lift of 0.3 (L = 2)
    } else {
        m = (digits[0] - '0') / 100.0;
        p = (digits[1] - '0') / 10.0;
    }
    for (int i = 0; i < n; ++i) {
        double x = 0.5 * (1 - std::cos(kPi * i / (n - 1)));
        double yt = 5 * t * (0.2969 * std::sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1015 * x * x * x * x);
        double yc = 0, dy = 0;
        if (five) {
            if (x < r) {
                yc = k1 / 6 * (x * x * x - 3 * r * x * x + r * r * (3 - r) * x);
                dy = k1 / 6 * (3 * x * x - 6 * r * x + r * r * (3 - r));
            } else {
                yc = k1 * r * r * r / 6 * (1 - x);
                dy = -k1 * r * r * r / 6;
            }
            yc *= scale;
            dy *= scale;
        } else if (m > 0 && p > 0) {
            if (x < p) {
                yc = m / (p * p) * (2 * p * x - x * x);
                dy = 2 * m / (p * p) * (p - x);
            } else {
                yc = m / ((1 - p) * (1 - p)) * ((1 - 2 * p) + 2 * p * x - x * x);
                dy = 2 * m / ((1 - p) * (1 - p)) * (p - x);
            }
        }
        double th = std::atan(dy);
        s.upper.push_back({x - yt * std::sin(th), yc + yt * std::cos(th)});
        s.lower.push_back({x + yt * std::sin(th), yc - yt * std::cos(th)});
    }
    return s;
}

// Even (cosine) spacing along each surface by arc length, so every airfoil has the same point count.
std::vector<glm::dvec2> resample(const std::vector<glm::dvec2>& pts, int n) {
    if (pts.size() < 2) throw std::runtime_error("An airfoil surface has too few points");
    std::vector<double> s(pts.size(), 0.0);
    for (size_t i = 1; i < pts.size(); ++i) s[i] = s[i - 1] + glm::length(pts[i] - pts[i - 1]);
    double total = s.back();
    if (total <= 0) throw std::runtime_error("An airfoil surface has no length");
    std::vector<glm::dvec2> out;
    size_t j = 0;
    for (int i = 0; i < n; ++i) {
        double want = total * 0.5 * (1 - std::cos(kPi * i / (n - 1)));
        while (j + 2 < pts.size() && s[j + 1] < want) ++j;
        double span = s[j + 1] - s[j];
        double f = span > 0 ? std::clamp((want - s[j]) / span, 0.0, 1.0) : 0.0;
        out.push_back(pts[j] + (pts[j + 1] - pts[j]) * f);
    }
    return out;
}

Surfaces splitOutline(std::vector<glm::dvec2> pts) {
    // Normalize: leading edge (min x) at 0, chord 1.
    double xmin = 1e18, xmax = -1e18;
    size_t le = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].x < xmin) { xmin = pts[i].x; le = i; }
        xmax = std::max(xmax, pts[i].x);
    }
    double c = xmax - xmin;
    if (c <= 0) throw std::runtime_error("The airfoil has no chord");
    glm::dvec2 o = pts[le];
    for (auto& p : pts) p = (p - glm::dvec2(xmin, o.y)) / c;
    Surfaces s;
    for (size_t i = le + 1; i-- > 0;) s.upper.push_back(pts[i]);
    for (size_t i = le; i < pts.size(); ++i) s.lower.push_back(pts[i]);
    // Selig files run upper first; flip if this one ran lower first.
    double au = 0, al = 0;
    for (auto& p : s.upper) au += p.y;
    for (auto& p : s.lower) al += p.y;
    if (au / s.upper.size() < al / s.lower.size()) std::swap(s.upper, s.lower);
    return s;
}

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

} // namespace

std::vector<std::string> library(const std::string& folder) {
    std::vector<std::string> out(std::begin(kPresets), std::end(kPresets));
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(u8path(folder), ec)) {
        std::vector<std::string> files;
        for (auto& e : std::filesystem::directory_iterator(u8path(folder), ec))
            if (e.is_regular_file(ec) && toLower(pathToU8(e.path().extension())) == ".dat") files.push_back(pathToU8(e.path().stem()));
        std::sort(files.begin(), files.end());
        out.insert(out.end(), files.begin(), files.end());
    }
    return out;
}

std::vector<glm::dvec2> readDat(const std::string& path) {
    std::ifstream f(u8path(path));
    if (!f) throw std::runtime_error("Could not open " + path);
    std::vector<glm::dvec2> raw;
    std::vector<int> blockStart;   // Lednicer: a blank line between the surfaces
    bool lednicer = false, first = true, gap = false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        double a, b;
        if (!(ss >> a >> b)) {
            if (!raw.empty()) gap = true;
            continue;
        }
        if (first) {
            first = false;
            if (a > 1.5 && b > 1.5) { lednicer = true; continue; }   // point counts line
        }
        if (gap || raw.empty()) blockStart.push_back((int)raw.size());
        gap = false;
        raw.push_back({a, b});
    }
    if (raw.size() < 6) throw std::runtime_error("No airfoil coordinates found in " + path);
    if (lednicer && blockStart.size() >= 2) {
        // Upper LE->TE, then lower LE->TE: make it one TE->LE->TE run.
        std::vector<glm::dvec2> up(raw.begin(), raw.begin() + blockStart[1]), lo(raw.begin() + blockStart[1], raw.end());
        std::vector<glm::dvec2> out(up.rbegin(), up.rend());
        out.insert(out.end(), lo.begin() + (glm::length(lo.front() - up.front()) < 1e-9 ? 1 : 0), lo.end());
        return out;
    }
    return raw;
}

std::vector<glm::dvec2> outline(const std::string& name0, const std::string& folder, int n) {
    std::string name = name0;
    while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
    while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
    std::string low = toLower(name);
    Surfaces s;
    std::string digits;
    if (low.rfind("naca", 0) == 0) {
        for (char c : name.substr(4))
            if (!std::isspace((unsigned char)c) && c != '-') digits += c;
    }
    if (isDigits(digits) && (digits.size() == 4 || digits.size() == 5)) {
        s = naca(digits, std::max(n, 80));
    } else if (low == "clark y" || low == "clarky") {
        for (int i = 0; i < 17; ++i) {
            s.upper.push_back({kClarkX[i] / 100, kClarkU[i] / 100});
            s.lower.push_back({kClarkX[i] / 100, kClarkL[i] / 100});
        }
        // Rounder nose than straight segments: add a point just behind the leading edge.
        s.upper.insert(s.upper.begin() + 1, {0.003, 0.046});
        s.lower.insert(s.lower.begin() + 1, {0.003, 0.025});
    } else {
        std::string path = folder + "/" + name + ".dat";
        std::error_code ec;
        if (folder.empty() || !std::filesystem::exists(u8path(path), ec))
            throw std::runtime_error("Unknown airfoil \"" + name + "\". Type NACA and 4 or 5 digits, or import a .dat file.");
        s = splitOutline(readDat(path));
    }
    std::vector<glm::dvec2> up = resample(s.upper, n), lo = resample(s.lower, n);
    std::vector<glm::dvec2> out(up.rbegin(), up.rend());
    out.insert(out.end(), lo.begin() + 1, lo.end());
    return out;
}

} // namespace kst::airfoils
