#pragma once

#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace events {
namespace naming {

/// 事件词表: 事件名 / 总线主题的字符集、长度与保留前缀
///
/// 唯一实现: `EventBus` 的前缀订阅方 (`EventHub`) 与 `agentxx_event` 工具的
/// define/watch 都经这里校验, 以后改命名规则只改这一处。
///
/// 两层名字:
/// - **事件名** (`build.done` / `chat.group42`): 模型与插件看到的名字, hub 内唯一
/// - **总线主题** (`evt.build.done` / 插件经 `qualifyEventTopic` 得到的
///   `plugin.evt.build.done`): 真正在 EventBus 上发布的名字, 由事件名加前缀得到

/// 事件在总线上的主题前缀 (hub 监听它)
inline constexpr std::string_view kValueTopicPrefix = "evt.";

/// 插件事件转发时追加的前缀 (见插件运行时 `qualifyEventTopic`)
inline constexpr std::string_view kPluginTopicPrefix = "plugin.";

/// 事件名最大长度
inline constexpr size_t kMaxEventNameLength = 96;

/// 保留的事件名前缀
/// - 这些前缀的主题属于总线自身 (`agent.model.*` / `io.*` / `service.*` 等),
///   事件名借用它们会与既有主题混淆
inline constexpr std::string_view kReservedPrefixes[] = {
    "agent.",
    "io.",
    "service.",
    "hub.",
    "plugin.",
    "evt.",
    "client.",
};

/// 单个事件名是否合法 (只检查名字本身: 非空/长度/字符集/保留前缀)
/// - `args`:
///     - [err] 非空时写入不合格的原因 (供工具与插件回错误文本)
/// - `return` true = 可以用作事件名
inline bool validateEventName(std::string_view name, std::string* err = nullptr) {
    auto fail = [&](std::string msg) {
        if (err) {
            *err = std::move(msg);
        }
        return false;
    };
    if (name.empty()) {
        return fail("event name is empty");
    }
    if (name.size() > kMaxEventNameLength) {
        return fail(
            "event name too long (max " + std::to_string(kMaxEventNameLength) + " chars)"
        );
    }
    for (const auto ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                        || ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return fail("event name may only contain letters, digits, '.', '_' and '-'");
        }
    }
    for (const auto reserved : kReservedPrefixes) {
        if (name.starts_with(reserved)) {
            return fail("event name prefix `" + std::string{reserved} + "` is reserved");
        }
    }
    return true;
}

/// 事件名 → 总线主题 (`evt.<name>`)
inline std::string busTopicOf(std::string_view name) {
    return std::string{kValueTopicPrefix} + std::string{name};
}

/// 总线主题 → 事件名; 不是值主题时返回空
/// - `evt.<name>` (agent 级发布) 与 `plugin.evt.<name>` (插件转发) 都认
inline std::optional<std::string> eventNameOfTopic(std::string_view topic) {
    auto strip = [&](std::string_view prefix) -> std::optional<std::string> {
        if (topic.size() > prefix.size() && topic.starts_with(prefix)) {
            return std::string{topic.substr(prefix.size())};
        }
        return std::nullopt;
    };
    if (auto name = strip(std::string{kPluginTopicPrefix} + std::string{kValueTopicPrefix})) {
        return name;
    }
    return strip(kValueTopicPrefix);
}

} // namespace naming
} // namespace events
} // namespace agentxx
