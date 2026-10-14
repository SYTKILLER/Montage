#include "io/dng_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <hilog/log.h>

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Dng";

// ---------- 极简 TIFF 读取（DNG = TIFF 容器） ----------

struct TiffIfd {
    std::unordered_map<uint16_t, std::vector<uint64_t>> tags;  // tag → 解码值（RATIONAL 展开为 num,den 两个槽）
    std::unordered_map<uint16_t, std::vector<uint8_t>> raw;    // tag → 原始字节
};

class TiffReader {
public:
    bool open(const std::vector<uint8_t>& d, std::string& err) {
        data_ = &d;
        if (d.size() < 8) {
            err = "dng: too small";
            return false;
        }
        if (d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0) {
            little_ = true;
        } else if (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 0x2A) {
            little_ = false;
        } else {
            err = "dng: not a TIFF container";
            return false;
        }
        ifd0Offset_ = u32(4);
        return true;
    }

    bool collectIfds(std::string& err) {
        if (!readIfd(ifd0Offset_, err)) {
            return false;
        }
        // SubIFDs（tag 330，v1 一层）
        std::vector<uint64_t> subs;
        for (const auto& kv : ifds_[0].tags) {
            if (kv.first == 330) {
                subs = kv.second;
            }
        }
        for (uint64_t sub : subs) {
            if (sub != 0 && sub + 2 <= data_->size()) {
                readIfd(static_cast<uint32_t>(sub), err);
            }
        }
        return !ifds_.empty();
    }

    const TiffIfd& ifd(size_t i) const { return ifds_[i]; }
    size_t ifdCount() const { return ifds_.size(); }
    const std::vector<uint8_t>& data() const { return *data_; }
    bool little() const { return little_; }
    uint16_t u16At(uint32_t off) const { return u16(off); }

private:
    bool readIfd(uint32_t off, std::string& err) {
        if (off + 2 > data_->size()) {
            err = "dng: ifd out of range";
            return false;
        }
        const uint16_t count = u16(off);
        TiffIfd f;
        uint32_t p = off + 2;
        for (uint16_t i = 0; i < count; ++i) {
            if (p + 12 > data_->size()) {
                err = "dng: entry out of range";
                return false;
            }
            const uint16_t tag = u16(p);
            const uint16_t type = u16(p + 2);
            const uint32_t cnt = u32(p + 4);
            static const uint16_t kTypeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
            const uint32_t tsz = type < 13 ? kTypeSize[type] : 1;
            const uint64_t total = static_cast<uint64_t>(tsz) * cnt;
            uint64_t valOff = p + 8;
            if (total > 4) {
                valOff = u32(p + 8);
            }
            if (valOff + total > data_->size()) {
                err = "dng: value out of range";
                return false;
            }
            std::vector<uint8_t> raw(data_->begin() + static_cast<long>(valOff),
                                     data_->begin() + static_cast<long>(valOff + total));
            std::vector<uint64_t> vals;
            for (uint32_t k = 0; k < cnt && tsz > 0; ++k) {
                const uint64_t vo = valOff + static_cast<uint64_t>(k) * tsz;
                if (type == 1 || type == 6 || type == 7) {
                    vals.push_back((*data_)[static_cast<size_t>(vo)]);
                } else if (type == 3) {
                    vals.push_back(u16(static_cast<uint32_t>(vo)));
                } else if (type == 4) {
                    vals.push_back(u32(static_cast<uint32_t>(vo)));
                } else if (type == 5 || type == 10) {
                    // RATIONAL/SRATIONAL：num 与 den 各占一槽
                    const uint32_t num = u32(static_cast<uint32_t>(vo));
                    const uint32_t den = u32(static_cast<uint32_t>(vo + 4));
                    vals.push_back(num);
                    vals.push_back(den);
                }
            }
            f.tags[tag] = std::move(vals);
            f.raw[tag] = std::move(raw);
            p += 12;
        }
        ifds_.push_back(std::move(f));
        return true;
    }

