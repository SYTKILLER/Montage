#include "io/cms/synth.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "io/cms/builtin.h"

namespace montage {
namespace cms {

namespace {

constexpr double kYn = 2.0;  // Yule–Nielsen n

constexpr double kSeeds[5][3] = {
    {0.3, 0.3, 0.3}, {0.05, 0.05, 0.05}, {0.8, 0.8, 0.8}, {0.9, 0.2, 0.2}, {0.2, 0.9, 0.9},
};

// 有界最小二乘求 C/M/Y（给定 K）：最小化到 target 的 ΔE76，约束 C+M+Y ≤ limit
//（投影 Levenberg–Marquardt + 数值雅可比）。返回 (CMY, ΔE)。
std::pair<std::array<double, 3>, double> solveCmy(const CmykModel& model, const std::array<double, 3>& target,
                                                  double k, double limit,
                                                  const std::vector<std::array<double, 3>>& seeds) {
    auto project = [limit](std::array<double, 3> x) {
        for (int iter = 0; iter < 4; ++iter) {
            for (double& v : x) {
                v = std::min(std::max(v, 0.0), 1.0);
            }
            const double s = x[0] + x[1] + x[2];
            if (s <= limit + 1e-12) {
                break;
            }
            double active = 0.0;
            for (double v : x) {
                if (v > 0.0) {
                    active += 1.0;
                }
            }
            active = std::max(active, 1.0);
            const double d = (s - limit) / active;
            for (double& v : x) {
                if (v > 0.0) {
                    v -= d;
                }
            }
        }
        return x;
    };
    auto err = [&](const std::array<double, 3>& x) {
        const auto l = model.lab({x[0], x[1], x[2], k});
        return std::array<double, 3>{l[0] - target[0], l[1] - target[1], l[2] - target[2]};
    };
    auto norm2 = [](const std::array<double, 3>& e) { return e[0] * e[0] + e[1] * e[1] + e[2] * e[2]; };
    std::array<double, 3> bestX = {0.0, 0.0, 0.0};
    double bestF = std::numeric_limits<double>::infinity();
    for (const auto& seed : seeds) {
        std::array<double, 3> x = project(seed);
        std::array<double, 3> e = err(x);
        double f = norm2(e);
        double lambda = 1e-3;
        for (int iter = 0; iter < 60; ++iter) {
            // 前向差分雅可比（3×3）。
            const double h = 1e-5;
            double j[3][3];
            for (int c = 0; c < 3; ++c) {
                std::array<double, 3> xp = x;
                xp[c] += h;
                const auto ep = err(xp);
                for (int r = 0; r < 3; ++r) {
                    j[r][c] = (ep[r] - e[r]) / h;
                }
            }
            // (JᵀJ + λ diag) δ = −Jᵀe
            double a[3][3] = {{0}};
            double g[3] = {0, 0, 0};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    double sum = 0.0;
                    for (int kk = 0; kk < 3; ++kk) {
                        sum += j[kk][r] * j[kk][c];
                    }
                    a[r][c] = sum;
                }
                double sum = 0.0;
                for (int kk = 0; kk < 3; ++kk) {
                    sum += j[kk][r] * e[kk];
                }
                g[r] = -sum;
            }
            bool improved = false;
            for (int attempt = 0; attempt < 8; ++attempt) {
                Mat3 al{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        al[r][c] = a[r][c];
                    }
                    al[r][r] += lambda * (1.0 + a[r][r]);
                }
                const auto inv = invert(al);
                if (!inv) {
                    lambda *= 10.0;
                    continue;
                }
                const std::array<double, 3> step = apply(*inv, {g[0], g[1], g[2]});
                const auto xn = project({x[0] + step[0], x[1] + step[1], x[2] + step[2]});
                const auto en = err(xn);
                const double fnew = norm2(en);
                if (fnew < f) {
                    double moved = 0.0;
                    for (int i = 0; i < 3; ++i) {
                        moved = std::max(moved, std::abs(xn[i] - x[i]));
                    }
                    x = xn;
                    e = en;
                    f = fnew;
                    lambda = std::max(lambda * 0.3, 1e-9);
                    improved = moved > 1e-9;
                    break;
                }
                lambda *= 10.0;
            }
            if (!improved || f < 1e-10) {
                break;
            }
        }
        if (f < bestF) {
            bestX = x;
            bestF = f;
        }
        if (bestF < 1e-6) {
            break;
        }
    }
    return std::make_pair(bestX, std::sqrt(bestF));
}

// 色域边界描述子：每 (L*, 色相) 桶的最大 chroma（从模型采样）。
struct Gbd {
    size_t lBins = 41;
    size_t hBins = 72;
    std::vector<double> cmax;
    double blackL = 100.0;

