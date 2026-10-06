#include "agentxx/util/diagnostics.h"

#include "agentxx/agent/assembly_snapshot.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/util/observability.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/task_scope.h"
#include "agentxx/version.h"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/system.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

namespace agentxx {
namespace util {

namespace {

// ---------------------------------------------------------------------------
// 日志捕获 (进程级环形缓冲)
// ---------------------------------------------------------------------------

/// 只保留最近 [capacity] 行: 诊断包要的是"刚刚发生了什么", 不是全量日志
/// - 用 [ThreadedLogSink] (自带后台线程 pump): 普通 LogSink 需要宿主线程主动
///   pump, 而诊断包是"随时可能被调用"的只读导出, 不该要求调用方先驱动日志管线;
/// - [recentLogLines] 会先 flush 再取快照, 保证刚写入的日志能出现在诊断包里
class CaptureLogSink : public utilxx_base::ThreadedLogSink {
public:

    explicit CaptureLogSink(size_t capacity) :
        capacity_(capacity == 0 ? 1 : capacity) {}

    ~CaptureLogSink() override {
        // 在虚表仍为本类时停止后台线程 (基类析构期间回调已不可用)
        shutdownThread();
    }

    /// 后台线程串行调用 (队列由基类维护, 这里只做环形缓冲)
    void onLog(const utilxx_base::LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.size() >= capacity_) {
            lines_.pop_front();
        }
        lines_.push_back(formatLine(entry.level, entry.message));
    }

