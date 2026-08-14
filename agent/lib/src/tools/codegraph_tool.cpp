#include "agentxx/tools/codegraph_tool.h"

#if AGENTXX_ENABLE_CODEGRAPH

#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace agentxx {
namespace tools {
// 结果类型定义于 agentxx::expand, 此处显式引用需导入 (原代码用 auto 推导)
using agentxx::expand::CodeGraphSearchResult;
using agentxx::expand::CodeGraphContextResult;
using agentxx::expand::CodeGraphStatusResult;
using agentxx::expand::CodeGraphImpactResult;
using agentxx::expand::CodeGraphPathResult;

namespace {

/// 将 codegraph 同步查询卸载到阻塞线程池执行, 并支持会话取消传播
///
/// 为什么必须卸载:
/// - codegraph 查询 (searchSymbols 等) 是同步阻塞调用, 内部持有互斥锁并遍历
///   sqlite/图数据, 可能耗时数秒; 若直接在 agent 的单线程 io_context 上执行,
///   整个事件循环被占住: transport 接收循环无法处理 WireCancel, 取消请求
///   得不到响应 (表现为"发送取消但 agent 未响应, tool 继续执行"), 还会卡死
///   所有其他会话的消息处理。
/// - 卸载到 blockingPool 后主协程挂起等待 (co_await), 事件循环保持空闲:
///   WireCancel 可被及时处理 -> token->cancel() -> 等待中的主协程被取消立即
///   返回 (结果丢弃), 不再等待查询完成。
///
/// 取消语义:
/// - 工作线程 (blockingPool) 执行前检查 cancelFlag, 已取消则抛 CancelledException
///   提前退出; 查询执行中无法抢占 (单次同步调用无轮询点), 但主线程不阻塞,
///   取消请求可被及时响应, 查询自然后台完成并丢弃结果。
template<typename R, typename F>
asio::awaitable<R> offloadCodeGraphQuery(
    const std::weak_ptr<agentxx::agent::AgentContext>& agentCtx,
    const neograph::json&                              args,
    F&&                                                fn
) {
    auto agentPtr = agentCtx.lock();
    if (!agentPtr || !agentPtr->blockingPool) {
        throw std::runtime_error{"AgentContext unavailable"};
    }
    // 会话取消令牌: 取消时 watcher 协程置位 cancelFlag, 工作线程检测后提前退出
    auto cancelToken = agentxx::tools::getSessionCancelToken(agentPtr, args);
    auto cancelFlag  = std::make_shared<std::atomic<bool>>(false);
    co_return co_await agentxx::util::offloadCancellableAsync<R>(
        *agentPtr->blockingPool,
        std::move(cancelFlag),
        std::move(cancelToken),
        std::function<asio::awaitable<R>(std::atomic<bool>&)>{std::move(fn)}
    );
}

/// 若 CodeGraph 仍在索引 (indexDirectory 生命周期内), 在返回 JSON 顶层附加
/// `"warning"` 提示 (结果可能不完整); 索引完成后输出结构与历史一致 (不附加)。
/// - object: 直接插入 "warning" 字段
/// - array (search 结果): "warning" 无法附加到数组, 包装为 {"symbols": [...], ...}
/// - 解析失败 (非 JSON/内部错误) 时原样返回, 不额外处理
static std::string addIndexingWarning(
    const std::shared_ptr<agentxx::expand::CodeGraphManager>& cg,
    std::string                                               out
) {
    if (!cg || !cg->isIndexing()) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto j = neograph::json::parse(out);
            if (j.is_array()) {
                j = neograph::json{{"symbols", std::move(j)}};
            }
            j["warning"] = "CodeGraph is still indexing, results may be incomplete";
            out         = j.dump();
            return true;
        },
        [](std::string) -> bool {
            return true;
        }
    );
    return out;
}

} // namespace

