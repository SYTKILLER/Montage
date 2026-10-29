#include "io/psd/psd_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <hilog/log.h>

#include "io/cms/convert.h"
#include "io/psd/psd_deep.h"

namespace montage {
namespace psd {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.PSD";
constexpr int kMaxSide = 16384;
constexpr int64_t kMaxSurfacePixels = 100LL * 1000 * 1000;

// ---------- 游标（对拍 PSDCursor） ----------

class Cursor {
  public:
    Cursor(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    size_t offset = 0;

    size_t size() const { return len_; }

    bool skip(int64_t count) {
        if (count < 0 || offset + static_cast<uint64_t>(count) > len_) {
            return false;
        }
        offset += static_cast<size_t>(count);
        return true;
    }
    bool u8(uint8_t* out) {
        if (offset + 1 > len_) {
            return false;
        }
        *out = data_[offset++];
        return true;
    }
    bool u16(uint16_t* out) {
        if (offset + 2 > len_) {
            return false;
        }
        *out = static_cast<uint16_t>(data_[offset] << 8 | data_[offset + 1]);
        offset += 2;
        return true;
    }
    bool i16(int16_t* out) {
        uint16_t v = 0;
        if (!u16(&v)) {
            return false;
        }
        *out = static_cast<int16_t>(v);
        return true;
    }
    bool u32(uint32_t* out) {
        if (offset + 4 > len_) {
            return false;
        }
        *out = static_cast<uint32_t>(data_[offset]) << 24 |
               static_cast<uint32_t>(data_[offset + 1]) << 16 |
               static_cast<uint32_t>(data_[offset + 2]) << 8 | data_[offset + 3];
        offset += 4;
        return true;
    }
    bool i32(int32_t* out) {
        uint32_t v = 0;
        if (!u32(&v)) {
            return false;
        }
        *out = static_cast<int32_t>(v);
        return true;
    }
    bool u64(uint64_t* out) {
        if (offset + 8 > len_) {
            return false;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = v << 8 | data_[offset + i];
        }
        offset += 8;
        *out = v;
        return true;
    }
    bool bytes(uint8_t* out, size_t count) {
        if (offset + count > len_) {
            return false;
        }
        std::memcpy(out, data_ + offset, count);
        offset += count;
        return true;
    }
    bool string(char* out, size_t count) {
        if (!bytes(reinterpret_cast<uint8_t*>(out), count)) {
            return false;
        }
        out[count] = '\0';
        return true;
    }
    bool string4(std::string* out) {
        char buf[5] = {0};
        if (!string(buf, 4)) {
            return false;
        }
        *out = buf;
        return true;
    }

  private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
};

bool u16At(const std::vector<uint8_t>& d, size_t at, uint16_t* out) {
    if (at + 2 > d.size()) {
        return false;
    }
    *out = static_cast<uint16_t>(d[at] << 8 | d[at + 1]);
    return true;
}

// ---------- 原始图层记录（对拍 RawLayer） ----------

struct RawLayer {
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    int sourceTop = 0, sourceLeft = 0, sourceBottom = 0, sourceRight = 0;
    uint8_t opacity = 255;
    uint8_t fill = 255;
    bool clipping = false;
    bool hidden = false;
    std::string blendKey = "norm";
    std::vector<std::pair<int, int64_t>> channels;  // (id, length)
    std::map<std::string, std::vector<uint8_t>> extra;
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    int sourceMaskTop = 0, sourceMaskLeft = 0, sourceMaskBottom = 0, sourceMaskRight = 0;
    uint8_t maskDefault = 255;
    bool maskDisabled = false;
    bool maskLinked = true;
    bool maskFromRender = false;
    bool hasMask = false;
    int section = -1;               // 对齐源 Int?（-1 = 无）
    std::vector<uint8_t> image;     // 预乘 RGBA
    std::vector<uint8_t> maskImage; // 灰度 patch
    // P6 D1-b：深色保真平面（f32 RGBA 预乘直 alpha 0..1；RGB/Gray 16/32 位）
    std::vector<float> deepRgba;
    bool deepValid = false;
    uint16_t deepDepth = 8;
    // 裁剪（画布越界预算兜底，对拍 PSDCrop/cropped）
    bool imageCropValid = false;
    int imageCropX = 0, imageCropY = 0, imageCropW = 0, imageCropH = 0;
    bool maskCropValid = false;
    int maskCropX = 0, maskCropY = 0, maskCropW = 0, maskCropH = 0;
    bool cropped = false;
    // M17c：lrFX 五类效果（readRecord 解析；组装期转 Record.style）+ 降级键
    std::vector<Record::PsdStyleEffect> style;
    bool hasStyle = false;
    std::vector<std::string> skippedFx;
    // M22：深色原始通道段 + 导入指纹（组装期转 Record）
    std::vector<uint8_t> srcSection;
    uint64_t importHash = 0;
    bool srcCropped = false;
};

// ---- M18d PSD 路径记录（photocraft psd/path.rs 对拍，MIT OR Apache-2.0） ----
// 26 字节/记录：u16 selector + 24B 数据；knot 坐标 = 有符号 8.24 定点（先 y 后 x），
// 为文档宽高的分数。length 记录 data[2..4] = 路径运算（-1/1 combine、2 subtract、
// 3 intersect、0 exclude——psd-tools 非公开语义）。
constexpr uint32_t kPsdPathRecordLen = 26;

inline double psdFixed24(const uint8_t* b) {
    const int32_t v = (static_cast<int32_t>(b[0]) << 24) | (static_cast<int32_t>(b[1]) << 16) |
                      (static_cast<int32_t>(b[2]) << 8) | static_cast<int32_t>(b[3]);
    return static_cast<double>(v) / 16777216.0;
}

// 记录列表 → 子路径（宽松：无 length 记录的 knot 延续前一子路径）。layerSemantics =
// vmsk 读法（photocraft layer_path_from_records）：首子路径 subtract → inverted，
// 其余运算取补对偶（combine↔subtract）；否则资源读法（initial_fill = inverted）。
static bool pathFromPsdRecords(const std::vector<uint8_t>& data, uint32_t w, uint32_t h,
                               bool layerSemantics, VectorPath& out) {
    out = VectorPath{};
    const size_t n = data.size() / kPsdPathRecordLen;
    if (n > 4000000) {
        return false;
    }
    bool initialFill = false;
    struct RawSub {
        bool closed = false;
        int32_t op = 1;
        struct RawKnot {
            bool linked = false;
            double pre[2] = {0, 0};
            double anchor[2] = {0, 0};
            double post[2] = {0, 0};
        };
        std::vector<RawKnot> knots;
    };
    std::vector<RawSub> subs;
    size_t remaining = 0;
    auto knotSel = [](uint16_t sel) { return sel == 1 || sel == 2 || sel == 4 || sel == 5; };
    for (size_t i = 0; i < n; i++) {
        const uint8_t* r = data.data() + i * kPsdPathRecordLen;
        const uint16_t sel = static_cast<uint16_t>((r[0] << 8) | r[1]);
        if (sel == 0 || sel == 3) {  // closed/open length
            RawSub sub;
            sub.closed = sel == 0;
            sub.op = static_cast<int16_t>(static_cast<uint16_t>((r[4] << 8) | r[5]));
            remaining = static_cast<size_t>((r[2] << 8) | r[3]);
            subs.push_back(std::move(sub));
        } else if (knotSel(sel)) {
            const bool closed = sel <= 2;
            if (remaining == 0 && (subs.empty() || subs.back().closed != closed)) {
                RawSub sub;
                sub.closed = closed;
                sub.op = -1;
                subs.push_back(std::move(sub));
            }
            if (remaining > 0) {
                remaining--;
            }
            if (subs.empty()) {
                continue;
            }
            RawSub::RawKnot k;
            k.linked = sel == 1 || sel == 4;
            // 先 y 后 x：pt(i) = (fixed(i*8+4), fixed(i*8))——x 在每对第二个
            const auto pt = [&r](int idx, double* out) {
                out[0] = psdFixed24(r + 2 + idx * 8 + 4);  // x
                out[1] = psdFixed24(r + 2 + idx * 8);      // y
            };
            pt(0, k.pre);
            pt(1, k.anchor);
            pt(2, k.post);
            subs.back().knots.push_back(k);
        } else if (sel == 8) {  // initial fill
            initialFill = (r[2] | (r[3] << 8)) != 0;
        }
        // selector 6/7（fill rule/clipboard）忽略
    }
    const double fw = static_cast<double>(w > 0 ? w : 1);
    const double fh = static_cast<double>(h > 0 ? h : 1);
    bool inverted = false;
    for (size_t i = 0; i < subs.size(); i++) {
        if (subs[i].knots.empty()) {
            continue;
        }
        PathOp op = PathOp::Combine;
        switch (subs[i].op) {
            case 0: op = PathOp::Exclude; break;
            case 2: op = PathOp::Subtract; break;
            case 3: op = PathOp::Intersect; break;
            case -1: op = PathOp::Join; break;
            default: op = PathOp::Combine; break;
        }
        if (layerSemantics && i == 0 && op == PathOp::Subtract) {
            // vmsk：首分量 subtract = 从全画布起步（模型 inverted + 补对偶）
            inverted = true;
            op = PathOp::Combine;
        } else if (layerSemantics && inverted && op == PathOp::Combine && i > 0) {
            op = PathOp::Subtract;
        } else if (layerSemantics && inverted && op == PathOp::Subtract && i > 0) {
            op = PathOp::Combine;
        }
        Subpath sp;
        sp.closed = subs[i].closed;
        sp.op = op;
        for (const auto& k : subs[i].knots) {
            Knot kn;
            kn.anchor = vpt(k.anchor[0] * fw, k.anchor[1] * fh);
            kn.inCtrl = vpt(k.pre[0] * fw, k.pre[1] * fh);
            kn.outCtrl = vpt(k.post[0] * fw, k.post[1] * fh);
            kn.smooth = k.linked;
            sp.knots.push_back(kn);
        }
        out.subpaths.push_back(std::move(sp));
    }
    if (!layerSemantics) {
        out.inverted = initialFill;
    }
    return !out.subpaths.empty() || initialFill;
}

bool readRecord(Cursor& c, bool isPSB, RawLayer& layer, std::string& err) {
    int32_t v = 0;
    if (!c.i32(&v)) { err = "truncated"; return false; }
    layer.top = v;
    if (!c.i32(&v)) { err = "truncated"; return false; }
    layer.left = v;
    if (!c.i32(&v)) { err = "truncated"; return false; }
    layer.bottom = v;
    if (!c.i32(&v)) { err = "truncated"; return false; }
    layer.right = v;
    layer.sourceTop = layer.top;
    layer.sourceLeft = layer.left;
    layer.sourceBottom = layer.bottom;
    layer.sourceRight = layer.right;

    uint16_t channelCount = 0;
    if (!c.u16(&channelCount)) { err = "truncated"; return false; }
    if (channelCount > 56) { err = "too many channels"; return false; }
    for (uint16_t i = 0; i < channelCount; ++i) {
        int16_t id = 0;
        if (!c.i16(&id)) { err = "truncated"; return false; }
        int64_t length = 0;
        if (isPSB) {
            uint64_t lv = 0;
            if (!c.u64(&lv)) { err = "truncated"; return false; }
            length = static_cast<int64_t>(lv);
        } else {
            uint32_t lv = 0;
            if (!c.u32(&lv)) { err = "truncated"; return false; }
            length = lv;
        }
        layer.channels.emplace_back(id, length);
    }

    std::string sig;
    if (!c.string4(&sig) || sig != "8BIM") { err = "truncated"; return false; }
    if (!c.string4(&layer.blendKey)) { err = "truncated"; return false; }
    if (!c.u8(&layer.opacity)) { err = "truncated"; return false; }
    uint8_t clip = 0;
    if (!c.u8(&clip)) { err = "truncated"; return false; }
    layer.clipping = clip != 0;
    uint8_t flags = 0;
    if (!c.u8(&flags)) { err = "truncated"; return false; }
    layer.hidden = (flags & 2) != 0;
    if (!c.skip(1)) { err = "truncated"; return false; }

    uint32_t extraLength = 0;
    if (!c.u32(&extraLength)) { err = "truncated"; return false; }
    const uint64_t extraEnd = c.offset + extraLength;

    uint32_t maskLength = 0;
    if (!c.u32(&maskLength)) { err = "truncated"; return false; }
    const uint64_t maskEnd = c.offset + maskLength;
    if (maskLength >= 20) {
        layer.hasMask = true;
        int32_t mv = 0;
        if (!c.i32(&mv)) { err = "truncated"; return false; }
        layer.maskTop = mv;
        if (!c.i32(&mv)) { err = "truncated"; return false; }
        layer.maskLeft = mv;
        if (!c.i32(&mv)) { err = "truncated"; return false; }
        layer.maskBottom = mv;
        if (!c.i32(&mv)) { err = "truncated"; return false; }
        layer.maskRight = mv;
        layer.sourceMaskTop = layer.maskTop;
        layer.sourceMaskLeft = layer.maskLeft;
        layer.sourceMaskBottom = layer.maskBottom;
        layer.sourceMaskRight = layer.maskRight;
        if (!c.u8(&layer.maskDefault)) { err = "truncated"; return false; }
        uint8_t maskFlags = 0;
        if (!c.u8(&maskFlags)) { err = "truncated"; return false; }
        layer.maskDisabled = (maskFlags & 2) != 0;
        layer.maskLinked = (maskFlags & 1) == 0;
        layer.maskFromRender = (maskFlags & 8) != 0;
    }
    if (!c.skip(static_cast<int64_t>(maskEnd) - static_cast<int64_t>(c.offset))) {
        err = "truncated";
        return false;
    }

    uint32_t ranges = 0;
    if (!c.u32(&ranges)) { err = "truncated"; return false; }
    if (!c.skip(ranges)) { err = "truncated"; return false; }

    uint8_t nameCount = 0;
    if (!c.u8(&nameCount)) { err = "truncated"; return false; }
    std::vector<uint8_t> nameBytes(nameCount);
    if (!c.bytes(nameBytes.data(), nameCount)) { err = "truncated"; return false; }
    layer.name.assign(nameBytes.begin(), nameBytes.end());
    if (layer.name.empty()) {
        layer.name = "Layer";
    }
    const int namePad = (4 - ((nameCount + 1) % 4)) % 4;
    if (!c.skip(namePad)) { err = "truncated"; return false; }

    // additional layer info（对拍 psbLargeAdditionalInfoKeys/luni/iOpa/lsct/lsdk）
    while (c.offset + 12 <= extraEnd) {
        std::string signature;
        if (!c.string4(&signature)) { err = "truncated"; return false; }
        if (signature != "8BIM" && signature != "8B64") {
            break;
        }
        std::string key;
        if (!c.string4(&key)) { err = "truncated"; return false; }
        static const char* kPsbLargeKeys[] = {
            "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
            "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD",
        };
        bool large = signature == "8B64";
        if (!large) {
            for (const char* k : kPsbLargeKeys) {
                if (key == k) {
                    large = true;
                    break;
                }
            }
        }
        int64_t length = 0;
        if (large) {
            uint64_t lv = 0;
            if (c.offset + 8 > extraEnd || !c.u64(&lv)) { break; }
            length = static_cast<int64_t>(lv);
        } else {
            uint32_t lv = 0;
            if (!c.u32(&lv)) { err = "truncated"; return false; }
            length = lv;
        }
        if (length < 0 || c.offset + static_cast<uint64_t>(length) > extraEnd ||
            c.offset + static_cast<uint64_t>(length) > c.size()) {
            err = "truncated";
            return false;
        }
        std::vector<uint8_t> payload(static_cast<size_t>(length));
        if (length > 0 && !c.bytes(payload.data(), payload.size())) { err = "truncated"; return false; }
        if (length % 2 == 1 && !c.skip(1)) { err = "truncated"; return false; }
        layer.extra[key] = std::move(payload);
        if (key == "luni") {
            // Unicode 名（对拍 unicodeName）
            const std::vector<uint8_t>& d = layer.extra[key];
            if (d.size() >= 4) {
                const uint32_t count = static_cast<uint32_t>(d[0]) << 24 | d[1] << 16 | d[2] << 8 | d[3];
                if (count > 0 && d.size() >= 4 + static_cast<size_t>(count) * 2) {
                    std::string uni;
                    uni.reserve(count * 3);
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint32_t u =
                            static_cast<uint32_t>(d[4 + i * 2]) << 8 | d[5 + i * 2];
                        if (u < 0x80) {
                            uni.push_back(static_cast<char>(u));
                        } else if (u < 0x800) {
                            uni.push_back(static_cast<char>(0xC0 | (u >> 6)));
                            uni.push_back(static_cast<char>(0x80 | (u & 0x3F)));
                        } else {
                            uni.push_back(static_cast<char>(0xE0 | (u >> 12)));
                            uni.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
                            uni.push_back(static_cast<char>(0x80 | (u & 0x3F)));
                        }
                    }
                    while (!uni.empty() && uni.back() == '\0') {
                        uni.pop_back();
                    }
                    if (!uni.empty()) {
                        layer.name = uni;
                    }
                }
            }
        }
        if (key == "iOpa" && !layer.extra[key].empty()) {
            layer.fill = layer.extra[key][0];
        }
        if ((key == "lsct" || key == "lsdk") && layer.extra[key].size() >= 4) {
            const std::vector<uint8_t>& d = layer.extra[key];
            layer.section = static_cast<int>(
                static_cast<uint32_t>(d[0]) << 24 | d[1] << 16 | d[2] << 8 | d[3]);
        }
        // M17c：lrFX 图层效果（Photoshop 5 布局，对拍 photocraft parse_lrfx）
        if (key == "lrFX" && layer.extra[key].size() >= 4) {
            const std::vector<uint8_t>& d = layer.extra[key];
            const uint32_t count = static_cast<uint32_t>(d[2]) << 8 | d[3];
            auto rd32 = [&d](size_t o) -> uint32_t {
                return static_cast<uint32_t>(d[o]) << 24 | static_cast<uint32_t>(d[o + 1]) << 16 |
                       static_cast<uint32_t>(d[o + 2]) << 8 | static_cast<uint32_t>(d[o + 3]);
            };
            auto colorAt = [&d, &rd32](size_t o, float rgb[3]) -> bool {
                // space u16 + 4×u16 分量（RGB 用前三个）；v>>8/255
                if (o + 10 > d.size()) {
                    return false;
                }
                for (int k = 0; k < 3; ++k) {
                    const uint16_t v =
                        static_cast<uint16_t>(d[o + 2 + k * 2]) << 8 | d[o + 3 + k * 2];
                    rgb[k] = static_cast<float>(v >> 8) / 255.0f;
                }
                return true;
            };
            size_t at = 4;
            bool anyMapped = false;
            for (uint32_t e = 0; e < count && at + 12 <= d.size(); ++e) {
                if (!(d[at] == '8' && d[at + 1] == 'B' && d[at + 2] == 'I' && d[at + 3] == 'M')) {
                    break;
                }
                const std::string ekey(d.begin() + static_cast<long>(at + 4),
                                       d.begin() + static_cast<long>(at + 8));
                const uint32_t size = rd32(at + 8);
                const size_t b = at + 12;
                if (b + size > d.size()) {
                    break;
                }
                bool mapped = false;
                if ((ekey == "dsdw" || ekey == "isdw") && size >= 41) {
                    Record::PsdStyleEffect fx;
                    fx.key = ekey;
                    fx.size = static_cast<double>(rd32(b + 4)) / 65536.0;
                    fx.angle = static_cast<double>(static_cast<int32_t>(rd32(b + 12))) / 65536.0;
                    fx.distance = static_cast<double>(rd32(b + 16)) / 65536.0;
                    if (colorAt(b + 20, fx.color)) {
                        const std::string blend(d.begin() + static_cast<long>(b + 30),
                                                d.begin() + static_cast<long>(b + 34));
                        fx.blendNormal = blend == "Nrml";
                        fx.enabled = d[b + 38] != 0;
                        fx.useGlobal = d[b + 39] != 0;
                        fx.opacity = static_cast<double>(d[b + 40]) / 255.0;
                        layer.style.push_back(fx);
                        mapped = true;
                    }
                } else if ((ekey == "oglw" || ekey == "iglw") && size >= 32) {
                    Record::PsdStyleEffect fx;
                    fx.key = ekey;
                    fx.size = static_cast<double>(rd32(b + 4)) / 65536.0;
                    if (colorAt(b + 12, fx.color)) {
                        const std::string blend(d.begin() + static_cast<long>(b + 22),
                                                d.begin() + static_cast<long>(b + 26));
                        fx.blendNormal = blend == "Nrml";
                        fx.enabled = d[b + 30] != 0;
                        fx.opacity = static_cast<double>(d[b + 31]) / 255.0;
                        layer.style.push_back(fx);
                        mapped = true;
                    }
                } else if (ekey == "sofi" && size >= 24) {
                    Record::PsdStyleEffect fx;
                    fx.key = ekey;
                    const std::string blend(d.begin() + static_cast<long>(b + 4),
                                            d.begin() + static_cast<long>(b + 8));
                    fx.blendNormal = blend == "Nrml";
                    if (colorAt(b + 12, fx.color)) {
                        fx.opacity = static_cast<double>(d[b + 22]) / 255.0;
                        fx.enabled = d[b + 23] != 0;
                        layer.style.push_back(fx);
                        mapped = true;
                    }
                } else if (ekey != "ebbl" && ekey != "sosl" && ekey != "strk" &&
                           ekey != "grfl" && ekey != "ptfl" && ekey != "bevl") {
                    layer.skippedFx.push_back(ekey);
                }
                if (mapped) {
                    anyMapped = true;
                    if (!layer.style.back().blendNormal) {
                        layer.skippedFx.push_back(ekey + "-blend");
                    }
                }
                at = b + size;  // 效果记录间不填充（photocraft parse_lrfx 同款；块级 pad 由外层处理）
            }
            if (anyMapped) {
                layer.hasStyle = true;
            }
        }
    }
    // M17c：仅有 lfx2（无 lrFX）的文件——五类效果也拿不到，降级提示（PS 正常产物两者都有）
    if (layer.extra.count("lfx2") != 0 && !layer.hasStyle) {
        layer.skippedFx.push_back("lfx2-only");
    }
    if (!c.skip(static_cast<int64_t>(extraEnd) - static_cast<int64_t>(c.offset))) {
        err = "truncated";
        return false;
    }
    return true;
}

// ---------- 通道解码（对拍 PSDChannelCoder：raw/PackBits + 裁剪） ----------

struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
};

bool unpackRLE(const std::vector<uint8_t>& data, size_t& offset, int width, int height, bool largeDoc,
               uint8_t* plane, const Crop* crop, std::string& err) {
    // crop = {x, y, w, h} 或 nullptr（整层）
    const int cw = crop != nullptr ? crop->w : width;
    std::vector<uint8_t> rowBuf(static_cast<size_t>(width));
    std::vector<int> counts(static_cast<size_t>(height));
    for (int row = 0; row < height; ++row) {
        if (largeDoc) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                if (offset >= data.size()) { err = "truncated"; return false; }
                v = v << 8 | data[offset++];
            }
            counts[static_cast<size_t>(row)] = static_cast<int>(v);
        } else {
            if (offset + 2 > data.size()) { err = "truncated"; return false; }
            counts[static_cast<size_t>(row)] = data[offset] << 8 | data[offset + 1];
            offset += 2;
        }
    }
    for (int row = 0; row < height; ++row) {
        const size_t end = offset + static_cast<size_t>(counts[static_cast<size_t>(row)]);
        if (end > data.size()) { err = "truncated"; return false; }
        if (crop != nullptr && (row < crop->y || row >= crop->y + crop->h)) {
            offset = end;
            continue;
        }
        int written = 0;
        while (written < width) {
            if (offset >= end) { err = "truncated"; return false; }
            const int8_t n = static_cast<int8_t>(data[offset]);
            offset += 1;
            if (n >= 0) {
                const int count = n + 1;
                if (written + count > width || offset + static_cast<size_t>(count) > end) {
                    err = "truncated";
                    return false;
                }
                for (int i = 0; i < count; ++i) {
                    rowBuf[static_cast<size_t>(written + i)] = data[offset + static_cast<size_t>(i)];
                }
                offset += static_cast<size_t>(count);
                written += count;
            } else if (n != -128) {
                const int count = 1 - n;
                if (written + count > width || offset >= end) {
                    err = "truncated";
                    return false;
                }
                const uint8_t value = data[offset];
                offset += 1;
                for (int i = 0; i < count; ++i) {
                    rowBuf[static_cast<size_t>(written + i)] = value;
                }
                written += count;
            }
        }
        if (crop == nullptr) {
            std::memcpy(plane + static_cast<size_t>(row) * width, rowBuf.data(), static_cast<size_t>(width));
        } else {
            // H7 corpus 门禁实证修复（P0 堆越界 + 内容错位）：dst 只按裁剪后行距排布，
            // crop->x 是源行内列偏移（对拍 raw 压缩路径无 x 偏移的既有语义）。原式
            // `+ crop->x` 令每行右移 x 且末行越界写 cw·ch 平面 crop->x 字节——凡层越出
            // 画布左/上缘（croppedToCanvas）即触发。
            std::memcpy(plane + static_cast<size_t>(row - crop->y) * cw,
                        rowBuf.data() + crop->x, static_cast<size_t>(crop->w));
        }
    }
    return true;
}

