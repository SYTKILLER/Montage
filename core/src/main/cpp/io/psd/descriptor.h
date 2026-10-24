#ifndef MONTAGE_IO_PSD_DESCRIPTOR_H
#define MONTAGE_IO_PSD_DESCRIPTOR_H

// PSD ActionDescriptor（OSType 标签键值树；photocraft psd/src/descriptor.rs C++ 翻译子集，
// MIT OR Apache-2.0）。lfx2/SoLd/TySh 等标记块的载体；解析与写出逐式对拍（write(parse(b))==b）。
// UnicodeString = u32 单元数 + UTF-16BE 单元；Id = u32 长度（0 = 4 字节 code）+ 字节。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace montage {
namespace psd {

constexpr int kDescMaxDepth = 64;

// 长度前缀标识（0 = 4 字节 code；非 0 = 显式长度字符串。"warp"/"list"/"time" 恒 Str）
struct DescId {
    bool isCode = true;
    std::string bytes;  // code 4 字节或字符串字节

    static DescId code(const char* c4);
    static DescId str(const std::string& s);
    bool is(const char* s) const;
    std::string text() const;
};

struct DescValue;

// 类引用（显示名 + 类 id）
struct DescClass {
    std::u16string name;
    DescId classId;
};

// 'obj ' 引用项
struct DescRefItem {
    enum Kind { Prop, Cls, Enmr, Offset, Identifier, Index, Name } kind = Prop;
    DescClass cls;
    DescId typeId;   // Enmr
    DescId valueId;  // Enmr
    DescId key;      // Prop
    std::u16string name;  // Name
    int32_t offset = 0;
    uint32_t id = 0;
};

// 描述符值（OSType 标签）
struct DescValue {
    enum Type {
        None = 0,
        Reference,   // 'obj '
        Object,      // 'Objc'
        GlobalObject,// 'GlbO'
        List,        // 'VlLs'
        Double,      // 'doub'
        UnitFloat,   // 'UntF'
        UnitFloats,  // 'UnFl'
        Text,        // 'TEXT'
        Enumerated,  // 'enum'
        Integer,     // 'long'
        LargeInteger,// 'comp'
        Boolean,     // 'bool'
        Cls,         // 'type'
        GlobalCls,   // 'GlbC'
        Alias,       // 'alis'
        Path,        // 'Pth '
        RawData,     // 'tdta'
        ObjectArray, // 'ObAr'
    };
    Type type = None;
    std::vector<DescRefItem> refItems;
    std::shared_ptr<struct DescObject> obj;  // Object/GlobalObject/Cls/GlobalCls 共用
    std::vector<DescValue> list;
    double dbl = 0;
    char unit[4] = {0, 0, 0, 0};
    std::vector<double> doubles;
    std::u16string text;
    DescId enumType;
    DescId enumValue;
    int64_t integer = 0;
    bool b = false;
    std::vector<uint8_t> raw;  // Alias/Path/RawData
    uint32_t obArPrefix = 0;

    const DescValue* get(const char* key) const;  // Object/List-of-Desc 便捷查找
};

// ActionDescriptor：类名 + 类 id + 保序键值项
struct DescObject {
    std::u16string name;
    DescId classId;
    std::vector<std::pair<DescId, DescValue>> items;

    const DescValue* get(const char* key) const;
    void set(const char* key, DescValue v);   // 原位替换或追加
    void remove(const char* key);

    std::string textOf(const char* key) const;        // TEXT → UTF-8（去尾部 NUL）
    double numOf(const char* key) const;              // Double/Integer/UnitFloat → 值
    bool boolOf(const char* key, bool dflt) const;    // Boolean
    bool hasKey(const char* key) const;
};

// UTF-8 ↔ UTF-16（无 BOM；写侧 NUL 终止由调用方 append）
std::u16string utf8ToUtf16(const std::string& s);
std::string utf16ToUtf8(const std::u16string& u);

// 解析（data 必须整块消费；失败 false + err）
bool parseDescriptor(const uint8_t* data, size_t n, DescObject& out, std::string& err);
bool parseVersionedDescriptor(const uint8_t* data, size_t n, DescObject& out,
                              size_t* consumed, std::string& err);  // 前缀解析（尾部允许）
void writeDescriptor(const DescObject& d, std::vector<uint8_t>& out);
void writeVersionedDescriptor(const DescObject& d, std::vector<uint8_t>& out);  // version 16

}  // namespace psd
}  // namespace montage

#endif  // MONTAGE_IO_PSD_DESCRIPTOR_H
