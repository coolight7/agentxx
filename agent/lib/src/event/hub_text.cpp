#include "agentxx/event/hub.h"

#include "fmt/format.h"
#include <string>
#include <vector>

namespace agentxx {
namespace events {

namespace {

/// "wait: w-3 ref: call_1" 一行的尾部 (没有 ref 时为空)
std::string refSuffix(const EventWait& wait) {
    if (wait.spec.ref.empty()) {
        return {};
    }
    return fmt::format(" ref: {}", wait.spec.ref);
}

} // namespace

std::string renderEventText(
    const EventWait&                wait,
    std::string_view                status,
    const std::vector<std::string>& valueTexts,
    std::string_view                note
) {
    std::string out = fmt::format(
        "[Event] {} ({})\nwait: {}{}\nstatus: {}\n",
        wait.spec.event,
        wait.spec.mode,
        wait.waitId,
        refSuffix(wait),
        status
    );
    if (valueTexts.size() == 1) {
        out += fmt::format("value: {}\n", valueTexts.front());
    } else if (!valueTexts.empty()) {
        out += "values:\n";
        size_t index = 0;
        for (const auto& text : valueTexts) {
            ++index;
            out += fmt::format("  {}. {}\n", index, text);
        }
    }
    if (!note.empty()) {
        out += fmt::format("note: {}\n", note);
    }
    if (!wait.spec.label.empty()) {
        out += fmt::format("label: {}\n", wait.spec.label);
    }
    // 去掉末尾换行: 投递文本作为一条 user 消息, 末尾空行没有意义
    if (!out.empty() && out.back() == '\n') {
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