// M22：扁平 PSD 的合成图（ImageData）段解码 → 单背景 Record（深度/模式全通道）。
void decodeFlatComposite(const uint8_t* data, Cursor& c, bool isPSB, int width, int height,
                         uint16_t depth, uint16_t mode, PsdDocument& out);

bool decodeChannel(int compression, int width, int height, const std::vector<uint8_t>& payload,
                   bool largeDoc, const Crop* crop, std::vector<uint8_t>& out, std::string& err) {
    if (width <= 0 || height <= 0) {
        return true;
    }
    const int cw = crop != nullptr ? crop->w : width;
    const int ch = crop != nullptr ? crop->h : height;
    if (crop != nullptr && (crop->x < 0 || crop->y < 0 || crop->w < 0 || crop->h < 0 ||
                            crop->x + crop->w > width || crop->y + crop->h > height)) {
        err = "truncated";
        return false;
    }
    if (cw == 0 || ch == 0) {
        out.clear();
        return true;
    }
    if (compression == 0) {
        if (crop == nullptr) {
            if (payload.size() < static_cast<size_t>(width) * height) { err = "truncated"; return false; }
            out.assign(payload.begin(), payload.begin() + static_cast<size_t>(width) * height);
            return true;
        }
        if (payload.size() < static_cast<size_t>(width) * height) { err = "truncated"; return false; }
        out.assign(static_cast<size_t>(cw) * ch, 0);
        for (int row = 0; row < ch; ++row) {
            const uint8_t* src = payload.data() + static_cast<size_t>(crop->y + row) * width + crop->x;
            std::memcpy(out.data() + static_cast<size_t>(row) * cw, src, static_cast<size_t>(cw));
        }
        return true;
    }
    if (compression == 1) {
        out.assign(static_cast<size_t>(cw) * ch, 0);
        size_t offset = 0;
        return unpackRLE(payload, offset, width, height, largeDoc, out.data(), crop, err);
    }
    err = "unsupported compression";
    return false;
}

