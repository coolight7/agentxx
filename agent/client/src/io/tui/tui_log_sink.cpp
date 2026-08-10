#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"

void TUILogSink::onLog(const agentxx::util::LogEntry& entry) {
    // 日志等级过滤: 低于当前设置的日志等级不入列 (设置弹窗可调整)。
    // Out (stdout 输出类) 恒显示 —— 它是模型输出/工具结果等重要内容,
    // 不受日志等级过滤影响。切换等级后已收集的历史行由外部 (设置弹窗
    // onLogLevelChange 回调) 调用 clear() 清空, 重新按新等级收集。
    const auto minLevel = TUISettings::instance().logLevel();
    if (entry.level != agentxx::util::LogLevel::Out && entry.level < minLevel) {
        return;
    }
    lines_.push_back(Line{entry.level, entry.message});
    while (lines_.size() > maxLines_) {
        lines_.pop_front();
        ++poppedCount_;
    }
}

std::vector<TUILogSink::Line> TUILogSink::snapshot() const {
    return {lines_.begin(), lines_.end()};
}

void TUILogSink::clear() {
    lines_.clear();
}
