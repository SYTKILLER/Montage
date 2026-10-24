#include "io/cms/curve.h"

#include <algorithm>
#include <optional>

namespace montage {
namespace cms {
namespace {

// 负输入镜像（f 偶延拓：-f(-x)）。
template <typename F>
inline double mirror(double x, F f) {
    return x < 0.0 ? -f(-x) : f(x);
}

double pw(double base, double g) {
    return base <= 0.0 ? 0.0 : std::pow(base, g);
}

double para(uint16_t kind, const double (&p)[7], double x) {
    const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
    switch (kind) {
        case 0:
            return mirror(x, [g](double v) { return std::pow(v, g); });
        case 1:
            return (a != 0.0 && x >= -b / a) ? pw(a * x + b, g) : 0.0;
        case 2:
            return (a != 0.0 && x >= -b / a) ? pw(a * x + b, g) + c : c;
        case 3:
            if (x < 0.0 && b == 0.0 && d == 0.0) {
                // 纯 gamma 写成 type 3：负输入镜像。
                return -para(kind, p, -x);
            }
            return x >= d ? pw(a * x + b, g) : c * x;
        default:
            return x >= d ? pw(a * x + b, g) + e : c * x + f;
    }
}

std::optional<double> paraInverse(uint16_t kind, const double (&p)[7], double y) {
    const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
    if (std::abs(g) < 1e-12) {
        return std::nullopt;
    }
    auto root = [g](double v) { return v <= 0.0 ? 0.0 : std::pow(v, 1.0 / g); };
    switch (kind) {
        case 0:
            return mirror(y, [g](double v) { return std::pow(v, 1.0 / g); });
        case 1:
            if (a == 0.0) {
                return std::nullopt;
            }
            return y <= 0.0 ? -b / a : (root(y) - b) / a;
        case 2:
            if (a == 0.0) {
                return std::nullopt;
            }
            return y <= c ? -b / a : (root(y - c) - b) / a;
        case 3: {
            if (a == 0.0) {
                return std::nullopt;
            }
            if (y < 0.0 && b == 0.0 && d == 0.0) {
                const auto r = paraInverse(kind, p, -y);
                return r ? std::optional<double>(-*r) : std::nullopt;
            }
            const double yd = pw(a * d + b, g);
            if (y >= yd) {
                return (root(y) - b) / a;
            }
            return c != 0.0 ? y / c : 0.0;
        }
        default: {
            if (a == 0.0) {
                return std::nullopt;
            }
            const double yd = pw(a * d + b, g) + e;
            if (y >= yd) {
                return (root(y - e) - b) / a;
            }
            return c != 0.0 ? (y - f) / c : 0.0;
        }
    }
}

// [0,1] 上递增函数的二分逆。
double numericInverse(double (*f)(double, const double (&)[7], uint16_t), double y, const double (&p)[7],
                      uint16_t kind) {
    const double f0 = f(0.0, p, kind);
    const double f1 = f(1.0, p, kind);
    const bool increasing = f1 >= f0;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double v = f(mid, p, kind);
        if ((v < y) == increasing) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

double paraFn(double x, const double (&p)[7], uint16_t kind) {
    return para(kind, p, x);
}

// 分段线性表求值。
inline float tableEval(const std::vector<float>& t, float x) {
    const size_t n = t.size();
    if (n == 0) {
        return x;
    }
    if (n == 1) {
        return t[0];
    }
    const float pos = std::clamp(x, 0.0f, 1.0f) * static_cast<float>(n - 1);
    size_t i = static_cast<size_t>(pos);
    if (i > n - 2) {
        i = n - 2;
    }
    const float f = pos - static_cast<float>(i);
    return t[i] + (t[i + 1] - t[i]) * f;
}

// 分段线性表的精确逆（二分搜索），支持递增或递减表。
float tableInverse(const std::vector<float>& t, float y) {
    const size_t n = t.size();
    if (n < 2) {
        return y;
    }
    const bool increasing = t[n - 1] >= t[0];
    const float loV = increasing ? t[0] : t[n - 1];
    const float hiV = increasing ? t[n - 1] : t[0];
    // 平坦端：取仍是端值的最后/第一个下标（归一化输入）。
    auto edgeIndex = [&](bool start) -> float {
        if (start) {
            const float v = t[0];
            size_t idx = n - 1;
            for (size_t i = 0; i < n; ++i) {
                if (std::abs(t[i] - v) > 1e-7f) {
                    idx = i - 1;
                    break;
                }
            }
            return static_cast<float>(idx) / static_cast<float>(n - 1);
        }
        const float v = t[n - 1];
        size_t idx = 0;
        for (size_t i = n; i-- > 0;) {
            if (std::abs(t[i] - v) > 1e-7f) {
                idx = i + 1;
                break;
            }
        }
        return static_cast<float>(idx) / static_cast<float>(n - 1);
    };
    if (y <= loV) {
        return edgeIndex(increasing);
    }
    if (y >= hiV) {
        return edgeIndex(!increasing);
    }
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if ((t[mid] < y) == increasing) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float a = t[lo], b = t[hi];
    const float f = std::abs(b - a) > 1e-12f ? (y - a) / (b - a) : 0.5f;
    return (static_cast<float>(lo) + f) / static_cast<float>(n - 1);
}

}  // namespace

Curve Curve::srgbTrc() {
    const double p[7] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045, 0.0, 0.0};
    return Curve::parametric(3, p);
}

int Curve::paramCount(uint16_t kind) {
    switch (kind) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 4;
        case 3: return 5;
        default: return 7;
    }
}

bool Curve::isIdentity() const {
    switch (kind) {
        case Kind::Identity:
            return true;
        case Kind::Gamma:
            return std::abs(gamma - 1.0) < 1e-9;
        case Kind::Parametric:
            return paraKind == 0 && std::abs(p[0] - 1.0) < 1e-9;
        case Kind::Table: {
            const size_t n = table.size();
            if (n < 2) {
                return true;
            }
            for (size_t i = 0; i < n; ++i) {
                if (std::abs(static_cast<double>(table[i]) - static_cast<double>(i) / static_cast<double>(n - 1)) >=
                    1.0 / 65535.0) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

double Curve::eval64(double x) const {
    switch (kind) {
        case Kind::Identity:
            return x;
        case Kind::Gamma:
            return mirror(x, [g = gamma](double v) { return std::pow(v, g); });
        case Kind::Parametric:
            return para(paraKind, p, x);
        case Kind::Table:
            return static_cast<double>(tableEval(table, static_cast<float>(x)));
    }
    return x;
}

float Curve::eval(float x) const {
    switch (kind) {
        case Kind::Identity:
            return x;
        case Kind::Table:
            return tableEval(table, x);
        default:
            return static_cast<float>(eval64(static_cast<double>(x)));
    }
}

double Curve::evalInverse64(double y) const {
    switch (kind) {
        case Kind::Identity:
            return y;
        case Kind::Gamma:
            if (std::abs(gamma) < 1e-12) {
                return y;
            }
            return mirror(y, [g = gamma](double v) { return std::pow(v, 1.0 / g); });
        case Kind::Parametric: {
            const auto r = paraInverse(paraKind, p, y);
            return r ? *r : numericInverse(paraFn, y, p, paraKind);
        }
        case Kind::Table:
            return static_cast<double>(tableInverse(table, static_cast<float>(y)));
    }
    return y;
}

float Curve::evalInverse(float y) const {
    switch (kind) {
        case Kind::Identity:
            return y;
        case Kind::Table:
            return tableInverse(table, y);
        default:
            return static_cast<float>(evalInverse64(static_cast<double>(y)));
    }
}

std::vector<float> Curve::sample(size_t n) const {
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(eval64(static_cast<double>(i) / static_cast<double>(n - 1)));
    }
    return out;
}

bool Curve::isMonotonic() const {
    const std::vector<float> s = sample(1024);
    bool inc = true, dec = true;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] < s[i - 1] - 1e-6f) {
            inc = false;
        }
        if (s[i] > s[i - 1] + 1e-6f) {
            dec = false;
        }
    }
    return inc || dec;
}

bool Curve::operator==(const Curve& o) const {
    if (kind != o.kind) {
        return false;
    }
    switch (kind) {
        case Kind::Identity:
            return true;
        case Kind::Gamma:
            return gamma == o.gamma;
        case Kind::Parametric:
            return paraKind == o.paraKind && std::equal(std::begin(p), std::end(p), std::begin(o.p));
        case Kind::Table:
            return table == o.table;
    }
    return false;
}

}  // namespace cms
}  // namespace montage
