/// agentxx::util::JsonView 实现
#include "agentxx/util/json_view.h"

#include <string>

namespace agentxx {
namespace util {

namespace {

Json jsonFromElement(const simdjson::dom::element& elem) {
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
                arr.push_back(jsonFromElement(child));
            }
            return arr;
        }
        case element_type::OBJECT: {
            Json obj = Json::object();
            for (auto kv : simdjson::dom::object(elem)) {
                obj[kv.key] = jsonFromElement(kv.value);
            }
            return obj;
        }
        default:
            return Json{};
    }
}

} // namespace

JsonView::JsonView(simdjson::dom::element elem) noexcept :
    elem_(elem),
    valid_(true) {}

JsonView JsonView::parse(std::string_view sv) {
    auto storage = std::make_shared<Storage>();
    storage->padded = simdjson::padded_string(sv.data(), sv.size());
    simdjson::dom::element elem;
    auto                   err = storage->parser.parse(storage->padded).get(elem);
    if (err != simdjson::SUCCESS) {
        throw Json::parse_error(
            std::string("JsonView::parse: ") + simdjson::error_message(err)
        );
    }
    JsonView view;
    view.elem_    = elem;
    view.storage_ = std::move(storage);
    view.valid_   = true;
    return view;
}

JsonView JsonView::parse(std::string_view sv, simdjson::dom::parser& parser) {
    // 外部 parser 场景: 输入拷贝进视图自持的 padded 缓冲, element tape
    // 仍在外部 parser 内 —— 调用方须保证 parser 在视图使用期间有效
    auto storage = std::make_shared<Storage>();
    storage->padded = simdjson::padded_string(sv.data(), sv.size());
    simdjson::dom::element elem;
    auto                   err = parser.parse(storage->padded).get(elem);
    if (err != simdjson::SUCCESS) {
        throw Json::parse_error(
            std::string("JsonView::parse: ") + simdjson::error_message(err)
        );
    }
    JsonView view;
    view.elem_    = elem;
    view.storage_ = std::move(storage);
    view.valid_   = true;
    return view;
}

bool JsonView::is_null() const noexcept {
    if (!valid_) {
        return true;
    }
    return elem_.type() == simdjson::dom::element_type::NULL_VALUE;
}

bool JsonView::is_bool() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::BOOL;
}

bool JsonView::is_int64() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::INT64;
}

bool JsonView::is_uint64() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::UINT64;
}

bool JsonView::is_double() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::DOUBLE;
}

bool JsonView::is_number() const noexcept {
    if (!valid_) {
        return false;
    }
    using simdjson::dom::element_type;
    const auto t = elem_.type();
    return t == element_type::INT64 || t == element_type::UINT64 || t == element_type::DOUBLE;
}

bool JsonView::is_string() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::STRING;
}

bool JsonView::is_array() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::ARRAY;
}

bool JsonView::is_object() const noexcept {
    return valid_ && elem_.type() == simdjson::dom::element_type::OBJECT;
}

std::string_view JsonView::get_string_view() const {
    if (!valid_ || !is_string()) {
        throw Json::type_error("JsonView::get_string_view: not a string");
    }
    std::string_view sv;
    if (elem_.get_string().get(sv)) {
        throw Json::type_error("JsonView::get_string_view: not a string");
    }
    return sv;
}

bool JsonView::get_bool() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get_bool: null view");
    }
    bool v = false;
    if (elem_.get_bool().get(v)) {
        throw Json::type_error("JsonView::get_bool: not a bool");
    }
    return v;
}

int64_t JsonView::get_int64() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get_int64: null view");
    }
    int64_t v = 0;
    if (elem_.get_int64().get(v)) {
        throw Json::type_error("JsonView::get_int64: not an int64");
    }
    return v;
}

uint64_t JsonView::get_uint64() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get_uint64: null view");
    }
    uint64_t v = 0;
    if (elem_.get_uint64().get(v)) {
        throw Json::type_error("JsonView::get_uint64: not a uint64");
    }
    return v;
}

double JsonView::get_double() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get_double: null view");
    }
    double v = 0.0;
    if (elem_.get_double().get(v)) {
        throw Json::type_error("JsonView::get_double: not a number");
    }
    return v;
}

JsonView JsonView::operator[](std::string_view key) const noexcept {
    if (!is_object()) {
        return JsonView{};
    }
    auto res = simdjson::dom::object(elem_).at_key(key);
    if (res.error() != simdjson::SUCCESS) {
        return JsonView{};
    }
    JsonView child;
    child.elem_    = res.value();
    child.storage_ = storage_;
    child.valid_   = true;
    return child;
}

JsonView JsonView::operator[](size_t index) const noexcept {
    if (!is_array()) {
        return JsonView{};
    }
    size_t i = 0;
    for (auto childElem : simdjson::dom::array(elem_)) {
        if (i == index) {
            JsonView child;
            child.elem_    = childElem;
            child.storage_ = storage_;
            child.valid_   = true;
            return child;
        }
        ++i;
    }
    return JsonView{};
}

