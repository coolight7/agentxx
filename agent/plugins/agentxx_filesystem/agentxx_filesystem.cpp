/// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
#include "agentxx_fs_plugin.h"
#include "filesystem_impl.h"
#include "fmt/format.h"
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
Can also be used to check whether a specific file or directory exists.)";
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
constexpr std::string_view kTimeoutDesc
    = R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)";

} // namespace

struct FsPluginCtx : public PluginBase {};

/// 注册事务 (start 的实际内容); 失败由宿主按拒绝处理并回滚。
static int32_t fsSetup(FsPluginCtx& ctx) {
        // 1. List
        auto listSchema
            = ctx.schema(kNameList)
                  .string("path", kPathDesc, /*required=*/true)
                  .boolean(
                      "recursive",
                      "Default `false`. If `true`, list subdirectories recursively.",
                      false,
                      false
                  )
                  .integer(
                      "limit",
                      "Default `100`. Maximum number of entries to return. Set `limit <= 0` for unlimited.",
                      false,
                      100
                  )
                  .number("timeout", kTimeoutDesc, false, 60.0)
                  .build();

        blocking_tool(
            ctx,
            kNameList,
            kDepictList,
            listSchema,
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileListExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

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

        blocking_tool(
            ctx,
            kNameRead,
            kDepictRead,
            readSchema,
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path = args.require<std::string>("path");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileReadExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

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

        blocking_tool(
            ctx,
            kNameWrite,
            kDepictWrite,
            writeSchema,
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path    = args.require<std::string>("path");
                auto      content = args.require<std::string>("content");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileWriteExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

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

        blocking_tool(
            ctx,
            kNameEdit,
            kDepictEdit,
            editSchema,
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      path   = args.require<std::string>("path");
                auto      oldStr = args.require<std::string>("old_str");
                auto      newStr = args.require<std::string>("new_str");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileEditExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

        // 5. Glob
        auto globSchema
            = ctx.schema(kNameGlob)
                  .stringArray(
                      "file_patterns",
                      "Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.",
                      /*required=*/true
                  )
                  .stringArray(
                      "exclude_patterns",
                      "Glob patterns to exclude from results. Matched paths are removed."
                  )
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
                  .number("timeout", kTimeoutDesc, false, 60.0)
                  .build();

        blocking_tool(
            ctx,
            kNameGlob,
            kDepictGlob,
            globSchema,
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      patterns = args.require<std::vector<std::string>>("file_patterns");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileGlobExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

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
            [](FsPluginCtx&      c,
               std::string_view  args_json,
               std::string_view  tid,
               std::string_view  workDir,
               const AgentxxPluginCancelToken* cancel) {
                ArgReader args(args_json);
                auto      patterns = args.require<std::vector<std::string>>("file_patterns");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                return fileGrepExecute(args.raw(), std::string(workDir), [&] {
                    return agentxx_plugin_cancel_is_requested(cancel) != 0 || c.sessionCancelled(tid);
                });
            }
        );

        return 0;
}

static void* fsStart(
    FsPluginCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
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
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* fsStop(FsPluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    // 无自管线程/定时器; 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(FsPluginCtx, fsStart, fsStop)

AGENTXX_PLUGIN_AGENT_EXPORT(
    FsPluginCtx,
    "agentxx_filesystem",
    "1.0.0",
    "File system tools: list, read, write, edit, glob, grep",
    [](FsPluginCtx&) -> int32_t {
        // create 只构造上下文; 工具注册在 start 事务中执行。
        return 0;
    }
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
                rangeStr = fmt::format("{}", off);
            } else {
                rangeStr = fmt::format("{}, {}", off, lim);
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
            std::string summary;
            if (!quoted.empty() && !filesStr.empty()) {
                summary = fmt::format(" · {} in {}", quoted, filesStr);
            } else if (!quoted.empty()) {
                summary = " · " + quoted;
            } else if (!filesStr.empty()) {
                summary = " · in " + filesStr;
            }
            out.displayName = "Grep";
            out.summary     = std::move(summary);
        });

        // 6. Edit
        ctx.registerRenderer(kNameEdit, [](const ToolRenderInput& in, ToolRenderOutput& out) {
            ArgReader   args(in.argsJson);
            std::string path = args.value("path", "");
            out.displayName  = "Edit";
            if (!path.empty()) {
                out.summary = " · " + path;
            }
            std::string oldStr = args.value("old_str", "");
            std::string newStr = args.value("new_str", "");
            if (!path.empty() && (!oldStr.empty() || !newStr.empty())) {
                agentxx::util::Json diffItem;
                diffItem["kind"]    = "diff";
                diffItem["path"]    = path;
                diffItem["old_str"] = std::move(oldStr);
                diffItem["new_str"] = std::move(newStr);
                out.items.push_back(std::move(diffItem));
            }
        });

        return 0;
}

static void* fsClientStart(
    FsClientCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
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
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* fsClientStop(
    FsClientCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    // UI 注册记录由宿主在 stop 后统一撤销。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(FsClientCtx, fsClientStart, fsClientStop)

AGENTXX_PLUGIN_CLIENT_EXPORT(
    FsClientCtx,
    "agentxx_filesystem",
    "1.0.0",
    "Filesystem specialized renderer",
    [](FsClientCtx&) -> int32_t {
        // create 只构造上下文; UI 注册在 start 事务中执行。
        return 0;
    }
);
