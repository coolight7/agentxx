// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
#include "agentxx_fs_plugin.h"
#include "filesystem_impl.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_fs_plugin;

namespace {

constexpr auto kNameList  = "agentxx_filesystem_list";
constexpr auto kNameRead  = "agentxx_filesystem_read";
constexpr auto kNameWrite = "agentxx_filesystem_write";
constexpr auto kNameEdit  = "agentxx_filesystem_edit";
constexpr auto kNameGlob  = "agentxx_filesystem_glob";
constexpr auto kNameGrep  = "agentxx_filesystem_grep";

constexpr auto kDepictList
    = R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.
Directory paths end with `/`, symlinks show their target. Types: `d` directory, `-` file, `l` symlink.
Can also be used to check whether a specific file or directory exists.)";
constexpr auto kDepictRead
    = R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.
Supports offset/limit for reading portions of large files.)";
constexpr auto kDepictWrite
    = "Create a new file or overwrite an existing file with the given content.";
constexpr auto kDepictEdit
    = R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.
Note! This tool will replace all `\r\n` to `\n` when find `old_str` and replace.)";
constexpr auto kDepictGlob = "Find files and directories matching glob patterns.";
constexpr auto kDepictGrep
    = R"(Search file contents using text or regular expressions. Supports glob-based file filtering.
Use this to locate code, find references, or search logs across a project.)";

std::string
    argDesc(const agentxx::kit::ToolPromptText& p, const char* key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

const char* kPathDesc
    = R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.)";
const char* kTimeoutDesc
    = R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)";
const char* kGlobPatternHelp =
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
        {"type",       "object"                                    },
        {"properties",
         {{"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
          {"recursive",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             argDesc(p, "recursive", "Default `false`. If `true`, list subdirectories recursively.")
            }}},
          {"limit",
           {{"type", "integer"},
            {"default", 100},
            {"description",
             argDesc(
                 p,
                 "limit",
                 "Default `100`. Maximum number of entries to return. Set `limit <= 0` for unlimited."
             )}}},
          {"timeout",
           {{"type", "number"},
            {"default", 60},
            {"description", argDesc(p, "timeout", kTimeoutDesc)}}}}},
        {"required",   neograph::json::array({"path"})             }
    }.dump();
}

std::string schemaRead(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameRead);
    return neograph::json{
        {"type",       "object"                       },
        {"properties",
         {{"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
          {"line_offset",
           {{"type", "integer"},
            {"default", 0},
            {"description",
             argDesc(
                 p,
                 "line_offset",
                 "Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count."
             )}}},
          {"line_limit",
           {{"type", "integer"},
            {"description",
             argDesc(
                 p,
                 "line_limit",
                 "Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error."
             )}}}}                                    },
        {"required",   neograph::json::array({"path"})}
    }.dump();
}

std::string schemaWrite(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameWrite);
    return neograph::json{
        {"type",       "object"                                  },
        {"properties",
         {{"path",
           {{"type", "string"},
            {"description",
             argDesc(
                 p,
                 "path",
                 "Path to the target file. Relative paths are resolved against the current working directory; `~` expands to the home directory."
             )}}},
          {"content",
           {{"type", "string"},
            {"description", argDesc(p, "content", "Content to write into the file.")}}},
          {"overwrite",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             argDesc(
                 p,
                 "overwrite",
                 "Default `false`. Controls write behavior:\n`true`: Create the file if it doesn't exist; overwrite if it does.\n`false`: Create a new file only; returns an error if the file already exists."
             )}}}}                                               },
        {"required",   neograph::json::array({"path", "content"})}
    }.dump();
}

std::string schemaEdit(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameEdit);
    return neograph::json{
        {"type",       "object"                                             },
        {"properties",
         {{"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
          {"old_str",
           {{"type", "string"},
            {"description",
             argDesc(
                 p,
                 "old_str",
                 "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation)."
             )}}},
          {"new_str",
           {{"type", "string"}, {"description", argDesc(p, "new_str", "The replacement string.")}}},
          {"multi_replace",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             argDesc(
                 p,
                 "multi_replace",
                 "Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence."
             )}}}}                                                          },
        {"required",   neograph::json::array({"path", "old_str", "new_str"})}
    }.dump();
}