    Gbd(const CmykModel& model, const CmykParams& p) : cmax(41 * 72, 0.0) {
        const int n = 21;
        for (int ci = 0; ci < n; ++ci) {
            for (int mi = 0; mi < n; ++mi) {
                for (int yi = 0; yi < n; ++yi) {
                    for (int ki = 0; ki < 11; ++ki) {
                        const std::array<double, 4> v = {ci / (n - 1.0), mi / (n - 1.0), yi / (n - 1.0),
                                                         ki / 10.0};
                        double sum = 0.0;
                        for (double x : v) {
                            sum += x;
                        }
                        if (sum > p.tac + 1e-9) {
                            continue;
                        }
                        const auto lab = model.lab(v);
                        blackL = std::min(blackL, lab[0]);
                        const double c = std::hypot(lab[1], lab[2]);
                        double h = std::atan2(lab[2], lab[1]);
                        if (h < 0) {
                            h += 2.0 * 3.14159265358979323846;
                        }
                        const double ld = std::round((lab[0] / 100.0) * (lBins - 1));
                        const size_t li =
                            static_cast<size_t>(std::min(std::max(ld, 0.0), static_cast<double>(lBins - 1)));
                        double hd = std::round((h / (2.0 * 3.14159265358979323846)) * hBins);
                        const size_t hi = static_cast<size_t>(std::fmod(hd, static_cast<double>(hBins)));
                        double& e = cmax[li * hBins + hi];
                        e = std::max(e, c);
                    }
                }
            }
        }
        // 补洞并平滑：3×3 邻域 max，再盒模糊。
        auto get = [&](const std::vector<double>& v, long l, long h) {
            l = std::min(std::max(l, 0L), static_cast<long>(lBins) - 1);
            h = ((h % static_cast<long>(hBins)) + static_cast<long>(hBins)) % static_cast<long>(hBins);
            return v[static_cast<size_t>(l) * hBins + static_cast<size_t>(h)];
        };
        std::vector<double> filled = cmax;
        for (long l = 0; l < static_cast<long>(lBins); ++l) {
            for (long h = 0; h < static_cast<long>(hBins); ++h) {
                double m = 0.0;
                for (long dl = -1; dl <= 1; ++dl) {
                    for (long dh = -1; dh <= 1; ++dh) {
                        m = std::max(m, get(cmax, l + dl, h + dh));
                    }
                }
                filled[static_cast<size_t>(l) * hBins + static_cast<size_t>(h)] = m;
            }
        }
        std::vector<double> smooth = filled;
        for (long l = 0; l < static_cast<long>(lBins); ++l) {
            for (long h = 0; h < static_cast<long>(hBins); ++h) {
                double s = 0.0;
                for (long dl = -1; dl <= 1; ++dl) {
                    for (long dh = -1; dh <= 1; ++dh) {
                        s += get(filled, l + dl, h + dh);
                    }
                }
                smooth[static_cast<size_t>(l) * hBins + static_cast<size_t>(h)] = s / 9.0;
            }
        }
        cmax = std::move(smooth);
    }

    double maxChroma(double l, double h) const {
        const double lp = std::min(std::max(l / 100.0, 0.0), 1.0) * static_cast<double>(lBins - 1);
        double hn = std::fmod(h, 2.0 * 3.14159265358979323846);
        if (hn < 0) {
            hn += 2.0 * 3.14159265358979323846;
        }
        const double hp = (hn / (2.0 * 3.14159265358979323846)) * static_cast<double>(hBins);
        const size_t l0 = static_cast<size_t>(std::floor(lp));
        const size_t h0 = static_cast<size_t>(std::floor(hp));
        const double fl = lp - static_cast<double>(l0);
        const double fh = hp - static_cast<double>(h0);
        const size_t l1 = std::min(l0 + 1, lBins - 1);
        const size_t h0m = h0 % hBins;
        const size_t h1m = (h0 + 1) % hBins;
        auto v = [&](size_t l, size_t h) { return cmax[l * hBins + h]; };
        const double a = v(l0, h0m) + (v(l0, h1m) - v(l0, h0m)) * fh;
        const double b = v(l1, h0m) + (v(l1, h1m) - v(l1, h0m)) * fh;
        return a + (b - a) * fl;
    }

    // 相对 Lab 颜色的感知色域压缩。
    std::array<double, 3> compress(const std::array<double, 3>& lab) const {
        const double l =
            blackL + std::min(std::max(lab[0], 0.0), 100.0) * (100.0 - blackL) / 100.0;
        const double c = std::hypot(lab[1], lab[2]);
        if (c < 1e-9) {
            return {l, 0.0, 0.0};
        }
        const double h = std::atan2(lab[2], lab[1]);
        const double cm = std::max(maxChroma(l, h), 1e-3);
        const double knee = 0.8 * cm;
        const double c2 = c <= knee ? c : knee + (cm - knee) * std::tanh((c - knee) / (cm - knee));
        return {l, c2 * std::cos(h), c2 * std::sin(h)};
    }
};

