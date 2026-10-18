/// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
///
/// 分工 (见 plugin_kit.h 的 polled_tool 说明):
/// - `read` / `write` / `edit`: 实现体是 asio 协程 (`asio::stream_file` 异步读写),
///   等待的是插件本地 reactor 上的文件 IO 就绪事件, 因此文件异步 I/O 可用时注册为
///   **声明式受控轮询**工具 —— 大文件读写不再占用主程序工作线程池;
///   文件异步 I/O 不可用 (编译期未启用, 或运行环境的 io_uring 被 seccomp 拦截)
///   时回退同步实现, 改注册 `blocking_tool` (offload 工作线程);
///   可用性判断统一走 `utilxx_base::isAsyncFileIoSupported()`;
/// - `list` / `glob` / `grep`: 目录遍历 + 全文件扫描 + 正则/编码转换, 这是 CPU/阻塞
///   工作而非异步 IO, 放进受控轮询只会阻塞主程序 IO 线程, 因此保持 `blocking_tool`。
#include "agentxx_fs_plugin.h"
#include "asio/awaitable.hpp"
#include "filesystem_impl.h"
#include "fmt/format.h"
#include "utilxx_base/system.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace agentxx_fs_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameList  = "agentxx_filesystem_list";
constexpr std::string_view kNameRead  = "agentxx_filesystem_read";
constexpr std::string_view kNameWrite = "agentxx_filesystem_write";
constexpr std::string_view kNameEdit  = "agentxx_filesystem_edit";
constexpr std::string_view kNameGlob  = "agentxx_filesystem_glob";
constexpr std::string_view kNameGrep  = "agentxx_filesystem_grep";

constexpr std::string_view kDepictList
    = R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.
Directory paths end with `/`, symlinks show their target. Types: `d` directory, `-` file, `l` symlink.
Can also be used to check whether a specific file or directory exists.
`path` may contain shell wildcards as in bash: `*` any characters, `?` one character, `[...]` a character class, `**` any directory depth (e.g. `src/*.cpp`, `src/**/*.h`). Matched entries themselves are then listed (like `ls -d`); with `recursive` = `true`, matched directories are expanded as well.
Wildcard matching is case-sensitive and does not match hidden entries (name starting with `.`).)";
constexpr std::string_view kDepictRead
    = R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents.
Supports `offset`/`limit` (both counted in lines) for reading portions of large files: `offset` lines are skipped from the beginning, then at most `limit` lines are returned. Both are optional; omitting them reads the whole file.)";
constexpr std::string_view kDepictWrite
    = "Create a new file or overwrite an existing file with the given content.";
constexpr std::string_view kDepictEdit
    = R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.
One call can change several places of the same file: pass them as `edits` (applied in order), or a single replacement as `old_str`/`new_str`.
Note! This tool will replace all `\r\n` to `\n` when find `old_str` and replace.)";
constexpr std::string_view kDepictGlob = "Find files and directories matching glob patterns.";
constexpr std::string_view kDepictGrep
    = R"(Search file contents using literal text and/or regular expression patterns (when both are given, the result is the union). Supports glob-based file filtering.
Use this to locate code, find references, or search logs across a project.)";

constexpr std::string_view kPathDesc
    = R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.)";
constexpr std::string_view kPathDescList
    = R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.
Shell wildcards are supported (`*`, `?`, `[...]`, and `**` for any directory depth), e.g. `src/*.cpp`; matching entries are listed instead of expanding their contents.)";
constexpr std::string_view kTimeoutDesc
    = R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)";

constexpr int32_t kAutoSummary = AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY;

/// 连续相同调用重复检查 (与内置工具 [XXToolBase::repeatCallCheck] 同语义):
/// 同一 llm <-> tool 交替链内 (无用户消息打断) 连续同名同参调用达到阈值
/// ([AgentConfig::toolcallRepeatCheckThreshold], 默认 5 次) 时, 主程序询问用户
/// 确认后才继续执行; read/list/glob/grep/write 启用, edit 不启用
constexpr int32_t kRepeatCheck = AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK;

