/// 自主 JSON DOM (agentxx::util::Json)
///
/// - 替代散落在全代码库的 `agentxx::util::Json` (业务层/插件层不再直接依赖图引擎的 JSON)
/// - 底层解析由 simdjson 驱动 (SIMD 向量化), 内存模型为保序的紧凑 DOM
/// - Object 采用 `vector<pair<string, Json>>` 保序存储: `dump()` 输出确定性,
///   会话链式哈希 (ChainHash) 指纹稳定
/// - 与 `agentxx::util::Json` 的 nlohmann 兼容子集保持源代码级兼容:
///   `object()/array()/parse()`, `operator[]/at()/value()/get<>/contains()`,
///   `push_back()`, `items()/begin()/end()/find()/front()/back()`, `dump()`
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// C++26 静态反射 (P2996) 特性检测
/// - 当前工具链 (GCC 16) 尚未提供 `<meta>` / `__cpp_static_reflection`,
///   此处仅做前瞻性开关, 不可用时自动降级为 ADL `toJson/fromJson` 回退
#if defined(__cpp_impl_reflection) || defined(__cpp_static_reflection) \
    || (defined(__has_include) && __has_include(<meta>))
#define AGENTXX_HAS_CPP26_REFLECTION 1
#else
#define AGENTXX_HAS_CPP26_REFLECTION 0
#endif

namespace agentxx {
namespace util {

class Json {
public:

    /// - Null: 空值
    /// - Boolean: 布尔
    /// - NumberInt: 有符号 64 位整数
    /// - NumberUint: 无符号 64 位整数 (仅能放入 uint64 的大整数)
    /// - NumberFloat: 双精度浮点
    /// - String/Array/Object: 字符串 / 数组 / 保序对象
    enum class Type : uint8_t {
        Null = 0,
        Boolean,
        NumberInt,
        NumberUint,
        NumberFloat,
        String,
        Array,
        Object,
    };

    using array_t  = std::vector<Json>;
    using object_t = std::vector<std::pair<std::string, Json>>;

    // ----- 异常体系 (与 agentxx::util::Json 同名, 便于迁移) -----
    class exception : public std::runtime_error {
    public:

        using std::runtime_error::runtime_error;
    };
    class parse_error : public exception {
    public:

        using exception::exception;
    };
    class type_error : public exception {
    public:

        using exception::exception;
    };
    class out_of_range : public exception {
    public:

        using exception::exception;
    };

    // ----- 构造 -----
    Json() noexcept;
    Json(std::nullptr_t) noexcept;
    Json(bool b) noexcept;
    Json(int i) noexcept;
    Json(unsigned int u) noexcept;
    Json(long l) noexcept;
    Json(unsigned long ul) noexcept;
    Json(long long ll) noexcept;
    Json(unsigned long long ull) noexcept;
    Json(double d) noexcept;
    Json(float f) noexcept;
    Json(const char* s);
    Json(std::string_view sv);
    Json(const std::string& s);
    Json(std::string&& s) noexcept;
    Json(const std::vector<std::string>& vec);

    /// 智能初始化列表构造 (与 agentxx::util::Json 相同的启发式规则):
    /// - 全部元素都是 `[string, X]` 二元数组时视为 Object, 否则视为 Array
    /// - 空列表视为 Array (与 neograph 一致)
    Json(std::initializer_list<Json> il);

    Json(const Json& other);
    Json(Json&& other) noexcept;
    Json& operator=(const Json& other);
    Json& operator=(Json&& other) noexcept;
    Json& operator=(bool b) noexcept;
    Json& operator=(int i) noexcept;
    Json& operator=(unsigned int u) noexcept;
    Json& operator=(long l) noexcept;
    Json& operator=(unsigned long ul) noexcept;
    Json& operator=(long long ll) noexcept;
    Json& operator=(unsigned long long ull) noexcept;
    Json& operator=(double d) noexcept;
    Json& operator=(float f) noexcept;
    Json& operator=(const char* s);
    Json& operator=(std::string_view sv);
    Json& operator=(const std::string& s);
    Json& operator=(const std::vector<std::string>& vec);
    /// 初始化列表赋值 (与构造相同的对象/数组启发式, 解决 braced-list 的重载歧义)
    Json& operator=(std::initializer_list<Json> il);
    ~Json();

    // ----- 工厂 -----
    static Json object();
    static Json object(std::initializer_list<std::pair<std::string_view, Json>> il);
    static Json array();
    static Json array(std::initializer_list<Json> il);

    /// simdjson 驱动的解析 (UTF-8 校验 + 语法解析, 失败抛 parse_error)
    static Json parse(std::string_view sv);
    static Json parse(std::istream& is);

    // ----- 类型检测 -----
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_boolean() const noexcept {
        return is_bool();
    }
    bool is_number() const noexcept;
    bool is_number_integer() const noexcept;
    bool is_number_unsigned() const noexcept;
    bool is_number_float() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;
    /// 非 null 且非容器 (与 agentxx::util::Json 一致)
    bool is_primitive() const noexcept;
    Type type() const noexcept {
        return type_;
    }

