#include "io/project_save.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <zlib.h>

#include <hilog/log.h>

#include "engine/document.h"

#include "io/psdsrc_blob.h"
#include "engine/vector_render.h"
#include "engine/engine.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Save";
constexpr const char* kFormatId = "com.sytkiller.montage.project";
constexpr int kFormatVersion = 15;  // v15：+形状填充 stops 渐变/图案（fillStops/fillAngleDeg/fillPatternId/fillPatScale，P1，非默认才写 = v14 包字节不变）；v14：+深色 PSD 往返（文档 psd 块 + 层 psdSrc blob，M22，无深色数据 = 字节兼容 v13）；v13：+智能对象（smart 块 + 包内源文件/滤镜蒙版 PNG，M21，无 SO 层 = 字节兼容 v12）；v12：+形状层/矢量蒙版/工作路径/保存路径/自定义形状（M18）；v11：+等高线/Range/图案叠加/文档图案（M17d）；v10：+图层样式效果扩展（M17a 内阴影/外发光/内发光/渐变叠加，非默认才写）；v9：+参考线 guides 块（M15a，空则不写兼容 v8 包）；v8：+图层样式/填充图层（M14c）；v7：+图层组（M14a）；v6：+文本层（M12）；v5：+Alpha 通道；v4：调整层 kind 扩充+p0..p3；v3：+调整层；v2：+蒙版
constexpr int kDeflateLevel = 6;
// 04 §1.3 格式上限（沿用上游档位，与引擎运行预算分立）
constexpr uint32_t kMaxSide = 30000;
constexpr uint64_t kMaxTotalPixels = 100ull * 1000 * 1000;
constexpr size_t kMaxLayers = 10000;
constexpr size_t kMaxManifestBytes = 4 * 1024 * 1024;

// ---------- fd 写原语 ----------

bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool pwriteAllAt(int fd, const uint8_t* p, size_t n, uint64_t at) {
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, static_cast<off_t>(at));
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
        at += static_cast<uint64_t>(w);
    }
    return true;
}

void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> (i * 8));
    }
}

void putU32BE(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> ((3 - i) * 8));
    }
}

// ---------- zip 写出（流式单遍；条目先写占位本地头，收尾回填；不做 zip64） ----------
// M21：写出目标抽象化（fd 流式 / 内存包）——智能对象源 = 嵌套 .montage 内存编码复用同一条
// 序列化路径（唯一来源，防 fd/内存双实现漂移）。

class ZipSink {
  public:
    virtual ~ZipSink() = default;
    virtual bool write(const uint8_t* p, size_t n) = 0;
    virtual bool writeAt(uint64_t at, const uint8_t* p, size_t n) = 0;
};

class FdZipSink : public ZipSink {
  public:
    explicit FdZipSink(int fd) : fd_(fd) {}
    bool write(const uint8_t* p, size_t n) override { return writeAll(fd_, p, n); }
    bool writeAt(uint64_t at, const uint8_t* p, size_t n) override {
        return pwriteAllAt(fd_, p, n, at);
    }

  private:
    int fd_;
};

class MemZipSink : public ZipSink {
  public:
    explicit MemZipSink(std::vector<uint8_t>& out) : out_(out) {}
    bool write(const uint8_t* p, size_t n) override {
        out_.insert(out_.end(), p, p + n);
        return true;
    }
    bool writeAt(uint64_t at, const uint8_t* p, size_t n) override {
        if (out_.size() < at + n) {
            out_.resize(static_cast<size_t>(at + n));
        }
        std::memcpy(out_.data() + at, p, n);
        return true;
    }

  private:
    std::vector<uint8_t>& out_;
};

class ZipWriter {
  public:
    explicit ZipWriter(ZipSink& sink) : sink_(sink) {}

    uint64_t totalBytes() const { return tell_; }

    // 开一个条目（占位本地头，收尾回填）。PNG 等 deflate 已压缩载荷用 STORE(0)，manifest 也 STORE。
    bool beginEntry(const std::string& name, uint16_t method) {
        streamValid_ = false;
        if (method == 8) {
            if (deflateInit2(&zs_, kDeflateLevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                return false;
            }
            streamValid_ = true;
        }
        entryOffset_ = tell_;
        entryMethod_ = method;
        pendingName_ = name;
        crc_ = crc32(0L, Z_NULL, 0);
        usize_ = 0;
        csize_ = 0;
        if (!writeLocalHeader(name, 0, 0, 0, method)) {
            return false;
        }
        if (method == 8) {
            out_.resize(kChunk);
            zs_.next_out = out_.data();
            zs_.avail_out = static_cast<uInt>(out_.size());
        }
        return true;
    }

    // 条目载荷字节（STORE 直写；DEFLATE 经流压缩）
    bool writeEntryBytes(const uint8_t* p, size_t n) {
        crc_ = crc32(crc_, p, static_cast<uInt>(n));
        usize_ += n;
        if (entryMethod_ == 0) {
            if (!sink_.write(p, n)) {
                return false;
            }
            csize_ += n;
            tell_ += n;
            return true;
        }
        zs_.next_in = const_cast<Bytef*>(p);
        zs_.avail_in = static_cast<uInt>(n);
        return pump(Z_NO_FLUSH);
    }

    bool endEntry() {
        if (entryMethod_ == 8) {
            if (!pump(Z_FINISH)) {
                return false;
            }
            deflateEnd(&zs_);
            streamValid_ = false;
        }
        uint8_t patch[12];
        putU32(patch + 0, crc_);
        putU32(patch + 4, static_cast<uint32_t>(csize_));
        putU32(patch + 8, static_cast<uint32_t>(usize_));
        if (!sink_.writeAt(entryOffset_ + 14, patch, sizeof(patch))) {
            return false;
        }
        Entry e;
        e.name = pendingName_;
        e.offset = entryOffset_;
        e.crc = crc_;
        e.csize = csize_;
        e.usize = usize_;
        e.method = entryMethod_;
        entries_.push_back(e);
        return true;
    }

    bool finish() {
        const uint64_t cdOffset = tell_;
        for (const Entry& e : entries_) {
            uint8_t ch[46];
            putU32(ch + 0, 0x02014b50u);
            putU16(ch + 4, 20);   // version made by
            putU16(ch + 6, 20);   // version needed
            putU16(ch + 8, 0);    // flags
            putU16(ch + 10, e.method);
            putU16(ch + 12, 0);        // dos time 00:00:00
            putU16(ch + 14, 0x21);     // dos date 1980-01-01
            putU32(ch + 16, e.crc);
            putU32(ch + 20, static_cast<uint32_t>(e.csize));
            putU32(ch + 24, static_cast<uint32_t>(e.usize));
            putU16(ch + 28, static_cast<uint16_t>(e.name.size()));
            putU16(ch + 30, 0);
            putU16(ch + 32, 0);
            putU16(ch + 34, 0);
            putU16(ch + 36, 0);
            putU32(ch + 38, 0);
            putU32(ch + 42, static_cast<uint32_t>(e.offset));
            if (!sink_.write(ch, sizeof(ch)) ||
                !sink_.write(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size())) {
                return false;
            }
            tell_ += sizeof(ch) + e.name.size();
        }
        const uint64_t cdSize = tell_ - cdOffset;
        uint8_t eo[22];
        putU32(eo + 0, 0x06054b50u);
        putU16(eo + 4, 0);
        putU16(eo + 6, 0);
        putU16(eo + 8, static_cast<uint16_t>(entries_.size()));
        putU16(eo + 10, static_cast<uint16_t>(entries_.size()));
        putU32(eo + 12, static_cast<uint32_t>(cdSize));
        putU32(eo + 16, static_cast<uint32_t>(cdOffset));
        putU16(eo + 20, 0);
        if (!sink_.write(eo, sizeof(eo))) {
            return false;
        }
        tell_ += sizeof(eo);
        return true;
    }

    ~ZipWriter() {
        if (streamValid_) {
            deflateEnd(&zs_);
        }
    }

  private:
    static constexpr size_t kChunk = 64 * 1024;
    struct Entry {
        std::string name;
        uint64_t offset = 0;
        uint32_t crc = 0;
        uint64_t csize = 0;
        uint64_t usize = 0;
        uint16_t method = 0;
    };

    bool writeLocalHeader(const std::string& name, uint32_t crc, uint64_t usize, uint64_t csize,
                          uint16_t method) {
        uint8_t h[30];
        putU32(h + 0, 0x04034b50u);
        putU16(h + 4, 20);
        putU16(h + 6, 0);
        putU16(h + 8, method);
        putU16(h + 10, 0);     // dos time
        putU16(h + 12, 0x21);  // dos date 1980-01-01
        putU32(h + 14, crc);
        putU32(h + 18, static_cast<uint32_t>(csize));
        putU32(h + 22, static_cast<uint32_t>(usize));
        putU16(h + 26, static_cast<uint16_t>(name.size()));
        putU16(h + 28, 0);
        if (!sink_.write(h, sizeof(h)) ||
            !sink_.write(reinterpret_cast<const uint8_t*>(name.data()), name.size())) {
            return false;
        }
        tell_ += sizeof(h) + name.size();
        return true;
    }

    // 压缩字节灌满 kChunk 即落盘（zlib 输出与 fd 写解耦于输入节奏）
    bool pump(int flush) {
        for (;;) {
            const int rc = deflate(&zs_, flush);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                return false;
            }
            const size_t produced = out_.size() - zs_.avail_out;
            if (produced > 0) {
                if (!sink_.write(out_.data(), produced)) {
                    return false;
                }
                csize_ += produced;
                tell_ += produced;
                zs_.next_out = out_.data();
                zs_.avail_out = static_cast<uInt>(out_.size());
            }
            if (rc == Z_STREAM_END) {
                return true;
            }
            if (zs_.avail_out == 0) {
                continue;
            }
            if (flush == Z_NO_FLUSH) {
                return zs_.avail_in == 0;
            }
            if (rc == Z_BUF_ERROR && produced == 0) {
                return false;
            }
        }
    }

