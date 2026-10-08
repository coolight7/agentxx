#include "agentxx/agent/prompt.h"

#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace agentxx {
namespace agent {

namespace {

/// 提示词文本里的会话级占位符替换: 把 text 中所有 token 换成 value
/// - 顺序扫描, 插入的 value 不再参与本轮替换 (value 里含 token 也不会反复替换)
void replaceAllInPlace(std::string& text, std::string_view token, std::string_view value) {
    if (token.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
}

/// 取不到的取值统一写成 unknown: 提示词里不留 token, 也不留空目录名
std::string_view orUnknown(const std::string& value) {
    return value.empty() ? std::string_view{"unknown"} : std::string_view{value};
}

} // namespace

AgentPrompt::AgentPrompt() :
    systemPrompt(R"_(
You are a helpful, knowledgeable AI coding assistant.
Your (LLM/Agent) name is Agentxx.

## Agent Env
- You are running inside the `agentxx` program on ${platform}.
- When agentxx's request to the LLM-Api fails, agentxx will append a `[Exception aborted]` message to the context. This may be caused by network fluctuations or other issues, and you can ignore these messages.

## Working Directory
- Working directory: `${work_dir}` — relative paths passed to tools are resolved against it, and `~` expands to the home directory.
- If you need to create temporary files (scratch scripts, downloads, logs, build output you do not want in the project), put them in the Temp directory `${temp_dir}`, that is the current system temp directory + `agentxx/${session_id}/`, instead of the working directory:
    - Missing parent directories are created automatically when you write a file; reuse the same Temp directory for the whole session.
    - Delete the files you no longer need, so scratch files do not pile up.
    - Temporary files are not part of the deliverable: keep the names and the changes of the real work in the working directory.

## Core Behavior
- This is not a test. You will serve the user as required by the system
- Understand the user's intent before acting; ask for clarification only when truly ambiguous
- If the user only wants to discuss an approach, do not start writing code right away; derive a solution plan from their ideas and requirements, and implement only after the user confirms it
- Use available tools to gather information, inspect code, and perform actions; verify results rather than assume
- Read and understand existing code before modifying it, and follow the project's conventions
- After changing code, verify it works when possible (build / run tests) before concluding
- Avoid irreversible destructive operations (such as unconfirmed `git reset --hard`, `git clean -fd`, or recursive deletion of important project directories) without user confirmation
- For large operations or changes, make a plan first and update it after each completed step. After tests pass, review the modified code for issues, then give the final overall summary
- To inspect characters that can't be displayed properly in UTF-8 (e.g. binary data or text that shows up as garbled characters), save the content to a file and view it as hexadecimal
- Provide accurate, well-structured answers with concrete examples
- **Writing code comments**:
    - When writing code, clear comments should be added in places that need attention and for design descriptions. Do not arbitrarily remove comments from the original code.
    - Write comments, names and labels in plain words and short sentences that any developer can follow. Do not use jargon, newly invented terms, or metaphors and analogies. For example, do not use terms such as `在途, 触达, 水位, 赋能, 抓手, 沉淀, 组合拳, 弹药, 倒逼, 脱节, 旗标, 旁路, 门禁, 闭环, 颗粒度, 感知度, 方法论, 点线面, 体验度量, 信息屏障, 冒烟, 鲁棒性`. Say what the code actually does in ordinary words instead of borrowing such terms.
    - When a comment is written or translated into the language the reader expects, read the surrounding code first and use the wording that this language normally uses. Do not translate word by word: a literal translation reads like machine output and hides the real meaning.
    - Code comments should not add task-planning section labels such as `A1, C2, B1, M1-1`, or priority markers such as `P0, P1`.
    - When documenting functions, use clear labels and depict.

## Response Style
- Be concise and direct; avoid unnecessary preamble or filler
- Respond in the same language the user uses, in the wording that language normally reads (never a word-by-word translation)
- Use clear formatting (headings, lists, code blocks) when it improves readability
- Prefer concrete solutions over vague suggestions
- When modifying code, show only the relevant changed sections unless full context is needed
)_"),
    appendSystemPrompts{
        {
         "skill", R"_(
## How to Use Skills (Progressive Disclosure)

Skills follow a progressive disclosure pattern — you see their name and description,
but only read full instructions when needed:

1. **Recognize when a skill applies**: Check if the user's task matches a skill's description.
2. **Read the skill's full instructions**: Use `agentxx_filesystem_read` on the skill path.
   It reads the whole file by default; only set `line_offset`/`line_limit` if the file is very large.
3. **Follow the skill's instructions**: SKILL.md contains step-by-step workflows, best practices, and examples.
4. **Access supporting files**: Skills may include helper scripts, configs, or reference docs — use absolute paths.

### When to Use Skills
- User's request matches a skill's domain (e.g., "analyse X" → `data-analyse` skill)
- You need specialized knowledge or structured workflows
- A skill provides proven patterns for complex tasks

### Executing Skill Scripts
Skills may contain Python scripts or other executables. Always use absolute paths from the skill list.

### Example Workflow
User: "Can you analyse the latest developments in quantum computing?"
1. Check available skills → see "data-analyse" skill with its path
2. Read the full skill file via `agentxx_filesystem_read`
3. Follow the skill's research workflow (search → organize → synthesize)
4. Use any helper scripts with absolute paths

When in doubt, check if a skill exists for the task.
)_",
         },
    },
    kGitWorktreePrompt(R"(
## Git Worktree Mode
This session supports isolated git worktrees (`agentxx_git_worktree` tool).
When the task modifies code, create an isolated worktree FIRST via opt=create, then do all edits/builds/tests inside it — this keeps parallel sessions from interfering with each other.
Read-only tasks (analysis/questions) don't need a worktree.
)"),
    summarizePrompt(R"_(
The conversation above will be compacted to free context space.

Summarize the ENTIRE conversation into ONE self-contained summary that preserves everything needed to continue the current work.
Write it with these fixed sections, in this order, keeping each one short:

## Goal
The user's goals and core requirements (near-verbatim for the critical ones), and the current task state.

## Done
What is finished: key decisions and their rationale, files modified (path + what changed), important commands executed.

## In progress
What is being worked on right now, and the next planned step.

## Blocked
Open issues, unresolved problems, errors that are not fixed yet.

## Key facts
File paths, code locations, configs, error messages and their solutions that later steps must know.

## Next
The immediate next actions, in order.

Rules:
- If an earlier summary appears above, MERGE it into the new one without losing information.
- MAY discard: exploratory read/search process details (keep file names and conclusions), retry noise, verbose or superseded tool outputs, details of reasoning/thinking content.

{omitted_note}Output ONLY the summary text in the user's language, no meta commentary, under about {max_words} words.
)_"),
    toolPrompt{
      {
          "agentxx_filesystem_list",
          ToolPrompt{
              .depict =
                  R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.
Directory paths end with `/`, symlinks show their target. Types: `d` directory, `-` file, `l` symlink.
Can also be used to check whether a specific file or directory exists.
`path` may contain shell wildcards as in bash: `*` any characters, `?` one character, `[...]` a character class, `**` any directory depth (e.g. `src/*.cpp`, `src/**/*.h`). Matched entries themselves are then listed (like `ls -d`); with `recursive` = `true`, matched directories are expanded as well.
Wildcard matching is case-sensitive and does not match hidden entries (name starting with `.`).)",
              .args =
                  {
                      {"path",
                       R"(Path to a file or directory. Relative paths are resolved against the current working directory; `~` expands to the home directory.
Shell wildcards are supported (`*`, `?`, `[...]`, and `**` for any directory depth), e.g. `src/*.cpp`; matching entries are listed instead of expanding their contents.)"},
                      {"recursive",
                       "Default `false`. If `true`, list subdirectories recursively; when `path` contains wildcards, matched directories are expanded as well."},
                      {
                        "limit",
                        R"(Default `100`. Maximum number of entries to return (total lines in output). Set `limit <= 0` for unlimited.)",
                      },
                      {
                          "max_files",
                          R"(Default `1000`. Maximum number of matched entries expanded before listing; a larger expansion is cut to the first entries with a `[Note]` (raise it or narrow `path`). Set `0` for no limit.)",
                      },
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "agentxx_filesystem_read",
          ToolPrompt{
              .depict =
                  R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.
Supports offset/limit for reading portions of large files.)",
              .args =
                  {
                      {"path",
                       R"(Path to the text file. Relative paths are resolved against the current working directory; `~` expands to the home directory.)"},
                      {"line_offset",
                       R"(Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count.)"},
                      {"line_limit",
                       R"(Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error.)"},
                  },
          },
      },
      {
          "agentxx_filesystem_write",
          ToolPrompt{
              .depict = "Create a new file or overwrite an existing file with the given content.",
              .args =
                  {
                      {"path",
                       R"(Path to the target file. Relative paths are resolved against the current working directory; `~` expands to the home directory.)"},
                      {"content", "Content to write into the file."},
                      {"overwrite", R"(Controls write behavior:
`true`: Create the file if it doesn't exist; overwrite if it does.
`false`: Create a new file only; returns an error if the file already exists.)"},
                  },
          },
      },
      {
          "agentxx_filesystem_edit",
          ToolPrompt{
              .depict =
                  R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.
Note! This tool will replace all `\r\n` to `\n` when find `old_str` and replace.)",
              .args =
                  {
                      {"path",
                       R"(Path to the text file. Relative paths are resolved against the current working directory; `~` expands to the home directory.)"},
                      {"old_str", "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation)."},
                      {"new_str", "The replacement string."},
                      {"multi_replace",
                       R"(Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence.)"},
                  },
          },
      },
      {
          "agentxx_filesystem_glob",
          ToolPrompt{
              .depict = "Find files and directories matching glob patterns.",
              .args =
                  {
                      {"file_patterns",
                       R"(Path with glob patterns to match. Relative paths are resolved against the current working directory; `~` expands to the home directory.

| Wildcard | Matches | Example |
|----------|---------|---------|
| `*` | Any characters | `*.txt` matches all .txt files |
| `**` | Any directory recursively | `src/**/*.h` matches all .h files under src/ |
| `?` | Exactly one character | `file?.log` matches file1.log, fileA.log |
| `[ABC]` | One char from set | `[ABC]*.cpp` matches files starting with A, B, or C |
| `[A-Z]` | One char from range | `[A-Z]*` matches files starting with uppercase |
| `[!ABC]` | One char NOT in set | `[!ABC]*` matches files not starting with A, B, or C |

Examples: `/upload/**/*.txt`, `/src/*[0-9].cpp`, `/usr/include/nc*.h`, `/output/file[0-9].*`.)"},
                      {"type",
                       R"(Filter results by file type. Accepts a string or array of strings.
Valid values: `file`, `dir`, `symlink`, `other`, `any`.
Default: `any` (no filter).
Example: `"file"` returns only regular files; `["file","symlink"]` returns files and symlinks.)"},
                      {"exclude_patterns",
                       R"(Glob patterns to exclude from results. Matched paths are removed.
Example: `["**/node_modules/**", "**/.git/**", "**/build/**"]`.)"},
                      {"max_depth",
                       R"(Maximum directory depth relative to the pattern's base directory.
Default `-1` (no limit). Example: `max_depth=1` matches only direct children.
Similar to `find -maxdepth`.)"},
                      {"sort",
                       R"(Default `false`. If `true`, sort results alphabetically.
Results are always deduplicated regardless of this setting.)"},
                      {"max_files",
                       R"(Default `1000`. Maximum number of matched paths; when more paths match, the walk stops there and the paths found so far are returned with a leading `[Note]` (`0` = no limit).)"},
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "agentxx_filesystem_grep",
          ToolPrompt{
              .depict =
                  R"(Search file contents using literal text and/or regular expression patterns (when both are given, the result is the union). Supports glob-based file filtering.
Use this to locate code, find references, or search logs across a project.)",
              .args =
                  {
                      {"text_patterns",
                       R"(One or more literal text patterns (NOT regular expressions). A match is found if ANY pattern occurs.
Optional if `regex_patterns` is given; `text_patterns` and `regex_patterns` can be specified together, and the result is the union of both matches.)"},
                      {"regex_patterns",
                       R"(One or more regular expression patterns (like `grep -E`). A match is found if ANY pattern matches.
Optional if `text_patterns` is given; `text_patterns` and `regex_patterns` can be specified together, and the result is the union of both matches.
Use this for pattern-based matching (e.g. `line[0-9]+`, `throw|co_return`); use `text_patterns` for literal search.)"},
                      {"file_patterns",
                       R"(Path with glob patterns to select which files to search. Relative paths are resolved against the current working directory; `~` expands to the home directory.

| Wildcard | Matches | Example |
|----------|---------|---------|
| `*` | Any characters | `*.cpp` matches all .cpp files |
| `**` | Any directory recursively | `src/**/*.h` matches all .h files under src/ |
| `?` | Exactly one character | `file?.log` matches file1.log, fileA.log |
| `[ABC]` | One char from set | `[ABC]*.cpp` matches files starting with A, B, or C |
| `[A-Z]` | One char from range | `[A-Z]*` matches files starting with uppercase |
| `[!ABC]` | One char NOT in set | `[!ABC]*` matches files not starting with A, B, or C |

Examples: `/src/**/*.cpp`, `/project/*.h`, `/logs/**/*.log`.)"},
                      {"output_mode",
                       R"(Default: `files_with_matches`.
`files_with_matches`: Return file paths with match counts (format: `file:count`).
`content`: Return matching lines grouped by file to reduce path repetition. Each file
starts with a header line `{filepath}:`, followed by that file's lines (`{line}:{content}`). Example:
/path/to/file1:
12:int foo() {
40:int bar() {
/path/to/file2:
7:return 0;)"},
                      {"case_sensitive",
                       R"(Default `true`. If `false`, matching is case-insensitive (like `grep -i`).)"},
                      {"exclude_patterns",
                       R"(Glob patterns to exclude while walking. A matched entry is skipped; a matched directory has its whole subtree pruned, so excluding a big generated directory (e.g. `**/build/**`, `**/third_party/**`) also saves the walk.)"},
                      {"max_count_per_file",
                       R"(Default `0` (no limit). Maximum matches to report per file.
Similar to `grep -m N`. Example: `max_count_per_file=3` stops after 3 matches per file.)"},
                      {"max_files",
                       R"(Default `1000`. Maximum number of files to scan (after `file_patterns` expansion). When more files match, the walk stops there and only the files found so far are scanned, with a leading `[Note]` telling you how many were scanned: narrow `file_patterns`, add `exclude_patterns`, or raise this value to scan more. Set `0` for no limit.)"},
                      {"max_file_size_mb",
                       R"(Default `32`. Files larger than this many MB are skipped (a trailing `[Note]` reports how many were skipped). Set `0` for no limit.)"},
                      {"context_lines",
                       R"(Default `0`. Number of context lines before and after each match.
Only applies to `content` output mode. Similar to `grep -C N`.
Context lines use `-` separator; match lines use `:` separator.)"},
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "agentxx_git_worktree",
          ToolPrompt{
              .depict =
                  R"(Manage isolated git worktrees for this session.

## Workflow
- **At the start of a code-modifying task**: call with `opt=create` to make an isolated worktree and BIND this session to it. Afterwards all file operations, builds and tests run inside the worktree automatically (relative paths resolve there; writes into the main checkout are denied).
- Read-only tasks (analysis/questions) don't need a worktree.
- Commit your work regularly inside the worktree.
- **Before finishing**: call with `opt=status`, summarize pending changes, and remind the user to review / commit / merge them.
- Worktrees are KEPT after tasks; only delete via `opt=remove` when the user explicitly asks (it refuses when uncommitted work exists unless `force=true`).)",
              .args =
                  {
                      {"opt",
                       R"(Operation to perform:
`create`: Create an isolated worktree and bind THIS session to it.
`info`: Show the current binding and all worktrees of the repository.
`status`: Summarize pending changes (uncommitted files / unpushed commits) of the bound worktree.
`remove`: Delete a worktree (refuses when it has pending work unless `force`).)"},
                      {"name",
                       R"(Worktree name, used as directory name under `{repo}/.agentxx/agent/worktrees/` and in branch `agentxx/wt-{name}`.
Allowed chars: letters, digits, `.`, `_`, `-`. Required by `create` (auto-generated when empty) and `remove`; ignored by other ops.)"},
                      {"base_ref",
                       R"(`create` only. Branch or commit the new worktree is based on. Default: current HEAD.)"},
                      {"force",
                       R"(`remove` only. Default `false`. When true, delete even if the worktree has uncommitted/untracked changes or unpushed commits — data-loss risk; prefer committing first.)"},
                  },
          },
      },
      {
          "agentxx_rag_search",
          ToolPrompt{
              .depict =
                  R"(Search the knowledge base using semantic similarity.
Use this to find relevant documents before answering questions.
Returns the most relevant documents with content, source, and similarity score.)",
              .args =
                  {
                      {"query", "Search query to find relevant documents."},
                      {"top_k", "Number of results to return. Default: 3."},
                  },
          },
      },
      {
          "agentxx_web_search",
          ToolPrompt{
              .depict =
                  R"(Perform a web search. Returns a markdown-formatted list of results.
Use `agentxx_web_fetch_markdown` afterwards to retrieve full page content from a result.)",
              .args =
                  {
                      {"query", "The search query string."},
                      {"timeout", "Default `15` seconds. Request timeout in seconds."},
                      {"header",
                       R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})"},
                  },
          },
      },
      {
          "agentxx_web_fetch",
          ToolPrompt{
              .depict = "Perform an HTTP GET request and return the raw response body.",
              .args =
                  {
                      {"url", "Absolute HTTP/HTTPS URL to fetch."},
                      {"timeout", "Default `30` seconds. Request timeout in seconds."},
                      {"header",
                       R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})"},
                  },
          },
      },
      {
          "agentxx_web_fetch_markdown",
          ToolPrompt{
              .depict =
                  R"(Perform an HTTP GET request and return the page content converted to Markdown.
Commonly used after `agentxx_web_search` to read a specific page.)",
              .args =
                  {
                      {"url", R"(Absolute HTTP/HTTPS URL to fetch.

When resolving relative links found in the returned Markdown, combine them with this `url`:
- Page `http://example.com/help/`:
  - `model/delete/` (no leading /) → `http://example.com/help/model/delete/`
  - `./model/create/` (leading .) → `http://example.com/help/model/create/`
  - `../model/create/` (leading ..) → `http://example.com/model/create/`
  - `/model/view/` (leading /) → `http://example.com/model/view/`
- Page `http://example.com/help/what.html`:
  - `model/delete/` (no leading /) → strip filename, append → `http://example.com/help/model/delete/`
)"},
                      {"timeout", "Default `15` seconds. Request timeout in seconds."},
                      {"header",
                       R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})"},
                  },
          },
      },
      {
          "agentxx_share_store",
          ToolPrompt{
              .depict =
                  R"(Persistent text storage with unique IDs. Store text and retrieve it later by ID.
Useful for passing large content between tool calls without repeating it in messages.)",
              .args =
                  {
                      {"opt", R"(Operation to perform:
`get`: Retrieve stored text by its unique ID.
`insert`: Store new text; returns a unique ID.
`set`: Update existing text by its unique ID.
)"},
                      {"text", "The text content to store. Required for `insert` and `set`."},
                      {"line_offset",
                       R"(Optional for `insert`/`set`. Line offset for partial operations. Default `0` (no offset). Returns an error if offset exceeds the stored text's line count.)"},
                      {"line_limit",
                       R"(Optional for `insert`/`set`. Maximum lines to read. Range: [1, ∞]. Default `null` (no limit). Values exceeding line count are allowed without error.)"},
                      {"id",
                       "The unique ID of stored text. Required for `get` and `set`. It must be an ID returned by `insert`; a larger (never allocated) ID is rejected as an invalid argument."},
                  },
          },
      },
      {
          "agentxx_string_html_to_markdown",
          ToolPrompt{
              .depict = "Convert HTML content to Markdown format.",
              .args =
                  {
                      {"content", "The HTML string to convert."},
                  },
          },
      },
      {
          "agentxx_string_regexp",
          ToolPrompt{
              .depict =
                  R"(Search, replace, or remove text using regular expressions.
Operates on in-memory text content (not files).)",
              .args =
                  {
                      {"content", "The input text to operate on."},
                      {"exps", "Array of regex patterns. A match succeeds if ANY pattern matches."},
                      {"opt", R"(Operation mode:
`search`: Return all match results.
`replace`: Replace matches with `replace_str` and return the resulting text.
`remove`: Remove all matches and return the resulting text.
)"},
                      {"replace_str",
                       R"(Default: empty string. The replacement string used when `opt` is `replace`.)"},
                  },
          },
      },
      {
          "agentxx_get_current_datetime",
          ToolPrompt{
              .depict = "Get the current date, time, and Unix timestamp.",
              .args = {},
          },
      },
      {
          "agentxx_subagent",
          ToolPrompt{
              .depict =
                  R"(Switch a isolation messages context sub-agent to exec.
The sub-agent runs with an isolated message context: it cannot see the parent conversation history, and its final output is returned to the parent as the tool result.

## Notes
- The sub-agent runs synchronously: the parent agent pauses until the sub-agent finishes.
- Provide a clear, self-contained task description in `message`; the sub-agent has no access to the parent's conversation context.
- `subagent` must be one of the registered sub-agent names (see the enum).
- `system_prompt` is optional; when `subagent` is not set, it is used as the sub-agent's system prompt.)",
              .args =
                  {
                      {"subagent",
                       "Target sub-agent name. Choose from the registered sub-agent list (see enum values)."},
                      {"system_prompt",
                       "Optional. Custom system prompt for the sub-agent when `subagent` is not set."},
                      {"message", "Task content as a user message for the sub-agent."},
                      {"messages",
                       R"(Optional. Structured message list (array of {role, content, tool_calls, ...}) passed through verbatim as the sub-agent's initial context (may include a system message). Takes precedence over `message`. Use this for same-context delegation (e.g. context compression) that must preserve the exact message prefix.)"},
                      {"session_id",
                       R"(Optional. Session id the sub-agent should run on. Empty (default): the sub-agent runs on its own isolated subagent thread. Non-empty (same-context mode): the sub-agent runs on the given thread with the parent session's current model, so the shared context prefix + thread id + model let the provider reuse its KV/prefix cache.)"},
                      {"tools",
                       R"(Optional. Tool policy for the sub-agent, as an array of tool names:
- `[]` (empty array): no tools at all (pure text answer).
- `["*"]`: inherit ALL tools of the parent agent.
- `["name1", "name2", ...]`: only these tools.
Absent (default): the sub-agent's default full tool set.)"},
                      {"enable_summarization",
                       R"(Optional boolean. Whether the sub-agent runs with context summarization enabled. Default: inherit config (enabled). Pass `false` when delegating same-context work (e.g. context compression) so the sub-agent never re-compresses the passed-in context prefix (would break KV/prefix cache reuse).)"},
                  },
          },
      },
  } {}