    /// 高效的字符串提取 (无拷贝, 引用内部存储; 修改 Json 将使视图失效)
    std::string_view get_string_view() const;

    // ----- 容器查询与修改 -----
    /// - 数组/对象返回元素数, 字符串返回字节数, 其余返回 0 (与 neograph 一致)
    size_t size() const noexcept;
    bool   empty() const noexcept;
    /// nlohmann 语义: null 不变, bool=false, 数字=0, 字符串="", 数组=[], 对象={}
    void clear();
    void push_back(const Json& val);
    void push_back(Json&& val);
    /// 删除对象键, 返回是否命中
    bool erase(std::string_view key) noexcept;
    /// 删除数组下标, 越界返回 false
    bool erase(size_t index) noexcept;

    // ----- 键访问 (原生 string_view, 无临时 string 构造) -----
    /// 可写访问: 缺失键自动插入 Null (null 节点先提升为对象)
    /// - 类型不匹配 (非对象) 时返回线程局部的空节点引用 (写入丢失,
    ///   与 agentxx::util::Json 的游离 null 句柄语义一致, 仅读场景安全)
    Json&       operator[](std::string_view key);
    /// 只读访问: 缺失时返回全局 Null 常量
    const Json& operator[](std::string_view key) const noexcept;
    Json&       operator[](size_t index);
    const Json& operator[](size_t index) const noexcept;
    Json&       operator[](int index) {
        return (*this)[static_cast<size_t>(index)];
    }
    const Json& operator[](int index) const noexcept {
        return (*this)[static_cast<size_t>(index)];
    }

    /// 强边界检查: 缺失抛 out_of_range, 类型不匹配抛 type_error
    Json&       at(std::string_view key);
    const Json& at(std::string_view key) const;
    Json&       at(size_t index);
    const Json& at(size_t index) const;
    Json&       at(int index) {
        return at(static_cast<size_t>(index));
    }
    const Json& at(int index) const {
        return at(static_cast<size_t>(index));
    }

    /// 数组首尾端点 (非数组抛 type_error, 空数组抛 out_of_range)
    Json&       front();
    Json&       back();
    const Json& front() const;
    const Json& back() const;

    bool contains(std::string_view key) const noexcept;

    // ----- 类型安全提取 -----
    template<typename T>
    T get() const;

    /// 对象作用域的安全提取: 缺失/类型不匹配时返回默认值 (不抛异常)
    template<typename T>
    T value(std::string_view key, const T& default_val) const {
        if (!is_object()) {
            return default_val;
        }
        const Json* child = findChild(key);
        if (child == nullptr || child->is_null()) {
            return default_val;
        }
        try {
            return child->get<T>();
        } catch (...) {
            return default_val;
        }
    }
    /// 字符串字面量默认值重载 (避免模板把 `const char*` 推导为数组类型)
    std::string value(std::string_view key, const char* default_val) const;

    // ----- 迭代 -----
    class iterator;
    class const_iterator;

    iterator       begin() noexcept;
    iterator       end() noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;

    /// 对象键查找 (数组上查找恒返回 end, 与 neograph 一致)
    iterator       find(std::string_view key) noexcept;
    const_iterator find(std::string_view key) const noexcept;

    // ----- items() 结构化绑定迭代 -----
    class ItemsIterable;
    class ConstItemsIterable;
    ItemsIterable      items();
    ConstItemsIterable items() const;

    // ----- 序列化 -----
    /// - indent < 0: 紧凑模式 (默认)
    /// - indent >= 0: 格式化模式 (每级缩进 indent 个空格)
    std::string dump(int indent = -1) const;

    // ----- 比较 -----
    /// 结构化相等 (类型 + 值 + 对象键序均一致; 1 与 1.0 不相等)
    bool operator==(const Json& o) const noexcept;
    bool operator!=(const Json& o) const noexcept {
        return !(*this == o);
    }

    friend std::ostream& operator<<(std::ostream& os, const Json& j);

private:

    const Json* findChild(std::string_view key) const noexcept;
    Json*       findChild(std::string_view key) noexcept;
    Json*       findOrInsert(std::string_view key);

    void destroy() noexcept;
    void copyFrom(const Json& other);
    void moveFrom(Json&& other) noexcept;

    /// 越界/类型不匹配的可写访问兜底 (线程局部, 写入丢失, 仅读安全)
    static Json& oobNull() noexcept;
    /// 只读访问缺失时的全局 Null 常量
    static const Json& constNull() noexcept;

    Type type_ = Type::Null;
    union Storage {
        bool        bool_val_;
        int64_t     int_val_;
        uint64_t    uint_val_;
        double      float_val_;
        std::string str_val_;
        array_t     arr_val_;
        object_t    obj_val_;

