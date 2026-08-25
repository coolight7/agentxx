#pragma once

/// @file worktree.h
/// @brief Git worktree 管理工具函数 (worktree 模式底层支撑)
///
/// 为 `agentxx_git_worktree` 工具与会话绑定机制提供纯同步的 git 操作封装:
/// - 创建/列举/删除 worktree (`git worktree add/list/remove`)
/// - 仓库探测与仓库根解析 (`git rev-parse`)
/// - 工作区状态摘要 (`git status --porcelain`)
/// - `.git/info/exclude` 维护 (使 worktree 目录不出现在主检出 untracked 中)
///
/// 设计要点:
/// - 全部经 argv 直调 git 子进程, 不经 shell 解析 (参数注入安全)
/// - 同步阻塞实现: 调用方负责卸载到线程池 (工具层经 offloadCancellableAsync)
/// - header-only: 仅被 lib 内工具与测试包含, 不进入 agentxx_util 静态库
///   (boost.process 头依赖不随 util 库传递)

#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/read.hpp"
#include "asio/readable_pipe.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>
#include <neograph/define.h>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if !defined(XX_IS_WIN_D)
#define XX_IS_WIN_D 0
#endif

#if defined(AGENTXX_ENABLE_BOOST_PROCESS) && AGENTXX_ENABLE_BOOST_PROCESS
#include "boost/process.hpp"
#define AGENTXX_WORKTREE_HAS_BOOST_PROCESS 1
#else
#define AGENTXX_WORKTREE_HAS_BOOST_PROCESS 0
#endif

namespace agentxx {
namespace util {
namespace worktree {

/// worktree 存放根目录名 (固定约定: {repoRoot}/.agentxx/agent/worktrees)
inline constexpr std::string_view kWorktreesDirName = ".agentxx/agent/worktrees";
/// 自动创建的分支前缀
inline constexpr std::string_view kBranchPrefix = "agentxx/wt-";
/// 单个 git 操作默认超时 (秒); clone/add 大仓库可能较慢, 给足余量
inline constexpr int kDefaultTimeoutSec = 120;

/// 名称合法字符集: 字母数字与 . _ - (首字符不为 '.'/'-')
inline bool isValidWorktreeName(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name.front() == '.' || name.front() == '-') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-';
    });
}

/// 清洗用户输入的名称: 非法字符映射为 '-', 折叠连续分隔符, 去除首尾悬空符,
/// 截断到 64 字符。返回空串表示清洗后无有效内容 (如全为非法字符)
/// - 合法字符: 字母数字与 . _ - (保留原样, 仅折叠重复)
/// - 其余字符 (含路径分隔符, 防目录穿越) 一律替换为 '-'
inline std::string sanitizeWorktreeName(std::string_view candidate) {
    std::string out;
    out.reserve(candidate.size());
    for (char c : candidate) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        } else if (c == '.' || c == '_' || c == '-') {
            if (out.empty() || out.back() != c) {
                out.push_back(c);
            }
        } else {
            if (out.empty() || out.back() != '-') {
                out.push_back('-');
            }
        }
    }
    // 首尾清理: 剥离前导 '.'/'-' (避免隐藏目录名/选项样式的分支名),
    // 剥离尾部 '.'/'-' (Windows 目录名尾部点号非法)
    size_t b = 0;
    while (b < out.size() && (out[b] == '.' || out[b] == '-')) {
        ++b;
    }
    size_t e = out.size();
    while (e > b && (out[e - 1] == '.' || out[e - 1] == '-')) {
        --e;
    }
    out = out.substr(b, e - b);
    // Windows 保留名规避 (con/prn/aux/nul/com1-9/lpt1-9, 不区分大小写且可带扩展名)
    static const char* kReserved[] = {"con", "prn", "aux",  "nul",  "com1", "com2", "com3",
                                      "com4", "com5", "com6", "com7", "com8", "com9",
                                      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7",
                                      "lpt8", "lpt9"};
    for (const auto* r : kReserved) {
        std::string lower;
        lower.reserve(out.size());
        for (char c : out) {
            lower.push_back((char)std::tolower((unsigned char)c));
        }
        if (lower == r || (lower.size() > std::strlen(r) && lower.compare(0, std::strlen(r), r) == 0
                           && lower[std::strlen(r)] == '.')) {
            // 前缀 '_' 隔离保留名; 紧随的 '.' 改为 '-' (避免 aux.txt 形态被系统按设备解析)
            out.insert(out.begin(), '_');
            if (lower.size() > std::strlen(r) && lower[std::strlen(r)] == '.') {
                out[std::strlen(r) + 1] = '-';
            }
            break;
        }
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    return out;
}

