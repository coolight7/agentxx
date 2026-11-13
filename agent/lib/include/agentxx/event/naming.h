#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace events {
namespace naming {

/// 命名契约: 总线主题 / 事件中枢事件名 / 功能埋点 id 共用一套前缀规则
///
/// 三族命名空间, **写** (注册/发布) 只能落在自己那一族的根下, **读** (订阅/等待)
/// 可以跨族:
/// - 内置 agent 端: `agentxx.agent.<域>...`  (轮次/模型/工具/服务/中枢...)
/// - 内置 client 端: `agentxx.client.<域>...` (客户端主程序上报/下发)
/// - 插件:          `plugin.<名字>.<自由部分>`
///
/// 插件那一族**只校验 `plugin.` 根, 不限定后面的名字是不是本实例的**:
/// 同一组织的多个插件可能复用代码、同一功能由不同插件包提供实现, 共用一段是
/// 正当需求。建议仍用自己的插件名作前缀 (`plugin.<本实例插件名>.xxx`), 便于归属与
/// 退回时整批回收记录, 但框架不做强制。
///
/// 框架**只校验不补前缀**: 名字不在所属族里就报错, 由插件自己写全名。
///
/// 事件中枢的事件名与总线主题是**同一个字符串**: 中枢直接按主题收值
/// (`plugin.` 前缀), 内置值事件统一放在 `agentxx.agent.event.` 段,
/// 与 `agentxx.agent.turn.start` 这类强类型主题分开, 避免同一个主题被
/// 两种载荷类型共用。

/// 内置命名根 (agent 端与 client 端共用)
inline constexpr std::string_view kBuiltinRoot = "agentxx.";

/// 内置 agent 端前缀 (轮次/模型/工具/服务/中枢等全部内置主题)
inline constexpr std::string_view kAgentRoot = "agentxx.agent.";

/// 内置 client 端前缀 (客户端主程序的主题)
inline constexpr std::string_view kClientRoot = "agentxx.client.";

/// 插件命名根
inline constexpr std::string_view kPluginRoot = "plugin.";

/// 事件中枢的值事件段 (内置源与模型自建事件)
inline constexpr std::string_view kAgentEventPrefix = "agentxx.agent.event.";

/// 内置异步任务的结果事件段 (框架保留: 后台任务完成值都发在这一段)
/// - 已实现: `agentxx.agent.async.done` (单事件名, 靠值里的 `asyncId` 区分任务)
inline constexpr std::string_view kAgentAsyncPrefix = "agentxx.agent.async.";

/// 后台任务结果事件名 (所有任务都发这一条, 靠值里的 `asyncId` 区分)
inline constexpr std::string_view kAgentAsyncDoneEvent = "agentxx.agent.async.done";

/// 后台任务结果事件的归属标记 (发布方与日志用)
/// - 事件现在只是广播: 结果回到会话由 `AsyncWorkManager::finishEntry` 直接做
/// - `agentxx_async_await` 不再订阅这条事件 (它等任务条目上的票据)
inline constexpr std::string_view kAgentAsyncNotifyOwner = "async_work";

/// client 端插件数据的主题段 (客户端插件上行到 agent 总线)
/// - 组装结果: `agentxx.client.plugin.<插件名>.<事件名>`
inline constexpr std::string_view kClientPluginDataPrefix = "agentxx.client.plugin.";

/// 名字最大长度 (字符数, 按字节计; 主题与事件名同一上限)
inline constexpr size_t kMaxNameLength = 128;

/// 内置值事件里保留给框架的子段
/// - 模型/插件都不能用这些子段建自己的事件, 否则与内建源同名混淆
inline constexpr std::string_view kReservedEventSegments[] = {
    "turn.",
    "tool.",
    "subagent.",
    "timer.",
    "host.",
};

/// 名字语法检查 (只检查名字本身: 长度/字符集/段结构)
/// - 字符集: `[A-Za-z0-9._-]`
/// - 必须字母开头, 不许空段 (`a..b`)、不许首尾是 `.`
///
/// - `args`:
///     - [err] 非空时写入不合格的原因 (供工具与插件回错误文本)
inline bool validateNameSyntax(std::string_view name, std::string* err = nullptr) {
    auto fail = [&](std::string msg) {
        if (err) {
            *err = std::move(msg);
        }
        return false;
    };
    if (name.empty()) {
        return fail("name is empty");
    }
    if (name.size() > kMaxNameLength) {
        return fail(
            "name too long (max " + std::to_string(kMaxNameLength) + " chars)"
        );
    }
    for (const char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                        || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return fail("name may only contain letters, digits, '.', '_' and '-'");
        }
    }
    const char first = name.front();
    const bool firstOk = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z');
    if (!firstOk) {
        return fail("name must start with a letter");
    }
    if (name.back() == '.') {
        return fail("name must not end with '.'");
    }
    if (name.find("..") != std::string_view::npos) {
        return fail("name must not contain an empty segment ('..')");
    }
    return true;
}

