#include "agentxx/util/tool_args_fix.h"

#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

namespace agentxx {
namespace util {

namespace {

/// 解析字符串为数值: 容忍首尾空白与一个前导 '+', 要求整个字符串被完整解析
/// (std::from_chars 不接受空白与前导 '+', 且允许部分解析, 这里做严格校验)
template<typename T>
bool parseFullNumber(std::string_view s, T& out) {
    size_t b = 0, e = s.size();
    while (b < e && utilxx_base::charIsSpace(s[b])) {
        ++b;
    }
    while (e > b && utilxx_base::charIsSpace(s[e - 1])) {
        --e;
    }
    s = s.substr(b, e - b);
    if (s.empty()) {
        return false;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    // 浮点 from_chars (C++23+) 支持 0x 十六进制浮点写法, 这里拒绝, 避免 "0x10" 被
    // 意外解析为 16.0 (LLM 可能输出十六进制文本, 不应视为十进制数值)
    if constexpr (std::is_floating_point_v<T>) {
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return false;
        }
    }
#if defined(_LIBCPP_VERSION)
    // llvm-mingw libc++ 的 from_chars 浮点特化缺失/被 delete，回退到 strtod/strtof
    if constexpr (std::is_floating_point_v<T>) {
        std::string tmp(s);
        char*       end = nullptr;
        errno           = 0;
        if constexpr (std::is_same_v<T, float>) {
            float fv = std::strtof(tmp.c_str(), &end);
            if (errno != 0 || end != tmp.c_str() + tmp.size() || !std::isfinite(fv)) {
                return false;
            }
            out = static_cast<T>(fv);
            return true;
        } else if constexpr (std::is_same_v<T, double>) {
            double dv = std::strtod(tmp.c_str(), &end);
            if (errno != 0 || end != tmp.c_str() + tmp.size() || !std::isfinite(dv)) {
                return false;
            }
            out = static_cast<T>(dv);
            return true;
        } else {
            long double ldv = std::strtold(tmp.c_str(), &end);
            if (errno != 0 || end != tmp.c_str() + tmp.size() || !std::isfinite(ldv)) {
                return false;
            }
            out = static_cast<T>(ldv);
            return true;
        }
    } else {
        T    v;
        auto result = std::from_chars(s.data(), s.data() + s.size(), v);
        if (result.ec == std::errc{} && result.ptr == s.data() + s.size()) {
            out = v;
            return true;
        }
        return false;
    }
#else
    T    v;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec == std::errc{} && result.ptr == s.data() + s.size()) {
        out = v;
        return true;
    }
    return false;
#endif
}

/// 解析布尔文本: 忽略首尾空白与 ASCII 大小写
/// - 真: `true` / `yes` / `y` / `on` / `enable` / `enabled` / `1`
/// - 假: `false` / `no` / `n` / `off` / `disable` / `disabled` / `0`
/// - 其它文本 (如 `"ok"`/`"maybe"`): 不转换, 保留原值交给工具参数校验
///   (插件侧读不到类型时用默认值或报参数错误, 不会静默当成 false)
/// - 与插件 SDK 的 ArgReader 读取时同一套认法 (kit.h 的 `detail::parseBoolText`)
///
/// - `args`:
///     - [s] 待解析文本
///     - [out] 解析成功时的布尔值
///
/// - `return` 是否解析成功
bool parseBoolText(std::string_view s, bool& out) {
    size_t b = 0, e = s.size();
    while (b < e && utilxx_base::charIsSpace(s[b])) {
        ++b;
    }
    while (e > b && utilxx_base::charIsSpace(s[e - 1])) {
        --e;
    }
    s = s.substr(b, e - b);
    if (s.empty()) {
        return false;
    }
    std::string lower;
    lower.reserve(s.size());
    for (const char c : s) {
        lower.push_back(utilxx_base::charToLower(c));
    }
    if (lower == "true" || lower == "yes" || lower == "y" || lower == "on"
        || lower == "enable" || lower == "enabled" || lower == "1") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "n" || lower == "off"
        || lower == "disable" || lower == "disabled" || lower == "0") {
        out = false;
        return true;
    }
    return false;
}

/// 数值 json -> 十进制字符串 (整数用 to_string, 浮点用最短可往返表示)
std::string numberToJsonString(const utilxx_base::Json& v) {
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    return fmt::format("{}", v.get<double>());
}

/// double -> long long 的取整 (参数自愈: 工具声明 integer 而模型给了浮点数)
/// - 与向上取整的距离不超过 1e-6 时取向上取整 (处理 4.999999999 这类浮点表示偏差,
///   也让"跟整数只差一点点"的值回到整数)
/// - 其余向下取整: 3.7 -> 3, -3.7 -> -4
/// - NaN/Inf 与超出 int64 表示范围的值返回 false (保持原样)
bool doubleToInt64Rounded(double v, long long& out) {
    if (!std::isfinite(v)) {
        return false;
    }
    constexpr double kRoundEps = 1e-6;
    const auto       up        = std::ceil(v);
    const double     rounded   = (up - v <= kRoundEps) ? up : std::floor(v);
    // 边界用可被 double 精确表示的 ±2^63 比较, 避免越界 static_cast 的未定义行为
    // (-2^63 为闭区间下界, +2^63 为开区间上界)
    constexpr double kInt64Min   = -9223372036854775808.0;
    constexpr double kInt64MaxP1 = 9223372036854775808.0;
    if (!(rounded >= kInt64Min && rounded < kInt64MaxP1)) {
        return false;
    }
    out = static_cast<long long>(rounded);
    return true;
}

/// schema 声明的类型集合
struct SchemaTypes {
    bool isArray   = false;
    bool isString  = false;
    bool isNumber  = false; // 声明了 "number"
    bool isInteger = false; // 声明了 "integer"
    bool isBool    = false;
    bool isObject  = false; // 声明了 "object"

