/// agentxx_filesystem 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
/// - 从 libagentxx src/tools/filesystem 拆分: 同名工具同行为
///     list / read / write / edit / glob / grep
/// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
#pragma once

#include "agentxx/util/exception.h"
#include "asio/any_io_executor.hpp"
#include "asio/error.hpp"
#include "asio/read.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/stream_file.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include "glob/glob.h"
#include "utilxx/aho_corasick.h"
#include "utilxx/regex.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "utilxx_base/system.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace agentxx_fs_plugin {

/// 取消查询回调 (返回 true 表示会话已取消); 测试可传 nullptr 等价无取消支持
using IsCancelledFn = std::function<bool()>;

/// 路径权限过滤回调 (批量): 入参为工具枚举出的**完整路径数组**, 返回等长允许标记
/// (1 = 已明确允许); 返回空数组表示查询不可用, 调用方跳过过滤 (保持原行为)
/// - 由模式类工具 (glob / grep) 使用: 声明的权限目标只描述"扫描起点"
///   (决定是否询问用户一次), 模式实际展开出的路径在此逐项复核
using PathFilterFn = std::function<std::vector<uint8_t>(const std::vector<std::string>&)>;

/// 超时上下文: 循环内经 expired() 轮询 (deadline <= 0 表示不限时)
struct Deadline {
    std::chrono::steady_clock::time_point point{};
    int64_t                               seconds = 0;

    static Deadline after(int64_t seconds) {
        Deadline d;
        d.seconds = seconds;
        if (seconds > 0) {
            d.point = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
        }
        return d;
    }

    bool enabled() const {
        return seconds > 0;
    }

    bool expired() const {
        return seconds > 0 && std::chrono::steady_clock::now() >= point;
    }
};

namespace detail {

/// 将 std::filesystem::path 转换为无损 UTF-8 字符串
inline std::string toUtf8(const std::filesystem::path& p) {
    return utilxx_base::pathToUtf8Generic(p);
}

/// 基于 workDir 的会话工作目录解析绝对路径
/// - workDir 非空时以其为相对路径基准 (会话工作目录与进程 cwd 解耦);
///   为空时回退进程 cwd (resolvedWorkDir 兜底, 与单参 toCurrentSystemAbsolutePath 一致)
inline std::string wsAbs(const std::string& workDir, const std::string& path) {
    return utilxx_base::toCurrentSystemAbsolutePath(path, workDir);
}

// ---------------------------------------------------------------------------
// 按规范化文件路径排队执行
//
// 目的: 写文件与"读-改-写"(edit) 操作在同一进程内按**规范化绝对路径**互斥, 避免
// 两个会话(或同一会话的两次工具调用) 并发改同一文件时互相覆盖 (丢失更新)。
//
// 形状与边界:
// - 表是按路径的短生命周期门闩, **不属于插件实例状态**: 同一动态库内所有实例共享
//   同一张表 —— 这正是需要的行为 (两个 agent 实例写同一文件同样要排队);
// - 表项只保存 `mutex + locked 标记 + 引用计数`, 引用计数归零且未加锁时立即删除,
//   不会随"历史上访问过的路径数"增长 (见 [pathLockStats] 的 entries 计数);
// - 同步执行体用阻塞等待, 协程执行体用 1ms 间隔的 try-lock 轮询 (单个文件操作通常
//   在毫秒级, 轮询次数可忽略); 两者共用同一个 locked 标记, 因此不会同时进入;
// - 只读操作 (list/read/glob/grep) 不加门闩: 它们本就声明并行安全, 且门闩只解决
//   "写-写/写-改" 冲突, 读到的中间态由调用方的调用顺序决定。
// ---------------------------------------------------------------------------

/// 规范化路径键: 已存在部分解析软链接/`..` (weakly_canonical), 其余按词法归一
/// - 同一文件的不同写法 (相对/绝对/`..`/软链接) 归一到同一把门闩
inline std::string pathLockKey(std::string_view absolutePath) {
    auto            p  = utilxx_base::utf8ToPath(std::string{absolutePath});
    std::error_code ec;
    auto            canonical = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        canonical = p.lexically_normal();
    } else {
        canonical = canonical.lexically_normal();
    }
    return utilxx_base::pathToUtf8Generic(canonical);
}

/// 路径门闩表 (进程内按动态库共享)
class PathLockTable {
public:

    struct Entry {
        std::mutex mutex;
        bool       locked = false;
        size_t     refs   = 0;
    };

    /// 单个路径的持有凭证 (RAII): 析构即解锁并归还表项
    class Lease {
    public:

        Lease(std::shared_ptr<Entry> entry, std::string key) :
            entry_(std::move(entry)),
            key_(std::move(key)) {}

        ~Lease() {
            if (!entry_) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(entry_->mutex);
                entry_->locked = false;
            }
            PathLockTable::instance().release(key_, entry_);
        }

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        /// 是否真正持有门闩 (暂不支持超时, 恒为 true; 保留接口便于将来扩展)
        bool ownsLock() const noexcept {
            return entry_ != nullptr;
        }

        const std::string& key() const noexcept {
            return key_;
        }

    private:

        std::shared_ptr<Entry> entry_;
        std::string            key_;
    };

    static PathLockTable& instance() {
        static PathLockTable table;
        return table;
    }

    /// 取表项 (引用计数 +1; 不存在则创建)
    std::shared_ptr<Entry> acquireEntry(const std::string& key) {
        std::lock_guard<std::mutex> lock(tableMutex_);
        auto&                       slot = table_[key];
        if (!slot) {
            slot = std::make_shared<Entry>();
        }
        slot->refs++;
        return slot;
    }

    /// 归还表项 (引用计数 -1; 归零且未加锁时删除, 避免表无限增长)
    void release(const std::string& key, const std::shared_ptr<Entry>& entry) {
        std::lock_guard<std::mutex> lock(tableMutex_);
        auto                        it = table_.find(key);
        if (it == table_.end()) {
            return;
        }
        if (entry && entry->refs > 0) {
            entry->refs--;
        }
        if (it->second && it->second->refs == 0) {
            bool locked = false;
            {
                std::lock_guard<std::mutex> entryLock(it->second->mutex);
                locked = it->second->locked;
            }
            if (!locked) {
                table_.erase(it);
            }
        }
    }

    /// 表内条目数 (测试断言"用完即删")
    size_t entryCount() {
        std::lock_guard<std::mutex> lock(tableMutex_);
        return table_.size();
    }

private:

    PathLockTable() = default;

    std::mutex                                              tableMutex_;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> table_;
};

/// 同步获取路径门闩 (阻塞直到拿到)
inline std::shared_ptr<PathLockTable::Lease> lockPathBlocking(const std::string& absolutePath) {
    const auto                    key   = pathLockKey(absolutePath);
    std::shared_ptr<PathLockTable::Entry> entry = PathLockTable::instance().acquireEntry(key);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (!entry->locked) {
                entry->locked = true;
                return std::make_shared<PathLockTable::Lease>(entry, key);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

/// 协程获取路径门闩 (等待时按 1ms 间隔重试)
inline asio::awaitable<std::shared_ptr<PathLockTable::Lease>>
    lockPathAsync(std::string absolutePath) {
    const auto                              key   = pathLockKey(absolutePath);
    std::shared_ptr<PathLockTable::Entry>   entry = PathLockTable::instance().acquireEntry(key);
    const auto                              executor = co_await asio::this_coro::executor;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (!entry->locked) {
                entry->locked = true;
                co_return std::make_shared<PathLockTable::Lease>(entry, key);
            }
        }
        asio::steady_timer timer{executor};
        timer.expires_after(std::chrono::milliseconds{1});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}

/// 路径门闩表统计 (测试用: entries 应当在使用结束后回到 0)
inline PathLockTable& pathLockTable() {
    return PathLockTable::instance();
}

/// 将文本中的 CRLF (`\r\n`) 行尾统一转换为 LF (`\n`)。
inline void normalizeCrlfToLf(std::string& text) {
    utilxx_base::normalizeCrlfToLf(text);
}

/// 判断路径是否含 shell 通配符 (`*` `?` `[`)。
/// 与 glob 库内部判定 (has_magic) 及 bash 一致: 只有这三个字符触发通配展开,
/// `]` 单独出现不触发
inline bool hasGlobMagic(std::string_view path) {
    return path.find_first_of("*?[") != std::string_view::npos;
}

/// 解析 `type` 参数为类型集合。支持 string 或 array 两种形式。
/// 返回空集合表示 "any" (不按类型过滤)。合法值: file / dir / symlink / other / any。
inline std::set<std::string> collectTypeFilter(const utilxx_base::Json& typeArg) {
    std::set<std::string> types;
    auto                  addOne = [&](const std::string& t) {
        if (t == "any" || t.empty()) {
            return;
        }
        types.insert(t);
    };
    if (typeArg.is_string()) {
        addOne(typeArg.get<std::string>());
    } else if (typeArg.is_array()) {
        for (const auto& item : typeArg) {
            if (item.is_string()) {
                addOne(item.get<std::string>());
            }
        }
    }
    return types;
}

/// 获取路径实体的类型字符串 (与 filesystem_glob 的 type 参数语义一致)。
/// 先判 symlink 以准确识别符号链接 (即使其指向目录)。
inline std::string fileTypeOf(const std::filesystem::path& path) {
    std::error_code ec;
    auto            status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        return "other";
    }
    if (std::filesystem::is_symlink(status)) {
        return "symlink";
    }
    if (std::filesystem::is_directory(status)) {
        return "dir";
    }
    if (std::filesystem::is_regular_file(status)) {
        return "file";
    }
    return "other";
}

/// 预编译 exclude_patterns 为正则列表 (非法模式静默忽略)。
inline std::vector<std::regex> compileExcludeRegexes(const std::vector<std::string>& excludePatterns
) {
    std::vector<std::regex> regexes;
    regexes.reserve(excludePatterns.size());
    for (const auto& ep : excludePatterns) {
        // 非法模式忽略
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                regexes.emplace_back(glob::to_regex(ep));
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
    }
    return regexes;
}

/// 判断路径 (generic_string) 是否命中任一 exclude 正则。
inline bool isExcluded(const std::string& pathStr, const std::vector<std::regex>& excludeRegexes) {
    for (const auto& re : excludeRegexes) {
        if (std::regex_match(pathStr, re)) {
            return true;
        }
    }
    return false;
}

