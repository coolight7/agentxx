/// agentxx::util::Json <-> neograph::json 桥接 (窄边界专用)
///
/// - 唯一合法包含点: `agent/lib/src/nodes/*` 与 BaseAgent 图边界
///   (StateGraph channel 写入 / ChatMessage::extra / ChatTool::parameters)
/// - 严禁泄漏到业务层、工具层、插件层: 业务代码一律使用 agentxx::util::Json
#pragma once

#include "agentxx/util/json.h"
#include <neograph/json.h>

namespace agentxx {
namespace util {

/// agentxx::util::Json -> neograph::json (进入 NeoGraph 图边界时使用)
inline neograph::json toNeographJson(const Json& j) {
    switch (j.type()) {
        case Json::Type::Null:
            return neograph::json{};
        case Json::Type::Boolean:
            return neograph::json(j.get<bool>());
        case Json::Type::NumberInt:
            return neograph::json(j.get<int64_t>());
        case Json::Type::NumberUint:
            return neograph::json(j.get<uint64_t>());
        case Json::Type::NumberFloat:
            return neograph::json(j.get<double>());
        case Json::Type::String:
            return neograph::json(j.get<std::string>());
        case Json::Type::Array: {
            auto arr = neograph::json::array();
            for (size_t i = 0; i < j.size(); ++i) {
                arr.push_back(toNeographJson(j[i]));
            }
            return arr;
        }
        case Json::Type::Object: {
            auto obj = neograph::json::object();
            for (const auto& [k, v] : j.items()) {
                obj[std::string(k)] = toNeographJson(v);
            }
            return obj;
        }
    }
    return neograph::json{};
}

/// neograph::json -> agentxx::util::Json (从 NeoGraph 图边界退出时使用)
///
/// - 整数区分: is_number_unsigned() (仅 uint64 大整数) -> NumberUint,
///   其余整数 -> NumberInt (与 Json::get<int64_t> 宽容读取一致)
inline Json fromNeographJson(const neograph::json& j) {
    if (j.is_null()) {
        return Json{};
    }
    if (j.is_boolean()) {
        return Json(j.get<bool>());
    }
    if (j.is_number_unsigned()) {
        return Json(j.get<uint64_t>());
    }
    if (j.is_number_integer()) {
        return Json(j.get<int64_t>());
    }
    if (j.is_number_float()) {
        return Json(j.get<double>());
    }
    if (j.is_string()) {
        return Json(j.get<std::string>());
    }
    if (j.is_array()) {
        auto arr = Json::array();
        for (const auto& item : j) {
            arr.push_back(fromNeographJson(item));
        }
        return arr;
    }
    if (j.is_object()) {
        auto obj = Json::object();
        for (const auto& [k, v] : j.items()) {
            obj[k] = fromNeographJson(v);
        }
        return obj;
    }
    return Json{};
}

/// 文本直解为 neograph::json (测试/图边界构造 schema 用)
inline neograph::json parseNeographJson(std::string_view sv) {
    return neograph::json::parse(sv);
}

} // namespace util
} // namespace agentxx