    /// 是否声明了数值类型 (number 或 integer)
    bool isNumeric() const {
        return isNumber || isInteger;
    }
};

SchemaTypes getSchemaTypes(const utilxx_base::Json& schema) {
    SchemaTypes t;
    if (!schema.is_object()) {
        return t;
    }
    auto check = [&t](std::string_view one) {
        if ("array" == one) {
            t.isArray = true;
        } else if ("string" == one) {
            t.isString = true;
        } else if ("number" == one) {
            t.isNumber = true;
        } else if ("integer" == one) {
            t.isInteger = true;
        } else if ("boolean" == one) {
            t.isBool = true;
        } else if ("object" == one) {
            t.isObject = true;
        }
    };
    auto type = schema.value("type", std::string{});
    if (!type.empty()) {
        check(type);
    } else if (schema.contains("type") && schema["type"].is_array()) {
        // 联合类型, 如 {"type": ["array", "string"]}
        for (const auto& item : schema["type"]) {
            if (item.is_string()) {
                check(item.get<std::string>());
            }
        }
    }
    return t;
}

/// 数组元素是否允许为字符串 (items 未声明或声明为 string 时视为字符串数组)
bool isStringArrayItems(const utilxx_base::Json& schema) {
    auto items = schema["items"];
    if (items.is_object()) {
        auto itemType = items.value("type", std::string{});
        if (!itemType.empty() && "string" != itemType) {
            return false;
        }
    }
    return true;
}

/// 数组元素是否为对象 (`items` 声明 object; 未声明 type 时看是否写了对象关键字):
/// 用于把"少写一层数组"的单个对象包装成 `[obj]`
/// - `items.type` 为 `"object"` (或联合类型中含 object) 时成立
/// - `items` 未声明 type 但写了 `properties`/`required` 时按对象元素处理
/// - 其余 (string/number/... 元素, 或 items 缺失) 不包装对象参数
bool isObjectArrayItems(const utilxx_base::Json& schema) {
    const auto items = schema["items"];
    if (!items.is_object()) {
        return false;
    }
    const auto itemTypes = getSchemaTypes(items);
    if (itemTypes.isObject) {
        return true;
    }
    if (itemTypes.isArray || itemTypes.isString || itemTypes.isNumeric() || itemTypes.isBool) {
        return false;
    }
    return items.contains("properties") || items.contains("required");
}

/// 收集 schema 声明的字符串枚举值, 构造"忽略大小写 -> 规范值"映射 (Set 语义):
/// - 仅取 `enum` 数组中的字符串项 (数值/布尔枚举与字符串参数无关, 不参与);
///   枚举项本身大小写不同 (如 `["A", "a"]`) 时保留先出现者, 避免不确定改写
/// - 查询经 utilxx_base::IgnoreCaseMap (IgnoreCaseHash/IgnoreCaseEqual):
///   "File" / "FILE" / "file" 均命中规范值 `File`, 取出后按规范值重新赋值
/// - `return` 该 schema 的字符串枚举映射 (无字符串枚举时为空)
utilxx_base::IgnoreCaseMap<std::string> makeEnumIgnoreCaseMap(const utilxx_base::Json& schema) {
    utilxx_base::IgnoreCaseMap<std::string> enumMap;
    if (!schema.is_object() || !schema.contains("enum")) {
        return enumMap;
    }
    const auto& enumValues = schema["enum"];
    if (!enumValues.is_array()) {
        return enumMap;
    }
    for (const auto& item : enumValues) {
        if (!item.is_string()) {
            continue;
        }
        const auto value = item.get<std::string>();
        if (value.empty() || enumMap.contains(value)) {
            continue; // 空枚举值 / 大小写重复项: 保留先出现者, 避免不确定改写
        }
        // 键为规范值本身 (查询经忽略大小写哈希/相等比较)
        enumMap.emplace(value, value);
    }
    return enumMap;
}

/// 枚举字符串值的大小写自动修正 (对 [schema] 声明的 `enum` 逐项做忽略大小写匹配):
/// - [value] 为字符串: 命中枚举项且与规范值大小写不一致时改写为规范值
/// - [value] 为字符串数组: 按 `items.enum` 对每个字符串元素分别修正
///   (元素类型错误等其它情况不动, 交由工具自身校验)
/// - [fixInfo] 发生改写时追加描述 (供调用方记日志; 已有内容时以 ", " 连接)
/// `return` 是否发生了改写
bool autoFixEnumStringCase(
    const utilxx_base::Json& schema,
    utilxx_base::Json&       value,
    std::string&             fixInfo
) {
    auto appendFixInfo = [&fixInfo](std::string_view one) {
        if (fixInfo.empty()) {
            fixInfo = one;
        } else {
            fixInfo += ", ";
            fixInfo += one;
        }
    };
    if (value.is_string()) {
        const auto enumMap = makeEnumIgnoreCaseMap(schema);
        if (enumMap.empty()) {
            return false;
        }
        const auto cur = value.get<std::string>();
        auto       it  = enumMap.find(cur);
        if (it == enumMap.end() || it->second == cur) {
            return false; // 未命中枚举 (工具自身报错) / 大小写已一致
        }
        appendFixInfo(fmt::format("enum string case: `{}` -> `{}`", cur, it->second));
        value = it->second;
        return true;
    }
    if (value.is_array()) {
        const auto itemsIt = schema.find("items");
        if (itemsIt == schema.end() || !itemsIt->is_object()) {
            return false;
        }
        const auto enumMap = makeEnumIgnoreCaseMap(*itemsIt);
        if (enumMap.empty()) {
            return false;
        }
        bool        changed = false;
        std::string oneInfo;
        for (auto& item : value) {
            if (!item.is_string()) {
                continue;
            }
            const auto cur = item.get<std::string>();
            auto       it  = enumMap.find(cur);
            if (it == enumMap.end() || it->second == cur) {
                continue;
            }
            if (oneInfo.empty()) {
                oneInfo = fmt::format("enum string case: `{}` -> `{}`", cur, it->second);
            }
            item    = it->second;
            changed = true;
        }
        if (changed) {
            appendFixInfo(oneInfo);
        }
        return changed;
    }
    return false;
}

} // namespace

