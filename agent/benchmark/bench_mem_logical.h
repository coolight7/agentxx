#pragma once

// 资源基准测试的"逻辑内存"统计:
// 统计进程内各模块**数据结构自身**占用 (消息容器 / 中间件状态 / 工具注册表 /
// TUI 渲染状态 / 持久化文件大小), 与 bench_mem_probe.h 的操作系统级采样相互补充:
// - OS 级 (RSS/PSS/smaps): 反映真实物理内存, 但无法区分是哪个模块的数据
// - 逻辑级 (本文件): 精确到模块的字节数, 但只是数据本身的字节 (不含分配器开销/碎片)
//
// 两者一起看即可回答"内存花在哪里": 逻辑占比高 → 优化数据结构; 逻辑占比低但 RSS
// 高 → 优化分配器使用 (碎片 / 未归还 / 缓存过多)。

#include "bench_mem_probe.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "fmt/format.h"
#include "neograph/types.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace bench {

struct LogicalMemReport {
    std::vector<LogicalMemRow> rows;

    void add(LogicalMemRow row) {
        rows.push_back(std::move(row));
    }

    void addRows(const std::vector<LogicalMemRow>& more) {
        for (const auto& r : more) {
            rows.push_back(r);
        }
    }

    size_t totalBytes() const {
        size_t total = 0;
        for (const auto& r : rows) {
            if (r.name.rfind("__", 0) == 0) {
                continue; // __ 前缀行不参与合计 (如磁盘文件大小)
            }
            total += r.bytes;
        }
        return total;
    }

    void sortByBytesDesc() {
        std::sort(rows.begin(), rows.end(), [](const LogicalMemRow& a, const LogicalMemRow& b) {
            return a.bytes > b.bytes;
        });
    }

    void printTable(const std::string& title, std::ostream& os = std::cout) const {
        if (rows.empty()) {
            return;
        }
        os << fmt::format(
            "  [{}] 逻辑内存合计 {:.2f} MB ({} 项)\n",
            title,
            static_cast<double>(totalBytes()) / (1024.0 * 1024.0),
            rows.size()
        );
        os << fmt::format("    {:<40} {:>12} {:>10} {}\n", "模块", "字节", "条目", "说明");
        for (const auto& r : rows) {
            if (r.bytes == 0 && r.count == 0) {
                continue;
            }
            std::string name = r.name;
            if (name.size() > 40) {
                name = name.substr(0, 37) + "...";
            }
            os << fmt::format(
                "    {:<40} {:>12} {:>10} {}\n",
                name,
                r.bytes,
                r.count,
                r.note
            );
        }
    }
};

// ---------------------------------------------------------------------------
// 消息字节估算
// ---------------------------------------------------------------------------

/// 单条展示消息的字节估算: 各字符串字段长度之和 + 对象自身开销
/// (不调用 toJson().dump(), 避免统计本身产生大量临时分配而干扰 RSS 采样)
inline size_t estimateViewMessageBytes(const agentxx::agent::ViewMessage& msg) {
    size_t bytes = sizeof(agentxx::agent::ViewMessage) + msg.id.size() + msg.text.size();
    if (msg.tool) {
        bytes += msg.tool->toolName.size() + msg.tool->toolCallId.size()
                 + msg.tool->toolResult.size() + msg.tool->diff.size()
                 + sizeof(agentxx::agent::ViewMessage::ToolData);
    }
    if (msg.interrupt) {
        bytes += msg.interrupt->interruptResult.size()
                 + sizeof(agentxx::agent::ViewMessage::InterruptData);
        if (!msg.interrupt->ui.is_null()) {
            bytes += msg.interrupt->ui.dump().size();
        }
    }
    if (msg.think) {
        bytes += sizeof(agentxx::agent::ViewMessage::ThinkData);
    }
    return bytes;
}

inline size_t estimateViewMessagesBytes(const std::vector<agentxx::agent::ViewMessage>& msgs) {
    size_t total = 0;
    for (const auto& m : msgs) {
        total += estimateViewMessageBytes(m);
    }
    return total;
}

