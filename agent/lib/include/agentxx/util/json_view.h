/// 零拷贝只读 JSON 视图 (agentxx::util::JsonView)
///
/// - 高频只读场景 (SSE 流式 chunk、WS 消息路由、日志过滤) 避免构造完整 DOM:
///   字符串切片直接借用 simdjson tape 内存, 全程无 `std::string` 堆分配
/// - 值语义: 内部 `simdjson::dom::element` 仅为 tape 游标 (轻量可拷贝),
///   子视图拷贝 element 值 + 共享 Storage, 无 thread_local, 可跨语句持有
/// - 生命周期: Storage (padded 输入 + parser) 由 shared_ptr 延续;
///   `parse(sv, parser)` 外部 parser 重载要求调用方保证 parser 存活
/// - 需要持久化/修改时经 `to_json()` 按需物化为 `agentxx::util::Json`
#pragma once

#include "agentxx/util/json.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "simdjson.h"

namespace agentxx {
namespace util {

class JsonView {
public:

    JsonView() noexcept = default;
    JsonView(simdjson::dom::element elem) noexcept;

    /// 一次性只读解析 (失败抛 Json::parse_error)
    ///
    /// - 输入被拷贝进内部 padded 缓冲, 返回视图自给自足 (不借用调用方内存)
    /// - 每个视图独占一个 parser (tape 生命周期与视图绑定, 跨 parse 安全)
    /// - 高频批量解析请用下方的 parser 复用重载
    static JsonView parse(std::string_view sv);

    /// 高性能批处理解析 (显式借用外部复用的 parser, 复用其 tape 缓冲区)
    ///
    /// - 输入被拷贝进视图自持的 padded 缓冲, 但 element tape 仍在外部 parser 内:
    ///   调用方须保证 parser 在返回视图使用期间有效且不再次 parse
    static JsonView parse(std::string_view sv, simdjson::dom::parser& parser);

    bool valid() const noexcept {
        return valid_;
    }

    explicit operator bool() const noexcept {
        return valid();
    }

    // ----- 类型探测 -----
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_int64() const noexcept;
    bool is_uint64() const noexcept;
    bool is_double() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    // ----- 零拷贝提取 (失败抛 Json::type_error) -----
    std::string_view get_string_view() const;
    bool             get_bool() const;
    int64_t          get_int64() const;
    uint64_t         get_uint64() const;
    double           get_double() const;

    // ----- 只读导航 (缺失时返回无效视图, 不抛异常; 子视图可跨语句持有) -----
    JsonView operator[](std::string_view key) const noexcept;
    JsonView operator[](size_t index) const noexcept;

    JsonView operator[](int index) const noexcept {
        return (*this)[static_cast<size_t>(index)];
    }

    JsonView at(std::string_view key) const;
    JsonView at(size_t index) const;
    bool     contains(std::string_view key) const noexcept;
    size_t   size() const noexcept;
    bool     empty() const noexcept;

    // ----- 安全提取 (缺失/类型不匹配时返回默认值, 不抛异常) -----
    template<typename T>
    T value(std::string_view key, const T& def) const noexcept {
        try {
            JsonView child = (*this)[key];
            if (!child.valid()) {
                return def;
            }
            return child.get<T>();
        } catch (...) {
            return def;
        }
    }

    std::string value(std::string_view key, const char* def) const;

    template<typename T>
    T get() const;

    // ----- 按需物化为可修改的 Json -----
    Json to_json() const;

private:

    // elem_ 为 simdjson tape 游标 (值语义, 拷贝轻量);
    // tape 内存由 storage_ (自持 parse) 或外部 parser (复用重载) 提供
    struct Storage {
        simdjson::padded_string padded;
        simdjson::dom::parser   parser;
    };

    simdjson::dom::element   elem_{};
    std::shared_ptr<Storage> storage_;
    bool                     valid_ = false;
};

template<>
std::string_view JsonView::get<std::string_view>() const;
template<>
std::string JsonView::get<std::string>() const;
template<>
bool JsonView::get<bool>() const;
template<>
int JsonView::get<int>() const;
template<>
unsigned int JsonView::get<unsigned int>() const;
template<>
long JsonView::get<long>() const;
template<>
unsigned long JsonView::get<unsigned long>() const;
template<>
long long JsonView::get<long long>() const;
template<>
unsigned long long JsonView::get<unsigned long long>() const;
template<>
double JsonView::get<double>() const;
template<>
float JsonView::get<float>() const;

} // namespace util
} // namespace agentxx