CodeGraphSearchTool::CodeGraphSearchTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_search", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "query",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("query")},
                        },
                    },
                    {
                        "limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("limit")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"query"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphSearchTool::execute_async(const neograph::json& arguments) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }
    int limit = static_cast<int>(arguments.value("limit", 20.0));

    // 同步查询卸载到 blockingPool (内部持锁+遍历 sqlite/fst, 阻塞主线程会
    // 卡死事件循环导致取消请求无法响应), 并支持会话取消传播
    auto result = co_await offloadCodeGraphQuery<CodeGraphSearchResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, query, limit](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphSearchResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph search cancelled");
            }
            co_return codegraph->searchSymbols(query, limit);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }

    auto json = neograph::json::array();
    for (const auto& node : result.nodes) {
        json.push_back({
            {"kind",           codegraph::node_kind_str(node.kind)},
            {"name",           node.name                          },
            {"qualified_name", node.qualified_name                },
            {"file",           node.file_path                     },
            {"line",           node.line                          },
            {"signature",      node.signature                     },
        });
    }
    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, json.dump());
}

CodeGraphContextTool::CodeGraphContextTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_context", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphContextTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("limit")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"symbol"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphContextTool::execute_async(const neograph::json& arguments) {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
        co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int limit     = static_cast<int>(arguments.value("limit", 10.0));
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphContextResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, symbol, limit, max_depth](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphContextResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph context cancelled");
            }
            co_return codegraph->getSymbolContext(symbol, limit, max_depth);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }

    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, result.context.dump());
}

CodeGraphCallersTool::CodeGraphCallersTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_callers", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphCallersTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"symbol"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphCallersTool::execute_async(const neograph::json& arguments) {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
        co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphImpactResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, symbol, max_depth](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphImpactResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph callers cancelled");
            }
            co_return codegraph->getCallers(symbol, max_depth);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }
    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, result.impact.dump());
}

CodeGraphCalleesTool::CodeGraphCalleesTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_callees", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphCalleesTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"symbol"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphCalleesTool::execute_async(const neograph::json& arguments) {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
        co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 3.0));

    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphImpactResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, symbol, max_depth](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphImpactResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph callees cancelled");
            }
            co_return codegraph->getCallees(symbol, max_depth);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }
    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, result.impact.dump());
}

CodeGraphImpactTool::CodeGraphImpactTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_impact", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphImpactTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "symbol",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("symbol")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"symbol"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphImpactTool::execute_async(const neograph::json& arguments) {
    std::string symbol = arguments.value("symbol", std::string{});
    if (symbol.empty()) {
        co_return R"({"error":"Arg `symbol` is empty"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 5.0));

    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphImpactResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, symbol, max_depth](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphImpactResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph impact cancelled");
            }
            co_return codegraph->getImpact(symbol, max_depth);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }
    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, result.impact.dump());
}

CodeGraphStatusTool::CodeGraphStatusTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_status", in_agentContext, false, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphStatusTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {"properties", neograph::json::object()},
                       },
    };
}

asio::awaitable<std::string> CodeGraphStatusTool::execute_async(const neograph::json& arguments) {
    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphStatusResult>(
        agentContext,
        arguments,
        [codegraph = codegraph](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphStatusResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph status cancelled");
            }
            co_return codegraph->getStatus();
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }

    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, fmt::format(
        R"({{"total_nodes":{},"total_edges":{},"total_files":{},"circular_deps":{}}})",
        result.total_nodes,
        result.total_edges,
        result.total_files,
        result.circular_deps
    ));
}