const std::string& ToolPrompt::getArg(std::string_view name) const {
    const auto it = args.find(name);
    if (it == args.end()) {
        XX_LOGW("ToolPrompt::getArg: argument '{}' not found", name);
        static const std::string empty;
        return empty;
    }
    return it->second;
}

std::string AgentPrompt::renderVars(std::string_view text, const PromptSessionVars& vars) {
    std::string out{text};
    replaceAllInPlace(out, kVarWorkDir, orUnknown(vars.workDir));
    replaceAllInPlace(out, kVarTempDir, orUnknown(vars.tempDir));
    replaceAllInPlace(out, kVarSessionId, orUnknown(vars.sessionId));
    replaceAllInPlace(out, kVarPlatform, orUnknown(vars.platform));
    return out;
}

void AgentPrompt::setAppendSection(
    std::string_view key,
    std::string      text,
    int              order,
    std::string_view source
) {
    if (key.empty()) {
        return;
    }
    utilxx_base::insertOrAssignHeterogeneous(appendSystemPrompts, key, std::move(text));
    auto& meta  = utilxx_base::getOrCreateHeterogeneous(appendSystemPromptMeta, key);
    meta.order  = order;
    meta.source = std::string{source};
}

bool AgentPrompt::removeAppendSection(std::string_view key) {
    // 异构删除复用 utilxx_base::eraseHeterogeneous (libc++ 无 C++23 异构 erase)
    const bool removed = utilxx_base::eraseHeterogeneous(appendSystemPrompts, key);
    utilxx_base::eraseHeterogeneous(appendSystemPromptMeta, key);
    return removed;
}