/// 构造遍历策略 ([glob::WalkPolicy]): 排除过滤 + 匹配结果数量上限, 二者都在
/// **遍历过程中**生效 (而不是遍历完再过滤/再判断) —— 这才能省掉被排除大目录的
/// 遍历开销, 也才能在超限时立刻停止而不是先把几十万条路径读进内存。
///
/// 排除语义 (与"匹配到的路径被排除"一致, 并对覆盖子树的模式做剪枝优化):
/// - 命中的**文件**直接跳过 (不进结果, 也不计入数量上限);
/// - 命中的**目录**:
///     - 模式覆盖整棵子树 (除首段外含 `**`, 如 `**/build/**`、`build/**`) 时整棵
///       子树剪枝 —— 这是跳过 build/third_party 这类大目录的主要收益;
///     - 只匹配目录自身或单层的模式 (如 `**/build/*`、`build`) 命中目录时仍继续
///       向下遍历, 仅由结果集后置复核剔除该目录条目, 避免误删未被模式覆盖的后代;
/// - `maxFiles`: 匹配结果数量上限 (>0 生效); 到限时**立即停止遍历并返回已匹配到的
///   结果** (设置 [glob::WalkPolicy::stopOnLimit], 不抛
///   [glob::walk_limit_exceeded]) —— 调用方据 [glob::WalkPolicy::limitReached]
///   在输出首行追加提示, 并把已匹配结果照常返回给调用方, 而不是丢掉全部结果
inline glob::WalkPolicy
    makeWalkPolicy(const std::vector<std::string>& excludePatterns, int64_t maxFiles) {
    glob::WalkPolicy policy;
    policy.maxResults  = (maxFiles > 0) ? static_cast<size_t>(maxFiles) : 0;
    policy.stopOnLimit = true;
    if (false == excludePatterns.empty()) {
        std::vector<std::string> prunePatterns; // 覆盖整棵子树的排除模式 (可剪枝)
        for (const auto& pattern : excludePatterns) {
            if (glob::is_subtree_exclude_pattern(pattern)) {
                prunePatterns.push_back(pattern);
            }
        }
        auto allRegexes   = compileExcludeRegexes(excludePatterns);
        auto pruneRegexes = compileExcludeRegexes(prunePatterns);
        if (false == allRegexes.empty()) {
            policy.keepPath = [allRegexes   = std::move(allRegexes),
                               pruneRegexes = std::move(pruneRegexes
                               )](const std::filesystem::path& p, bool isDir) -> bool {
                const auto pathStr = toUtf8(p);
                if (false == isDir) {
                    return false == isExcluded(pathStr, allRegexes);
                }
                // 目录: 未被任何模式命中 -> 保留 + 继续遍历
                if (false == isExcluded(pathStr, allRegexes)
                    && false == isExcluded(pathStr + "/", allRegexes)) {
                    return true;
                }
                // 目录被排除: 模式覆盖整棵子树时剪枝; 否则保留以便继续遍历
                // (该目录条目本身由结果集的后置复核剔除)
                return false == isExcluded(pathStr, pruneRegexes)
                       && false == isExcluded(pathStr + "/", pruneRegexes);
            };
        }
    }
    return policy;
}

/// 读取完整文件文本 (同步); 打开失败抛出异常
inline std::string readFileContent(const std::string& filepath) {
    auto            p = utilxx_base::utf8ToPath(filepath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(p, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(p, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }

    std::ifstream stream;
    stream.open(p, std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::generic_category()};
        throw std::runtime_error(fmt::format(R"(Can not open file. Error: {})", ec.message()));
    }
    auto result
        = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    return result;
}

/// 拼接多行文本 (与命令行 ls 一致, 行间以 `\n` 分隔, 末尾不加换行)
inline std::string joinLines(const std::vector<std::string>& lines) {
    std::string output;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) {
            output += '\n';
        }
        output += lines[i];
    }
    return output;
}

/// 按权限规则逐项过滤路径列表 (模式类工具用: 模式展开后复核实际路径)
/// - 仅保留"已明确允许"的路径: 被拒绝 (Deny) 与未获批准 (Ask) 都不进入结果,
///   避免模式展开绕过针对子目录的拒绝规则
/// - [pathFilter] 为空或查询不可用 (返回空标记数组) 时不做过滤: 保持原行为,
///   且不会把所有路径误判为拒绝
///
/// - `return` 过滤后的路径列表 (顺序不变)
inline std::vector<std::filesystem::path>
    filterByPermission(std::vector<std::filesystem::path> paths, const PathFilterFn& pathFilter) {
    if (!pathFilter || paths.empty()) {
        return paths;
    }
    std::vector<std::string> candidates;
    candidates.reserve(paths.size());
    for (const auto& p : paths) {
        candidates.push_back(toUtf8(p));
    }
    // 批量查询 (过滤器内部按批调用主程序并对相同路径去重)
    const auto allowed = pathFilter(candidates);
    if (allowed.size() != candidates.size()) {
        // 主程序不支持路径查询或调用失败: 跳过过滤 (日志留痕便于排查)
        XX_LOGW("filesystem: path permission query unavailable, path filtering skipped");
        return paths;
    }
    std::vector<std::filesystem::path> kept;
    kept.reserve(paths.size());
    size_t dropped = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (allowed[i] != 0) {
            kept.push_back(std::move(paths[i]));
        } else {
            ++dropped;
        }
    }
    if (dropped > 0) {
        XX_LOGD("filesystem: {} path(s) excluded by permission rules", dropped);
    }
    return kept;
}

} // namespace detail

// =====================================================================
// agentxx_filesystem_list 执行体 (原 FileSystemListTool::execute_async)
// =====================================================================
inline std::string fileListExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    auto rawPath    = arguments.value("path", std::string{});
    auto targetPath = detail::wsAbs(workDir, rawPath);
    if (targetPath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto recursive = arguments.value("recursive", false);
    auto limit     = arguments.value<int64_t>("limit", 100);
    // 通配展开的条目上限 (防止 `agent/**/*` 这类模式一次展开出几十万条路径):
    // 超过则只取前 N 条 (排序后) 并在输出首行说明; 0 表示不限
    auto maxFiles = arguments.value<int64_t>("max_files", 1000);
    auto timeout  = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline = Deadline::after(timeout);

    std::vector<std::string> lines;

    auto onAppendItem = [&](const std::filesystem::directory_entry& entity) {
        // 单个条目处理失败仅记录错误行, 不中断整个列表
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                auto file_time = entity.last_write_time();

                auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    file_time - std::filesystem::file_time_type::clock::now()
                    + std::chrono::system_clock::now()
                );

                // 类型标识列 (仿 ls -l): 首字符为真实类型 (d/-/l/?), 权限位无数据用通用占位
                std::string typeStr = "??????????";
                std::string sizeStr = "-";
                if (entity.is_directory()) {
                    typeStr = "drwxr-xr-x";
                } else if (entity.is_regular_file()) {
                    typeStr = "-rw-r--r--";
                    sizeStr = std::to_string(entity.file_size());
                } else if (entity.is_symlink()) {
                    typeStr = "lrwxrwxrwx";
                }

                // 路径列: 目录加 `/` 后缀, 符号链接附加指向目标 (对齐 ls -l)
                auto pathStr = detail::toUtf8(entity.path());
                if (entity.is_directory()) {
                    pathStr += "/";
                } else if (entity.is_symlink()) {
                    std::error_code ec;
                    auto            target = std::filesystem::read_symlink(entity.path(), ec);
                    if (!ec) {
                        pathStr += fmt::format(" -> {}", detail::toUtf8(target));
                    }
                }

                auto timeStr = std::format("{:%Y-%m-%d %H:%M}", sys_time);
                lines.push_back(fmt::format("{} {} {} {}", typeStr, sizeStr, timeStr, pathStr));
                return true;
            },
            [&](std::string errmsg) -> bool {
                lines.push_back(fmt::format("[Error: {}: {}]", detail::toUtf8(entity.path()), errmsg)
                );
                return false;
            }
        );
    };

    auto checkStop = [&]() -> bool {
        // 返回 true 表示应提前终止 (取消或超时)
        if (isCancelled && isCancelled()) {
            lines.push_back("[Error: cancelled]");
            return true;
        }
        if (deadline.expired()) {
            lines.push_back(fmt::format(
                R"([Error: timed out after {} seconds. Try narrowing the path or setting a limit.])",
                timeout
            ));
            return true;
        }
        return false;
    };

    /// 是否已达到输出条目上限 (limit <= 0 表示不限)
    auto limitReached = [&]() -> bool {
        return limit > 0 && static_cast<int64_t>(lines.size()) >= limit;
    };

    // 通配模式下 recursive 的目录下钻结果可能与 `**` 匹配结果重叠 (如 `src/**` 已含
    // 全部后代, 再叠加 recursive 会重复列出), 此时按路径去重; 普通路径模式不去重
    bool                  dedupByPath = false;
    std::set<std::string> listedPaths;

    /// 权限过滤批次大小: 条目先入待处理缓冲, 满批后批量查询权限再输出
    constexpr size_t                              kPermissionBatch = 256;
    std::vector<std::filesystem::directory_entry> pending;
    size_t                                        excludedTotal = 0;

    /// 输出待处理条目 (按权限过滤后输出 "已明确允许" 的条目)
    /// - 只在这里应用输出上限: 达到 limit 后丢弃剩余待处理条目 (与未过滤时
    ///   "读到一个条目就判断上限" 的输出结果一致)
    /// - 不在此检查取消/超时: 由外层循环处理 (避免重复输出终止提示行)
    /// - 查询不可用 (主程序未装配权限中间件/失败) 时不过滤, 保持原行为
    auto flushPending = [&]() {
        if (pending.empty()) {
            return;
        }
        if (pathFilter) {
            std::vector<std::string> paths;
            paths.reserve(pending.size());
            for (const auto& entity : pending) {
                paths.push_back(detail::toUtf8(entity.path()));
            }
            const auto allowed = pathFilter(paths);
            if (allowed.size() == paths.size()) {
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (allowed[i] == 0) {
                        ++excludedTotal;
                        continue;
                    }
                    if (limitReached()) {
                        break;
                    }
                    onAppendItem(pending[i]);
                }
                pending.clear();
                return;
            }
            XX_LOGW("filesystem_list: path permission query unavailable, path filtering skipped");
        }
        for (const auto& entity : pending) {
            if (limitReached()) {
                break;
            }
            onAppendItem(entity);
        }
        pending.clear();
    };

    /// 追加一个条目 (按路径去重生效时同一路径只输出一次)
    /// - 条目先进入待处理缓冲, 满批后经权限批量查询再输出: `limit` 只统计真正
    ///   输出的条目, 且避免"每条目一次跨线程查询"
    auto appendEntry = [&](const std::filesystem::directory_entry& entity) {
        if (dedupByPath && false == listedPaths.insert(detail::toUtf8(entity.path())).second) {
            return;
        }
        pending.push_back(entity);
        if (pending.size() >= kPermissionBatch) {
            flushPending();
        }
    };

    /// 列出目录内容 (recursive 为 true 时递归子目录); 返回 true 表示应停止
    /// (已取消 / 超时 / 达到 limit)
    auto appendDirContents = [&](const std::filesystem::path& fsPath) -> bool {
        // skip_permission_denied: 单个不可读目录被跳过而非中断整个列表
        auto options = std::filesystem::directory_options::skip_permission_denied;
        if (recursive) {
            for (const auto& entity :
                 std::filesystem::recursive_directory_iterator(fsPath, options)) {
                if (checkStop() || limitReached()) {
                    return true;
                }
                appendEntry(entity);
            }
            return false;
        }
        for (const auto& entity : std::filesystem::directory_iterator(fsPath, options)) {
            if (checkStop() || limitReached()) {
                return true;
            }
            appendEntry(entity);
        }
        return false;
    };

    // ---- 通配模式展开 (`*` `?` `[`) ----
    // path 含 shell 通配符时按 bash 语义展开 (与 agentxx_filesystem_glob 同一 glob
    // 实现: 大小写敏感、不匹配 `.` 开头的隐藏条目、`**` 表示递归任意层级),
    // 输出匹配到的条目本身; recursive 为 true 时再下钻匹配到的目录
    // (recursive 始终表示"展开子目录", 与普通目录路径的语义一致)
    std::vector<std::filesystem::path> matched;
    if (detail::hasGlobMagic(targetPath)) {
        // 展开期间无法回调 isCancelled (glob 库只接受 atomic 标志), 与 glob 工具一致
        // 传恒假标志, 取消/超时由外层 checkStop 在条目之间轮询
        std::atomic<bool> globNeverCancel{false};
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                if (glob::has_recursive_segment(targetPath)) {
                    matched = glob::rglob(targetPath, true, globNeverCancel);
                } else {
                    matched = glob::glob(targetPath, true, globNeverCancel);
                }
                return true;
            },
            [&](std::string errmsg) -> bool {
                // 遍历失败 (如 MSVC 下目录中存在系统代码页无法表示的条目名):
                // 记录日志后按无匹配处理
                XX_LOGW("filesystem_list: glob pattern '{}' failed: {}", targetPath, errmsg);
                return false;
            }
        );

        if (false == matched.empty()) {
            // glob 的 `**` 以 `dir/.` 形式返回目录自身, 词法规范化后自带尾部 `/`,
            // 去掉尾部斜杠: 路径写法唯一 (便于显示与按路径去重, 目录的 `/` 后缀由
            // 条目格式化统一补)
            for (auto& item : matched) {
                if (item.filename().empty()) {
                    auto parent = item.parent_path();
                    if (false == parent.empty()) {
                        item = std::move(parent);
                    }
                }
            }
            // 排序去重: 输出顺序只取决于名称 (与 `ls` 一致), 不受目录遍历顺序影响
            std::sort(matched.begin(), matched.end());
            matched.erase(std::unique(matched.begin(), matched.end()), matched.end());

            // 展开条目上限: 超出部分不展开 (输出首行说明, 不做静默丢弃)
            if (maxFiles > 0 && static_cast<int64_t>(matched.size()) > maxFiles) {
                lines.push_back(fmt::format(
                    "[Note] only first {} of {} matched entries are listed (`max_files`); narrow `path` or raise it.",
                    maxFiles,
                    matched.size()
                ));
                matched.resize(static_cast<size_t>(maxFiles));
            }

            // 只有 recursive 下的目录下钻才可能与 `**` 匹配结果重叠, 故仅此时去重
            dedupByPath = recursive;
            for (const auto& item : matched) {
                if (checkStop() || limitReached()) {
                    break;
                }
                appendEntry(std::filesystem::directory_entry{item});
                std::error_code ec;
                if (recursive && std::filesystem::is_directory(item, ec)) {
                    if (appendDirContents(item)) {
                        break;
                    }
                }
            }
            flushPending();
            return detail::joinLines(lines);
        }

        // 通配符无匹配: 字面路径确实存在时 (文件名本身含 `*`/`[` 等) 按普通路径处理,
        // 与 shell "无匹配时保留模式原样" 的行为一致
        std::error_code ec;
        if (false == std::filesystem::exists(utilxx_base::utf8ToPath(targetPath), ec)) {
            lines.push_back(fmt::format(R"([Error: no match `path`({}) file found])", rawPath));
            return detail::joinLines(lines);
        }
    }

    auto fsPath = utilxx_base::utf8ToPath(targetPath);
    if (false == std::filesystem::exists(fsPath)) {
        lines.push_back("[Error: path does not exist]");
    } else if (std::filesystem::is_directory(fsPath)) {
        appendDirContents(fsPath);
    } else if (std::filesystem::is_regular_file(fsPath)) {
        // 单文件同样走待处理缓冲, 使权限过滤对"直接指定的文件"同样生效
        // (与 read/write/edit 等按路径判定的工具保持一致)
        appendEntry(std::filesystem::directory_entry(fsPath));
    } else {
        lines.push_back("[Error: path exists, but is not a directory or a file]");
    }

    flushPending();

    // 未触发提前终止且无条目: 空目录提示行, 让 LLM 能区分 "空目录" 与失败
    if (lines.empty()) {
        lines.push_back("[Empty]");
    }
    if (excludedTotal > 0) {
        XX_LOGD("filesystem_list: {} entries excluded by permission rules", excludedTotal);
    }

    // 拼接为多行文本 (与命令行 ls 一致)
    return detail::joinLines(lines);
}