std::string schemaGlob(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameGlob);
    return neograph::json{
        {"type",       "object"                                    },
        {"properties",
         {{"file_patterns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             argDesc(
                 p,
                 "file_patterns",
                 std::string(
                     "Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.\n"
                 ) + kGlobPatternHelp
             )}}},
          {"exclude_patterns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             argDesc(
                 p,
                 "exclude_patterns",
                 "Glob patterns to exclude from results. Matched paths are removed.\nExample: `[\"**/node_modules/**\", \"**/.git/**\", \"**/build/**\"]`."
             )}}},
          {"type",
           {{"description",
             argDesc(
                 p,
                 "type",
                 "Filter results by file type. Accepts a string or array of strings.\nValid values: `file`, `dir`, `symlink`, `other`, `any`.\nDefault: `any` (no filter).\nExample: `\"file\"` returns only regular files; `[\"file\",\"symlink\"]` returns files and symlinks."
             )}}},
          {"max_depth",
           {{"type", "integer"},
            {"default", -1},
            {"description",
             argDesc(
                 p,
                 "max_depth",
                 "Maximum directory depth relative to the pattern's base directory.\nDefault `-1` (no limit). Example: `max_depth=1` matches only direct children.\nSimilar to `find -maxdepth`."
             )}}},
          {"sort",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             argDesc(
                 p,
                 "sort",
                 "Default `false`. If `true`, sort results alphabetically.\nResults are always deduplicated regardless of this setting."
             )}}},
          {"timeout",
           {{"type", "number"},
            {"default", 60},
            {"description", argDesc(p, "timeout", kTimeoutDesc)}}}}},
        {"required",   neograph::json::array({"file_patterns"})    }
    }.dump();
}

std::string schemaGrep(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameGrep);
    return neograph::json{
        {"type",       "object"                                                 },
        {"properties",
         {{"text_patterns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             argDesc(
                 p,
                 "text_patterns",
                 "One or more search patterns (text or regex, depending on `text_patterns_is_regex`).\nA match is found if ANY pattern matches."
             )}}},
          {"file_patterns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             argDesc(
                 p,
                 "file_patterns",
                 std::string(
                     "Path with glob patterns to select which files to search. Relative paths are resolved against the current working directory; `~` expands to the home directory.\n"
                 ) + kGlobPatternHelp
             )}}},
          {"case_sensitive",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             argDesc(
                 p,
                 "case_sensitive",
                 "Default `true`. If `false`, matching is case-insensitive (like `grep -i`)."
             )}}},
          {"text_patterns_is_regex",
           {{"type", "boolean"},
            {"description",
             argDesc(
                 p,
                 "text_patterns_is_regex",
                 "Determines how `text_patterns` are interpreted.\n`true`: Patterns are regular expressions.\n`false`: Patterns are literal text strings."
             )}}},
          {"output_mode",
           {{"type", "string"},
            {"enum", neograph::json::array({"content", "files_with_matches"})},
            {"description",
             argDesc(
                 p,
                 "output_mode",
                 "Default: `files_with_matches`.\n`files_with_matches`: Return file paths with match counts (format: `file:count`).\n`content`: Return matching lines grouped by file to reduce path repetition. Each file\nstarts with a header line `{filepath}:`, followed by that file's lines (`{line}:{content}`). Example:\n/path/to/file1:\n12:int foo() {\n40:int bar() {\n/path/to/file2:\n7:return 0;"
             )}}},
          {"context_lines",
           {{"type", "integer"},
            {"default", 0},
            {"description",
             argDesc(
                 p,
                 "context_lines",
                 "Default `0`. Number of context lines before and after each match.\nOnly applies to `content` output mode. Similar to `grep -C N`.\nContext lines use `-` separator; match lines use `:` separator."
             )}}},
          {"max_count_per_file",
           {{"type", "integer"},
            {"default", 0},
            {"description",
             argDesc(
                 p,
                 "max_count_per_file",
                 "Default `0` (no limit). Maximum matches to report per file.\nSimilar to `grep -m N`. Example: `max_count_per_file=3` stops after 3 matches per file."
             )}}},
          {"timeout",
           {{"type", "number"},
            {"default", 60},
            {"description", argDesc(p, "timeout", kTimeoutDesc)}}}}             },
        {"required",   neograph::json::array({"text_patterns", "file_patterns"})}
    }.dump();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                AGENTXX_SV("agentxx_filesystem"),
                AGENTXX_SV("1.0.0"),
                AGENTXX_SV("File system tools: list, read, write, edit, glob, grep"),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
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
            agentxx::kit::blocking_tool(
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
                        return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                    };
                    return fileListExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // glob
            agentxx::kit::blocking_tool(
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
                        return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                    };
                    return fileGlobExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // grep
            agentxx::kit::blocking_tool(
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
                        return c.sessionCancelled(agentxx_plugin_sv(tid.data(), tid.size()));
                    };
                    return fileGrepExecute(arguments, std::string(workDir), isCancelled);
                }
            );

            // read (workDir 预取)
            agentxx::kit::blocking_tool(
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
            agentxx::kit::blocking_tool(
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
            agentxx::kit::blocking_tool(
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
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
