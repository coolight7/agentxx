// agentxx_filesystem —— 文件系统工具插件 (list / read / write / edit / glob / grep)
// - 从 libagentxx src/tools/filesystem 拆分独立 (同名同行为)
// - 会话工作目录经宿主 agentxx.agent.config v2 get_work_dir 获取 (相对路径基准,
//   原实现经 AgentContext::wsAbs 同语义); 取消查询经 agentxx.agent.cancel 接口表
//   (长遍历 list/glob/grep 循环内轮询, 与原 asyncWithTimeout/取消 watcher 一致)
// - 业务逻辑在 filesystem_impl.h (纯函数, 测试直测同一实现)
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

constexpr auto kDepictList = R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.
Directory paths end with `/`, symlinks show their target. Types: `d` directory, `-` file, `l` symlink.
Can also be used to check whether a specific file or directory exists.)";
constexpr auto kDepictRead = R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.
Supports offset/limit for reading portions of large files.)";
constexpr auto kDepictWrite    = "Create a new file or overwrite an existing file with the given content.";
constexpr auto kDepictEdit     = R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.
Note! This tool will replace all `\r\n` to `\n` when find `old_str` and replace.)";
constexpr auto kDepictGlob     = "Find files and directories matching glob patterns.";
constexpr auto kDepictGrep     = R"(Search file contents using text or regular expressions. Supports glob-based file filtering.
Use this to locate code, find references, or search logs across a project.)";

/// 参数说明兜底 (正常情况下由宿主 toolPrompt 提供完整文案; 默认文案与 lib
/// prompt.h 同步, 宿主未装配 prompt 接口时保证描述可用)
/// fallback 兼容字面量与运行期拼接的说明文本 (glob/grep 的通配符表为动态拼接)
std::string argDesc(const ToolPromptText& p, const char* key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

const char* kPathDesc =
    R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.)";
const char* kTimeoutDesc =
    R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)";
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

/// 注册常规工具 (schema/描述存储于插件侧静态区; 宿主注册时拷贝)
/// - 统一异步操作模型: 经阻塞委托垫片注册 (offload 池线程执行, io 线程
///   只等完成通知); execute 签名追加 cancel_flag 形参
void registerTool(
    const char*        name,
    const char*        defaultDepict,
    const std::string& schema,
    char* (*execute)(
        void*                   user_data,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        volatile int*           cancel_flag,
        char**                  error_out
    )
) {
    static std::vector<std::string> g_storage;
    std::string                     depict = readToolPrompt(name).depict;
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = nullptr;
    spec.flags           = AGENTXX_TOOL_FLAG_NONE;
    spec.execute         = execute;
    if (agentxx_register_sync_tool(g_host, &spec) != 0) {
        XX_LOGW("agentxx_filesystem: register tool {} failed", name);
    }
}

