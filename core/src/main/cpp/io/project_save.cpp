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
#include "engine/engine.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Save";
constexpr const char* kFormatId = "com.sytkiller.montage.project";
constexpr int kFormatVersion = 1;
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

// ---------- zip 写出（流式单遍；条目先写占位本地头，收尾 pwrite 回填；不做 zip64） ----------

class ZipWriter {
  public:
    explicit ZipWriter(int fd) : fd_(fd) {}

    int fd() const { return fd_; }
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
            if (!writeAll(fd_, p, n)) {
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
        if (!pwriteAllAt(fd_, patch, sizeof(patch), entryOffset_ + 14)) {
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
            if (!writeAll(fd_, ch, sizeof(ch)) ||
                !writeAll(fd_, reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size())) {
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
        if (!writeAll(fd_, eo, sizeof(eo))) {
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
        if (!writeAll(fd_, h, sizeof(h)) ||
            !writeAll(fd_, reinterpret_cast<const uint8_t*>(name.data()), name.size())) {
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
                if (!writeAll(fd_, out_.data(), produced)) {
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

    int fd_;
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
};

struct DocSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string name;
    LayerId activeId = 0;
    std::vector<LayerSnapshot> layers;
};

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
        j += "    {\n";
        j += "      \"id\": " + std::to_string(l.id) + ",\n";
        j += "      \"name\": " + jsonEscape(l.name) + ",\n";
        j += std::string("      \"visible\": ") + (l.visible ? "true" : "false") + ",\n";
        char num[32];
        std::snprintf(num, sizeof(num), "%.6g", l.opacity);
        j += "      \"opacity\": " + std::string(num) + ",\n";
        j += "      \"blendMode\": " + jsonEscape(blendModeName(l.blendMode)) + ",\n";
        if (l.pixels != nullptr) {
            // 图像以内容包围盒存出；originX/Y = 包围盒左上角在文档坐标系的落点（load 据此重建 Transform）
            j += "      \"originX\": " + std::to_string(static_cast<int64_t>(l.originX)) + ",\n";
            j += "      \"originY\": " + std::to_string(static_cast<int64_t>(l.originY)) + ",\n";
            j += "      \"width\": " + std::to_string(l.pw) + ",\n";
            j += "      \"height\": " + std::to_string(l.ph) + ",\n";
            j += "      \"imageFile\": \"images/" + std::to_string(l.id) + ".png\"\n";
        } else {
            j += "      \"imageFile\": null\n";
        }
        j += "    }";
        j += (i + 1 < doc.layers.size()) ? ",\n" : "\n";
    }
    j += "  ]\n}\n";
    return j;
}

}  // namespace

bool saveProject(int fd, size_t* outLayers, uint64_t* outBytes, std::string& err) {
    // 1) 锁内快照（元数据 + 不可变 TileGrid 引用），编解码全程锁外（02 §7 并发规则）
    DocSnapshot snap;
    {
        auto& e = Engine::get();
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            close(fd);
            err = "no document";
            return false;
        }
        snap.width = e.doc.width;
        snap.height = e.doc.height;
        snap.name = e.doc.name;
        snap.activeId = e.doc.activeId;
        snap.layers.reserve(e.doc.layers.size());
        for (const Layer& l : e.doc.layers) {
            LayerSnapshot s;
            s.id = l.id;
            s.name = l.name;
            s.visible = l.visible;
            s.opacity = l.opacity;
            s.blendMode = l.blendMode;
            s.pixels = l.pixels;
            s.originX = l.transform.originX;
            s.originY = l.transform.originY;
            snap.layers.push_back(std::move(s));
        }
    }

    // 2) 格式校验（04 §1.3：超限给明确错误）
    if (snap.width > kMaxSide || snap.height > kMaxSide) {
        close(fd);
        err = "document size exceeds format limit";
        return false;
    }
    if (snap.layers.size() > kMaxLayers) {
        close(fd);
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
    }
    if (totalPixels > kMaxTotalPixels) {
        close(fd);
        err = "total layer pixels exceed format budget";
        return false;
    }

    // 3) 包体：逐层 PNG（内容包围盒）→ manifest.json → central directory
    ZipWriter zip(fd);
    for (const LayerSnapshot& l : snap.layers) {
        if (l.pixels == nullptr) {
            continue;
        }
        const TileGrid& grid = *l.pixels;
        if (!zip.beginEntry("images/" + std::to_string(l.id) + ".png", 0)) {
            err = "zip init failed";
            close(fd);
            return false;
        }
        PngRowEncoder png(zip, l.pw, l.ph);
        if (!png.begin()) {
            err = "png init failed";
            close(fd);
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
                close(fd);
                return false;
            }
        }
        if (!png.finish() || !zip.endEntry()) {
            err = "png finalize failed";
            close(fd);
            return false;
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "layer %{public}lu saved %{public}ux%{public}u @(%{public}ld,%{public}ld)",
                     static_cast<unsigned long>(l.id), l.pw, l.ph,
                     static_cast<long>(l.originX), static_cast<long>(l.originY));
    }

    const std::string manifest = buildManifest(snap);
    if (manifest.size() > kMaxManifestBytes) {
        err = "manifest too large";
        close(fd);
        return false;
    }
    if (!zip.beginEntry("manifest.json", 0) ||
        !zip.writeEntryBytes(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) ||
        !zip.endEntry() || !zip.finish()) {
        err = "zip finalize failed";
        close(fd);
        return false;
    }
    if (fsync(fd) != 0) {
        err = "fsync failed";
        close(fd);
        return false;
    }
    close(fd);
    if (outLayers != nullptr) {
        *outLayers = snap.layers.size();
    }
    if (outBytes != nullptr) {
        *outBytes = zip.totalBytes();
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "project saved: %{public}zu layers, %{public}lu bytes",
                 snap.layers.size(), static_cast<unsigned long>(zip.totalBytes()));
    return true;
}

}  // namespace io
}  // namespace montage