/// 搜索/列表类只读工具的注册 flags: 自动摘要 + 声明并行安全 + 重复调用检查
/// - 输出可能远超 [agentxx::agent::AgentConfig::toolcallSummaryLimitOutputLength],
///   由主程序压缩 (原文经 share_store 卸载, 需要时可按行取回)
/// - list/glob/grep 只读且不改插件内部共享状态 (会话取消注册表只按
///   sessionId 读写), 允许与其他并行安全工具在同一批 tool_call 里并发执行;
///   写/编辑工具保持独占 (不声明), 与其他调用形成顺序屏障
constexpr int32_t kReadOnlyFlags
    = kAutoSummary | AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE | kRepeatCheck;

/// `read` 的注册 flags: 并行安全 + 重复调用检查, 不做自动摘要
/// - `read` 自带 `offset`/`limit` 分页参数, 读多少行由调用方决定;
///   主程序再按 [agentxx::agent::AgentConfig::toolcallSummaryLimitOutputLength]
///   自动摘要会把一次读取切成"头部 + share_store 页码", 与工具自身的分页重复 ——
///   调用方看到的行号会变成摘要后的结果行号, 与文件行号对不上
constexpr int32_t kReadFlags = AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE | kRepeatCheck;

/// `write` 的注册 flags: 只做重复调用检查
/// - 写文件保持独占 (不声明并行安全), 与其他调用形成顺序屏障
constexpr int32_t kWriteFlags = kRepeatCheck;

} // namespace

struct FsPluginCtx : public PluginBase {};

/// 构造按模式参数展开后的路径权限过滤器 (读作用域; 见 filesystem_impl.h 的 PathFilterFn)
/// - 已明确允许(Allow)才保留; 被拒绝(Deny)与未获批准(Ask)一律不进入结果 (fail-closed)
/// - 主程序未装配权限中间件时查询不可用, 过滤器返回空标记 → 工具跳过过滤 (保持原行为)
static PathFilterFn makeReadPathFilter(FsPluginCtx& ctx, std::string_view sessionId) {
    return [&ctx, sid = std::string{sessionId}](const std::vector<std::string>& paths) {
        return agentxx::plugin::filterPathPermissions(
            ctx,
            agentxx::plugin::PermissionScope::Read,
            sid,
            paths
        );
    };
}

