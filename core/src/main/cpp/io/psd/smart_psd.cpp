#include "io/psd/smart_psd.h"

// PSD 智能对象双向实现（photocraft smart_map.rs/linked.rs 逐式对拍，MIT OR Apache-2.0）。

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "io/psd/descriptor.h"

namespace montage {
namespace psd {

namespace {

// ---------- linked 块游标 ----------
struct Lrd {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t p = 0;

    bool take(size_t k, const uint8_t** out) {
        if (p + k > n) {
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
    // Unicode 文件名（u32 单元数 + UTF-16BE；去尾部 NUL）
    bool unicodeStr(std::string* out) {
        uint32_t units = 0;
        if (!u32v(&units) || units > 4096) {
            return false;
        }
        std::u16string u;
        for (uint32_t i = 0; i < units; ++i) {
            const uint8_t* b = nullptr;
            if (!take(2, &b)) {
                return false;
            }
            u.push_back(static_cast<char16_t>(static_cast<uint16_t>(b[0]) << 8 | b[1]));
        }
        while (!u.empty() && u.back() == u'\0') {
            u.pop_back();
        }
        *out = utf16ToUtf8(u);
        return true;
    }
};

// 一项的解析（photocraft parse_item；非 liFD 或畸形 → false）
bool parseLinkedItem(const uint8_t* item, size_t n, LinkedFile* out) {
    Lrd r{item, n, 0};
    const uint8_t* kind = nullptr;
    uint32_t version = 0;
    uint8_t uuidLen = 0;
    if (!r.take(4, &kind) || !r.u32v(&version) || !r.u8v(&uuidLen)) {
        return false;
    }
    const uint8_t* uuidBytes = nullptr;
    if (!r.take(uuidLen, &uuidBytes)) {
        return false;
    }
    out->uuid.assign(reinterpret_cast<const char*>(uuidBytes), uuidLen);
    if (!r.unicodeStr(&out->fileName)) {
        return false;
    }
    const uint8_t* skip = nullptr;
    if (!r.take(8, &skip)) {  // file type + creator
        return false;
    }
    uint64_t len = 0;
    if (!r.u64v(&len) || len > 512ull * 1024 * 1024) {
        return false;
    }
    uint8_t openFlag = 0;
    if (!r.u8v(&openFlag)) {
        return false;
    }
    if (openFlag != 0) {
        // versioned descriptor 跳过（尾部允许）
        DescObject d;
        size_t used = 0;
        std::string err;
        if (!parseVersionedDescriptor(item + r.p, n - r.p, d, &used, err)) {
            return false;
        }
        r.p += used;
    }
    if (std::memcmp(kind, "liFD", 4) != 0) {
        return false;  // liFE 外部 / liFA 别名不承载内嵌字节
    }
    const uint8_t* bytes = nullptr;
    if (!r.take(static_cast<size_t>(len), &bytes)) {
        return false;
    }
    out->bytes.assign(bytes, bytes + static_cast<size_t>(len));
    return true;
}

// ---------- 滤镜映射（photocraft KNOWN 表 ↔ montage FilterKind） ----------

struct KnownFilter {
    const char* cls;      // PS class id（4 字节）
    const char* name;     // PS 菜单名
    int32_t kind;         // montage FilterKind；-1 = 无对应
};

const KnownFilter kKnown[] = {
    {"GsnB", "Gaussian Blur...", 0},
    {"boxblur", "Box Blur...", 1},
    {"MtnB", "Motion Blur...", 2},
    {"UnsM", "Unsharp Mask...", 3},
    {"AdNs", "Add Noise...", 6},
    {"Mdn ", "Median...", 7},
    {"HghP", "High Pass...", 5},
    {"Mxm ", "Maximum...", 25},
    {"Mnm ", "Minimum...", 24},
    {"Msc ", "Mosaic...", 10},
    {"Embs", "Emboss...", 17},
};

const KnownFilter* knownByClass(const std::string& cls) {
    for (const KnownFilter& k : kKnown) {
        if (cls == k.cls) {
            return &k;
        }
    }
    return nullptr;
}

const KnownFilter* knownByKind(int32_t kind) {
    for (const KnownFilter& k : kKnown) {
        if (k.kind == kind) {
            return &k;
        }
    }
    return nullptr;
}

int32_t filterIdOf(const char* cls) {
    // photocraft code()：4 字节大端 i32
    const uint8_t* b = reinterpret_cast<const uint8_t*>(cls);
    return static_cast<int32_t>(static_cast<uint32_t>(b[0]) << 24 |
                                static_cast<uint32_t>(b[1]) << 16 |
                                static_cast<uint32_t>(b[2]) << 8 | static_cast<uint32_t>(b[3]));
}

// 混合键表（对拍 photocraft effects_map BLEND_NAMES/BLEND_LONG_NAMES；Montage 24 值）
struct BlendName {
    BlendMode mode;
    const char* code;      // 4 字符键
    const char* longName;  // 长名（有则写长名）
};

const BlendName kBlendNames[] = {
    {BlendMode::Normal, "Nrml", "normal"},
    {BlendMode::Darken, "Drkn", "darken"},
    {BlendMode::Multiply, "Mltp", "multiply"},
    {BlendMode::ColorBurn, "CBrn", "colorBurn"},
    {BlendMode::LinearBurn, "linearBurn", nullptr},
    {BlendMode::Lighten, "Lghn", "lighten"},
    {BlendMode::Screen, "Scrn", "screen"},
    {BlendMode::ColorDodge, "CDdg", "colorDodge"},
    {BlendMode::LinearDodge, "linearDodge", nullptr},
    {BlendMode::Overlay, "Ovrl", "overlay"},
    {BlendMode::SoftLight, "SftL", "softLight"},
    {BlendMode::HardLight, "HrdL", "hardLight"},
    {BlendMode::VividLight, "vividLight", nullptr},
    {BlendMode::LinearLight, "linearLight", nullptr},
    {BlendMode::PinLight, "pinLight", nullptr},
    {BlendMode::HardMix, "hardMix", nullptr},
    {BlendMode::Difference, "Dfrn", "difference"},
    {BlendMode::Exclusion, "Xclu", "exclusion"},
    {BlendMode::Subtract, "blendSubtraction", nullptr},
    {BlendMode::Divide, "blendDivide", nullptr},
    {BlendMode::Hue, "H   ", "hue"},
    {BlendMode::Saturation, "Strt", "saturation"},
    {BlendMode::Color, "Clr ", "color"},
    {BlendMode::Luminosity, "Lmns", "luminosity"},
};

bool blendFromEnumValue(const std::string& v, BlendMode* out) {
    for (const BlendName& b : kBlendNames) {
        if (v == b.code || (b.longName != nullptr && v == b.longName)) {
            *out = b.mode;
            return true;
        }
    }
    return false;
}

const char* blendCode(BlendMode m) {
    for (const BlendName& b : kBlendNames) {
        if (b.mode == m) {
            return b.longName != nullptr ? b.longName : b.code;
        }
    }
    return "normal";
}

std::string hexOf(const std::vector<uint8_t>& bytes) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(H[b >> 4]);
        s.push_back(H[b & 15]);
    }
    return s;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool bytesOfHex(const std::string& s, std::vector<uint8_t>* out) {
    if (s.size() % 2 != 0) {
        return false;
    }
    out->clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = hexVal(s[i]);
        const int lo = hexVal(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out->push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

// 单滤镜参数提取（photocraft known_params；None = 有 Fltr 但参数不可映射 → unsupported）
bool paramsFromClass(const std::string& cls, const DescObject* fltr, SmartFilter* sf) {
    auto px = [&](const char* k) -> double { return fltr != nullptr ? fltr->numOf(k) : 0.0; };
    auto iv = [&](const char* k) -> int32_t {
        if (fltr == nullptr) {
            return 0;
        }
        const DescValue* v = fltr->get(k);
        return v != nullptr && v->type == DescValue::Integer ? static_cast<int32_t>(v->integer) : 0;
    };
    if (cls == "GsnB" || cls == "boxblur" || cls == "Mdn " || cls == "HghP" || cls == "Mxm " ||
        cls == "Mnm ") {
        sf->params.p0 = static_cast<float>(px("Rds "));
        return true;
    }
    if (cls == "MtnB") {
        sf->params.p0 = static_cast<float>(iv("Angl"));
        sf->params.p1 = static_cast<float>(px("Dstn"));
        return true;
    }
    if (cls == "UnsM") {
        sf->params.p0 = static_cast<float>(px("Amnt"));
        sf->params.p1 = static_cast<float>(px("Rds "));
        sf->params.p2 = static_cast<float>(iv("Thsh"));
        return true;
    }
    if (cls == "AdNs") {
        // Dstr enum（Unfr=均匀/Gsn =高斯）+ Nose % + Mnch 单色 + FlRs 种子
        if (fltr == nullptr) {
            return false;
        }
        const DescValue* dstr = fltr->get("Dstr");
        if (dstr == nullptr || dstr->type != DescValue::Enumerated) {
            return false;
        }
        const std::string val = dstr->enumValue.text();
        if (val != "Unfr" && val != "Gsn ") {
            return false;
        }
        sf->params.p0 = static_cast<float>(px("Nose"));
        sf->params.p1 = val == "Gsn " ? 1.0f : 0.0f;
        const DescValue* mnch = fltr->get("Mnch");
        if (mnch == nullptr || mnch->type != DescValue::Boolean) {
            return false;
        }
        sf->params.p2 = mnch->b ? 1.0f : 0.0f;
        sf->params.seed = static_cast<uint32_t>(iv("FlRs"));
        return true;
    }
    if (cls == "Msc ") {
        sf->params.p0 = static_cast<float>(px("ClSz"));
        return true;
    }
    if (cls == "Embs") {
        sf->params.p0 = static_cast<float>(iv("Angl"));
        sf->params.p1 = static_cast<float>(iv("Hght"));
        sf->params.p2 = static_cast<float>(iv("Amnt"));
        return true;
    }
    return false;
}

// 一个 filterFXList item → SmartFilter（photocraft filter_from_item）
bool filterFromItem(const DescObject& item, SmartFilter* sf) {
    const DescObject* opts = nullptr;
    if (const DescValue* bv = item.get("blendOptions")) {
        if ((bv->type == DescValue::Object || bv->type == DescValue::GlobalObject) &&
            bv->obj != nullptr) {
            opts = bv->obj.get();
        }
    }
    double opacity = 1.0;
    if (opts != nullptr) {
        const DescValue* opct = opts->get("Opct");
        if (opct != nullptr && opct->type == DescValue::UnitFloat) {
            opacity = opct->dbl / 100.0;
        }
    }
    sf->opacity = std::min(1.0, std::max(0.0, opacity));
    sf->blend = BlendMode::Normal;
    if (opts != nullptr) {
        const DescValue* md = opts->get("Md  ");
        if (md != nullptr && md->type == DescValue::Enumerated) {
            blendFromEnumValue(md->enumValue.text(), &sf->blend);
        }
    }
    sf->visible = item.boolOf("enab", true);
    sf->name = item.textOf("Nm  ");
    const int32_t filterId = static_cast<int32_t>(item.numOf("filterID"));
    const DescObject* fltr = nullptr;
    if (const DescValue* fv = item.get("Fltr")) {
        if ((fv->type == DescValue::Object || fv->type == DescValue::GlobalObject) &&
            fv->obj != nullptr) {
            fltr = fv->obj.get();
        }
    }
    const std::string cls = fltr != nullptr ? fltr->classId.text() : "";
    const KnownFilter* known = cls.empty() ? nullptr : knownByClass(cls);
    if (known == nullptr && filterId != 0) {
        for (const KnownFilter& k : kKnown) {
            if (filterIdOf(k.cls) == filterId) {
                known = &k;
                break;
            }
        }
    }
    if (known != nullptr) {
        sf->kind = known->kind;
        sf->psdClass = known->cls;
        if (!paramsFromClass(known->cls, fltr, sf)) {
            // 参数不可映射（如 AdNs 变体）→ unsupported 保 descriptor
            sf->kind = -1;
        }
    }
    if (sf->kind < 0) {
        // 未实现滤镜：item descriptor 原样 hex（导出时 set blendOptions/enab 回写当前态）
        sf->kind = -1;
        sf->name = sf->name.empty() ? "PSD 滤镜" : sf->name;
        std::vector<uint8_t> raw;
        writeDescriptor(item, raw);
        sf->psdHex = hexOf(raw);
    }
    return true;
}

// SmartFilter → filterFXList item descriptor（photocraft item_for_filter）
bool itemForFilter(const SmartFilter& f, DescObject* out) {
    DescValue blendOpts;
    blendOpts.type = DescValue::Object;
    blendOpts.obj = std::make_shared<DescObject>();
    blendOpts.obj->classId = DescId::str("blendOptions");
    {
        DescValue opct;
        opct.type = DescValue::UnitFloat;
        std::memcpy(opct.unit, "#Prc", 4);
        opct.dbl = std::isfinite(f.opacity)
                       ? std::min(1.0, std::max(0.0, f.opacity)) * 100.0
                       : 100.0;
        blendOpts.obj->items.emplace_back(DescId::str("Opct"), std::move(opct));
        DescValue md;
        md.type = DescValue::Enumerated;
        md.enumType = DescId::str("BlnM");
        md.enumValue = DescId::str(blendCode(f.blend));
        blendOpts.obj->items.emplace_back(DescId::str("Md  "), std::move(md));
    }
    if (f.kind < 0) {
        // 未实现滤镜：descriptor 自 hex 还原 + blend/enab 回写当前态
        std::vector<uint8_t> raw;
        if (!bytesOfHex(f.psdHex, &raw)) {
            return false;
        }
        DescObject d;
        std::string err;
        if (!parseDescriptor(raw.data(), raw.size(), d, err)) {
            return false;
        }
        d.set("blendOptions", std::move(blendOpts));
        {
            DescValue enab;
            enab.type = DescValue::Boolean;
            enab.b = f.visible;
            d.set("enab", std::move(enab));
        }
        *out = std::move(d);
        return true;
    }
    const KnownFilter* k = knownByKind(f.kind);
    if (k == nullptr) {
        return false;
    }
    DescObject fltr;
    fltr.classId = DescId::str(k->cls);
    auto pxVal = [&](const char* key, double v) {
        DescValue d;
        d.type = DescValue::UnitFloat;
        std::memcpy(d.unit, "#Pxl", 4);
        d.dbl = v;
        fltr.items.emplace_back(DescId::str(key), std::move(d));
    };
    auto iVal = [&](const char* key, int32_t v) {
        DescValue d;
        d.type = DescValue::Integer;
        d.integer = v;
        fltr.items.emplace_back(DescId::str(key), std::move(d));
    };
    if (f.kind == 0 || f.kind == 1 || f.kind == 7 || f.kind == 5 || f.kind == 25 || f.kind == 24) {
        pxVal("Rds ", f.params.p0);
    } else if (f.kind == 2) {
        iVal("Angl", static_cast<int32_t>(std::lround(f.params.p0)));
        pxVal("Dstn", f.params.p1);
    } else if (f.kind == 3) {
        DescValue amnt;
        amnt.type = DescValue::UnitFloat;
        std::memcpy(amnt.unit, "#Prc", 4);
        amnt.dbl = f.params.p0;
        fltr.items.emplace_back(DescId::str("Amnt"), std::move(amnt));
        pxVal("Rds ", f.params.p1);
        iVal("Thsh", static_cast<int32_t>(std::lround(f.params.p2)));
    } else if (f.kind == 6) {
        DescValue dstr;
        dstr.type = DescValue::Enumerated;
        dstr.enumType = DescId::str("Dstr");
        dstr.enumValue = DescId::str(f.params.p1 >= 0.5f ? "Gsn " : "Unfr");
        fltr.items.emplace_back(DescId::str("Dstr"), std::move(dstr));
        DescValue nose;
        nose.type = DescValue::UnitFloat;
        std::memcpy(nose.unit, "#Prc", 4);
        nose.dbl = f.params.p0;
        fltr.items.emplace_back(DescId::str("Nose"), std::move(nose));
        DescValue mnch;
        mnch.type = DescValue::Boolean;
        mnch.b = f.params.p2 >= 0.5f;
        fltr.items.emplace_back(DescId::str("Mnch"), std::move(mnch));
        iVal("FlRs", static_cast<int32_t>(f.params.seed));
    } else if (f.kind == 10) {
        pxVal("ClSz", f.params.p0);
    } else if (f.kind == 17) {
        iVal("Angl", static_cast<int32_t>(std::lround(f.params.p0)));
        iVal("Hght", static_cast<int32_t>(std::lround(f.params.p1)));
        iVal("Amnt", static_cast<int32_t>(std::lround(f.params.p2)));
    } else {
        return false;
    }
    DescObject d;
    d.classId = DescId::str("filterFX");
    {
        DescValue nm;
        nm.type = DescValue::Text;
        nm.text = utf8ToUtf16(f.name.empty() ? k->name : f.name);
        nm.text.push_back(u'\0');
        d.items.emplace_back(DescId::str("Nm  "), std::move(nm));
    }
    d.items.emplace_back(DescId::str("blendOptions"), std::move(blendOpts));
    {
        DescValue enab;
        enab.type = DescValue::Boolean;
        enab.b = f.visible;
        d.items.emplace_back(DescId::str("enab"), std::move(enab));
        DescValue hasoptions;
        hasoptions.type = DescValue::Boolean;
        hasoptions.b = true;
        d.items.emplace_back(DescId::str("hasoptions"), std::move(hasoptions));
        DescValue frg;
        frg.type = DescValue::Object;
        frg.obj = std::make_shared<DescObject>();
        frg.obj->classId = DescId::str("RGBC");
        for (const char* k2 : {"Rd  ", "Grn ", "Bl  "}) {
            DescValue c;
            c.type = DescValue::Double;
            c.dbl = 0.0;
            frg.obj->items.emplace_back(DescId::str(k2), std::move(c));
        }
        d.items.emplace_back(DescId::str("FrgC"), std::move(frg));
        DescValue bck;
        bck.type = DescValue::Object;
        bck.obj = std::make_shared<DescObject>();
        bck.obj->classId = DescId::str("RGBC");
        for (const char* k2 : {"Rd  ", "Grn ", "Bl  "}) {
            DescValue c;
            c.type = DescValue::Double;
            c.dbl = 255.0;
            bck.obj->items.emplace_back(DescId::str(k2), std::move(c));
        }
        d.items.emplace_back(DescId::str("BckC"), std::move(bck));
        DescValue fx;
        fx.type = DescValue::Object;
        fx.obj = std::make_shared<DescObject>(std::move(fltr));
        d.items.emplace_back(DescId::str("Fltr"), std::move(fx));
        DescValue fid;
        fid.type = DescValue::Integer;
        fid.integer = filterIdOf(k->cls);
        d.items.emplace_back(DescId::str("filterID"), std::move(fid));
    }
    *out = std::move(d);
    return true;
}

// filterFX 栈 descriptor（photocraft filter_fx）
DescObject filterFxDescriptor(const SmartLayerState& sm) {
    DescObject fx;
    fx.classId = DescId::str("filterFXStyle");
    auto boolItem = [&](const char* k, bool v) {
        DescValue d;
        d.type = DescValue::Boolean;
        d.b = v;
        fx.items.emplace_back(DescId::str(k), std::move(d));
    };
    boolItem("enab", sm.filtersEnabled);
    boolItem("validAtPosition", true);
    boolItem("filterMaskEnable", sm.filterMask != nullptr && sm.filterMask->enabled);
    boolItem("filterMaskLinked", true);
    boolItem("filterMaskExtendWithWhite", true);
    DescValue list;
    list.type = DescValue::List;
    for (const SmartFilter& f : sm.filters) {
        DescObject item;
        if (itemForFilter(f, &item)) {
            DescValue dv;
            dv.type = DescValue::Object;
            dv.obj = std::make_shared<DescObject>(std::move(item));
            list.list.push_back(std::move(dv));
        }
    }
    fx.items.emplace_back(DescId::str("filterFXList"), std::move(list));
    return fx;
}

void putQuad(DescObject* d, const char* key, const double q[8]) {
    DescValue v;
    v.type = DescValue::List;
    for (int i = 0; i < 8; ++i) {
        DescValue e;
        e.type = DescValue::Double;
        e.dbl = q[i];
        v.list.push_back(std::move(e));
    }
    d->set(key, std::move(v));
}

// 线性 warp descriptor（photocraft warp_desc WarpStyle::None 分支）
DescValue warpDescriptor(double srcW, double srcH) {
    DescValue v;
    v.type = DescValue::Object;
    v.obj = std::make_shared<DescObject>();
    v.obj->classId = DescId::str("warp");
    auto enumv = [&](const char* t, const char* val) {
        DescValue e;
        e.type = DescValue::Enumerated;
        e.enumType = DescId::str(t);
        e.enumValue = DescId::str(val);
        return e;
    };
    v.obj->items.emplace_back(DescId::str("warpStyle"), enumv("warpStyle", "warpNone"));
    {
        DescValue d;
        d.type = DescValue::Double;
        d.dbl = 0.0;
        v.obj->items.emplace_back(DescId::str("warpValue"), std::move(d));
        v.obj->items.emplace_back(DescId::str("warpPerspective"), std::move(d));
        DescValue d2;
        d2.type = DescValue::Double;
        d2.dbl = 0.0;
        v.obj->items.emplace_back(DescId::str("warpPerspectiveOther"), std::move(d2));
    }
    v.obj->items.emplace_back(DescId::str("warpRotate"), enumv("Ornt", "Hrzn"));
    {
        DescValue bounds;
        bounds.type = DescValue::Object;
        bounds.obj = std::make_shared<DescObject>();
        bounds.obj->classId = DescId::str("classFloatRect");
        auto rect = [&](const char* k, double val) {
            DescValue d;
            d.type = DescValue::Double;
            d.dbl = val;
            bounds.obj->items.emplace_back(DescId::str(k), std::move(d));
        };
        rect("Top ", 0.0);
        rect("Left", 0.0);
        rect("Btom", srcH);
        rect("Rght", srcW);
        v.obj->items.emplace_back(DescId::str("bounds"), std::move(bounds));
    }
    {
        DescValue uo;
        uo.type = DescValue::Integer;
        uo.integer = 4;
        v.obj->items.emplace_back(DescId::str("uOrder"), std::move(uo));
        DescValue vo;
        vo.type = DescValue::Integer;
        vo.integer = 4;
        v.obj->items.emplace_back(DescId::str("vOrder"), std::move(vo));
    }
    return v;
}

}  // namespace

std::vector<LinkedFile> parseLinkedFiles(const uint8_t* data, size_t n) {
    std::vector<LinkedFile> out;
    size_t p = 0;
    while (p + 8 <= n) {
        uint64_t len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | data[p + i];
        }
        p += 8;
        if (len > n - p) {
            break;
        }
        LinkedFile f;
        if (parseLinkedItem(data + p, static_cast<size_t>(len), &f)) {
            out.push_back(std::move(f));
        }
        p += static_cast<size_t>(len);
        p = (p + 3) & ~size_t{3};  // 4 字节对齐（next_multiple_of(4)）
    }
    return out;
}

std::string linkedFileType(const std::vector<uint8_t>& b) {
    if (b.size() >= 6 && b[0] == '8' && b[1] == 'B' && b[2] == 'P' && b[3] == 'S') {
        return b[4] == 0 && b[5] == 2 ? "8BPB" : "8BPS";
    }
    if (b.size() >= 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        return "PNGf";
    }
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xD8) {
        return "JPEG";
    }
    if (b.size() >= 4 && ((b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0) ||
                          (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 42))) {
        return "TIFF";
    }
    if (b.size() >= 3 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
        return "GIFf";
    }
    return "    ";
}

std::vector<uint8_t> encodeLinkedFile(const LinkedFile& f) {
    std::vector<uint8_t> item;
    auto put = [&item](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        item.insert(item.end(), b, b + n);
    };
    auto putU32 = [&item, &put](uint32_t v) {
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = static_cast<uint8_t>(v >> ((3 - i) * 8));
        }
        put(b, 4);
    };
    auto putU64 = [&item, &put](uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<uint8_t>(v >> ((7 - i) * 8));
        }
        put(b, 8);
    };
    item.insert(item.end(), {'l', 'i', 'F', 'D'});
    putU32(7);  // version 7
    const std::string& uuid = f.uuid;
    const size_t uuidLen = std::min<size_t>(uuid.size(), 255);
    item.push_back(static_cast<uint8_t>(uuidLen));
    put(uuid.data(), uuidLen);
    // NUL 终止计入单元数（Photoshop 写法）
    std::u16string name = utf8ToUtf16(f.fileName);
    name.push_back(u'\0');
    putU32(static_cast<uint32_t>(name.size()));
    for (char16_t c : name) {
        item.push_back(static_cast<uint8_t>(c >> 8));
        item.push_back(static_cast<uint8_t>(c & 0xFF));
    }
    const std::string ft = linkedFileType(f.bytes);
    put(ft.data(), 4);
    put("8BIM", 4);
    putU64(f.bytes.size());
    item.push_back(0);  // open descriptor 无
    put(f.bytes.data(), f.bytes.size());
    putU32(0);  // child document id（version 5）
    putU64(0);  // asset modification time（version 6）
    item.push_back(0);  // asset locked（version 7）
    std::vector<uint8_t> out;
    {
        uint8_t lb[8];
        const uint64_t sz = static_cast<uint64_t>(item.size());
        for (int i = 0; i < 8; ++i) {
            lb[i] = static_cast<uint8_t>(sz >> ((7 - i) * 8));
        }
        out.insert(out.end(), lb, lb + 8);
    }
    out.insert(out.end(), item.begin(), item.end());
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
    return out;
}

bool parsePlaced(const std::string& key, const uint8_t* data, size_t n, PlacedSmart& out) {
    if (n < 8) {
        return false;
    }
    if (key == "SoLd" || key == "SoLE") {
        // 'soLD' + u32 version(4) + versioned descriptor（尾部 warp 允许）
        DescObject d;
        size_t used = 0;
        std::string err;
        if (!parseVersionedDescriptor(data + 8, n - 8, d, &used, err)) {
            return false;
        }
        out.idnt = d.textOf("Idnt");
        out.placedId = d.textOf("placed");
        if (const DescValue* sz = d.get("Sz  ")) {
            if ((sz->type == DescValue::Object || sz->type == DescValue::GlobalObject) &&
                sz->obj != nullptr) {
                out.srcW = sz->obj->numOf("Wdth");
                out.srcH = sz->obj->numOf("Hght");
            }
        }
        // Trnf 四角 → 仿射（placed_frame 无 warp = 源矩形 [0,0,w,h]）
        const DescValue* trnf = d.get("Trnf");
        if (trnf != nullptr && trnf->type == DescValue::List && trnf->list.size() == 8 &&
            out.srcW > 0.0 && out.srcH > 0.0) {
            double p[8];
            bool ok = true;
            for (int i = 0; i < 8; ++i) {
                p[i] = trnf->list[static_cast<size_t>(i)].type == DescValue::Double
                           ? trnf->list[static_cast<size_t>(i)].dbl
                           : 0.0;
                ok = ok && trnf->list[static_cast<size_t>(i)].type == DescValue::Double;
            }
            if (ok) {
                const double bw = out.srcW;
                const double bh = out.srcH;
                const double a = (p[2] - p[0]) / bw;
                const double b = (p[3] - p[1]) / bw;
                const double c = (p[6] - p[0]) / bh;
                const double dd = (p[7] - p[1]) / bh;
                out.m[0] = a;
                out.m[1] = b;
                out.m[2] = p[0] - a * 0.0 - c * 0.0;
                out.m[3] = c;
                out.m[4] = dd;
                out.m[5] = p[1] - b * 0.0 - dd * 0.0;
            }
        }
        // filterFX 栈
        if (const DescValue* fxv = d.get("filterFX")) {
            if ((fxv->type == DescValue::Object || fxv->type == DescValue::GlobalObject) &&
                fxv->obj != nullptr) {
                const DescObject& fx = *fxv->obj;
                out.hasStack = true;
                out.stackEnabled = fx.boolOf("enab", true);
                out.maskEnabled = fx.boolOf("filterMaskEnable", true);
                out.maskLinked = fx.boolOf("filterMaskLinked", false);
                if (const DescValue* list = fx.get("filterFXList")) {
                    if (list->type == DescValue::List) {
                        for (const DescValue& e : list->list) {
                            if ((e.type == DescValue::Object ||
                                 e.type == DescValue::GlobalObject) &&
                                e.obj != nullptr) {
                                SmartFilter sf;
                                if (filterFromItem(*e.obj, &sf)) {
                                    out.filters.push_back(std::move(sf));
                                }
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
    if (key == "PlLd") {
        // 'plcL' + u32 version(3) + Pascal uuid + 4×i32 + quad 8 doubles（legacy 无滤镜栈）
        Lrd r{data, n, 4};
        uint32_t version = 0;
        if (!r.u32v(&version) || version != 3) {
            return false;
        }
        uint8_t uuidLen = 0;
        const uint8_t* uuidBytes = nullptr;
        if (!r.u8v(&uuidLen) || !r.take(uuidLen, &uuidBytes)) {
            return false;
        }
        out.idnt.assign(reinterpret_cast<const char*>(uuidBytes), uuidLen);
        const uint8_t* skip = nullptr;
        if (!r.take(16, &skip)) {  // 4×i32 (1,1,16,2)
            return false;
        }
        double p[8];
        for (int i = 0; i < 8; ++i) {
            if (!r.u64v(reinterpret_cast<uint64_t*>(&p[i]))) {
                return false;
            }
        }
        out.srcW = 0.0;
        out.srcH = 0.0;
        out.m[0] = 1;
        out.m[1] = 0;
        out.m[2] = p[0];
        out.m[3] = 0;
        out.m[4] = 1;
        out.m[5] = p[1];
        return true;
    }
    return false;
}

std::string uuidFromContents(uint64_t contentsId) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint64_t>((contentsId >> (i * 8)) & 0xFF);
        h *= 1099511628211ull;
    }
    char buf[40];
    // v4 形态：version nibble=4、variant bits=10
    uint64_t hi = (h & 0xFFFFFFFF0FFFF0FFull) | 0x0000000000004000ull;
    uint64_t lo = (h ^ 0x9E3779B97F4A7C15ull) & 0x3FFFFFFFFFFFFFFFull | 0x8000000000000000ull;
    std::snprintf(buf, sizeof(buf), "%08llx-%04x-%04llx-%04llx-%012llx",
                  static_cast<unsigned long long>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(hi & 0xFFFF),
                  static_cast<unsigned long long>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    return buf;
}

bool filtersRepresentable(const std::vector<SmartFilter>& filters) {
    for (const SmartFilter& f : filters) {
        if (f.kind < 0) {
            if (f.psdHex.empty()) {
                return false;
            }
            continue;
        }
        if (knownByKind(f.kind) == nullptr) {
            return false;
        }
    }
    return true;
}

bool soldBytes(const SmartLayerState& sm, double dpi, std::vector<uint8_t>& out) {
    if (!filtersRepresentable(sm.filters)) {
        return false;
    }
    const double sw = sm.srcW > 0.0 ? sm.srcW : 0.0;
    const double sh = sm.srcH > 0.0 ? sm.srcH : 0.0;
    if (sw <= 0.0 || sh <= 0.0) {
        return false;
    }
    // 四角 = m 映射源框四点（TL/TR/BR/BL）
    const double q[8] = {
        sm.m[2], sm.m[5],                                     // TL = m·(0,0)
        sm.m[0] * sw + sm.m[2], sm.m[3] * sw + sm.m[5],       // TR = m·(w,0)
        sm.m[0] * sw + sm.m[1] * sh + sm.m[2],
        sm.m[3] * sw + sm.m[4] * sh + sm.m[5],                // BR
        sm.m[1] * sh + sm.m[2], sm.m[4] * sh + sm.m[5],       // BL
    };
    DescObject d;
    d.classId = DescId::str("null");
    auto textItem = [&](const char* k, const std::string& s) {
        DescValue v;
        v.type = DescValue::Text;
        v.text = utf8ToUtf16(s);
        v.text.push_back(u'\0');
        d.items.emplace_back(DescId::str(k), std::move(v));
    };
    auto longItem = [&](const char* k, int32_t v) {
        DescValue dv;
        dv.type = DescValue::Integer;
        dv.integer = v;
        d.items.emplace_back(DescId::str(k), std::move(dv));
    };
    textItem("Idnt", sm.uuid);
    textItem("placed", sm.placedId);
    longItem("PgNm", 1);
    longItem("totalPages", 1);
    longItem("Crop", 1);
    auto ratio = [&](const char* k, int32_t num, int32_t den) {
        DescValue v;
        v.type = DescValue::Object;
        v.obj = std::make_shared<DescObject>();
        v.obj->classId = DescId::str("null");
        DescValue n;
        n.type = DescValue::Integer;
        n.integer = num;
        v.obj->items.emplace_back(DescId::str("numerator"), std::move(n));
        DescValue dv2;
        dv2.type = DescValue::Integer;
        dv2.integer = den;
        v.obj->items.emplace_back(DescId::str("denominator"), std::move(dv2));
        d.items.emplace_back(DescId::str(k), std::move(v));
    };
    ratio("frameStep", 0, 600);
    ratio("duration", 0, 600);
    longItem("frameCount", 1);
    longItem("Annt", 16);
    longItem("Type", 2);
    putQuad(&d, "Trnf", q);
    putQuad(&d, "nonAffineTransform", q);
    d.items.emplace_back(DescId::str("warp"), warpDescriptor(sw, sh));
    {
        DescValue sz;
        sz.type = DescValue::Object;
        sz.obj = std::make_shared<DescObject>();
        sz.obj->classId = DescId::str("Pnt ");
        DescValue wv;
        wv.type = DescValue::Double;
        wv.dbl = sw;
        sz.obj->items.emplace_back(DescId::str("Wdth"), std::move(wv));
        DescValue hv;
        hv.type = DescValue::Double;
        hv.dbl = sh;
        sz.obj->items.emplace_back(DescId::str("Hght"), std::move(hv));
        d.items.emplace_back(DescId::str("Sz  "), std::move(sz));
    }
    {
        DescValue rslt;
        rslt.type = DescValue::UnitFloat;
        std::memcpy(rslt.unit, "#Rsl", 4);
        rslt.dbl = dpi > 0 ? dpi : 72.0;
        d.items.emplace_back(DescId::str("Rslt"), std::move(rslt));
    }
    // filterFX（有滤镜或整栈关/蒙版态非默认时写）
    const bool hasFilters = !sm.filters.empty();
    const bool maskDefault = sm.filterMask == nullptr;
    if (hasFilters || !sm.filtersEnabled || !maskDefault) {
        DescValue fxv;
        fxv.type = DescValue::Object;
        fxv.obj = std::make_shared<DescObject>(filterFxDescriptor(sm));
        d.items.emplace_back(DescId::str("filterFX"), std::move(fxv));
    }
    longItem("comp", -1);
    {
        DescValue compInfo;
        compInfo.type = DescValue::Object;
        compInfo.obj = std::make_shared<DescObject>();
        compInfo.obj->classId = DescId::str("null");
        DescValue cid;
        cid.type = DescValue::Integer;
        cid.integer = -1;
        compInfo.obj->items.emplace_back(DescId::str("compID"), std::move(cid));
        DescValue oid;
        oid.type = DescValue::Integer;
        oid.integer = -1;
        compInfo.obj->items.emplace_back(DescId::str("originalCompID"), std::move(oid));
        d.items.emplace_back(DescId::str("compInfo"), std::move(compInfo));
    }
    // 'soLD' + version 4 + versioned descriptor + pad4
    out.clear();
    out.insert(out.end(), {'s', 'o', 'L', 'D'});
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(4);
    writeVersionedDescriptor(d, out);
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
    return true;
}

}  // namespace psd
}  // namespace montage
