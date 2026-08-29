#include "agentxx/tools/git_worktree.h"

#include "agentxx/middlewares/permission.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/worktree.h"
#include <chrono>
#include <filesystem>

namespace agentxx {
namespace tools {

namespace {

using agentxx::util::worktree::GitResult;

/// 读取工具参数中的字符串 (缺失/类型不符回退默认值)
/// - neograph::json 的 contains/operator[] 不接受 string_view, 键统一用 std::string
std::string
    argString(const neograph::json& args, const std::string& key, std::string_view def = {}) {
    if (args.contains(key) && args[key].is_string()) {
        return args[key].get<std::string>();
    }
    return std::string{def};
}

/// 生成自动 worktree 名称: wt-{unix秒}-{自增序号} (同秒冲突时递增序号)
std::string generateWorktreeName(const std::string& repoRootDir) {
    namespace fw = agentxx::util::worktree;
    auto now     = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()
    )
                   .count();
    for (unsigned i = 0; i < 100; ++i) {
        auto            name = fmt::format("wt-{:x}-{}", now, i);
        std::error_code ec;
        if (!std::filesystem::exists(fw::worktreesRoot(repoRootDir) + "/" + name, ec)) {
            return name;
        }
    }
    // 兜底: 理论不可达 (同秒 100 个), 时间戳进位后重试一次
    return fmt::format("wt-{:x}-{}", now + 1, 0);
}

/// 绑定会话到 worktree 并注册权限隔离边界 (io 线程调用)
void bindSession(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const std::string&                                   sessionId,
    const std::string&                                   name,
    const std::string&                                   path,
    const std::string&                                   branch,
    const std::string&                                   repoRootDir
) {
    auto session = ctx->sessions->get(sessionId);
    if (!session) {
        return;
    }
    session->setWorktreeBinding(agentxx::agent::WorktreeBinding{
        .name     = name,
        .path     = path,
        .branch   = branch,
        .repoRoot = repoRootDir,
    });
    // 权限隔离: 主检出子树写 DENY / worktree 子树放行。
    // 路径为绝对路径, normalizePermissionPath 单参版本即可 (基准仅影响相对路径)
    if (ctx->permissionMiddleware) {
        agentxx::middleware::SessionFsIsolation iso;
        iso.allowPath     = ctx->permissionMiddleware->normalizePermissionPath(path);
        iso.denyWritePath = ctx->permissionMiddleware->normalizePermissionPath(repoRootDir);
        ctx->permissionMiddleware->setSessionIsolation(sessionId, std::move(iso));
    }
}

/// 解绑会话并清除权限隔离边界 (io 线程调用)
void unbindSession(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    const std::string&                                   sessionId
) {
    auto session = ctx->sessions->get(sessionId);
    if (session && !session->getWorktreeBinding().path.empty()) {
        session->clearWorktreeBinding();
    }
    if (ctx->permissionMiddleware) {
        ctx->permissionMiddleware->clearSessionIsolation(sessionId);
    }
}

/// 格式化单个 worktree 条目行 (info 列表用)
std::string
    formatEntryLine(const agentxx::util::worktree::WorktreeEntry& e, bool dirty, bool isCurrent) {
    std::string headDisp = e.head.empty() ? "(unknown)" : e.head.substr(0, 8);
    std::string line     = fmt::format(
        "- {}{} branch={} head={}{}{}",
        isCurrent ? "* " : "  ",
        e.path,
        e.detached ? "(detached)" : (e.branch.empty() ? "(none)" : e.branch),
        headDisp,
        dirty ? " [dirty]" : "",
        e.bare ? " [bare]" : ""
    );
    return line;
}

} // namespace

GitWorktreeTool::GitWorktreeTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_git_worktree",
        std::move(in_agentContext),
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false // 核心工作流工具, 始终完整注册不延迟加载
    ) {}

