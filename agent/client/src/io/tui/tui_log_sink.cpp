#include "agentxx-client/io/tui/agent_tui.h"

void TUILogSink::onLog(const agentxx::util::LogEntry& entry) {
    lines_.push_back(Line{entry.level, entry.message});
    while (lines_.size() > maxLines_) {
        lines_.pop_front();
    }
}

std::vector<TUILogSink::Line> TUILogSink::snapshot() const {
    return {lines_.begin(), lines_.end()};
}

void TUILogSink::clear() {
    lines_.clear();
}
