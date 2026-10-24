#ifndef MONTAGE_IO_RAW_RAW_TIFF_H
#define MONTAGE_IO_RAW_RAW_TIFF_H

// M23a RAW 解码内核：TIFF 6.0 / TIFF-EP 结构读取器。
// 翻译自 photocraft crates/raw/src/tiff.rs（MIT OR Apache-2.0，允许移植）。
// 只保留 raw 解码所需：带类型取值的 IFD 条目、IFD 链、SubIFDs(330)、EXIF IFD(34665)
// 与内嵌 IFD（maker note）。全部偏移对缓冲校验，越界读返回空；遍历有 IFD 数与
// 条目数上限，敌意文件不能死循环或撑爆内存。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace montage {
namespace raw {

// 标签号（TIFF 6.0、TIFF/EP、EXIF 与 DNG 1.7）。
namespace tag {
constexpr uint16_t NEW_SUBFILE_TYPE = 254;
constexpr uint16_t IMAGE_WIDTH = 256;
constexpr uint16_t IMAGE_LENGTH = 257;
constexpr uint16_t BITS_PER_SAMPLE = 258;
constexpr uint16_t COMPRESSION = 259;
constexpr uint16_t PREDICTOR = 317;
constexpr uint16_t PHOTOMETRIC = 262;
constexpr uint16_t MAKE = 271;
constexpr uint16_t MODEL = 272;
constexpr uint16_t STRIP_OFFSETS = 273;
constexpr uint16_t ORIENTATION = 274;
constexpr uint16_t SAMPLES_PER_PIXEL = 277;
constexpr uint16_t ROWS_PER_STRIP = 278;
constexpr uint16_t STRIP_BYTE_COUNTS = 279;
constexpr uint16_t PLANAR_CONFIGURATION = 284;
constexpr uint16_t TILE_WIDTH = 322;
constexpr uint16_t TILE_LENGTH = 323;
constexpr uint16_t TILE_OFFSETS = 324;
constexpr uint16_t TILE_BYTE_COUNTS = 325;
constexpr uint16_t SUB_IFDS = 330;
constexpr uint16_t SAMPLE_FORMAT = 339;
constexpr uint16_t JPEG_INTERCHANGE_FORMAT = 513;
constexpr uint16_t JPEG_INTERCHANGE_FORMAT_LENGTH = 514;
constexpr uint16_t CFA_REPEAT_PATTERN_DIM = 33421;
constexpr uint16_t CFA_PATTERN = 33422;
constexpr uint16_t EXIF_IFD = 34665;
constexpr uint16_t EXIF_CFA_PATTERN = 41730;
constexpr uint16_t MAKER_NOTE = 37500;
constexpr uint16_t COLOR_SPACE = 40961;
constexpr uint16_t INTEROP_IFD = 40965;
// Interoperability IFD 内。
constexpr uint16_t INTEROP_INDEX = 1;
constexpr uint16_t DNG_VERSION = 50706;
constexpr uint16_t UNIQUE_CAMERA_MODEL = 50708;
constexpr uint16_t CFA_PLANE_COLOR = 50710;
constexpr uint16_t CFA_LAYOUT = 50711;
constexpr uint16_t LINEARIZATION_TABLE = 50712;
constexpr uint16_t BLACK_LEVEL_REPEAT_DIM = 50713;
constexpr uint16_t BLACK_LEVEL = 50714;
constexpr uint16_t BLACK_LEVEL_DELTA_H = 50715;
constexpr uint16_t BLACK_LEVEL_DELTA_V = 50716;
constexpr uint16_t WHITE_LEVEL = 50717;
constexpr uint16_t DEFAULT_SCALE = 50718;
constexpr uint16_t DEFAULT_CROP_ORIGIN = 50719;
constexpr uint16_t DEFAULT_CROP_SIZE = 50720;
constexpr uint16_t COLOR_MATRIX_1 = 50721;
constexpr uint16_t COLOR_MATRIX_2 = 50722;
constexpr uint16_t CAMERA_CALIBRATION_1 = 50723;
constexpr uint16_t CAMERA_CALIBRATION_2 = 50724;
constexpr uint16_t ANALOG_BALANCE = 50727;
constexpr uint16_t AS_SHOT_NEUTRAL = 50728;
constexpr uint16_t AS_SHOT_WHITE_XY = 50729;
constexpr uint16_t BASELINE_EXPOSURE = 50730;
constexpr uint16_t CALIBRATION_ILLUMINANT_1 = 50778;
constexpr uint16_t CALIBRATION_ILLUMINANT_2 = 50779;
constexpr uint16_t ACTIVE_AREA = 50829;
constexpr uint16_t FORWARD_MATRIX_1 = 50964;
constexpr uint16_t FORWARD_MATRIX_2 = 50965;
constexpr uint16_t OPCODE_LIST_1 = 51008;
constexpr uint16_t OPCODE_LIST_2 = 51009;
constexpr uint16_t OPCODE_LIST_3 = 51022;
constexpr uint16_t BASELINE_EXPOSURE_OFFSET = 51109;
constexpr uint16_t PROFILE_GAIN_TABLE_MAP = 52525;
// raw IFD 里的 CR2 私有标签：切片数、切片宽、末切片宽。
constexpr uint16_t CR2_SLICE = 50752;
}  // namespace tag

// 一次遍历最多访问的 IFD 数 / 一个 IFD 最多条目数 / 列表访问器最多取值数。
constexpr size_t kMaxIfds = 64;
constexpr size_t kMaxEntries = 4096;
constexpr size_t kMaxValues = 1u << 20;

// 一个目录条目。
struct Entry {
    uint16_t tag = 0;
    uint16_t typ = 0;
    uint32_t count = 0;
    // 值字节在缓冲中的绝对偏移（可能越界；访问器自校验）。
    size_t at = 0;