/// 注册事务 (start 的实际内容); 失败由主程序按拒绝处理并回滚。
static int32_t fsSetup(FsPluginCtx& ctx) {
    // 说明: 每个工具注册后立即声明其权限限制 (目标参数 `path`, 读/写作用域),
    // 具体判定 (白/黑名单、permission.mode、记住的选择、工作区隔离) 由主程序
    // 权限中间件执行; 未声明的工具不参与权限判定 (直接放行)

    // 1. List
    auto listSchema
        = ctx.schema(kNameList)
              .string("path", kPathDescList, /*required=*/true)
              .boolean(
                  "recursive",
                  "Default `false`. If `true`, list subdirectories recursively; when `path` "
                  "contains wildcards, matched directories are expanded as well.",
                  false,
                  false
              )
              .integer(
                  "limit",
                  "Default `100`. Maximum number of entries to return (total lines in output). Set `limit <= 0` for unlimited.",
                  false,
                  100
              )
              .integer(
                  "max_files",
                  "Default `1000`. Maximum number of matched entries expanded before listing; a "
                  "larger expansion is cut to the first entries with a `[Note]` (raise it or narrow "
                  "`path`). Set `0` for no limit.",
                  false,
                  1000
              )
              .number("timeout", kTimeoutDesc, false, 60.0)
              .build();

    blocking_tool(
        ctx,
        kNameList,
        kDepictList,
        listSchema,
        [](FsPluginCtx&               c,
           std::string_view           args_json,
           std::string_view           tid,
           std::string_view           workDir,
           const PluginxxCancelToken* cancel) {
            ArgReader args(args_json);
            auto      path = args.require<std::string>("path");
            if (!args.ok()) {
                return args.errorMessage();
            }
            return fileListExecute(
                args.raw(),
                std::string(workDir),
                [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                },
                makeReadPathFilter(c, tid)
            );
        },
        0,
        kReadOnlyFlags
    );
    // 列表按读取类工具处理: 声明 `path` (可为通配模式) 决定是否询问一次;
    // 列出的条目 (含 recursive 下钻结果) 在工具内逐项复核权限后才输出
    registerReadPathPermission(ctx, kNameList, "path");

    // 2. Read
    auto readSchema
        = ctx.schema(kNameRead)
              .string("path", kPathDesc, /*required=*/true)
              .integer(
                  "offset",
                  R"(Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count.)",
                  false,
                  0
              )
              .integer(
                  "limit",
                  R"(Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error.)"
              )
              .build();

    if (utilxx_base::isAsyncFileIoSupported()) {
        polled_tool(
            ctx,
            kNameRead,
            kDepictRead,
            readSchema,
            [](FsPluginCtx&,
               std::string_view args_json,
               std::string_view,
               std::string_view workDir,
               const PluginxxCancelToken*) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    co_return args.errorMessage();
                }
                // 局部量: 其生命周期覆盖整个 co_await (异步读完整文件/逐行读)
                std::string workDirStr(workDir);
                co_return co_await fileReadExecuteAsync(args.raw(), workDirStr);
            },
            0,
            kReadFlags
        );
    } else {
        blocking_tool(
            ctx,
            kNameRead,
            kDepictRead,
            readSchema,
            [](FsPluginCtx&               c,
               std::string_view           args_json,
               std::string_view           tid,
               std::string_view           workDir,
               const PluginxxCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileReadExecute(args.raw(), std::string(workDir), [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            },
            0,
            kReadFlags
        );
    }
    registerReadPathPermission(ctx, kNameRead, "path");

    // 3. Write
    auto writeSchema
        = ctx.schema(kNameWrite)
              .string(
                  "path",
                  "Path to the target file. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                  /*required=*/true
              )
              .string("content", "Content to write into the file.", /*required=*/true)
              .boolean(
                  "overwrite",
                  R"(Controls write behavior:
`true`: Create the file if it doesn't exist; overwrite if it does.
`false`: Create a new file only; returns an error if the file already exists.)",
                  true
              )
              .build();

    if (utilxx_base::isAsyncFileIoSupported()) {
        polled_tool(
            ctx,
            kNameWrite,
            kDepictWrite,
            writeSchema,
            [](FsPluginCtx&,
               std::string_view args_json,
               std::string_view,
               std::string_view workDir,
               const PluginxxCancelToken*) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                auto      path    = args.require<std::string>("path");
                auto      content = args.require<std::string>("content");
                if (!args.ok()) {
                    co_return args.errorMessage();
                }
                std::string workDirStr(workDir);
                co_return co_await fileWriteExecuteAsync(args.raw(), workDirStr);
            },
            0,
            kWriteFlags
        );
    } else {
        blocking_tool(
            ctx,
            kNameWrite,
            kDepictWrite,
            writeSchema,
            [](FsPluginCtx&               c,
               std::string_view           args_json,
               std::string_view           tid,
               std::string_view           workDir,
               const PluginxxCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path    = args.require<std::string>("path");
                auto      content = args.require<std::string>("content");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileWriteExecute(args.raw(), std::string(workDir), [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            },
            0,
            kWriteFlags
        );
    }
    registerWritePathPermission(ctx, kNameWrite, "path");

    // 4. Edit
    // 两种写法: 多段 `edits: [{old_str,new_str}, ...]` (推荐, 一次改多处) 与
    // 单段 `old_str`/`new_str` (兼容旧写法); 两处同时给出时工具报错。
    // 每段只替换第一处匹配 (要改多处就写多段)
    auto editSchema
        = ctx.schema(kNameEdit)
              .string(
                  "path",
                  "Path to the text file. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                  /*required=*/true
              )
              .objectArray(
                  "edits",
                  R"(A list of replacements applied in order, each item is `{"old_str": "...", "new_str": "..."}`.
Every `old_str` must be non-empty and match the file content exactly (including whitespace and indentation); each item replaces the first match, and item N matches against the content produced by items 1..N-1, so a later item may target text written by an earlier one.
All items must match: when any item does not match, nothing is written and the error names that item.
Use this instead of `old_str`/`new_str` when changing several places in one file.)",
                  utilxx_base::Json{
                      {"type", "object"},
                      {"properties",
                       utilxx_base::Json{
                           {"old_str", utilxx_base::Json{{"type", "string"}}},
                           {"new_str", utilxx_base::Json{{"type", "string"}}},
                       }},
                      {"required", utilxx_base::Json::array({"old_str", "new_str"})},
                  }
              )
              .string(
                  "old_str",
                  "Single-edit shorthand: the exact string to find and replace (the first match is replaced). Must be non-empty and match precisely (including whitespace and indentation). Not used together with `edits`.",
                  false
              )
              .string(
                  "new_str",
                  "Single-edit shorthand: the replacement string.",
                  false
              )
              .build();

    if (utilxx_base::isAsyncFileIoSupported()) {
        polled_tool(
            ctx,
            kNameEdit,
            kDepictEdit,
            editSchema,
            [](FsPluginCtx&,
               std::string_view args_json,
               std::string_view,
               std::string_view workDir,
               const PluginxxCancelToken*) -> asio::awaitable<std::string> {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    co_return args.errorMessage();
                }
                std::string workDirStr(workDir);
                co_return co_await fileEditExecuteAsync(args.raw(), workDirStr);
            }
        );
    } else {
        blocking_tool(
            ctx,
            kNameEdit,
            kDepictEdit,
            editSchema,
            [](FsPluginCtx&               c,
               std::string_view           args_json,
               std::string_view           tid,
               std::string_view           workDir,
               const PluginxxCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileEditExecute(args.raw(), std::string(workDir), [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );
    }
    registerWritePathPermission(ctx, kNameEdit, "path");

    // 5. Glob
    // 权限限制: 先按 `file_patterns` (数组) 声明读权限 —— 这决定"是否询问用户一次"
    // (目标是模式表达的扫描起点); 模式实际展开出的路径在工具内用
    // makeReadPathFilter 逐项复核 (拒绝/未获批准的路径不进入结果), 防止
    // `**` 之类模式绕过针对子目录的拒绝规则
    auto globSchema
        = ctx.schema(kNameGlob)
              .stringArray(
                  "file_patterns",
                  "Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                  /*required=*/true
              )
              .stringArray("exclude_patterns", "Glob patterns to exclude.).")
              .integer(
                  "max_depth",
                  "Maximum directory depth relative to the pattern's base directory. Default `-1` (no limit).",
                  false,
                  -1
              )
              .boolean(
                  "sort",
                  "Default `false`. If `true`, sort results alphabetically.",
                  false,
                  false
              )
              .string(
                  "type",
                  "Filter results by file type: `file`, `dir`, `symlink`, `other`, `any`. Default: `any`.",
                  false,
                  "any"
              )
              .integer(
                  "max_files",
                  "Default `1000`. Maximum number of matched paths; when more paths match, the walk "
                  "stops there and the paths found so far are returned with a leading `[Note]` "
                  "(`0` = no limit).",
                  false,
                  1000
              )
              .number("timeout", kTimeoutDesc, false, 60.0)
              .build();

    blocking_tool(
        ctx,
        kNameGlob,
        kDepictGlob,
        globSchema,
        [](FsPluginCtx&               c,
           std::string_view           args_json,
           std::string_view           tid,
           std::string_view           workDir,
           const PluginxxCancelToken* cancel) {
            ArgReader args(args_json);
            auto      patterns = args.require<std::vector<std::string>>("file_patterns");
            if (!args.ok()) {
                return args.errorMessage();
            }
            return fileGlobExecute(
                args.raw(),
                std::string(workDir),
                [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                },
                makeReadPathFilter(c, tid)
            );
        },
        0,
        kReadOnlyFlags
    );
    registerReadPathPermission(ctx, kNameGlob, "file_patterns");

    // 6. Grep
    auto grepSchema
        = ctx.schema(kNameGrep)
              .stringArray(
                  "file_patterns",
                  "Path with glob patterns to select which files to search. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                  /*required=*/true
              )
              .stringArray(
                  "text_patterns",
                  "One or more literal text patterns (NOT regular expressions). A match is found if ANY pattern occurs."
              )
              .stringArray(
                  "regex_patterns",
                  "One or more regular expression patterns (like `grep -E`). A match is found if ANY pattern matches."
              )
              .stringArray(
                  "exclude_patterns",
                  "Glob patterns to exclude while walking. A matched entry is "
                  "skipped; a matched directory has its whole subtree pruned, so excluding a big "
                  "generated directory (e.g. `**/build/**`, `**/third_party/**`) also saves the walk."
              )
              .enumString(
                  "output_mode",
                  R"(Default: `files_with_matches`.
`files_with_matches`: Return file paths with match counts.
`content`: Return matching lines grouped by file.)",
                  {"files_with_matches", "content"},
                  false,
                  "files_with_matches"
              )
              .integer(
                  "max_count_per_file",
                  "Default `0` (no limit). Maximum matches to report per file.",
                  false,
                  0
              )
              .integer(
                  "max_files",
                  "Default `1000`. Maximum number of files to scan (after `file_patterns` expansion). "
                  "When more files match, the walk stops there and only the files found so far are "
                  "scanned, with a leading `[Note]` telling you how many were scanned: narrow "
                  "`file_patterns`, add `exclude_patterns`, or raise this value to scan more. "
                  "Set `0` for no limit.",
                  false,
                  1000
              )
              .number(
                  "max_file_size_mb",
                  "Default `32`. Files larger than this many MB are skipped (a trailing `[Note]` reports "
                  "how many were skipped). Set `0` for no limit.",
                  false,
                  32.0
              )
              .integer(
                  "context_lines",
                  "Default `0`. Number of context lines before and after each match.",
                  false,
                  0
              )
              .boolean(
                  "case_sensitive",
                  "Default `true`. If `false`, matching is case-insensitive.",
                  false,
                  true
              )
              .number("timeout", kTimeoutDesc, false, 60.0)
              .build();

    blocking_tool(
        ctx,
        kNameGrep,
        kDepictGrep,
        grepSchema,
        [](FsPluginCtx&               c,
           std::string_view           args_json,
           std::string_view           tid,
           std::string_view           workDir,
           const PluginxxCancelToken* cancel) {
            ArgReader args(args_json);
            auto      patterns = args.require<std::vector<std::string>>("file_patterns");
            if (!args.ok()) {
                return args.errorMessage();
            }
            return fileGrepExecute(
                args.raw(),
                std::string(workDir),
                [&] {
                    return pluginxx_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                },
                makeReadPathFilter(c, tid)
            );
        },
        0,
        kReadOnlyFlags
    );
    // 权限限制: 同 glob —— 按 `file_patterns` (数组) 声明读权限决定是否询问,
    // 扫描出的文件在工具内逐项复核后才读取 (被拒/未获批准的文件不读)
    registerReadPathPermission(ctx, kNameGrep, "file_patterns");

    return 0;
}

static void*
    fsStart(FsPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_filesystem start: notify required");
        }
        return nullptr;
    }
    if (fsSetup(ctx) != 0) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_filesystem start: registration failed");
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* fsStop(FsPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 无自管线程/定时器; 注册记录由主程序在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    FsPluginCtx,
    "agentxx_filesystem",
    "1.0.0",
    "File system tools: list, read, write, edit, glob, grep",
    fsStart,
    fsStop
);

