#include "agentxx/event/hub.h"

#include "fmt/format.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace events {

namespace {

/// 纯文本渲染的嵌套深度上限: 超过后退回紧凑 JSON, 防止异常深的嵌套把渲染撑爆
constexpr int kMaxPlainDepth = 8;

/// 每级缩进的空格数
constexpr std::string_view kIndentStep = "  ";

/// 该值是否要在键/项后换行再展开 (对象、非空数组, 或含换行的字符串)
bool isBlockValue(const Json& value) {
    if (value.is_object()) {
        return !value.empty();
    }
    if (value.is_array()) {
        return !value.empty();
    }
    if (value.is_string()) {
        return value.get_string_view().find('\n') != std::string_view::npos;
    }
    return false;
}

/// 标量值文本 (字符串原样, 其余同 JSON 文本)
std::string scalarText(const Json& value) {
    if (value.is_string()) {
        return std::string{value.get_string_view()};
    }
    return value.dump();
}

/// 多行文本逐行前补缩进 (首行也补, 末尾换行不再补一行空内容)
void appendIndented(std::string& out, std::string_view text, std::string_view indent) {
    size_t pos = 0;
    while (true) {
        const auto nl   = text.find('\n', pos);
        const auto line = text.substr(
            pos,
            nl == std::string_view::npos ? std::string_view::npos : nl - pos
        );
        out += indent;
        out += line;
        if (nl == std::string_view::npos) {
            return;
        }
        out += '\n';
        pos = nl + 1;
        if (pos >= text.size()) {
            return;
        }
    }
}

void appendPlain(std::string& out, const Json& value, std::string_view indent, int depth);

/// 对象: 逐行 `键: 值`
void appendObject(std::string& out, const Json& value, std::string_view indent, int depth) {
    if (value.empty()) {
        out += "{}";
        return;
    }
    std::string deeper{indent};
    deeper += kIndentStep;
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!first) {
            out += '\n';
        }
        first = false;
        out += indent;
        out += it.key();
        if (isBlockValue(*it)) {
            out += ":\n";
            if (it->is_string()) {
                appendIndented(out, it->get_string_view(), deeper);
            } else {
                appendPlain(out, *it, deeper, depth + 1);
            }
        } else {
            out += ": ";
            out += scalarText(*it);
        }
    }
}

/// 数组: 逐行 `- 值`
/// - 嵌套对象/多行字符串接在 `- ` 之后, 后续行按上一级缩进对齐 (与 YAML 写法一致)
void appendArray(std::string& out, const Json& value, std::string_view indent, int depth) {
    if (value.empty()) {
        out += "[]";
        return;
    }
    std::string deeper{indent};
    deeper += kIndentStep;
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            out += '\n';
        }
        first = false;
        out += indent;
        out += '-';
        if (!isBlockValue(item)) {
            out += ' ';
            out += scalarText(item);
            continue;
        }
        out += ' ';
        // 嵌套内容先按下一级缩进渲染, 再把首行缩进换成 "- " 对齐
        std::string nested;
        if (item.is_string()) {
            appendIndented(nested, item.get_string_view(), deeper);
        } else {
            appendPlain(nested, item, deeper, depth + 1);
        }
        if (nested.starts_with(deeper)) {
            nested.erase(0, deeper.size());
        }
        out += nested;
    }
}

/// 值 → 纯文本 (按类型分派; 深度超限时退回紧凑 JSON)
void appendPlain(std::string& out, const Json& value, std::string_view indent, int depth) {
    if (depth > kMaxPlainDepth) {
        out += value.dump();
        return;
    }
    if (value.is_object()) {
        appendObject(out, value, indent, depth);
        return;
    }
    if (value.is_array()) {
        appendArray(out, value, indent, depth);
        return;
    }
    out += scalarText(value);
}

} // namespace

std::string renderEventValuesText(const std::vector<Json>& values) {
    std::string out;
    bool        first = true;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!first) {
            out += '\n';
        }
        first = false;
        // 多值: 每段前一行 `#<序号>`, 模型据此区分是"同一条流的第几个值"
        if (values.size() > 1) {
            out += fmt::format("#{}\n", i + 1);
        }
        appendPlain(out, values[i], std::string_view{}, 0);
    }
    return out;
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
    out += "[Content]\n";
    if (contentText.empty()) {
        out += "(no value)";
    } else {
        out += contentText;
    }
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