/// C ABI execute 包装: 解析参数 JSON → 调用实现 (workDir + 取消查询注入) →
/// 结果 strdup (异常不外泄; impl 内部已把可预期错误编码进返回文本)
/// - 取消双通道: cancel_flag 由宿主 op 驱动器在会话取消/超时时置位;
///   会话取消查询经宿主 cancel 接口表按 thread_id 轮询 (impl 在长遍历
///   循环内调用; 接口缺失时传 nullptr 等价无取消支持)
template<auto ExecFn>
char* wrapExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    (void)user_data;
    (void)tool_call_id;
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto arguments = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
        std::function<bool()> isCancelled;
        if (cancel_flag) {
            int flag = *cancel_flag;
            if (flag) {
                return pluginStrdup("[Error] Cancelled");
            }
            isCancelled = [cancel_flag]() -> bool {
                return *cancel_flag != 0;
            };
        }
        if (!isCancelled && g_if.cancel && g_if.cancel->is_cancelled && thread_id.data) {
            std::string tid{thread_id.data, thread_id.size};
            isCancelled  = [tid]() -> bool {
                return g_if.cancel->is_cancelled(
                           g_host,
                           agentxx_plugin_sv(tid.data(), tid.size())
                       )
                    != 0;
            };
        }
        auto result = ExecFn(arguments, workDir(), isCancelled);
        return pluginStrdup(result.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup("unknown exception");
        }
        return nullptr;
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理, 从库名推导插件名)
    XX_PGUARD_BEGIN
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_filesystem"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Filesystem tools: list / read / write / edit / glob / grep"),
    };
    return &info;
    XX_PGUARD_END_RET(nullptr)
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 内含 JSON schema 构建/接口查询等可抛操作,
    // 异常返回 -1 走宿主加载失败清理路径 (异常穿越 C ABI 即 UB)
    XX_PGUARD_BEGIN
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    g_host      = host;
    g_if        = agentxx::plugin::AgentIfaces::query(host);
    *plugin_ctx = nullptr;

    if (workDir().empty()) {
        XX_LOGW(
            "agentxx_filesystem: host work_dir unavailable, relative paths resolve against process cwd"
        );
    }

    // ---- agentxx_filesystem_list ----
    {
        ToolPromptText p      = readToolPrompt(kNameList);
        std::string schema = neograph::json{
            {"type", "object"},
            {"properties",
             {
                 {"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
                 {"recursive",
                  {
                      {"type", "boolean"},
                      {"description",
                       argDesc(p,
                               "recursive",
                               "Default `false`. If `true`, list subdirectories recursively.")},
                  }},
                 {"limit",
                  {
                      {"type", "integer"},
                      {"description",
                       argDesc(p,
                               "limit",
                               "Default `100`. Maximum number of entries to return. Set `limit <= 0` for unlimited.")},
                  }},
                 {"timeout",
                  {
                      {"type", "number"},
                      {"description", argDesc(p, "timeout", kTimeoutDesc)},
                  }},
             }},
            {"required", neograph::json::array({"path"})},
        }
                              .dump();
        registerTool(kNameList, kDepictList, schema, &wrapExecute<agentxx::filesystem_plugin::fileListExecute>);
    }

    // ---- agentxx_filesystem_read ----
    {
        ToolPromptText p      = readToolPrompt(kNameRead);
        std::string schema = neograph::json{
            {"type", "object"},
            {"properties",
             {
                 {"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
                 {"line_offset",
                  {
                      {"type", "integer"},
                      {"description",
                       argDesc(p,
                               "line_offset",
                               "Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count.")},
                  }},
                 {"line_limit",
                  {
                      {"type", "integer"},
                      {"description",
                       argDesc(p,
                               "line_limit",
                               "Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error.")},
                  }},
             }},
            {"required", neograph::json::array({"path"})},
        }
                              .dump();
        registerTool(kNameRead, kDepictRead, schema, &wrapExecute<agentxx::filesystem_plugin::fileReadExecute>);
    }

    // ---- agentxx_filesystem_write ----
    {
        ToolPromptText p      = readToolPrompt(kNameWrite);
        std::string schema = neograph::json{
            {"type", "object"},
            {"properties",
             {
                 {"path", {{"type", "string"}, {"description", argDesc(p, "path", kPathDesc)}}},
                 {"content",
                  {
                      {"type", "string"},
                      {"description", argDesc(p, "content", "Content to write into the file.")},
                  }},
                 {"overwrite",
                  {
                      {"type", "boolean"},
                      {"description",
                       argDesc(p,
                               "overwrite",
                               R"(Default `false`. Controls write behavior:
`true`: Create the file if it doesn't exist; overwrite if it does.
`false`: Create a new file only; returns an error if the file already exists.)")},
                  }},
             }},
            {"required", neograph::json::array({"path", "content"})},
        }
                              .dump();
        registerTool(kNameWrite, kDepictWrite, schema, &wrapExecute<agentxx::filesystem_plugin::fileWriteExecute>);
    }

    // ---- agentxx_filesystem_edit ----
    {
        ToolPromptText p      = readToolPrompt(kNameEdit);
        std::string schema = neograph::json{
            {"type", "object"},
            {"properties",
             {
                 {"path",
                  {
                      {"type", "string"},
                      {"description",
                       argDesc(p,
                               "path",
                               "Path to the text file. Relative paths are resolved against the current working directory; `~` expands to the home directory.")},
                  }},
                 {"old_str",
                  {
                      {"type", "string"},
                      {"description",
                       argDesc(p,
                               "old_str",
                               "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation).")},
                  }},
                 {"new_str",
                  {
                      {"type", "string"},
                      {"description", argDesc(p, "new_str", "The replacement string.")},
                  }},
                 {"multi_replace",
                  {
                      {"type", "boolean"},
                      {"description",
                       argDesc(p,
                               "multi_replace",
                               "Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence.")},
                  }},
             }},
            {"required", neograph::json::array({"path", "old_str", "new_str"})},
        }
                              .dump();
        registerTool(kNameEdit, kDepictEdit, schema, &wrapExecute<agentxx::filesystem_plugin::fileEditExecute>);
    }

    // ---- agentxx_filesystem_glob ----
    {
        ToolPromptText p      = readToolPrompt(kNameGlob);
        std::string schema = [&p]() {
            auto patternsDesc = fmt::format(
                R"(Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.

{}
Examples: `/upload/**/*.txt`, `/src/*[0-9].cpp`, `/usr/include/nc*.h`, `/output/file[0-9].*`.)",
                kGlobPatternHelp
            );
            return neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"file_patterns",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "string"}}},
                          {"description", argDesc(p, "file_patterns", patternsDesc)},
                      }},
                     {"type",
                      {
                          {"description",
                           argDesc(p,
                                   "type",
                                   R"(Filter results by file type. Accepts a string or array of strings.
Valid values: `file`, `dir`, `symlink`, `other`, `any`.
Default: `any` (no filter).
Example: `"file"` returns only regular files; `["file","symlink"]` returns files and symlinks.)")},
                      }},
                     {"exclude_patterns",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "string"}}},
                          {"description",
                           argDesc(p,
                                   "exclude_patterns",
                                   R"(Glob patterns to exclude from results. Matched paths are removed.
Example: `["**/node_modules/**", "**/.git/**", "**/build/**"]`.)")},
                      }},
                     {"max_depth",
                      {
                          {"type", "integer"},
                          {"description",
                           argDesc(p,
                                   "max_depth",
                                   "Maximum directory depth relative to the pattern's base directory.\nDefault `-1` (no limit). Example: `max_depth=1` matches only direct children.\nSimilar to `find -maxdepth`.")},
                      }},
                     {"sort",
                      {
                          {"type", "boolean"},
                          {"description",
                           argDesc(p,
                                   "sort",
                                   "Default `false`. If `true`, sort results alphabetically.\nResults are always deduplicated regardless of this setting.")},
                      }},
                     {"timeout",
                      {
                          {"type", "number"},
                          {"description", argDesc(p, "timeout", kTimeoutDesc)},
                      }},
                 }},
                {"required", neograph::json::array({"file_patterns"})},
            }
                              .dump();
        }();
        registerTool(kNameGlob, kDepictGlob, schema, &wrapExecute<agentxx::filesystem_plugin::fileGlobExecute>);
    }

    // ---- agentxx_filesystem_grep ----
    {
        ToolPromptText p      = readToolPrompt(kNameGrep);
        std::string schema = [&p]() {
            auto filePatternsDesc = fmt::format(
                R"(Path with glob patterns to select which files to search. Relative paths are resolved against the current working directory; `~` expands to the home directory.

{}
Examples: `/src/**/*.cpp`, `/project/*.h`, `/logs/**/*.log`.)",
                kGlobPatternHelp
            );
            return neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"text_patterns_is_regex",
                      {
                          {"type", "boolean"},
                          {"description",
                           argDesc(p,
                                   "text_patterns_is_regex",
                                   "Determines how `text_patterns` are interpreted.\n`true`: Patterns are regular expressions.\n`false`: Patterns are literal text strings.")},
                      }},
                     {"text_patterns",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "string"}}},
                          {"description",
                           argDesc(p,
                                   "text_patterns",
                                   "One or more search patterns (text or regex, depending on `text_patterns_is_regex`).\nA match is found if ANY pattern matches.")},
                      }},
                     {"file_patterns",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "string"}}},
                          {"description", argDesc(p, "file_patterns", filePatternsDesc)},
                      }},
                     {"output_mode",
                      {
                          {"type", "string"},
                          {"enum", neograph::json::array({"content", "files_with_matches"})},
                          {"default", "files_with_matches"},
                          {"description",
                           argDesc(p,
                                   "output_mode",
                                   R"(Default: `files_with_matches`.
`files_with_matches`: Return file paths with match counts (format: `file:count`).
`content`: Return matching lines grouped by file to reduce path repetition. Each file
starts with a header line `{filepath}:`, followed by that file's lines (`{line}:{content}`). Example:
/path/to/file1:
12:int foo() {
40:int bar() {
/path/to/file2:
7:return 0;)")},
                      }},
                     {"case_sensitive",
                      {
                          {"type", "boolean"},
                          {"description",
                           argDesc(p,
                                   "case_sensitive",
                                   "Default `true`. If `false`, matching is case-insensitive (like `grep -i`).")},
                      }},
                     {"max_count_per_file",
                      {
                          {"type", "integer"},
                          {"description",
                           argDesc(p,
                                   "max_count_per_file",
                                   "Default `0` (no limit). Maximum matches to report per file.\nSimilar to `grep -m N`. Example: `max_count_per_file=3` stops after 3 matches per file.")},
                      }},
                     {"context_lines",
                      {
                          {"type", "integer"},
                          {"description",
                           argDesc(p,
                                   "context_lines",
                                   "Default `0`. Number of context lines before and after each match.\nOnly applies to `content` output mode. Similar to `grep -C N`.\nContext lines use `-` separator; match lines use `:` separator.")},
                      }},
                     {"timeout",
                      {
                          {"type", "number"},
                          {"description", argDesc(p, "timeout", kTimeoutDesc)},
                      }},
                 }},
                {"required", neograph::json::array({"text_patterns", "file_patterns"})},
            }
                              .dump();
        }();
        registerTool(kNameGrep, kDepictGrep, schema, &wrapExecute<agentxx::filesystem_plugin::fileGrepExecute>);
    }

    return 0;
    XX_PGUARD_END_RET(-1)
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄 (否则宿主卸载流程被打断)
    XX_PGUARD_BEGIN
    (void)plugin_ctx;
    g_host = nullptr;
    g_if   = {};
    XX_PGUARD_END_VOID()
}