    std::vector<std::string> snapshot(size_t limit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t                count = (limit == 0 || limit > lines_.size()) ? lines_.size()
                                                                                  : limit;
        const size_t                begin = lines_.size() - count;
        return {lines_.begin() + static_cast<std::ptrdiff_t>(begin), lines_.end()};
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

    void setCapacity(size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = (capacity == 0) ? 1 : capacity;
        while (lines_.size() > capacity_) {
            lines_.pop_front();
        }
    }

    static std::string formatLine(const utilxx_base::LogLevel level, const std::string& message) {
        const char* tag = "?";
        switch (level) {
            case utilxx_base::LogLevel::Trace:
                tag = "T";
                break;
            case utilxx_base::LogLevel::Debug:
                tag = "D";
                break;
            case utilxx_base::LogLevel::Info:
                tag = "I";
                break;
            case utilxx_base::LogLevel::Warn:
                tag = "W";
                break;
            case utilxx_base::LogLevel::Error:
                tag = "E";
                break;
            case utilxx_base::LogLevel::Out:
                tag = "O";
                break;
        }
        return fmt::format("[{}] {}", tag, message);
    }

private:

    mutable std::mutex       mutex_;
    std::deque<std::string>  lines_;
    size_t                   capacity_;
};

/// 进程级捕获 sink (首次 [enableLogCapture] 时安装)
std::mutex                        gCaptureMutex;
std::shared_ptr<CaptureLogSink>   gCaptureSink;
bool                              gCaptureInstalled = false;

// ---------------------------------------------------------------------------
// 会话段
// ---------------------------------------------------------------------------

std::string humanBytes(uint64_t bytes) {
    if (bytes >= 1024 * 1024) {
        return fmt::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024) {
        return fmt::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return fmt::format("{} B", bytes);
}

} // namespace

// ---------------------------------------------------------------------------
// 日志捕获
// ---------------------------------------------------------------------------

void enableLogCapture(const size_t capacity) {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (gCaptureSink) {
        gCaptureSink->setCapacity(capacity);
        return;
    }
    gCaptureSink = std::make_shared<CaptureLogSink>(capacity);
    utilxx_base::LogDispatcher::instance().addSink(gCaptureSink);
    gCaptureInstalled = true;
}

void disableLogCapture() {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (!gCaptureSink) {
        return;
    }
    if (gCaptureInstalled) {
        utilxx_base::LogDispatcher::instance().removeSink(gCaptureSink);
        gCaptureInstalled = false;
    }
    gCaptureSink.reset();
}

std::vector<std::string> recentLogLines(const size_t limit) {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (!gCaptureSink) {
        return {};
    }
    // 等后台线程把已入队的日志处理完 (调用方不必知道日志管线的驱动方式)
    gCaptureSink->flush();
    return gCaptureSink->snapshot(limit);
}

void clearCapturedLogs() {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (gCaptureSink) {
        gCaptureSink->clear();
    }
}

// ---------------------------------------------------------------------------
// 凭据屏蔽
// ---------------------------------------------------------------------------

std::string redactSecrets(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::string out{text};

    // ① `key = value` / `key: "value"` 形态: 关键字命中时把值整体替换为 ***
    //    (值可能带引号; 关键字不区分大小写)
    static const std::regex kKeyValue{
        R"re(((?:api[_-]?key|apikey|access[_-]?token|refresh[_-]?token|auth[_-]?token|token|secret|password|passwd|authorization|bearer|credential)["']?\s*[:=]\s*["']?)((?:(?:bearer|basic|token)\s+)?[^\s"',;)&]+))re",
        std::regex::icase
    };
    out = std::regex_replace(out, kKeyValue, "$1***");

    // ② `sk-xxxx` / `ghp_xxxx` / `github_pat_xxxx` 之类的裸 token
    static const std::regex kBareToken{R"re(\b(sk-[A-Za-z0-9_\-]{8,}|gh[pousr]_[A-Za-z0-9]{16,}|github_pat_[A-Za-z0-9_]{16,}))re"};
    out = std::regex_replace(out, kBareToken, "***");

    // ③ URL 里的 userinfo (https://user:pass@host) 与查询参数里的 key/token
    static const std::regex kUrlUserInfo{R"re((://[^/\s:@]+:)[^@\s/]+(@))re"};
    out = std::regex_replace(out, kUrlUserInfo, "$1***$2");
    static const std::regex kUrlQuery{R"re(([?&](?:api_?key|access_?token|token|key|secret)=)[^&\s]+)re"};
    out  = std::regex_replace(out, kUrlQuery, "$1***");

    return out;
}

// ---------------------------------------------------------------------------
// 诊断包
// ---------------------------------------------------------------------------

namespace {

/// 会话段: 计数/用量/存储体积 (默认不含消息正文)
std::vector<std::string> sessionSection(
    agent::AgentContext&         ctx,
    const std::string_view       sessionId,
    const DiagnosticsOptions&    options
) {
    std::vector<std::string> lines;
    if (sessionId.empty()) {
        return lines;
    }
    lines.push_back(fmt::format("- session id: {}", sessionId));

    // 内存中的会话 (若本进程持有)
    if (ctx.sessions) {
        auto session = ctx.sessions->get(std::string{sessionId});
        if (session) {
            lines.push_back(fmt::format("- loaded in memory: yes"));
            lines.push_back(fmt::format("- llm messages: {}", session->messagesCount()));
            lines.push_back(fmt::format("- view messages: {}", session->viewMessageCount()));
            lines.push_back(fmt::format("- last view seq: {}", session->lastViewSeq()));
        } else {
            lines.push_back("- loaded in memory: no (not opened in this process)");
        }
    }

    const auto store = (ctx.sessions && ctx.sessions->sessionStore)
                           ? ctx.sessions->sessionStore
                           : nullptr;
    if (!store) {
        lines.push_back("- persistence: disabled (in-memory only)");
        return lines;
    }

    const auto title = store->sessionTitle(std::string{sessionId});
    lines.push_back(fmt::format("- title: {}", title.empty() ? "(none)" : title));
    lines.push_back(fmt::format("- schema version: v{}", agent::SessionStore::schemaVersion()));

    const auto usage = store->usageSummary(std::string{sessionId});
    lines.push_back(fmt::format(
        "- usage ledger: calls={} failed={} prompt={} completion={} total={} cached={} reasoning={}",
        usage.calls,
        usage.failedCalls,
        usage.promptTokens,
        usage.completionTokens,
        usage.totalTokens,
        usage.cachedPromptTokens,
        usage.reasoningTokens
    ));

    // 存储体积 (会话库文件大小; 取不到就跳过)
    const auto loaded = store->loadSession(std::string{sessionId});
    lines.push_back(fmt::format("- persisted view messages: {}", loaded.viewMessages.size()));
    lines.push_back(fmt::format(
        "- persisted llm messages: {}",
        loaded.llmMessages.is_array() ? loaded.llmMessages.size() : 0
    ));
    lines.push_back(fmt::format("- share store last id: {}", store->shareStoreLastId(std::string{sessionId})));

    if (options.includeMessages) {
        const auto rows = store->loadViewMessagesAfter(std::string{sessionId}, 0, 50);
        lines.push_back(fmt::format("- recent view messages (up to 50, truncated):"));
        for (const auto& row : rows) {
            std::string text = row.message.text;
            if (text.size() > options.maxMessageChars) {
                text.resize(options.maxMessageChars);
                text += "...";
            }
            // 消息正文也可能被用户粘进凭据, 一并屏蔽
            lines.push_back(fmt::format(
                "    - #{} [{}] {}",
                row.seq,
                row.message.id,
                redactSecrets(text)
            ));
        }
    }
    return lines;
}

} // namespace

std::string buildDiagnosticsText(
    agent::AgentContext&         ctx,
    const std::string_view       sessionId,
    const DiagnosticsOptions&    options
) {
    std::vector<std::string> lines;
    lines.push_back("# agentxx diagnostics");
    lines.push_back("");
    {
        // 本地时间字符串 (strftime; 不用 fmt 的时间格式化, 避免依赖 fmt 的 chrono 支持)
        const auto  now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm     tmBuf{};
#if XX_IS_WIN_D
        localtime_s(&tmBuf, &now);
#else
        localtime_r(&now, &tmBuf);
#endif
        char buf[32] = {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
        lines.push_back(fmt::format("- time: {}", buf));
    }
    lines.push_back(fmt::format("- version: {}", agentxx::kVersion));
    lines.push_back(fmt::format(
        "- platform: {} / {} ({} cores)",
        XX_IS_WIN_D ? "windows" : (XX_IS_MACOS_D ? "macos" : "linux"),
        utilxx_base::getSystemName(),
        std::thread::hardware_concurrency()
    ));

    // ---- 指标 (本进程累计; 无指标对象时跳过) ----
    if (ctx.metrics) {
        lines.push_back("");
        lines.push_back("## metrics (this process)");
        lines.push_back(fmt::format("- {}", ctx.metrics->summary()));
        lines.push_back(fmt::format("- json: {}", ctx.metrics->toJson().dump()));
    }

    // ---- 装配 (配置 + 运行侧: 已脱敏快照, 与 --dump-config 同源) ----
    if (ctx.agentConfig) {
        lines.push_back("");
        lines.push_back("## assembly");
        const auto configSnapshot = agent::buildConfigSnapshot(*ctx.agentConfig);
        auto       snapshot       = agent::mergeAssemblySnapshot(
            configSnapshot,
            agent::buildRuntimeSnapshot(ctx)
        );
        for (const auto& line : agent::renderAssemblySnapshot(snapshot)) {
            lines.push_back(fmt::format("- {}", line));
        }
        // 配置侧快照 JSON: 报障时最需要"当时的配置形状" (键路径/取值)。
        // 该快照按设计不含凭据取值 (只输出 api_key_set 等布尔/键名), 因此可以随包导出。
        lines.push_back(fmt::format("- config_json: {}", configSnapshot.dump()));
    }

    // ---- 会话 ----
    {
        auto section = sessionSection(ctx, sessionId, options);
        if (!section.empty()) {
            lines.push_back("");
            lines.push_back("## session");
            for (auto& line : section) {
                lines.push_back(std::move(line));
            }
        }
    }

    // ---- 后台任务 (关闭/收敛诊断) ----
    if (ctx.taskScope) {
        lines.push_back("");
        lines.push_back("## background tasks");
        lines.push_back(fmt::format(
            "- pending={} total_spawned={}",
            ctx.taskScope->pending(),
            ctx.taskScope->totalSpawned()
        ));
        for (const auto& name : ctx.taskScope->pendingNames()) {
            lines.push_back(fmt::format("    - {}", name));
        }
    }

    // ---- 插件逐项 (detailedLists 关闭时只给计数, 已在 assembly 段里) ----
    if (options.detailedLists && ctx.pluginManager) {
        lines.push_back("");
        lines.push_back("## plugins");
        for (const auto& view : ctx.pluginManager->list()) {
            const auto instance = ctx.pluginManager->find(view.name);
            if (!instance) {
                continue;
            }
            const auto inv = ctx.pluginManager->registrationInventory(*instance);
            lines.push_back(fmt::format(
                "- {} v{} [{}] tools={} hooks={} caps={} events={} prompt_keys={} resources="
                "skill:{} mem:{} mcp:{} graph_owner={}",
                view.name,
                view.version,
                view.enabled ? "enabled" : "disabled",
                inv.tools,
                inv.hooks,
                inv.capabilities,
                inv.eventSubscriptions,
                inv.promptKeys,
                inv.skillDirs,
                inv.memoryFiles,
                inv.mcpNamespaces,
                inv.ownsGraphDefinition ? "yes" : "no"
            ));
        }
        if (ctx.pluginManager->graphDefinitionOwner().empty()) {
            lines.push_back("- graph definition: built-in");
        } else {
            lines.push_back(fmt::format(
                "- graph definition: plugin `{}`",
                ctx.pluginManager->graphDefinitionOwner()
            ));
        }
    }

    // ---- 日志尾部 (屏蔽凭据) ----
    if (options.logTailLines > 0) {
        const auto logs = recentLogLines(options.logTailLines);
        lines.push_back("");
        lines.push_back(fmt::format("## log tail (last {} lines)", logs.size()));
        if (logs.empty()) {
            lines.push_back("- (log capture is not enabled; call enableLogCapture() first)");
        }
        for (const auto& entry : logs) {
            lines.push_back(redactSecrets(entry));
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << "\n";
        }
    }
    return out.str();
}

} // namespace util
} // namespace agentxx