    uint16_t u16(uint32_t off) const {
        const uint8_t a = (*data_)[off];
        const uint8_t b = (*data_)[off + 1];
        return little_ ? static_cast<uint16_t>(a | (b << 8))
                       : static_cast<uint16_t>((a << 8) | b);
    }
    uint32_t u32(uint32_t off) const {
        if (little_) {
            return static_cast<uint32_t>((*data_)[off]) |
                   (static_cast<uint32_t>((*data_)[off + 1]) << 8) |
                   (static_cast<uint32_t>((*data_)[off + 2]) << 16) |
                   (static_cast<uint32_t>((*data_)[off + 3]) << 24);
        }
        return (static_cast<uint32_t>((*data_)[off]) << 24) |
               (static_cast<uint32_t>((*data_)[off + 1]) << 16) |
               (static_cast<uint32_t>((*data_)[off + 2]) << 8) |
               static_cast<uint32_t>((*data_)[off + 3]);
    }

    const std::vector<uint8_t>* data_ = nullptr;
    bool little_ = true;
    uint32_t ifd0Offset_ = 0;
    std::vector<TiffIfd> ifds_;
};

inline float srgbEncode(float v) {
    v = std::min(1.0f, std::max(0.0f, v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// 双线性去马赛克：CFA 2×2（通道 0=R 1=G 2=B）
void demosaicBilinear(const std::vector<uint16_t>& raw, uint32_t w, uint32_t h,
                      const uint8_t cfa[4], std::vector<float>* planes) {
    auto channelAt = [&](uint32_t y, uint32_t x) -> int {
        return cfa[(y % 2) * 2 + (x % 2)];
    };
    for (int ch = 0; ch < 3; ++ch) {
        std::fill(planes[ch].begin(), planes[ch].end(), 0.0f);
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int ch = channelAt(y, x);
            planes[ch][static_cast<size_t>(y) * w + x] =
                static_cast<float>(raw[static_cast<size_t>(y) * w + x]);
        }
    }
    // 双线性补全（边界外跳过）：邻域模式按目标通道与点位关系
    //  - 补 G（本点 = R/B）：4 邻接 (±1,0)(0,±1)
    //  - 本点 = G 补 R/B：±2 同轴 + 对角 ±1
    //  - 本点 = R 补 B（或反之）：对角 ±1
    static const int kAdj[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const int kDiag[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int self = channelAt(y, x);
            for (int c = 0; c < 3; ++c) {
                if (c == self) {
                    continue;
                }
                // 邻域规则：补 G（本点 R/B）= 4 邻接；本点 G 补 R/B = 4 邻接
                // （R/B 各得 2 有效邻点）；本点 R 补 B（或反之）= 对角 ±1
                const bool needG = c == 1;
                const bool selfG = self == 1;
                const int(*offs)[2] = (!needG && !selfG) ? kDiag : kAdj;
                const int nOff = (!needG && !selfG) ? 4 : 4;
                float sum = 0.0f;
                int n = 0;
                for (int k = 0; k < nOff; ++k) {
                    const int nx = static_cast<int>(x) + offs[k][0];
                    const int ny = static_cast<int>(y) + offs[k][1];
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(w) ||
                        ny >= static_cast<int>(h)) {
                        continue;
                    }
                    if (channelAt(static_cast<uint32_t>(ny), static_cast<uint32_t>(nx)) != c) {
                        continue;
                    }
                    sum += static_cast<float>(raw[static_cast<size_t>(ny) * w + nx]);
                    ++n;
                }
                if (n > 0) {
                    planes[c][static_cast<size_t>(y) * w + x] = sum / static_cast<float>(n);
                }
            }
        }
    }
}

// 电平标签（SHORT×n / LONG×1 / RATIONAL×1 兜底均值；endianness 感知）
float levelTag(const TiffIfd& f, uint16_t tag, float def, bool little) {
    auto it = f.raw.find(tag);
    if (it == f.raw.end() || it->second.empty()) {
        return def;
    }
    const std::vector<uint8_t>& b = it->second;
    auto rd16 = [&](size_t o) -> uint32_t {
        return little ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8))
                      : static_cast<uint32_t>((b[o] << 8) | b[o + 1]);
    };
    auto rd32 = [&](size_t o) -> uint32_t {
        return little
                   ? static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                         (static_cast<uint32_t>(b[o + 2]) << 16) |
                         (static_cast<uint32_t>(b[o + 3]) << 24)
                   : (static_cast<uint32_t>(b[o]) << 24) |
                         (static_cast<uint32_t>(b[o + 1]) << 16) |
                         (static_cast<uint32_t>(b[o + 2]) << 8) |
                         static_cast<uint32_t>(b[o + 3]);
    };
    if (b.size() == 2) {
        return static_cast<float>(rd16(0));
    }
    if (b.size() == 4) {
        return static_cast<float>(rd32(0));  // LONG×1
    }
    if (b.size() == 8) {
        // RATIONAL×1：num/den（den==0 退 LONG）
        const uint32_t num = rd32(0);
        const uint32_t den = rd32(4);
        return den != 0 ? static_cast<float>(static_cast<double>(num) / den)
                        : static_cast<float>(num);
    }
    // 多值（SHORT×n 或 RATIONAL×n）：SHORT 步进均值（v1 简化，黑电平常为 SHORT 数组）
    double acc = 0;
    int cnt = 0;
    for (size_t k = 0; k + 1 < b.size(); k += 2) {
        acc += static_cast<double>(rd16(k));
        ++cnt;
    }
    return static_cast<float>(acc / std::max(1, cnt));
}

}  // namespace

