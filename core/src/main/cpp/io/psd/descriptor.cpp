#include "io/psd/descriptor.h"

// PSD ActionDescriptor 读写（photocraft psd/src/descriptor.rs C++ 翻译，MIT OR Apache-2.0；
// 逐式对拍 read_descriptor/read_value/write_value/write_reference_item 与 Id/UnicodeString 布局）。

#include <algorithm>
#include <cstring>

namespace montage {
namespace psd {

namespace {

// ---------- 读取游标（大端；越界即失败） ----------
struct Rd {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t p = 0;
    bool ok = true;

    bool take(size_t k, const uint8_t** out) {
        if (!ok || p + k > n) {
            ok = false;
            return false;
        }
        *out = d + p;
        p += k;
        return true;
    }
    bool u8v(uint8_t* v) {
        const uint8_t* b = nullptr;
        if (!take(1, &b)) {
            return false;
        }
        *v = b[0];
        return true;
    }
    bool u32v(uint32_t* v) {
        const uint8_t* b = nullptr;
        if (!take(4, &b)) {
            return false;
        }
        *v = static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 |
             static_cast<uint32_t>(b[2]) << 8 | static_cast<uint32_t>(b[3]);
        return true;
    }
    bool u64v(uint64_t* v) {
        const uint8_t* b = nullptr;
        if (!take(8, &b)) {
            return false;
        }
        *v = 0;
        for (int i = 0; i < 8; ++i) {
            *v = (*v << 8) | b[i];
        }
        return true;
    }
    bool i32v(int32_t* v) {
        uint32_t u = 0;
        if (!u32v(&u)) {
            return false;
        }
        std::memcpy(v, &u, 4);
        return true;
    }
    bool i64v(int64_t* v) {
        uint64_t u = 0;
        if (!u64v(&u)) {
            return false;
        }
        std::memcpy(v, &u, 8);
        return true;
    }
    bool f64v(double* v) {
        uint64_t u = 0;
        if (!u64v(&u)) {
            return false;
        }
        std::memcpy(v, &u, 8);
        return true;
    }
    bool bytesV(size_t k, std::vector<uint8_t>* out) {
        const uint8_t* b = nullptr;
        if (!take(k, &b)) {
            return false;
        }
        out->assign(b, b + k);
        return true;
    }
};

// ---------- 写出缓冲 ----------
struct Wr {
    std::vector<uint8_t>* out;
    void put(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out->insert(out->end(), b, b + n);
    }
    void putU8(uint8_t v) { out->push_back(v); }
    void putU32(uint32_t v) {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = static_cast<uint8_t>(v >> ((3 - i) * 8));
        }
        put(b, 4);
    }
    void putU64(uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<uint8_t>(v >> ((7 - i) * 8));
        }
        put(b, 8);
    }
    void putI32(int32_t v) {
        uint32_t u = 0;
        std::memcpy(&u, &v, 4);
        putU32(u);
    }
    void putI64(int64_t v) {
        uint64_t u = 0;
        std::memcpy(&u, &v, 8);
        putU64(u);
    }
    void putF64(double v) {
        uint64_t u = 0;
        std::memcpy(&u, &v, 8);
        putU64(u);
    }
    void putStr4(const char* s) { put(s, 4); }
};

void writeId(const DescId& id, Wr& w) {
    if (id.isCode) {
        w.putU32(0);
        w.put(id.bytes.data(), 4);
    } else {
        w.putU32(static_cast<uint32_t>(id.bytes.size()));
        w.put(id.bytes.data(), id.bytes.size());
    }
}

void writeUnicode(const std::u16string& u, Wr& w) {
    w.putU32(static_cast<uint32_t>(u.size()));
    for (char16_t c : u) {
        w.putU8(static_cast<uint8_t>(c >> 8));
        w.putU8(static_cast<uint8_t>(c & 0xFF));
    }
}

void writeClass(const DescClass& c, Wr& w) {
    writeUnicode(c.name, w);
    writeId(c.classId, w);
}

void writeValue(const DescValue& v, Wr& w);
void writeRefItem(const DescRefItem& it, Wr& w) {
    switch (it.kind) {
        case DescRefItem::Prop:
            w.putStr4("prop");
            writeClass(it.cls, w);
            writeId(it.key, w);
            break;
        case DescRefItem::Cls:
            w.putStr4("Clss");
            writeClass(it.cls, w);
            break;
        case DescRefItem::Enmr:
            w.putStr4("Enmr");
            writeClass(it.cls, w);
            writeId(it.typeId, w);
            writeId(it.valueId, w);
            break;
        case DescRefItem::Offset:
            w.putStr4("rele");
            writeClass(it.cls, w);
            w.putI32(it.offset);
            break;
        case DescRefItem::Identifier:
            w.putStr4("Idnt");
            w.putU32(it.id);
            break;
        case DescRefItem::Index:
            w.putStr4("indx");
            w.putU32(it.id);
            break;
        case DescRefItem::Name:
            w.putStr4("name");
            writeClass(it.cls, w);
            writeUnicode(it.name, w);
            break;
    }
}

const char* valueType(const DescValue& v) {
    switch (v.type) {
        case DescValue::Reference: return "obj ";
        case DescValue::Object: return "Objc";
        case DescValue::GlobalObject: return "GlbO";
        case DescValue::List: return "VlLs";
        case DescValue::Double: return "doub";
        case DescValue::UnitFloat: return "UntF";
        case DescValue::UnitFloats: return "UnFl";
        case DescValue::Text: return "TEXT";
        case DescValue::Enumerated: return "enum";
        case DescValue::Integer: return "long";
        case DescValue::LargeInteger: return "comp";
        case DescValue::Boolean: return "bool";
        case DescValue::Cls: return "type";
        case DescValue::GlobalCls: return "GlbC";
        case DescValue::Alias: return "alis";
        case DescValue::Path: return "Pth ";
        case DescValue::RawData: return "tdta";
        case DescValue::ObjectArray: return "ObAr";
        default: return "bool";
    }
}

void writeValue(const DescValue& v, Wr& w) {
    w.putStr4(valueType(v));
    switch (v.type) {
        case DescValue::Reference:
            w.putU32(static_cast<uint32_t>(v.refItems.size()));
            for (const DescRefItem& it : v.refItems) {
                writeRefItem(it, w);
            }
            break;
        case DescValue::Object:
        case DescValue::GlobalObject:
            if (v.obj != nullptr) {
                writeDescriptor(*v.obj, *w.out);
            } else {
                // 空对象：名字空 + 类 id code 0 + 0 项（合法形态）
                w.putU32(0);
                w.putU32(0);
                w.put("\0\0\0\0", 4);
                w.putU32(0);
            }
            break;
        case DescValue::List:
            w.putU32(static_cast<uint32_t>(v.list.size()));
            for (const DescValue& e : v.list) {
                writeValue(e, w);
            }
            break;
        case DescValue::Double:
            w.putF64(v.dbl);
            break;
        case DescValue::UnitFloat:
            w.put(v.unit, 4);
            w.putF64(v.dbl);
            break;
        case DescValue::UnitFloats:
            w.put(v.unit, 4);
            w.putU32(static_cast<uint32_t>(v.doubles.size()));
            for (double d : v.doubles) {
                w.putF64(d);
            }
            break;
        case DescValue::Text:
            writeUnicode(v.text, w);
            break;
        case DescValue::Enumerated:
            writeId(v.enumType, w);
            writeId(v.enumValue, w);
            break;
        case DescValue::Integer:
            w.putI32(static_cast<int32_t>(v.integer));
            break;
        case DescValue::LargeInteger:
            w.putI64(v.integer);
            break;
        case DescValue::Boolean:
            w.putU8(v.b ? 1 : 0);
            break;
        case DescValue::Cls:
        case DescValue::GlobalCls:
            if (v.obj != nullptr) {
                writeClass(DescClass{v.obj->name, v.obj->classId}, w);
            } else {
                w.putU32(0);
                w.putU32(0);
                w.put("\0\0\0\0", 4);
            }
            break;
        case DescValue::Alias:
        case DescValue::Path:
        case DescValue::RawData:
            w.putU32(static_cast<uint32_t>(v.raw.size()));
            w.put(v.raw.data(), v.raw.size());
            break;
        case DescValue::ObjectArray:
            w.putU32(v.obArPrefix);
            if (v.obj != nullptr) {
                writeDescriptor(*v.obj, *w.out);
            }
            break;
        default:
            w.putU8(0);
            break;
    }
}

bool readId(Rd& r, DescId* id) {
    uint32_t n = 0;
    if (!r.u32v(&n)) {
        return false;
    }
    if (n == 0) {
        const uint8_t* b = nullptr;
        if (!r.take(4, &b)) {
            return false;
        }
        id->isCode = true;
        id->bytes.assign(reinterpret_cast<const char*>(b), 4);
        return true;
    }
    if (n > 1024 * 1024) {
        return false;
    }
    std::vector<uint8_t> buf;
    if (!r.bytesV(n, &buf)) {
        return false;
    }
    id->isCode = false;
    id->bytes.assign(buf.begin(), buf.end());
    return true;
}

bool readUnicode(Rd& r, std::u16string* u) {
    uint32_t n = 0;
    if (!r.u32v(&n) || n > 16 * 1024 * 1024) {
        return false;
    }
    u->clear();
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* b = nullptr;
        if (!r.take(2, &b)) {
            return false;
        }
        u->push_back(static_cast<char16_t>(static_cast<uint16_t>(b[0]) << 8 | b[1]));
    }
    return true;
}