// ---------------------------------------------------------------------------
// agent 侧逻辑内存
// ---------------------------------------------------------------------------

struct AgentLogicalOptions {
    bool        includeToolSchema = true; ///< 统计工具 schema 大小 (需构建临时 ChatTool 列表)
    /// 需要统计的会话 id 列表;
    /// - 非空: 仅统计这些会话 (SessionsManager 只提供按 id 取用, 无枚举接口)
    /// - 空: 跳过会话级统计, 只统计与全局对象相关的项
    std::vector<std::string> sessionIds;
};

/// 收集 agent 上下文各容器的逻辑内存
inline std::vector<LogicalMemRow> collectAgentLogicalMem(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const AgentLogicalOptions&                          opt = {}
) {
    std::vector<LogicalMemRow> rows;
    if (!ctx) {
        return rows;
    }

    // ---- 会话消息容器 ----
    if (ctx->sessions) {
        size_t totalViewBytes = 0, totalViewCount = 0;
        size_t totalLlmBytes = 0, totalLlmCount = 0;
        for (const auto& sessionId : opt.sessionIds) {
            auto sess = ctx->sessions->get(sessionId);
            if (!sess) {
                continue;
            }
            totalViewBytes += estimateViewMessagesBytes(sess->viewMessages);
            totalViewCount += sess->viewMessages.size();
            totalLlmBytes += sess->llmMessages.is_null() ? 0 : sess->llmMessages.dump().size();
            totalLlmCount += sess->llmMessages.is_array() ? sess->llmMessages.size() : 0;
        }
        rows.push_back(
            {"agent.session.view_messages",
             totalViewBytes,
             totalViewCount,
             "展示历史 (字段长度求和 + 对象开销)"}
        );
        rows.push_back(
            {"agent.session.llm_messages",
             totalLlmBytes,
             totalLlmCount,
             "LLM 上下文 (JSON dump 字节)"}
        );
        // msgIndex_ 为 map<string,size_t>, 每条约 80B (键 + 节点开销)
        rows.push_back(
            {"agent.session.msg_index",
             totalViewCount * 80,
             totalViewCount,
             "msgId→下标索引估算 80B/条"}
        );
    }

    // ---- 中间件状态 (share store / graph data) ----
    if (ctx->middlewareHandleContext) {
        size_t shareBytes = 0, shareCount = 0;
        for (const auto& kv : ctx->middlewareHandleContext->shareStore) {
            if (!opt.sessionIds.empty()
                && std::find(opt.sessionIds.begin(), opt.sessionIds.end(), kv.first)
                       == opt.sessionIds.end()) {
                continue;
            }
            shareBytes += kv.first.size() + 64; // 会话键 + map 节点开销
            for (const auto& item : kv.second.store) {
                shareBytes += item.second.size() + 48;
                ++shareCount;
            }
        }
        rows.push_back(
            {"agent.middleware.share_store",
             shareBytes,
             shareCount,
             "share_store 内容 + 键/节点开销"}
        );
        size_t graphSessionCount = ctx->middlewareHandleContext->graphData.size();
        rows.push_back(
            {"agent.middleware.graph_data",
             0,
             graphSessionCount,
             "graphData 为 std::any, 仅统计会话数"}
        );
    }

    // ---- 插件与工具注册表 ----
    if (ctx->pluginManager) {
        size_t pluginBytes = 0;
        auto   plugins     = ctx->pluginManager->list();
        for (const auto& p : plugins) {
            pluginBytes += p.name.size() + p.version.size() + p.description.size() + p.path.size()
                           + p.configPath.size();
            for (const auto& t : p.tools) {
                pluginBytes += t.size() + 64;
            }
            for (const auto& c : p.capabilities) {
                pluginBytes += c.size() + 32;
            }
        }
        rows.push_back(
            {"agent.plugins.registry",
             pluginBytes,
             plugins.size(),
             "插件名/路径/工具名注册信息"}
        );
        if (ctx->toolRegistry) {
            size_t      schemaBytes = 0;
            std::string namesConcat;
            for (const auto& n : ctx->toolRegistry->names()) {
                namesConcat += n;
            }
            if (opt.includeToolSchema) {
                std::vector<neograph::ChatTool> defs;
                ctx->toolRegistry->appendDefinitions(defs);
                for (const auto& d : defs) {
                    schemaBytes += d.name.size() + d.description.size()
                                   + (d.parameters.is_null() ? 0 : d.parameters.dump().size());
                }
            }
            rows.push_back(
                {"agent.plugins.tool_schema",
                 schemaBytes + namesConcat.size(),
                 ctx->toolRegistry->size(),
                 "工具 name/description/parameters JSON (每轮调用 LLM 都会发送)"}
            );
        }
    }
    rows.push_back(
        {"agent.graph.definition",
         ctx->graphDefinitionJson.is_null() ? 0 : ctx->graphDefinitionJson.dump().size(),
         1,
         "执行图 JSON 定义"}
    );
    rows.push_back(
        {"agent.tool_names",
         [&] {
             size_t n = 0;
             for (const auto& t : ctx->toolNames) {
                 n += t.size() + 32;
             }
             return n;
         }(),
         ctx->toolNames.size(),
         "本 agent 工具名列表"}
    );

    // ---- 启动加载的组件信息 ----
    {
        const auto& info = ctx->appendComponentInfo;
        size_t      bytes = 0;
        for (const auto& s : info.mcpTools) {
            bytes += s.size() + 32;
        }
        for (const auto& s : info.skills) {
            bytes += s.size() + 32;
        }
        for (const auto& s : info.memoryFiles) {
            bytes += s.size() + 32;
        }
        for (const auto& f : info.failedComponents) {
            bytes += f.name.size() + f.errorMessage.size() + 64;
        }
        rows.push_back(
            {"agent.components.info",
             bytes,
             info.mcpTools.size() + info.skills.size() + info.memoryFiles.size()
                 + info.failedComponents.size(),
             "MCP/Skill/Memory 加载信息"}
        );
    }

    // ---- 配置文本 (系统提示词 / 模型配置等) ----
    if (ctx->agentConfig) {
        size_t bytes = 0;
        if (auto promptJson = ctx->agentConfig->prompt.toJson(); !promptJson.is_null()) {
            bytes += promptJson.dump().size();
        }
        for (const auto& kv : ctx->agentConfig->availableModels) {
            bytes += kv.first.size() + kv.second.baseUrl.size() + kv.second.modelName.size()
                     + kv.second.apiKey.size() + 128;
        }
        rows.push_back(
            {"agent.config",
             bytes,
             ctx->agentConfig->availableModels.size(),
             "系统提示词/工具提示词 JSON + 模型配置"}
        );
    }

    LogicalMemReport report;
    report.addRows(rows);
    report.sortByBytesDesc();
    return report.rows;
}