bool looksLikeDng(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8) {
        return false;
    }
    const bool tiff = (bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 0x2A && bytes[3] == 0) ||
                      (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == 0x2A);
    if (!tiff) {
        return false;
    }
    const bool little = bytes[0] == 'I';
    auto rd16 = [&](uint32_t o) -> uint16_t {
        return little ? static_cast<uint16_t>(bytes[o] | (bytes[o + 1] << 8))
                      : static_cast<uint16_t>((bytes[o] << 8) | bytes[o + 1]);
    };
    auto rd32 = [&](uint32_t o) -> uint32_t {
        return little
                   ? static_cast<uint32_t>(bytes[o]) | (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                         (static_cast<uint32_t>(bytes[o + 2]) << 16) |
                         (static_cast<uint32_t>(bytes[o + 3]) << 24)
                   : (static_cast<uint32_t>(bytes[o]) << 24) |
                         (static_cast<uint32_t>(bytes[o + 1]) << 16) |
                         (static_cast<uint32_t>(bytes[o + 2]) << 8) |
                         static_cast<uint32_t>(bytes[o + 3]);
    };
    const uint32_t ifd0 = rd32(4);
    if (ifd0 + 2 > bytes.size()) {
        return false;
    }
    const uint16_t n = rd16(ifd0);
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t e = ifd0 + 2 + static_cast<uint32_t>(i) * 12;
        if (e + 12 > bytes.size()) {
            break;
        }
        if (rd16(e) == 50706) {  // DNGVersion
            return true;
        }
    }
    return false;
}