/// 插件主题前缀 (`plugin.<插件名>.`)
/// - 插件名本身为空时返回空串 (调用方据此判参数错误)
inline std::string pluginPrefix(std::string_view pluginName) {
    if (pluginName.empty()) {
        return std::string{};
    }
    return std::string{kPluginRoot} + std::string{pluginName} + ".";
}

/// 主题是否属于内置 agent 端
inline bool isAgentTopic(std::string_view topic) {
    return topic.size() > kAgentRoot.size() && topic.starts_with(kAgentRoot);
}

/// 主题是否属于内置 client 端
inline bool isClientTopic(std::string_view topic) {
    return topic.size() > kClientRoot.size() && topic.starts_with(kClientRoot);
}

/// 主题是否属于插件族 (`plugin.<插件名>.x`)
inline bool isPluginTopic(std::string_view topic) {
    if (topic.size() <= kPluginRoot.size() || !topic.starts_with(kPluginRoot)) {
        return false;
    }
    const auto rest = topic.substr(kPluginRoot.size());
    const auto dot  = rest.find('.');
    return dot != std::string_view::npos && dot > 0 && dot + 1 < rest.size();
}

/// 插件族主题里的插件名 (`plugin.<这里>.x`); 不是插件族返回空
inline std::string_view pluginNameOfTopic(std::string_view topic) {
    if (!isPluginTopic(topic)) {
        return {};
    }
    const auto rest = topic.substr(kPluginRoot.size());
    return rest.substr(0, rest.find('.'));
}

/// client 端插件数据主题的前缀 (`agentxx.client.plugin.<插件名>.`)
/// - client 插件只能往这条族里写; 名字由插件写全, 框架不补前缀
inline std::string clientPluginPrefix(std::string_view pluginName) {
    if (pluginName.empty()) {
        return std::string{};
    }
    return std::string{kClientPluginDataPrefix} + std::string{pluginName} + ".";
}

/// client 插件数据主题里的插件名 (`agentxx.client.plugin.<这里>.x`); 不匹配返回空
inline std::string_view clientPluginNameOfTopic(std::string_view topic) {
    if (topic.size() <= kClientPluginDataPrefix.size()
        || !topic.starts_with(kClientPluginDataPrefix)) {
        return {};
    }
    const auto rest = topic.substr(kClientPluginDataPrefix.size());
    const auto dot  = rest.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size()) {
        return {};
    }
    return rest.substr(0, dot);
}

/// client 插件上行主题校验 (`agentxx.client.plugin.<插件名>.<事件>`)
/// - 只要求落在 client 插件族里, **不限定插件名**: 同一组织的多个插件、
///   以及“同一功能由不同插件包提供实现”的场景可以写同一个段
///   (建议用自己的插件名, 见 docs/zh-cn/design/plugins.md §21)
inline bool validateClientPluginWriteTopic(std::string_view topic, std::string* err = nullptr) {
    if (!validateNameSyntax(topic, err)) {
        return false;
    }
    if (!clientPluginNameOfTopic(topic).empty()) {
        return true;
    }
    if (err) {
        *err = "topic must be inside `agentxx.client.plugin.<plugin name>.<event>` (the framework "
               "does not add the prefix for you)";
    }
    return false;
}

/// 中枢值事件名: 内置段 (`agentxx.agent.event.*`) / 异步任务段
/// (`agentxx.agent.async.*`) 或插件族 (`plugin.<名>.*`)
/// - 内置 agent 主题里只有这两段是值事件, 其余是强类型主题
inline bool isEventName(std::string_view name) {
    if (name.size() > kAgentEventPrefix.size() && name.starts_with(kAgentEventPrefix)) {
        return true;
    }
    if (name.size() > kAgentAsyncPrefix.size() && name.starts_with(kAgentAsyncPrefix)) {
        return true;
    }
    return isPluginTopic(name);
}

