/// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
#include "agentxx_fs_plugin.h"
#include "filesystem_impl.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_fs_plugin;

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

std::string_view kPathDesc
    = R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.)";
std::string_view kTimeoutDesc
    = R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)";
std::string_view kGlobPatternHelp =
    R"(
| Wildcard | Matches | Example |
|----------|---------|---------|
| `*` | Any characters | `*.txt` matches all .txt files |
| `**` | Any directory recursively | `src/**/*.h` matches all .h files under src/ |
| `?` | Exactly one character | `file?.log` matches file1.log, fileA.log |
| `[ABC]` | One char from set | `[ABC]*.cpp` matches files starting with A, B, or C |
| `[A-Z]` | One char from range | `[A-Z]*` matches files starting with uppercase |
| `[!ABC]` | One char NOT in set | `[!ABC]*` matches files not starting with A, B, or C |
)";

std::string schemaList(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameList);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "path",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "path", kPathDesc),
                     },
                 },
             },
             {
                 "recursive",
                 {
                     {"type", "boolean"},
                     {"default", false},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "recursive",
                             "Default `false`. If `true`, list subdirectories recursively."
                         ),
                     },
                 },
             },
             {
                 "limit",
                 {
                     {"type", "integer"},
                     {"default", 100},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "limit",
                             "Default `100`. Maximum number of entries to return. Set `limit <= 0` for unlimited."
                         ),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "number"},
                     {"default", 60},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", kTimeoutDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"path"})}
    }.dump();
}

std::string schemaRead(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameRead);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "path",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "path", kPathDesc),
                     },
                 },
             },
             {
                 "line_offset",
                 {
                     {"type", "integer"},
                     {"default", 0},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "line_offset",
                             "Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count."
                         ),
                     },
                 },
             },
             {
                 "line_limit",
                 {
                     {"type", "integer"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "line_limit",
                             "Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error."
                         ),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"path"})}
    }.dump();
}

std::string schemaWrite(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameWrite);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "path",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "path",
                             "Path to the target file. Relative paths are resolved against the current working directory; `~` expands to the home directory."
                         ),
                     },
                 },
             },
             {
                 "content",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::
                             toolPromptArgDesc(p, "content", "Content to write into the file."),
                     },
                 },
             },
             {
                 "overwrite",
                 {
                     {"type", "boolean"},
                     {"default", false},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "overwrite",
                             "Default `false`. Controls write behavior:\n`true`: Create the file if it doesn't exist; overwrite if it does.\n`false`: Create a new file only; returns an error if the file already exists."
                         ),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"path", "content"})}
    }.dump();
}

std::string schemaEdit(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameEdit);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "path",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "path", kPathDesc),
                     },
                 },
             },
             {
                 "old_str",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "old_str",
                             "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation)."
                         ),
                     },
                 },
             },
             {
                 "new_str",
                 {{"type", "string"},
                  {
                      "description",
                      agentxx::plugin::toolPromptArgDesc(p, "new_str", "The replacement string."),
                  }},
             },
             {
                 "multi_replace",
                 {
                     {"type", "boolean"},
                     {"default", false},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "multi_replace",
                             "Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence."
                         ),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"path", "old_str", "new_str"})}
    }.dump();
}

std::string schemaGlob(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameGlob);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "file_patterns",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "file_patterns",
                             fmt::format(
                                 "Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.\n{}",
                                 kGlobPatternHelp
                             )
                         ),
                     },
                 },
             },
             {
                 "exclude_patterns",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "exclude_patterns",
                             "Glob patterns to exclude from results. Matched paths are removed.\nExample: `[\"**/node_modules/**\", \"**/.git/**\", \"**/build/**\"]`."
                         ),
                     },
                 },
             },
             {
                 "type",
                 {
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "type",
                             "Filter results by file type. Accepts a string or array of strings.\nValid values: `file`, `dir`, `symlink`, `other`, `any`.\nDefault: `any` (no filter).\nExample: `\"file\"` returns only regular files; `[\"file\",\"symlink\"]` returns files and symlinks."
                         ),
                     },
                 },
             },
             {
                 "max_depth",
                 {
                     {"type", "integer"},
                     {"default", -1},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "max_depth",
                             "Maximum directory depth relative to the pattern's base directory.\nDefault `-1` (no limit). Example: `max_depth=1` matches only direct children.\nSimilar to `find -maxdepth`."
                         ),
                     },
                 },
             },
             {
                 "sort",
                 {
                     {"type", "boolean"},
                     {"default", false},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "sort",
                             "Default `false`. If `true`, sort results alphabetically.\nResults are always deduplicated regardless of this setting."
                         ),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "number"},
                     {"default", 60},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", kTimeoutDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"file_patterns"})}
    }.dump();
}

