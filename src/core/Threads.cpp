#include "core/Threads.h"

#include <algorithm>
#include <cmath>

namespace kst::threads {
namespace {

const double kPi = 3.14159265358979323846;
const double kIn = 25.4;

Size mm(const char* n, double d, double p, int starts = 1) { return {n, d, p, starts}; }
Size tpi(const char* n, double dIn, double t) { return {n, dIn * kIn, kIn / t, 1}; }

// Symmetric V form truncated equally at crest and root to a working depth h (x pitch).
void vForm(Family& f, double halfAngle, double h) {
    double H = 1.0 / (2.0 * std::tan(halfAngle * kPi / 180.0));
    double flat = std::max(0.02, (H - h) * std::tan(halfAngle * kPi / 180.0));
    f.angleL = f.angleR = halfAngle;
    f.hExt = f.hInt = h;
    f.flatExt = f.flatInt = flat;
}

std::vector<Family> build() {
    std::vector<Family> out;
    auto iso = [](Family f) {
        f.angleL = f.angleR = 30;
        f.hExt = 0.6134; f.flatExt = 0.125;   // d3 = d - 1.22687 P, crest flat P/8
        f.hInt = 0.5413; f.flatInt = 0.25;    // D1 = D - 1.0825 P, crest flat P/4
        return f;
    };
    {
        Family f = iso({});
        f.name = "ISO Metric (coarse)";
        f.note = "ISO 261 / 262, 60 deg";
        f.sizes = {mm("M1", 1, 0.25), mm("M1.2", 1.2, 0.25), mm("M1.4", 1.4, 0.3), mm("M1.6", 1.6, 0.35), mm("M2", 2, 0.4),
                   mm("M2.5", 2.5, 0.45), mm("M3", 3, 0.5), mm("M3.5", 3.5, 0.6), mm("M4", 4, 0.7), mm("M5", 5, 0.8),
                   mm("M6", 6, 1), mm("M7", 7, 1), mm("M8", 8, 1.25), mm("M10", 10, 1.5), mm("M12", 12, 1.75),
                   mm("M14", 14, 2), mm("M16", 16, 2), mm("M18", 18, 2.5), mm("M20", 20, 2.5), mm("M22", 22, 2.5),
                   mm("M24", 24, 3), mm("M27", 27, 3), mm("M30", 30, 3.5), mm("M33", 33, 3.5), mm("M36", 36, 4),
                   mm("M39", 39, 4), mm("M42", 42, 4.5), mm("M45", 45, 4.5), mm("M48", 48, 5), mm("M52", 52, 5),
                   mm("M56", 56, 5.5), mm("M60", 60, 5.5), mm("M64", 64, 6)};
        out.push_back(f);
    }
    {
        Family f = iso({});
        f.name = "ISO Metric (fine)";
        f.note = "ISO 261 fine pitches, 60 deg";
        f.sizes = {mm("M3x0.35", 3, 0.35), mm("M4x0.5", 4, 0.5), mm("M5x0.5", 5, 0.5), mm("M6x0.75", 6, 0.75),
                   mm("M8x1", 8, 1), mm("M10x1", 10, 1), mm("M10x1.25", 10, 1.25), mm("M12x1.25", 12, 1.25),
                   mm("M12x1.5", 12, 1.5), mm("M14x1.5", 14, 1.5), mm("M16x1.5", 16, 1.5), mm("M18x1.5", 18, 1.5),
                   mm("M20x1.5", 20, 1.5), mm("M20x2", 20, 2), mm("M22x1.5", 22, 1.5), mm("M24x1.5", 24, 1.5),
                   mm("M24x2", 24, 2), mm("M27x2", 27, 2), mm("M30x2", 30, 2), mm("M33x2", 33, 2), mm("M36x3", 36, 3),
                   mm("M39x3", 39, 3), mm("M42x3", 42, 3), mm("M48x3", 48, 3), mm("M56x4", 56, 4), mm("M64x4", 64, 4)};
        out.push_back(f);
    }
    {
        Family f = iso({});
        f.name = "Unified UNC";
        f.note = "ASME B1.1 coarse, 60 deg";
        f.inch = true;
        f.sizes = {tpi("#1-64", 0.073, 64), tpi("#2-56", 0.086, 56), tpi("#3-48", 0.099, 48), tpi("#4-40", 0.112, 40),
                   tpi("#5-40", 0.125, 40), tpi("#6-32", 0.138, 32), tpi("#8-32", 0.164, 32), tpi("#10-24", 0.190, 24),
                   tpi("#12-24", 0.216, 24), tpi("1/4-20", 0.25, 20), tpi("5/16-18", 0.3125, 18), tpi("3/8-16", 0.375, 16),
                   tpi("7/16-14", 0.4375, 14), tpi("1/2-13", 0.5, 13), tpi("9/16-12", 0.5625, 12), tpi("5/8-11", 0.625, 11),
                   tpi("3/4-10", 0.75, 10), tpi("7/8-9", 0.875, 9), tpi("1-8", 1.0, 8), tpi("1 1/8-7", 1.125, 7),
                   tpi("1 1/4-7", 1.25, 7), tpi("1 3/8-6", 1.375, 6), tpi("1 1/2-6", 1.5, 6), tpi("1 3/4-5", 1.75, 5),
                   tpi("2-4.5", 2.0, 4.5)};
        out.push_back(f);
    }
    {
        Family f = iso({});
        f.name = "Unified UNF";
        f.note = "ASME B1.1 fine, 60 deg";
        f.inch = true;
        f.sizes = {tpi("#0-80", 0.060, 80), tpi("#1-72", 0.073, 72), tpi("#2-64", 0.086, 64), tpi("#3-56", 0.099, 56),
                   tpi("#4-48", 0.112, 48), tpi("#5-44", 0.125, 44), tpi("#6-40", 0.138, 40), tpi("#8-36", 0.164, 36),
                   tpi("#10-32", 0.190, 32), tpi("#12-28", 0.216, 28), tpi("1/4-28", 0.25, 28), tpi("5/16-24", 0.3125, 24),
                   tpi("3/8-24", 0.375, 24), tpi("7/16-20", 0.4375, 20), tpi("1/2-20", 0.5, 20), tpi("9/16-18", 0.5625, 18),
                   tpi("5/8-18", 0.625, 18), tpi("3/4-16", 0.75, 16), tpi("7/8-14", 0.875, 14), tpi("1-12", 1.0, 12),
                   tpi("1 1/8-12", 1.125, 12), tpi("1 1/4-12", 1.25, 12), tpi("1 3/8-12", 1.375, 12), tpi("1 1/2-12", 1.5, 12)};
        out.push_back(f);
    }
    {
        Family f = iso({});
        f.name = "Unified UNEF";
        f.note = "ASME B1.1 extra fine, 60 deg";
        f.inch = true;
        f.sizes = {tpi("#12-32", 0.216, 32), tpi("1/4-32", 0.25, 32), tpi("5/16-32", 0.3125, 32), tpi("3/8-32", 0.375, 32),
                   tpi("7/16-28", 0.4375, 28), tpi("1/2-28", 0.5, 28), tpi("9/16-24", 0.5625, 24), tpi("5/8-24", 0.625, 24),
                   tpi("3/4-20", 0.75, 20), tpi("7/8-20", 0.875, 20), tpi("1-20", 1.0, 20)};
        out.push_back(f);
    }
    {
        Family f;
        vForm(f, 27.5, 0.6403);
        f.name = "Whitworth BSW";
        f.note = "BS 84 coarse, 55 deg";
        f.inch = true;
        f.sizes = {tpi("1/8-40", 0.125, 40), tpi("3/16-24", 0.1875, 24), tpi("1/4-20", 0.25, 20), tpi("5/16-18", 0.3125, 18),
                   tpi("3/8-16", 0.375, 16), tpi("7/16-14", 0.4375, 14), tpi("1/2-12", 0.5, 12), tpi("5/8-11", 0.625, 11),
                   tpi("3/4-10", 0.75, 10), tpi("7/8-9", 0.875, 9), tpi("1-8", 1.0, 8), tpi("1 1/4-7", 1.25, 7),
                   tpi("1 1/2-6", 1.5, 6), tpi("2-4.5", 2.0, 4.5)};
        out.push_back(f);
        f.name = "Whitworth BSF";
        f.note = "BS 84 fine, 55 deg";
        f.sizes = {tpi("3/16-32", 0.1875, 32), tpi("1/4-26", 0.25, 26), tpi("5/16-22", 0.3125, 22), tpi("3/8-20", 0.375, 20),
                   tpi("7/16-18", 0.4375, 18), tpi("1/2-16", 0.5, 16), tpi("5/8-14", 0.625, 14), tpi("3/4-12", 0.75, 12),
                   tpi("7/8-11", 0.875, 11), tpi("1-10", 1.0, 10)};
        out.push_back(f);
        std::vector<Size> pipe = {mm("1/16", 7.723, kIn / 28), mm("1/8", 9.728, kIn / 28), mm("1/4", 13.157, kIn / 19),
                                  mm("3/8", 16.662, kIn / 19), mm("1/2", 20.955, kIn / 14), mm("5/8", 22.911, kIn / 14),
                                  mm("3/4", 26.441, kIn / 14), mm("7/8", 30.201, kIn / 14), mm("1", 33.249, kIn / 11),
                                  mm("1 1/4", 41.910, kIn / 11), mm("1 1/2", 47.803, kIn / 11), mm("2", 59.614, kIn / 11),
                                  mm("2 1/2", 75.184, kIn / 11), mm("3", 87.884, kIn / 11), mm("4", 113.030, kIn / 11)};
        f.name = "BSPP pipe (G)";
        f.note = "ISO 228 parallel pipe, 55 deg";
        f.sizes = pipe;
        out.push_back(f);
        f.name = "BSPT pipe (R)";
        f.note = "ISO 7 taper 1:16, 55 deg";
        f.taperDeg = std::atan(1.0 / 32.0) * 180.0 / kPi;
        f.sizes = pipe;
        out.push_back(f);
    }
    {
        Family f;
        vForm(f, 30, 0.8);
        f.inch = true;
        std::vector<Size> pipe = {tpi("1/16", 0.3125, 27), tpi("1/8", 0.405, 27), tpi("1/4", 0.540, 18), tpi("3/8", 0.675, 18),
                                  tpi("1/2", 0.840, 14), tpi("3/4", 1.050, 14), tpi("1", 1.315, 11.5), tpi("1 1/4", 1.660, 11.5),
                                  tpi("1 1/2", 1.900, 11.5), tpi("2", 2.375, 11.5), tpi("2 1/2", 2.875, 8), tpi("3", 3.5, 8),
                                  tpi("4", 4.5, 8)};
        f.name = "NPT pipe";
        f.note = "ASME B1.20.1 taper 1:16, 60 deg";
        f.taperDeg = std::atan(1.0 / 32.0) * 180.0 / kPi;
        f.sizes = pipe;
        out.push_back(f);
        f.name = "NPS pipe (straight)";
        f.note = "ASME B1.20.1 straight, 60 deg";
        f.taperDeg = 0;
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = f.angleR = 14.5;
        f.hExt = f.hInt = 0.5;
        f.flatExt = f.flatInt = 0.3707;
        f.name = "ACME";
        f.note = "ASME B1.5 general purpose, 29 deg";
        f.inch = true;
        f.sizes = {tpi("1/4-16", 0.25, 16), tpi("5/16-14", 0.3125, 14), tpi("3/8-12", 0.375, 12), tpi("7/16-12", 0.4375, 12),
                   tpi("1/2-10", 0.5, 10), tpi("5/8-8", 0.625, 8), tpi("3/4-6", 0.75, 6), tpi("7/8-6", 0.875, 6),
                   tpi("1-5", 1.0, 5), tpi("1 1/4-5", 1.25, 5), tpi("1 1/2-4", 1.5, 4), tpi("1 3/4-4", 1.75, 4),
                   tpi("2-4", 2.0, 4), tpi("2 1/2-3", 2.5, 3), tpi("3-2", 3.0, 2)};
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = f.angleR = 15;
        f.hExt = f.hInt = 0.5;
        f.flatExt = f.flatInt = 0.366;
        f.trapAllowance = true;
        f.name = "Trapezoidal (Tr)";
        f.note = "ISO 2904 / DIN 103, 30 deg. Lead screws too";
        f.sizes = {mm("Tr8x1.5", 8, 1.5), mm("Tr8x2", 8, 2), mm("Tr8x8 (4 start)", 8, 2, 4), mm("Tr10x2", 10, 2),
                   mm("Tr10x3", 10, 3), mm("Tr12x3", 12, 3), mm("Tr14x3", 14, 3), mm("Tr14x4", 14, 4), mm("Tr16x4", 16, 4),
                   mm("Tr18x4", 18, 4), mm("Tr20x4", 20, 4), mm("Tr22x5", 22, 5), mm("Tr24x5", 24, 5), mm("Tr26x5", 26, 5),
                   mm("Tr28x5", 28, 5), mm("Tr30x6", 30, 6), mm("Tr32x6", 32, 6), mm("Tr36x6", 36, 6), mm("Tr40x7", 40, 7),
                   mm("Tr44x7", 44, 7), mm("Tr48x8", 48, 8), mm("Tr50x8", 50, 8), mm("Tr52x8", 52, 8), mm("Tr60x9", 60, 9),
                   mm("Tr70x10", 70, 10), mm("Tr80x10", 80, 10), mm("Tr100x12", 100, 12)};
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = 7; f.angleR = 45;
        f.hExt = f.hInt = 0.6;
        f.flatExt = f.flatInt = 0.163;
        f.name = "Buttress (ANSI 7/45)";
        f.note = "ASME B1.9, load flank 7 deg";
        f.inch = true;
        f.sizes = {tpi("1/2-16", 0.5, 16), tpi("3/4-16", 0.75, 16), tpi("1-12", 1.0, 12), tpi("1 1/4-12", 1.25, 12),
                   tpi("1 1/2-10", 1.5, 10), tpi("2-8", 2.0, 8), tpi("2 1/2-8", 2.5, 8), tpi("3-6", 3.0, 6), tpi("4-6", 4.0, 6)};
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = 3; f.angleR = 30;
        f.hExt = 0.8678; f.flatExt = 0.2636;
        f.hInt = 0.75; f.flatInt = 0.2636;
        f.name = "Buttress (DIN 513 S)";
        f.note = "DIN 513 saw tooth, 3 / 30 deg";
        f.sizes = {mm("S10x2", 10, 2), mm("S12x3", 12, 3), mm("S16x4", 16, 4), mm("S20x4", 20, 4), mm("S24x5", 24, 5),
                   mm("S28x5", 28, 5), mm("S32x6", 32, 6), mm("S36x6", 36, 6), mm("S40x7", 40, 7), mm("S44x7", 44, 7),
                   mm("S48x8", 48, 8), mm("S52x8", 52, 8), mm("S60x9", 60, 9), mm("S70x10", 70, 10), mm("S80x10", 80, 10),
                   mm("S100x12", 100, 12)};
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = f.angleR = 0;
        f.hExt = f.hInt = 0.5;
        f.flatExt = f.flatInt = 0.5;
        f.name = "Square";
        f.note = "Square power screw";
        f.sizes = {mm("Sq10x2", 10, 2), mm("Sq12x3", 12, 3), mm("Sq16x4", 16, 4), mm("Sq20x4", 20, 4), mm("Sq24x5", 24, 5),
                   mm("Sq30x6", 30, 6), mm("Sq36x6", 36, 6), mm("Sq40x7", 40, 7), mm("Sq50x8", 50, 8), mm("Sq60x9", 60, 9)};
        out.push_back(f);
    }
    {
        Family f;
        f.angleL = f.angleR = 15;
        f.hExt = f.hInt = 0.5;
        f.flatExt = f.flatInt = 0.04;
        f.round = true;
        f.name = "Knuckle / round (Rd)";
        f.note = "DIN 405 round thread, 30 deg";
        const double p8 = kIn / 8, p6 = kIn / 6, p4 = kIn / 4;
        f.sizes = {mm("Rd8x1/10\"", 8, kIn / 10), mm("Rd10x1/8\"", 10, p8), mm("Rd12x1/8\"", 12, p8), mm("Rd16x1/8\"", 16, p8),
                   mm("Rd20x1/8\"", 20, p8), mm("Rd24x1/8\"", 24, p8), mm("Rd30x1/8\"", 30, p8), mm("Rd36x1/8\"", 36, p8),
                   mm("Rd40x1/6\"", 40, p6), mm("Rd50x1/6\"", 50, p6), mm("Rd60x1/6\"", 60, p6), mm("Rd80x1/6\"", 80, p6),
                   mm("Rd100x1/6\"", 100, p6), mm("Rd120x1/4\"", 120, p4)};
        out.push_back(f);
    }
    {
        Family f;
        vForm(f, 23.75, 0.6);
        f.name = "British Association (BA)";
        f.note = "BS 93, 47.5 deg";
        f.sizes = {mm("0BA", 6.0, 1.0), mm("1BA", 5.3, 0.9), mm("2BA", 4.7, 0.81), mm("3BA", 4.1, 0.73), mm("4BA", 3.6, 0.66),
                   mm("5BA", 3.2, 0.59), mm("6BA", 2.8, 0.53), mm("7BA", 2.5, 0.48), mm("8BA", 2.2, 0.43), mm("10BA", 1.7, 0.35)};
        out.push_back(f);
    }
    {
        Family f;
        vForm(f, 40, 0.4);
        f.name = "Conduit (Pg)";
        f.note = "DIN 40430 steel conduit, 80 deg";
        f.sizes = {mm("Pg7", 12.5, 1.27), mm("Pg9", 15.2, 1.41), mm("Pg11", 18.6, 1.41), mm("Pg13.5", 20.4, 1.41),
                   mm("Pg16", 22.5, 1.41), mm("Pg21", 28.3, 1.588), mm("Pg29", 37.0, 1.588), mm("Pg36", 47.0, 1.588),
                   mm("Pg42", 54.0, 1.588), mm("Pg48", 59.3, 1.588)};
        out.push_back(f);
    }
    return out;
}

double depth(const Family& f, double pitch, bool internal) {
    double h = (internal ? f.hInt : f.hExt) * pitch;
    if (f.trapAllowance) h += pitch <= 1.5 ? 0.15 : pitch <= 5 ? 0.25 : pitch <= 12 ? 0.5 : 1.0;
    return h;
}

} // namespace

const std::vector<Family>& families() {
    static const std::vector<Family> all = build();
    return all;
}

double minorDia(const Family& f, double major, double pitch) {
    double h = f.hInt * pitch;
    return major - 2.0 * h;
}

int closestSize(const Family& f, double faceDia, bool internal) {
    int best = -1;
    double bestErr = 1e18;
    for (size_t i = 0; i < f.sizes.size(); ++i) {
        double d = internal ? minorDia(f, f.sizes[i].major, f.sizes[i].pitch) : f.sizes[i].major;
        double e = std::abs(d - faceDia);
        if (e < bestErr) { bestErr = e; best = (int)i; }
    }
    return best;
}

std::vector<glm::dvec2> groove(const Family& f, double major, double pitch, bool internal, double faceRadius) {
    const double P = pitch;
    const double R = major * 0.5;
    const double e = 0.1 * P + 0.05;
    const double tl = std::tan(f.angleL * kPi / 180.0), tr = std::tan(f.angleR * kPi / 180.0);
    const double h = depth(f, P, internal);
    std::vector<glm::dvec2> pts;
    if (f.round) {
        // Rounded form: a smooth bell from a near point crest to a near point root.
        const int n = 10;
        double wo = 0.96 * P;
        double rOut = internal ? R - f.hInt * P : R;   // wide end of the groove
        double dir = internal ? 1.0 : -1.0;      // toward the narrow end
        double ext = internal ? std::min(rOut, faceRadius) - e : std::max(rOut, faceRadius) + e;
        pts.push_back({ext, -wo / 2});
        for (int i = 0; i <= n; ++i) {
            double u = (double)i / n;
            double hw = std::max(0.02 * P, wo / 2 * (0.5 + 0.5 * std::cos(kPi * u)));
            pts.push_back({rOut + dir * u * h, -hw});
        }
        for (int i = n; i >= 0; --i) {
            double u = (double)i / n;
            double hw = std::max(0.02 * P, wo / 2 * (0.5 + 0.5 * std::cos(kPi * u)));
            pts.push_back({rOut + dir * u * h, hw});
        }
        pts.push_back({ext, wo / 2});
        return pts;
    }
    double flat = internal ? f.flatInt : f.flatExt;
    double wo = std::min(P * (1.0 - flat), 0.98 * P);   // groove width at its wide end
    double lb = -wo / 2 + h * tl, rb = wo / 2 - h * tr; // narrow end
    if (rb - lb < 0.02 * P) {
        double mid = 0.5 * (lb + rb);
        lb = mid - 0.01 * P;
        rb = mid + 0.01 * P;
    }
    // The flank corners sit a hair off the nominal diameter: a corner exactly on the face would sweep
    // a helix lying on it, which booleans handle badly.
    const double k = 0.02 * P;
    if (!internal) {
        double top = std::max(R, faceRadius) + e, ri = R - h, rk = R + k;
        double l0 = -wo / 2 - k * tl, r0 = wo / 2 + k * tr;
        pts = {{top, l0}, {rk, l0}, {ri, lb}, {ri, rb}, {rk, r0}, {top, r0}};
    } else {
        double rm = R - f.hInt * P, bot = std::min(rm, faceRadius) - e, rk = rm - k;
        double l0 = -wo / 2 - k * tl, r0 = wo / 2 + k * tr;
        pts = {{bot, l0}, {rk, l0}, {rm + h, lb}, {rm + h, rb}, {rk, r0}, {bot, r0}};
    }
    return pts;
}

} // namespace kst::threads