double labNodeLegacy(size_t i, size_t n) {
    return static_cast<double>(i) / static_cast<double>(n - 1);
}

// 旧式（lut16）Lab 节点坐标三元组解码。
std::array<double, 3> decodeV2(const std::array<double, 3>& v) {
    return {v[0] * 100.0 * 65535.0 / 65280.0, v[1] * 65535.0 / 256.0 - 128.0, v[2] * 65535.0 / 256.0 - 128.0};
}

std::array<float, 3> encodeV2(const std::array<double, 3>& lab) {
    return {static_cast<float>(std::min(std::max(lab[0] / (100.0 * 65535.0 / 65280.0), 0.0), 1.0)),
            static_cast<float>(std::min(std::max((lab[1] + 128.0) * 256.0 / 65535.0, 0.0), 1.0)),
            static_cast<float>(std::min(std::max((lab[2] + 128.0) * 256.0 / 65535.0, 0.0), 1.0))};
}

Clut b2aTable(const CmykModel& model, const CmykParams& p, const Gbd* gbd) {
    const size_t n = p.gridB2a;
    std::vector<float> data;
    data.reserve(n * n * n * 4);
    for (size_t li = 0; li < n; ++li) {
        for (size_t ai = 0; ai < n; ++ai) {
            // 沿 b 的一行，每个求解从邻居热启动。
            std::optional<std::array<double, 4>> warm;
            for (size_t bi = 0; bi < n; ++bi) {
                const auto lab = decodeV2({labNodeLegacy(li, n), labNodeLegacy(ai, n), labNodeLegacy(bi, n)});
                const std::array<double, 3> target = gbd != nullptr ? gbd->compress(lab) : lab;
                auto res = synthSeparate(model, p, target, warm.has_value() ? &*warm : nullptr);
                warm = res.first;
                for (double v : res.first) {
                    data.push_back(static_cast<float>(v));
                }
            }
        }
    }
    return Clut::make({n, n, n}, 4, std::move(data));
}

Lut lut16FromClut(Clut clut) {
    Lut l;
    l.kind = LutKind::Lut16;
    l.inputs = clut.inputs;
    l.outputs = clut.outputs;
    l.stages.push_back(Stage::curvesStage(std::vector<Curve>(clut.inputs, Curve::fromTable({0.0f, 1.0f}))));
    l.stages.push_back(Stage::clutStage(std::move(clut)));
    l.stages.push_back(Stage::curvesStage(std::vector<Curve>(l.outputs, Curve::fromTable({0.0f, 1.0f}))));
    return l;
}

}  // namespace

CmykModel CmykModel::coated(const double (&tvi)[4]) {
    auto labAt = [](double l, double a, double b) { return labToXyz({l, a, b}, kD50); };
    // ISO 12647-2（涂布、纸型 1/2）Lab 目标，D50/2°；CMY 叠印为估计值。
    const std::array<std::array<double, 3>, 8> base = {
        labAt(95.0, 0.0, -2.0),   // 纸
        labAt(55.0, -37.0, -50.0),  // C
        labAt(48.0, 74.0, -3.0),    // M
        labAt(24.0, 22.0, -46.0),   // C+M（蓝）
        labAt(89.0, -5.0, 93.0),    // Y
        labAt(50.0, -65.0, 27.0),   // C+Y（绿）
        labAt(47.0, 68.0, 48.0),    // M+Y（红）
        labAt(23.0, 0.0, 0.0),      // C+M+Y
    };
    const auto k = labAt(16.0, 0.0, 0.0);
    const std::array<double, 3> w = base[0];
    // 带 K 的叠印：首表面反射下限之上的反射率乘法。
    constexpr double floorR = 0.006;
    CmykModel m;
    for (int mask = 0; mask < 16; ++mask) {
        const std::array<double, 3>& p = base[mask & 7];
        std::array<double, 3> prim{};
        if ((mask & 8) == 0) {
            prim = p;
        } else {
            for (int i = 0; i < 3; ++i) {
                const double s = floorR * w[i];
                prim[i] = s + (p[i] - s) * (k[i] - s) / (w[i] - s);
            }
        }
        for (int i = 0; i < 3; ++i) {
            m.primN[mask][i] = std::pow(std::max(prim[i], 0.0), 1.0 / kYn);
        }
    }
    m.paper = w;
    for (int i = 0; i < 4; ++i) {
        m.tvi[i] = tvi[i];
    }
    return m;
}