std::vector<std::string> AgentPrompt::appendSectionKeys() const {
    std::vector<std::string> keys;
    keys.reserve(appendSystemPrompts.size());
    for (const auto& kv : appendSystemPrompts) {
        keys.push_back(kv.first);
    }
    return keys; // map 本身按键名升序
}

std::vector<AppendSectionView> AgentPrompt::orderedAppendSections() const {
    std::vector<AppendSectionView> sections;
    sections.reserve(appendSystemPrompts.size());
    for (const auto& kv : appendSystemPrompts) {
        if (kv.second.empty() || kv.first == "summarization") {
            continue;
        }
        AppendSectionView view{.key = kv.first, .text = kv.second};
        if (auto it = appendSystemPromptMeta.find(kv.first); it != appendSystemPromptMeta.end()) {
            view.order  = it->second.order;
            view.source = it->second.source;
        }
        sections.push_back(std::move(view));
    }
    // order 升序; 同 order 按键名字典序 (map 迭代序即键名序, 用稳定排序保持)
    std::stable_sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) {
        return a.order < b.order;
    });
    return sections;
}

utilxx_base::Json AgentPrompt::toJson() const {
    utilxx_base::Json j;
    j["systemPrompt"]      = systemPrompt;
    j["summarizePrompt"]   = summarizePrompt;
    j["gitWorktreePrompt"] = kGitWorktreePrompt;
    {
        utilxx_base::Json append = utilxx_base::Json::object();
        for (const auto& kv : appendSystemPrompts) {
            utilxx_base::Json section;
            section["text"]   = kv.second;
            section["order"]  = 0;
            section["source"] = "";
            if (auto it = appendSystemPromptMeta.find(kv.first);
                it != appendSystemPromptMeta.end()) {
                section["order"] = it->second.order;
                if (!it->second.source.empty()) {
                    section["source"] = it->second.source;
                }
            }
            append[kv.first] = std::move(section);
        }
        j["appendSystemPrompts"] = std::move(append);
    }
    {
        utilxx_base::Json tools = utilxx_base::Json::object();
        for (const auto& kv : toolPrompt) {
            utilxx_base::Json tp;
            tp["depict"]           = kv.second.depict;
            utilxx_base::Json args = utilxx_base::Json::object();
            for (const auto& a : kv.second.args) {
                args[a.first] = a.second;
            }
            tp["args"]      = args;
            tools[kv.first] = tp;
        }
        j["toolPrompt"] = tools;
    }
    return j;
}