bool autoFixToolArgsTypes(
    const utilxx_base::Json& parameters,
    utilxx_base::Json&       args,
    std::string_view         toolName
) {
    if (!args.is_object()) {
        return false;
    }
    if (!parameters.is_object()) {
        return false;
    }
    const auto props = parameters["properties"];
    if (!props.is_object()) {
        return false;
    }

    bool changed = false;
    for (const auto& [name, schema] : props.items()) {
        // 跳过未传值的参数
        if (!args.contains(name)) {
            continue;
        }
        const auto& arg   = args[name];
        const auto  types = getSchemaTypes(schema);
        // 记录到日志的转换信息
        std::string fixInfo;

        if (arg.is_string()) {
            auto str = arg.get<std::string>();
            // 1) string -> 字符串数组
            if (types.isArray && isStringArrayItems(schema)) {
                auto arr = utilxx_base::Json::array();
                arr.push_back(arg);
                args[name] = std::move(arr);
                fixInfo    = "string -> [string]";
                changed    = true;
            } else if (types.isNumeric() && !types.isString) {
                // 2) string -> number/integer (目标不含 string 时)
                if (types.isNumber) {
                    // 声明了 number: 按浮点解析 (兼容小数/指数)
                    double v = 0;
                    if (parseFullNumber(str, v)) {
                        args[name] = v;
                        fixInfo    = "string -> number";
                        changed    = true;
                    }
                } else {
                    // 仅声明 integer: 整数写法直接取; 小数写法按“相近整数”取整
                    // ("3.5" -> 3, "49.9999999" -> 50); 整串不是数值则不转换
                    long long v = 0;
                    if (parseFullNumber(str, v)) {
                        args[name] = v;
                        fixInfo    = "string -> integer";
                        changed    = true;
                    } else {
                        double d = 0;
                        if (parseFullNumber(str, d) && doubleToInt64Rounded(d, v)) {
                            args[name] = v;
                            fixInfo    = "string -> integer (rounded)";
                            changed    = true;
                        }
                    }
                }
            } else if (types.isBool && !types.isString) {
                // 3) string -> boolean (目标不含 string 时): 忽略首尾空白与大小写,
                // 认 true/yes/y/on/enable/enabled/1 与 false/no/n/off/disable/disabled/0;
                // 其它文本保持原样交给工具参数校验
                bool v = false;
                if (parseBoolText(str, v)) {
                    args[name] = v;
                    fixInfo    = "string -> boolean";
                    changed    = true;
                }
            }
        } else if (arg.is_number()) {
            // number/integer -> string (目标不含数值时)
            if (types.isString && !types.isNumeric()) {
                args[name] = numberToJsonString(arg);
                fixInfo    = "number -> string";
                changed    = true;
            } else if (arg.is_number_float()) {
                // number(double) -> integer: 目标声明 integer 时取相近整数
                // (3.7 -> 3, 4.999999999 -> 5, -3.7 -> -4); 越界/NaN 保持原样
                if (types.isInteger && !types.isString) {
                    long long v = 0;
                    if (doubleToInt64Rounded(arg.get<double>(), v)) {
                        args[name] = v;
                        fixInfo    = "number -> integer";
                        changed    = true;
                    }
                }
            } else {
                // integer -> number(double): 工具声明 number 即期望 double 类型
                // (联合类型含 string 或已声明 integer 时 arg 已合法, 不转换)
                if (types.isNumber && !types.isInteger && !types.isString) {
                    args[name] = arg.get<double>();
                    fixInfo    = "integer -> number";
                    changed    = true;
                }
            }
        } else if (arg.is_bool()) {
            // bool -> string (目标不含 boolean 时)
            if (types.isString && !types.isBool) {
                args[name] = arg.get<bool>() ? std::string{"true"} : std::string{"false"};
                fixInfo    = "bool -> string";
                changed    = true;
            }
        } else if (arg.is_object()) {
            // object -> 对象数组 (目标声明数组且元素为对象, 目标不含 object 时):
            // LLM 少写一层数组的常见形态, 如 `edits`/`actions` 传成单个对象
            if (types.isArray && !types.isObject && isObjectArrayItems(schema)) {
                auto arr = utilxx_base::Json::array();
                arr.push_back(arg);
                args[name] = std::move(arr);
                fixInfo    = "object -> [object]";
                changed    = true;
            }
        } else if (arg.is_array()) {
            // [单字符串数组] -> string (目标不含 array 时)
            if (types.isString && !types.isArray && arg.size() == 1 && arg[0].is_string()) {
                args[name] = arg[0].get<std::string>();
                fixInfo    = "[string] -> string";
                changed    = true;
            } else if (types.isObject && !types.isArray && arg.size() == 1 && arg[0].is_object()) {
                // [单对象数组] -> object (目标不含 array 时): 与字符串数组解包同理
                // - 来源与被赋值对象同属 `args[name]`: 赋值会先销毁旧值 (arg 随即失效),
                //   故先把元素拷出来再赋值
                utilxx_base::Json unwrapped = arg[0];
                args[name]                  = std::move(unwrapped);
                fixInfo                     = "[object] -> object";
                changed                     = true;
            }
        }

        // 枚举字符串大小写自动修正: 先按 schema `enum` 构造忽略大小写的映射
        // (Set 语义), 参数传入值经该映射取值后按枚举的规范值重新赋值
        // - 放在类型转换之后: 以最终写入的值为准 (字符串被包装为数组时,
        //   按 `items.enum` 对元素逐项修正)
        if (autoFixEnumStringCase(schema, args[name], fixInfo)) {
            changed = true;
        }

        if (!fixInfo.empty()) {
            XX_LOGD("Tool args auto-fix `{}`: {} (tool: {})", name, fixInfo, toolName);
        }
    }
    return changed;
}

std::string autoFixedToolArgsJson(const utilxx_base::Json& parameters, std::string_view argsJson) {
    if (argsJson.empty() || !parameters.is_object()) {
        return std::string{argsJson};
    }
    try {
        auto args = utilxx_base::Json::parse(argsJson);
        if (!args.is_object()) {
            return std::string{argsJson};
        }
        if (!autoFixToolArgsTypes(parameters, args)) {
            return std::string{argsJson};
        }
        return args.dump();
    } catch (...) {
        // 参数仍在流式输出中/形状异常: 保持原文 (与渲染器"解析不了就只给显示名"一致)
        return std::string{argsJson};
    }
}

} // namespace util
} // namespace agentxx