/// git 命令执行结果
struct GitResult {
    int         exitCode = -1;
    std::string out;  ///< stdout (utf-8 直传, git 默认输出 ascii 路径加引号转义)
    std::string err;  ///< stderr

    bool ok() const {
        return exitCode == 0;
    }

    /// 组合错误描述 (供工具结果文本直接使用)
    std::string errorText(std::string_view what) const {
        std::string e = err.empty() ? out : err;
        if (e.size() > 2000) {
            e.resize(2000);
        }
        return fmt::format("[Error] {} failed (exit={}): {}", what, exitCode, e);
    }
};

#if AGENTXX_WORKTREE_HAS_BOOST_PROCESS

namespace detail {

/// 同步执行 git 子进程并捕获 stdout/stderr (超时整组终止)
/// - Linux 经 setsid 启动 (pgid == pid), 超时 killpg 整组清理
/// - cwd 为空时继承当前进程工作目录
inline GitResult runGitImpl(const std::vector<std::string>& args, const std::string& cwd, int timeoutSec) {
    GitResult result;
    asio::io_context io;
    asio::readable_pipe outpip{io}, errpip{io};

    std::vector<std::string> procArgs;
    const char*              exeName = nullptr;
#if XX_IS_WIN_D
    exeName = "git.exe";
    procArgs = args;
#else
    exeName  = "setsid";
    procArgs.reserve(args.size() + 1);
    procArgs.push_back("git");
    procArgs.insert(procArgs.end(), args.begin(), args.end());
#endif
    auto procExe = boost::process::environment::find_executable(exeName);
    if (procExe.empty()) {
        result.exitCode = -1;
        result.err      = fmt::format("'{}' not found in PATH", exeName);
        return result;
    }

    try {
        boost::process::process proc{
            io,
            procExe,
            procArgs,
            boost::process::process_stdio{.in = nullptr, .out = outpip, .err = errpip},
            cwd.empty() ? boost::process::process_start_dir{"."}
                        : boost::process::process_start_dir{cwd}
        };

        neograph_asio_error_code ecOut, ecErr;
        auto readOut = asio::async_read(
            outpip,
            asio::dynamic_buffer(result.out),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, ecOut)
        );
        auto readErr = asio::async_read(
            errpip,
            asio::dynamic_buffer(result.err),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, ecErr)
        );

        std::exception_ptr ep{};
        std::atomic<bool>  timedOut{false};
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                auto mainWork = [&]() -> asio::awaitable<void> {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (
                        std::move(readOut) && std::move(readErr) && proc.async_wait(asio::use_awaitable)
                    );
                };
                auto timeoutGuard = [&]() -> asio::awaitable<void> {
                    if (timeoutSec > 0) {
                        asio::steady_timer t(co_await asio::this_coro::executor);
                        t.expires_after(std::chrono::seconds{timeoutSec});
                        auto [ec] = co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
                        if (!ec) {
                            timedOut.store(true, std::memory_order_release);
#if XX_IS_WIN_D
                            neograph_asio_error_code ec2;
                            proc.terminate(ec2);
#else
                            ::kill(-static_cast<pid_t>(proc.id()), SIGKILL);
#endif
                        }
                    }
                    // 动作完成后挂起直至被并行组取消 (语义同 execute_command_impl):
                    // 若守护先返回会把主工作在结果组装前整体取消
                    asio::steady_timer idle(co_await asio::this_coro::executor);
                    idle.expires_after(std::chrono::hours(24));
                    co_await idle.async_wait(asio::as_tuple(asio::use_awaitable));
                };
                try {
                    using namespace asio::experimental::awaitable_operators;
                    co_await (mainWork() || timeoutGuard());
                } catch (...) {
                    ep = std::current_exception();
                }
            },
            asio::detached
        );
        io.run();
        if (ep) {
            std::rethrow_exception(ep);
        }
        result.exitCode = timedOut.load(std::memory_order_acquire) ? -1 : proc.exit_code();
        if (timedOut.load(std::memory_order_acquire)) {
            result.err += "\n[timed out]";
        }
    } catch (const std::exception& ex) {
        result.exitCode = -1;
        result.err      = ex.what();
    }
    return result;
}

} // namespace detail