    ZipSink& sink_;
    std::vector<Entry> entries_;
    uint64_t tell_ = 0;
    uint64_t entryOffset_ = 0;
    uint16_t entryMethod_ = 0;
    z_stream zs_ {};
    bool streamValid_ = false;
    std::string pendingName_;
    uint32_t crc_ = 0;
    uint64_t usize_ = 0;
    uint64_t csize_ = 0;
    std::vector<uint8_t> out_;
};

// ---------- PNG 行编码器（RGBA8、filter 0、多 IDAT 流式，防大画布整块驻留） ----------

class PngRowEncoder {
  public:
    PngRowEncoder(ZipWriter& zip, uint32_t width, uint32_t height)
        : zip_(zip), width_(width), height_(height) {}

    bool begin() {
        static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (!raw(kSig, sizeof(kSig))) {
            return false;
        }
        uint8_t ihdr[13];
        putU32BE(ihdr + 0, width_);
        putU32BE(ihdr + 4, height_);
        ihdr[8] = 8;   // bit depth
        ihdr[9] = 6;   // color type RGBA
        ihdr[10] = 0;  // deflate
        ihdr[11] = 0;  // filter 0（逐行 adaptive 留优化空间）
        ihdr[12] = 0;  // interlace none
        if (!chunk("IHDR", ihdr, sizeof(ihdr))) {
            return false;
        }
        if (deflateInit(&zs_, kDeflateLevel) != Z_OK) {
            return false;
        }
        streamValid_ = true;
        out_.resize(kChunk);
        zs_.next_out = out_.data();
        zs_.avail_out = static_cast<uInt>(out_.size());
        return true;
    }

    // 一行 RGBA 像素（宽度须等于 width_*4；行前拼 filter 0 字节）
    bool addRow(const uint8_t* rgba) {
        rowBuf_.resize(1 + static_cast<size_t>(width_) * 4u);
        rowBuf_[0] = 0;
        std::memcpy(rowBuf_.data() + 1, rgba, static_cast<size_t>(width_) * 4u);
        zs_.next_in = rowBuf_.data();
        zs_.avail_in = static_cast<uInt>(rowBuf_.size());
        return pumpIdat(Z_NO_FLUSH);
    }

    bool finish() {
        if (!pumpIdat(Z_FINISH)) {
            return false;
        }
        deflateEnd(&zs_);
        streamValid_ = false;
        return chunk("IEND", nullptr, 0);
    }

    ~PngRowEncoder() {
        if (streamValid_) {
            deflateEnd(&zs_);
        }
    }

  private:
    static constexpr size_t kChunk = 64 * 1024;

    // PNG 字节流直接进 zip 条目（STORE，PNG 自身已 deflate）
    bool raw(const uint8_t* p, size_t n) { return zip_.writeEntryBytes(p, n); }

    bool chunk(const char* type, const uint8_t* data, size_t n) {
        uint8_t hdr[8];
        putU32BE(hdr, static_cast<uint32_t>(n));
        std::memcpy(hdr + 4, type, 4);
        uint32_t crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
        if (n > 0) {
            crc = crc32(crc, data, static_cast<uInt>(n));
        }
        uint8_t crcb[4];
        putU32BE(crcb, crc);
        return raw(hdr, sizeof(hdr)) && (n == 0 || raw(data, n)) && raw(crcb, sizeof(crcb));
    }

    // deflate 产出的压缩字节按 ≤kChunk 分片，每片一个 IDAT（spec 允许多 IDAT 顺序拼接）
    bool emitIdat(const uint8_t* p, size_t n) {
        uint8_t hdr[8];
        putU32BE(hdr, static_cast<uint32_t>(n));
        std::memcpy(hdr + 4, "IDAT", 4);
        uint32_t crc = crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>("IDAT"), 4);
        crc = crc32(crc, p, static_cast<uInt>(n));
        uint8_t crcb[4];
        putU32BE(crcb, crc);
        return raw(hdr, sizeof(hdr)) && raw(p, n) && raw(crcb, sizeof(crcb));
    }

    bool pumpIdat(int flush) {
        for (;;) {
            const int rc = deflate(&zs_, flush);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                return false;
            }
            const size_t produced = out_.size() - zs_.avail_out;
            if (produced > 0) {
                if (!emitIdat(out_.data(), produced)) {
                    return false;
                }
                zs_.next_out = out_.data();
                zs_.avail_out = static_cast<uInt>(out_.size());
            }
            if (rc == Z_STREAM_END) {
                return true;
            }
            if (zs_.avail_out == 0) {
                continue;
            }
            if (flush == Z_NO_FLUSH) {
                if (zs_.avail_in != 0) {
                    continue;
                }
                return true;
            }
            if (rc == Z_BUF_ERROR && produced == 0) {
                return false;
            }
        }
    }

    ZipWriter& zip_;
    uint32_t width_;
    uint32_t height_;
    z_stream zs_ {};
    bool streamValid_ = false;
    std::vector<uint8_t> out_;
    std::vector<uint8_t> rowBuf_;
};

// ---------- 快照与 manifest ----------

struct LayerSnapshot {
    LayerId id = 0;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    std::shared_ptr<const TileGrid> pixels;  // null = 空图层
    double originX = 0.0;
    double originY = 0.0;
    // 内容包围盒（图层局部像素坐标）
    uint32_t px = 0;
    uint32_t py = 0;
    uint32_t pw = 0;
    uint32_t ph = 0;
    // M5a 蒙版
    std::shared_ptr<const TileGrid> maskGrid;  // patch 网格（R=G=B=gray）
    uint32_t maskW = 0;
    uint32_t maskH = 0;
    int maskOffsetX = 0;
    int maskOffsetY = 0;
    int maskOutside = 255;
    bool maskEnabled = true;
    bool clipping = false;  // M5b-2
    std::shared_ptr<LayerAdjustment> adjustment;  // M6a：null = 普通层
    std::shared_ptr<TextLayerState> text;  // M12：null = 非文本层
    // M14a 组（平铺栈）
    bool isGroup = false;
    uint64_t parentId = 0;
    bool collapsed = false;
    // M14c 图层样式 / 填充图层（null = 无）
    std::shared_ptr<LayerStyle> style;
    std::shared_ptr<FillLayerState> fill;
    // M18 形状层 / 矢量蒙版（null = 无；pixels 已存 shapeCache 派生结果）
    std::shared_ptr<ShapeLayerState> shape;
    std::shared_ptr<VectorMaskData> vectorMask;
    bool vectorMaskEnabled = true;
    // M21 智能对象（null = 非 SO 层；pixels 已存 smartCache 派生结果、origin = transform）
    // 状态指针共享（源字节不经拷贝；COW 快照同指针）
    std::shared_ptr<const SmartLayerState> smart;
    // M22 v14：深色 PSD 层 verbatim blob（自包含，空 = 无；无深色数据层不写 = v13 包不变）
    std::shared_ptr<std::vector<uint8_t>> psdSrcBlob;
};

struct ChannelSnapshot {
    std::string name;
    std::shared_ptr<const TileGrid> mask;  // 文档域灰度网格
};

struct PatternSnapshot {  // v11：文档图案（M17d）
    std::string id;
    std::string name;
    uint32_t w = 0;
    uint32_t h = 0;
    std::shared_ptr<const TileGrid> pixels;
};

struct DocSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string name;
    LayerId activeId = 0;
    std::vector<LayerSnapshot> layers;
    std::vector<ChannelSnapshot> channels;  // v5：Alpha 通道（存储选区）
    // v9：参考线（M15a 会话态；空且全默认则整块不写——v8 及以下包字节不变）
    std::vector<float> guidesH;
    std::vector<float> guidesV;
    bool guidesLocked = false;
    bool guidesVisible = true;
    double globalLightAngle = 120.0;  // v10（M17b）
    std::vector<PatternSnapshot> patterns;  // v11（M17d；空则整块不写——v10 包字节不变）
    // v12（M18 矢量；空则整块不写——v11 包字节不变）
    VectorPath workPath;
    std::vector<NamedPathEntry> paths;
    std::vector<CustomShapeEntry> customShapes;
    uint32_t customShapeSeq = 0;
    // v14（M22 深色 PSD 往返；全空不写 = v13 包字节不变）
    uint16_t psdSrcDepth = 8;
    uint16_t psdSrcMode = 3;
    uint16_t psdSrcChannels = 3;
    std::shared_ptr<std::vector<uint8_t>> psdIcc;        // images/psdicc.bin（资源 1039 原样）
    std::shared_ptr<std::vector<uint8_t>> psdColorMode;  // images/psdcmode.bin（色板段原样）
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> psdGlobalBlocks;
};

