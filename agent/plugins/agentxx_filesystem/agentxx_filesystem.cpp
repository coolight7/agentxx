/// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
///
/// 分工 (见 plugin_kit.h 的 polled_tool 说明):
/// - `read` / `write` / `edit`: 实现体是 asio 协程 (`asio::stream_file` 异步读写),
///   等待的是插件本地 reactor 上的文件 IO 就绪事件, 因此文件异步 I/O 可用时注册为
///   **声明式受控轮询**工具 —— 大文件读写不再占用宿主工作线程池;
///   文件异步 I/O 不可用 (编译期未启用, 或运行环境的 io_uring 被 seccomp 拦截)
///   时回退同步实现, 改注册 `blocking_tool` (offload 工作线程);
///   可用性判断统一走 `utilxx_base::isAsyncFileIoSupported()`;
/// - `list` / `glob` / `grep`: 目录遍历 + 全文件扫描 + 正则/编码转换, 这是 CPU/阻塞
///   工作而非异步 IO, 放进受控轮询只会阻塞宿主 IO 线程, 因此保持 `blocking_tool`。
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
    = R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.
Supports offset/limit for reading portions of large files.)";
constexpr std::string_view kDepictWrite
    = "Create a new file or overwrite an existing file with the given content.";
constexpr std::string_view kDepictEdit
    = R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.
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

} // namespace

struct FsPluginCtx : public PluginBase {};

/// 构造按模式参数展开后的路径权限过滤器 (读作用域; 见 filesystem_impl.h 的 PathFilterFn)
/// - 已明确允许(Allow)才保留; 被拒绝(Deny)与未获批准(Ask)一律不进入结果 (fail-closed)
/// - 宿主未装配权限中间件时查询不可用, 过滤器返回空标记 → 工具跳过过滤 (保持原行为)
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

/// 注册事务 (start 的实际内容); 失败由宿主按拒绝处理并回滚。
static int32_t fsSetup(FsPluginCtx& ctx) {
    // 说明: 每个工具注册后立即声明其权限限制 (目标参数 `path`, 读/写作用域),
    // 具体判定 (白/黑名单、permission.mode、记住的选择、工作区隔离) 由宿主
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
        kAutoSummary
    );
    // 列表按读取类工具处理: 声明 `path` (可为通配模式) 决定是否询问一次;
    // 列出的条目 (含 recursive 下钻结果) 在工具内逐项复核权限后才输出
    registerReadPathPermission(ctx, kNameList, "path");

    // 2. Read
    auto readSchema
        = ctx.schema(kNameRead)
              .string("path", kPathDesc, /*required=*/true)
              .integer(
                  "line_offset",
                  "Default `0`. Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count.",
                  false,
                  0
              )
              .integer(
                  "line_limit",
                  "Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error."
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
            }
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
            }
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
                  R"(Default `false`. Controls write behavior:
`true`: Create the file if it doesn't exist; overwrite if it does.
`false`: Create a new file only; returns an error if the file already exists.)",
                  false,
                  false
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
            }
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
            }
        );
    }
    registerWritePathPermission(ctx, kNameWrite, "path");

    // 4. Edit
    auto editSchema
        = ctx.schema(kNameEdit)
              .string(
                  "path",
                  "Path to the text file. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                  /*required=*/true
              )
              .string(
                  "old_str",
                  "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation).",
                  /*required=*/true
              )
              .string("new_str", "The replacement string.", /*required=*/true)
              .boolean(
                  "multi_replace",
                  "Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence.",
                  false,
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
                auto      path   = args.require<std::string>("path");
                auto      oldStr = args.require<std::string>("old_str");
                auto      newStr = args.require<std::string>("new_str");
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
                auto      path   = args.require<std::string>("path");
                auto      oldStr = args.require<std::string>("old_str");
                auto      newStr = args.require<std::string>("new_str");
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
        kAutoSummary
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
        kAutoSummary
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
    // 无自管线程/定时器; 注册记录由宿主在 stop 后统一撤销。
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
        int64_t     off  = args.value("line_offset", -1);
        int64_t     lim  = args.value("line_limit", -1);
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
    ctx.registerRenderer(kNameEdit, [](const ToolRenderInput& in, ToolRenderOutput& out) {
        ArgReader   args(in.argsJson);
        std::string path   = args.value("path", "");
        std::string oldStr = args.value("old_str", "");
        std::string newStr = args.value("new_str", "");
        out.displayName    = "Edit";
        // 折叠头摘要: git 风格的 "+行 -行" 提示在前, 路径在后 (如 " · [+3 -1] /home/user/a.cpp")
        // - 行数统计见 [diffStatText] (与展开体 diff 的逐行结果同源)
        // - multi_replace 一处模式会替换多处, 实际处数只有结果里才有
        //   ("Success, Replace N hits", 见 [parseEditReplaceHits]), 因此完成前按
        //   单处展示, 完成后换成整个文件的总行数 (单处 × 处数)
        int64_t repeat = 1;
        if (args.value("multi_replace", false) && in.isFinished) {
            const int64_t hits = parseEditReplaceHits(in.resultText);
            if (hits > 1) {
                repeat = hits;
            }
        }
        const std::string diffStat = diffStatText(oldStr, newStr, repeat);
        std::string       summary  = " ·";
        if (!diffStat.empty()) {
            summary += " " + diffStat;
        }
        if (!path.empty()) {
            summary += " " + path;
        }
        out.summary = std::move(summary);
        if (!path.empty() && (!oldStr.empty() || !newStr.empty())) {
            utilxx_base::Json diffItem;
            diffItem["kind"]    = "diff";
            diffItem["path"]    = path;
            diffItem["old_str"] = std::move(oldStr);
            diffItem["new_str"] = std::move(newStr);
            out.items.push_back(std::move(diffItem));
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
    // UI 注册记录由宿主在 stop 后统一撤销。
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