bool readClass(Rd& r, DescClass* c) {
    return readUnicode(r, &c->name) && readId(r, &c->classId);
}

bool readRefItem(Rd& r, DescRefItem* it);
bool readValue(Rd& r, int depth, DescValue* v);

bool readRefItem(Rd& r, DescRefItem* it) {
    const uint8_t* ty = nullptr;
    if (!r.take(4, &ty)) {
        return false;
    }
    if (std::memcmp(ty, "prop", 4) == 0) {
        it->kind = DescRefItem::Prop;
        return readClass(r, &it->cls) && readId(r, &it->key);
    }
    if (std::memcmp(ty, "Clss", 4) == 0) {
        it->kind = DescRefItem::Cls;
        return readClass(r, &it->cls);
    }
    if (std::memcmp(ty, "Enmr", 4) == 0) {
        it->kind = DescRefItem::Enmr;
        return readClass(r, &it->cls) && readId(r, &it->typeId) && readId(r, &it->valueId);
    }
    if (std::memcmp(ty, "rele", 4) == 0) {
        it->kind = DescRefItem::Offset;
        return readClass(r, &it->cls) && r.i32v(&it->offset);
    }
    if (std::memcmp(ty, "Idnt", 4) == 0) {
        it->kind = DescRefItem::Identifier;
        return r.u32v(&it->id);
    }
    if (std::memcmp(ty, "indx", 4) == 0) {
        it->kind = DescRefItem::Index;
        return r.u32v(&it->id);
    }
    if (std::memcmp(ty, "name", 4) == 0) {
        it->kind = DescRefItem::Name;
        return readClass(r, &it->cls) && readUnicode(r, &it->name);
    }
    return false;
}