neograph::ChatTool GitWorktreeTool::get_definition() const {
    // 描述与参数说明经 AgentPrompt::toolPrompt 下发 (支持 yaml/训练覆盖),
    // 未配置时回退内置文案 —— 与其他 lib 内置工具同模式
    std::string depict
        = "Manage isolated git worktrees for this session. "
          "`create` makes a new worktree and BINDS the current session to it, so all subsequent file "
          "operations and commands run inside it; `info` lists worktrees and shows the current binding; "
          "`status` summarizes pending changes in the current worktree; `remove` deletes a worktree.";
    if (auto p = agentContext.lock()) {
        if (p->agentConfig) {
            auto it = p->agentConfig->prompt.toolPrompt.find(name);
            if (it != p->agentConfig->prompt.toolPrompt.end() && !it->second.depict.empty()) {
                depict = it->second.depict;
            }
        }
    }
    neograph::json params = neograph::json{
        {"type",       "object"                      },
        {"properties",
         {
             {"opt",
              {
                  {"type", "string"},
                  {"enum", neograph::json::array({"create", "info", "status", "remove"})},
                  {"description",
                   R"(Operation to perform:
`create`: Create an isolated worktree and bind THIS session to it. Use at the start of code-modifying tasks.
`info`: Show the current binding and all worktrees of the repository.
`status`: Summarize pending changes in the bound worktree.
`remove`: Delete a worktree (refuses when it has uncommitted work unless `force`.)"},
              }},
             {"name",
              {
                  {"type", "string"},
                  {"description",
                   R"(Worktree name (used as directory name and in the branch `agentxx/wt-{name}`).
Allowed chars: letters, digits, `.`, `_`, `-`. Required by `create` (a timestamped name is generated when empty) and `remove`; ignored by others.)"
                  },
              }},
             {"base_ref",
              {
                  {"type", "string"},
                  {"description",
                   R"(`create` only. Branch/commit to base the new worktree on; default: current HEAD.)"
                  },
              }},
             {"force",
              {
                  {"type", "boolean"},
                  {"description",
                   R"(`remove` only. Default `false`. When true, delete even if the worktree has uncommitted/untracked changes or unpushed commits — data loss risk; prefer committing first.)"
                  },
              }},
         }                                           },
        {"required",   neograph::json::array({"opt"})},
    };
    return {name, depict, std::move(params)};
}

