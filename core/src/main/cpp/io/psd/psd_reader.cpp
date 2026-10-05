#include "io/psd/psd_reader.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <hilog/log.h>

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
    // 裁剪（画布越界预算兜底，对拍 PSDCrop/cropped）
    bool imageCropValid = false;
    int imageCropX = 0, imageCropY = 0, imageCropW = 0, imageCropH = 0;
    bool maskCropValid = false;
    int maskCropX = 0, maskCropY = 0, maskCropW = 0, maskCropH = 0;
    bool cropped = false;
};

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
            std::memcpy(plane + static_cast<size_t>(row - crop->y) * cw + crop->x,
                        rowBuf.data() + crop->x, static_cast<size_t>(crop->w));
        }
    }
    return true;
}

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
    if (depth != 8) { err = "only 8-bit RGB PSD can be imported"; return false; }
    if (mode != 3) { err = "only 8-bit RGB PSD can be imported"; return false; }

    uint32_t colorModeLen = 0;
    if (!c.u32(&colorModeLen)) { err = "truncated"; return false; }
    if (!c.skip(colorModeLen)) { err = "truncated"; return false; }

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
        if (!c.skip(nameLength)) { err = "truncated"; return false; }
        if ((nameLength + 1) % 2 == 1 && !c.skip(1)) { err = "truncated"; return false; }
        uint32_t len = 0;
        if (!c.u32(&len)) { err = "truncated"; return false; }
        const uint64_t dataStart = c.offset;
        if (id == 1005 && len >= 4) {
            uint32_t fixed = 0;
            if (!c.u32(&fixed)) { err = "truncated"; return false; }
            double res = static_cast<double>(fixed) / 65536.0;
            if (!std::isfinite(res) || res < 1) {
                res = 72;
            }
            out.resolution = std::min(9600.0, std::max(1.0, res));
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
        return true;  // 无图层段（扁平 PSD）
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

    // 通道解码（对拍 decodeChannels；源尺寸解码 + 目标裁剪）
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
        for (const auto& ch : layer.channels) {
            const size_t start = c.offset;
            if (c.offset + static_cast<uint64_t>(std::max<int64_t>(0, ch.second)) > c.size()) {
                err = "truncated";
                return false;
            }
            const bool unpacked = ch.first == -1 || ch.first == 0 || ch.first == 1 || ch.first == 2 ||
                                  ch.first == -2;
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
            if (!decodeChannel(compression, isMask ? sourceMaskW : sourceW,
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
        const std::vector<uint8_t>& red = planes.count(0) ? planes[0] : black;
        const std::vector<uint8_t>& green = planes.count(1) ? planes[1] : black;
        const std::vector<uint8_t>& blue = planes.count(2) ? planes[2] : black;
        const std::vector<uint8_t>& alpha = planes.count(-1) ? planes[-1] : opaque;
        if (red.size() < count2 || green.size() < count2 || blue.size() < count2 ||
            alpha.size() < count2) {
            err = "truncated";
            return false;
        }
        // 预乘 RGBA（对拍 rgbaImage：(c*a+127)/255）
        layer.image.assign(count2 * 4, 0);
        for (size_t i = 0; i < count2; ++i) {
            const uint8_t a = alpha[i];
            layer.image[i * 4] = static_cast<uint8_t>((static_cast<uint16_t>(red[i]) * a + 127) / 255);
            layer.image[i * 4 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(green[i]) * a + 127) / 255);
            layer.image[i * 4 + 2] = static_cast<uint8_t>((static_cast<uint16_t>(blue[i]) * a + 127) / 255);
            layer.image[i * 4 + 3] = a;
        }
    }
    c.offset = layerSectionEnd;

    // 组装（对拍 assemble：组 divider 配对/组 pass 语义/不透明度/蒙版/kind 提示）
    std::vector<uint64_t> groupIds;
    uint64_t idGen = 1;
    for (const RawLayer& layer : raw) {
        if (layer.section == 3) {
            groupIds.push_back(idGen++);
            continue;
        }
        const bool isGroup = layer.section == 1 || layer.section == 2;
        Record rec;
        rec.name = layer.name;
        rec.isGroup = isGroup;
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
            // 组记录：M2 引擎无组层级 → 面板显示 + 提示扁平化
            rec.width = 0;
            rec.height = 0;
            rec.notes.push_back("Group flattened; layers were kept.");
            if (layer.blendKey != "pass" && layer.blendKey != "norm") {
                rec.notes.push_back("Folder blend mode not supported; treated as pass-through.");
            }
            rec.blendMode = BlendMode::Normal;
            rec.blendExact = true;
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
                rec.notes.push_back("The smart object was rasterized.");
            } else if (rec.kind == LayerKind::Effects) {
                rec.notes.push_back("Layer effects were discarded.");
            } else if (rec.kind == LayerKind::Adjustment) {
                rec.notes.push_back("Adjustment layer was skipped.");
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
