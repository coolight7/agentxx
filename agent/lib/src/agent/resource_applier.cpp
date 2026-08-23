/*
 * resource_applier.cpp —— 宿主会话组件资源应用器实现
 * 见 resource_applier.h (单一具体实现, 无接口/实现分层)
 */
#include "agentxx/agent/resource_applier.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/this_coro.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <filesystem>

namespace agentxx {
namespace agent {

namespace {

/// 简单包含判断 (componentInfo 去重用)
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

AgentResourceApplier::AgentResourceApplier(
    std::weak_ptr<AgentContext>                             in_agentContext,
    asio::any_io_executor                                   in_ioExecutor,
    std::shared_ptr<middleware::SkillMiddlewareHandle>      in_skillMiddleware,
    std::shared_ptr<middleware::MemoryFileMiddlewareHandle> in_memoryMiddleware
) :
    agentContext_(std::move(in_agentContext)),
    ioExecutor_(std::move(in_ioExecutor)),
    skillMiddleware_(std::move(in_skillMiddleware)),
    memoryMiddleware_(std::move(in_memoryMiddleware)) {}

std::string AgentResourceApplier::normalizePath(std::string_view p) {
    namespace fs = std::filesystem;
    if (p.empty()) {
        return {};
    }
    std::error_code ec;
    auto abs = fs::absolute(fs::path(p), ec);
    if (ec) {
        return std::string{p};
    }
    auto canon = fs::weakly_canonical(abs, ec);
    if (ec) {
        return abs.lexically_normal().string();
    }
    return canon.string();
}

void AgentResourceApplier::applyDecls(const std::string& owner, const PluginResourceDecls& decls) {
    for (const auto& dir : decls.skillDirs) {
        std::string err;
        addSkillDirImpl(owner, normalizePath(dir), err);
    }
    for (const auto& file : decls.memoryFiles) {
        std::string err;
        addMemoryFileImpl(owner, normalizePath(file), err);
    }
    for (const auto& [ns, cfg] : decls.mcpServers) {
        std::string err;
        addMcpServer(owner, ns, cfg, err);
    }
}

bool AgentResourceApplier::addSkillDir(
    const std::string& owner,
    const std::string& absPath,
    std::string&       errOut
) {
    return addSkillDirImpl(owner, normalizePath(absPath), errOut);
}

bool AgentResourceApplier::addSkillDirImpl(
    const std::string& owner,
    std::string        normPath,
    std::string&       errOut
) {
    if (normPath.empty()) {
        errOut = "empty path";
        return false;
    }
    auto ctx = agentContext_.lock();

    // 冲突规则: 主程序 yaml 配置优先 (跳过 + WARN); 插件之间先到先得
    auto conflictsWithYaml = [&]() -> bool {
        if (!ctx || !ctx->agentConfig) {
            return false;
        }
        const auto& list = ctx->agentConfig->skillDirPaths;
        return std::any_of(list.begin(), list.end(), [&](const auto& p) {
            return normalizePath(p) == normPath;
        });
    }();
    if (conflictsWithYaml) {
        errOut = fmt::format("skill dir `{}` conflicts with main yaml config", normPath);
        XX_LOGW("[Resources] Plugin `{}` {}: skipped (yaml priority)", owner, errOut);
        return false;
    }

    // 其他插件已注册同一目录 → 先到先得
    for (const auto& [otherOwner, rec] : owned_) {
        if (otherOwner == owner) {
            continue;
        }
        if (std::find(rec.applied.skillDirs.begin(), rec.applied.skillDirs.end(), normPath)
            != rec.applied.skillDirs.end()) {
            errOut = fmt::format("skill dir `{}` already owned by plugin `{}`", normPath, otherOwner);
            XX_LOGW("[Resources] Plugin `{}` {}: skipped", owner, errOut);
            return false;
        }
    }

    // 注意: 不因"所有权记录已存在"而短路 —— 记录 ≠ 生效 (disable 后 enable
    // 恢复场景记录仍在但未挂载); 生效侧由中间件自身去重, 此处始终确保挂载
    if (skillMiddleware_) {
        skillMiddleware_->addSkillDirs({normPath});
    }
    auto& rec = owned_[owner];
    if (std::find(rec.applied.skillDirs.begin(), rec.applied.skillDirs.end(), normPath)
        == rec.applied.skillDirs.end()) {
        rec.applied.skillDirs.push_back(normPath);
    }
    if (ctx) {
        auto& list = ctx->appendComponentInfo.skills;
        if (!contains(list, normPath)) {
            list.push_back(normPath);
        }
    }
    return true;
}

bool AgentResourceApplier::removeSkillDir(const std::string& owner, const std::string& absPath) {
    auto norm = normalizePath(absPath);
    auto it   = owned_.find(owner);
    if (it == owned_.end()
        || std::find(it->second.applied.skillDirs.begin(), it->second.applied.skillDirs.end(), norm)
            == it->second.applied.skillDirs.end()) {
        XX_LOGW(
            "[Resources] Plugin `{}` remove skill dir `{}` failed (not owned)",
            owner,
            norm
        );
        return false;
    }
    deactivateSkill(owner, norm, /*keepOwned=*/false);
    return true;
}

void AgentResourceApplier::deactivateSkill(
    const std::string& owner,
    const std::string& normPath,
    bool               keepOwned
) {
    if (skillMiddleware_) {
        skillMiddleware_->removeSkillDirs({normPath});
    }
    if (auto ctx = agentContext_.lock()) {
        auto& list = ctx->appendComponentInfo.skills;
        list.erase(std::remove(list.begin(), list.end(), normPath), list.end());
    }
    if (!keepOwned) {
        auto it = owned_.find(owner);
        if (it != owned_.end()) {
            auto& l = it->second.applied.skillDirs;
            l.erase(std::remove(l.begin(), l.end(), normPath), l.end());
        }
    }
}

bool AgentResourceApplier::addMemoryFile(
    const std::string& owner,
    const std::string& absPath,
    std::string&       errOut
) {
    return addMemoryFileImpl(owner, normalizePath(absPath), errOut);
}

bool AgentResourceApplier::addMemoryFileImpl(
    const std::string& owner,
    std::string        normPath,
    std::string&       errOut
) {
    if (normPath.empty()) {
        errOut = "empty path";
        return false;
    }
    auto ctx = agentContext_.lock();

    bool conflictsWithYaml = [&]() -> bool {
        if (!ctx || !ctx->agentConfig) {
            return false;
        }
        const auto& list = ctx->agentConfig->memoryFilePaths;
        return std::any_of(list.begin(), list.end(), [&](const auto& p) {
            return normalizePath(p) == normPath;
        });
    }();
    if (conflictsWithYaml) {
        errOut = fmt::format("memory file `{}` conflicts with main yaml config", normPath);
        XX_LOGW("[Resources] Plugin `{}` {}: skipped (yaml priority)", owner, errOut);
        return false;
    }

    // 其他插件已注册同一文件 → 先到先得
    for (const auto& [otherOwner, rec] : owned_) {
        if (otherOwner == owner) {
            continue;
        }
        if (std::find(rec.applied.memoryFiles.begin(), rec.applied.memoryFiles.end(), normPath)
            != rec.applied.memoryFiles.end()) {
            errOut
                = fmt::format("memory file `{}` already owned by plugin `{}`", normPath, otherOwner);
            XX_LOGW("[Resources] Plugin `{}` {}: skipped", owner, errOut);
            return false;
        }
    }

    // 同 addSkillDirImpl: 不因记录已存在短路 (disable→enable 恢复场景)
    if (memoryMiddleware_) {
        memoryMiddleware_->addMemoryFiles({normPath});
    }
    auto& rec = owned_[owner];
    if (std::find(rec.applied.memoryFiles.begin(), rec.applied.memoryFiles.end(), normPath)
        == rec.applied.memoryFiles.end()) {
        rec.applied.memoryFiles.push_back(normPath);
    }
    if (ctx) {
        auto& list = ctx->appendComponentInfo.memoryFiles;
        if (!contains(list, normPath)) {
            list.push_back(normPath);
        }
    }
    return true;
}

bool AgentResourceApplier::removeMemoryFile(const std::string& owner, const std::string& absPath) {
    auto norm = normalizePath(absPath);
    auto it   = owned_.find(owner);
    if (it == owned_.end()
        || std::find(
                   it->second.applied.memoryFiles.begin(),
                   it->second.applied.memoryFiles.end(),
                   norm
               )
            == it->second.applied.memoryFiles.end()) {
        XX_LOGW("[Resources] Plugin `{}` remove memory file `{}` failed (not owned)", owner, norm);
        return false;
    }
    deactivateMemory(owner, norm, /*keepOwned=*/false);
    return true;
}

void AgentResourceApplier::deactivateMemory(
    const std::string& owner,
    const std::string& normPath,
    bool               keepOwned
) {
    if (memoryMiddleware_) {
        memoryMiddleware_->removeMemoryFiles({normPath});
    }
    if (auto ctx = agentContext_.lock()) {
        auto& list = ctx->appendComponentInfo.memoryFiles;
        list.erase(std::remove(list.begin(), list.end(), normPath), list.end());
    }
    if (!keepOwned) {
        auto it = owned_.find(owner);
        if (it != owned_.end()) {
            auto& l = it->second.applied.memoryFiles;
            l.erase(std::remove(l.begin(), l.end(), normPath), l.end());
        }
    }
}

bool AgentResourceApplier::addMcpServer(
    const std::string&     owner,
    std::string_view       nameSpace,
    const McpServerConfig& cfg,
    std::string&           errOut
) {
    if (nameSpace.empty()) {
        errOut = "empty mcp namespace";
        return false;
    }
    if (cfg.url.empty()) {
        errOut = fmt::format("mcp `{}`: empty url", nameSpace);
        return false;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        errOut = "agent context released";
        return false;
    }
    const std::string ns{nameSpace};

    // 主配置 yaml 优先
    if (ctx->agentConfig && ctx->agentConfig->mcpServerUrls.contains(ns)) {
        errOut = fmt::format("mcp namespace `{}` conflicts with main yaml config", ns);
        XX_LOGW("[Resources] Plugin `{}` {}: skipped (yaml priority)", owner, errOut);
        return false;
    }
    // 已有动态条目 (含 Connecting) → 先到先得
    if (auto* exist = findMcp(ns)) {
        errOut = fmt::format("mcp namespace `{}` already owned by plugin `{}`", ns, exist->owner);
        XX_LOGW("[Resources] Plugin `{}` {}: skipped", owner, errOut);
        return false;
    }
    // 被禁用插件的保留记录同样占用命名空间 (enable 恢复时不被抢占)
    for (const auto& [otherOwner, rec] : owned_) {
        if (otherOwner != owner && rec.applied.mcpServers.contains(ns)) {
            errOut = fmt::format("mcp namespace `{}` already owned by plugin `{}`", ns, otherOwner);
            XX_LOGW("[Resources] Plugin `{}` {}: skipped", owner, errOut);
            return false;
        }
    }

    // 连接身份: client 在登记前创建, 协程各阶段经指针比对识别 stale 条目
    // (注销后旧协程不再写回任何状态; 连接释放依赖 shared_ptr 引用计数 —— 不与
    // 在途网络操作并发 close, 由最后一个引用析构时关闭, 规避竞态)
    auto client = std::make_shared<server::McpClient>(server::McpClient::Config{
        .serverUrl       = cfg.url,
        .protocolVersion = std::string{server::McpClient::kProtocol2026_07_28},
        .toolNamespace   = ns,
        .toolCallTimeout = cfg.toolTimeout,
    });

    auto& entry    = mcpEntries_[ns];
    entry.owner    = owner;
    entry.cfg      = cfg;
    entry.client   = client;
    owned_[owner].applied.mcpServers[ns] = cfg;

    spawnMcpConnect(ns, std::move(client));
    return true;
}

bool AgentResourceApplier::removeMcpServer(const std::string& owner, std::string_view nameSpace) {
    const std::string ns{nameSpace};
    // 所有权校验: live 条目存在且归属不匹配 → 拒绝
    if (auto* entry = findMcp(ns); entry && entry->owner != owner) {
        XX_LOGW(
            "[Resources] Plugin `{}` remove mcp `{}` failed (owned by `{}`)",
            owner,
            ns,
            entry->owner
        );
        return false;
    }
    // 摘除 live 条目 (可能不存在: 连接已失败/插件被禁用场景)
    deactivateMcp(ns);
    // 清理本 owner 的所有权记录; 无 live 条目且无记录 → 未持有
    bool hadRecord = false;
    if (auto it = owned_.find(owner); it != owned_.end()) {
        hadRecord = it->second.applied.mcpServers.erase(ns) > 0;
    }
    if (!hadRecord) {
        XX_LOGW("[Resources] Plugin `{}` remove mcp `{}` failed (not owned)", owner, ns);
        return false;
    }
    return true;
}

bool AgentResourceApplier::deactivateMcp(std::string_view nameSpace) {
    const std::string ns{nameSpace};
    auto              it = mcpEntries_.find(ns);
    if (it == mcpEntries_.end()) {
        return false;
    }
    // Ready: 摘除其全部动态工具 (在途调用靠 ToolRegistry 返回的 shared_ptr 保活跑完);
    // Connecting: 直接摘条目 —— 协程各阶段检查 stale 后自行退出。
    // 两种情况均不主动并发 close (见 addMcpServer 注释), 连接由引用计数回收。
    if (it->second.status == McpEntry::Status::Ready) {
        if (auto ctx = agentContext_.lock(); ctx && ctx->toolRegistry) {
            for (const auto& toolName : it->second.toolNames) {
                ctx->toolRegistry->unregisterTool(toolName);
            }
        }
        if (auto ctx = agentContext_.lock()) {
            auto& list = ctx->appendComponentInfo.mcpTools;
            list.erase(std::remove(list.begin(), list.end(), ns), list.end());
        }
    }
    it->second.abortRequested = true;
    mcpEntries_.erase(it);
    return true;
}

void AgentResourceApplier::failMcp(const std::string& ns, const std::shared_ptr<server::McpClient>& client
) {
    auto it = mcpEntries_.find(ns);
    if (it == mcpEntries_.end() || it->second.client != client) {
        return; // 已被注销/替换 (stale)
    }
    mcpEntries_.erase(it);
    // 命名空间记录保留于 owned_ (enable 恢复时可重试); 此处仅清 live 条目
}

void AgentResourceApplier::spawnMcpConnect(
    std::string                                   ns,
    std::shared_ptr<server::McpClient>            client
) {
    // self 保活: applier 析构 (随 AgentContext) 后协程仍可安全完成清理;
    // 各阶段经 ctx 弱引用判活, agent 已销毁时静默退出
    auto self = shared_from_this();
    asio::co_spawn(
        ioExecutor_,
        [self, ns = std::move(ns), client]() -> asio::awaitable<void> {
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    auto ctx = self->agentContext_.lock();

                    // ---- 阶段 1: initialize ----
                    auto initRes = co_await client->initialize();
                    {
                        auto* entry = self->findMcp(ns);
                        if (!entry || entry->client != client || entry->abortRequested) {
                            co_return false; // 已被注销 (stale)
                        }
                        if (!initRes.has_value()) {
                            XX_LOGE(
                                "[Resources] Plugin `{}` mcp `{}` initialize failed: {}",
                                entry->owner,
                                ns,
                                initRes.error()
                            );
                            self->failMcp(ns, client);
                            co_return false;
                        }
                    }

                    // ---- 阶段 2: listTools ----
                    auto tools = co_await client->listTools();
                    {
                        auto* entry = self->findMcp(ns);
                        if (!entry || entry->client != client || entry->abortRequested) {
                            co_return false;
                        }
                        if (!tools.has_value()) {
                            XX_LOGE(
                                "[Resources] Plugin `{}` mcp `{}` listTools failed: {}",
                                entry->owner,
                                ns,
                                tools.error()
                            );
                            self->failMcp(ns, client);
                            co_return false;
                        }
                    }

                    // ---- 阶段 3: 工具注册进 ToolRegistry (动态可见) ----
                    auto* entry = self->findMcp(ns);
                    if (!entry || entry->client != client) {
                        co_return false;
                    }
                    if (ctx && ctx->toolRegistry) {
                        for (auto& def : tools.value()) {
                            std::shared_ptr<agentxx::tools::XXToolBase> tool
                                = client->createTool(std::move(def), ctx);
                            auto toolName = tool->get_name();
                            if (ctx->toolRegistry->registerTool(toolName, tool)) {
                                entry->toolNames.push_back(std::move(toolName));
                            } else {
                                XX_LOGW(
                                    "[Resources] mcp `{}` tool `{}` register failed (conflict?)",
                                    ns,
                                    toolName
                                );
                            }
                        }
                    } else {
                        XX_LOGE("[Resources] mcp `{}`: no tool registry, skip tool registration", ns);
                    }
                    entry->status = McpEntry::Status::Ready;
                    if (ctx) {
                        ctx->appendComponentInfo.mcpTools.push_back(ns);
                    }
                    XX_LOGI(
                        "[Resources] Plugin `{}` mcp `{}` ready ({} tools)",
                        entry->owner,
                        ns,
                        entry->toolNames.size()
                    );
                    co_return true;
                },
                [](std::string errmsg) -> asio::awaitable<bool> {
                    XX_LOGE("[Resources] mcp connect coroutine error: {}", errmsg);
                    co_return false;
                }
            );
            co_return;
        },
        // 完成处理器兜底: catchErrorAsync 已捕获普通异常, 此处防御异常逃逸 detached 协程
        [](std::exception_ptr ep) {
            if (ep) {
                agentxx::util::catchError<bool>(
                    [&]() {
                        std::rethrow_exception(ep);
                        return true;
                    },
                    [](std::string msg) {
                        XX_LOGE("[Resources] mcp connect coroutine fatal: {}", msg);
                        return false;
                    }
                );
            }
        }
    );
}

void AgentResourceApplier::removeAllOwned(const std::string& owner) {
    auto it = owned_.find(owner);
    if (it == owned_.end()) {
        return;
    }
    // 先拷贝待摘除清单再逐项处理 (摘除过程会修改 owned_)
    auto skillDirs  = it->second.applied.skillDirs;
    auto memoryList = it->second.applied.memoryFiles;
    auto nses       = it->second.applied.mcpServers;
    for (const auto& [ns, cfg] : nses) {
        (void)cfg;
        deactivateMcp(ns);
    }
    for (const auto& p : skillDirs) {
        deactivateSkill(owner, p, /*keepOwned=*/true);
    }
    for (const auto& p : memoryList) {
        deactivateMemory(owner, p, /*keepOwned=*/true);
    }
    owned_.erase(it); // 卸载路径: 记录一并清除
}

void AgentResourceApplier::setOwnerEnabled(const std::string& owner, bool enabled) {
    auto it = owned_.find(owner);
    if (it == owned_.end()) {
        return;
    }
    if (enabled) {
        // 按记录恢复 (拷贝一份避免 apply 过程修改容器导致迭代失效)
        auto rec = it->second.applied;
        applyDecls(owner, rec);
    } else {
        // 禁用: 仅摘"生效"部分, 保留 owned_ 记录供 enable 恢复
        auto skillDirs  = it->second.applied.skillDirs;
        auto memoryList = it->second.applied.memoryFiles;
        auto nses       = it->second.applied.mcpServers;
        for (const auto& [ns, cfg] : nses) {
            (void)cfg;
            deactivateMcp(ns);
        }
        for (const auto& p : skillDirs) {
            deactivateSkill(owner, p, /*keepOwned=*/true);
        }
        for (const auto& p : memoryList) {
            deactivateMemory(owner, p, /*keepOwned=*/true);
        }
    }
}

AgentResourceSnapshot AgentResourceApplier::ownedBy(std::string_view owner) const {
    AgentResourceSnapshot snap;
    auto                  it = owned_.find(owner);
    if (it == owned_.end()) {
        // MCP live 条目可能存在而记录已被清理的极端情况不在此处理 (正常流程一致)
        return snap;
    }
    snap.skillDirs   = it->second.applied.skillDirs;
    snap.memoryFiles = it->second.applied.memoryFiles;
    for (const auto& [ns, cfg] : it->second.applied.mcpServers) {
        (void)cfg;
        snap.mcpNamespaces.push_back(ns);
    }
    return snap;
}

} // namespace agent
} // namespace agentxx
