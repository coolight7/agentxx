#include "agentxx/event/hub.h"

#include "fmt/format.h"
#include <string>
#include <vector>

namespace agentxx {
namespace events {

namespace {

/// 状态/种类/模式的显示文本
std::string_view kindText(EventKind kind) noexcept {
    return kind == EventKind::Single ? "single" : "stream";
}

std::string_view stateText(EventState state) noexcept {
    switch (state) {
        case EventState::Open:
            return "open";
        case EventState::Ended:
            return "ended";
        case EventState::Expired:
            return "expired";
    }
    return "unknown";
}

std::string_view waitStateText(WaitState state) noexcept {
    switch (state) {
        case WaitState::Armed:
            return "armed";
        case WaitState::Satisfied:
            return "satisfied";
        case WaitState::Cancelled:
            return "cancelled";
        case WaitState::TimedOut:
            return "timed-out";
    }
    return "unknown";
}

/// "wait: w-3 ref: call_1" 一行的尾部 (没有 ref 时为空)
std::string refSuffix(const EventWait& wait) {
    if (wait.spec.ref.empty()) {
        return {};
    }
    return fmt::format(" ref: {}", wait.spec.ref);
}

} // namespace

std::string renderWaitArmed(const EventWait& wait) {
    return fmt::format(
        "[Event wait armed] wait={} event={} mode={} delivery={}\n"
        "The value will arrive later as an input starting with \"[Event] {}\".\n"
        "Keep working, or end the turn — you do not need to poll. "
        "Use opt=status wait={} to check.",
        wait.waitId,
        wait.spec.event,
        wait.spec.mode,
        wait.spec.delivery,
        wait.spec.event,
        wait.waitId
    );
}

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

std::string renderWaitStatus(const EventWait& wait) {
    return fmt::format(
        "[Event wait] wait={} event={} mode={} delivery={} state={} values={} deliveries={}{}",
        wait.waitId,
        wait.spec.event,
        wait.spec.mode,
        wait.spec.delivery,
        waitStateText(wait.state),
        wait.values.size(),
        wait.deliveries,
        wait.closeReason.empty() ? std::string{} : fmt::format(" reason={}", wait.closeReason)
    );
}

std::string renderEventStatus(const EventRecord& rec) {
    std::string out = fmt::format(
        "[Event record] name={} kind={} state={} owner={} published={} history={} waits={}"
        "{}",
        rec.name,
        kindText(rec.kind),
        stateText(rec.state),
        rec.owner.empty() ? std::string{"-"} : rec.owner,
        rec.published,
        rec.history.size(),
        rec.waitCount,
        rec.droppedForHistory > 0 ? fmt::format(" dropped={}", rec.droppedForHistory)
                                  : std::string{}
    );
    if (!rec.description.empty()) {
        out += fmt::format("\ndescription: {}", rec.description);
    }
    if (!rec.history.empty()) {
        out += fmt::format("\nlast: {}", rec.history.back().dump());
    }
    return out;
}

std::string renderAwaitTimeout(const EventWaitSpec& spec, int64_t waitedMs) {
    const auto seconds = waitedMs / 1000;
    return fmt::format(
        "[Event await timeout] event={} waited={}s (no value arrived; the wait was removed, "
        "use opt=watch to keep waiting in the background)",
        spec.event,
        seconds
    );
}

} // namespace events
} // namespace agentxx