struct FsClientCtx : public ClientPluginBase {};

/// client 侧注册事务 (start 的实际内容)。
static int32_t fsClientSetup(FsClientCtx& ctx) {
    // 1. List
    ctx.registerTemplate(kNameList, "List", "path");

    // 2. Write
    ctx.registerTemplate(kNameWrite, "Write", "path");

    // 3. Read
    ctx.registerRenderer(kNameRead, [](const ToolRenderInput& in, ToolRenderOutput& out) {
        ArgReader   args(in.argsJson);
        std::string path = args.value("path", "");
        int64_t     off  = args.value("offset", -1);
        int64_t     lim  = args.value("limit", -1);
        std::string rangeStr;
        if (off <= 0 && lim <= 0) {
        } else if (off <= 0) {
            rangeStr = fmt::format("0, {}", lim);
        } else if (lim <= 0) {
            rangeStr = fmt::format("{}, ~", off);
        } else {
            rangeStr = fmt::format("{}, {}", off, off + lim);
        }
        std::string summary = " ·";
        if (!rangeStr.empty()) {
            summary += " [" + rangeStr + "]";
        }
        if (!path.empty()) {
            summary += " " + path;
        }
        out.displayName = "Read";
        out.summary     = std::move(summary);
    });

    // 4. Glob
    ctx.registerRenderer(kNameGlob, [](const ToolRenderInput& in, ToolRenderOutput& out) {
        ArgReader    args(in.argsJson);
        auto         files = agentxx_fs_plugin::stringListArg(args.raw(), "file_patterns");
        std::string  joined;
        const size_t n = std::min<size_t>(files.size(), 2);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += files[i];
        }
        if (files.size() > 2) {
            joined += ", ...";
        }
        out.displayName = "Glob";
        if (!joined.empty()) {
            out.summary = " · " + joined;
        }
    });

    // 5. Grep
    ctx.registerRenderer(kNameGrep, [](const ToolRenderInput& in, ToolRenderOutput& out) {
        ArgReader args(in.argsJson);
        auto      textPats  = agentxx_fs_plugin::stringListArg(args.raw(), "text_patterns");
        auto      regexPats = agentxx_fs_plugin::stringListArg(args.raw(), "regex_patterns");
        auto      files     = agentxx_fs_plugin::stringListArg(args.raw(), "file_patterns");
        std::vector<std::string> shown;
        for (const auto& p : textPats) {
            if (shown.size() >= 2) {
                break;
            }
            shown.push_back(p);
        }
        for (const auto& p : regexPats) {
            if (shown.size() >= 2) {
                break;
            }
            shown.push_back(p);
        }
        const size_t totalPatterns = textPats.size() + regexPats.size();
        std::string  quoted;
        for (size_t i = 0; i < shown.size(); ++i) {
            if (i > 0) {
                quoted += ", ";
            }
            quoted += '"' + shown[i] + '"';
        }
        if (totalPatterns > 2) {
            quoted += ", ...";
        }
        std::string filesStr;
        for (size_t i = 0; i < std::min<size_t>(files.size(), 2); ++i) {
            if (i > 0) {
                filesStr += ", ";
            }
            filesStr += files[i];
        }
        if (files.size() > 2) {
            filesStr += ", ...";
        }
        std::string summary = fmt::format(" · [{}] {}", quoted, filesStr);
        out.displayName     = "Grep";
        out.summary         = std::move(summary);
    });

    // 6. Edit
    // 展开体按替换段展开: 界面描述层的 `Diff` 组件只描述一段 `oldStr`/`newStr`,
    // 因此多段替换表达为**多个 Diff 组件项** —— 文件路径只在首段显示, 段与段之间
    // 用分隔线隔开 (渲染层无需改动: 每个 Diff 项各渲染一份逐行差异)
    ctx.registerRenderer(kNameEdit, [](const ToolRenderInput& in, ToolRenderOutput& out) {
        ArgReader   args(in.argsJson);
        std::string path     = args.value("path", "");
        auto        segments = agentxx_fs_plugin::editSegmentsArg(args.raw());

        // 折叠头摘要: git 风格的 "+行 -行" 提示在前, 路径在后 (如 " · [+3 -1] /home/user/a.cpp")
        // - 行数统计见 [diffStatTextOfSegments]: 各段各替换一处, 逐行 diff 求和
        //   (与展开体渲染的差异同源, 因此显示的行数就是展开后能看到的)
        const std::string diffStat = agentxx_fs_plugin::diffStatTextOfSegments(segments);
        std::string summary = " ·";
        if (!diffStat.empty()) {
            summary += " " + diffStat;
        }
        if (!path.empty()) {
            summary += " " + path;
        }
        out.displayName = "Edit";
        out.summary     = std::move(summary);

        bool firstSegment = true;
        for (const auto& seg : segments) {
            if (seg.oldStr.empty() && seg.newStr.empty()) {
                continue;
            }
            if (!firstSegment) {
                utilxx_base::Json divider;
                divider["kind"] = "Divider";
                out.items.push_back(std::move(divider));
            }
            utilxx_base::Json diffItem;
            diffItem["kind"]   = "Diff";
            diffItem["path"]   = firstSegment ? std::string{path} : std::string{};
            diffItem["oldStr"] = seg.oldStr;
            diffItem["newStr"] = seg.newStr;
            out.items.push_back(std::move(diffItem));
            firstSegment = false;
        }
    });

    return 0;
}

static void*
    fsClientStart(FsClientCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_filesystem client start: notify required");
        }
        return nullptr;
    }
    if (fsClientSetup(ctx) != 0) {
        if (error) {
            PluginString::set(
                ctx.host,
                error,
                "agentxx_filesystem client start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* fsClientStop(FsClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // UI 注册记录由主程序在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_CLIENT_EXPORT(
    FsClientCtx,
    "agentxx_filesystem",
    "1.0.0",
    "Filesystem specialized renderer",
    fsClientStart,
    fsClientStop
);