JsonView JsonView::at(std::string_view key) const {
    if (!is_object()) {
        throw Json::type_error("JsonView::at: not an object");
    }
    auto res = simdjson::dom::object(elem_).at_key(key);
    if (res.error() != simdjson::SUCCESS) {
        throw Json::out_of_range(std::string("JsonView::at: key not found: ") + std::string(key));
    }
    JsonView child;
    child.elem_    = res.value();
    child.storage_ = storage_;
    child.valid_   = true;
    return child;
}

JsonView JsonView::at(size_t index) const {
    JsonView child = (*this)[index];
    if (!child.valid()) {
        if (!is_array()) {
            throw Json::type_error("JsonView::at: not an array");
        }
        throw Json::out_of_range("JsonView::at: index out of range");
    }
    return child;
}

bool JsonView::contains(std::string_view key) const noexcept {
    if (!is_object()) {
        return false;
    }
    return simdjson::dom::object(elem_).at_key(key).error() == simdjson::SUCCESS;
}

size_t JsonView::size() const noexcept {
    if (!valid_) {
        return 0;
    }
    if (is_array()) {
        return simdjson::dom::array(elem_).size();
    }
    if (is_object()) {
        return simdjson::dom::object(elem_).size();
    }
    if (is_string()) {
        std::string_view sv;
        if (elem_.get_string().get(sv)) {
            return 0;
        }
        return sv.size();
    }
    return 0;
}

bool JsonView::empty() const noexcept {
    return size() == 0;
}

std::string JsonView::value(std::string_view key, const char* def) const {
    return value<std::string>(key, def != nullptr ? std::string{def} : std::string{});
}

template<>
std::string_view JsonView::get<std::string_view>() const {
    return get_string_view();
}

template<>
std::string JsonView::get<std::string>() const {
    return std::string(get_string_view());
}

template<>
bool JsonView::get<bool>() const {
    return get_bool();
}

template<>
int JsonView::get<int>() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get<int>: null view");
    }
    using simdjson::dom::element_type;
    switch (elem_.type()) {
        case element_type::INT64:
            return static_cast<int>(get_int64());
        case element_type::UINT64:
            return static_cast<int>(get_uint64());
        case element_type::DOUBLE:
            return static_cast<int>(get_double());
        default:
            throw Json::type_error("JsonView::get<int>: not a number");
    }
}

template<>
unsigned int JsonView::get<unsigned int>() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get<unsigned>: null view");
    }
    using simdjson::dom::element_type;
    switch (elem_.type()) {
        case element_type::INT64:
            return static_cast<unsigned int>(get_int64());
        case element_type::UINT64:
            return static_cast<unsigned int>(get_uint64());
        case element_type::DOUBLE:
            return static_cast<unsigned int>(get_double());
        default:
            throw Json::type_error("JsonView::get<unsigned>: not a number");
    }
}

template<>
long JsonView::get<long>() const {
    return static_cast<long>(get<long long>());
}

template<>
unsigned long JsonView::get<unsigned long>() const {
    return static_cast<unsigned long>(get<unsigned long long>());
}

template<>
long long JsonView::get<long long>() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get<long long>: null view");
    }
    using simdjson::dom::element_type;
    switch (elem_.type()) {
        case element_type::INT64:
            return get_int64();
        case element_type::UINT64:
            return static_cast<long long>(get_uint64());
        case element_type::DOUBLE:
            return static_cast<long long>(get_double());
        default:
            throw Json::type_error("JsonView::get<long long>: not a number");
    }
}

template<>
unsigned long long JsonView::get<unsigned long long>() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get<unsigned long long>: null view");
    }
    using simdjson::dom::element_type;
    switch (elem_.type()) {
        case element_type::INT64:
            return static_cast<unsigned long long>(get_int64());
        case element_type::UINT64:
            return get_uint64();
        case element_type::DOUBLE:
            return static_cast<unsigned long long>(get_double());
        default:
            throw Json::type_error("JsonView::get<unsigned long long>: not a number");
    }
}

template<>
double JsonView::get<double>() const {
    if (!valid_) {
        throw Json::type_error("JsonView::get<double>: null view");
    }
    using simdjson::dom::element_type;
    switch (elem_.type()) {
        case element_type::DOUBLE:
            return get_double();
        case element_type::INT64:
            return static_cast<double>(get_int64());
        case element_type::UINT64:
            return static_cast<double>(get_uint64());
        default:
            throw Json::type_error("JsonView::get<double>: not a number");
    }
}

template<>
float JsonView::get<float>() const {
    return static_cast<float>(get<double>());
}

Json JsonView::to_json() const {
    if (!valid_) {
        return Json{};
    }
    return jsonFromElement(elem_);
}

} // namespace util
} // namespace agentxx
