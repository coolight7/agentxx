/// agentxx::util::Json 实现 (simdjson 驱动解析 + 保序紧凑 DOM)
///
/// 内存模型: variant 风格 union + 显式 Type 标记
/// - 标量内联存储 (bool/int64/uint64/double), 字符串/数组/对象堆存储
/// - Object 保序 (`vector<pair<string, Json>>`), dump() 确定性输出
#include "agentxx/util/json.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>

#include "simdjson.h"

namespace agentxx {
namespace util {

namespace {

Json elementToJson(const simdjson::dom::element& elem) {
    using simdjson::dom::element_type;
    switch (elem.type()) {
        case element_type::NULL_VALUE:
            return Json{};
        case element_type::BOOL: {
            bool v = false;
            if (elem.get_bool().get(v)) {
                return Json{};
            }
            return Json(v);
        }
        case element_type::INT64: {
            int64_t v = 0;
            if (elem.get_int64().get(v)) {
                return Json{};
            }
            return Json(v);
        }
        case element_type::UINT64: {
            uint64_t v = 0;
            if (elem.get_uint64().get(v)) {
                return Json{};
            }
            return Json(v);
        }
        case element_type::DOUBLE: {
            double v = 0.0;
            if (elem.get_double().get(v)) {
                return Json{};
            }
            return Json(v);
        }
        case element_type::STRING: {
            std::string_view sv;
            if (elem.get_string().get(sv)) {
                return Json{};
            }
            return Json(sv);
        }
        case element_type::ARRAY: {
            Json arr = Json::array();
            for (auto child : simdjson::dom::array(elem)) {
                arr.push_back(elementToJson(child));
            }
            return arr;
        }
        case element_type::OBJECT: {
            Json obj = Json::object();
            for (auto kv : simdjson::dom::object(elem)) {
                obj[kv.key] = elementToJson(kv.value);
            }
            return obj;
        }
        default:
            return Json{};
    }
}

// JSON 字符串转义写入 (紧凑/格式化共用)
void appendEscaped(std::string& out, std::string_view s) {
    out.push_back('"');
    // 批量拷贝 + 逐个转义 (控制字符/引号/反斜杠)
    size_t runStart = 0;
    auto   flushRun = [&](size_t end) {
        if (end > runStart) {
            out.append(s.data() + runStart, end - runStart);
        }
    };
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char                shortEsc = '\0';
        switch (c) {
            case '"':
                shortEsc = '"';
                break;
            case '\\':
                shortEsc = '\\';
                break;
            case '\b':
                shortEsc = 'b';
                break;
            case '\f':
                shortEsc = 'f';
                break;
            case '\n':
                shortEsc = 'n';
                break;
            case '\r':
                shortEsc = 'r';
                break;
            case '\t':
                shortEsc = 't';
                break;
            default:
                break;
        }
        if (shortEsc != '\0') {
            flushRun(i);
            out.push_back('\\');
            out.push_back(shortEsc);
            runStart = i + 1;
        } else if (c < 0x20) {
            flushRun(i);
            out.append("\\u00", 4);
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
            runStart = i + 1;
        }
    }
    flushRun(s.size());
    out.push_back('"');
}

// double 最短往返表示 (与 yyjson/neograph 的 dump 口径一致: 1.0 -> "1.0")
void appendDouble(std::string& out, double v) {
    if (!std::isfinite(v)) {
        // JSON 无 NaN/Inf 字面量: 降级为 null (与 yyjson 写行为一致)
        out += "null";
        return;
    }
    char buf[32];
    // %.17g 保证往返精度, 再裁掉多余尾零 (保留至少一位小数点后数字)
    int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s(buf, static_cast<size_t>(n > 0 ? n : 0));
    // 纯整数写法 (如 "3") 补 ".0", 与 neograph dump 的浮点形态一致
    const bool hasDotOrExp = (s.find('.') != std::string::npos) || (s.find('e') != std::string::npos)
                             || (s.find('E') != std::string::npos);
    if (!hasDotOrExp) {
        s += ".0";
    }
    out += s;
}

void dumpInto(std::string& out, const Json& j, int indent, int depth) {
    using T = Json::Type;
    switch (j.type()) {
        case T::Null:
            out += "null";
            return;
        case T::Boolean:
            out += j.get<bool>() ? "true" : "false";
            return;
        case T::NumberInt:
            out += std::to_string(j.get<int64_t>());
            return;
        case T::NumberUint:
            out += std::to_string(j.get<uint64_t>());
            return;
        case T::NumberFloat:
            appendDouble(out, j.get<double>());
            return;
        case T::String: {
            std::string s = j.get<std::string>();
            appendEscaped(out, std::string_view{s.data(), s.size()});
            return;
        }
        case T::Array: {
            if (j.empty()) {
                out += "[]";
                return;
            }
            if (indent < 0) {
                out.push_back('[');
                for (size_t i = 0; i < j.size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    dumpInto(out, j[i], indent, depth + 1);
                }
                out.push_back(']');
                return;
            }
            out.push_back('[');
            out.push_back('\n');
            for (size_t i = 0; i < j.size(); ++i) {
                out.append(static_cast<size_t>(depth + 1) * static_cast<size_t>(indent), ' ');
                dumpInto(out, j[i], indent, depth + 1);
                if (i + 1 < j.size()) {
                    out.push_back(',');
                }
                out.push_back('\n');
            }
            out.append(static_cast<size_t>(depth) * static_cast<size_t>(indent), ' ');
            out.push_back(']');
            return;
        }
        case T::Object: {
            if (j.empty()) {
                out += "{}";
                return;
            }
            if (indent < 0) {
                out.push_back('{');
                bool first = true;
                for (const auto& [k, v] : j.items()) {
                    if (!first) {
                        out.push_back(',');
                    }
                    first = false;
                    appendEscaped(out, k);
                    out.push_back(':');
                    dumpInto(out, v, indent, depth + 1);
                }
                out.push_back('}');
                return;
            }
            out.push_back('{');
            out.push_back('\n');
            size_t idx = 0;
            for (const auto& [k, v] : j.items()) {
                out.append(static_cast<size_t>(depth + 1) * static_cast<size_t>(indent), ' ');
                appendEscaped(out, k);
                out.append(": ", 2);
                dumpInto(out, v, indent, depth + 1);
                if (++idx < j.size()) {
                    out.push_back(',');
                }
                out.push_back('\n');
            }
            out.append(static_cast<size_t>(depth) * static_cast<size_t>(indent), ' ');
            out.push_back('}');
            return;
        }
    }
    out += "null";
}

// string_view 键比较 (保序对象线性查找用)
bool keyEquals(const std::string& k, std::string_view key) noexcept {
    return k.size() == key.size()
           && (k.empty() || std::string_view{k.data(), k.size()} == key);
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

Json::Json() noexcept :
    type_(Type::Null) {}

Json::Json(std::nullptr_t) noexcept :
    type_(Type::Null) {}

Json::Json(bool b) noexcept :
    type_(Type::Boolean) {
    storage_.bool_val_ = b;
}

Json::Json(int i) noexcept :
    Json(static_cast<long long>(i)) {}

Json::Json(unsigned int u) noexcept :
    Json(static_cast<unsigned long long>(u)) {}

Json::Json(long l) noexcept :
    Json(static_cast<long long>(l)) {}

Json::Json(unsigned long ul) noexcept :
    Json(static_cast<unsigned long long>(ul)) {}

Json::Json(long long ll) noexcept :
    type_(Type::NumberInt) {
    storage_.int_val_ = ll;
}

Json::Json(unsigned long long ull) noexcept :
    type_(Type::NumberUint) {
    storage_.uint_val_ = ull;
}

Json::Json(double d) noexcept :
    type_(Type::NumberFloat) {
    storage_.float_val_ = d;
}

Json::Json(float f) noexcept :
    Json(static_cast<double>(f)) {}

Json::Json(const char* s) :
    Json(s != nullptr ? std::string_view{s} : std::string_view{}) {}

Json::Json(std::string_view sv) :
    type_(Type::String) {
    new (&storage_.str_val_) std::string(sv.data(), sv.size());
}

Json::Json(const std::string& s) :
    type_(Type::String) {
    new (&storage_.str_val_) std::string(s);
}

Json::Json(std::string&& s) noexcept :
    type_(Type::String) {
    new (&storage_.str_val_) std::string(std::move(s));
}

Json::Json(const std::vector<std::string>& vec) :
    type_(Type::Array) {
    new (&storage_.arr_val_) array_t();
    storage_.arr_val_.reserve(vec.size());
    for (const auto& s : vec) {
        storage_.arr_val_.emplace_back(s);
    }
}

Json::Json(std::initializer_list<Json> il) :
    type_(Type::Null) {
    // nlohmann 启发式: 全部为 [string, X] 二元数组时视为对象, 否则为数组
    // (空列表视为数组, 与 agentxx::util::Json 一致)
    bool looksLikeObject = il.size() > 0;
    for (const auto& el : il) {
        if (!el.is_array() || el.size() != 2 || !el[static_cast<size_t>(0)].is_string()) {
            looksLikeObject = false;
            break;
        }
    }
    if (looksLikeObject) {
        type_ = Type::Object;
        new (&storage_.obj_val_) object_t();
        storage_.obj_val_.reserve(il.size());
        for (const auto& el : il) {
            storage_.obj_val_.emplace_back(
                el[static_cast<size_t>(0)].get<std::string>(),
                el[static_cast<size_t>(1)]
            );
        }
    } else {
        type_ = Type::Array;
        new (&storage_.arr_val_) array_t(il);
    }
}

Json::Json(const Json& other) :
    type_(Type::Null) {
    copyFrom(other);
}

Json::Json(Json&& other) noexcept :
    type_(Type::Null) {
    moveFrom(std::move(other));
}

Json& Json::operator=(const Json& other) {
    if (this != &other) {
        destroy();
        copyFrom(other);
    }
    return *this;
}

Json& Json::operator=(Json&& other) noexcept {
    if (this != &other) {
        destroy();
        moveFrom(std::move(other));
    }
    return *this;
}

Json& Json::operator=(bool b) noexcept {
    destroy();
    type_              = Type::Boolean;
    storage_.bool_val_ = b;
    return *this;
}

Json& Json::operator=(int i) noexcept {
    return (*this) = static_cast<long long>(i);
}

Json& Json::operator=(unsigned int u) noexcept {
    return (*this) = static_cast<unsigned long long>(u);
}

Json& Json::operator=(long l) noexcept {
    return (*this) = static_cast<long long>(l);
}

Json& Json::operator=(unsigned long ul) noexcept {
    return (*this) = static_cast<unsigned long long>(ul);
}

Json& Json::operator=(long long ll) noexcept {
    destroy();
    type_             = Type::NumberInt;
    storage_.int_val_ = ll;
    return *this;
}

Json& Json::operator=(unsigned long long ull) noexcept {
    destroy();
    type_              = Type::NumberUint;
    storage_.uint_val_ = ull;
    return *this;
}

Json& Json::operator=(double d) noexcept {
    destroy();
    type_               = Type::NumberFloat;
    storage_.float_val_ = d;
    return *this;
}

Json& Json::operator=(float f) noexcept {
    return (*this) = static_cast<double>(f);
}

Json& Json::operator=(const char* s) {
    return (*this) = (s != nullptr ? std::string_view{s} : std::string_view{});
}

Json& Json::operator=(std::string_view sv) {
    destroy();
    type_ = Type::String;
    new (&storage_.str_val_) std::string(sv.data(), sv.size());
    return *this;
}

Json& Json::operator=(const std::string& s) {
    destroy();
    type_ = Type::String;
    new (&storage_.str_val_) std::string(s);
    return *this;
}

Json& Json::operator=(const std::vector<std::string>& vec) {
    destroy();
    type_ = Type::Array;
    new (&storage_.arr_val_) array_t();
    storage_.arr_val_.reserve(vec.size());
    for (const auto& s : vec) {
        storage_.arr_val_.emplace_back(s);
    }
    return *this;
}

Json& Json::operator=(std::initializer_list<Json> il) {
    return (*this) = Json(il);
}

Json::~Json() {
    destroy();
}

void Json::destroy() noexcept {
    switch (type_) {
        case Type::String:
            storage_.str_val_.~basic_string();
            break;
        case Type::Array:
            storage_.arr_val_.~vector();
            break;
        case Type::Object:
            storage_.obj_val_.~vector();
            break;
        default:
            break;
    }
    type_ = Type::Null;
}

void Json::copyFrom(const Json& other) {
    type_ = other.type_;
    switch (other.type_) {
        case Type::Boolean:
            storage_.bool_val_ = other.storage_.bool_val_;
            break;
        case Type::NumberInt:
            storage_.int_val_ = other.storage_.int_val_;
            break;
        case Type::NumberUint:
            storage_.uint_val_ = other.storage_.uint_val_;
            break;
        case Type::NumberFloat:
            storage_.float_val_ = other.storage_.float_val_;
            break;
        case Type::String:
            new (&storage_.str_val_) std::string(other.storage_.str_val_);
            break;
        case Type::Array:
            new (&storage_.arr_val_) array_t(other.storage_.arr_val_);
            break;
        case Type::Object:
            new (&storage_.obj_val_) object_t(other.storage_.obj_val_);
            break;
        case Type::Null:
            break;
    }
}

void Json::moveFrom(Json&& other) noexcept {
    type_ = other.type_;
    switch (other.type_) {
        case Type::Boolean:
            storage_.bool_val_ = other.storage_.bool_val_;
            break;
        case Type::NumberInt:
            storage_.int_val_ = other.storage_.int_val_;
            break;
        case Type::NumberUint:
            storage_.uint_val_ = other.storage_.uint_val_;
            break;
        case Type::NumberFloat:
            storage_.float_val_ = other.storage_.float_val_;
            break;
        case Type::String:
            new (&storage_.str_val_) std::string(std::move(other.storage_.str_val_));
            other.storage_.str_val_.~basic_string();
            break;
        case Type::Array:
            new (&storage_.arr_val_) array_t(std::move(other.storage_.arr_val_));
            other.storage_.arr_val_.~vector();
            break;
        case Type::Object:
            new (&storage_.obj_val_) object_t(std::move(other.storage_.obj_val_));
            other.storage_.obj_val_.~vector();
            break;
        case Type::Null:
            break;
    }
    other.type_ = Type::Null;
}

Json& Json::oobNull() noexcept {
    thread_local Json oob;
    // 重置为 Null (clear() 会保留原类型, 此处必须回到 Null 语义,
    // 与 agentxx::util::Json 的游离 null 句柄一致)
    oob = Json{};
    return oob;
}

const Json& Json::constNull() noexcept {
    static const Json kNull;
    return kNull;
}

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    new (&j.storage_.obj_val_) object_t();
    return j;
}

Json Json::object(std::initializer_list<std::pair<std::string_view, Json>> il) {
    Json j = Json::object();
    j.storage_.obj_val_.reserve(il.size());
    for (const auto& [k, v] : il) {
        j.storage_.obj_val_.emplace_back(std::string(k.data(), k.size()), v);
    }
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    new (&j.storage_.arr_val_) array_t();
    return j;
}

Json Json::array(std::initializer_list<Json> il) {
    Json j;
    j.type_ = Type::Array;
    new (&j.storage_.arr_val_) array_t(il);
    return j;
}

Json Json::parse(std::string_view sv) {
    // simdjson 要求输入尾部 SIMDJSON_PADDING 字节可读: padded_string 拷贝保证安全
    simdjson::padded_string padded(sv.data(), sv.size());
    simdjson::dom::parser   parser;
    simdjson::dom::element  elem;
    auto                    err = parser.parse(padded).get(elem);
    if (err != simdjson::SUCCESS) {
        std::string msg = "Json::parse: ";
        msg += simdjson::error_message(err);
        msg += " (input ";
        msg += std::to_string(sv.size());
        msg += " bytes)";
        throw parse_error(msg);
    }
    return elementToJson(elem);
}

Json Json::parse(std::istream& is) {
    std::ostringstream ss;
    ss << is.rdbuf();
    return parse(ss.str());
}

// ---------------------------------------------------------------------------
// 类型检测 / 容器
// ---------------------------------------------------------------------------

bool Json::is_null() const noexcept {
    return type_ == Type::Null;
}

bool Json::is_bool() const noexcept {
    return type_ == Type::Boolean;
}

bool Json::is_number() const noexcept {
    return type_ == Type::NumberInt || type_ == Type::NumberUint || type_ == Type::NumberFloat;
}

bool Json::is_number_integer() const noexcept {
    return type_ == Type::NumberInt || type_ == Type::NumberUint;
}

bool Json::is_number_unsigned() const noexcept {
    return type_ == Type::NumberUint;
}

bool Json::is_number_float() const noexcept {
    return type_ == Type::NumberFloat;
}

bool Json::is_string() const noexcept {
    return type_ == Type::String;
}

bool Json::is_array() const noexcept {
    return type_ == Type::Array;
}

bool Json::is_object() const noexcept {
    return type_ == Type::Object;
}

bool Json::is_primitive() const noexcept {
    return !is_null() && !is_array() && !is_object();
}

size_t Json::size() const noexcept {
    switch (type_) {
        case Type::Array:
            return storage_.arr_val_.size();
        case Type::Object:
            return storage_.obj_val_.size();
        case Type::String:
            return storage_.str_val_.size();
        default:
            return 0;
    }
}

bool Json::empty() const noexcept {
    return size() == 0;
}

void Json::clear() {
    switch (type_) {
        case Type::Null:
            return;
        case Type::Boolean:
            storage_.bool_val_ = false;
            return;
        case Type::NumberInt:
            storage_.int_val_ = 0;
            return;
        case Type::NumberUint:
            storage_.uint_val_ = 0;
            return;
        case Type::NumberFloat:
            storage_.float_val_ = 0.0;
            return;
        case Type::String:
            storage_.str_val_.clear();
            return;
        case Type::Array:
            storage_.arr_val_.clear();
            return;
        case Type::Object:
            storage_.obj_val_.clear();
            return;
    }
}

void Json::push_back(const Json& val) {
    if (type_ == Type::Null) {
        destroy();
        type_ = Type::Array;
        new (&storage_.arr_val_) array_t();
    }
    if (type_ != Type::Array) {
        throw type_error("Json::push_back: not an array");
    }
    storage_.arr_val_.push_back(val);
}

void Json::push_back(Json&& val) {
    if (type_ == Type::Null) {
        destroy();
        type_ = Type::Array;
        new (&storage_.arr_val_) array_t();
    }
    if (type_ != Type::Array) {
        throw type_error("Json::push_back: not an array");
    }
    storage_.arr_val_.push_back(std::move(val));
}

bool Json::erase(std::string_view key) noexcept {
    if (type_ != Type::Object) {
        return false;
    }
    auto& obj = storage_.obj_val_;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (keyEquals(it->first, key)) {
            obj.erase(it);
            return true;
        }
    }
    return false;
}

bool Json::erase(size_t index) noexcept {
    if (type_ != Type::Array) {
        return false;
    }
    auto& arr = storage_.arr_val_;
    if (index >= arr.size()) {
        return false;
    }
    arr.erase(arr.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// ---------------------------------------------------------------------------
// 访问
// ---------------------------------------------------------------------------

const Json* Json::findChild(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& [k, v] : storage_.obj_val_) {
        if (keyEquals(k, key)) {
            return &v;
        }
    }
    return nullptr;
}

Json* Json::findChild(std::string_view key) noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (auto& [k, v] : storage_.obj_val_) {
        if (keyEquals(k, key)) {
            return &v;
        }
    }
    return nullptr;
}

Json* Json::findOrInsert(std::string_view key) {
    if (type_ == Type::Null) {
        destroy();
        type_ = Type::Object;
        new (&storage_.obj_val_) object_t();
    }
    if (type_ != Type::Object) {
        return nullptr;
    }
    if (Json* hit = findChild(key)) {
        return hit;
    }
    storage_.obj_val_.emplace_back(std::string(key.data(), key.size()), Json{});
    return &storage_.obj_val_.back().second;
}

Json& Json::operator[](std::string_view key) {
    if (Json* hit = findOrInsert(key)) {
        return *hit;
    }
    return oobNull();
}

const Json& Json::operator[](std::string_view key) const noexcept {
    if (const Json* hit = findChild(key)) {
        return *hit;
    }
    return constNull();
}

Json& Json::operator[](size_t index) {
    if (type_ != Type::Array || index >= storage_.arr_val_.size()) {
        return oobNull();
    }
    return storage_.arr_val_[index];
}

const Json& Json::operator[](size_t index) const noexcept {
    if (type_ != Type::Array || index >= storage_.arr_val_.size()) {
        return constNull();
    }
    return storage_.arr_val_[index];
}

Json& Json::at(std::string_view key) {
    if (type_ != Type::Object) {
        throw type_error("Json::at: not an object");
    }
    if (Json* hit = findChild(key)) {
        return *hit;
    }
    throw out_of_range(std::string("Json::at: key not found: ") + std::string(key));
}

const Json& Json::at(std::string_view key) const {
    if (type_ != Type::Object) {
        throw type_error("Json::at: not an object");
    }
    if (const Json* hit = findChild(key)) {
        return *hit;
    }
    throw out_of_range(std::string("Json::at: key not found: ") + std::string(key));
}

Json& Json::at(size_t index) {
    if (type_ != Type::Array) {
        throw type_error("Json::at: not an array");
    }
    if (index >= storage_.arr_val_.size()) {
        throw out_of_range("Json::at: index out of range");
    }
    return storage_.arr_val_[index];
}

const Json& Json::at(size_t index) const {
    if (type_ != Type::Array) {
        throw type_error("Json::at: not an array");
    }
    if (index >= storage_.arr_val_.size()) {
        throw out_of_range("Json::at: index out of range");
    }
    return storage_.arr_val_[index];
}

Json& Json::front() {
    return at(static_cast<size_t>(0));
}

Json& Json::back() {
    if (type_ != Type::Array) {
        throw type_error("Json::back: not an array");
    }
    if (storage_.arr_val_.empty()) {
        throw out_of_range("Json::back: empty array");
    }
    return storage_.arr_val_.back();
}

const Json& Json::front() const {
    return at(static_cast<size_t>(0));
}

const Json& Json::back() const {
    if (type_ != Type::Array) {
        throw type_error("Json::back: not an array");
    }
    if (storage_.arr_val_.empty()) {
        throw out_of_range("Json::back: empty array");
    }
    return storage_.arr_val_.back();
}

bool Json::contains(std::string_view key) const noexcept {
    return findChild(key) != nullptr;
}

std::string_view Json::get_string_view() const {
    if (type_ != Type::String) {
        throw type_error("Json::get_string_view: not a string");
    }
    return std::string_view{storage_.str_val_.data(), storage_.str_val_.size()};
}

std::string Json::value(std::string_view key, const char* default_val) const {
    return value<std::string>(
        key,
        default_val != nullptr ? std::string{default_val} : std::string{}
    );
}

// ---------------------------------------------------------------------------
// get<T>
// ---------------------------------------------------------------------------

template<>
Json Json::get<Json>() const {
    return *this;
}

template<>
std::string Json::get<std::string>() const {
    if (type_ != Type::String) {
        throw type_error("Json::get<string>: not a string");
    }
    return storage_.str_val_;
}

template<>
bool Json::get<bool>() const {
    if (type_ != Type::Boolean) {
        throw type_error("Json::get<bool>: not a bool");
    }
    return storage_.bool_val_;
}

template<>
int Json::get<int>() const {
    return static_cast<int>(get<long long>());
}

template<>
unsigned int Json::get<unsigned int>() const {
    return static_cast<unsigned int>(get<unsigned long long>());
}

template<>
long Json::get<long>() const {
    return static_cast<long>(get<long long>());
}

template<>
unsigned long Json::get<unsigned long>() const {
    return static_cast<unsigned long>(get<unsigned long long>());
}

template<>
long long Json::get<long long>() const {
    switch (type_) {
        case Type::NumberInt:
            return storage_.int_val_;
        case Type::NumberUint:
            return static_cast<long long>(storage_.uint_val_);
        case Type::NumberFloat:
            return static_cast<long long>(storage_.float_val_);
        default:
            throw type_error("Json::get<long long>: not a number");
    }
}

template<>
unsigned long long Json::get<unsigned long long>() const {
    switch (type_) {
        case Type::NumberUint:
            return storage_.uint_val_;
        case Type::NumberInt:
            return static_cast<unsigned long long>(storage_.int_val_);
        case Type::NumberFloat:
            return static_cast<unsigned long long>(storage_.float_val_);
        default:
            throw type_error("Json::get<unsigned long long>: not a number");
    }
}

template<>
double Json::get<double>() const {
    switch (type_) {
        case Type::NumberFloat:
            return storage_.float_val_;
        case Type::NumberInt:
            return static_cast<double>(storage_.int_val_);
        case Type::NumberUint:
            return static_cast<double>(storage_.uint_val_);
        default:
            throw type_error("Json::get<double>: not a number");
    }
}

template<>
float Json::get<float>() const {
    return static_cast<float>(get<double>());
}

template<>
std::string_view Json::get<std::string_view>() const {
    return get_string_view();
}

template<>
std::vector<std::string> Json::get<std::vector<std::string>>() const {
    if (type_ != Type::Array) {
        throw type_error("Json::get<vector<string>>: not an array");
    }
    std::vector<std::string> out;
    out.reserve(storage_.arr_val_.size());
    for (const auto& item : storage_.arr_val_) {
        if (item.is_string()) {
            out.push_back(item.storage_.str_val_);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 迭代器
// ---------------------------------------------------------------------------

Json::iterator Json::begin() noexcept {
    iterator it;
    it.obj_ = this;
    it.idx_ = 0;
    return it;
}

Json::iterator Json::end() noexcept {
    iterator it;
    it.obj_ = this;
    if (type_ == Type::Array) {
        it.idx_ = storage_.arr_val_.size();
    } else if (type_ == Type::Object) {
        it.idx_ = storage_.obj_val_.size();
    } else {
        it.idx_ = 0;
    }
    return it;
}

Json::const_iterator Json::begin() const noexcept {
    const_iterator it;
    it.obj_ = this;
    it.idx_ = 0;
    return it;
}

Json::const_iterator Json::end() const noexcept {
    const_iterator it;
    it.obj_ = this;
    if (type_ == Type::Array) {
        it.idx_ = storage_.arr_val_.size();
    } else if (type_ == Type::Object) {
        it.idx_ = storage_.obj_val_.size();
    } else {
        it.idx_ = 0;
    }
    return it;
}

Json::iterator Json::find(std::string_view key) noexcept {
    if (type_ != Type::Object) {
        return end();
    }
    iterator it;
    it.obj_ = this;
    it.idx_ = 0;
    for (size_t i = 0; i < storage_.obj_val_.size(); ++i) {
        if (keyEquals(storage_.obj_val_[i].first, key)) {
            it.idx_ = i;
            return it;
        }
    }
    return end();
}

Json::const_iterator Json::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return end();
    }
    const_iterator it;
    it.obj_ = this;
    it.idx_ = 0;
    for (size_t i = 0; i < storage_.obj_val_.size(); ++i) {
        if (keyEquals(storage_.obj_val_[i].first, key)) {
            it.idx_ = i;
            return it;
        }
    }
    return end();
}

Json::iterator::reference Json::iterator::operator*() const noexcept {
    assert(obj_ != nullptr);
    if (obj_->type_ == Type::Array) {
        return obj_->storage_.arr_val_[idx_];
    }
    return obj_->storage_.obj_val_[idx_].second;
}

Json::iterator::pointer Json::iterator::operator->() const noexcept {
    return &(**this);
}

Json::iterator& Json::iterator::operator++() noexcept {
    ++idx_;
    return *this;
}

bool Json::iterator::operator==(const iterator& o) const noexcept {
    if (obj_ != o.obj_) {
        return false;
    }
    if (obj_ == nullptr) {
        return true;
    }
    size_t n = 0;
    if (obj_->type_ == Type::Array) {
        n = obj_->storage_.arr_val_.size();
    } else if (obj_->type_ == Type::Object) {
        n = obj_->storage_.obj_val_.size();
    }
    // 双端 end 语义: 两个越界迭代器即使 idx 不同也相等
    // (与 agentxx::util::Json::iterator 的 done_ 语义一致)
    const bool aDone = idx_ >= n;
    const bool bDone = o.idx_ >= n;
    if (aDone && bDone) {
        return true;
    }
    return idx_ == o.idx_;
}

std::string Json::iterator::key() const {
    if (obj_ == nullptr || obj_->type_ != Type::Object) {
        return {};
    }
    if (idx_ >= obj_->storage_.obj_val_.size()) {
        return {};
    }
    return obj_->storage_.obj_val_[idx_].first;
}

Json::iterator::reference Json::iterator::value() const noexcept {
    return **this;
}

Json::const_iterator::const_iterator(const iterator& it) noexcept {
    obj_ = it.obj_;
    idx_ = it.idx_;
}

Json::const_iterator::reference Json::const_iterator::operator*() const noexcept {
    assert(obj_ != nullptr);
    if (obj_->type_ == Type::Array) {
        return obj_->storage_.arr_val_[idx_];
    }
    return obj_->storage_.obj_val_[idx_].second;
}

Json::const_iterator::pointer Json::const_iterator::operator->() const noexcept {
    return &(**this);
}

Json::const_iterator& Json::const_iterator::operator++() noexcept {
    ++idx_;
    return *this;
}

bool Json::const_iterator::operator==(const const_iterator& o) const noexcept {
    if (obj_ != o.obj_) {
        return false;
    }
    if (obj_ == nullptr) {
        return true;
    }
    size_t n = 0;
    if (obj_->type_ == Type::Array) {
        n = obj_->storage_.arr_val_.size();
    } else if (obj_->type_ == Type::Object) {
        n = obj_->storage_.obj_val_.size();
    }
    const bool aDone = idx_ >= n;
    const bool bDone = o.idx_ >= n;
    if (aDone && bDone) {
        return true;
    }
    return idx_ == o.idx_;
}

std::string Json::const_iterator::key() const {
    if (obj_ == nullptr || obj_->type_ != Type::Object) {
        return {};
    }
    if (idx_ >= obj_->storage_.obj_val_.size()) {
        return {};
    }
    return obj_->storage_.obj_val_[idx_].first;
}

Json::const_iterator::reference Json::const_iterator::value() const noexcept {
    return **this;
}

// ---------------------------------------------------------------------------
// items()
// ---------------------------------------------------------------------------

Json::ItemsIterable Json::items() {
    return ItemsIterable{*this};
}

Json::ConstItemsIterable Json::items() const {
    return ConstItemsIterable{*this};
}

Json::ItemsIterable::iterator Json::ItemsIterable::begin() noexcept {
    iterator it;
    it.it_ = j_.begin();
    return it;
}

Json::ItemsIterable::iterator Json::ItemsIterable::end() noexcept {
    iterator it;
    it.it_ = j_.end();
    return it;
}

std::pair<std::string_view, Json&> Json::ItemsIterable::iterator::operator*() const noexcept {
    Json& v = *it_;
    // 直接取底层键引用, 保证视图有效性 (key() 返回拷贝, 此处不用)
    std::string_view k;
    if (it_.obj_ != nullptr && it_.obj_->type_ == Json::Type::Object
        && it_.idx_ < it_.obj_->storage_.obj_val_.size()) {
        const std::string& ks = it_.obj_->storage_.obj_val_[it_.idx_].first;
        k = std::string_view{ks.data(), ks.size()};
    }
    return {k, v};
}

Json::ItemsIterable::iterator& Json::ItemsIterable::iterator::operator++() noexcept {
    ++it_;
    return *this;
}

bool Json::ItemsIterable::iterator::operator==(const iterator& o) const noexcept {
    return it_ == o.it_;
}

Json::ConstItemsIterable::iterator Json::ConstItemsIterable::begin() const noexcept {
    iterator it;
    it.it_ = j_.begin();
    return it;
}

Json::ConstItemsIterable::iterator Json::ConstItemsIterable::end() const noexcept {
    iterator it;
    it.it_ = j_.end();
    return it;
}

std::pair<std::string, Json> Json::ConstItemsIterable::iterator::operator*() const {
    return {it_.key(), *it_};
}

Json::ConstItemsIterable::iterator& Json::ConstItemsIterable::iterator::operator++() noexcept {
    ++it_;
    return *this;
}

bool Json::ConstItemsIterable::iterator::operator==(const iterator& o) const noexcept {
    return it_ == o.it_;
}

// ---------------------------------------------------------------------------
// 序列化 / 比较
// ---------------------------------------------------------------------------

std::string Json::dump(int indent) const {
    std::string out;
    // 紧凑 JSON 约为输入 1:1, 预留避免反复 realloc
    out.reserve(size() * 8 + 16);
    dumpInto(out, *this, indent, 0);
    return out;
}

bool Json::operator==(const Json& o) const noexcept {
    if (type_ != o.type_) {
        return false;
    }
    switch (type_) {
        case Type::Null:
            return true;
        case Type::Boolean:
            return storage_.bool_val_ == o.storage_.bool_val_;
        case Type::NumberInt:
            return storage_.int_val_ == o.storage_.int_val_;
        case Type::NumberUint:
            return storage_.uint_val_ == o.storage_.uint_val_;
        case Type::NumberFloat: {
            // NaN != NaN (与 IEEE 语义一致)
            const double a = storage_.float_val_;
            const double b = o.storage_.float_val_;
            return a == b;
        }
        case Type::String:
            return storage_.str_val_ == o.storage_.str_val_;
        case Type::Array:
            return storage_.arr_val_ == o.storage_.arr_val_;
        case Type::Object:
            return storage_.obj_val_ == o.storage_.obj_val_;
    }
    return false;
}

} // namespace util
} // namespace agentxx