// =====================================================================
// agentxx_filesystem_read 执行体 (原 FilesystemReadTextFileTool::execute_async)
// =====================================================================

namespace detail {

/// 拒绝已改名的分页参数 (`line_offset`/`line_limit` → `offset`/`limit`)
/// - 发现旧名时直接报参数错误: 静默忽略会让模型以为读了指定区间, 实际返回整个文件
///   (read 不做自动摘要, 大文件会直接填满上下文)
inline void rejectLegacyLineArgs(const utilxx_base::Json& arguments) {
    if (arguments.contains("line_offset") || arguments.contains("line_limit")) {
        throw std::invalid_argument{
            "Args `line_offset`/`line_limit` are renamed to `offset`/`limit`"
        };
    }
}

/// 单次 read 的输出上限 (行数 / UTF-8 字节数)
/// - 超过上限直接报错, 不截断: 截断会让调用方以为拿到了完整内容
/// - 依据: 一次把几十万行/几 MB 的文本放进上下文既贵又没用, 定位内容应当先用
///   `offset`/`limit` 分页或 grep 找到目标行
constexpr size_t kReadMaxLines = 2000;
constexpr size_t kReadMaxBytes = 256 * 1024;

/// read 结果超限时报错 (给可行做法; `lines` 未知传 0)
[[noreturn]] inline void throwReadTooLarge(size_t bytes, size_t lines = 0) {
    const auto sizeText = lines > 0 ? fmt::format("{} line(s) / {} bytes", lines, bytes)
                                    : fmt::format("{} bytes", bytes);
    throw std::invalid_argument{fmt::format(
        R"(Read result is too large for one call: {} (limit {} lines / {} bytes). Read it in parts with `offset`/`limit` (both counted in lines), or locate the wanted part with `agentxx_filesystem_grep`. A single line longer than the limit can not be split by this tool: use `agentxx_execute_command` (e.g. `head -c`, `cut`, `split`) instead.)",
        sizeText,
        kReadMaxLines,
        kReadMaxBytes
    )};
}

/// 检查 read 结果是否超过单次上限 (超过即报错)
inline void checkReadResultLimit(std::string_view text) {
    if (text.empty()) {
        return;
    }
    size_t lines = 1;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    if (text.back() == '\n') {
        --lines; // 末尾换行符不算新的一行
    }
    if (lines <= kReadMaxLines && text.size() <= kReadMaxBytes) {
        return;
    }
    throwReadTooLarge(text.size(), lines);
}

/// 整文件读取前的体积预检: 超过上限直接报错, 不把超大体量的文件读进内存
/// - 分页读取不受此限制: 只取指定行区间时文件本身可以很大 (只检查读出来的那段)
inline void checkWholeFileSizeBeforeRead(const std::filesystem::path& fsPath) {
    std::error_code ec;
    const auto      size = std::filesystem::file_size(fsPath, ec);
    if (ec || size <= static_cast<uintmax_t>(kReadMaxBytes)) {
        return;
    }
    throwReadTooLarge(static_cast<size_t>(size));
}

} // namespace detail

inline std::string fileReadExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    detail::rejectLegacyLineArgs(arguments);
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto            fsPath = utilxx_base::utf8ToPath(filepath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(fsPath, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(fsPath, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }
    auto text_line_offset = arguments.value<int64_t>("offset", -1);
    auto text_line_limit  = arguments.value<int64_t>("limit", -1);

    /// 同步阻塞读取文件
    std::ifstream stream;
    stream.open(fsPath, std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::generic_category()};
        throw std::runtime_error(fmt::format(R"(Can not open file. Error: {})", ec.message()));
    }

    if (text_line_offset >= 0 || text_line_limit > 0) {
        // 读取部分文件
        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;

        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        for (std::string line; std::getline(stream, line) && lineNum < endLine; lineNum++) {
            // 跳过偏移行
            if (lineNum < offset) {
                continue;
            }
            result << line << "\n";
            // 边读边看体量: 请求的区间过大时立即报错, 不先把整段读进内存
            const auto readLines = lineNum - offset + 1;
            const auto readBytes = static_cast<size_t>(result.tellp());
            if (readLines > detail::kReadMaxLines || readBytes > detail::kReadMaxBytes) {
                detail::throwReadTooLarge(readBytes, readLines);
            }
        }

        stream.close();
        if (lineNum == 0 && offset == 0) {
            // 空文件: 第 0 行视为空行, 返回空串而非报错
            return "";
        }
        if (lineNum <= offset) {
            // offset 超出文件行数
            throw std::runtime_error{fmt::format(
                R"(Arg `offset`({} lines) is out of range of file lines({} lines).)",
                offset,
                lineNum
            )};
        }

        auto rawStr = result.str();
        // 保留原始的 crlf 或 \n 换行符不转换
        utilxx_base::autoConvertToUtf8(rawStr);
        detail::checkReadResultLimit(rawStr);
        return rawStr;
    }

    // 读取完整文件 (先看体量: 超大文件直接报错, 不读进内存)
    detail::checkWholeFileSizeBeforeRead(fsPath);
    auto result
        = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    // 保留原始的 crlf 或 \n 换行符不转换
    utilxx_base::autoConvertToUtf8(result);
    detail::checkReadResultLimit(result);
    return result;
}

// =====================================================================
// agentxx_filesystem_write 执行体 (原 FilesystemWriteFileTool::execute_async)
// =====================================================================
inline std::string fileWriteExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);

    std::ofstream stream;
    auto          path = utilxx_base::utf8ToPath(filepath);
    // 按规范化路径排队: 存在性检查与写入必须相对其它写者原子
    auto pathLease = detail::lockPathBlocking(filepath);
    if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist. Set `overwrite` = true if want to overwrite."
        };
    }
    if (!path.parent_path().empty() && false == std::filesystem::exists(path.parent_path())
        && false == std::filesystem::create_directories(path.parent_path())) {
        // 创建父目录
        throw std::runtime_error{fmt::format(
            R"(Can not create `path`({})'s parent dirs.)",
            detail::toUtf8(path.parent_path())
        )};
    }

    stream.open(path, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{
            fmt::format(R"(Can not create or open file. Error: {})", ec.message())
        };
    }

    if (false == content.empty()) {
        // 写入文本内容
        stream << content;
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format(R"(File created success, but write failed. Error: {})", ec.message())
            };
        }
    }

    stream.close();
    return "success";
}