    // 该条目类型单值字节宽（0 = 未知类型）。
    size_t unit() const;
};

// 一个图像文件目录。
struct Ifd {
    size_t offset = 0;
    std::vector<Entry> entries;

    const Entry* get(uint16_t t) const;
    bool has(uint16_t t) const { return get(t) != nullptr; }
};

size_t typeSize(uint16_t typ);

// TIFF 结构化缓冲。
struct Tiff {
    const uint8_t* data = nullptr;
    size_t len = 0;
    bool le = true;
    // IFD0 相对头的偏移。
    size_t firstIfd = 0;

    // 解析 8 字节头。接受 TIFF 魔数与相机 raw 的 TIFF 变体
    // （ORF IIRO/IIRS/MMOR、RW2 IIU\0）。失败返回 nullopt。
    static std::optional<Tiff> create(const uint8_t* data, size_t len);
    static std::optional<Tiff> create(const std::vector<uint8_t>& bytes) {
        return create(bytes.data(), bytes.size());
    }

    std::optional<size_t> rangeAt(size_t at, size_t len) const;  // 越界检查，返回 at 或空
    const uint8_t* bytes(size_t at, size_t len) const;
    std::optional<uint16_t> u16At(size_t at) const;
    std::optional<uint32_t> u32At(size_t at) const;
    std::optional<uint64_t> u64At(size_t at) const;

    // 读 `offset` 处 IFD。值偏移相对 `base`（普通 IFD 为 0；maker note 有时用自己的基）。
    std::optional<Ifd> ifdAt(size_t offset, size_t base) const;

    // `ifd` 之后那条 IFD 的偏移（0 = 无）。
    size_t nextIfd(const Ifd& ifd) const;

    // 从头可达的每条 IFD：主链、SubIFD 与 EXIF IFD，深度优先，去重有界。
    std::vector<Ifd> allIfds() const;

    // 条目第 `i` 个值按无符号整数读（BYTE/SHORT/LONG/IFD 与其有符号变体，负值拒绝）。
    std::optional<uint32_t> uint(const Entry& e, size_t i) const;
    // 条目第 `i` 个值按浮点读（含有理数）。
    std::optional<double> floatv(const Entry& e, size_t i) const;
    // 全部值按无符号整数（任一不可读则空表）。
    std::vector<uint32_t> uints(const Entry& e) const;
    // 全部值按浮点（任一不可读则空表）。
    std::vector<double> floats(const Entry& e) const;
    // 条目原始值字节。
    const uint8_t* raw(const Entry& e, size_t& outLen) const;
    std::optional<std::string> ascii(const Entry& e) const;

    std::optional<uint32_t> tagUint(const Ifd& ifd, uint16_t t) const;
    std::vector<uint32_t> tagUints(const Ifd& ifd, uint16_t t) const;
    std::vector<double> tagFloats(const Ifd& ifd, uint16_t t) const;
    std::optional<std::string> tagAscii(const Ifd& ifd, uint16_t t) const;
    const Entry* tagEntry(const Ifd& ifd, uint16_t t) const;
};

// raw 标签值对应的 EXIF orientation（1..=8）；其余一律默认 1。范围检查在 u32 上做
// （先窄化会让 65538 以 2 通过 1..=8 检查——上游 #1817）。
uint16_t orientationOf(std::optional<uint32_t> v);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_TIFF_H