bool readObject(Rd& r, int depth, DescObject* out);

bool readValue(Rd& r, int depth, DescValue* v) {
    if (depth > kDescMaxDepth) {
        return false;
    }
    const uint8_t* ty = nullptr;
    if (!r.take(4, &ty)) {
        return false;
    }
    if (std::memcmp(ty, "obj ", 4) == 0) {
        v->type = DescValue::Reference;
        uint32_t n = 0;
        if (!r.u32v(&n) || n > 4096) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            DescRefItem it;
            if (!readRefItem(r, &it)) {
                return false;
            }
            v->refItems.push_back(std::move(it));
        }
        return true;
    }
    if (std::memcmp(ty, "Objc", 4) == 0 || std::memcmp(ty, "GlbO", 4) == 0) {
        v->type = std::memcmp(ty, "Objc", 4) == 0 ? DescValue::Object : DescValue::GlobalObject;
        v->obj = std::make_shared<DescObject>();
        return readObject(r, depth + 1, v->obj.get());
    }
    if (std::memcmp(ty, "VlLs", 4) == 0) {
        v->type = DescValue::List;
        uint32_t n = 0;
        if (!r.u32v(&n) || n > 16 * 1024 * 1024) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            DescValue e;
            if (!readValue(r, depth + 1, &e)) {
                return false;
            }
            v->list.push_back(std::move(e));
        }
        return true;
    }
    if (std::memcmp(ty, "doub", 4) == 0) {
        v->type = DescValue::Double;
        return r.f64v(&v->dbl);
    }
    if (std::memcmp(ty, "UntF", 4) == 0) {
        v->type = DescValue::UnitFloat;
        const uint8_t* u = nullptr;
        return r.take(4, &u) && r.f64v(&v->dbl) &&
               (std::memcpy(v->unit, u, 4), true);
    }
    if (std::memcmp(ty, "UnFl", 4) == 0) {
        v->type = DescValue::UnitFloats;
        const uint8_t* u = nullptr;
        uint32_t n = 0;
        if (!r.take(4, &u) || !r.u32v(&n) || n > 1024 * 1024) {
            return false;
        }
        std::memcpy(v->unit, u, 4);
        for (uint32_t i = 0; i < n; ++i) {
            double d = 0;
            if (!r.f64v(&d)) {
                return false;
            }
            v->doubles.push_back(d);
        }
        return true;
    }
    if (std::memcmp(ty, "TEXT", 4) == 0) {
        v->type = DescValue::Text;
        return readUnicode(r, &v->text);
    }
    if (std::memcmp(ty, "enum", 4) == 0) {
        v->type = DescValue::Enumerated;
        return readId(r, &v->enumType) && readId(r, &v->enumValue);
    }
    if (std::memcmp(ty, "long", 4) == 0) {
        v->type = DescValue::Integer;
        int32_t i = 0;
        if (!r.i32v(&i)) {
            return false;
        }
        v->integer = i;
        return true;
    }
    if (std::memcmp(ty, "comp", 4) == 0) {
        v->type = DescValue::LargeInteger;
        return r.i64v(&v->integer);
    }
    if (std::memcmp(ty, "bool", 4) == 0) {
        v->type = DescValue::Boolean;
        uint8_t b = 0;
        if (!r.u8v(&b)) {
            return false;
        }
        v->b = b != 0;
        return true;
    }
    if (std::memcmp(ty, "type", 4) == 0 || std::memcmp(ty, "GlbC", 4) == 0) {
        v->type = std::memcmp(ty, "type", 4) == 0 ? DescValue::Cls : DescValue::GlobalCls;
        DescClass c;
        if (!readClass(r, &c)) {
            return false;
        }
        v->obj = std::make_shared<DescObject>();
        v->obj->name = c.name;
        v->obj->classId = c.classId;
        return true;
    }
    if (std::memcmp(ty, "alis", 4) == 0 || std::memcmp(ty, "Pth ", 4) == 0 ||
        std::memcmp(ty, "tdta", 4) == 0) {
        v->type = std::memcmp(ty, "alis", 4) == 0
                      ? DescValue::Alias
                      : (std::memcmp(ty, "Pth ", 4) == 0 ? DescValue::Path : DescValue::RawData);
        uint32_t n = 0;
        if (!r.u32v(&n) || n > 64 * 1024 * 1024) {
            return false;
        }
        return r.bytesV(n, &v->raw);
    }
    if (std::memcmp(ty, "ObAr", 4) == 0) {
        v->type = DescValue::ObjectArray;
        v->obj = std::make_shared<DescObject>();
        return r.u32v(&v->obArPrefix) && readObject(r, depth + 1, v->obj.get());
    }
    return false;
}