std::array<double, 3> CmykModel::xyz(const std::array<double, 4>& cmyk) const {
    std::array<double, 4> a{};
    for (int i = 0; i < 4; ++i) {
        const double v = std::min(std::max(cmyk[i], 0.0), 1.0);
        a[i] = std::min(std::max(v + 4.0 * tvi[i] * v * (1.0 - v), 0.0), 1.0);
    }
    std::array<double, 3> s = {0.0, 0.0, 0.0};
    for (int mask = 0; mask < 16; ++mask) {
        double w = 1.0;
        for (int i = 0; i < 4; ++i) {
            w *= (mask & (1 << i)) != 0 ? a[i] : 1.0 - a[i];
        }
        if (w == 0.0) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            s[i] += w * primN[mask][i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        s[i] = std::pow(s[i], kYn);
    }
    return s;
}

std::array<double, 3> CmykModel::lab(const std::array<double, 4>& cmyk) const {
    const auto x = xyz(cmyk);
    const std::array<double, 3> rel = {x[0] * kD50[0] / paper[0], x[1] * kD50[1] / paper[1],
                                       x[2] * kD50[2] / paper[2]};
    return xyzToLab(rel, kD50);
}

std::pair<std::array<double, 4>, double> synthSeparate(const CmykModel& model, const CmykParams& p,
                                                       const std::array<double, 3>& target,
                                                       const std::array<double, 4>* warm) {
    std::vector<std::array<double, 3>> seeds;
    if (warm != nullptr) {
        seeds.push_back({(*warm)[0], (*warm)[1], (*warm)[2]});
    }
    for (const auto& s : kSeeds) {
        seeds.push_back({s[0], s[1], s[2]});
    }
    const auto cmy0 = solveCmy(model, target, 0.0, 3.0, seeds).first;
    const double g = std::min(std::min(cmy0[0], cmy0[1]), cmy0[2]);
    const double k =
        g <= p.gcrStart ? 0.0 : p.kMax * std::pow((g - p.gcrStart) / (1.0 - p.gcrStart), p.gcrGamma);
    std::vector<std::array<double, 3>> seeds2;
    seeds2.push_back({std::max(cmy0[0] - k, 0.0), std::max(cmy0[1] - k, 0.0), std::max(cmy0[2] - k, 0.0)});
    seeds2.push_back(cmy0);
    seeds2.insert(seeds2.end(), seeds.begin(), seeds.end());
    const auto res = solveCmy(model, target, k, std::max(p.tac - k, 0.0), seeds2);
    return std::make_pair(std::array<double, 4>{res.first[0], res.first[1], res.first[2], k}, res.second);
}

Profile synthCmykProfile(const CmykParams& p) {
    const CmykModel model = CmykModel::coated(p.tvi);
    const size_t g = p.gridA2b;
    Clut a2bClut = Clut::sample(
        std::vector<size_t>{g, g, g, g}, 3,
        [&](const float* inp, float* out) {
            const auto lab =
                model.lab({static_cast<double>(inp[0]), static_cast<double>(inp[1]),
                           static_cast<double>(inp[2]), static_cast<double>(inp[3])});
            const auto enc = encodeV2(lab);
            out[0] = enc[0];
            out[1] = enc[1];
            out[2] = enc[2];
        });
    const Gbd gbd(model, p);
    Clut colorimetric = b2aTable(model, p, nullptr);
    Clut perceptual = b2aTable(model, p, &gbd);
    Lut a2b = lut16FromClut(std::move(a2bClut));
    Lut b0 = lut16FromClut(std::move(perceptual));
    Lut b1 = lut16FromClut(std::move(colorimetric));
    Profile prof;
    prof.versionMajor = 4;
    prof.versionMinor = 0x30;
    prof.klass = ProfileClass::Output;
    prof.colorSpace = ColorSpace::Cmyk;
    prof.colorSpaceSig = colorSpaceSig(ColorSpace::Cmyk);
    prof.pcs = Pcs::Lab;
    prof.renderingIntent = Intent::Perceptual;
    prof.description = p.description;
    prof.copyright = kBuiltinCopyright;
    prof.whitePoint = {s15f16Round(model.paper[0]), s15f16Round(model.paper[1]), s15f16Round(model.paper[2])};
    prof.a2b[0] = a2b;
    prof.a2b[1] = a2b;
    prof.a2b[2] = a2b;
    prof.b2a[0] = b0;
    prof.b2a[1] = b1;
    prof.b2a[2] = b0;
    prof.withEncodedBytes();
    return prof;
}

Profile synthCoatedCmyk() {
    return synthCmykProfile(CmykParams{});
}

}  // namespace cms
}  // namespace montage