// ---------------------------------------------------------------------------
// 客户端 TUI 逻辑内存 (模板化以避免在非 client 目标里引入 FTXUI 依赖)
// ---------------------------------------------------------------------------

template<typename TuiRenderStateT>
inline std::vector<LogicalMemRow> collectTuiLogicalMem(const TuiRenderStateT& st) {
    std::vector<LogicalMemRow> rows;
    size_t                     msgBytes = 0;
    for (const auto& m : st.messages) {
        if (m) {
            msgBytes += estimateViewMessageBytes(*m) + sizeof(std::shared_ptr<TuiRenderStateT>);
        }
    }
    rows.push_back(
        {"client.tui.messages", msgBytes, st.messages.size(), "客户端已加载的展示消息 (shared_ptr 持有)"}
    );
    rows.push_back(
        {"client.tui.stream_token",
         st.currentToken ? st.currentToken->size() : 0,
         st.currentToken ? size_t{1} : size_t{0},
         "流式累积 token 文本"}
    );
    rows.push_back(
        {"client.tui.context_snapshot",
         st.contextMessages ? st.contextMessages->dump().size() : 0,
         st.contextMessages ? size_t{1} : size_t{0},
         "上下文弹窗快照 (整份 JSON 拷贝)"}
    );
    size_t sessionListBytes = 0;
    for (const auto& s : st.sessionList) {
        sessionListBytes += s.sessionId.size() + s.title.size() + 32;
    }
    rows.push_back(
        {"client.tui.session_list",
         sessionListBytes,
         st.sessionList.size(),
         "会话选择弹窗数据"}
    );
    size_t componentBytes = 0;
    for (const auto& c : st.appendComponents) {
        componentBytes += c.name.size() + c.errorMessage.size() + 48;
    }
    rows.push_back(
        {"client.tui.append_components",
         componentBytes,
         st.appendComponents.size(),
         "MCP/Skill/Memory 加载信息"}
    );
    size_t modelBytes = 0;
    for (const auto& kv : st.modelCapabilities) {
        modelBytes += kv.first.size() + 128;
    }
    modelBytes += st.cachedModelName.size() + st.pendingModel.size() + st.startupProgress.size();
    rows.push_back({"client.tui.model_info", modelBytes, st.modelCapabilities.size(), "模型列表与能力"});
    size_t pendingBytes = 0;
    for (const auto& p : st.pendingInputs) {
        pendingBytes += p.text.size() + p.model.size() + sizeof(p);
    }
    rows.push_back(
        {"client.tui.pending_inputs",
         pendingBytes,
         st.pendingInputs.size(),
         "待发送输入队列"}
    );

    LogicalMemReport report;
    report.addRows(rows);
    report.sortByBytesDesc();
    return report.rows;
}