bool developDngToGrid(const std::vector<uint8_t>& bytes,
                      std::shared_ptr<const TileGrid>& out, uint32_t& outW, uint32_t& outH,
                      std::string& err) {
    TiffReader r;
    if (!r.open(bytes, err) || !r.collectIfds(err)) {
        return false;
    }
    // 选 raw IFD：PhotometricInterpretation == 32803 (CFA) 且面积最大
    const TiffIfd* raw = nullptr;
    uint64_t bestArea = 0;
    for (size_t i = 0; i < r.ifdCount(); ++i) {
        const TiffIfd& f = r.ifd(i);
        auto wIt = f.tags.find(256);
        auto hIt = f.tags.find(257);
        auto phIt = f.tags.find(262);
        if (wIt == f.tags.end() || hIt == f.tags.end() || wIt->second.empty() ||
            hIt->second.empty()) {
            continue;
        }
        const uint64_t area = wIt->second[0] * hIt->second[0];
        const bool isCfa = phIt != f.tags.end() && !phIt->second.empty() &&
                           phIt->second[0] == 32803;
        if (isCfa && area > bestArea) {
            raw = &f;
            bestArea = area;
        }
    }
    if (raw == nullptr) {
        err = "dng: no CFA raw ifd found";
        return false;
    }
    auto tagU64 = [&](uint16_t tag, uint64_t def) -> uint64_t {
        auto it = raw->tags.find(tag);
        return (it != raw->tags.end() && !it->second.empty()) ? it->second[0] : def;
    };
    const uint32_t w = static_cast<uint32_t>(tagU64(256, 0));
    const uint32_t h = static_cast<uint32_t>(tagU64(257, 0));
    const uint64_t bps = tagU64(258, 16);
    const uint64_t compression = tagU64(259, 1);
    const uint64_t samples = tagU64(277, 1);
    const uint64_t photometric = tagU64(262, 32803);
    if (w == 0 || h == 0 || w > 16384 || h > 16384 ||
        static_cast<uint64_t>(w) * h > 100ull * 1000 * 1000) {
        err = "dng: size out of limits";
        return false;
    }
    if (compression != 1) {
        err = "dng: only uncompressed raw supported";
        return false;
    }
    if (bps != 16) {
        err = "dng: only 16-bit raw supported";
        return false;
    }
    if (samples != 1 || photometric != 32803) {
        err = "dng: not a single-channel CFA raw";
        return false;
    }
    // CFA 2×2（tag 33421/33422；缺 pattern 给明确错误）
    uint8_t cfa[4] = {0, 1, 1, 2};  // 默认 RGGB
    auto cfaDim = raw->tags.find(33421);
    auto cfaPat = raw->raw.find(33422);
    if (cfaDim != raw->tags.end() && cfaPat != raw->raw.end() &&
        cfaDim->second.size() >= 2 && cfaDim->second[0] == 2 && cfaDim->second[1] == 2 &&
        cfaPat->second.size() >= 4) {
        std::memcpy(cfa, cfaPat->second.data(), 4);
    } else {
        err = "dng: missing CFA pattern";
        return false;
    }
    const float black = levelTag(*raw, 50714, 0.0f, r.little());
    const float white = levelTag(*raw, 50717, 65535.0f, r.little());
    if (!(white > black + 1)) {
        err = "dng: bad black/white levels";
        return false;
    }
    // AsShotNeutral（RATIONAL×3 = 24 字节）→ 通道增益（G 归一）
    float gain[3] = {1.0f, 1.0f, 1.0f};
    auto asn = raw->raw.find(50728);
    if (asn != raw->raw.end() && asn->second.size() == 24) {
        const std::vector<uint8_t>& b = asn->second;
        const bool littleFile = r.little();
        auto rd32 = [&](size_t o) -> uint32_t {
            return littleFile
                       ? static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                             (static_cast<uint32_t>(b[o + 2]) << 16) |
                             (static_cast<uint32_t>(b[o + 3]) << 24)
                       : (static_cast<uint32_t>(b[o]) << 24) |
                             (static_cast<uint32_t>(b[o + 1]) << 16) |
                             (static_cast<uint32_t>(b[o + 2]) << 8) |
                             static_cast<uint32_t>(b[o + 3]);
        };
        double neutral[3] = {1.0, 1.0, 1.0};
        for (int ch = 0; ch < 3; ++ch) {
            const size_t o = static_cast<size_t>(ch) * 8;
            const uint32_t num = rd32(o);
            const uint32_t den = rd32(o + 4);
            neutral[ch] = den != 0 ? static_cast<double>(num) / den : 1.0;
        }
        for (int ch = 0; ch < 3; ++ch) {
            gain[ch] = neutral[ch] > 1e-6 ? static_cast<float>(1.0 / neutral[ch]) : 1.0f;
        }
        if (gain[1] > 1e-6) {
            const float s = 1.0f / gain[1];
            gain[0] *= s;
            gain[1] = 1.0f;
            gain[2] *= s;
        }
    }
    // 条带（tiled 不支持）
    auto stripOff = raw->tags.find(273);
    auto stripLen = raw->tags.find(279);
    if (stripOff == raw->tags.end() || stripLen == raw->tags.end() ||
        stripOff->second.empty() || stripLen->second.size() != stripOff->second.size()) {
        err = "dng: missing strips";
        return false;
    }
    const auto& file = r.data();
    const bool little = r.little();
    auto rd16at = [&](const std::vector<uint8_t>& d, uint64_t o) -> uint16_t {
        return little ? static_cast<uint16_t>(d[static_cast<size_t>(o)] |
                                              (d[static_cast<size_t>(o) + 1] << 8))
                      : static_cast<uint16_t>((d[static_cast<size_t>(o)] << 8) |
                                              d[static_cast<size_t>(o) + 1]);
    };
    std::vector<uint16_t> rawPx(static_cast<size_t>(w) * h, 0);
    const uint64_t need = static_cast<uint64_t>(w) * h * 2;
    uint64_t fill = 0;
    for (size_t s = 0; s < stripOff->second.size() && fill < need; ++s) {
        const uint64_t off = stripOff->second[s];
        const uint64_t len = stripLen->second[s];
        const uint64_t copy = std::min<uint64_t>(len, need - fill);
        if (off + copy > file.size()) {
            err = "dng: strip out of range";
            return false;
        }
        for (uint64_t k = 0; k + 1 < copy; k += 2) {
            rawPx[static_cast<size_t>((fill + k) / 2)] = rd16at(file, off + k);
        }
        fill += copy & ~1ULL;
    }
    if (fill < need) {
        err = "dng: raw data short";
        return false;
    }
    // 显影：线性化（黑/白电平）→ 去马赛克 → WB → sRGB
    std::vector<uint16_t> lin(rawPx.size());
    for (size_t i = 0; i < rawPx.size(); ++i) {
        const float v = (static_cast<float>(rawPx[i]) - black) / (white - black);
        lin[i] = static_cast<uint16_t>(std::min(65535.0f, std::max(0.0f, v * 65535.0f)));
    }
    std::vector<float> planes[3] = {
        std::vector<float>(static_cast<size_t>(w) * h),
        std::vector<float>(static_cast<size_t>(w) * h),
        std::vector<float>(static_cast<size_t>(w) * h)};
    demosaicBilinear(lin, w, h, cfa, planes);

    auto builder = std::make_shared<TileGridBuilder>(
        (w + kTileSize - 1u) / kTileSize, (h + kTileSize - 1u) / kTileSize);
    for (uint32_t ty = 0; ty < (h + kTileSize - 1u) / kTileSize; ++ty) {
        for (uint32_t tx = 0; tx < (w + kTileSize - 1u) / kTileSize; ++tx) {
            EngineBuffer& buf = builder->ensureTile(tx, ty);
            uint8_t* dst = buf.mapCpuWrite();
            const uint32_t x0 = tx * kTileSize;
            const uint32_t y0 = ty * kTileSize;
            const uint32_t copyW = std::min(kTileSize, w - x0);
            const uint32_t copyH = std::min(kTileSize, h - y0);
            for (uint32_t rr = 0; rr < copyH; ++rr) {
                uint8_t* row = dst + static_cast<size_t>(buf.rowBytes()) * rr;
                for (uint32_t x = 0; x < copyW; ++x) {
                    const size_t idx = static_cast<size_t>(y0 + rr) * w + (x0 + x);
                    uint8_t* px = row + static_cast<size_t>(x) * 4;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float v = std::min(4.0f, planes[ch][idx] / 65535.0f * gain[ch]);
                        px[ch] = static_cast<uint8_t>(std::lround(srgbEncode(v) * 255.0f));
                    }
                    px[3] = 255;
                }
            }
        }
    }
    out = builder->publish(1);
    outW = w;
    outH = h;
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "developed %ux%u bps=%llu black=%.0f white=%.0f wb=(%.2f,%.2f,%.2f)", w, h,
                 static_cast<unsigned long long>(bps), black, white,
                 static_cast<double>(gain[0]), static_cast<double>(gain[1]),
                 static_cast<double>(gain[2]));
    return true;
}

}  // namespace io
}  // namespace montage