/// 值事件是否落在框架保留的内置子段里 (`turn.` / `tool.` / `subagent.` / ...)
/// - 模型与插件建事件时据此拒绝, 内建源自己发布时不查
inline bool isReservedEventSegment(std::string_view name) {
    if (!name.starts_with(kAgentEventPrefix)) {
        return false;
    }
    const auto rest = name.substr(kAgentEventPrefix.size());
    for (const auto segment : kReservedEventSegments) {
        if (rest.starts_with(segment)) {
            return true;
        }
    }
    return false;
}

/// 事件中枢的事件名校验 (定义/发布/等待三处共用)
/// - 名字必须是 `agentxx.agent.event.*` / `agentxx.agent.async.*` 或 `plugin.<插件名>.*`
inline bool validateEventName(std::string_view name, std::string* err = nullptr) {
    if (!validateNameSyntax(name, err)) {
        return false;
    }
    if (isEventName(name)) {
        return true;
    }
    if (err) {
        *err = "event name must be `agentxx.agent.event.<name>` or "
               "`agentxx.agent.async.<name>` (built in) or `plugin.<plugin name>.<name>` "
               "(plugin)";
    }
    return false;
}

/// 内置 agent 端值事件名 (`agentxx.agent.event.*`, 不计保留段)
/// - 内建事件源与框架自建事件用; 模型建事件时另加 [isReservedEventSegment] 检查
inline bool validateAgentEventName(std::string_view name, std::string* err = nullptr) {
    if (!validateNameSyntax(name, err)) {
        return false;
    }
    if (name.starts_with(kAgentEventPrefix) && name.size() > kAgentEventPrefix.size()) {
        return true;
    }
    if (err) {
        *err = "built-in event name must be `agentxx.agent.event.<name>`";
    }
    return false;
}

/// 插件写侧校验 (`plugin.<插件名>.<事件>`)
/// - 只要求落在插件族里, **不限定插件名**: 同一组织的多个插件可以共用一个段
///   (代码复用、同一功能由不同插件包提供实现等), 建议仍用自己的插件名
/// - 框架**不补前缀**: 不在插件族里就报错, 错误文本直接给出应有的前缀
inline bool validatePluginWriteTopic(std::string_view name, std::string* err = nullptr) {
    if (!validateNameSyntax(name, err)) {
        return false;
    }
    if (isPluginTopic(name)) {
        return true;
    }
    if (err) {
        *err = "name must be inside `plugin.<plugin name>.<event>` (the framework does not add "
               "the prefix for you)";
    }
    return false;
}

/// 读侧校验 (订阅/等待): 三族任意, 但必须是全名 (不许裸名)
inline bool validateReadableTopic(std::string_view topic, std::string* err = nullptr) {
    if (!validateNameSyntax(topic, err)) {
        return false;
    }
    if (isAgentTopic(topic) || isClientTopic(topic) || isPluginTopic(topic)) {
        return true;
    }
    if (err) {
        *err = "topic must be a full name in one of: `agentxx.agent.*`, `agentxx.client.*`, "
               "`plugin.<plugin name>.*`";
    }
    return false;
}

/// 内置主题名 (框架自己的主题常量) 校验: 必须在内置 agent/client 段内
inline bool validateBuiltinTopic(std::string_view topic, std::string* err = nullptr) {
    if (!validateNameSyntax(topic, err)) {
        return false;
    }
    if (isAgentTopic(topic) || isClientTopic(topic)) {
        return true;
    }
    if (err) {
        *err = "built-in topic must be `agentxx.agent.*` or `agentxx.client.*`";
    }
    return false;
}

/// 功能埋点 id 校验 (与事件名同一套前缀规则)
/// - 内置点: `agentxx.agent.*` (核心实现声明)
/// - 插件点: `plugin.<插件名>.*` (不限定插件名, 见 [validatePluginWriteTopic])
inline bool validateFeaturePointId(
    std::string_view id,
    std::string_view pluginName, ///< 空 = 内置实现
    std::string*     err = nullptr
) {
    if (!validateNameSyntax(id, err)) {
        return false;
    }
    if (!pluginName.empty()) {
        return validatePluginWriteTopic(id, err);
    }
    if (id.size() > kAgentRoot.size() && id.starts_with(kAgentRoot)) {
        return true;
    }
    if (err) {
        *err = "built-in feature point id must be `agentxx.agent.<name>`";
    }
    return false;
}

} // namespace naming
} // namespace events
} // namespace agentxx