// =====================================================================
// agentxx_filesystem_edit 执行体 (原 FilesystemEditTextFileTool::execute_async)
// =====================================================================
namespace detail {

/// edit 工具的一段替换 (`old_str` -> `new_str`)
struct EditSegment {
    std::string oldStr;
    std::string newStr;
};

/// 段数上限: 只用于拦住畸形的超大参数 (正常调用不会接近这个数量)
constexpr size_t kEditMaxSegments = 1000;

/// 取 `old_str` 的首行片段 (错误提示里指明"哪一段没匹配上"用)
/// - 按 UTF-8 长度截断, 不把多字节字符切成两半; 超长时追加 `...`
inline std::string editOldStrExcerpt(std::string_view oldStr) {
    constexpr size_t kMaxChars = 60;
    auto             firstLine = oldStr.substr(0, oldStr.find('\n'));
    const auto       cutIndex  = utilxx_base::findIndexByUtf8Length(firstLine, kMaxChars);
    if (cutIndex > 0 && cutIndex < firstLine.size()) {
        return fmt::format("{}...", firstLine.substr(0, cutIndex));
    }
    return std::string{firstLine};
}

/// 解析 edit 工具的替换段:
/// - `edits` 数组 (推荐写法): 逐项取 `old_str`/`new_str`
/// - `edits` 为单个对象: 按单段处理 (等价于只写一段的数组)
/// - 未给 `edits`: 按 `old_str`/`new_str` 取单段 (与旧写法兼容)
/// 每段要求 `old_str` 非空且与 `new_str` 不同; 两处写法同时给出时拒绝
/// (避免调用方以为两处都会生效)
///
/// - `return` 至少一段的替换列表 (顺序即应用顺序)
inline std::vector<EditSegment> parseEditSegments(const utilxx_base::Json& arguments) {
    // 缺失键 → 只读 operator[] 返回 Null 常量
    const auto& editsArg    = arguments["edits"];
    const bool  hasEditsArg = !editsArg.is_null();
    const auto  oldStrArg   = arguments.value<std::string>("old_str", std::string{});
    const auto  newStrArg   = arguments.value<std::string>("new_str", std::string{});

    std::vector<EditSegment> segments;

    /// 从一段的 JSON 取 `old_str`/`new_str` (两键都必须给, 且都是字符串)
    /// - `new_str` 缺失时报错而不是按空串处理: 漏写会变成"删除整段文本",
    ///   删除请显式传空串
    auto takeSegment = [](const utilxx_base::Json& item, size_t index) -> EditSegment {
        if (!item.contains("old_str") || !item["old_str"].is_string()) {
            throw std::invalid_argument{
                fmt::format(R"(Arg `old_str` of edit #{} must be a string)", index)
            };
        }
        if (!item.contains("new_str") || !item["new_str"].is_string()) {
            throw std::invalid_argument{fmt::format(
                R"(Arg `new_str` of edit #{} must be a string (pass an empty string to delete the matched text))",
                index
            )};
        }
        return EditSegment{
            item["old_str"].get<std::string>(),
            item["new_str"].get<std::string>()
        };
    };

    if (hasEditsArg) {
        if (!oldStrArg.empty() || !newStrArg.empty()) {
            throw std::invalid_argument{
                "Arg `edits` and `old_str`/`new_str` are both given, use only one of them."
            };
        }
        // 单对象写法 (少写一层数组): 与只含一段的数组等价
        if (editsArg.is_object()) {
            segments.push_back(takeSegment(editsArg, 1));
        } else if (editsArg.is_array()) {
            segments.reserve(editsArg.size());
            for (const auto& item : editsArg) {
                if (!item.is_object()) {
                    throw std::invalid_argument{
                        "Arg `edits` must be an array of objects like "
                        "`[{\"old_str\": \"...\", \"new_str\": \"...\"}]`."
                    };
                }
                segments.push_back(takeSegment(item, segments.size() + 1));
            }
        } else {
            throw std::invalid_argument{
                "Arg `edits` must be an array of objects like "
                "`[{\"old_str\": \"...\", \"new_str\": \"...\"}]`."
            };
        }
        if (segments.empty()) {
            throw std::invalid_argument{"Arg `edits` is empty"};
        }
        if (segments.size() > kEditMaxSegments) {
            throw std::invalid_argument{
                fmt::format(R"(Arg `edits` has too many segments (max {}))", kEditMaxSegments)
            };
        }
    } else {
        if (oldStrArg.empty()) {
            throw std::invalid_argument{
                "Arg `old_str` is empty (or pass the replacements as `edits`)."
            };
        }
        // 旧写法 (单段): 与数组形态同样要求 `new_str` 明确给出
        if (!arguments.contains("new_str") || !arguments["new_str"].is_string()) {
            throw std::invalid_argument{
                "Arg `new_str` is missing (pass an empty string to delete the matched text)."
            };
        }
        segments.push_back(EditSegment{oldStrArg, newStrArg});
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        if (seg.oldStr.empty()) {
            throw std::invalid_argument{
                fmt::format(R"(Arg `old_str` of edit #{} is empty)", i + 1)
            };
        }
        if (seg.newStr == seg.oldStr) {
            throw std::invalid_argument{
                fmt::format(R"(Arg `old_str` and `new_str` of edit #{} are equal and unchanged.)", i + 1)
            };
        }
    }
    return segments;
}

/// 按顺序把各段应用到 content: 后一段作用于前一段改完的内容 (可用前一段写入的文本)
/// - 每段只替换**第一处**匹配 (要改多处就写多段)
/// - 某段匹配不到时抛 std::runtime_error (调用方据此整体放弃, 不落盘)
inline void applyEditSegments(std::string& content, const std::vector<EditSegment>& segments) {
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        const auto  pos = content.find(seg.oldStr);
        if (pos == std::string::npos) {
            throw std::runtime_error{fmt::format(
                R"(No match `old_str` of edit #{} (`{}`) found, no change was written; try re-reading to get the latest file content.)",
                i + 1,
                editOldStrExcerpt(seg.oldStr)
            )};
        }
        content.replace(pos, seg.oldStr.length(), seg.newStr);
    }
}

} // namespace detail

inline std::string fileEditExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto segments = detail::parseEditSegments(arguments);

    // 统一到 \n 换行符
    // - 与 filesystem_read 的逻辑不同，read 应当保留原始的内容，edit 应当尽可能保证修改成功，
    //   如果 llm 需要写回 crlf，可使用 shell
    for (auto& seg : segments) {
        detail::normalizeCrlfToLf(seg.oldStr);
        detail::normalizeCrlfToLf(seg.newStr);
    }

    auto            path = utilxx_base::utf8ToPath(filepath);
    // 按规范化路径排队: 读-改-写整段相对其它写者原子
    auto            pathLease = detail::lockPathBlocking(filepath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(path, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(path, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }

    // 读取完整文件并预处理 (先转 UTF-8 使 GBK 等编码文件可正常匹配, 再统一换行符)
    std::string content = detail::readFileContent(filepath);
    utilxx_base::autoConvertToUtf8(content);
    detail::normalizeCrlfToLf(content);

    // 任一段匹配不到即整体放弃 (读到内容后才会写盘, 失败时文件保持原样)
    detail::applyEditSegments(content, segments);

    // 原子写: 先写同目录临时文件, 成功后 rename 覆盖原文件,
    // 避免直接 truncate 原文件后写入中途失败导致原内容永久丢失
    static std::atomic<uint64_t> s_editTmpSeq{0};
    const auto                   tmpPathStr
        = fmt::format("{}.agentxx_edit_tmp_{}", filepath, s_editTmpSeq.fetch_add(1));
    const auto fsTmpPath = utilxx_base::utf8ToPath(tmpPathStr);

    {
        std::ofstream stream(
            fsTmpPath,
            std::ios_base::out | std::ios_base::trunc | std::ios_base::binary
        );
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format(R"(Can not open temp file to write: {})", ec.message())
            };
        }
        stream << content;
        stream.close();
        if (!stream) {
            std::error_code rmEc;
            std::filesystem::remove(fsTmpPath, rmEc);
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Write temp file failed: {})", ec.message())};
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(fsTmpPath, path, renameEc);
    if (renameEc) {
        std::error_code rmEc;
        std::filesystem::remove(fsTmpPath, rmEc);
        throw std::runtime_error{
            fmt::format(R"(Failed to replace original file: {})", renameEc.message())
        };
    }

    return "success";
}

// =====================================================================
// agentxx_filesystem_glob 执行体 (原 FilesystemGlobTool::execute_async)
// =====================================================================
inline std::string fileGlobExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        throw std::invalid_argument{"Arg `file_patterns` is empty"};
    }
    auto timeout  = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline = Deadline::after(timeout);

    for (auto& item : file_patterns) {
        item = detail::wsAbs(workDir, item);
    }

    // 注: 路径匹配固定为大小写敏感 (移除 case-insensitive 支持)。历史原因见
    // lib filesystem.cpp 同注释 (case_fold 会破坏 max_depth 前缀计算与盘符识别)。
    auto maxDepth        = arguments.value<int64_t>("max_depth", -1);
    auto doSort          = arguments.value<bool>("sort", false);
    auto typeFilter      = detail::collectTypeFilter(arguments.value("type", utilxx_base::Json{}));
    auto excludePatterns = arguments.value("exclude_patterns", std::vector<std::string>{});
    for (auto& item : excludePatterns) {
        item = detail::wsAbs(workDir, item);
    }
    // 结果条目数上限 (防止 `agent/**/*` 这类模式一次展开出几十万条路径):
    // 遍历过程中到达上限即**停止遍历并保留已匹配结果**, 输出首行以 `[Note]` 说明
    // 结果被截断 (不静默丢弃, 也不丢弃全部结果 —— 已匹配的部分对调用方仍有价值);
    // 0 表示不限
    auto maxFiles = arguments.value<int64_t>("max_files", 1000);

    auto checkCancel = [&]() -> bool {
        return isCancelled && isCancelled();
    };

    // glob 取消标志: 本实现以 deadline/cancel 轮询控制整体流程,
    // 遍历内部传入恒为 false 的局部标志 (不泄漏堆分配)
    std::atomic<bool> globNeverCancel{false};

    // 遍历策略 (排除过滤 + 结果数量上限) 在多 pattern 之间共享: 排除项在遍历中
    // 直接剪枝, 数量上限对"全部 pattern 的总匹配数"生效, 且到限即停止遍历
    // (已匹配结果保留, 由下方 [glob::WalkPolicy::limitReached] 生成首行提示)
    glob::WalkPolicy walkPolicy = detail::makeWalkPolicy(excludePatterns, maxFiles);

    // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob (仅当前目录)
    // 对齐 shell globstar 行为: `*.txt` 只匹配当前目录, `**/*.txt` 才递归
    std::vector<std::filesystem::path> resultList;
    for (const auto& pattern : file_patterns) {
        if (checkCancel() || deadline.expired()) {
            return "[Error: cancelled or timed out]";
        }
        // 到限即停: 遍历已在策略内停止, 后续 pattern 不再展开 (总量不超上限)
        if (walkPolicy.limitReached) {
            break;
        }
        try {
            if (glob::has_recursive_segment(pattern)) {
                auto matched = glob::rglob(pattern, true, globNeverCancel, walkPolicy);
                resultList.insert(
                    resultList.end(),
                    std::make_move_iterator(matched.begin()),
                    std::make_move_iterator(matched.end())
                );
            } else {
                auto matched = glob::glob(pattern, true, globNeverCancel, walkPolicy);
                resultList.insert(
                    resultList.end(),
                    std::make_move_iterator(matched.begin()),
                    std::make_move_iterator(matched.end())
                );
            }
        } catch (const glob::walk_limit_exceeded&) {
            // stopOnLimit 模式下不会走到这里 (保留分支以兼容其他调用方语义)
            break;
        }
    }

    if (resultList.empty()) {
        return R"([Error: no file matched `file_patterns`])";
    }

    // 默认去重: 多 pattern 可能匹配到相同路径
    std::sort(resultList.begin(), resultList.end());
    resultList.erase(std::unique(resultList.begin(), resultList.end()), resultList.end());

    // 类型过滤 (对齐 find -type): file / dir / symlink / other / any
    if (!typeFilter.empty()) {
        std::vector<std::filesystem::path> filtered;
        filtered.reserve(resultList.size());
        for (const auto& p : resultList) {
            if (checkCancel() || deadline.expired()) {
                return "[Error: cancelled or timed out]";
            }
            if (typeFilter.count(detail::fileTypeOf(p))) {
                filtered.push_back(p);
            }
        }
        resultList = std::move(filtered);
    }

    // exclude_patterns 过滤: 将 glob 模式转为正则, 匹配到的路径被排除
    if (!excludePatterns.empty()) {
        auto excludeRegexes = detail::compileExcludeRegexes(excludePatterns);
        if (!excludeRegexes.empty()) {
            std::vector<std::filesystem::path> filtered;
            filtered.reserve(resultList.size());
            for (const auto& p : resultList) {
                if (false == detail::isExcluded(detail::toUtf8(p), excludeRegexes)) {
                    filtered.push_back(p);
                }
            }
            resultList = std::move(filtered);
        }
    }

    // max_depth 过滤 (对齐 find -maxdepth):
    // 计算匹配路径相对于模式静态前缀目录的深度, 超出则排除
    if (maxDepth >= 0) {
        std::vector<std::filesystem::path> filtered;
        filtered.reserve(resultList.size());
        for (const auto& p : resultList) {
            // 对每个 pattern 检查深度, 任一 pattern 满足即保留
            bool keep = false;
            for (const auto& pattern : file_patterns) {
                auto            baseDir = glob::static_prefix(pattern);
                std::error_code ec;
                auto            rel = std::filesystem::relative(p, baseDir, ec);
                if (ec) {
                    continue;
                }
                if (glob::path_depth(rel) <= static_cast<int>(maxDepth)) {
                    keep = true;
                    break;
                }
            }
            if (keep) {
                filtered.push_back(p);
            }
        }
        resultList = std::move(filtered);
    }

    // 排序 (去重时已排序, 但过滤后顺序可能变化; 用户显式要求排序时重新排序)
    if (doSort) {
        std::sort(resultList.begin(), resultList.end());
    }

    // 权限逐项过滤: 模式 (可含 `*`/`**`) 展开出的路径可能与"针对子目录的拒绝规则"
    // 相交, 声明的权限目标只覆盖"扫描起点", 因此在此对实际路径逐项复核
    resultList = detail::filterByPermission(std::move(resultList), pathFilter);

    if (resultList.empty()) {
        if (walkPolicy.limitReached) {
            // 被截断且过滤后无剩余: 提示里带上"未收集完整"的信息, 避免误判为"无匹配"
            return fmt::format(
                R"([Error: no file matched `file_patterns` after filtering. Note: the walk stopped early at `max_files` = {}, so more matches may exist beyond the ones collected (raise `max_files` or narrow `file_patterns`).])",
                maxFiles
            );
        }
        return R"([Error: no file matched `file_patterns` after filtering])";
    }

    // 结果数量达到上限时在首行说明: 遍历已停止, 只列出已匹配到的部分路径
    // (保留已匹配结果比整条报错更有用; 提示明示"还有更多匹配未收集", 避免
    // 调用方误以为已拿到全部匹配)
    auto oss = std::ostringstream{};
    if (walkPolicy.limitReached) {
        oss << fmt::format(
            "[Note] Too many paths match `file_patterns` (> `max_files` = {}): the walk stopped and only the first {} matched paths are listed. Narrow `file_patterns`, add `exclude_patterns`, or raise `max_files` to see more.\n",
            maxFiles,
            resultList.size()
        );
    }
    for (const auto& item : resultList) {
        if (checkCancel() || deadline.expired()) {
            return "[Error: cancelled or timed out]";
        }
        oss << detail::toUtf8(item) << '\n';
    }
    return oss.str();
}