asio::awaitable<std::string> GitWorktreeTool::execute_async(const neograph::json& arguments) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->agentConfig) {
        co_return R"({"error":"agent context unavailable"})";
    }
    auto config    = ctxPtr->agentConfig;
    auto sessionId = arguments.value("sessionId", std::string{});
    auto opt       = argString(arguments, "opt", "info");

    // 当前绑定状态 (io 线程读取)
    const agentxx::agent::WorktreeBinding* binding = nullptr;
    auto session = sessionId.empty() ? nullptr : ctxPtr->sessions->get(sessionId);
    if (session && !session->getWorktreeBinding().path.empty()) {
        binding = &session->getWorktreeBinding();
    }

    // ---- info / status 的仓库根解析: 绑定时直接复用记录值, 避免重复起 git 进程 ----
    std::string effectiveDir;
    std::string knownRepoRoot;
    if (binding != nullptr) {
        knownRepoRoot = binding->repoRoot;
        effectiveDir  = binding->path;
    } else if (!config->inheritedWorktreePath.empty()) {
        effectiveDir = config->inheritedWorktreePath;
    } else {
        // 统一经会话级入口解析 (worktree 绑定 > 会话工作目录覆写 >
        // AgentConfig::workDir / 进程 cwd), 不直接依赖进程 cwd
        effectiveDir = ctxPtr->getSessionWorkDir(sessionId);
    }

    auto cancelToken = getSessionCancelToken(ctxPtr, arguments);

    // ---- info: 无需写操作, 但列举/状态探测涉及子进程, 卸载线程池执行 ----
    if (opt == "info") {
        auto result = co_await agentxx::util::offloadCancellableAsync<std::string>(
            *ctxPtr->threadPool,
            cancelToken,
            [&, effectiveDir, knownRepoRoot](std::atomic<bool>& cancelFlag
            ) -> asio::awaitable<std::string> {
                namespace fw     = agentxx::util::worktree;
                std::string root = knownRepoRoot;
                if (root.empty()) {
                    if (effectiveDir.empty() || !fw::isInsideWorkTree(effectiveDir)) {
                        co_return R"({"error":"not inside a git repository","hint":"worktree mode requires a git repository with at least one commit"})";
                    }
                    auto r = fw::repoRoot(effectiveDir);
                    if (!r) {
                        co_return R"({"error":"cannot resolve repository root"})";
                    }
                    root = *r;
                }
                auto           entries = fw::listWorktrees(root);
                neograph::json arr     = neograph::json::array();
                std::string    boundName;
                if (binding != nullptr) {
                    boundName = binding->name;
                }
                for (const auto& e : entries) {
                    if (cancelFlag.load(std::memory_order_acquire)) {
                        co_return R"({"error":"cancelled"})";
                    }
                    auto st    = fw::statusSummary(e.path);
                    bool dirty = st ? st->dirtyFiles() : false;
                    arr.push_back(neograph::json{
                        {"path",     e.path     },
                        {"branch",   e.branch   },
                        {"head",     e.head     },
                        {"bare",     e.bare     },
                        {"detached", e.detached },
                        {"dirty",    dirty      },
                        {"current",
                         !boundName.empty()
                             && std::filesystem::path{e.path}.filename().generic_string()
                                    == boundName},
                    });
                }
                neograph::json out{
                    {"repoRoot",  root},
                    {"worktrees", arr },
                };
                if (binding != nullptr) {
                    out["current"] = neograph::json{
                        {"name",   binding->name  },
                        {"path",   binding->path  },
                        {"branch", binding->branch},
                    };
                } else if (!config->inheritedWorktreePath.empty()) {
                    out["inherited"] = config->inheritedWorktreePath;
                } else {
                    out["current"] = nullptr;
                    out["hint"]
                        = "Not in a worktree yet. Call opt=create to start an isolated workspace for code-modifying tasks.";
                }
                co_return out.dump();
            }
        );
        co_return result;
    }

    // ---- create: 创建 + 绑定 + 权限隔离 ----
    if (opt == "create") {
        if (binding != nullptr) {
            co_return neograph::json{
                {"error",
                 fmt::format(
                     "session already bound to worktree '{}' ({})", binding->name,
                 binding->path
                 )},
                {"hint", "Use opt=remove to delete it first, or opt=status/info to inspect."},
            }
                .dump();
        }
        auto userName = argString(arguments, "name");
        auto baseRef  = argString(arguments, "base_ref");

        // 名称清洗/生成 (纯函数, io 线程可做); 冲突检查在 offload 内以最终状态为准
        std::string name = userName.empty()
                               ? std::string{}
                               : agentxx::util::worktree::sanitizeWorktreeName(userName);
        if (!userName.empty() && name.empty()) {
            co_return neograph::json{
                {"error", fmt::format("invalid worktree name '{}'", userName)},
                {"hint", "allowed chars: letters, digits, '.', '_', '-'"},
            }
                .dump();
        }

        struct CreateOutcome {
            std::string name;
            std::string path;
            std::string branch;
            std::string repoRoot; ///< 仓库根绝对路径 (绑定与权限隔离边界注册用)
            std::string error;
            bool        ok = false;
        };

        // 仓库探测 + 创建全部卸载到线程池 (git 子进程调用不可在 io 线程阻塞)
        auto outcome = co_await agentxx::util::offloadCancellableAsync<CreateOutcome>(
            *ctxPtr->threadPool,
            cancelToken,
            [effectiveDir, name, baseRef](std::atomic<bool>& cancelFlag
            ) -> asio::awaitable<CreateOutcome> {
                namespace fw = agentxx::util::worktree;
                CreateOutcome out;
                if (effectiveDir.empty() || !fw::isInsideWorkTree(effectiveDir)) {
                    out.error = "not inside a git repository";
                    co_return out;
                }
                auto rootOpt = fw::repoRoot(effectiveDir);
                if (!rootOpt) {
                    out.error = "cannot resolve repository root";
                    co_return out;
                }
                const std::string repoRootDir = *rootOpt;
                out.name                      = name.empty()
                                                    ? fw::sanitizeWorktreeName(generateWorktreeName(repoRootDir))
                                                    : name;
                if (out.name.empty()) {
                    out.error = "failed to generate a valid worktree name";
                    co_return out;
                }
                // 同名已注册的 worktree 直接拒绝 (避免误删用户数据; 复用请换名)
                for (const auto& e : fw::listWorktrees(repoRootDir)) {
                    if (cancelFlag.load(std::memory_order_acquire)) {
                        out.error = "[cancelled]";
                        co_return out;
                    }
                    if (std::filesystem::path{e.path}.filename().generic_string() == out.name) {
                        out.error
                            = fmt::format("worktree '{}' already exists at {}", out.name, e.path);
                        co_return out;
                    }
                }
                auto r = fw::createWorktree(repoRootDir, out.name, baseRef);
                if (!r.ok()) {
                    out.error = r.errorText(fmt::format("git worktree add (name='{}')", out.name));
                    co_return out;
                }
                out.repoRoot = repoRootDir;
                out.path     = (std::filesystem::path{fw::worktreesRoot(repoRootDir)} / out.name)
                               .generic_string();
                out.branch = fw::branchForName(out.name);
                out.ok     = true;
                co_return out;
            }
        );

        if (!outcome.ok) {
            co_return neograph::json{
                {"error", outcome.error}
            }.dump();
        }
        if (sessionId.empty()) {
            co_return neograph::json{
                {"error", "no session id available, cannot bind"},
                {"path",  outcome.path                          },
            }
                .dump();
        }
        // 绑定 + 权限隔离 (io 线程)
        bindSession(
            ctxPtr,
            sessionId,
            outcome.name,
            outcome.path,
            outcome.branch,
            outcome.repoRoot
        );
        co_return neograph::json{
            {"ok",       true                                                                        },
            {"op",       "create"                                                                    },
            {"name",     outcome.name                                                                },
            {"path",     outcome.path                                                                },
            {"branch",   outcome.branch                                                              },
            {"repoRoot", outcome.repoRoot                                                            },
            {"note",
             "Session is now bound to this worktree: relative paths resolve here, writes into the main "
             "checkout are denied. Commit regularly; before finishing run opt=status and remind the user "
             "to review/commit/merge. The worktree is kept after the task unless explicitly removed."
            },
        }
            .dump();
    }

    // ---- status: 当前(或指定) worktree 的工作区摘要 ----
    if (opt == "status") {
        auto        targetName = argString(arguments, "name");
        std::string targetPath;
        std::string rootForList = knownRepoRoot;
        if (binding != nullptr && targetName.empty()) {
            targetPath = binding->path;
        } else if (session && !session->getWorktreeBinding().path.empty() && targetName.empty()) {
            targetPath = session->getWorktreeBinding().path;
        }
        auto result = co_await agentxx::util::offloadCancellableAsync<std::string>(
            *ctxPtr->threadPool,
            cancelToken,
            [&, targetName, targetPath, rootForList, effectiveDir](std::atomic<bool>&)
                -> asio::awaitable<std::string> {
                namespace fw     = agentxx::util::worktree;
                std::string root = rootForList;
                std::string path = targetPath;
                if (path.empty()) {
                    // 按名称定位 (未绑定会话指定 name 的场景)
                    if (root.empty()) {
                        if (effectiveDir.empty() || !fw::isInsideWorkTree(effectiveDir)) {
                            co_return R"({"error":"not inside a git repository"})";
                        }
                        auto r = fw::repoRoot(effectiveDir);
                        if (!r) {
                            co_return R"({"error":"cannot resolve repository root"})";
                        }
                        root = *r;
                    }
                    auto cleanName
                        = targetName.empty() ? std::string{} : fw::sanitizeWorktreeName(targetName);
                    if (cleanName.empty()) {
                        co_return R"({"error":"no active worktree for this session; pass `name` or create one first"})";
                    }
                    path = (std::filesystem::path{fw::worktreesRoot(root)} / cleanName)
                               .generic_string();
                }
                if (!std::filesystem::exists(path)) {
                    co_return neograph::json{
                        {"error", fmt::format("worktree directory not found: {}", path)},
                        {"hint", "it may have been removed; use opt=info to list existing worktrees"
                        },
                    }
                        .dump();
                }
                auto st = fw::statusSummary(path);
                if (!st) {
                    co_return neograph::json{
                        {"error", "git status failed"},
                        {"path",  path               }
                    }.dump();
                }
                neograph::json out{
                    {"path",           path         },
                    {"modified",       st->modified },
                    {"added",          st->added    },
                    {"deleted",        st->deleted  },
                    {"renamed",        st->renamed  },
                    {"untracked",      st->untracked},
                    {"ahead",          st->ahead    },
                    {"behind",         st->behind   },
                    {"headLine",       st->headLine },
                    {"hasPendingWork", st->hasWork()},
                };
                if (st->hasWork()) {
                    out["reminder"]
                        = "There are uncommitted changes or unpushed commits. Before finishing, summarize them "
                          "and remind the user to review / commit / merge. Do NOT delete the worktree unless "
                          "the user explicitly confirms.";
                }
                co_return out.dump();
            }
        );
        co_return result;
    }

    // ---- remove: 默认保留策略下唯一的删除入口 ----
    if (opt == "remove") {
        auto targetName = argString(arguments, "name");
        bool force      = arguments.value("force", false);
        if (targetName.empty() && binding == nullptr) {
            co_return R"({"error":"no active worktree for this session; pass `name` or bind one first"})";
        }
        std::string name = targetName.empty()
                               ? binding->name
                               : agentxx::util::worktree::sanitizeWorktreeName(targetName);
        if (name.empty()) {
            co_return neograph::json{
                {"error", "invalid worktree name"}
            }.dump();
        }
        bool removesCurrent = (binding != nullptr && binding->name == name);

        struct RemoveOutcome {
            std::string error;
            std::string summary;
            size_t      modified = 0, added = 0, deleted = 0, renamed = 0;
            size_t      untracked = 0, ahead = 0, unmerged = 0;
            bool        removed = false;
        };

        auto outcome = co_await agentxx::util::offloadCancellableAsync<RemoveOutcome>(
            *ctxPtr->threadPool,
            cancelToken,
            [&, name, force](std::atomic<bool>& cancelFlag) -> asio::awaitable<RemoveOutcome> {
                namespace fw = agentxx::util::worktree;
                RemoveOutcome out;
                std::string   root;
                {
                    // remove 必然发生在主检出上下文 (当前会话绑定时 repoRoot 已知;
                    // 指定名字删除时从进程工作目录解析)
                    std::string dir = effectiveDir;
                    if (dir.empty() || !fw::isInsideWorkTree(dir)) {
                        out.error = "not inside a git repository";
                        co_return out;
                    }
                    auto r = fw::repoRoot(dir);
                    if (!r) {
                        out.error = "cannot resolve repository root";
                        co_return out;
                    }
                    root = *r;
                }
                // 目标必须是本约定目录下的已注册 worktree (防误删任意路径)
                auto expected
                    = (std::filesystem::path{fw::worktreesRoot(root)} / name).generic_string();
                bool found = false;
                for (const auto& e : fw::listWorktrees(root)) {
                    if (cancelFlag.load(std::memory_order_acquire)) {
                        out.error = "[cancelled]";
                        co_return out;
                    }
                    if (std::filesystem::absolute(std::filesystem::path{e.path}).generic_string()
                        == expected) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out.error = fmt::format("'{}' is not a registered agentxx worktree", name);
                    co_return out;
                }
                // 脏检查: 有未提交工作或未合并提交且未显式 force 时拒绝 (提醒先提交)
                auto st = fw::statusSummary(expected);
                if (st && st->hasWork() && !force) {
                    out.summary   = st->headLine;
                    out.modified  = st->modified;
                    out.added     = st->added;
                    out.deleted   = st->deleted;
                    out.renamed   = st->renamed;
                    out.untracked = st->untracked;
                    out.ahead     = st->ahead;
                    out.unmerged  = st->unmerged;
                    out.error     = fmt::format(
                        "worktree '{}' has pending work (modified:{} added:{} deleted:{} renamed:{} untracked:{} unmerged-commits:{}); commit them first or pass force=true",
                        name,
                        st->modified,
                        st->added,
                        st->deleted,
                        st->renamed,
                        st->untracked,
                        st->unmerged
                    );
                    co_return out;
                }
                auto r = fw::removeWorktreeByName(root, name, force);
                if (!r.ok()) {
                    out.error = r.errorText(fmt::format("git worktree remove '{}'", name));
                    co_return out;
                }
                out.removed = true;
                co_return out;
            }
        );

        if (!outcome.removed) {
            neograph::json out{
                {"error",   outcome.error},
                {"removed", false        },
            };
            if (!outcome.summary.empty()) {
                out["pending"] = neograph::json{
                    {"modified",  outcome.modified },
                    {"added",     outcome.added    },
                    {"deleted",   outcome.deleted  },
                    {"renamed",   outcome.renamed  },
                    {"untracked", outcome.untracked},
                    {"ahead",     outcome.ahead    },
                    {"unmerged",  outcome.unmerged },
                };
                out["reminder"]
                    = "Remind the user about these pending changes: they can commit/merge them, then call remove again (or force=true to discard).";
            }
            co_return out.dump();
        }
        if (removesCurrent && !sessionId.empty()) {
            unbindSession(ctxPtr, sessionId);
        }
        co_return neograph::json{
            {"ok",      true          },
            {"op",      "remove"      },
            {"name",    name          },
            {"unbound", removesCurrent},
        }
            .dump();
    }

    co_return neograph::json{
        {"error", fmt::format("unknown opt '{}', expect create|info|status|remove", opt)},
    }
        .dump();
}

} // namespace tools
} // namespace agentxx