bool readObject(Rd& r, int depth, DescObject* out) {
    if (depth > kDescMaxDepth) {
        return false;
    }
    if (!readUnicode(r, &out->name) || !readId(r, &out->classId)) {
        return false;
    }
    uint32_t n = 0;
    if (!r.u32v(&n) || n > 1024 * 1024) {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        DescId key;
        DescValue v;
        if (!readId(r, &key) || !readValue(r, depth + 1, &v)) {
            return false;
        }
        out->items.emplace_back(std::move(key), std::move(v));
    }
    return true;
}

}  // namespace

DescId DescId::code(const char* c4) {
    DescId id;
    id.isCode = true;
    id.bytes.assign(c4, 4);
    return id;
}

DescId DescId::str(const std::string& s) {
    DescId id;
    // photocraft STRING_IDS：'warp'/'list'/'time' 4 字节也按字符串存
    if (s.size() == 4 && std::memcmp(s.data(), "warp", 4) != 0 &&
        std::memcmp(s.data(), "list", 4) != 0 && std::memcmp(s.data(), "time", 4) != 0) {
        id.isCode = true;
    } else {
        id.isCode = false;
    }
    id.bytes = s;
    return id;
}

bool DescId::is(const char* s) const {
    const size_t n = std::strlen(s);
    return bytes.size() == n && std::memcmp(bytes.data(), s, n) == 0;
}