// =====================================================================
// agentxx_filesystem_grep 执行体 (原 FilesystemGrepTool::execute_async)
// =====================================================================
inline std::string fileGrepExecuteImpl(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    // 搜索模式参数 (两者均可省略, 但至少指定其一; 同时指定时结果为两者并集):
    // - text_patterns  : 纯文本字面量匹配 (对齐 grep -F, 不经正则解释)
    // - regex_patterns : 正则表达式匹配 (对齐 grep -E)
    auto text_patterns  = arguments.value("text_patterns", std::vector<std::string>{});
    auto regex_patterns = arguments.value("regex_patterns", std::vector<std::string>{});
    if (text_patterns.empty() && regex_patterns.empty()) {
        throw std::invalid_argument{"Arg `text_patterns` and `regex_patterns` are both empty (specify at least one of them)"};
    }
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        throw std::invalid_argument{"Arg `file_patterns` is empty"};
    }
    for (auto& item : file_patterns) {
        item = detail::wsAbs(workDir, item);
    }
    // 排除模式 (与 glob 工具一致): 命中的条目在**遍历过程中**被跳过, 命中目录时
    // 整棵子树剪枝 —— 用于跳过 build/third_party 这类大目录, 省掉遍历开销
    auto excludePatterns = arguments.value("exclude_patterns", std::vector<std::string>{});
    for (auto& item : excludePatterns) {
        item = detail::wsAbs(workDir, item);
    }
    auto output_mode = arguments.value("output_mode", std::string{"files_with_matches"});
    if (output_mode != "files_with_matches" && output_mode != "content" && output_mode != "count") {
        throw std::invalid_argument{fmt::format(
            R"(Arg `output_mode`({}) is invalid: use `files_with_matches`, `content` or `count`.)",
            output_mode
        )};
    }
    auto timeout  = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline = Deadline::after(timeout);

    // 规模上限 (防止一次调用把整个大目录树 —— 如全仓库含 build/third_party 的
    // 几十万文件/几十 GB —— 全部读入内存并扫描
    // - max_files: 候选文件数上限; 到限即停止收集候选文件, **保留并搜索已收集到
    //   的候选**, 输出首行以 `[Note]` 说明"还有更多文件未搜索" (整条报错会让
    //   调用方一无所获; 静默截断又会让"有匹配"被误判为"无匹配", 故两者都取中间:
    //   给出部分结果 + 明确提示)
    // - max_file_size_mb: 单文件大小上限, 超过则跳过 (结果末尾以 `[Note]` 说明),
    //   避免超大文件 (或伪装成文本的大文件) 被整文件读入内存
    // 0 表示不限 (与 timeout 的 0 = 不限语义一致)
    auto maxFiles      = arguments.value<int64_t>("max_files", 1000);
    auto maxFileSizeMb = arguments.value<double>("max_file_size_mb", 32.0);

    // 新增参数 (对齐 Linux grep 行为)
    auto caseSensitive   = arguments.value<bool>("case_sensitive", true);
    auto maxCountPerFile = arguments.value<int64_t>("max_count_per_file", 0); // 0 = 不限
    auto contextLines    = arguments.value<int64_t>("context_lines", 0);

    auto checkStop = [&]() -> bool {
        return (isCancelled && isCancelled()) || deadline.expired();
    };

    // ---- glob 收集候选文件 ----
    std::atomic<bool>                  globNeverCancel{false};
    std::vector<std::filesystem::path> refilelist{};
    // 遍历策略: 数量上限在**遍历过程中**生效 (到限立即停止遍历, 不会先把几十万条
    // 路径读进内存); 计数在多 pattern 之间共享, 限制的是候选文件总数。
    // 到限时**保留已收集的候选文件**并继续搜索其内容 (未搜索的文件在输出首行
    // `[Note]` 中说明), 而不是整条调用报错
    glob::WalkPolicy walkPolicy = detail::makeWalkPolicy(excludePatterns, maxFiles);
    for (const auto& pattern : file_patterns) {
        if (checkStop()) {
            return "[Error: cancelled or timed out]";
        }
        // 到限即停: 遍历已在策略内停止, 后续 pattern 不再展开 (候选总数不超上限)
        if (walkPolicy.limitReached) {
            break;
        }
        // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob
        // (仅当前目录); 路径匹配固定为大小写敏感
        // 单个 pattern 的遍历失败 (如目录树中存在系统代码页无法表示的文件名,
        // MSVC 下 fs::path 窄化转换抛 system_error) 不应中断整体搜索,
        // 经 catchError 隔离后跳过该 pattern 继续其余 pattern
        std::vector<std::filesystem::path> matched;
        auto                               globOk = agentxx::util::catchError<bool>(
            [&]() -> bool {
                if (glob::has_recursive_segment(pattern)) {
                    matched = glob::rglob(pattern, true, globNeverCancel, walkPolicy);
                } else {
                    matched = glob::glob(pattern, true, globNeverCancel, walkPolicy);
                }
                return true;
            },
            [&](std::string errmsg) -> bool {
                XX_LOGW("filesystem_grep: glob pattern '{}' failed, skipped: {}", pattern, errmsg);
                return false;
            }
        );
        if (false == globOk || matched.empty()) {
            continue;
        }
        refilelist.insert(
            refilelist.end(),
            std::make_move_iterator(matched.begin()),
            std::make_move_iterator(matched.end())
        );
    }
    // 去重: 多 pattern 可能匹配到相同路径
    std::sort(refilelist.begin(), refilelist.end());
    refilelist.erase(std::unique(refilelist.begin(), refilelist.end()), refilelist.end());
    // 仅保留普通文件: glob 模式可能匹配到目录 (如 `**/*`), 在此过滤
    refilelist.erase(
        std::remove_if(
            refilelist.begin(),
            refilelist.end(),
            [](const std::filesystem::path& p) {
                std::error_code ec;
                return false == std::filesystem::is_regular_file(p, ec);
            }
        ),
        refilelist.end()
    );

    // 权限逐项过滤: `file_patterns` 展开出的文件可能与"针对子目录的拒绝规则"相交
    // (声明的权限目标只覆盖扫描起点), 在此按实际文件路径复核, 未获批准的文件不读
    refilelist = detail::filterByPermission(std::move(refilelist), pathFilter);

    // exclude_patterns 复核 (遍历层已按同一判定剪枝, 此处保证结果集语义不受
    // "目录剪枝"与"逐条匹配"的差异影响)
    if (false == excludePatterns.empty()) {
        auto excludeRegexes = detail::compileExcludeRegexes(excludePatterns);
        if (false == excludeRegexes.empty()) {
            std::vector<std::filesystem::path> filtered;
            filtered.reserve(refilelist.size());
            for (const auto& p : refilelist) {
                if (false == detail::isExcluded(detail::toUtf8(p), excludeRegexes)) {
                    filtered.push_back(p);
                }
            }
            refilelist = std::move(filtered);
        }
    }

    if (refilelist.empty()) {
        if (walkPolicy.limitReached) {
            // 极端情况: 到限停止后候选被 (权限/exclude/非普通文件) 过滤为空
            throw std::runtime_error{fmt::format(
                R"(No match `file_patterns` file found. Note: the walk stopped early at `max_files` = {}, so more files may exist beyond the ones collected (raise `max_files` or narrow `file_patterns`).)",
                maxFiles
            )};
        }
        throw std::runtime_error{"No match `file_patterns` file found"};
    }

    // 候选文件数达到上限时在输出首行说明 (遍历已停止, 未收集到的文件未搜索):
    // 保留已收集候选并照常搜索其内容, 比整条报错更有用; 提示明确"还有更多文件
    // 未搜索", 避免调用方把结果当成完整结果
    const bool walkTruncated = walkPolicy.limitReached;

    // 输出模式 (三种模式下都完整扫描, 区别只在输出的内容):
    // - `files_with_matches`: 每个命中文件一行 `路径:命中数`
    // - `content`: 命中行按文件分组输出 (可带上下文行)
    // - `count`: 只输出汇总数 (命中文件数 + 命中数), 输出行数固定
    const bool isContentMode = ("content" == output_mode);
    const bool isCountMode   = ("count" == output_mode);
    auto       resultStr     = std::ostringstream{};
    // count 模式的累计量 (只有该模式写入)
    size_t countMatchedFiles = 0;
    size_t countMatchesTotal = 0;

    // ---- 行索引辅助: 每文件只扫描一次构建"行起始偏移索引", 行号查询二分 O(log n)

    /// 构建行起始偏移索引 (0-based): lineStarts[i] 为第 i 行的起始字节偏移
    auto buildLineStarts = [](std::string_view text) -> std::vector<size_t> {
        std::vector<size_t> lineStarts;
        lineStarts.reserve(text.size() / 40 + 1);
        lineStarts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                lineStarts.push_back(i + 1);
            }
        }
        return lineStarts;
    };

    /// 计算 pos 所在的行号 (0-based)
    auto lineNumberOf = [](const std::vector<size_t>& lineStarts, size_t pos) -> size_t {
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), pos);
        return static_cast<size_t>(it - lineStarts.begin()) - 1;
    };

    /// 提取 text 中第 lineIdx 行 (0-based) 的内容 (不含末尾换行符)
    auto extractLine
        = [](std::string_view text, const std::vector<size_t>& lineStarts, size_t lineIdx
          ) -> std::string_view {
        if (lineIdx >= lineStarts.size()) {
            return {};
        }
        const size_t start = lineStarts[lineIdx];
        // lineStarts[i+1] 为下一行行首 (本行 `\n` 后一字节), 减 1 排除换行符
        const size_t end
            = (lineIdx + 1 < lineStarts.size()) ? lineStarts[lineIdx + 1] - 1 : text.size();
        return text.substr(start, end - start);
    };

    /// 计算文件总行数
    auto totalLines = [](std::string_view text) -> size_t {
        size_t count = 1; // 至少 1 行 (即使空文件)
        for (char c : text) {
            if (c == '\n') {
                ++count;
            }
        }
        // 如果文件以 \n 结尾, 最后不算额外一行
        if (!text.empty() && text.back() == '\n') {
            --count;
        }
        return count;
    };

    /// 加载并预处理文本文件: 跳过二进制文件 (含 NUL 字节), 非 UTF-8 编码
    /// (GBK 等) 转换为 UTF-8, 转换失败视为非文本跳过。额外统一 CRLF -> LF
    /// (归一化仅作用于本次搜索的内存副本, 不修改原文件)
    /// 超过 `max_file_size_mb` 的文件在读入前跳过并计数 (见 [skippedTooLarge])
    size_t skippedTooLarge    = 0;
    auto   loadSearchableText = [&](const std::string& filepath) -> std::optional<std::string> {
        try {
            if (maxFileSizeMb > 0) {
                std::error_code sizeEc;
                auto            fileSize
                    = std::filesystem::file_size(utilxx_base::utf8ToPath(filepath), sizeEc);
                if (false == static_cast<bool>(sizeEc)
                    && fileSize > static_cast<uintmax_t>(
                           static_cast<double>(1024 * 1024) * maxFileSizeMb
                       )) {
                    ++skippedTooLarge;
                    return std::nullopt;
                }
            }
            auto filetext = detail::readFileContent(filepath);
            // 仅搜索文本文件: 含 NUL 字节视为二进制, 跳过
            if (filetext.find('\0') != std::string::npos) {
                return std::nullopt;
            }
            // 非 UTF-8 编码文本转 UTF-8; 转换失败视为非文本, 跳过
            if (false == utilxx_base::autoConvertToUtf8(filetext)) {
                return std::nullopt;
            }
            detail::normalizeCrlfToLf(filetext);
            return filetext;
        } catch (...) {
            // 单个文件读取失败跳过 (与原实现异常跳过行为一致)
            return std::nullopt;
        }
    };

    /// content 模式输出辅助: 将单个文件的匹配结果以"按文件分组"格式写入输出流。
    /// 格式设计 (减少每行的文件路径重复):
    ///   {filepath}:          <- 组头, 每个文件仅输出一次
    ///   {line}:{content}     <- 匹配行 (对齐 grep -n)
    ///   {line}-{content}     <- 上下文行 (对齐 grep -n -C)
    ///   --                   <- 同一文件内不连续的上下文块之间 (对齐 grep -C)
    auto appendGroupedContent = [&](std::ostringstream&        out,
                                    const std::string&         filepath,
                                    std::string_view           filetext,
                                    const std::vector<size_t>& matchStarts) {
        auto lineStarts = buildLineStarts(filetext);
        auto fileLines  = totalLines(filetext);

        // 收集需要输出的行号 (含上下文), 去重后按行号排序输出
        std::set<size_t> matchLineSet{};   // 匹配行
        std::set<size_t> contextLineSet{}; // 上下文行
        for (size_t mi = 0; mi < matchStarts.size(); ++mi) {
            size_t lineIdx = lineNumberOf(lineStarts, matchStarts[mi]);
            matchLineSet.insert(lineIdx);
            if (contextLines > 0) {
                size_t ctxStart = (lineIdx > static_cast<size_t>(contextLines))
                                      ? lineIdx - static_cast<size_t>(contextLines)
                                      : 0;
                size_t ctxEnd   = std::min(
                    lineIdx + static_cast<size_t>(contextLines),
                    fileLines > 0 ? fileLines - 1 : 0
                );
                for (size_t l = ctxStart; l <= ctxEnd; ++l) {
                    contextLineSet.insert(l);
                }
            }
        }

        // 合并所有需要输出的行, 按行号排序
        std::set<size_t> allLines = contextLineSet;
        allLines.insert(matchLineSet.begin(), matchLineSet.end());

        // 文件路径作为组头仅输出一次, 行前缀不再重复完整路径
        out << filepath << ":\n";
        size_t prevLine = 0;
        bool   first    = true;
        for (size_t lineIdx : allLines) {
            // 对齐 grep -C: 不连续的上下文块之间用 -- 分隔
            if (!first && contextLines > 0 && lineIdx > prevLine + 1) {
                out << "--\n";
            }
            first = false;

            auto lineContent = extractLine(filetext, lineStarts, lineIdx);
            // 匹配行用 `:` 分隔, 上下文行用 `-` 分隔 (对齐 grep -n -C)
            char sep = matchLineSet.count(lineIdx) ? ':' : '-';
            out << (lineIdx + 1) << sep << lineContent << "\n";
            prevLine = lineIdx;
        }
    };

    // 应用 max_count_per_file 限制 (对齐 grep -m) 并按 output_mode 输出。
    // MatchT 需提供 start 成员 (MatchRange / XXRegexMatchResult /
    // AhoCorasick 匹配结构); 以模板 lambda 实现 (块作用域内不允许
    // template 函数声明)。调用前区间已并集去重, 单文件只会输出一次
    auto emitMatches = [&]<typename MatchT>(
                           const std::string&         filepath,
                           const std::string&         filetext,
                           const std::vector<MatchT>& matchRanges
                       ) -> void {
        // 应用 max_count_per_file 限制 (对齐 grep -m)
        size_t effectiveCount = matchRanges.size();
        if (maxCountPerFile > 0 && static_cast<size_t>(maxCountPerFile) < effectiveCount) {
            effectiveCount = static_cast<size_t>(maxCountPerFile);
        }

        if (isCountMode) {
            // count 模式: 只累计, 不逐文件输出 (扫描结束后汇总输出)
            countMatchesTotal += effectiveCount;
            ++countMatchedFiles;
        } else if (!isContentMode) {
            // files_with_matches 模式: 输出 file:match_count
            resultStr << filepath << ":" << effectiveCount << "\n";
        } else {
            // content 模式: 输出匹配整行, 按文件分组格式 (对齐 grep -n);
            // 行索引构建 + 行号查询 O(log n)
            std::vector<size_t> matchStarts;
            matchStarts.reserve(effectiveCount);
            for (size_t mi = 0; mi < effectiveCount; ++mi) {
                matchStarts.push_back(matchRanges[mi].start);
            }
            appendGroupedContent(resultStr, filepath, filetext, matchStarts);
        }
    };

    // ---- 双模式匹配 (text_patterns 纯文本 ∪ regex_patterns 正则) ----
    // 模式参数允许同时指定, 结果为两者命中区间的并集 (按文件合并后统一输出,
    // 保证单文件仅输出一次: files_with_matches 单行、content 模式单组头,
    // max_count_per_file 对并集生效); 两者都为空已在入口拒绝

    /// 文件内两种模式命中区间的统一表示 (对齐 XXRegexMatchResult 语义)
    struct MatchRange {
        size_t start;
        size_t end;
    };

    // 文本轮: 纯文本精确匹配 (对齐 grep -F, AhoCorasick 多模式同时扫描)
    auto textSearch
        = text_patterns.empty()
              ? nullptr
              : std::make_unique<utilxx::AhoCorasick<char>>(text_patterns, !caseSensitive);

    // 正则轮: 大小写不敏感直接由 XXRegex 后端实现
    // (Hyperscan 用 HS_FLAG_CASELESS, std::regex fallback 用 icase)
    auto regex = regex_patterns.empty() ? nullptr
                                        : utilxx::XXRegex::createRegex(
                                              regex_patterns,
                                              utilxx::XXRegex::defHSFlags_normal,
                                              !caseSensitive
                                          );
    if (false == regex_patterns.empty() && !regex) {
        return fmt::format(
            R"_([Error: regex compilation failed (regex_patterns are parsed as regular expressions). If you intended literal text search, put the patterns in `text_patterns` instead.])_"
        );
    }

    bool anyHit = false;
    for (const auto& item : refilelist) {
        if (checkStop()) {
            return "[Error: cancelled or timed out]";
        }
        auto filepath = detail::toUtf8(item);
        // 读取并预处理: 跳过二进制/非文本文件 (glob 阶段已过滤目录)
        auto filetextOpt = loadSearchableText(filepath);
        if (false == filetextOpt.has_value()) {
            continue;
        }
        auto& filetext = filetextOpt.value();

        // 收集该文件全部命中区间 (文本 + 正则, 并集去重)
        std::vector<MatchRange> ranges;
        if (textSearch) {
            auto matchs = textSearch->search(filetext);
            ranges.reserve(ranges.size() + matchs.size());
            for (const auto& m : matchs) {
                ranges.push_back({m.start, m.end});
            }
        }
        if (regex) {
            auto matchs = std::vector<utilxx::XXRegexMatchResult>{};
            if (regex->match(filetext, matchs)) {
                ranges.reserve(ranges.size() + matchs.size());
                for (const auto& m : matchs) {
                    ranges.push_back({m.start, m.end});
                }
            }
        }
        if (ranges.empty()) {
            continue;
        }

        // 合并重叠区间 (对齐 XXRegex match 语义: 仅真正重叠的合并, 相邻不合并),
        // 使并集区间互不重叠、计数/行输出不重复
        std::sort(ranges.begin(), ranges.end(), [](const MatchRange& a, const MatchRange& b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        });
        std::vector<MatchRange> merged;
        merged.reserve(ranges.size());
        for (const auto& r : ranges) {
            if (merged.empty() || r.start >= merged.back().end) {
                // 与上一区间不重叠 (含相邻), 新增一段
                merged.push_back(r);
            } else if (r.end > merged.back().end) {
                // 与上一区间重叠, 扩展上一区间右端
                merged.back().end = r.end;
            }
        }

        emitMatches(filepath, filetext, merged);
        anyHit = true;
    }

    if (anyHit) {
        if (isCountMode) {
            // count 模式: 固定几行汇总 (只给规模, 不给路径与行内容)
            auto summary = fmt::format(
                "{} file(s) matched, {} match(es) in total.\n",
                countMatchedFiles,
                countMatchesTotal
            );
            if (skippedTooLarge > 0) {
                summary += fmt::format(
                    "[Note] {} file(s) skipped: larger than `max_file_size_mb` = {}.\n",
                    skippedTooLarge,
                    maxFileSizeMb
                );
            }
            if (walkTruncated) {
                summary += fmt::format(
                    "[Note] The walk stopped early at `max_files` = {}: more matched files may exist unscanned.\n",
                    maxFiles
                );
            }
            return summary;
        }
        if (skippedTooLarge > 0) {
            // 跳过说明写在结果末尾 (不做静默丢弃): 调用方据此判断是否需要提高上限
            resultStr << fmt::format(
                "[Note] {} file(s) skipped: larger than `max_file_size_mb` = {}. Raise it if they are needed.\n",
                skippedTooLarge,
                maxFileSizeMb
            );
        }
        if (walkTruncated) {
            // 数量达到上限导致遍历提前停止: 提示写在**首行** (结果主体是搜索命中,
            // 提示需先被看到), 并明示"还有更多文件未搜索", 避免把结果当成完整结果
            return fmt::format(
                "[Note] Too many files match `file_patterns` (> `max_files` = {}): the walk stopped and only {} matched file(s) were scanned. Narrow `file_patterns`, add `exclude_patterns`, or raise `max_files` to search more.\n{}",
                maxFiles,
                refilelist.size(),
                resultStr.str()
            );
        }
        return resultStr.str();
    }
    // 无匹配错误附带当前匹配模式说明, 便于调用方排查:
    // 正则元字符 (如 `(` `[` `*`) 未转义/未成对时会匹配不到, 此时可改用
    // text_patterns 纯文本匹配 (text_patterns 不经正则解释) 或转义后重试
    throw std::runtime_error{fmt::format(
        R"_(Found {} files match `file_patterns`, but no match `text_patterns`/`regex_patterns` file found.{}{} Active modes: {}.)_",
        refilelist.size(),
        skippedTooLarge > 0 ? fmt::format(
                                  " {} file(s) were skipped (larger than `max_file_size_mb` = {}).",
                                  skippedTooLarge,
                                  maxFileSizeMb
                              )
                            : std::string{},
        // 遍历因数量上限提前停止: 未搜索的文件里可能有匹配, 提示调用方提高上限
        walkTruncated
            ? fmt::format(
                  " The walk stopped early at `max_files` = {}, so more matched files may exist unscanned (raise `max_files` or narrow `file_patterns`).",
                  maxFiles
              )
            : std::string{},
        (!text_patterns.empty() && !regex_patterns.empty())
            ? "literal text + regular expression (union)"
            : (text_patterns.empty() ? "regular expression" : "literal text")
    )};
}