std::string schemaGrep(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameGrep);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "text_patterns",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "text_patterns",
                             "One or more literal text patterns (NOT regular expressions). A match is found if ANY pattern occurs.\nOptional if `regex_patterns` is given; `text_patterns` and `regex_patterns` can be specified together, and the result is the union of both matches."
                         ),
                     },
                 },
             },
             {
                 "regex_patterns",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "regex_patterns",
                             "One or more regular expression patterns (like `grep -E`). A match is found if ANY pattern matches.\nOptional if `text_patterns` is given; `text_patterns` and `regex_patterns` can be specified together, and the result is the union of both matches.\nUse this for pattern-based matching (e.g. `line[0-9]+`, `throw|co_return`); use `text_patterns` for literal search."
                         ),
                     },
                 },
             },
             {
                 "file_patterns",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "file_patterns",
                             fmt::format(
                                 "Path with glob patterns to select which files to search. Relative paths are resolved against the current working directory; `~` expands to the home directory.\n{}",
                                 kGlobPatternHelp
                             )
                         ),
                     },
                 },
             },
             {
                 "case_sensitive",
                 {
                     {"type", "boolean"},
                     {"default", true},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "case_sensitive",
                             "Default `true`. If `false`, matching is case-insensitive (like `grep -i`)."
                         ),
                     },
                 },
             },
             {
                 "output_mode",
                 {
                     {"type", "string"},
                     {"enum", neograph::json::array({"content", "files_with_matches"})},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "output_mode",
                             "Default: `files_with_matches`.\n`files_with_matches`: Return file paths with match counts (format: `file:count`).\n`content`: Return matching lines grouped by file to reduce path repetition. Each file\nstarts with a header line `{filepath}:`, followed by that file's lines (`{line}:{content}`). Example:\n/path/to/file1:\n12:int foo() {\n40:int bar() {\n/path/to/file2:\n7:return 0;"
                         ),
                     },
                 },
             },
             {
                 "context_lines",
                 {
                     {"type", "integer"},
                     {"default", 0},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "context_lines",
                             "Default `0`. Number of context lines before and after each match.\nOnly applies to `content` output mode. Similar to `grep -C N`.\nContext lines use `-` separator; match lines use `:` separator."
                         ),
                     },
                 },
             },
             {
                 "max_count_per_file",
                 {
                     {"type", "integer"},
                     {"default", 0},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "max_count_per_file",
                             "Default `0` (no limit). Maximum matches to report per file.\nSimilar to `grep -m N`. Example: `max_count_per_file=3` stops after 3 matches per file."
                         ),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "number"},
                     {"default", 60},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", kTimeoutDesc),
                     },
                 },
             }},
         },
        // JSON Schema 无法表达 "至少指定其一"; 实现在参数缺失时报错,
        // 并在两个数组参数的描述中说明至少其一 (可同时指定取并集)
        {"required", neograph::json::array({"file_patterns"})}
    }.dump();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_filesystem"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "File system tools: list, read, write, edit, glob, grep"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            // list (workDir 预取在 io 线程，避免 worker 跨线程 ioCallSync)
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameList,
                kDepictList,
                schemaList(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0) {
                            return true;
                        }
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    return fileListExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // glob
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameGlob,
                kDepictGlob,
                schemaGlob(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0) {
                            return true;
                        }
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    return fileGlobExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // grep
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameGrep,
                kDepictGrep,
                schemaGrep(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir,
                   volatile int*    cancel_flag) -> std::string {
                    auto arguments   = args_json.empty() ? neograph::json::object()
                                                         : neograph::json::parse(args_json);
                    auto isCancelled = [&c, tid, cancel_flag]() -> bool {
                        if (cancel_flag && *cancel_flag != 0) {
                            return true;
                        }
                        return c.sessionCancelled(
                            agentxx::plugin::PluginStringView::from(tid.data(), tid.size())
                        );
                    };
                    return fileGrepExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // read (workDir 预取)
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameRead,
                kDepictRead,
                schemaRead(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    (void)c;
                    (void)tid;
                    return fileReadExecute(arguments, std::string(workDir));
                }
            );

            // write
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameWrite,
                kDepictWrite,
                schemaWrite(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    (void)c;
                    (void)tid;
                    return fileWriteExecute(arguments, std::string(workDir));
                }
            );

            // edit
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameEdit,
                kDepictEdit,
                schemaEdit(ctx.get()),
                [](PluginCtx&       c,
                   std::string_view args_json,
                   std::string_view tid,
                   std::string_view workDir) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    (void)c;
                    (void)tid;
                    return fileEditExecute(arguments, std::string(workDir));
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}

/// ==================== Client 侧入口 ====================

namespace {

struct ClientCtx {
    const AgentxxPluginHost*                            host = nullptr;
    agentxx::plugin::ClientIfaces                       iface{};
    std::vector<std::unique_ptr<void, void (*)(void*)>> shims;