std::string DescId::text() const {
    return bytes;
}

const DescValue* DescObject::get(const char* key) const {
    for (const auto& [k, v] : items) {
        if (k.is(key)) {
            return &v;
        }
    }
    return nullptr;
}

void DescObject::set(const char* key, DescValue v) {
    for (auto& [k, val] : items) {
        if (k.is(key)) {
            val = std::move(v);
            return;
        }
    }
    items.emplace_back(DescId::str(key), std::move(v));
}

void DescObject::remove(const char* key) {
    items.erase(std::remove_if(items.begin(), items.end(),
                               [key](const std::pair<DescId, DescValue>& e) {
                                   return e.first.is(key);
                               }),
                items.end());
}

const DescValue* DescValue::get(const char* key) const {
    if ((type == Object || type == GlobalObject) && obj != nullptr) {
        return obj->get(key);
    }
    if (type == List) {
        for (const DescValue& e : list) {
            if ((e.type == Object || e.type == GlobalObject) && e.obj != nullptr &&
                e.obj->classId.is(key)) {
                return &e;
            }
        }
    }
    return nullptr;
}

std::string DescObject::textOf(const char* key) const {
    const DescValue* v = get(key);
    if (v == nullptr || v->type != DescValue::Text) {
        return "";
    }
    std::u16string u = v->text;
    while (!u.empty() && u.back() == u'\0') {
        u.pop_back();
    }
    return utf16ToUtf8(u);
}

double DescObject::numOf(const char* key) const {
    const DescValue* v = get(key);
    if (v == nullptr) {
        return 0.0;
    }
    if (v->type == DescValue::Double || v->type == DescValue::UnitFloat) {
        return v->dbl;
    }
    if (v->type == DescValue::Integer || v->type == DescValue::LargeInteger) {
        return static_cast<double>(v->integer);
    }
    return 0.0;
}

bool DescObject::boolOf(const char* key, bool dflt) const {
    const DescValue* v = get(key);
    if (v == nullptr || v->type != DescValue::Boolean) {
        return dflt;
    }
    return v->b;
}

bool DescObject::hasKey(const char* key) const {
    return get(key) != nullptr;
}

std::u16string utf8ToUtf16(const std::string& s) {
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6 |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = (c & 0x07u) << 18 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12 |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6 |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf16ToUtf8(const std::u16string& u) {
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = static_cast<uint16_t>(u[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < u.size()) {
            const uint32_t lo = static_cast<uint16_t>(u[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool parseDescriptor(const uint8_t* data, size_t n, DescObject& out, std::string& err) {
    Rd r{data, n, 0, true};
    if (!readObject(r, 0, &out) || r.p != n) {
        err = "descriptor parse failed";
        return false;
    }
    return true;
}

bool parseVersionedDescriptor(const uint8_t* data, size_t n, DescObject& out, size_t* consumed,
                              std::string& err) {
    Rd r{data, n, 0, true};
    uint32_t version = 0;
    if (!r.u32v(&version) || version != 16) {
        err = "descriptor version != 16";
        return false;
    }
    if (!readObject(r, 0, &out)) {
        err = "versioned descriptor parse failed";
        return false;
    }
    if (consumed != nullptr) {
        *consumed = r.p;
    }
    return true;
}

void writeDescriptor(const DescObject& d, std::vector<uint8_t>& out) {
    Wr w{&out};
    writeUnicode(d.name, w);
    writeId(d.classId, w);
    w.putU32(static_cast<uint32_t>(d.items.size()));
    for (const auto& [k, v] : d.items) {
        writeId(k, w);
        writeValue(v, w);
    }
}

void writeVersionedDescriptor(const DescObject& d, std::vector<uint8_t>& out) {
    Wr w{&out};
    w.putU32(16);
    std::vector<uint8_t> body;
    writeDescriptor(d, body);
    w.put(body.data(), body.size());
}

}  // namespace psd
}  // namespace montage