// =====================================================================
// 对外执行体: 与原 lib 工具的 asyncWithTimeout+catchErrorAsync 外层语义一致
// —— 可预期异常 (如无匹配/文件打开失败) 统一转为 "[Error: ...]" 错误文本返回,
// 而非向调用方抛出; 保证插件 execute 回调、测试直测两种路径行为一致
// =====================================================================
namespace detail {

/// 异常 → "[Error: ...]" 文本包装 (取消类异常不在本层出现: impl 内部仅以
/// 返回值表达取消, 见各执行体的 checkStop 分支)
template<typename Fn>
inline std::string asErrorText(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        return fmt::format("[Error: {}]", ex.what());
    }
}

} // namespace detail

// =====================================================================
// agentxx_filesystem_move / _copy / _remove 执行体
// =====================================================================
namespace detail {

/// 取路径参数并规范化为工作目录下的绝对路径 (空值报参数错误)
inline std::string requirePathArg(
    const utilxx_base::Json& arguments,
    std::string_view         name,
    const std::string&       workDir
) {
    auto value = arguments.value(std::string{name}, std::string{});
    if (value.empty()) {
        throw std::invalid_argument{fmt::format("Arg `{}` is empty", name)};
    }
    return wsAbs(workDir, value);
}

/// 路径是否存在 (悬空的符号链接也算: 它要能被删除或替换)
inline bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) || std::filesystem::is_symlink(path, ec);
}