    void logErr(const char* m) const noexcept {
        agentxx::plugin::logTo(host, iface.log, 4, "agentxx_filesystem", m ? m : "");
    }
};

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        0,
        agentxx::plugin::PluginStringView::fromCstr("agentxx_filesystem"),
        agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
        agentxx::plugin::PluginStringView::fromCstr("Filesystem tools specialized UI renderer"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int32_t AGENTXX_PLUGIN_CALL
    agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* m) noexcept {
            if (raw) {
                raw->logErr(m);
            }
        },
        -1,
        [&]() -> int32_t {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            raw        = ctx.get();

            if (!ctx->iface.ui) {
                *plugin_ctx = ctx.release();
                return 0;
            }

            // 1. List (预设模版)
            agentxx::plugin::registerToolTemplate(host, ctx->iface.ui, kNameList, "List", "path");

            // 2. Write (预设模版)
            agentxx::plugin::registerToolTemplate(host, ctx->iface.ui, kNameWrite, "Write", "path");

            // 3. Read (回调函数: [offset, limit] 区间参数)
            agentxx::plugin::registerToolRenderer(
                host,
                ctx->iface.ui,
                kNameRead,
                [](const agentxx::plugin::ToolRenderInput& in,
                   agentxx::plugin::ToolRenderOutput&      out) {
                    neograph::json args;
                    try {
                        args = neograph::json::parse(in.argsJson);
                    } catch (...) {
                        return;
                    }
                    if (!args.is_object()) {
                        return;
                    }
                    std::string path = args.value("path", std::string{});
                    int64_t     off  = args.value("line_offset", int64_t{-1});
                    int64_t     lim  = args.value("line_limit", int64_t{-1});
                    std::string rangeStr;
                    if (off <= 0 && lim <= 0) {
                        // empty
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
                },
                ctx->shims
            );

            // 4. Glob (回调函数: 数组折叠)
            agentxx::plugin::registerToolRenderer(
                host,
                ctx->iface.ui,
                kNameGlob,
                [](const agentxx::plugin::ToolRenderInput& in,
                   agentxx::plugin::ToolRenderOutput&      out) {
                    neograph::json args;
                    try {
                        args = neograph::json::parse(in.argsJson);
                    } catch (...) {
                        return;
                    }
                    if (!args.is_object()) {
                        return;
                    }
                    auto files = agentxx_fs_plugin::stringListArg(args, "file_patterns");
                    std::string  joined;
                    const size_t n = std::min<size_t>(files.size(), 2);
                    for (size_t i = 0; i < n; ++i) {
                        if (i > 0) {
                            joined += ", ";
                        }
                        joined += files[i];
                    }
                    if (files.size() > 2) {
                        joined += (n > 0 ? ", ..." : "...");
                    }
                    out.displayName = "Glob";
                    out.summary     = " · " + joined;
                },
                ctx->shims
            );

            // 5. Grep (回调函数: 引号包裹匹配模式 + 文件列表)
            agentxx::plugin::registerToolRenderer(
                host,
                ctx->iface.ui,
                kNameGrep,
                [](const agentxx::plugin::ToolRenderInput& in,
                   agentxx::plugin::ToolRenderOutput&      out) {
                    neograph::json args;
                    try {
                        args = neograph::json::parse(in.argsJson);
                    } catch (...) {
                        return;
                    }
                    if (!args.is_object()) {
                        return;
                    }
                    auto textPats = agentxx_fs_plugin::stringListArg(args, "text_patterns");
                    auto regexPats
                        = agentxx_fs_plugin::stringListArg(args, "regex_patterns");
                    auto files = agentxx_fs_plugin::stringListArg(args, "file_patterns");
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
                    if (totalPatterns > shown.size()) {
                        quoted += ", ...";
                    }
                    std::string  joinedFiles;
                    const size_t n = std::min<size_t>(files.size(), 2);
                    for (size_t i = 0; i < n; ++i) {
                        if (i > 0) {
                            joinedFiles += ", ";
                        }
                        joinedFiles += files[i];
                    }
                    if (files.size() > 2) {
                        joinedFiles += (n > 0 ? ", ..." : "...");
                    }
                    std::string summary = " ·";
                    if (!quoted.empty()) {
                        summary += " [" + quoted + "]";
                    }
                    if (!joinedFiles.empty()) {
                        summary += " " + joinedFiles;
                    }
                    out.displayName = "Grep";
                    out.summary     = std::move(summary);
                },
                ctx->shims
            );

            // 6. Edit (回调函数: path 摘要 + diff items)
            agentxx::plugin::registerToolRenderer(
                host,
                ctx->iface.ui,
                kNameEdit,
                [](const agentxx::plugin::ToolRenderInput& in,
                   agentxx::plugin::ToolRenderOutput&      out) {
                    neograph::json args;
                    try {
                        args = neograph::json::parse(in.argsJson);
                    } catch (...) {
                        return;
                    }
                    if (!args.is_object()) {
                        return;
                    }
                    std::string path   = args.value("path", std::string{});
                    std::string oldStr = args.value("old_str", std::string{});
                    std::string newStr = args.value("new_str", std::string{});

                    out.displayName = "Edit";
                    out.summary     = " · " + path;

                    if (!in.isError) {
                        neograph::json diffItem;
                        diffItem["kind"]    = "diff";
                        diffItem["path"]    = std::move(path);
                        diffItem["old_str"] = std::move(oldStr);
                        diffItem["new_str"] = std::move(newStr);
                        out.items.push_back(std::move(diffItem));
                    }
                },
                ctx->shims
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void AGENTXX_PLUGIN_CALL
    agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* m) noexcept {
            if (ctx) {
                ctx->logErr(m);
            }
        },
        [&] {
            delete ctx;
        }
    );
}