/// 执行 git 命令 (argv 直调; cwd 为工作目录; 超时整组终止)
/// - cancelFlag 非空时在执行前检查, 已取消则跳过启动 (快速失败)
inline GitResult runGit(
    const std::vector<std::string>& args,
    const std::string&              cwd        = {},
    int                             timeoutSec = kDefaultTimeoutSec,
    const std::atomic<bool>*        cancelFlag = nullptr
) {
    GitResult skipped;
    if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_acquire)) {
        skipped.exitCode = -1;
        skipped.err      = "[cancelled]";
        return skipped;
    }
    return detail::runGitImpl(args, cwd, timeoutSec);
}
#else

/// 无 boost.process 编译配置时的降级实现: 仅支持查询类操作不可用, 直接报错。
/// (lib 正常构建均定义 AGENTXX_ENABLE_BOOST_PROCESS, 此分支兜底保证可编译)
inline GitResult runGit(
    const std::vector<std::string>&,
    const std::string& = {},
    int                = kDefaultTimeoutSec,
    const std::atomic<bool>* = nullptr
) {
    GitResult r;
    r.exitCode = -1;
    r.err      = "git execution unavailable (built without boost.process)";
    return r;
}
#endif

/// 判断目录是否位于 git 工作树内
inline bool isInsideWorkTree(const std::string& dir) {
    auto r = runGit({"rev-parse", "--is-inside-work-tree"}, dir, 30);
    return r.ok() && r.out.find("true") != std::string::npos;
}

/// 解析仓库根目录绝对路径 (dir 不在仓库内时返回 nullopt)
inline std::optional<std::string> repoRoot(const std::string& dir) {
    auto r = runGit({"rev-parse", "--show-toplevel"}, dir, 30);
    if (!r.ok()) {
        return std::nullopt;
    }
    std::string root = r.out;
    while (!root.empty() && (root.back() == '\n' || root.back() == '\r' || root.back() == ' ')) {
        root.pop_back();
    }
    if (root.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    auto p = std::filesystem::path{root}.lexically_normal().make_preferred();
    auto abs = std::filesystem::absolute(p, ec);
    return ec ? root : abs.generic_string();
}

/// 解析当前 HEAD 提交 (空仓库/无提交时返回 nullopt)
inline std::optional<std::string> headCommit(const std::string& dir) {
    auto r = runGit({"rev-parse", "--verify", "HEAD"}, dir, 30);
    if (!r.ok()) {
        return std::nullopt;
    }
    std::string c = r.out;
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) {
        c.pop_back();
    }
    return c.empty() ? std::nullopt : std::optional<std::string>{c};
}

/// worktree 条目 (git worktree list --porcelain 输出行)
struct WorktreeEntry {
    std::string path;     ///< 绝对路径
    std::string head;     ///< HEAD 提交哈希
    std::string branch;   ///< refs/heads/... 或空 (detached/bare)
    bool        bare     = false;
    bool        detached = false;
};

