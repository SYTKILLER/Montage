#include "io/codecs/codec_table.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "io/codecs/bmp_codec.h"
#include "io/codecs/exr_codec.h"
#include "io/codecs/gif_codec.h"
#include "io/codecs/hdr_codec.h"
#include "io/codecs/qoi_codec.h"
#include "io/codecs/tga_codec.h"
#include "io/codecs/tiff_codec.h"

namespace montage {
namespace io {
namespace codecs {

const std::vector<CodecCap>& capabilities() {
    static const std::vector<CodecCap> kCaps = {
        {"tiff", true, true, "8/16bit", ".tif;.tiff",
         "读写：无压缩/LZW/PackBits/Deflate，条带/瓦片，双字节序，BigTIFF 读；PS 分层 8bit 读（写出恒合成图）"},
        {"gif", true, true, "8bit 调色板", ".gif",
         "读写：首帧/单帧，87a/89a，interlace/透明（写出 median-cut+FS 抖动）"},
        {"bmp", true, true, "8bit", ".bmp;.dib",
         "读写：读 1/4/8/16/24/32bpp + RLE4/RLE8/BITFIELDS；写 32bpp V4（含 alpha）"},
        {"tga", true, true, "8bit", ".tga;.vda;.icb;.pic",
         "读写：8/15/16/24/32bpp，RLE+裸流，origin 翻转（写出 RLE）"},
        {"qoi", true, true, "8bit", ".qoi", "读写：QOI 规范全量（sRGB 域）"},
        {"hdr", true, false, "32f→8", ".hdr;.pic",
         "读：Radiance RGBE（新式/旧式 RLE）→ sRGB8；写不支持"},
        {"exr", true, false, "half/float→8", ".exr",
         "读：扫描线 NONE/RLE/ZIPS/ZIP；tiled/multipart/deep/PIZ 族不支持；写不支持"},
        {"webp", true, true, "8bit", ".webp",
         "读写走系统解码器（ImageSource/ImagePacker）——VP8 不自译"},
    };
    return kCaps;
}

std::string formatFromExt(const std::string& ext) {
    std::string e;
    for (char c : ext) {
        if (c == '.') continue;
        e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (e == "tif" || e == "tiff") return "tiff";
    if (e == "gif") return "gif";
    if (e == "bmp" || e == "dib") return "bmp";
    if (e == "tga" || e == "vda" || e == "icb" || e == "pic") return "tga";
    if (e == "qoi") return "qoi";
    if (e == "hdr") return "hdr";
    if (e == "exr") return "exr";
    if (e == "webp") return "webp";
    return "";
}

bool sniff(const uint8_t* data, size_t len, const std::string& fileName, std::string& fmt) {
    if (len >= 4 && std::memcmp(data, "II*\0", 4) == 0) {
        fmt = "tiff";
        return true;
    }
    if (len >= 4 && std::memcmp(data, "MM\0*", 4) == 0) {
        fmt = "tiff";
        return true;
    }
    if (len >= 4 && std::memcmp(data, "II+\0", 4) == 0) {
        fmt = "tiff";  // BigTIFF 走同一解码器
        return true;
    }
    if (len >= 4 && std::memcmp(data, "MM\0+", 4) == 0) {
        fmt = "tiff";
        return true;
    }
    if (len >= 4 && std::memcmp(data, "qoif", 4) == 0) {
        fmt = "qoi";
        return true;
    }
    if (len >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0)) {
        fmt = "gif";
        return true;
    }
    if (len >= 4 && std::memcmp(data, "BM", 2) == 0) {
        fmt = "bmp";
        return true;
    }
    if (len >= 4 && std::memcmp(data, "\x76\x2f\x31\x01", 4) == 0) {
        fmt = "exr";
        return true;
    }
    // Radiance：#?RADIANCE / #?RGBE
    if (len >= 11 && data[0] == '#' && data[1] == '?') {
        bool isHdr = false;
        for (size_t i = 2; i < std::min<size_t>(len, 12); ++i) {
            if (data[i] == '\n') break;
            if (i + 4 <= len && (std::memcmp(data + i, "RADI", 4) == 0 ||
                                 std::memcmp(data + i, "RGBE", 4) == 0)) {
                isHdr = true;
                break;
            }
        }
        if (isHdr) {
            fmt = "hdr";
            return true;
        }
    }
    // TGA：footer 签名（v2 文件）；无 footer 不可靠探测，交由扩展名/系统解码器
    if (len >= 18 && std::memcmp(data + len - 18, "TRUEVISION-XFILE.", 17) == 0) {
        fmt = "tga";
        return true;
    }
    // 扩展名兜底（TGA 无魔数主路径；其余格式不靠它防误判）
    const size_t dot = fileName.rfind('.');
    if (dot != std::string::npos) {
        const std::string f = formatFromExt(fileName.substr(dot));
        if (f == "tga") {
            fmt = "tga";
            return true;
        }
    }
    return false;
}

bool decode(const uint8_t* data, size_t len, const std::string& fmt, CodecImage& out,
            std::string& err) {
    if (fmt == "tiff") return tiffDecode(data, len, out, err);
    if (fmt == "gif") return gifDecode(data, len, out, err);
    if (fmt == "bmp") return bmpDecode(data, len, out, err);
    if (fmt == "tga") return tgaDecode(data, len, out, err);
    if (fmt == "qoi") return qoiDecode(data, len, out, err);
    if (fmt == "hdr") return hdrDecode(data, len, out, err);
    if (fmt == "exr") return exrDecode(data, len, out, err);
    err = "codec: unknown format '" + fmt + "'";
    return false;
}

bool decodeLayers(const uint8_t* data, size_t len, const std::string& fmt, uint32_t& w, uint32_t& h,
                  std::vector<CodecLayerRec>& layers, std::vector<std::string>& notes,
                  std::string& err) {
    if (fmt == "tiff") {
        return tiffDecodeLayers(data, len, w, h, layers, notes, err);
    }
    err = "no layers";
    return false;
}

bool encode(const std::string& fmt, const uint8_t* rgba, uint32_t w, uint32_t h, int quality,
            std::vector<uint8_t>& out, std::string& err) {
    (void)quality;  // v1 各格式无质量轴（GIF 颜色数走默认 256）
    if (fmt == "tiff") {
        // alpha 全不透明时写 RGB 省体积；否则 RGBA
        bool withAlpha = false;
        const size_t n = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < n; ++i) {
            if (rgba[i * 4 + 3] != 255) {
                withAlpha = true;
                break;
            }
        }
        return tiffEncode(rgba, w, h, 8, withAlpha, "deflate", out, err);
    }
    if (fmt == "gif") return gifEncode(rgba, w, h, 256, true, out, err);
    if (fmt == "bmp") {
        bool withAlpha = false;
        const size_t n = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < n; ++i) {
            if (rgba[i * 4 + 3] != 255) {
                withAlpha = true;
                break;
            }
        }
        return bmpEncode(rgba, w, h, withAlpha, out, err);
    }
    if (fmt == "tga") {
        bool withAlpha = false;
        const size_t n = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < n; ++i) {
            if (rgba[i * 4 + 3] != 255) {
                withAlpha = true;
                break;
            }
        }
        return tgaEncode(rgba, w, h, withAlpha, out, err);
    }
    if (fmt == "qoi") return qoiEncode(rgba, w, h, out, err);
    if (fmt == "hdr" || fmt == "exr" || fmt == "webp") {
        err = "codec: '" + fmt + "' write unsupported here";
        return false;
    }
    err = "codec: unknown format '" + fmt + "'";
    return false;
}

bool canExport(const std::string& fmt) {
    for (const CodecCap& c : capabilities()) {
        if (fmt == c.format) {
            return c.write;
        }
    }
    return false;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
