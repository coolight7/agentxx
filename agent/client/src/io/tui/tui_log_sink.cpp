#include "agentxx-client/io/tui/agent_tui.h"

void TUILogSink::onLog(agentxx::util::LogLevel level, const std::string& message) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(Line{level, message});
        while (lines_.size() > maxLines_) {
            lines_.pop_front();
        }
        cb = onNewLog_;
    }
    if (cb) {
        cb();
    }
}

std::vector<TUILogSink::Line> TUILogSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

void TUILogSink::setOnNewLog(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onNewLog_ = std::move(cb);
}

void TUILogSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}
