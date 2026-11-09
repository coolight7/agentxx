#include "agentxx/event/hub.h"

#include "fmt/format.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace events {

namespace {

/// 值的 "产物正文" 字段: 只有框架自己的后台任务结果事件是这种形状
constexpr std::string_view kAsyncResultTextField = "content";

/// 拼一段 "键:\n原文" (键名就是值里那个字段名, 原文多行就是多行)
void appendTextField(
    std::string&     out,
    std::string_view fieldName,
    std::string_view body
) {
    out += fieldName;
    out += ":\n";
    out += body;
}

/// 单个值 → 文本 (见 [renderEventValuesText] 的规则说明)
std::string renderOneValue(const Json& value, std::string_view textPayloadField) {
    // 字符串: 它本身就是文本, 直接给原文 (不是 JSON 结构, 没有可保的东西)
    if (value.is_string()) {
        return std::string{value.get_string_view()};
    }
    // 对象/数组: 别人的 JSON 原样输出 (键序/转义/数字格式都不动)
    if (!value.is_object() || textPayloadField.empty() || !value.contains(textPayloadField)
        || !value[textPayloadField].is_string()) {
        return value.dump();
    }
    // 声明的正文字段: 从信封里拿出来按原文给出, 其余字段仍是同一个 JSON
    // (正文字段没丢: 它就在下面同名那一行里)
    Json envelope = Json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == textPayloadField) {
            continue;
        }
        envelope[it.key()] = *it;
    }
    std::string out;
    if (!envelope.empty()) {
        out += envelope.dump();
        out += '\n';
    }
    appendTextField(out, textPayloadField, value[textPayloadField].get_string_view());
    return out;
}

} // namespace

std::string_view eventTextPayloadField(std::string_view eventName) {
    // 后台任务结果事件把工具输出 / 子代理回答放在 `content` 里 (多行文本)
    if (eventName.starts_with(naming::kAgentAsyncPrefix)) {
        return kAsyncResultTextField;
    }
    // 其余一律不解释字段: 值可能是插件/模型自定义的 JSON
    return {};
}

std::string renderEventValuesText(const std::vector<Json>& values, std::string_view textPayloadField) {
    if (values.empty()) {
        return {};
    }
    // 多个值是一次投递的多条 (mode=all 等): 用 JSON 数组如实表达"这些值",
    // 不再另造编号/缩进格式
    if (values.size() > 1) {
        Json arr = Json::array();
        for (const auto& value : values) {
            arr.push_back(value);
        }
        return arr.dump();
    }
    return renderOneValue(values.front(), textPayloadField);
}

std::string renderEventText(
    const EventWait& wait,
    std::string_view status,
    std::string_view contentText,
    std::string_view note
) {
    // 首行只有事件名: 越短越容易在折叠预览与日志里认出是哪个事件
    std::string out = fmt::format(
        "[Event] {}\n[Info]\nmode: {}\nwait: {}\nstatus: {}\n",
        wait.spec.event,
        wait.spec.mode,
        wait.waitId,
        status
    );
    if (!wait.spec.ref.empty()) {
        out += fmt::format("ref: {}\n", wait.spec.ref);
    }
    if (!wait.spec.label.empty()) {
        out += fmt::format("label: {}\n", wait.spec.label);
    }
    if (!note.empty()) {
        out += fmt::format("note: {}\n", note);
    }
    out += contentText.empty() ? kEventNoContentSection : kEventContentSection;
    out += '\n';
    out += contentText;
    // 去掉末尾换行: 投递文本作为一条 user 消息, 末尾空行没有意义
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::string renderAwaitTimeout(const EventWaitSpec& spec, int64_t waitedMs) {
    const auto seconds = waitedMs / 1000;
    return fmt::format(
        "[Event await timeout] event={} waited={}s (no value arrived; the wait was removed, "
        "query the task again with agentxx_async_query, or keep working and let the automatic notification deliver it)",
        spec.event,
        seconds
    );
}

} // namespace events
} // namespace agentxx