// M22：扁平 PSD 的合成图（ImageData）段解码 → 单背景 Record（深度/模式全通道）。
// 位置 = c 当前处（layer & mask 段末尾）：u16 compression + 全通道数据。
// raw = 平面字节连续（srcChannels × rb × h）；RLE = 全通道行计数表
//（srcChannels × h 项，PSB u32）后接全部压缩行。截断/不支持 = 静默空文档（旧行为）。
void decodeFlatComposite(const uint8_t* data, Cursor& c, bool isPSB, int width, int height,
                         uint16_t depth, uint16_t mode, PsdDocument& out) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const size_t cc = deepColorChannels(mode);
    if (cc == 0 || out.srcChannels < cc || out.srcChannels > 56) {
        return;
    }
    const size_t channels = out.srcChannels;
    const size_t rb = deepRowBytes(static_cast<size_t>(width), depth);
    if (rb * static_cast<size_t>(height) > 512ull * 1024 * 1024) {
        return;  // 预算闸（与导入上限同族）
    }
    const size_t sectionStart = c.offset;
    uint16_t compression = 0;
    if (!c.u16(&compression)) {
        return;
    }
    std::vector<std::vector<uint8_t>> planesRaw(channels);
    if (compression == 0) {
        const size_t total = rb * static_cast<size_t>(height) * channels;
        std::vector<uint8_t> all;
        all.resize(total);
        if (!c.bytes(all.data(), all.size())) {
            return;
        }
        const size_t planeSize = rb * static_cast<size_t>(height);
        for (size_t k = 0; k < channels; ++k) {
            planesRaw[k].assign(all.begin() + static_cast<std::ptrdiff_t>(k * planeSize),
                                all.begin() + static_cast<std::ptrdiff_t>((k + 1) * planeSize));
        }
    } else if (compression == 1) {
        // 全通道行计数表。
        std::vector<size_t> counts(channels * static_cast<size_t>(height));
        for (size_t i = 0; i < counts.size(); ++i) {
            if (isPSB) {
                uint32_t v = 0;
                if (!c.u32(&v)) {
                    return;
                }
                counts[i] = v;
            } else {
                uint16_t v = 0;
                if (!c.u16(&v)) {
                    return;
                }
                counts[i] = v;
            }
        }
        // 每通道的行数据段（按表序连续）。
        for (size_t k = 0; k < channels; ++k) {
            std::vector<uint8_t> payload;
            // 行计数表重建（rleToBytes 读 h 项）。
            auto putCount = [&](size_t v) {
                if (isPSB) {
                    payload.push_back(static_cast<uint8_t>(v >> 24));
                    payload.push_back(static_cast<uint8_t>(v >> 16));
                    payload.push_back(static_cast<uint8_t>(v >> 8));
                    payload.push_back(static_cast<uint8_t>(v));
                } else {
                    payload.push_back(static_cast<uint8_t>(v >> 8));
                    payload.push_back(static_cast<uint8_t>(v));
                }
            };
            for (size_t r = 0; r < static_cast<size_t>(height); ++r) {
                putCount(counts[k * static_cast<size_t>(height) + r]);
            }
            for (size_t r = 0; r < static_cast<size_t>(height); ++r) {
                const size_t n = counts[k * static_cast<size_t>(height) + r];
                std::vector<uint8_t> row(n);
                if (n > 0 && !c.bytes(row.data(), row.size())) {
                    return;
                }
                payload.insert(payload.end(), row.begin(), row.end());
            }
            planesRaw[k] = std::move(payload);
        }
    } else {
        return;
    }
    // 字节平面 → 8-bit → 模式交错 → 预乘背景层。
    std::vector<std::vector<uint8_t>> color8(cc);
    for (size_t k = 0; k < cc; ++k) {
        std::string derr;
        auto p8 = planeToU8(planesRaw[k].data(), planesRaw[k].size(), depth, static_cast<size_t>(width),
                            static_cast<size_t>(height), derr);
        if (!p8) {
            return;
        }
        color8[k] = std::move(*p8);
    }
    std::vector<uint8_t> alpha8;
    const std::vector<uint8_t>* alphaPtr = nullptr;
    if (channels > cc) {
        std::string derr;
        auto p8 = planeToU8(planesRaw[cc].data(), planesRaw[cc].size(), depth, static_cast<size_t>(width),
                            static_cast<size_t>(height), derr);
        if (!p8) {
            return;
        }
        alpha8 = std::move(*p8);
        alphaPtr = &alpha8;
    }
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::string ierr;
    std::shared_ptr<const cms::CmykSpace> space =
        out.iccProfile.empty() ? nullptr : cms::CmykSpace::forProfile(&out.iccProfile);
    cms::CmykScope cmykScope(space);
    auto rgba = interleaveToRgba(mode, color8, alphaPtr,
                                 out.colorModeData.empty() ? nullptr : out.colorModeData.data(), n, ierr);
    if (!rgba) {
        return;
    }
    Record rec;
    rec.name = "Background";
    rec.left = 0;
    rec.top = 0;
    rec.width = width;
    rec.height = height;
    rec.rgba.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t a = (*rgba)[i * 4 + 3];
        rec.rgba[i * 4] = static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4]) * a + 127) / 255);
        rec.rgba[i * 4 + 1] =
            static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4 + 1]) * a + 127) / 255);
        rec.rgba[i * 4 + 2] =
            static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4 + 2]) * a + 127) / 255);
        rec.rgba[i * 4 + 3] = a;
    }
    // 往返元数据：整段（含 compression u16）原样 + 导入指纹。
    rec.srcSection.assign(data + sectionStart, data + c.offset);
    rec.srcCropped = false;
    uint64_t hsh = 1469598103934665603ull;
    auto fnvMix = [&hsh](const uint8_t* p, size_t cnt) {
        for (size_t i = 0; i < cnt; ++i) {
            hsh ^= p[i];
            hsh *= 1099511628211ull;
        }
    };
    auto mixU64 = [&hsh](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            hsh ^= static_cast<uint8_t>(v >> (8 * i));
            hsh *= 1099511628211ull;
        }
    };
    mixU64(0);
    mixU64(0);
    mixU64(static_cast<uint64_t>(width));
    mixU64(static_cast<uint64_t>(height));
    fnvMix(rec.rgba.data(), rec.rgba.size());
    rec.importHash = hsh;
    out.layers.push_back(std::move(rec));
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "flat composite decoded %{public}dx%{public}d depth=%{public}u mode=%{public}u",
                 width, height, depth, mode);
}