/// 列举仓库全部 worktree (解析失败返回空列表)
inline std::vector<WorktreeEntry> listWorktrees(const std::string& repoRootDir) {
    std::vector<WorktreeEntry> out;
    auto r = runGit({"worktree", "list", "--porcelain"}, repoRootDir, 60);
    if (!r.ok()) {
        return out;
    }
    WorktreeEntry cur;
    bool          hasCur = false;
    auto          flush  = [&]() {
        if (hasCur) {
            out.push_back(cur);
        }
        cur    = {};
        hasCur = false;
    };
    size_t pos = 0;
    auto   nextLine = [&](std::string& line) {
        if (pos > r.out.size()) {
            return false;
        }
        auto nl = r.out.find('\n', pos);
        line.assign(r.out, pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        pos = (nl == std::string::npos) ? r.out.size() + 1 : nl + 1;
        return true;
    };
    std::string line;
    while (nextLine(line)) {
        if (line.rfind("worktree ", 0) == 0) {
            flush();
            cur.path = line.substr(9);
            hasCur   = true;
        } else if (line.rfind("HEAD ", 0) == 0) {
            cur.head = line.substr(5);
        } else if (line.rfind("branch ", 0) == 0) {
            std::string b = line.substr(7);
            constexpr std::string_view refsHeads = "refs/heads/";
            if (b.rfind(refsHeads, 0) == 0) {
                b.erase(0, refsHeads.size());
            }
            cur.branch = b;
        } else if (line == "bare") {
            cur.bare = true;
        } else if (line == "detached") {
            cur.detached = true;
        } else if (line.empty()) {
            flush();
        }
    }
    flush();
    return out;
}

/// worktree 目录路径: {repoRoot}/.agentxx/agent/worktrees
inline std::string worktreesRoot(const std::string& repoRootDir) {
    return (std::filesystem::path{repoRootDir} / std::filesystem::path{kWorktreesDirName})
        .generic_string();
}

/// 由名称推导分支名
inline std::string branchForName(std::string_view name) {
    return fmt::format("{}{}", kBranchPrefix, name);
}

/// 把指定 pattern 追加进 .git/info/exclude (本地生效, 不污染 tracked 的 .gitignore)。
/// 已存在相同行时幂等返回。失败仅记日志不致命 (untracked 显示不影响功能)
inline void ensureInfoExcluded(const std::string& repoRootDir, std::string_view pattern) {
    std::error_code ec;
    auto excludePath = std::filesystem::path{repoRootDir} / ".git" / "info" / "exclude";
    std::string existing;
    {
        std::ifstream in{excludePath, std::ios::binary};
        if (in) {
            existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
        }
    }
    // 按行精确匹配检查 (忽略 \r)
    size_t s = 0;
    while (s <= existing.size()) {
        auto nl = existing.find('\n', s);
        size_t e = (nl == std::string::npos) ? existing.size() : nl;
        size_t len = e - s;
        while (len > 0 && (existing[s + len - 1] == '\r' || existing[s + len - 1] == ' ')) {
            --len;
        }
        if (len == pattern.size() && existing.compare(s, len, pattern.data(), pattern.size()) == 0) {
            return; // 已存在
        }
        if (nl == std::string::npos) {
            break;
        }
        s = nl + 1;
    }
    auto parent = excludePath.parent_path();
    if (!std::filesystem::exists(parent, ec) && !std::filesystem::create_directories(parent, ec)) {
        XX_LOGW("worktree: cannot create {}, skip exclude update", parent.generic_string());
        return;
    }
    std::ofstream out{excludePath, std::ios::app | std::ios::binary};
    if (!out) {
        XX_LOGW("worktree: cannot open {}, skip exclude update", excludePath.generic_string());
        return;
    }
    if (!existing.empty() && existing.back() != '\n') {
        out << '\n';
    }
    out << "# agentxx worktrees (auto)" << '\n' << pattern << '\n';
}

/// 工作区状态摘要 (基于 status --porcelain=v1 -b)
struct StatusSummary {
    size_t modified  = 0;  ///< M (staged/unstaged modified)
    size_t added     = 0;  ///< A
    size_t deleted   = 0;  ///< D
    size_t renamed   = 0;  ///< R
    size_t untracked = 0;  ///< ?? 条目数
    size_t ahead     = 0;  ///< 领先上游提交数 (无上游时为 0 且 hasUpstream=false)
    size_t behind    = 0;
    bool   hasUpstream = false;
    /// 未合并到主干参照的提交数 (保护性统计, 见 kUnmergedFallback*)
    /// - 解析顺序: 上游分支 → 主干参照分支 (main/master 等) → 回退整分支提交数
    size_t unmerged   = 0;
    bool   usedFallbackUnmerged = false;
    std::string headLine;  ///< porcelain 首行原文 (## ...)

    /// 是否有未提交的工作成果 (变更或未跟踪文件)
    bool dirtyFiles() const {
        return modified + added + deleted + renamed + untracked > 0;
    }

    /// 是否有未合并的本地提交
    bool unpushedCommits() const {
        return unmerged > 0;
    }

    /// 是否存在任何需要保留确认的工作
    bool hasWork() const {
        return dirtyFiles() || unpushedCommits();
    }
};

/// 无上游时依次尝试的主干参照分支 (用于 unmerged 统计)
inline const std::vector<std::string>& unmergedBaseCandidates() {
    static const std::vector<std::string> kCandidates{"main", "master", "trunk", "develop"};
    return kCandidates;
}

/// 统计 worktree 分支上未合并的提交数 (删除保护的依据)
/// - 有上游: rev-list --count @{upstream}..HEAD
/// - 无上游: 依次尝试主干参照分支 rev-list --count <base>..HEAD;
///   全部不可用时回退整分支提交数 (保守策略: 新 worktree 至少含初始提交,
///   必然 >0 → 删除需显式确认, 符合"默认保留"语义)
inline size_t countUnmergedCommits(const std::string& worktreePath, const std::string& branch) {
    auto cntOf = [](const GitResult& r) -> size_t {
        if (!r.ok()) {
            return 0;
        }
        return (size_t)std::strtoull(r.out.c_str(), nullptr, 10);
    };
    // 1) 上游分支
    auto r = runGit({"rev-list", "--count", "@{upstream}..HEAD"}, worktreePath, 30);
    if (r.ok()) {
        return cntOf(r);
    }
    // 2) 主干参照分支
    for (const auto& base : unmergedBaseCandidates()) {
        if (!branch.empty() && base == branch) {
            continue; // 参照不能是当前分支自身
        }
        r = runGit({"rev-list", "--count", fmt::format("{}..HEAD", base)}, worktreePath, 30);
        if (r.ok()) {
            return cntOf(r);
        }
    }
    // 3) 回退: 整分支提交数 (保守, 防误删已提交但未合并的工作)
    r = runGit({"rev-list", "--count", "HEAD"}, worktreePath, 30);
    return cntOf(r);
}

/// 解析 status --porcelain=v1 -b 输出 (失败返回 nullopt)
inline std::optional<StatusSummary> statusSummary(const std::string& worktreePath) {
    auto r = runGit({"status", "--porcelain=v1", "-b"}, worktreePath, 60);
    if (!r.ok()) {
        return std::nullopt;
    }
    StatusSummary st;
    size_t pos = 0;
    bool   first = true;
    std::string branch;
    while (pos <= r.out.size()) {
        auto nl = r.out.find('\n', pos);
        if (nl == std::string::npos && pos > r.out.size()) {
            break;
        }
        std::string line = r.out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        pos = (nl == std::string::npos) ? r.out.size() + 1 : nl + 1;
        if (first) {
            first    = false;
            st.headLine = line;
            // ## HEAD(no branch)/## main...origin/main [ahead 1, behind 2]
            if (line.rfind("## ", 0) == 0) {
                std::string headPart = line.substr(3);
                // 去掉跟踪信息与提示尾注, 提取纯分支名
                auto dots = headPart.find("...");
                if (dots != std::string::npos) {
                    headPart.erase(dots);
                }
                for (auto* sep : {" [", " ["}) {
                    auto p2 = headPart.find(sep);
                    if (p2 != std::string::npos) {
                        headPart.erase(p2);
                    }
                }
                if (headPart.rfind("No commits yet on ", 0) == 0) {
                    headPart.erase(0, std::strlen("No commits yet on "));
                }
                if (headPart != "HEAD (no branch)") {
                    branch = headPart;
                }
            }
            auto aheadPos = st.headLine.find("ahead ");
            if (aheadPos != std::string::npos) {
                st.ahead = (size_t)std::strtoull(st.headLine.c_str() + aheadPos + 6, nullptr, 10);
            }
            auto behindPos = st.headLine.find("behind ");
            if (behindPos != std::string::npos) {
                st.behind = (size_t)std::strtoull(st.headLine.c_str() + behindPos + 7, nullptr, 10);
            }
            st.hasUpstream = st.headLine.find("...") != std::string::npos;
            continue;
        }
        if (line.size() < 2) {
            continue;
        }
        char x = line[0], y = line[1];
        if (x == '?' && y == '?') {
            ++st.untracked;
        } else if (x == 'R' || y == 'R') {
            ++st.renamed;
        } else if (x == 'A' || y == 'A') {
            ++st.added;
        } else if (x == 'D' || y == 'D') {
            ++st.deleted;
        } else {
            ++st.modified;
        }
    }
    st.unmerged = countUnmergedCommits(worktreePath, branch);
    return st;
}

/// 创建 worktree: `git worktree add -b {branch} {path} [baseRef]`
/// - name 必须已通过 isValidWorktreeName 校验
/// - baseRef 为空时基于当前 HEAD (git 默认行为)
/// - 成功后自动维护 .git/info/exclude
/// 返回的 GitResult.ok() 表示创建成功
inline GitResult createWorktree(
    const std::string& repoRootDir,
    std::string_view   name,
    const std::string& baseRef = {}
) {
    GitResult fail;
    if (!isValidWorktreeName(name)) {
        fail.err = fmt::format("invalid worktree name '{}'", name);
        return fail;
    }
    std::error_code ec;
    auto rootDir = std::filesystem::path{worktreesRoot(repoRootDir)};
    std::filesystem::create_directories(rootDir, ec);

    auto wtPath = (rootDir / std::string{name}).generic_string();
    if (std::filesystem::exists(wtPath, ec)) {
        fail.err = fmt::format("worktree directory already exists: {}", wtPath);
        return fail;
    }

    std::vector<std::string> args{"worktree", "add", "-b", branchForName(name), wtPath};
    if (!baseRef.empty()) {
        args.push_back(baseRef);
    }
    auto r = runGit(args, repoRootDir);
    if (!r.ok()) {
        // 失败时清理可能的残留空目录 (git 失败一般不留目录, 兜底)
        std::filesystem::remove(wtPath, ec);
        return r;
    }
    ensureInfoExcluded(repoRootDir, ".agentxx/");
    return r;
}

/// 删除 worktree: `git worktree remove [--force] {path}` + 删除自动分支 (best effort)
/// - 保护检查: force=false 时若工作区有未提交变更/未跟踪文件/未合并提交,
///   拒绝删除并在 err 中返回摘要 (与工具层语义一致, 双层防护)
inline GitResult removeWorktree(const std::string& repoRootDir, const std::string& wtPath, bool force) {
    GitResult refused;
    if (!force) {
        if (auto st = statusSummary(wtPath); st && st->hasWork()) {
            refused.err = fmt::format(
                "worktree '{}' has pending work (modified:{} added:{} deleted:{} renamed:{} "
                "untracked:{} unmerged-commits:{}); commit them first or pass force=true",
                std::filesystem::path{wtPath}.filename().generic_string(),
                st->modified,
                st->added,
                st->deleted,
                st->renamed,
                st->untracked,
                st->unmerged
            );
            return refused;
        }
    }
    std::vector<std::string> args{"worktree", "remove"};
    if (force) {
        args.push_back("--force");
    }
    args.push_back(wtPath);
    auto r = runGit(args, repoRootDir);
    if (!r.ok()) {
        return r;
    }
    // 删除本工具创建的自动分支 (best effort; 用户自建分支不动)
    std::string leaf = std::filesystem::path{wtPath}.filename().generic_string();
    runGit({"branch", "-D", branchForName(leaf)}, repoRootDir, 30);
    // 清理空的父目录链 (.agentxx/agent/worktrees → .agentxx → 仓库根止步)
    std::error_code ec;
    auto dir = std::filesystem::path{wtPath}.parent_path();
    auto stop = std::filesystem::path{repoRootDir};
    while (dir != stop && !dir.empty()) {
        if (!std::filesystem::is_empty(dir, ec) || ec) {
            break;
        }
        if (!std::filesystem::remove(dir, ec) || ec) {
            break;
        }
        dir = dir.parent_path();
    }
    return r;
}

/// 按 worktree 名删除 (等价 removeWorktree(repoRoot, worktreesRoot/name, force))
inline GitResult removeWorktreeByName(const std::string& repoRootDir, std::string_view name, bool force) {
    return removeWorktree(
        repoRootDir,
        (std::filesystem::path{worktreesRoot(repoRootDir)} / std::string{name}).generic_string(),
        force
    );
}

} // namespace worktree
} // namespace util
} // namespace agentxx