/// 目标已存在时的统一处理: `overwrite` 未开启则报错, 开启时先删掉旧目标
/// - 旧目标可能是目录, 用 remove_all 整体替换
inline void prepareDestination(const std::string& dst, bool overwrite) {
    auto dstPath = utilxx_base::utf8ToPath(dst);
    if (false == pathExists(dstPath)) {
        return;
    }
    if (false == overwrite) {
        throw std::runtime_error{fmt::format(
            R"(Destination already exists: {}. Pass `overwrite` = true to replace it.)",
            dst
        )};
    }
    std::error_code ec;
    std::filesystem::remove_all(dstPath, ec);
    if (ec) {
        throw std::runtime_error{fmt::format(
            R"(Can not replace the existing destination {}: {})",
            dst,
            ec.message()
        )};
    }
}

/// 目标位于源目录内部时拒绝执行 (复制/移动到自己的子目录会递归到自身)
inline void rejectNestedDestination(const std::string& src, const std::string& dst) {
    if (dst.size() <= src.size() || dst.compare(0, src.size(), src) != 0) {
        return;
    }
    const char next = dst[src.size()];
    if (next == '/' || next == '\\') {
        throw std::invalid_argument{fmt::format(
            R"(Arg `dst`({}) is inside `src`({}): that would recurse into itself.)",
            dst,
            src
        )};
    }
}

/// 目标父目录缺失时创建 (写到新目录是常见用法, 与 write 工具一致)
inline void ensureParentDir(const std::filesystem::path& dstPath) {
    if (false == dstPath.has_parent_path() || pathExists(dstPath.parent_path())) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dstPath.parent_path(), ec);
    if (ec) {
        throw std::runtime_error{fmt::format(
            R"(Can not create `dst`({})'s parent dirs: {})",
            toUtf8(dstPath.parent_path()),
            ec.message()
        )};
    }
}

/// move/copy 的公共参数检查: 取绝对路径、非空、不同一路径、不相互嵌套、源存在
inline void resolveTransferArgs(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    std::string&             srcOut,
    std::string&             dstOut
) {
    srcOut = requirePathArg(arguments, "src", workDir);
    dstOut = requirePathArg(arguments, "dst", workDir);
    if (srcOut == dstOut) {
        throw std::invalid_argument{
            fmt::format("Args `src` and `dst` are the same path: {}", srcOut)
        };
    }
    rejectNestedDestination(srcOut, dstOut);
    if (false == pathExists(utilxx_base::utf8ToPath(srcOut))) {
        throw std::runtime_error{fmt::format("Source path does not exist: {}", srcOut)};
    }
}

/// 跨设备/跨盘复制 (rename 不可用时的回退): 递归复制后删除源
inline void copyThenRemoveSource(
    const std::filesystem::path& srcPath,
    const std::filesystem::path& dstPath,
    const std::string&           src,
    const std::string&           dst
) {
    std::error_code ec;
    std::filesystem::copy(
        srcPath,
        dstPath,
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::copy_symlinks,
        ec
    );
    if (ec) {
        throw std::runtime_error{fmt::format("Can not copy {} -> {}: {}", src, dst, ec.message())};
    }
    std::error_code rmEc;
    std::filesystem::remove_all(srcPath, rmEc);
    if (rmEc) {
        throw std::runtime_error{fmt::format(
            R"(Copied to {}, but can not remove the source {}: {})",
            dst,
            src,
            rmEc.message()
        )};
    }
}

} // namespace detail

/// 移动或重命名文件/目录 (同一文件系统内用 rename; 跨设备时回退为复制 + 删除源)
inline std::string
    fileMoveExecuteImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    std::string src;
    std::string dst;
    detail::resolveTransferArgs(arguments, workDir, src, dst);
    auto srcPath = utilxx_base::utf8ToPath(src);
    auto dstPath = utilxx_base::utf8ToPath(dst);
    detail::ensureParentDir(dstPath);
    detail::prepareDestination(dst, arguments.value<bool>("overwrite", false));

    std::error_code ec;
    std::filesystem::rename(srcPath, dstPath, ec);
    if (ec) {
        if (ec == std::errc::cross_device_link) {
            detail::copyThenRemoveSource(srcPath, dstPath, src, dst);
        } else {
            throw std::runtime_error{
                fmt::format("Can not move {} -> {}: {}", src, dst, ec.message())
            };
        }
    }
    return fmt::format("Moved: {} -> {}", src, dst);
}

/// 复制文件/目录 (目录递归复制; 符号链接按链接复制, 不跟随)
inline std::string
    fileCopyExecuteImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    std::string src;
    std::string dst;
    detail::resolveTransferArgs(arguments, workDir, src, dst);
    auto srcPath = utilxx_base::utf8ToPath(src);
    auto dstPath = utilxx_base::utf8ToPath(dst);
    detail::ensureParentDir(dstPath);
    detail::prepareDestination(dst, arguments.value<bool>("overwrite", false));

    std::error_code ec;
    std::filesystem::copy(
        srcPath,
        dstPath,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
        ec
    );
    if (ec) {
        throw std::runtime_error{fmt::format("Can not copy {} -> {}: {}", src, dst, ec.message())};
    }
    return fmt::format("Copied: {} -> {}", src, dst);
}

/// 删除文件或目录 (目录默认拒绝, 需显式 `recursive` = true; 符号链接只删链接本身)
inline std::string
    fileRemoveExecuteImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    auto            path   = detail::requirePathArg(arguments, "path", workDir);
    auto            fsPath = utilxx_base::utf8ToPath(path);
    std::error_code ec;
    if (false == detail::pathExists(fsPath)) {
        throw std::runtime_error{fmt::format("Path does not exist: {}", path)};
    }
    const bool isSymlink = std::filesystem::is_symlink(fsPath, ec);
    const bool isDir     = (false == isSymlink) && std::filesystem::is_directory(fsPath, ec);
    if (isDir && false == arguments.value<bool>("recursive", false)) {
        throw std::runtime_error{fmt::format(
            R"(Path is a directory: {}. Pass `recursive` = true to delete it with everything inside.)",
            path
        )};
    }
    if (isDir) {
        auto removed = std::filesystem::remove_all(fsPath, ec);
        if (ec) {
            throw std::runtime_error{fmt::format("Can not remove {}: {}", path, ec.message())};
        }
        return fmt::format("Removed: {} (directory, {} entries)", path, removed);
    }
    if (false == std::filesystem::remove(fsPath, ec) || ec) {
        throw std::runtime_error{fmt::format(
            "Can not remove {}: {}",
            path,
            ec ? ec.message() : std::string{"unknown error"}
        )};
    }
    return fmt::format("Removed: {}", path);
}

/// 对外执行体 (同步): 异常统一转 "[Error: ...]" 文本返回
/// - 三个工具都是单次元数据/目录操作, 不轮询取消 (形参与其他 *Execute 保持一致)
inline std::string fileMoveExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    (void)isCancelled;
    return detail::asErrorText([&] { return fileMoveExecuteImpl(arguments, workDir); });
}

inline std::string fileCopyExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    (void)isCancelled;
    return detail::asErrorText([&] { return fileCopyExecuteImpl(arguments, workDir); });
}

inline std::string fileRemoveExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    (void)isCancelled;
    return detail::asErrorText([&] { return fileRemoveExecuteImpl(arguments, workDir); });
}

inline std::string fileListExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    return detail::asErrorText([&] {
        return fileListExecuteImpl(arguments, workDir, isCancelled, pathFilter);
    });
}

inline std::string fileReadExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileReadExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileWriteExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileWriteExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileEditExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileEditExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileGlobExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    return detail::asErrorText([&] {
        return fileGlobExecuteImpl(arguments, workDir, isCancelled, pathFilter);
    });
}

inline std::string fileGrepExecute(
    const utilxx_base::Json& arguments,
    const std::string&       workDir,
    const IsCancelledFn&     isCancelled = nullptr,
    const PathFilterFn&      pathFilter  = nullptr
) {
    return detail::asErrorText([&] {
        return fileGrepExecuteImpl(arguments, workDir, isCancelled, pathFilter);
    });
}

// =====================================================================
// 协程版执行体 (read / write / edit)
// - 插件入口经 plugin_kit::polled_tool 注册: 协程跑在插件本地 reactor 上,
//   由主程序受控轮询 (driver 请求 + poll_one) 驱动 asio::stream_file 的异步 IO;
//   上述 *Execute 同步版仍由 list / glob / grep 使用 (CPU/遍历类工具走
//   blocking_tool + offload, 是显式例外);
// - 文件异步 I/O 的可用性由 utilxx_base::isAsyncFileIoSupported() 判断
//   (编译期宏 + 运行时 io_uring 探测), 不可用时 *ExecuteAsync 回退同步实现,
//   注册侧同样按该判断改走 blocking_tool, 行为与 offload 一致;
//   本回退亦供测试等直调场景保持单一入口
// =====================================================================
#if defined(ASIO_HAS_FILE) || defined(BOOST_ASIO_HAS_FILE)