Crop cropRect(int left, int top, int right, int bottom, int canvasW, int canvasH) {
    const int cl = std::min(canvasW, std::max(0, left));
    const int ct = std::min(canvasH, std::max(0, top));
    const int cr = std::max(cl, std::min(canvasW, right));
    const int cb = std::max(ct, std::min(canvasH, bottom));
    return Crop{cl - left, ct - top, cr - cl, cb - ct};
}

// ---------- 组装（对拍 assemble/kind，PSD-1 裁剪：调整层跳过、文本/矢量像素导入） ----------

bool hasAnyKey(const RawLayer& layer, const std::vector<const char*>& keys) {
    for (const char* k : keys) {
        if (layer.extra.count(k) != 0) {
            return true;
        }
    }
    return false;
}

LayerKind kindOf(const RawLayer& layer, bool isGroup) {
    if (isGroup) {
        return LayerKind::Group;
    }
    if (hasAnyKey(layer, {"TySh", "tySh", "txt2"})) {
        return LayerKind::Text;
    }
    if (hasAnyKey(layer, {"vmsk", "vsms", "vogk"})) {
        return LayerKind::Vector;
    }
    if (hasAnyKey(layer, {"SoLd", "SoLE"})) {
        return LayerKind::SmartObject;
    }
    if (hasAnyKey(layer, {"lfx2", "lrFX", "lmfx"})) {
        return LayerKind::Effects;
    }
    static const std::vector<const char*> kAdjustmentKeys = {
        "levl", "curv", "hue2", "hue ", "expA", "grdm", "brit", "blnc", "nvrt",
        "thrs", "post", "mixr", "selc", "blwh", "phfl", "vibA",
    };
    if (hasAnyKey(layer, kAdjustmentKeys)) {
        return LayerKind::Adjustment;
    }
    return LayerKind::Raster;
}

}  // namespace