// ---------------------------------------------------------------------------
// 持久化文件 (磁盘占用; 名称以 __ 开头表示不计入逻辑内存合计)
// ---------------------------------------------------------------------------

inline size_t directorySizeBytes(const std::string& dir, size_t* fileCount = nullptr) {
    namespace fs = std::filesystem;
    size_t total = 0;
    size_t count = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        total += static_cast<size_t>(it->file_size(ec));
        ++count;
    }
    if (fileCount != nullptr) {
        *fileCount = count;
    }
    return total;
}

inline std::vector<LogicalMemRow> collectStoreLogicalMem(
    const std::string& dataDir,
    std::string_view   sessionId = {}
) {
    std::vector<LogicalMemRow> rows;
    if (dataDir.empty()) {
        return rows;
    }
    namespace fs      = std::filesystem;
    std::string base  = dataDir;
    std::string sdir  = base + "/sqlite/sessions";
    if (!sessionId.empty()) {
        auto specific = fs::path(sdir) / std::string(sessionId);
        size_t files  = 0;
        auto   bytes  = directorySizeBytes(specific.string(), &files);
        rows.push_back({"__store.session_db", bytes, files, "会话持久化文件 (磁盘, 不计入合计)"});
    } else {
        size_t files = 0;
        auto   bytes = directorySizeBytes(sdir, &files);
        rows.push_back({"__store.sessions", bytes, files, "全部会话库文件 (磁盘)"});
    }
    {
        size_t files = 0;
        auto   bytes = directorySizeBytes(base + "/sqlite/codegraph", &files);
        if (bytes > 0 || files > 0) {
            rows.push_back({"__store.codegraph_index", bytes, files, "CodeGraph 索引库 (磁盘)"});
        }
    }
    {
        std::error_code ec;
        auto            p = fs::path(base) / "sqlite" / "global.db";
        if (fs::exists(p, ec)) {
            rows.push_back({"__store.global_db", static_cast<size_t>(fs::file_size(p, ec)), 1, "全局设置库 (磁盘)"});
        }
    }
    return rows;
}

} // namespace bench
} // namespace agentxx