void AgentPrompt::fromJson(const utilxx_base::Json& j) {
    mergeFromJson(j);
}

void AgentPrompt::mergeFromJson(const utilxx_base::Json& j) {
    if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
        systemPrompt = j["systemPrompt"].get<std::string>();
    }
    if (j.contains("summarizePrompt") && j["summarizePrompt"].is_string()) {
        summarizePrompt = j["summarizePrompt"].get<std::string>();
    }
    if (j.contains("gitWorktreePrompt") && j["gitWorktreePrompt"].is_string()) {
        kGitWorktreePrompt = j["gitWorktreePrompt"].get<std::string>();
    } else if (j.contains("kGitWorktreePrompt") && j["kGitWorktreePrompt"].is_string()) {
        kGitWorktreePrompt = j["kGitWorktreePrompt"].get<std::string>();
    }
    if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
        auto append = j["appendSystemPrompts"];
        for (const auto& item : append.items()) {
            const auto& key = item.first;
            const auto& val = item.second;
            if (val.is_string()) {
                // 兼容旧形态: 直接字符串 = 段落正文 (order 0, 无来源)
                setAppendSection(key, val.get<std::string>());
            } else if (val.is_object()) {
                // 新形态: {text, order, source} (计划 PRM-2)
                std::string text;
                if (val.contains("text") && val["text"].is_string()) {
                    text = val["text"].get<std::string>();
                }
                int         order = 0;
                std::string source;
                if (val.contains("order") && val["order"].is_number()) {
                    order = val["order"].get<int>();
                }
                if (val.contains("source") && val["source"].is_string()) {
                    source = val["source"].get<std::string>();
                }
                setAppendSection(key, std::move(text), order, source);
            } else if (val.is_null()) {
                // 删除段落同时清除正文与元数据, 避免遗留孤儿元数据
                removeAppendSection(key);
            }
        }
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
        auto tools = j["toolPrompt"];
        for (const auto& item : tools.items()) {
            const auto& name = item.first;
            const auto& tp   = item.second;
            auto&       target
                = utilxx_base::getOrCreateHeterogeneous(toolPrompt, name); // 不存在则默认构造插入
            if (tp.contains("depict") && tp["depict"].is_string()) {
                target.depict = tp["depict"].get<std::string>();
            }
            if (tp.contains("args") && tp["args"].is_object()) {
                auto args = tp["args"];
                for (const auto& a : args.items()) {
                    if (a.second.is_string()) {
                        utilxx_base::insertOrAssignHeterogeneous(
                            target.args,
                            a.first,
                            a.second.get<std::string>()
                        );
                    }
                }
            }
        }
    }
}

size_t AgentPrompt::promptHash() const {
    size_t h  = std::hash<std::string>{}(systemPrompt);
    h        ^= std::hash<std::string>{}(summarizePrompt) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h        ^= std::hash<std::string>{}(kGitWorktreePrompt) + 0x9e3779b9 + (h << 6) + (h >> 2);
    for (const auto& kv : appendSystemPrompts) {
        h ^= std::hash<std::string>{}(kv.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(kv.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        // 排序号参与哈希 (顺序是提示词内容的一部分); 未登记元数据的段落按 order 0,
        // 保证"有/无元数据"不影响哈希 (JSON 往返后哈希一致)
        int order = 0;
        if (auto it = appendSystemPromptMeta.find(kv.first); it != appendSystemPromptMeta.end()) {
            order = it->second.order;
        }
        h ^= std::hash<int>{}(order) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    for (const auto& kv : toolPrompt) {
        h ^= std::hash<std::string>{}(kv.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(kv.second.depict) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (const auto& a : kv.second.args) {
            h ^= std::hash<std::string>{}(a.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>{}(a.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
    }
    return h;
}

h ^= std::hash<std::string>{}(a.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
}
}

return h;
}

} // namespace agent
} // namespace agentxx