        Storage() noexcept {}
        ~Storage() noexcept {}
    } storage_;
};

// ----- get<T> 显式特化声明 -----
template<>
Json Json::get<Json>() const;
template<>
std::string Json::get<std::string>() const;
template<>
bool Json::get<bool>() const;
template<>
int Json::get<int>() const;
template<>
unsigned int Json::get<unsigned int>() const;
template<>
long Json::get<long>() const;
template<>
unsigned long Json::get<unsigned long>() const;
template<>
long long Json::get<long long>() const;
template<>
unsigned long long Json::get<unsigned long long>() const;
template<>
double Json::get<double>() const;
template<>
float Json::get<float>() const;
template<>
std::vector<std::string> Json::get<std::vector<std::string>>() const;
template<>
std::string_view Json::get<std::string_view>() const;

// ----- 迭代器 -----
class Json::iterator {
public:

    using iterator_category = std::forward_iterator_tag;
    using value_type        = Json;
    using difference_type   = std::ptrdiff_t;
    using pointer           = Json*;
    using reference         = Json&;

    iterator() noexcept = default;

    reference operator*() const noexcept;
    pointer   operator->() const noexcept;
    iterator& operator++() noexcept;
    bool      operator==(const iterator& o) const noexcept;
    bool      operator!=(const iterator& o) const noexcept {
        return !(*this == o);
    }

    /// 对象迭代时的当前键 (数组迭代返回空)
    std::string key() const;
    /// 当前值引用
    reference value() const noexcept;

private:

    friend class Json;
    Json*  obj_ = nullptr;
    size_t idx_ = 0;
};

class Json::const_iterator {
public:

    using iterator_category = std::forward_iterator_tag;
    using value_type        = const Json;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const Json*;
    using reference         = const Json&;

    const_iterator() noexcept = default;
    /// mutable 迭代器可隐式转为 const 迭代器
    const_iterator(const iterator& it) noexcept;

    reference operator*() const noexcept;
    pointer   operator->() const noexcept;
    const_iterator& operator++() noexcept;
    bool            operator==(const const_iterator& o) const noexcept;
    bool            operator!=(const const_iterator& o) const noexcept {
        return !(*this == o);
    }

    std::string key() const;
    reference   value() const noexcept;

private:

    friend class Json;
    const Json* obj_ = nullptr;
    size_t      idx_ = 0;
};

// ----- items() 代理 -----
/// 可写 items: `for (auto [k, v] : j.items())` / `for (const auto& kv : j.items())`
/// - `kv.first` 为键视图 (引用对象键内存, 迭代期间不得增删键)
/// - `kv.second` 为值引用
class Json::ItemsIterable {
public:

    class iterator {
    public:

        iterator() noexcept = default;
        std::pair<std::string_view, Json&> operator*() const noexcept;
        iterator&                          operator++() noexcept;
        bool operator==(const iterator& o) const noexcept;
        bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }

    private:

        friend class ItemsIterable;
        Json::iterator it_;
    };

    iterator begin() noexcept;
    iterator end() noexcept;

private:

    friend class Json;
    explicit ItemsIterable(Json& j) noexcept :
        j_(j) {}
    Json& j_;
};

/// 只读 items: 与 agentxx::util::Json 的 `pair<string, json>` 值语义一致
class Json::ConstItemsIterable {
public:

    class iterator {
    public:

        iterator() noexcept = default;
        std::pair<std::string, Json> operator*() const;
        iterator&                    operator++() noexcept;
        bool operator==(const iterator& o) const noexcept;
        bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }

    private:

        friend class ConstItemsIterable;
        Json::const_iterator it_;
    };

    iterator begin() const noexcept;
    iterator end() const noexcept;

private:

    friend class Json;
    explicit ConstItemsIterable(const Json& j) noexcept :
        j_(j) {}
    const Json& j_;
};

inline std::ostream& operator<<(std::ostream& os, const Json& j) {
    return os << j.dump();
}

/// ADL 兼容辅助 (agentxx::util::Json 的 to_json/from_json 惯用法迁移用)
inline void toJson(Json& j, const Json& v) {
    j = v;
}
inline void fromJson(const Json& j, Json& v) {
    v = j;
}

#if AGENTXX_HAS_CPP26_REFLECTION
// C++26 静态反射可用时的零样板转换声明 (实现见 json.cpp, 编译器不支持时
// 自动回退为下方的 ADL 版本)
template<typename T>
    requires(std::is_class_v<T> && !std::is_same_v<T, Json> && !std::is_same_v<T, std::string>)
Json reflectToJson(const T& obj);
template<typename T>
    requires(std::is_class_v<T> && !std::is_same_v<T, Json>)
T reflectFromJson(const Json& j);
#else
// 未支持反射的编译环境: 回退到普通 ADL toJson/fromJson (由各业务类型自行提供)
template<typename T>
Json reflectToJson(const T& obj) {
    Json j;
    toJson(j, obj);
    return j;
}
template<typename T>
T reflectFromJson(const Json& j) {
    T obj{};
    fromJson(j, obj);
    return obj;
}
#endif

} // namespace util
} // namespace agentxx