// M17d 等高线 JSON（非 custom 不写）：{"pts": [x0,y0,…], "corners": [0/1,…]}
std::string contourJson(const StyleContour& c) {
    if (!c.custom || c.pts.size() < 4) {
        return "";
    }
    char num[32];
    std::string j = "{\"pts\": [";
    for (size_t i = 0; i < c.pts.size(); ++i) {
        std::snprintf(num, sizeof(num), "%.6g", c.pts[i]);
        j += num;
        if (i + 1 < c.pts.size()) {
            j += ", ";
        }
    }
    j += "], \"corners\": [";
    for (size_t i = 0; i < c.corners.size(); ++i) {
        j += (c.corners[i] != 0) ? "1" : "0";
        if (i + 1 < c.corners.size()) {
            j += ", ";
        }
    }
    j += "]}";
    return j;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[7];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c));  // UTF-8 原样透传
                }
        }
    }
    out.push_back('"');
    return out;
}

// 内容包围盒 = 非空瓦片并集（瓦片恒 256×256、零填充；缺瓦片视为透明）
bool computeBbox(const TileGrid& grid, uint32_t& px, uint32_t& py, uint32_t& pw, uint32_t& ph) {
    uint32_t tx0 = UINT32_MAX;
    uint32_t ty0 = UINT32_MAX;
    uint32_t tx1 = 0;
    uint32_t ty1 = 0;
    for (const auto& [idx, tile] : grid.tiles) {
        const uint32_t tx = idx % grid.cols;
        const uint32_t ty = idx / grid.cols;
        tx0 = std::min(tx0, tx);
        ty0 = std::min(ty0, ty);
        tx1 = std::max(tx1, tx);
        ty1 = std::max(ty1, ty);
    }
    if (tx0 == UINT32_MAX) {
        return false;
    }
    px = tx0 * kTileSize;
    py = ty0 * kTileSize;
    pw = (tx1 - tx0 + 1u) * kTileSize;
    ph = (ty1 - ty0 + 1u) * kTileSize;
    return true;
}

// ---- M18 v12 路径序列化（扁平数组，坐标 %.17g 保 double 往返） ----

std::string jsonNumber(double v) {
    char num[40];
    std::snprintf(num, sizeof(num), "%.17g", v);
    return std::string(num);
}

std::string pathToJson(const VectorPath& p, const char* indent) {
    // [子路径数, (closed, op, knot数, [ax,ay,ix,iy,ox,oy,smooth])×n]
    std::string j = "[";
    j += std::to_string(p.subpaths.size());
    for (const Subpath& sp : p.subpaths) {
        j += std::string(",\n") + indent + (sp.closed ? "1" : "0");
        j += std::string(", ") + std::to_string(static_cast<int32_t>(sp.op));
        j += ", " + std::to_string(sp.knots.size());
        for (const Knot& kn : sp.knots) {
            j += std::string(",\n") + indent;
            const double vals[7] = {kn.anchor.x, kn.anchor.y, kn.inCtrl.x, kn.inCtrl.y,
                                    kn.outCtrl.x, kn.outCtrl.y, kn.smooth ? 1.0 : 0.0};
            for (int i = 0; i < 7; i++) {
                j += jsonNumber(vals[i]);
                if (i < 6) {
                    j += ", ";
                }
            }
        }
    }
    j += "\n" + std::string(indent) + "]";
    return j;
}