namespace detail {

/// stream_file 异步读取完整文件内容 (原始字节; 不做编码转换)
/// - 打开失败抛出异常; 读到 EOF 视为正常结束
inline asio::awaitable<std::string>
    asyncReadWholeFile(const asio::any_io_executor& executor, const std::string& utf8FilePath) {
    auto            fsPath = utilxx_base::utf8ToPath(utf8FilePath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(fsPath, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(fsPath, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }
    asio::stream_file          stream{executor};
    utilxx_base::AsioErrorCode errCode;
    stream.open(utf8FilePath, asio::stream_file::read_only, errCode);
    if (false == stream.is_open()) {
        throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
    }
    std::string data;
    co_await asio::async_read(
        stream,
        asio::dynamic_buffer(data),
        asio::transfer_all(),
        asio::redirect_error(asio::use_awaitable, errCode)
    );
    // transfer_all 在文件结束时以 eof 返回, 属预期终止
    if (errCode && errCode != asio::error::eof) {
        throw std::system_error{errCode};
    }
    stream.close();
    co_return data;
}

} // namespace detail

/// agentxx_filesystem_read 执行体协程版 (原 FilesystemReadTextFileTool::execute_async)
/// - offset/limit 模式经 async_read_until 逐行推进 (保留原始换行符);
///   其余整文件读取; 读取后 autoConvertToUtf8 (保留 crlf 或 \n 原样不转换)
inline asio::awaitable<std::string>
    fileReadExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    detail::rejectLegacyLineArgs(arguments);
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto            fsPath = utilxx_base::utf8ToPath(filepath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(fsPath, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(fsPath, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }
    auto text_line_offset = arguments.value<int64_t>("offset", -1);
    auto text_line_limit  = arguments.value<int64_t>("limit", -1);

    auto executor = co_await asio::this_coro::executor;

    if (text_line_offset >= 0 || text_line_limit > 0) {
        // 读取部分文件: 逐行 async_read_until, 跳过偏移行后收集至结果
        asio::stream_file          stream{executor};
        utilxx_base::AsioErrorCode errCode;
        stream.open(filepath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
        }

        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;
        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        for (std::string buf; lineNum < endLine; lineNum++) {
            auto readlen = co_await asio::async_read_until(
                stream,
                asio::dynamic_buffer(buf),
                '\n',
                asio::redirect_error(asio::use_awaitable, errCode)
            );

            if (errCode == asio::error::eof) {
                // 文件结束 (末行可能无换行符): readlen 为 EOF 前已读入的字节数
                if (lineNum >= offset) {
                    auto line = std::string_view{buf}.substr(0, readlen);
                    result << line;
                }
                break;
            } else if (errCode) {
                throw std::system_error{errCode};
            }

            if (lineNum >= offset) {
                auto line = std::string_view{buf}.substr(0, readlen);
                result << line;
                // 边读边看体量: 请求的区间过大时立即报错, 不先把整段读进内存
                const auto readLines = lineNum - offset + 1;
                const auto readBytes = static_cast<size_t>(result.tellp());
                if (readLines > detail::kReadMaxLines || readBytes > detail::kReadMaxBytes) {
                    detail::throwReadTooLarge(readBytes, readLines);
                }
            }
            buf.erase(0, readlen);
        }

        stream.close();
        if (lineNum == 0 && offset == 0) {
            // 空文件: 第 0 行视为空行, 返回空串而非报错
            co_return "";
        }
        if (lineNum <= offset) {
            // offset 超出文件行数
            throw std::runtime_error{fmt::format(
                R"(Arg `offset`({} lines) is out of range of file lines({} lines).)",
                offset,
                lineNum
            )};
        }

        auto rawStr = result.str();
        // 保留原始的 crlf 或 \n 换行符不转换
        utilxx_base::autoConvertToUtf8(rawStr);
        detail::checkReadResultLimit(rawStr);
        co_return rawStr;
    }

    // 读取完整文件 (先看体量: 超大文件直接报错, 不读进内存)
    detail::checkWholeFileSizeBeforeRead(fsPath);
    auto data = co_await detail::asyncReadWholeFile(executor, filepath);
    // 保留原始的 crlf 或 \n 换行符不转换
    utilxx_base::autoConvertToUtf8(data);
    detail::checkReadResultLimit(data);
    co_return data;
}

/// agentxx_filesystem_write 执行体协程版 (原 FilesystemWriteFileTool::execute_async)
/// - overwrite=false 且目标存在时报错; 自动创建缺失的父目录;
///   stream_file create|truncate 打开后 async_write 全量写入
inline asio::awaitable<std::string>
    fileWriteExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);

    // 存在性检查与父目录创建: 快速元数据操作, 与原实现一致内联执行
    auto path = utilxx_base::utf8ToPath(filepath);
    // 按规范化路径排队: 存在性检查与写入必须相对其它写者原子
    auto            pathLease = co_await detail::lockPathAsync(filepath);
    if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist. Set `overwrite` = true if want to overwrite."
        };
    }
    if (!path.parent_path().empty() && false == std::filesystem::exists(path.parent_path())
        && false == std::filesystem::create_directories(path.parent_path())) {
        throw std::runtime_error{fmt::format(
            R"(Can not create `path`({})'s parent dirs.)",
            detail::toUtf8(path.parent_path())
        )};
    }

    auto executor = co_await asio::this_coro::executor;

    asio::stream_file          stream{executor};
    utilxx_base::AsioErrorCode errCode;
    stream.open(
        filepath,
        asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
        errCode
    );
    if (false == stream.is_open()) {
        throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
    }

    if (false == content.empty()) {
        // 写入文本内容
        co_await asio::async_write(
            stream,
            asio::buffer(content),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode) {
            throw std::system_error{errCode};
        }
    }

    stream.close();
    co_return "success";
}

/// agentxx_filesystem_edit 执行体协程版 (原 FilesystemEditTextFileTool::execute_async)
/// - 异步读完整文件 → UTF-8/LF 归一化 → 按段顺序替换 → 原子写 (同目录临时文件 +
///   rename 覆盖), 与同步版行为一致
inline asio::awaitable<std::string>
    fileEditExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        throw std::invalid_argument{"Arg `path` is empty"};
    }
    auto segments = detail::parseEditSegments(arguments);

    // 统一到 \n 换行符
    // - 与 filesystem_read 的逻辑不同，read 应当保留原始的内容，edit 应当尽可能保证修改成功，
    //   如果 llm 需要写回 crlf，可使用 shell
    for (auto& seg : segments) {
        detail::normalizeCrlfToLf(seg.oldStr);
        detail::normalizeCrlfToLf(seg.newStr);
    }

    auto            path = utilxx_base::utf8ToPath(filepath);
    // 按规范化路径排队: 读-改-写整段相对其它写者原子
    auto            pathLease = co_await detail::lockPathAsync(filepath);
    std::error_code fsEc;
    bool            exists = std::filesystem::exists(path, fsEc);
    if (fsEc) {
        throw std::runtime_error{fmt::format(R"(Can not access file: {})", fsEc.message())};
    }
    if (!exists) {
        throw std::runtime_error{"File not exist"};
    }
    if (std::filesystem::is_directory(path, fsEc)) {
        throw std::runtime_error{"Path is a directory"};
    }

    // 异步读取完整文件并预处理 (先转 UTF-8 使 GBK 等编码文件可正常匹配, 再统一换行符)
    auto        executor = co_await asio::this_coro::executor;
    std::string content  = co_await detail::asyncReadWholeFile(executor, filepath);
    utilxx_base::autoConvertToUtf8(content);
    detail::normalizeCrlfToLf(content);

    // 任一段匹配不到即整体放弃 (读到内容后才会写盘, 失败时文件保持原样)
    detail::applyEditSegments(content, segments);

    // 原子写: 先写同目录临时文件, 成功后 rename 覆盖原文件,
    // 避免直接 truncate 原文件后写入中途失败导致原内容永久丢失
    // (注: 计数器仅保证进程内唯一性, 多实例共享无害, 不属于实例状态)
    static std::atomic<uint64_t> s_editTmpSeq{0};
    const auto                   tmpPathStr
        = fmt::format("{}.agentxx_edit_tmp_{}", filepath, s_editTmpSeq.fetch_add(1));
    const auto fsTmpPath = utilxx_base::utf8ToPath(tmpPathStr);

    {
        asio::stream_file          stream{executor};
        utilxx_base::AsioErrorCode errCode;
        stream.open(
            tmpPathStr,
            asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
            errCode
        );
        if (false == stream.is_open()) {
            throw std::runtime_error{
                fmt::format(R"(Can not open temp file to write: {})", errCode.message())
            };
        }
        co_await asio::async_write(
            stream,
            asio::buffer(content),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        stream.close();
        if (errCode) {
            std::error_code rmEc;
            std::filesystem::remove(fsTmpPath, rmEc);
            throw std::system_error{errCode};
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(fsTmpPath, path, renameEc);
    if (renameEc) {
        std::error_code rmEc;
        std::filesystem::remove(fsTmpPath, rmEc);
        throw std::runtime_error{
            fmt::format(R"(Failed to replace original file: {})", renameEc.message())
        };
    }

    co_return "success";
}

#else // !ASIO_HAS_FILE && !BOOST_ASIO_HAS_FILE

/// 文件异步 I/O 编译期不可用平台 (无 io_uring/iocp 文件支持): 回退同步实现。
/// 此时 utilxx_base::isAsyncFileIoSupported() 恒为 false, 注册侧会改走
/// offload线程池适配异步接口 注册, 本回退仅供测试等直调场景保持单一入口
inline asio::awaitable<std::string>
    fileReadExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    co_return fileReadExecuteImpl(arguments, workDir);
}

inline asio::awaitable<std::string>
    fileWriteExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    co_return fileWriteExecuteImpl(arguments, workDir);
}

inline asio::awaitable<std::string>
    fileEditExecuteAsyncImpl(const utilxx_base::Json& arguments, const std::string& workDir) {
    co_return fileEditExecuteImpl(arguments, workDir);
}

#endif // ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE

/// 对外协程执行体: 与同步版 *Execute 外层语义一致 —— 可预期异常统一转为
/// "[Error: ...]" 错误文本返回, 保证受控轮询路径与测试直测行为一致
/// (单文件读写为短操作不轮询取消, 故不设 isCancelled 形参)
/// - 文件异步 I/O 可用时走真异步实现 (受控轮询), 否则走同步实现
/// - 注意: 本包装自身必须是协程 (而非返回惰性协程的普通函数) —— 参数引用在
///   协程帧内存续, 若经普通函数中转临时 lambda 会因栈帧提前返回而悬垂
///   (ASan stack-use-after-return 已复现)
inline asio::awaitable<std::string>
    fileReadExecuteAsync(const utilxx_base::Json& arguments, const std::string& workDir) {
    try {
        if (utilxx_base::isAsyncFileIoSupported()) {
            co_return co_await fileReadExecuteAsyncImpl(arguments, workDir);
        }
        co_return fileReadExecuteImpl(arguments, workDir);
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error: {}]", ex.what());
    }
}

inline asio::awaitable<std::string>
    fileWriteExecuteAsync(const utilxx_base::Json& arguments, const std::string& workDir) {
    try {
        if (utilxx_base::isAsyncFileIoSupported()) {
            co_return co_await fileWriteExecuteAsyncImpl(arguments, workDir);
        }
        co_return fileWriteExecuteImpl(arguments, workDir);
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error: {}]", ex.what());
    }
}

inline asio::awaitable<std::string>
    fileEditExecuteAsync(const utilxx_base::Json& arguments, const std::string& workDir) {
    try {
        if (utilxx_base::isAsyncFileIoSupported()) {
            co_return co_await fileEditExecuteAsyncImpl(arguments, workDir);
        }
        co_return fileEditExecuteImpl(arguments, workDir);
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error: {}]", ex.what());
    }
}

} // namespace agentxx_fs_plugin