BlendMode blendFromKey(const std::string& key, bool* exact) {
    // 完整映射（对拍 PSDTypes.fromPSD 25 项；Dissolve 等上游即无等价 → Normal+报告）
    *exact = true;
    if (key == "norm") return BlendMode::Normal;
    if (key == "mul ") return BlendMode::Multiply;
    if (key == "scrn") return BlendMode::Screen;
    if (key == "over") return BlendMode::Overlay;
    if (key == "dark") return BlendMode::Darken;
    if (key == "lite") return BlendMode::Lighten;
    if (key == "diff") return BlendMode::Difference;
    if (key == "div ") return BlendMode::ColorDodge;
    if (key == "idiv") return BlendMode::ColorBurn;
    if (key == "hue ") return BlendMode::Hue;
    if (key == "sat ") return BlendMode::Saturation;
    if (key == "colr") return BlendMode::Color;
    if (key == "lum ") return BlendMode::Luminosity;
    if (key == "lbrn") return BlendMode::LinearBurn;
    if (key == "lddg") return BlendMode::LinearDodge;
    if (key == "hLit") return BlendMode::HardLight;
    if (key == "vLit") return BlendMode::VividLight;
    if (key == "lLit") return BlendMode::LinearLight;
    if (key == "pLit") return BlendMode::PinLight;
    if (key == "hMix") return BlendMode::HardMix;
    if (key == "sLit") return BlendMode::SoftLight;
    if (key == "smud") return BlendMode::Exclusion;
    if (key == "fsub") return BlendMode::Subtract;
    if (key == "fdiv") return BlendMode::Divide;
    *exact = false;  // Dissolve / Darker Color / Lighter Color：上游即落 Normal 并报告
    return BlendMode::Normal;
}
bool readPsd(const uint8_t* data, size_t length, PsdDocument& out, std::string& err) {
    Cursor c(data, length);
    std::string magic;
    if (!c.string4(&magic) || magic != "8BPS") { err = "not a PSD file"; return false; }
    uint16_t version = 0;
    if (!c.u16(&version)) { err = "truncated"; return false; }
    if (version != 1 && version != 2) { err = "unsupported version"; return false; }
    const bool isPSB = version == 2;
    if (!c.skip(6)) { err = "truncated"; return false; }
    uint16_t channels = 0;
    if (!c.u16(&channels)) { err = "truncated"; return false; }
    uint32_t h = 0;
    if (!c.u32(&h)) { err = "truncated"; return false; }
    uint32_t w = 0;
    if (!c.u32(&w)) { err = "truncated"; return false; }
    uint16_t depth = 0;
    if (!c.u16(&depth)) { err = "truncated"; return false; }
    uint16_t mode = 0;
    if (!c.u16(&mode)) { err = "truncated"; return false; }
    if (w < 1 || w > kMaxSide || h < 1 || h > kMaxSide ||
        static_cast<int64_t>(w) * h > kMaxSurfacePixels) {
        err = "image too large";
        return false;
    }
    // M22：深度 1/8/16/32、模式 Bitmap/Gray/Indexed/RGB/CMYK/Duotone/Lab 全收
    //（渲染仍 8-bit——读入转 8-bit RGB，原始深/模式/ICC 保真存储与往返）。
    if (depth != 1 && depth != 8 && depth != 16 && depth != 32) {
        err = "only 8-bit RGB PSD can be imported";
        return false;
    }
    if (mode == 7) {
        // M22 登记：多通道模式拒绝（photocraft 同；无 RGB 合成语义）。
        err = "multichannel PSD is not supported";
        return false;
    }
    if (mode > 9) { err = "unsupported color mode " + std::to_string(mode); return false; }
    out.srcDepth = depth;
    out.srcMode = mode;
    out.srcChannels = channels;

    uint32_t colorModeLen = 0;
    if (!c.u32(&colorModeLen)) { err = "truncated"; return false; }
    // indexed 调色板（768 字节，R/G/B 三平面表）保真捕获（其余模式跳过）。
    if (mode == 2 && colorModeLen > 0 && colorModeLen <= 2048) {
        std::vector<uint8_t> palette(static_cast<size_t>(colorModeLen));
        if (!c.bytes(palette.data(), palette.size())) { err = "truncated"; return false; }
        out.colorModeData = std::move(palette);
    } else if (!c.skip(colorModeLen)) {
        err = "truncated";
        return false;
    }



    // image resources（取 1005 分辨率）
    uint32_t resourcesLength = 0;
    if (!c.u32(&resourcesLength)) { err = "truncated"; return false; }
    const uint64_t resourcesEnd = c.offset + resourcesLength;
    while (c.offset + 12 <= resourcesEnd) {
        std::string signature;
        if (!c.string4(&signature) || signature != "8BIM") { break; }
        uint16_t id = 0;
        if (!c.u16(&id)) { err = "truncated"; return false; }
        uint8_t nameLength = 0;
        if (!c.u8(&nameLength)) { err = "truncated"; return false; }
        // M18 收口：保存路径名 = 资源 pascal name（PS「路径 1」等；原登记「路径资源名称丢失」）
        std::string resName;
        if (nameLength > 0) {
            resName.resize(nameLength);
            if (!c.bytes(reinterpret_cast<uint8_t*>(resName.data()), nameLength)) {
                err = "truncated";
                return false;
            }
        } else {
            resName.clear();
        }
        if ((nameLength + 1) % 2 == 1 && !c.skip(1)) { err = "truncated"; return false; }
        uint32_t len = 0;
        if (!c.u32(&len)) { err = "truncated"; return false; }
        const uint64_t dataStart = c.offset;
        if ((id == 1025 || (id >= 2000 && id <= 2997)) && len >= 26 && len <= 16u * 1024 * 1024) {
            // M18d：路径资源（1025 = 工作路径、2000–2997 = 保存路径）
            std::vector<uint8_t> pdata(static_cast<size_t>(len));
            if (c.bytes(pdata.data(), static_cast<size_t>(len))) {
                VectorPath vp;
                if (pathFromPsdRecords(pdata, w, h, false, vp)) {  // 局部 w/h：out.width 在图层段后才赋值
                    if (id == 1025) {
                        out.workPath = std::move(vp);
                    } else {
                        NamedPathEntry entry;
                        entry.name = resName.empty()
                                         ? "路径 " + std::to_string(out.paths.size() + 1)
                                         : resName;
                        entry.path = std::move(vp);
                        out.paths.push_back(std::move(entry));
                    }
                }
            }
        }
        if (id == 1005 && len >= 4) {
            uint32_t fixed = 0;
            if (!c.u32(&fixed)) { err = "truncated"; return false; }
            double res = static_cast<double>(fixed) / 65536.0;
            if (!std::isfinite(res) || res < 1) {
                res = 72;
            }
            out.resolution = std::min(9600.0, std::max(1.0, res));
        }
        // M22：内嵌 ICC profile（资源 1039）——文档 CMYK/Lab/RGB 空间的色彩受管转换用。
        if (id == 1039 && len > 0 && len <= 32u * 1024 * 1024) {
            std::vector<uint8_t> icc(static_cast<size_t>(len));
            if (c.bytes(icc.data(), icc.size())) {
                out.iccProfile = std::move(icc);
            }
        }
        c.offset = dataStart + len;
        if (len % 2 == 1 && !c.skip(1)) { err = "truncated"; return false; }
    }
    c.offset = resourcesEnd;

    // layer & mask information
    uint64_t layerSection = 0;
    if (isPSB) {
        if (!c.u64(&layerSection)) { err = "truncated"; return false; }
    } else {
        uint32_t lv = 0;
        if (!c.u32(&lv)) { err = "truncated"; return false; }
        layerSection = lv;
    }
    const uint64_t layerSectionEnd = c.offset + layerSection;
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    if (layerSection < 4) {
        // 无图层段（扁平 PSD）：M22 起解码合成图（ImageData）段为单背景层——
        // 16/32/CMYK/Lab/Gray 的扁平文件在互换场景常见；截断/失败保持空文档旧行为。
        decodeFlatComposite(data, c, isPSB, static_cast<int>(w), static_cast<int>(h), depth, mode, out);
        return true;
    }
    uint64_t layerInfoLength = 0;
    if (isPSB) {
        if (!c.u64(&layerInfoLength)) { err = "truncated"; return false; }
    } else {
        uint32_t lv = 0;
        if (!c.u32(&lv)) { err = "truncated"; return false; }
        layerInfoLength = lv;
    }
    (void)layerInfoLength;

    int16_t rawCount = 0;
    if (!c.i16(&rawCount)) { err = "truncated"; return false; }
    const int count = std::abs(static_cast<int>(rawCount));
    if (count > 10000) { err = "image too large"; return false; }
    if (count == 0) {
        // layerSection 存在但 0 层 = 扁平文件（PS 保存的合并 PSD 常见）：解码合成图为背景层。
        decodeFlatComposite(data, c, isPSB, static_cast<int>(w), static_cast<int>(h), depth, mode, out);
        c.offset = layerSectionEnd;
        return true;
    }

    std::vector<RawLayer> raw(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (!readRecord(c, isPSB, raw[static_cast<size_t>(i)], err)) {
            return false;
        }
    }

    // 画布越界裁剪（预算兜底，对拍 cropToCanvas）
    auto cropLayer = [&](RawLayer& l) {
        const Crop ic = cropRect(l.left, l.top, l.right, l.bottom, out.width, out.height);
        if (ic.x != 0 || ic.y != 0 || ic.w != l.right - l.left || ic.h != l.bottom - l.top) {
            l.left += ic.x;
            l.top += ic.y;
            l.right = l.left + ic.w;
            l.bottom = l.top + ic.h;
            l.imageCropValid = true;
            l.imageCropX = ic.x;
            l.imageCropY = ic.y;
            l.imageCropW = ic.w;
            l.imageCropH = ic.h;
            l.cropped = true;
        }
        if (!l.hasMask) {
            return;
        }
        const Crop mc = cropRect(l.maskLeft, l.maskTop, l.maskRight, l.maskBottom, out.width, out.height);
        if (mc.x != 0 || mc.y != 0 || mc.w != l.maskRight - l.maskLeft || mc.h != l.maskBottom - l.maskTop) {
            l.maskLeft += mc.x;
            l.maskTop += mc.y;
            l.maskRight = l.maskLeft + mc.w;
            l.maskBottom = l.maskTop + mc.h;
            l.maskCropValid = true;
            l.maskCropX = mc.x;
            l.maskCropY = mc.y;
            l.maskCropW = mc.w;
            l.maskCropH = mc.h;
            l.cropped = true;
        }
    };
    for (RawLayer& l : raw) {
        cropLayer(l);
    }

    // 通道解码（对拍 decodeChannels；源尺寸解码 + 目标裁剪）。
    // M22：深度 1/16/32 与模式 Gray/Indexed/Bitmap/Duotone/CMYK/Lab 走 psd_deep
    // （CMYK/Lab 受管转换——文档内嵌 ICC 经 CmykScope 生效，否则内置涂布 CMYK）；
    // 8-bit RGB 保持原路径不变（回归安全）。原始通道段整段捕获供未编辑回写。
    {
        std::shared_ptr<const cms::CmykSpace> space =
            out.iccProfile.empty() ? nullptr : cms::CmykSpace::forProfile(&out.iccProfile);
        cms::CmykScope cmykScope(space);
        const bool deepActive = depth != 8 || mode != 3;
        const size_t colorCount = deepActive ? deepColorChannels(mode) : 3;
        for (RawLayer& layer : raw) {
        const int width = std::max(0, layer.right - layer.left);
        const int height = std::max(0, layer.bottom - layer.top);
        const int maskWidth = std::max(0, layer.maskRight - layer.maskLeft);
        const int maskHeight = std::max(0, layer.maskBottom - layer.maskTop);
        const int sourceW = std::max(0, layer.sourceRight - layer.sourceLeft);
        const int sourceH = std::max(0, layer.sourceBottom - layer.sourceTop);
        const int sourceMaskW = std::max(0, layer.sourceMaskRight - layer.sourceMaskLeft);
        const int sourceMaskH = std::max(0, layer.sourceMaskBottom - layer.sourceMaskTop);
        std::map<int, std::vector<uint8_t>> planes;
        // P6 D1-b：RGB/Gray 16/32 位 f32 平面收集（保真装配用；CMYK/Lab 维持降 8）
        const bool deepF32Wanted = depth == 16 || depth == 32;
        std::map<int, std::vector<float>> deepF;
        const size_t sectionStart = c.offset;
        for (const auto& ch : layer.channels) {
            const size_t start = c.offset;
            if (c.offset + static_cast<uint64_t>(std::max<int64_t>(0, ch.second)) > c.size()) {
                err = "truncated";
                return false;
            }
            const bool unpacked = (ch.first >= 0 && static_cast<size_t>(ch.first) < colorCount) ||
                                  ch.first == -1 || ch.first == -2;
            if (!unpacked || ch.second < 2) {
                c.offset = start + static_cast<size_t>(std::max<int64_t>(0, ch.second));
                continue;
            }
            uint16_t compression = 0;
            if (!c.u16(&compression)) { err = "truncated"; return false; }
            std::vector<uint8_t> payload(static_cast<size_t>(std::max<int64_t>(0, ch.second - 2)));
            if (!payload.empty() && !c.bytes(payload.data(), payload.size())) {
                err = "truncated";
                return false;
            }
            const bool isMask = ch.first == -2;
            Crop crop;
            bool cropValid = false;
            if (isMask) {
                cropValid = layer.maskCropValid;
                crop = Crop{layer.maskCropX, layer.maskCropY, layer.maskCropW, layer.maskCropH};
            } else {
                cropValid = layer.imageCropValid;
                crop = Crop{layer.imageCropX, layer.imageCropY, layer.imageCropW, layer.imageCropH};
            }
            std::vector<uint8_t> plane;
            std::string derr;
            if (deepActive) {
                // 深色路径：字节平面（大端样本）→ 8-bit 平面；裁剪按采样粒度换算。
                // P6 D1-b：RGB/Gray 16/32 位同步产出 f32 平面（层原生深度保真；CMYK/Lab
                // 维持受管降 8）。蒙版通道（-2）恒灰度 U8 域。
                const int srcW = isMask ? sourceMaskW : sourceW;
                const int srcH = isMask ? sourceMaskH : sourceH;
                int cropArr[4] = {crop.x, crop.y, crop.w, crop.h};
                auto rawPlane = decodeDeepChannel(compression, static_cast<size_t>(std::max(srcW, 0)),
                                                  static_cast<size_t>(std::max(srcH, 0)), depth, isPSB,
                                                  payload.data(), payload.size(),
                                                  cropValid ? cropArr : nullptr, derr);
                if (!rawPlane) {
                    err = derr.empty() ? "decode failed" : derr;
                    return false;
                }
                const int dstW = cropValid ? crop.w : srcW;
                const int dstH = cropValid ? crop.h : srcH;
                if (deepF32Wanted && !isMask) {
                    auto pf = planeToF32(rawPlane->data(), rawPlane->size(), depth,
                                         static_cast<size_t>(std::max(dstW, 0)),
                                         static_cast<size_t>(std::max(dstH, 0)), derr);
                    if (pf) {
                        deepF[ch.first] = std::move(*pf);
                    }
                }
                auto p8 = planeToU8(rawPlane->data(), rawPlane->size(), depth,
                                    static_cast<size_t>(std::max(dstW, 0)),
                                    static_cast<size_t>(std::max(dstH, 0)), derr);
                if (!p8) {
                    err = derr.empty() ? "decode failed" : derr;
                    return false;
                }
                plane = std::move(*p8);
            } else if (!decodeChannel(compression, isMask ? sourceMaskW : sourceW,
                               isMask ? sourceMaskH : sourceH, payload, isPSB,
                               cropValid ? &crop : nullptr, plane, derr)) {
                err = derr.empty() ? "decode failed" : derr;
                return false;
            }
            if ((isMask ? maskWidth : width) > 0 && (isMask ? maskHeight : height) > 0) {
                planes[ch.first] = std::move(plane);
            }
            c.offset = start + static_cast<size_t>(std::max<int64_t>(0, ch.second));
        }
        if (deepActive) {
            layer.srcSection.assign(data + sectionStart, data + c.offset);
            layer.srcCropped = layer.cropped;
            layer.srcSection.assign(data + sectionStart, data + c.offset);
            layer.srcCropped = layer.cropped;
        }
        if (layer.hasMask && maskWidth > 0 && maskHeight > 0) {
            auto it = planes.find(-2);
            if (it != planes.end() &&
                it->second.size() >= static_cast<size_t>(maskWidth) * maskHeight) {
                layer.maskImage = it->second;
            }
        }
        if (width <= 0 || height <= 0) {
            continue;
        }
        const size_t count2 = static_cast<size_t>(width) * height;
        std::vector<uint8_t> opaque(count2, 255);
        std::vector<uint8_t> black(count2, 0);
        const std::vector<uint8_t>& alpha = planes.count(-1) ? planes[-1] : opaque;
        // 预乘 RGBA（对拍 rgbaImage：(c*a+127)/255）
        layer.image.assign(count2 * 4, 0);
        if (deepActive) {
            // 模式语义序的 8-bit 色彩平面；缺平面按模式语义填充
            //（CMYK 255 = 无墨、Lab a/b 128 = 中性，photocraft 填充语义的 M22 扩展）。
            std::vector<std::vector<uint8_t>> colorPlanes(colorCount);
            for (size_t k = 0; k < colorCount; ++k) {
                auto it = planes.find(static_cast<int>(k));
                if (it != planes.end()) {
                    colorPlanes[k] = it->second;
                } else if (mode == 4) {
                    colorPlanes[k].assign(count2, 255);
                } else if (mode == 9 && k > 0) {
                    colorPlanes[k].assign(count2, 128);
                } else {
                    colorPlanes[k] = black;
                }
                if (colorPlanes[k].size() < count2) {
                    err = "truncated";
                    return false;
                }
            }
            std::string ierr;
            auto rgba = interleaveToRgba(mode, colorPlanes,
                                         planes.count(-1) ? &planes[-1] : nullptr,
                                         out.colorModeData.empty() ? nullptr : out.colorModeData.data(),
                                         count2, ierr);
            if (!rgba) {
                err = ierr.empty() ? "interleave failed" : ierr;
                return false;
            }
            for (size_t i = 0; i < count2; ++i) {
                const uint8_t a = (*rgba)[i * 4 + 3];
                layer.image[i * 4] = static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4]) * a + 127) / 255);
                layer.image[i * 4 + 1] = static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4 + 1]) * a + 127) / 255);
                layer.image[i * 4 + 2] = static_cast<uint8_t>((static_cast<uint16_t>((*rgba)[i * 4 + 2]) * a + 127) / 255);
                layer.image[i * 4 + 3] = a;
            }
            // P6 D1-b：深色保真装配（f32 预乘 RGBA 直 alpha；色彩通道齐 + 模式 RGB/Gray）
            if (deepF32Wanted && (mode == 1 || mode == 3) &&
                static_cast<int>(deepF.size()) >= static_cast<int>(colorCount)) {
                bool ok = true;
                layer.deepRgba.assign(count2 * 4, 0.0f);
                for (int k = 0; k < static_cast<int>(colorCount); ++k) {
                    auto it = deepF.find(k);
                    if (it == deepF.end() || it->second.size() < count2) {
                        ok = false;
                        break;
                    }
                }
                const std::vector<float>* alphaF =
                    deepF.count(-1) ? &deepF[-1] : nullptr;
                if (ok && (alphaF == nullptr || alphaF->size() >= count2)) {
                    for (size_t i = 0; i < count2; ++i) {
                        const float a = alphaF != nullptr ? (*alphaF)[i] : 1.0f;
                        if (mode == 1) {
                            const float g = deepF[0][i] * a;
                            layer.deepRgba[i * 4] = g;
                            layer.deepRgba[i * 4 + 1] = g;
                            layer.deepRgba[i * 4 + 2] = g;
                        } else {
                            for (int k = 0; k < static_cast<int>(colorCount); ++k) {
                                layer.deepRgba[i * 4 + static_cast<size_t>(k)] =
                                    deepF[static_cast<int>(k)][i] * a;
                            }
                        }
                        layer.deepRgba[i * 4 + 3] = a;
                    }
                    layer.deepValid = true;
                    layer.deepDepth = depth;
                } else {
                    layer.deepRgba.clear();
                }
            }
        } else {
        const std::vector<uint8_t>& red = planes.count(0) ? planes[0] : black;
        const std::vector<uint8_t>& green = planes.count(1) ? planes[1] : black;
        const std::vector<uint8_t>& blue = planes.count(2) ? planes[2] : black;
        if (red.size() < count2 || green.size() < count2 || blue.size() < count2 ||
            alpha.size() < count2) {
            err = "truncated";
            return false;
        }
        for (size_t i = 0; i < count2; ++i) {
            const uint8_t a = alpha[i];
            layer.image[i * 4] = static_cast<uint8_t>((static_cast<uint16_t>(red[i]) * a + 127) / 255);
            layer.image[i * 4 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(green[i]) * a + 127) / 255);
            layer.image[i * 4 + 2] = static_cast<uint8_t>((static_cast<uint16_t>(blue[i]) * a + 127) / 255);
            layer.image[i * 4 + 3] = a;
        }
        }
        if (deepActive) {
        // 导入渲染指纹：预乘像素 + 蒙版 + 几何（导出比对判定「未编辑」→ 段字节级回写）。
        uint64_t hsh = 1469598103934665603ull;
        auto fnvMix = [&hsh](const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                hsh ^= p[i];
                hsh *= 1099511628211ull;
            }
        };
        auto mixU64 = [&hsh](uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                hsh ^= static_cast<uint8_t>(v >> (8 * i));
                hsh *= 1099511628211ull;
            }
        };
        mixU64(static_cast<uint64_t>(layer.left));
        mixU64(static_cast<uint64_t>(layer.top));
        mixU64(static_cast<uint64_t>(width));
        mixU64(static_cast<uint64_t>(height));
        fnvMix(layer.image.data(), layer.image.size());
        if (!layer.maskImage.empty()) {
            mixU64(0xDEE9BA5Eull);
            fnvMix(layer.maskImage.data(), layer.maskImage.size());
            mixU64(static_cast<uint64_t>(layer.maskLeft));
            mixU64(static_cast<uint64_t>(layer.maskTop));
        }
        layer.importHash = hsh;
        }
    }
    }  // M22 deepActive/CmykScope 作用域
    // M21：全局附加块（通道数据后：global layer mask info + additional layer info 序列；
    // lnk2/lnk3/lnkD 内嵌文件 → linkedFiles，其余原样存 globalExtra 供导出透传）
    if (c.offset < layerSectionEnd && c.offset + 4 <= c.size()) {
        static const char* kGlobalLargeKeys[] = {
            "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
            "Alph", "FMsk", "lnk2", "lnk3", "lnkD", "FEid", "FXid", "PxSD",
        };
        uint32_t gmLen = 0;
        if (c.u32(&gmLen) && c.offset + gmLen <= layerSectionEnd && c.skip(gmLen)) {
            while (c.offset + 12 <= layerSectionEnd && c.offset + 12 <= c.size()) {
                std::string sig;
                std::string gkey;
                if (!c.string4(&sig) || (sig != "8BIM" && sig != "8B64")) {
                    break;
                }
                if (!c.string4(&gkey)) {
                    break;
                }
                bool glarge = sig == "8B64";
                if (!glarge) {
                    for (const char* k : kGlobalLargeKeys) {
                        if (gkey == k) {
                            glarge = true;
                            break;
                        }
                    }
                }
                int64_t glen = 0;
                if (glarge) {
                    uint64_t lv = 0;
                    if (!c.u64(&lv)) {
                        break;
                    }
                    glen = static_cast<int64_t>(lv);
                } else {
                    uint32_t lv = 0;
                    if (!c.u32(&lv)) {
                        break;
                    }
                    glen = static_cast<int64_t>(lv);
                }
                if (glen < 0 || c.offset + static_cast<uint64_t>(glen) > layerSectionEnd ||
                    c.offset + static_cast<uint64_t>(glen) > c.size()) {
                    break;
                }
                std::vector<uint8_t> payload(static_cast<size_t>(glen));
                if (glen > 0 && !c.bytes(payload.data(), payload.size())) {
                    break;
                }
                if (glen % 2 == 1 && !c.skip(1)) {
                    break;
                }
                if (gkey == "lnk2" || gkey == "lnk3" || gkey == "lnkD") {
                    auto lf = parseLinkedFiles(payload.data(), payload.size());
                    out.linkedFiles.insert(out.linkedFiles.end(), lf.begin(), lf.end());
                }
                out.globalExtra[gkey] = std::move(payload);
            }
        }
    }
    c.offset = layerSectionEnd;

    // 组装（对拍 assemble：组 divider 配对/组 pass 语义/不透明度/蒙版/kind 提示）。
    // M14a：bottom→top 序中组尾 divider（section 3）先出现压栈、组头（1/2）弹栈，
    // 子层挂栈顶组 → 真编组层级（parentGroup/groupId），不再扁平化。
    std::vector<uint64_t> groupIds;
    uint64_t idGen = 1;
    for (const RawLayer& layer : raw) {
        if (layer.section == 3) {
            groupIds.push_back(static_cast<uint64_t>(idGen++));
            continue;
        }
        const bool isGroup = layer.section == 1 || layer.section == 2;
        Record rec;
        rec.name = layer.name;
        rec.hasStyle = layer.hasStyle;   // M17c：lrFX 效果
        rec.style = layer.style;
        rec.srcSection = layer.srcSection;   // M22：深色往返元数据
        rec.importHash = layer.importHash;
        rec.srcCropped = layer.srcCropped;
        rec.srcChannels = layer.channels;
        rec.srcLeft = layer.sourceLeft;
        rec.srcTop = layer.sourceTop;
        rec.srcRight = layer.sourceRight;
        rec.srcBottom = layer.sourceBottom;
        rec.srcMaskLeft = layer.sourceMaskLeft;
        rec.srcMaskTop = layer.sourceMaskTop;
        rec.srcMaskRight = layer.sourceMaskRight;
        rec.srcMaskBottom = layer.sourceMaskBottom;
        // M18d：vmsk/vsms 矢量蒙版（u32 version=3 + u32 flags(bit0 invert/1 not-linked/2
        // disabled) + 路径记录；vmsk 读法 = 首子路径 subtract → inverted + 补对偶）
        for (const char* vk : {"vmsk", "vsms"}) {
            auto it = layer.extra.find(vk);
            if (it == layer.extra.end() || it->second.size() < 8) {
                continue;
            }
            const std::vector<uint8_t>& d = it->second;
            const uint32_t flags = (static_cast<uint32_t>(d[4]) << 24) | (static_cast<uint32_t>(d[5]) << 16) |
                                   (static_cast<uint32_t>(d[6]) << 8) | static_cast<uint32_t>(d[7]);
            VectorPath vp;
            std::vector<uint8_t> records(d.begin() + 8, d.end());
            if (pathFromPsdRecords(records, static_cast<uint32_t>(out.width),
                                   static_cast<uint32_t>(out.height), true, vp)) {
                auto vmk = std::make_shared<VectorMaskData>();
                vmk->path = std::move(vp);
                vmk->path.inverted = vmk->path.inverted || (flags & 1u) != 0;
                vmk->enabled = (flags & 4u) == 0;
                vmk->linked = (flags & 2u) == 0;
                rec.vectorMask = std::move(vmk);
            } else if ((flags & 1u) != 0) {
                // 无子路径 + invert = 反转全显（PS 初始填充语义）
                auto vmk = std::make_shared<VectorMaskData>();
                vmk->path.inverted = true;
                vmk->enabled = (flags & 4u) == 0;
                rec.vectorMask = std::move(vmk);
            }
            break;
        }
        for (const std::string& k : layer.skippedFx) {
            if (k == "lfx2-only") {
                rec.notes.push_back("Layer effects in lfx2 format only were skipped.");
            } else if (k.size() > 6 && k.substr(k.size() - 6) == "-blend") {
                rec.notes.push_back("Layer effect " + k.substr(0, k.size() - 6) +
                                    " uses a non-normal blend; painted as normal.");
            } else {
                rec.notes.push_back("Layer effect " + k + " was skipped.");
            }
        }
        rec.isGroup = isGroup;
        rec.parentGroup = groupIds.empty() ? 0 : static_cast<int>(groupIds.back());
        rec.visible = !layer.hidden;
        rec.blendKey = layer.blendKey;
        rec.blendMode = blendFromKey(layer.blendKey, &rec.blendExact);
        rec.clipping = layer.clipping;
        rec.croppedToCanvas = layer.cropped;
        rec.kind = kindOf(layer, isGroup);
        const bool hasEffects = rec.kind == LayerKind::Effects ||
                                hasAnyKey(layer, {"lfx2", "lrFX", "lmfx"});
        if (hasEffects && layer.fill != 255) {
            rec.opacity = static_cast<double>(layer.opacity) / 255.0;
        } else {
            rec.opacity = (static_cast<double>(layer.opacity) / 255.0) *
                          (static_cast<double>(layer.fill) / 255.0);
        }
        if (isGroup) {
            // 组记录（M14a）：groupId = pop 前栈顶（自己的号，子层经 parentGroup 引用）；
            // parentGroup = pop 后栈顶（父组）。pass 键 = PS 穿透 → 引擎 Normal+不透明 1。
            rec.groupId = groupIds.empty() ? 0 : static_cast<int>(groupIds.back());
            rec.parentGroup = groupIds.size() >= 2
                                  ? static_cast<int>(groupIds[groupIds.size() - 2])
                                  : 0;
            rec.width = 0;
            rec.height = 0;
            if (layer.blendKey == "pass") {
                rec.blendMode = BlendMode::Normal;
                rec.blendExact = true;
            }
            if (!groupIds.empty()) {
                groupIds.pop_back();  // 配对 divider
            }
        } else {
            const int width = std::max(0, layer.right - layer.left);
            const int height = std::max(0, layer.bottom - layer.top);
            rec.left = layer.left;
            rec.top = layer.top;
            rec.width = width;
            rec.height = height;
            rec.rgba = layer.image;
            rec.deepRgba = layer.deepRgba;
            rec.deepValid = layer.deepValid;
            rec.deepDepth = layer.deepDepth;
            rec.hasMask = layer.hasMask;
            rec.maskLeft = layer.maskLeft;
            rec.maskTop = layer.maskTop;
            rec.maskW = std::max(0, layer.maskRight - layer.maskLeft);
            rec.maskH = std::max(0, layer.maskBottom - layer.maskTop);
            rec.mask = layer.maskImage;
            rec.maskDefault = layer.maskDefault;
            rec.maskEnabled = !layer.maskDisabled;
            if (rec.kind == LayerKind::Text) {
                rec.notes.push_back("Text layer was rasterized to pixels.");
            } else if (rec.kind == LayerKind::Vector) {
                rec.notes.push_back("Vector shape was rasterized to pixels.");
            } else if (rec.kind == LayerKind::SmartObject) {
                // M21：placed 块原始数据（psd_import 建 SO 层；成功时移除本 note）
                for (const char* pk : {"SoLd", "SoLE", "PlLd"}) {
                    auto pit = layer.extra.find(pk);
                    if (pit != layer.extra.end() && !pit->second.empty()) {
                        rec.placedKey = pk;
                        rec.placedRaw = pit->second;
                        break;
                    }
                }
                rec.notes.push_back("The smart object was rasterized.");
            } else if (rec.kind == LayerKind::Effects) {
                rec.notes.push_back("Layer effects were discarded.");
            } else if (rec.kind == LayerKind::Adjustment) {
                // M9d：levl/curv/nvrt 可映射非破坏（payload 提取给 import）；其余仍跳过
                static const std::vector<const char*> kMappable = {"levl", "curv", "nvrt"};
                bool extracted = false;
                for (const char* k : kMappable) {
                    auto it = layer.extra.find(k);
                    if (it != layer.extra.end()) {
                        rec.adjKey = k;
                        rec.adjPayload = it->second;
                        extracted = true;
                        break;
                    }
                }
                if (!extracted) {
                    rec.notes.push_back("Adjustment layer was skipped.");
                }
            }
            if (!rec.blendExact) {
                rec.notes.push_back("Blend mode \"" + layer.blendKey + "\" applied as Normal.");
            }
        }
        out.layers.push_back(std::move(rec));
    }
    if (!groupIds.empty()) {
        err = "truncated";
        return false;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "parsed %{public}dx%{public}d layers=%{public}zu", out.width, out.height,
                 out.layers.size());
    return true;
}

}  // namespace psd
}  // namespace montage