std::string buildManifest(const DocSnapshot& doc) {
    std::string j = "{\n";
    j += std::string("  \"format\": \"") + kFormatId + "\",\n";
    j += "  \"version\": " + std::to_string(kFormatVersion) + ",\n";
    j += "  \"colorSpace\": \"sRGB\",\n";
    j += "  \"name\": " + jsonEscape(doc.name) + ",\n";
    j += "  \"width\": " + std::to_string(doc.width) + ",\n";
    j += "  \"height\": " + std::to_string(doc.height) + ",\n";
    j += "  \"activeLayerId\": " + std::to_string(doc.activeId) + ",\n";
    j += "  \"layers\": [\n";
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        const LayerSnapshot& l = doc.layers[i];
        std::vector<std::string> fields;
        fields.push_back("      \"id\": " + std::to_string(l.id));
        fields.push_back("      \"name\": " + jsonEscape(l.name));
        fields.push_back(std::string("      \"visible\": ") + (l.visible ? "true" : "false"));
        char num[32];
        std::snprintf(num, sizeof(num), "%.6g", l.opacity);
        fields.push_back("      \"opacity\": " + std::string(num));
        fields.push_back("      \"blendMode\": " + jsonEscape(blendModeName(l.blendMode)));
        if (l.clipping) {
            fields.push_back("      \"clipping\": true");
        }
        if (l.adjustment != nullptr) {
            const LayerAdjustment& adj = *l.adjustment;
            std::string fields2;
            char num[32];
            const char* nl = "\n";
            const char* kindStr = "levels";
            switch (adj.kind) {
                case AdjustmentKind::Curves: kindStr = "curves"; break;
                case AdjustmentKind::BrightnessContrast: kindStr = "bc"; break;
                case AdjustmentKind::Invert: kindStr = "invert"; break;
                case AdjustmentKind::Threshold: kindStr = "threshold"; break;
                case AdjustmentKind::Posterize: kindStr = "posterize"; break;
                case AdjustmentKind::ColorBalance: kindStr = "colorbalance"; break;
                case AdjustmentKind::HueSaturation: kindStr = "huesat"; break;
                default: kindStr = "levels"; break;
            }
            // 审查修复：kind 串漏引号 → manifest 非法 JSON，任何含调整层的工程 open 必败
            //（宿主往返测试 rt_fix1.montage 实证 "kind": levels）
            fields2 += std::string("      \"kind\": \"") + kindStr + "\"," + nl;
            std::snprintf(num, sizeof(num), "%.6g", adj.inBlack);
            fields2 += std::string("      \"inBlack\": ") + num + "," + nl;
            std::snprintf(num, sizeof(num), "%.6g", adj.inWhite);
            fields2 += std::string("      \"inWhite\": ") + num + "," + nl;
            std::snprintf(num, sizeof(num), "%.6g", adj.gamma);
            fields2 += std::string("      \"gamma\": ") + num;
            for (int pi = 0; pi < 4; ++pi) {
                float pv = (&adj.p0)[pi];
                std::snprintf(num, sizeof(num), "%.6g", pv);
                fields2 += std::string(",") + nl + "      \"p" + std::to_string(pi) + "\": " + num;
            }
            // 审查修复：曲线点此前从未落盘（open 侧有读取但 save 漏写）→ 曲线参数存后即丢
            if (adj.kind == AdjustmentKind::Curves && !adj.curveX.empty() &&
                adj.curveX.size() == adj.curveY.size() && adj.curveX.size() <= 64) {
                std::string cx, cy;
                for (size_t ci = 0; ci < adj.curveX.size(); ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", adj.curveX[ci]);
                    cx += (ci != 0 ? std::string(", ") : std::string()) + num;
                    std::snprintf(num, sizeof(num), "%.6g", adj.curveY[ci]);
                    cy += (ci != 0 ? std::string(", ") : std::string()) + num;
                }
                fields2 += std::string(",") + nl + "      \"curveX\": [" + cx + "]";
                fields2 += std::string(",") + nl + "      \"curveY\": [" + cy + "]";
            }
            fields.push_back(std::string("      \"adjustment\": {") + nl + fields2 + nl + "      }");
        }
        if (l.isGroup || l.parentId != 0 || l.collapsed) {
            // v7 组字段（非默认才写；读取方按字段存在性兼容旧版）
            if (l.isGroup) {
                fields.push_back("      \"isGroup\": true");
            }
            if (l.parentId != 0) {
                fields.push_back("      \"parentId\": " + std::to_string(l.parentId));
            }
            if (l.collapsed) {
                fields.push_back("      \"collapsed\": true");
            }
        }
        if (l.style != nullptr) {
            // v8 图层样式（M14c）：效果参数全量写出（色 = 0..1 三元组）
            const LayerStyle& st = *l.style;
            char num[32];
            std::string sf;
            sf += "      \"enabled\": ";
            sf += (st.enabled ? "true" : "false");
            sf += ",\n      \"dropShadow\": ";
            sf += (st.dropShadow ? "true" : "false");
            if (st.dropShadow) {
                std::snprintf(num, sizeof(num), "%.6g", st.shadowAngleDeg);
                sf += std::string(",\n      \"shAngle\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.shadowDistance);
                sf += std::string(",\n      \"shDist\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.shadowSize);
                sf += std::string(",\n      \"shSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.shadowOpacity);
                sf += std::string(",\n      \"shOpacity\": ") + num;
                sf += ",\n      \"shColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.shadowColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (st.shadowSpread != 0.0) {
                    std::snprintf(num, sizeof(num), "%.6g", st.shadowSpread);
                    sf += std::string(",\n      \"shSpread\": ") + num;
                }
                if (!st.shadowUseGL) {
                    sf += ",\n      \"shUseGL\": false";
                }
            }
            sf += ",\n      \"stroke\": ";
            sf += (st.stroke ? "true" : "false");
            if (st.stroke) {
                std::snprintf(num, sizeof(num), "%.6g", st.strokeWidth);
                sf += std::string(",\n      \"stWidth\": ") + num;
                sf += ",\n      \"stColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.strokeColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
            }
            sf += ",\n      \"colorOverlay\": ";
            sf += (st.colorOverlay ? "true" : "false");
            if (st.colorOverlay) {
                sf += ",\n      \"ovColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.overlayColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                std::snprintf(num, sizeof(num), "%.6g", st.overlayOpacity);
                sf += std::string(",\n      \"ovOpacity\": ") + num;
            }
            // v10 效果扩展（M17a）：非默认才写（读兼容 v8/v9）
            if (st.innerShadow) {
                std::snprintf(num, sizeof(num), "%.6g", st.isAngleDeg);
                sf += std::string(",\n      \"isAngle\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.isDistance);
                sf += std::string(",\n      \"isDist\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.isSize);
                sf += std::string(",\n      \"isSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.isOpacity);
                sf += std::string(",\n      \"isOpacity\": ") + num;
                sf += ",\n      \"isColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.isColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (st.isSpread != 0.0) {
                    std::snprintf(num, sizeof(num), "%.6g", st.isSpread);
                    sf += std::string(",\n      \"isSpread\": ") + num;
                }
                if (!st.isUseGL) {
                    sf += ",\n      \"isUseGL\": false";
                }
            }
            if (st.outerGlow) {
                std::snprintf(num, sizeof(num), "%.6g", st.ogSize);
                sf += std::string(",\n      \"ogSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.ogOpacity);
                sf += std::string(",\n      \"ogOpacity\": ") + num;
                sf += ",\n      \"ogColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.ogColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (st.ogSpread != 0.0) {
                    std::snprintf(num, sizeof(num), "%.6g", st.ogSpread);
                    sf += std::string(",\n      \"ogSpread\": ") + num;
                }
            }
            if (st.innerGlow) {
                std::snprintf(num, sizeof(num), "%.6g", st.igSize);
                sf += std::string(",\n      \"igSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.igOpacity);
                sf += std::string(",\n      \"igOpacity\": ") + num;
                sf += ",\n      \"igColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.igColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (st.igSpread != 0.0) {
                    std::snprintf(num, sizeof(num), "%.6g", st.igSpread);
                    sf += std::string(",\n      \"igSpread\": ") + num;
                }
            }
            if (st.gradientOverlay) {
                std::snprintf(num, sizeof(num), "%.6g", st.gdAngleDeg);
                sf += std::string(",\n      \"gdAngle\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.gdOpacity);
                sf += std::string(",\n      \"gdOpacity\": ") + num;
                sf += ",\n      \"gdFrom\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.gdFrom[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "],\n      \"gdTo\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.gdTo[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
            }
            // M17b 光泽/斜面（非默认才写）
            if (st.satin) {
                std::snprintf(num, sizeof(num), "%.6g", st.satinAngleDeg);
                sf += std::string(",\n      \"saAngle\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.satinDistance);
                sf += std::string(",\n      \"saDist\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.satinSize);
                sf += std::string(",\n      \"saSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.satinOpacity);
                sf += std::string(",\n      \"saOpacity\": ") + num;
                sf += ",\n      \"saColor\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.satinColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (st.satinInvert) {
                    sf += ",\n      \"saInvert\": true";
                }
            }
            if (st.bevel) {
                sf += std::string(",\n      \"bvStyle\": ") + std::to_string(st.bevelStyle);
                std::snprintf(num, sizeof(num), "%.6g", st.bevelDepth);
                sf += std::string(",\n      \"bvDepth\": ") + num;
                if (!st.bevelUp) {
                    sf += ",\n      \"bvUp\": false";
                }
                std::snprintf(num, sizeof(num), "%.6g", st.bevelSize);
                sf += std::string(",\n      \"bvSize\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.bevelAngleDeg);
                sf += std::string(",\n      \"bvAngle\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.bevelAltitudeDeg);
                sf += std::string(",\n      \"bvAlt\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.bevelHiOpacity);
                sf += std::string(",\n      \"bvHiOp\": ") + num;
                sf += ",\n      \"bvHi\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.bevelHiColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                std::snprintf(num, sizeof(num), "%.6g", st.bevelShOpacity);
                sf += std::string(",\n      \"bvShOp\": ") + num;
                sf += ",\n      \"bvSh\": [";
                for (int ci = 0; ci < 3; ++ci) {
                    std::snprintf(num, sizeof(num), "%.6g", st.bevelShColor[ci]);
                    sf += num;
                    if (ci < 2) sf += ", ";
                }
                sf += "]";
                if (!st.bevelUseGL) {
                    sf += ",\n      \"bvUseGL\": false";
                }
            }
            // v11 等高线/Range/图案叠加（M17d）：非默认才写（读兼容 v1–v10）
            const auto pushContour = [&](const char* key, const StyleContour& c) {
                const std::string cj = contourJson(c);
                if (!cj.empty()) {
                    sf += std::string(",\n      \"") + key + "\": " + cj;
                }
            };
            pushContour("shContour", st.shadowContour);
            pushContour("isContour", st.isContour);
            pushContour("ogContour", st.ogContour);
            pushContour("igContour", st.igContour);
            pushContour("saContour", st.satinContour);
            pushContour("bvContour", st.bevelContour);
            pushContour("bvGlossContour", st.bevelGlossContour);
            if (st.ogRange != 0.5) {
                char num[32];
                std::snprintf(num, sizeof(num), "%.6g", st.ogRange);
                sf += std::string(",\n      \"ogRange\": ") + num;
            }
            if (st.igRange != 0.5) {
                char num[32];
                std::snprintf(num, sizeof(num), "%.6g", st.igRange);
                sf += std::string(",\n      \"igRange\": ") + num;
            }
            if (st.patternOverlay) {
                sf += ",\n      \"poOn\": true";
                sf += ",\n      \"poId\": " + jsonEscape(st.patternId);
                char num[32];
                std::snprintf(num, sizeof(num), "%.6g", st.poScale);
                sf += std::string(",\n      \"poScale\": ") + num;
                std::snprintf(num, sizeof(num), "%.6g", st.poOpacity);
                sf += std::string(",\n      \"poOpacity\": ") + num;
            }
            fields.push_back(std::string("      \"style\": {\n") + sf + "\n      }");
        }
        if (l.fill != nullptr) {
            // v8 填充图层（M14c）：kind/颜色/角度
            const FillLayerState& fl = *l.fill;
            char num[32];
            std::string ff;
            ff += "      \"kind\": " + std::to_string(fl.kind);
            ff += ",\n      \"color\": [";
            for (int ci = 0; ci < 3; ++ci) {
                std::snprintf(num, sizeof(num), "%.6g", fl.color[ci]);
                ff += num;
                if (ci < 2) ff += ", ";
            }
            ff += "]";
            std::snprintf(num, sizeof(num), "%.6g", fl.angleDeg);
            ff += std::string(",\n      \"angle\": ") + num;
            fields.push_back(std::string("      \"fill\": {\n") + ff + "\n      }");
        }
        if (l.text != nullptr) {
            // v6 文本层：内容/样式持久化；像素仍存 PNG（缺字体回退时可显示旧栅格）
            const TextLayerState& t = *l.text;
            char num[32];
            std::string tf;
            tf += "      \"content\": ";
            tf += jsonEscape(t.content);
            tf += ",\n";
            tf += "      \"fontFamily\": ";
            tf += jsonEscape(t.fontFamily);
            tf += ",\n";
            std::snprintf(num, sizeof(num), "%.6g", t.fontSize);
            tf += "      \"fontSize\": ";
            tf += num;
            tf += ",\n";
            std::snprintf(num, sizeof(num), "%.6g", t.lineHeight);
            tf += "      \"lineHeight\": ";
            tf += num;
            tf += ",\n";
            std::snprintf(num, sizeof(num), "%.6g", t.tracking);
            tf += "      \"tracking\": ";
            tf += num;
            tf += ",\n";
            tf += "      \"bold\": ";
            tf += (t.bold ? "true" : "false");
            tf += ",\n";
            tf += "      \"italic\": ";
            tf += (t.italic ? "true" : "false");
            tf += ",\n";
            tf += "      \"color\": [";
            std::snprintf(num, sizeof(num), "%.6g", t.color[0]);
            tf += num;
            tf += ", ";
            std::snprintf(num, sizeof(num), "%.6g", t.color[1]);
            tf += num;
            tf += ", ";
            std::snprintf(num, sizeof(num), "%.6g", t.color[2]);
            tf += num;
            tf += "],\n";
            tf += "      \"align\": ";
            tf += std::to_string(t.align);
            tf += ",\n";
            std::snprintf(num, sizeof(num), "%.6g", t.wrapWidth);
            tf += "      \"wrapWidth\": ";
            tf += num;
            // P2 文字项（非默认才写——旧包字节不变）：缩进 + 基线锚缓存
            if (t.indentLeft != 0.0) {
                tf += ",\n      \"indentLeft\": ";
                tf += jsonNumber(t.indentLeft);
            }
            if (t.indentFirst != 0.0) {
                tf += ",\n      \"indentFirst\": ";
                tf += jsonNumber(t.indentFirst);
            }
            if (t.baselineOff > 0.0) {
                tf += ",\n      \"baselineOff\": ";
                tf += jsonNumber(t.baselineOff);
            }
            fields.push_back("      \"text\": {\n" + tf + "\n      }");
        }
        if (l.shape != nullptr) {
            // v12 形状层（M18）：path 扁平数组 + 填充 + 描边 + live（坐标 %.17g 保往返）
            const ShapeLayerState& sh = *l.shape;
            std::string sf;
            sf += "      \"path\": " + pathToJson(sh.path, "        ");
            sf += std::string(",\n      \"fillKind\": ") + std::to_string(sh.fillKind);
            sf += ",\n      \"fillColor\": [";
            for (int ci = 0; ci < 4; ++ci) {
                sf += jsonNumber(sh.fillColor[ci]);
                if (ci < 3) sf += ", ";
            }
            sf += "]";
            // v15（P1 形状填充扩面；非默认才写——v14 包字节不变）
            if (sh.fillKind == 2 && sh.fillStops.size() >= 10) {
                sf += ",\n      \"fillStops\": [";
                for (size_t i = 0; i < sh.fillStops.size(); ++i) {
                    sf += jsonNumber(sh.fillStops[i]);
                    if (i + 1 < sh.fillStops.size()) sf += ", ";
                }
                sf += "]";
                sf += std::string(",\n      \"fillAngleDeg\": ") + jsonNumber(sh.fillAngleDeg);
            }
            if (sh.fillKind == 3 && !sh.fillPatternId.empty()) {
                sf += ",\n      \"fillPatternId\": " + jsonEscape(sh.fillPatternId);
                sf += std::string(",\n      \"fillPatScale\": ") + jsonNumber(sh.fillPatScale);
            }
            sf += std::string(",\n      \"hasStroke\": ") + (sh.hasStroke ? "true" : "false");
            if (sh.hasStroke) {
                const ShapeStroke& st = sh.stroke;
                sf += ",\n      \"stroke\": {";
                sf += "\"width\": " + jsonNumber(st.width);
                sf += ", \"color\": [";
                for (int ci = 0; ci < 4; ++ci) {
                    sf += jsonNumber(st.color[ci]);
                    if (ci < 3) sf += ", ";
                }
                sf += "]";
                sf += std::string(", \"opacity\": ") + jsonNumber(st.opacity);
                sf += std::string(", \"align\": ") + std::to_string(static_cast<int32_t>(st.align));
                sf += std::string(", \"cap\": ") + std::to_string(static_cast<int32_t>(st.cap));
                sf += std::string(", \"join\": ") + std::to_string(static_cast<int32_t>(st.join));
                sf += ", \"miter\": " + jsonNumber(st.miterLimit);
                sf += ", \"dashes\": [";
                for (size_t di = 0; di < st.dashes.size(); ++di) {
                    sf += jsonNumber(st.dashes[di]);
                    if (di + 1 < st.dashes.size()) sf += ", ";
                }
                sf += "]";
                sf += ", \"dashOffset\": " + jsonNumber(st.dashOffset);
                sf += "}";
            }
            if (sh.live.kind != ShapeLiveKind::None) {
                const LiveShape& lv = sh.live;
                sf += std::string(",\n      \"live\": {\"kind\": ") +
                      std::to_string(static_cast<int32_t>(lv.kind));
                sf += ", \"rect\": [";
                for (int ci = 0; ci < 4; ++ci) {
                    sf += jsonNumber(lv.rect[ci]);
                    if (ci < 3) sf += ", ";
                }
                sf += "]";
                if (lv.kind == ShapeLiveKind::Rect) {
                    sf += ", \"radii\": [";
                    for (int ci = 0; ci < 4; ++ci) {
                        sf += jsonNumber(lv.radii[ci]);
                        if (ci < 3) sf += ", ";
                    }
                    sf += "]";
                }
                if (lv.kind == ShapeLiveKind::Polygon) {
                    sf += std::string(", \"sides\": ") + std::to_string(lv.sides);
                    sf += ", \"star\": " + jsonNumber(lv.starRatio);
                }
                if (lv.kind == ShapeLiveKind::Line) {
                    sf += ", \"from\": [" + jsonNumber(lv.from[0]) + ", " + jsonNumber(lv.from[1]) + "]";
                    sf += ", \"to\": [" + jsonNumber(lv.to[0]) + ", " + jsonNumber(lv.to[1]) + "]";
                    sf += ", \"weight\": " + jsonNumber(lv.weight);
                }
                sf += "}";
            }
            fields.push_back("      \"shape\": {\n" + sf + "\n      }");
        }
        if (l.vectorMask != nullptr) {
            // v12 矢量蒙版（M18d）：路径 + enabled
            fields.push_back("      \"vectorMask\": {\"enabled\": " +
                             std::string(l.vectorMaskEnabled ? "true" : "false") +
                             ", \"path\": " + pathToJson(l.vectorMask->path, "           ") + "}");
        }
        if (l.smart != nullptr) {
            // v13 智能对象（M21）：源引用 + 放置仿射 + 智能滤镜栈 + 滤镜蒙版
            const SmartLayerState& sm = *l.smart;
            std::string so;
            so += "      \"contentsId\": " + std::to_string(sm.contentsId);
            so += std::string(",\n      \"embedded\": ") + (sm.embedded ? "true" : "false");
            so += ",\n      \"fileName\": " + jsonEscape(sm.fileName);
            if (sm.embedded) {
                std::string ext = "bin";
                const size_t dot = sm.fileName.rfind('.');
                if (dot != std::string::npos && dot + 1 < sm.fileName.size()) {
                    ext = sm.fileName.substr(dot + 1);
                }
                so += ",\n      \"sourceFile\": \"images/smart_" + std::to_string(sm.contentsId) +
                      "." + ext + "\"";
            } else {
                so += ",\n      \"linkedPath\": " + jsonEscape(sm.linkedPath);
            }
            so += ",\n      \"m\": [";
            for (int ci = 0; ci < 6; ++ci) {
                so += jsonNumber(sm.m[ci]);
                if (ci < 5) so += ", ";
            }
            so += "]";
            if (!sm.filtersEnabled) {
                so += ",\n      \"filtersEnabled\": false";
            }
            if (!sm.filters.empty()) {
                so += ",\n      \"filters\": [\n";
                for (size_t fi = 0; fi < sm.filters.size(); ++fi) {
                    const SmartFilter& f = sm.filters[fi];
                    std::string fs = "        {\"kind\": " + std::to_string(f.kind);
                    if (!f.psdClass.empty()) {
                        fs += ", \"psdClass\": " + jsonEscape(f.psdClass);
                    }
                    if (!f.name.empty()) {
                        fs += ", \"name\": " + jsonEscape(f.name);
                    }
                    if (f.kind >= 0) {
                        const float pv[6] = {f.params.p0, f.params.p1, f.params.p2,
                                             f.params.p3, f.params.p4, f.params.p5};
                        fs += ", \"p\": [";
                        for (int ci = 0; ci < 6; ++ci) {
                            fs += jsonNumber(pv[ci]);
                            if (ci < 5) fs += ", ";
                        }
                        fs += "]";
                        fs += ", \"seed\": " + std::to_string(f.params.seed);
                        if (f.kind == filters::FCustom) {
                            fs += ", \"scale\": " + jsonNumber(f.params.scale);
                            fs += ", \"offset\": " + jsonNumber(f.params.offset);
                            fs += ", \"kernel\": [";
                            for (int ki = 0; ki < 25; ++ki) {
                                fs += jsonNumber(f.params.kernel[ki]);
                                if (ki < 24) fs += ", ";
                            }
                            fs += "]";
                        }
                        if (f.kind == filters::FClouds || f.kind == filters::FDifferenceClouds) {
                            fs += ", \"colorFg\": [";
                            for (int ci = 0; ci < 3; ++ci) {
                                fs += jsonNumber(f.params.colorFg[ci]);
                                if (ci < 2) fs += ", ";
                            }
                            fs += "], \"colorBg\": [";
                            for (int ci = 0; ci < 3; ++ci) {
                                fs += jsonNumber(f.params.colorBg[ci]);
                                if (ci < 2) fs += ", ";
                            }
                            fs += "]";
                        }
                    }
                    if (f.blend != BlendMode::Normal) {
                        fs += ", \"blend\": " + std::to_string(static_cast<int32_t>(f.blend));
                    }
                    if (f.opacity != 1.0) {
                        fs += ", \"opacity\": " + jsonNumber(f.opacity);
                    }
                    if (!f.visible) {
                        fs += ", \"visible\": false";
                    }
                    if (!f.psdHex.empty()) {
                        fs += ", \"psd\": " + jsonEscape(f.psdHex);
                    }
                    fs += "}";
                    so += fs;
                    so += (fi + 1 < sm.filters.size()) ? ",\n" : "\n";
                }
                so += "      ]";
            }
            if (sm.filterMask != nullptr) {
                so += ",\n      \"filterMask\": \"images/smart_" + std::to_string(sm.contentsId) +
                      ".fmask.png\"";
                so += ",\n      \"filterMaskW\": " + std::to_string(sm.filterMask->width);
                so += ",\n      \"filterMaskH\": " + std::to_string(sm.filterMask->height);
                so += ",\n      \"filterMaskOffsetX\": " + std::to_string(sm.filterMask->offsetX);
                so += ",\n      \"filterMaskOffsetY\": " + std::to_string(sm.filterMask->offsetY);
                so += ",\n      \"filterMaskOutside\": " + std::to_string(sm.filterMask->outside);
                so += std::string(",\n      \"filterMaskEnabled\": ") +
                      (sm.filterMask->enabled ? "true" : "false");
            }
            if (!sm.placedId.empty()) {
                so += ",\n      \"placedId\": " + jsonEscape(sm.placedId);
            }
            if (!sm.uuid.empty()) {
                so += ",\n      \"uuid\": " + jsonEscape(sm.uuid);
            }
            fields.push_back("      \"smart\": {\n" + so + "\n      }");
        }
        if (l.psdSrcBlob != nullptr) {
            // M22 v14：深色 verbatim blob（自包含：深度/模式/通道表/源矩形/指纹/段字节）
            fields.push_back("      \"psdSrc\": \"images/psdsrc_" + std::to_string(l.id) +
                             ".bin\"");
        }
        if (l.pixels != nullptr) {
            // 图像以内容包围盒存出；originX/Y = 包围盒左上角在文档坐标系的落点（load 据此重建 Transform）
            fields.push_back("      \"originX\": " + std::to_string(static_cast<int64_t>(l.originX)));
            fields.push_back("      \"originY\": " + std::to_string(static_cast<int64_t>(l.originY)));
            fields.push_back("      \"width\": " + std::to_string(l.pw));
            fields.push_back("      \"height\": " + std::to_string(l.ph));
            fields.push_back("      \"imageFile\": \"images/" + std::to_string(l.id) + ".png\"");
        } else {
            fields.push_back("      \"imageFile\": null");
        }
        if (l.maskGrid != nullptr) {
            // v2 蒙版字段（v1 读取方按未知字段忽略）
            fields.push_back("      \"maskFile\": \"images/" + std::to_string(l.id) + ".mask.png\"");
            fields.push_back("      \"maskWidth\": " + std::to_string(l.maskW));
            fields.push_back("      \"maskHeight\": " + std::to_string(l.maskH));
            fields.push_back(std::string("      \"maskEnabled\": ") + (l.maskEnabled ? "true" : "false"));
            fields.push_back("      \"maskOffsetX\": " + std::to_string(l.maskOffsetX));
            fields.push_back("      \"maskOffsetY\": " + std::to_string(l.maskOffsetY));
            fields.push_back("      \"maskOutside\": " + std::to_string(l.maskOutside));
        }
        j += "    {\n";
        for (size_t f = 0; f < fields.size(); ++f) {
            j += fields[f];
            j += (f + 1 < fields.size()) ? ",\n" : "\n";
        }
        j += "    }";
        j += (i + 1 < doc.layers.size()) ? ",\n" : "\n";
    }
    j += "  ]";
    // v5：Alpha 通道（存储选区；文档域灰度 PNG）
    if (!doc.channels.empty()) {
        j += ",\n  \"channels\": [\n";
        for (size_t i = 0; i < doc.channels.size(); ++i) {
            j += "    {\n";
            j += "      \"name\": " + jsonEscape(doc.channels[i].name) + ",\n";
            j += "      \"file\": \"channels/" + std::to_string(i) + ".png\"\n";
            j += "    }";
            j += (i + 1 < doc.channels.size()) ? ",\n" : "\n";
        }
        j += "  ]\n";
    } else {
        j += "\n";
    }
    // v9：参考线（M15a 会话态；空且全默认不写 = v8 及以下包字节不变）
    const bool hasGuides = !doc.guidesH.empty() || !doc.guidesV.empty() ||
                           doc.guidesLocked || !doc.guidesVisible;
    if (hasGuides) {
        j += ",\n  \"guides\": {\n";
        char num[32];
        auto pushAxis = [&](const char* key, const std::vector<float>& v, bool last) {
            j += std::string("    \"") + key + "\": [";
            for (size_t i = 0; i < v.size(); ++i) {
                std::snprintf(num, sizeof(num), "%.6g", v[i]);
                j += num;
                if (i + 1 < v.size()) {
                    j += ", ";
                }
            }
            j += last ? "]\n" : "],\n";
        };
        pushAxis("h", doc.guidesH, false);
        pushAxis("v", doc.guidesV, false);
        j += std::string("    \"locked\": ") + (doc.guidesLocked ? "true" : "false") + ",\n";
        j += std::string("    \"visible\": ") + (doc.guidesVisible ? "true" : "false") + "\n";
        j += "  }\n";
    }
    // v10：全局光角度（M17b 文档级；非默认 120 不写 = 旧包字节不变）
    if (doc.globalLightAngle != 120.0) {
        char num[32];
        std::snprintf(num, sizeof(num), "%.6g", doc.globalLightAngle);
        j += std::string(",\n  \"globalLight\": ") + num + "\n";
    }
    // v11：文档图案（M17d；像素走 images/pattern_<id>.png，空表不写 = v10 包字节不变）
    if (!doc.patterns.empty()) {
        j += ",\n  \"patterns\": [\n";
        for (size_t i = 0; i < doc.patterns.size(); ++i) {
            const PatternSnapshot& p = doc.patterns[i];
            char num[32];
            j += "    {\"id\": " + jsonEscape(p.id) + ", \"name\": " + jsonEscape(p.name);
            std::snprintf(num, sizeof(num), "%u", p.w);
            j += std::string(", \"w\": ") + num;
            std::snprintf(num, sizeof(num), "%u", p.h);
            j += std::string(", \"h\": ") + num + "}";
            if (i + 1 < doc.patterns.size()) {
                j += ",";
            }
            j += "\n";
        }
        j += "  ]\n";
    }
    // v12：矢量文档级（M18；全空不写 = v11 包字节不变）
    const bool hasVector = !doc.workPath.subpaths.empty() || !doc.paths.empty() ||
                           !doc.customShapes.empty();
    if (hasVector) {
        j += ",\n  \"vector\": {\n";
        bool first = true;
        if (!doc.workPath.subpaths.empty()) {
            j += "    \"workPath\": " + pathToJson(doc.workPath, "      ");
            first = false;
        }
        if (!doc.paths.empty()) {
            if (!first) {
                j += ",\n";
            }
            j += "    \"paths\": [\n";
            for (size_t i = 0; i < doc.paths.size(); ++i) {
                j += "      {\"name\": " + jsonEscape(doc.paths[i].name) + ", \"path\": " +
                     pathToJson(doc.paths[i].path, "        ") + "}";
                if (i + 1 < doc.paths.size()) {
                    j += ",";
                }
                j += "\n";
            }
            j += "    ]";
            first = false;
        }
        if (!doc.customShapes.empty()) {
            if (!first) {
                j += ",\n";
            }
            j += "    \"customShapes\": [\n";
            for (size_t i = 0; i < doc.customShapes.size(); ++i) {
                const CustomShapeEntry& c = doc.customShapes[i];
                j += "      {\"id\": " + jsonEscape(c.id) + ", \"name\": " + jsonEscape(c.name) +
                     ", \"path\": " + pathToJson(c.path, "        ") + "}";
                if (i + 1 < doc.customShapes.size()) {
                    j += ",";
                }
                j += "\n";
            }
            j += "    ],\n";
            j += "    \"customShapeSeq\": " + std::to_string(doc.customShapeSeq) + "\n";
        }
        j += "  }\n";
    }
    // v14：深色 PSD 元数据（M22；全空不写 = v13 包字节不变）
    if (doc.psdIcc != nullptr || doc.psdColorMode != nullptr || !doc.psdGlobalBlocks.empty() ||
        doc.psdSrcDepth != 8 || doc.psdSrcMode != 3) {
        j += ",\n  \"psd\": {\n";
        j += "    \"depth\": " + std::to_string(doc.psdSrcDepth) + ",\n";
        j += "    \"mode\": " + std::to_string(doc.psdSrcMode) + ",\n";
        j += "    \"channels\": " + std::to_string(doc.psdSrcChannels);
        if (doc.psdIcc != nullptr) {
            j += ",\n    \"icc\": \"images/psdicc.bin\"";
        }
        if (doc.psdColorMode != nullptr) {
            j += ",\n    \"colorMode\": \"images/psdcmode.bin\"";
        }
        if (!doc.psdGlobalBlocks.empty()) {
            j += ",\n    \"blocks\": [\n";
            for (size_t i = 0; i < doc.psdGlobalBlocks.size(); ++i) {
                j += "      {\"key\": " + jsonEscape(doc.psdGlobalBlocks[i].first) +
                     ", \"file\": \"images/psdglb_" + std::to_string(i) + ".bin\"}";
                j += (i + 1 < doc.psdGlobalBlocks.size()) ? ",\n" : "\n";
            }
            j += "    ]\n";
        }
        j += "  }\n";
    }
    j += "}\n";
    return j;
}

// 快照构建（M21 抽出：saveProject 传引擎文档；saveProjectBytes 传任意文档——智能对象
// 嵌套 .montage 源编码共用同一条序列化路径）。只拷贝元数据与不可变网格引用。
void buildDocSnapshot(const Document& doc, DocSnapshot& snap) {
    snap.width = doc.width;
    snap.height = doc.height;
    snap.name = doc.name;
    snap.activeId = doc.activeId;
    snap.globalLightAngle = doc.globalLightAngle;
    for (const DocumentPattern& p : doc.patterns) {
        if (p.pixels != nullptr && p.w > 0 && p.h > 0) {
            PatternSnapshot ps;
            ps.id = p.id;
            ps.name = p.name;
            ps.w = p.w;
            ps.h = p.h;
            ps.pixels = p.pixels;
            snap.patterns.push_back(std::move(ps));
        }
    }
    for (const AlphaChannel& c : doc.channels) {
        if (c.mask != nullptr) {
            ChannelSnapshot cs;
            cs.name = c.name;
            cs.mask = c.mask;
            snap.channels.push_back(std::move(cs));
        }
    }
    snap.guidesH = doc.guidesH;
    snap.guidesV = doc.guidesV;
    snap.guidesLocked = doc.guidesLocked;
    snap.guidesVisible = doc.guidesVisible;
    snap.workPath = doc.workPath;
    snap.paths = doc.paths;
    snap.customShapes = doc.customShapes;
    snap.customShapeSeq = doc.customShapeSeq;
    snap.layers.reserve(doc.layers.size());
    for (const Layer& l : doc.layers) {
        LayerSnapshot s;
        s.id = l.id;
        s.name = l.name;
        s.visible = l.visible;
        s.opacity = l.opacity;
        s.blendMode = l.blendMode;
        s.pixels = l.pixels;
        s.originX = l.transform.originX;
        s.originY = l.transform.originY;
        if (l.mask != nullptr) {
            s.maskGrid = l.mask->pixels;
            s.maskW = l.mask->width;
            s.maskH = l.mask->height;
            s.maskOffsetX = l.mask->offsetX;
            s.maskOffsetY = l.mask->offsetY;
            s.maskOutside = l.mask->outside;
            s.maskEnabled = l.mask->enabled;
        }
        s.clipping = l.clipping;
        s.adjustment = l.adjustment;
        s.text = l.text;
        s.isGroup = l.isGroup;
        s.parentId = l.parentId;
        s.collapsed = l.collapsed;
        s.style = l.style;
        s.fill = l.fill;
        s.shape = l.shape;
        if (l.shape != nullptr && s.pixels == nullptr) {
            // M18：形状层像素 = shapeCache 派生结果、origin = shapeOrigin（PNG 编码复用）
            s.pixels = l.shapeCache;
            s.originX = l.shapeOriginX;
            s.originY = l.shapeOriginY;
        }
        if (l.smart != nullptr) {
            // M21：智能对象像素 = smartCache 派生结果、origin = transform.origin（渲染回填）
            s.smart = l.smart;
            if (l.smartCache != nullptr) {
                s.pixels = l.smartCache;
                s.originX = l.transform.originX;
                s.originY = l.transform.originY;
            }
        }
        if (l.vectorMask != nullptr) {
            s.vectorMask = l.vectorMask;
            s.vectorMaskEnabled = l.vectorMask->enabled;
        }
        if (!l.psdSrcSection.empty()) {
            // M22 v14：深色 verbatim 元数据 → 自包含 blob
            io::PsdSrcMeta m;
            m.depth = l.psdSrcDepth;
            m.mode = l.psdSrcMode;
            m.channels = l.psdSrcChannels;
            m.cropped = l.psdSrcCropped;
            m.importHash = l.psdImportHash;
            m.srcLeft = l.psdSrcLeft;
            m.srcTop = l.psdSrcTop;
            m.srcRight = l.psdSrcRight;
            m.srcBottom = l.psdSrcBottom;
            m.srcMaskLeft = l.psdSrcMaskLeft;
            m.srcMaskTop = l.psdSrcMaskTop;
            m.srcMaskRight = l.psdSrcMaskRight;
            m.srcMaskBottom = l.psdSrcMaskBottom;
            m.channelTable.reserve(l.psdChannelIds.size());
            for (size_t ci = 0; ci < l.psdChannelIds.size(); ++ci) {
                m.channelTable.emplace_back(l.psdChannelIds[ci], l.psdChannelLens[ci]);
            }
            m.section = l.psdSrcSection;
            s.psdSrcBlob = std::make_shared<std::vector<uint8_t>>(encodePsdSrcBlob(m));
        }
        snap.layers.push_back(std::move(s));
    }
    // v14：文档级深色元数据
    snap.psdSrcDepth = doc.psdSrcDepth;
    snap.psdSrcMode = doc.psdSrcMode;
    snap.psdSrcChannels = doc.psdSrcChannels;
    if (!doc.psdIccProfile.empty()) {
        snap.psdIcc = std::make_shared<std::vector<uint8_t>>(doc.psdIccProfile);
    }
    if (!doc.psdColorModeData.empty()) {
        snap.psdColorMode = std::make_shared<std::vector<uint8_t>>(doc.psdColorModeData);
    }
    for (const auto& [key, payload] : doc.psdGlobalExtra) {
        if (!payload.empty()) {
            snap.psdGlobalBlocks.emplace_back(key,
                                              std::make_shared<std::vector<uint8_t>>(payload));
        }
    }
}

// 格式校验（04 §1.3：超限给明确错误；bbox/像素预算统计在校验中一并完成）
bool validateSnapshot(DocSnapshot& snap, std::string& err) {
    if (snap.width > kMaxSide || snap.height > kMaxSide) {
        err = "document size exceeds format limit";
        return false;
    }
    if (snap.layers.size() > kMaxLayers) {
        err = "layer count exceeds format limit";
        return false;
    }
    uint64_t totalPixels = 0;
    for (LayerSnapshot& l : snap.layers) {
        if (l.pixels != nullptr && computeBbox(*l.pixels, l.px, l.py, l.pw, l.ph)) {
            totalPixels += static_cast<uint64_t>(l.pw) * l.ph;
        } else {
            l.pixels = nullptr;
        }
        if (l.maskGrid != nullptr) {
            totalPixels += static_cast<uint64_t>(l.maskW) * l.maskH;
        }
    }
    if (totalPixels > kMaxTotalPixels) {
        err = "total layer pixels exceed format budget";
        return false;
    }
    return true;
}

// 包体编码（M21 抽出：逐层/蒙版/通道/图案 PNG + v13 智能对象源/滤镜蒙版 + manifest →
// central directory；ZipSink 抽象 = fd 流式与内存包同路径）。失败返回 false + err。
bool encodeProject(const DocSnapshot& snap, ZipSink& sink, size_t* outLayers, uint64_t* outBytes,
                   std::string& err) {
    ZipWriter zip(sink);
    // 网格 → 全幅 PNG 条目（蒙版 patch 用；图层走 bbox 拼装分支）
    auto encodeGridPng = [&](const TileGrid& grid, uint32_t w, uint32_t h,
                             const std::string& name) -> bool {
        if (!zip.beginEntry(name, 0)) {
            err = "zip init failed";
            return false;
        }
        PngRowEncoder png(zip, w, h);
        if (!png.begin()) {
            err = "png init failed";
            return false;
        }
        std::vector<uint8_t> row(static_cast<size_t>(w) * 4u);
        for (uint32_t ry = 0; ry < h; ++ry) {
            const uint32_t ty = ry / kTileSize;
            const uint32_t inTileY = ry % kTileSize;
            for (uint32_t rx = 0; rx < w; rx += kTileSize) {
                const uint32_t tx = rx / kTileSize;
                const uint32_t copyW = std::min(kTileSize, w - rx);
                auto it = grid.tiles.find(ty * grid.cols + tx);
                if (it == grid.tiles.end()) {
                    std::memset(row.data() + static_cast<size_t>(rx) * 4u, 0,
                                static_cast<size_t>(copyW) * 4u);
                    continue;
                }
                const uint8_t* src = it->second->pixels->mapCpu();
                const uint32_t stride = it->second->pixels->rowBytes();
                std::memcpy(row.data() + static_cast<size_t>(rx) * 4u,
                            src + static_cast<size_t>(inTileY) * stride,
                            static_cast<size_t>(copyW) * 4u);
            }
            if (!png.addRow(row.data())) {
                err = "png encode failed";
                return false;
            }
        }
        if (!png.finish() || !zip.endEntry()) {
            err = "png finalize failed";
            return false;
        }
        return true;
    };

    for (const LayerSnapshot& l : snap.layers) {
        // 蒙版编码独立于像素（审查修复）：空图层+蒙版（M10c 合法）manifest 已写 maskFile，
        // 蒙版 PNG 若挂在 pixels 非空分支漏编码，open 侧 layer mask missing 整档打不开
        if (l.maskGrid != nullptr &&
            !encodeGridPng(*l.maskGrid, l.maskW, l.maskH,
                           "images/" + std::to_string(l.id) + ".mask.png")) {
            return false;
        }
        if (l.pixels == nullptr) {
            continue;
        }
        const TileGrid& grid = *l.pixels;
        if (!zip.beginEntry("images/" + std::to_string(l.id) + ".png", 0)) {
            err = "zip init failed";
            return false;
        }
        PngRowEncoder png(zip, l.pw, l.ph);
        if (!png.begin()) {
            err = "png init failed";
            return false;
        }
        std::vector<uint8_t> row(static_cast<size_t>(l.pw) * 4u);
        const uint32_t tx0 = l.px / kTileSize;
        const uint32_t ty0 = l.py / kTileSize;
        for (uint32_t ry = 0; ry < l.ph; ++ry) {
            const uint32_t ty = ty0 + ry / kTileSize;
            const uint32_t inTileY = ry % kTileSize;
            for (uint32_t rx = 0; rx < l.pw; rx += kTileSize) {
                const uint32_t tx = tx0 + rx / kTileSize;
                const uint32_t copyW = std::min(kTileSize, l.pw - rx);
                auto it = grid.tiles.find(ty * grid.cols + tx);
                if (it == grid.tiles.end()) {
                    std::memset(row.data() + static_cast<size_t>(rx) * 4u, 0,
                                static_cast<size_t>(copyW) * 4u);
                    continue;
                }
                const uint8_t* src = it->second->pixels->mapCpu();
                const uint32_t stride = it->second->pixels->rowBytes();
                std::memcpy(row.data() + static_cast<size_t>(rx) * 4u,
                            src + static_cast<size_t>(inTileY) * stride,
                            static_cast<size_t>(copyW) * 4u);
            }
            if (!png.addRow(row.data())) {
                err = "png encode failed";
                return false;
            }
        }
        if (!png.finish() || !zip.endEntry()) {
            err = "png finalize failed";
            return false;
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "layer %{public}lu saved %{public}ux%{public}u @(%{public}ld,%{public}ld)",
                     static_cast<unsigned long>(l.id), l.pw, l.ph,
                     static_cast<long>(l.originX), static_cast<long>(l.originY));
    }

    // v5：Alpha 通道掩码（文档域全幅灰度 PNG）
    for (size_t i = 0; i < snap.channels.size(); ++i) {
        const ChannelSnapshot& c = snap.channels[i];
        if (!encodeGridPng(*c.mask, snap.width, snap.height,
                           "channels/" + std::to_string(i) + ".png")) {
            return false;
        }
    }

    // v11：文档图案 PNG（M17d，整幅 images/pattern_<id>.png）
    for (const PatternSnapshot& p : snap.patterns) {
        if (!encodeGridPng(*p.pixels, p.w, p.h, "images/pattern_" + p.id + ".png")) {
            return false;
        }
    }

    // v14：深色 PSD blob/ICC/色板/全局块（原样字节 bin）
    auto writeBin = [&](const std::string& name, const std::vector<uint8_t>& bytes) -> bool {
        if (!zip.beginEntry(name, 0) ||
            !zip.writeEntryBytes(bytes.data(), bytes.size()) || !zip.endEntry()) {
            err = "psd bin write failed";
            return false;
        }
        return true;
    };
    for (const LayerSnapshot& l : snap.layers) {
        if (l.psdSrcBlob != nullptr &&
            !writeBin("images/psdsrc_" + std::to_string(l.id) + ".bin", *l.psdSrcBlob)) {
            return false;
        }
    }
    if (snap.psdIcc != nullptr && !writeBin("images/psdicc.bin", *snap.psdIcc)) {
        return false;
    }
    if (snap.psdColorMode != nullptr && !writeBin("images/psdcmode.bin", *snap.psdColorMode)) {
        return false;
    }
    for (size_t i = 0; i < snap.psdGlobalBlocks.size(); ++i) {
        if (!writeBin("images/psdglb_" + std::to_string(i) + ".bin", *snap.psdGlobalBlocks[i].second)) {
            return false;
        }
    }

    // v13：智能对象源文件（嵌入字节原样存储）+ 滤镜蒙版 patch PNG
    for (const LayerSnapshot& l : snap.layers) {
        if (l.smart == nullptr) {
            continue;
        }
        if (l.smart->filterMask != nullptr && l.smart->filterMask->pixels != nullptr &&
            !encodeGridPng(*l.smart->filterMask->pixels, l.smart->filterMask->width,
                           l.smart->filterMask->height,
                           "images/smart_" + std::to_string(l.smart->contentsId) + ".fmask.png")) {
            return false;
        }
        if (!l.smart->embedded || l.smart->sourceBytes.empty()) {
            continue;
        }
        std::string ext = "bin";
        const size_t dot = l.smart->fileName.rfind('.');
        if (dot != std::string::npos && dot + 1 < l.smart->fileName.size()) {
            ext = l.smart->fileName.substr(dot + 1);
        }
        if (!zip.beginEntry("images/smart_" + std::to_string(l.smart->contentsId) + "." + ext, 0) ||
            !zip.writeEntryBytes(l.smart->sourceBytes.data(), l.smart->sourceBytes.size()) ||
            !zip.endEntry()) {
            err = "smart source write failed";
            return false;
        }
    }

    const std::string manifest = buildManifest(snap);
    if (manifest.size() > kMaxManifestBytes) {
        err = "manifest too large";
        return false;
    }
    if (!zip.beginEntry("manifest.json", 0) ||
        !zip.writeEntryBytes(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) ||
        !zip.endEntry() || !zip.finish()) {
        err = "zip finalize failed";
        return false;
    }
    if (outLayers != nullptr) {
        *outLayers = snap.layers.size();
    }
    if (outBytes != nullptr) {
        *outBytes = zip.totalBytes();
    }
    return true;
}

}  // namespace

bool saveProject(int fd, size_t* outLayers, uint64_t* outBytes, std::string& err) {
    // 1) 锁内快照（元数据 + 不可变 TileGrid 引用），编解码全程锁外（02 §7 并发规则）
    DocSnapshot snap;
    {
        auto& e = Engine::get();
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc().width == 0 || e.doc().height == 0) {
            close(fd);
            err = "no document";
            return false;
        }
        buildDocSnapshot(e.doc(), snap);
    }

    // 2) 格式校验（04 §1.3：超限给明确错误）
    if (!validateSnapshot(snap, err)) {
        close(fd);
        return false;
    }

    // 3) 包体编码（fd 流式 sink；M21 起与内存编码共用 encodeProject）
    uint64_t bytes = 0;
    {
        FdZipSink sink(fd);
        if (!encodeProject(snap, sink, outLayers, &bytes, err)) {
            close(fd);
            return false;
        }
    }
    if (fsync(fd) != 0) {
        err = "fsync failed";
        close(fd);
        return false;
    }
    close(fd);
    if (outBytes != nullptr) {
        *outBytes = bytes;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "project saved: %{public}zu layers, %{public}lu bytes",
                 snap.layers.size(), static_cast<unsigned long>(bytes));
    return true;
}

// M21：任意文档 → .montage 内存包（智能对象嵌套源编码；不触碰 Engine/History）
bool saveProjectBytes(const Document& doc, std::vector<uint8_t>& out, std::string& err) {
    DocSnapshot snap;
    buildDocSnapshot(doc, snap);
    if (snap.width == 0 || snap.height == 0) {
        err = "no document";
        return false;
    }
    if (!validateSnapshot(snap, err)) {
        return false;
    }
    MemZipSink sink(out);
    return encodeProject(snap, sink, nullptr, nullptr, err);
}

}  // namespace io
}  // namespace montage