CodeGraphIndexTool::CodeGraphIndexTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_index", in_agentContext, false, false),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphIndexTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "incremental",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("incremental")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"path"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphIndexTool::execute_async(const neograph::json& arguments) {
    std::string path = arguments.value("path", std::string{});
    if (path.empty()) {
        co_return R"({"error":"Arg `path` is empty"})";
    }
    bool incremental = arguments.value("incremental", true);

    // 索引是同步阻塞操作 (遍历+解析整个目录), 卸载到线程池避免阻塞 io_context 事件循环
    // (否则大仓库索引期间所有会话的事件循环都会卡死)
    // - 支持会话取消传播: 取消时 watcher 置位 cancelFlag, 工作线程检测后抛
    //   CancelledException 提前退出, 释放线程
    auto  agentPtr    = agentContext.lock();
    auto& pool        = *agentPtr->blockingPool;
    auto  cancelToken = agentxx::tools::getSessionCancelToken(agentPtr, arguments);
    auto  cancelFlag  = std::make_shared<std::atomic<bool>>(false);
    bool  ok          = co_await agentxx::util::offloadCancellableAsync<bool>(
        pool,
        cancelFlag,
        cancelToken,
        [codegraph = codegraph, path, incremental](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<bool> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph index cancelled");
            }
            co_return codegraph->indexDirectory(path, incremental);
        }
    );
    // 状态查询同样卸载到阻塞线程池: getStatus 内部持锁, 索引线程 (blockingPool)
    // 仍持有 mutex_ 时直接查询会阻塞主 io_context 事件循环
    auto status = co_await offloadCodeGraphQuery<CodeGraphStatusResult>(
        agentContext,
        arguments,
        [codegraph = codegraph](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphStatusResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph status cancelled");
            }
            co_return codegraph->getStatus();
        }
    );

    if (ok) {
        if (status.success) {
            co_return fmt::format(
                R"({{"success":true,"total_nodes":{},"total_edges":{},"total_files":{}}})",
                status.total_nodes,
                status.total_edges,
                status.total_files
            );
        }
        // 索引成功但状态查询失败 (罕见): 返回具体原因
        co_return neograph::json{
            {"error", fmt::format("Indexing done, but status query failed: {}", status.error)},
        }
            .dump();
    }
    // 索引失败: 返回具体原因 (如未初始化), 便于 LLM 诊断
    if (false == status.success && false == status.error.empty()) {
        co_return neograph::json{
            {"error", fmt::format("Indexing failed: {}", status.error)},
        }
            .dump();
    }
    co_return R"({"error":"Indexing failed"})";
}

CodeGraphPathTool::CodeGraphPathTool(
    std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
    std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
) :
    XXToolBase("agentxx_codegraph_path", in_agentContext, true, true),
    codegraph(std::move(in_codegraph)) {}

neograph::ChatTool CodeGraphPathTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "from",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("from")},
                        },
                    },
                    {
                        "to",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("to")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"from", "to"})},
                       },
    };
}

asio::awaitable<std::string> CodeGraphPathTool::execute_async(const neograph::json& arguments) {
    std::string from = arguments.value("from", std::string{});
    std::string to   = arguments.value("to", std::string{});
    if (from.empty() || to.empty()) {
        co_return R"({"error":"Args `from` and `to` are required"})";
    }
    int max_depth = static_cast<int>(arguments.value("max_depth", 10.0));

    // 同步查询卸载到 blockingPool, 支持会话取消传播 (同上)
    auto result = co_await offloadCodeGraphQuery<CodeGraphPathResult>(
        agentContext,
        arguments,
        [codegraph = codegraph, from, to, max_depth](
            std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<CodeGraphPathResult> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("codegraph path cancelled");
            }
            co_return codegraph->findPath(from, to, max_depth);
        }
    );
    if (!result.success) {
        co_return neograph::json{
            {"error", result.error}
        }.dump();
    }

    auto json = neograph::json::array();
    for (const auto& node : result.path) {
        json.push_back({
            {"kind", codegraph::node_kind_str(node.kind)},
            {"name", node.name                          },
            {"file", node.file_path                     },
            {"line", node.line                          },
        });
    }
    // 索引进行中时附加"结果可能不完整"提示 (见 addIndexingWarning)
    co_return addIndexingWarning(codegraph, fmt::format(R"({{"path":{},"depth":{}}})", json.dump(), result.path.size()));
}

} // namespace tools
} // namespace agentxx

#endif
