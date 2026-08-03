#pragma once

#include "agentxx/util/util.h"
#include "fmt/format.h"
#include <cassert>
#include <map>
#include <neograph/json.h>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

class ToolPrompt {
public:

    /// Tool description. Non-const to allow modification during training.
    std::string depict;
    /// Tool parameter descriptions. Non-const to allow modification during training.
    std::map<std::string, std::string, std::less<>> args;

    const std::string& getArg(std::string_view name) const;
};

/// Prompt registry.
/// - Aggregates system prompts and tool prompts for easy customization,
///   self-updating, and training serialization.
class AgentPrompt {
public:

    std::string systemPrompt = R"_(
You are a helpful, knowledgeable AI coding assistant.

## Core Behavior
- Understand the user's intent before acting; ask for clarification if ambiguous
- Use available tools to gather information, inspect code, and perform actions
- Prefer reading existing code before making changes to respect conventions
- Provide accurate, well-structured answers with concrete examples

## Response Style
- Be concise and direct; avoid unnecessary preamble
- Use clear formatting (headings, lists, code blocks) when it improves readability
- Prefer concrete solutions over vague suggestions
- When modifying code, show only the relevant changed sections unless full context is needed
)_";

    std::string systemPlanningPrompt = R"_(
## Planning

You have access to the `planning_write` tool to manage and plan complex objectives.
Use this tool for multi-step tasks to ensure you track each necessary step.
It helps break down large objectives into smaller, manageable steps.

- Mark todos as completed as soon as you finish a step. Do NOT batch completions.
- For simple objectives (few steps), skip planning and execute directly.
- Planning costs tokens — use it only for complex, many-step problems.

### Important Notes

- Never call `planning_write` multiple times in parallel.
- Revise the plan as new information emerges. Remove irrelevant tasks, add newly discovered ones.

### Finishing a Task

When all work is done, write your final answer in the message AFTER your last `planning_write` call — not in the same turn.
Start the final message with the substantive content the user asked for (data, computation, summary, or analysis).
The user wants the result, not confirmation that the work is done.
)_";

    std::string systemSkillPrompt = R"_(
## How to Use Skills (Progressive Disclosure)

Skills follow a progressive disclosure pattern — you see their name and description,
but only read full instructions when needed:

1. **Recognize when a skill applies**: Check if the user's task matches a skill's description.
2. **Read the skill's full instructions**: Use `filesystem_read_text_file` on the skill path.
   Pass `line_limit=1000` since the default of 100 lines is too small for most skill files.
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
2. Read the full skill file via `filesystem_read_text_file`
3. Follow the skill's research workflow (search → organize → synthesize)
4. Use any helper scripts with absolute paths

When in doubt, check if a skill exists for the task.
)_";

    /// toolcall
    std::map<std::string, ToolPrompt, std::less<>> toolPrompt{
      {
          "execute_linux_command",
          ToolPrompt{
              .depict = "Execute a Linux shell/bash command and return its output.",
              .args =
                  {
                      {
                          "command",
                          fmt::format(
                              R"(The shell command to execute.
Current system: {}{}. Use standard Linux shell/bash syntax.)",
                              agentxx::util::getSystemName(),
                              agentxx::util::isRunningInWSL() ? " (WSL)" : ""),
                      },
                      {
                          "all_output",
                          R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)",
                      },
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "execute_windows_command",
          ToolPrompt{
              .depict = "Execute a Windows command via cmd.exe and return its output.",
              .args =
                  {
                      {
                          "command_process",
                          fmt::format(
                              R"({}

The command is passed directly to `cmd.exe` — do NOT prepend `cmd.exe /c` yourself.

## Examples:
- `powershell.exe -Command "Get-Process"`: Run PowerShell command
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `Control.exe`: Open Control Panel
- `regedit.exe`: Open Registry Editor
- `calc.exe`: Open Calculator
- `notepad.exe C:\file.txt`: Open file in Notepad)",
                              agentxx::util::isRunningInWSL()
                                  ? R"(Command to execute in the Windows terminal (via WSL interop).
Current system is WSL. This tool routes the command through cmd.exe into Windows.
If the user provides a Windows path (e.g. `C:\...` or `D:\...`), convert it to a WSL path (`/mnt/c/...` or `/mnt/d/...`) for file operations, but use the original Windows path when passing to Windows executables.)"
                                  : "The Windows command to execute."),
                      },
                      {
                          "command_popen",
                          fmt::format(
                              R"({}

## Examples:
- `cmd.exe /c "echo hello"`: Run a command in Windows CMD
- `cmd.exe /c "mkdir C:\test"`: Create directory via CMD
- `powershell.exe -Command "Get-ChildItem"`: Run PowerShell command
- `explorer.exe C:\Users`: Open File Explorer at path
- `Taskmgr.exe`: Open Task Manager
- `Control.exe`: Open Control Panel
- `regedit.exe`: Open Registry Editor
- `calc.exe`: Open Calculator
- `notepad.exe C:\file.txt`: Open file in Notepad)",
                              agentxx::util::isRunningInWSL()
                                  ? R"(Command to execute in the Windows terminal (via WSL interop).
Windows commands must be invoked through `cmd.exe`. Format: `cmd.exe /c "{win_command}"`.
- The outer command runs in the Linux/WSL shell, but `{win_command}` executes inside the Windows terminal.
- All Windows commands must go through cmd.exe: `cmd.exe /c "{win_command}"`.
If the user provides a Windows path (e.g. `C:\...` or `D:\...`), convert it to a WSL path (`/mnt/c/...` or `/mnt/d/...`) for file operations, but use the original Windows path when passing to Windows executables.)"
                                  : "The Windows command to execute."),
                      },
                      {
                          "all_output",
                          R"(Default `true`.
`true`: Always return stdout and stderr output.
`false`: Only return output when the command fails.)",
                      },
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "execute_python_command",
          ToolPrompt{
              .depict = "Execute Python code and return its output.",
              .args =
                  {
                      {"command", "Python code to execute."},
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "execute_javascript_command",
          ToolPrompt{
              .depict = "Execute JavaScript code (Node.js) and return its output.",
              .args =
                  {
                      {"command", "JavaScript code to execute."},
                      {
                          "timeout",
                          R"(Default `60` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "filesystem_list",
          ToolPrompt{
              .depict =
                  R"(List files and directories at a given path, including size (bytes), type, and last-modified time (nanosecond timestamp).
Can also be used to check whether a specific file or directory exists.)",
              .args =
                  {
                      {"path", "Absolute path to a file or directory."},
                      {"recursive", "Default `false`. If `true`, list subdirectories recursively."},
                      {
                        "limit",
                        R"(Default `100`. Maximum number of entries to return. Set `limit <= 0` for unlimited.)",
                      },
                      {
                          "timeout",
                          R"(Default `120` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "filesystem_read_text_file",
          ToolPrompt{
              .depict =
                  R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.
Supports offset/limit for reading portions of large files.)",
              .args =
                  {
                      {"path", "Absolute path to the text file."},
                      {"line_offset",
                       R"(Number of lines to skip from the beginning. Default `0` (no offset). Returns an error if offset exceeds the file's line count.)"},
                      {"line_limit",
                       R"(Maximum number of lines to read. Range: [1, ∞]. Default `null` (read all). Values exceeding the file's line count are allowed without error.)"},
                  },
          },
      },
      {
          "filesystem_read_binary_file",
          ToolPrompt{
              .depict =
                  R"(Read a binary file and return its contents as a base64-encoded string.
Supports byte offset/limit for reading portions of large files.)",
              .args =
                  {
                      {"path", "Absolute path to the file."},
                      {"byte_offset",
                       R"(Starting byte offset. Default `0` (from beginning). Returns an error if offset exceeds file size.)"},
                      {"byte_limit",
                       R"(Maximum number of bytes to read. Range: [1, ∞]. Default `null` (read all). Values exceeding file size are allowed without error.)"},
                  },
          },
      },
      {
          "filesystem_write_file",
          ToolPrompt{
              .depict = "Create a new file or overwrite an existing file with the given content.",
              .args =
                  {
                      {"path", "Absolute path to the target file."},
                      {"content", "Content to write into the file."},
                      {"overwrite", R"(Default `false`. Controls write behavior:
`true`: Create the file if it doesn't exist; overwrite if it does.
`false`: Create a new file only; returns an error if the file already exists.)"},
                      {"is_binary",
                       R"(Default `false`. Controls content encoding:
`true`: `content` must be a base64-encoded string; decoded and written as raw bytes.
`false`: `content` is treated as plain text and written directly.)"},
                  },
          },
      },
      {
          "filesystem_edit_text_file",
          ToolPrompt{
              .depict =
                  R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).
Use this for surgical edits without rewriting the entire file.)",
              .args =
                  {
                      {"path", "Absolute path to the text file."},
                      {"old_str", "The exact string to find and replace. Must be non-empty and match precisely (including whitespace and indentation)."},
                      {"new_str", "The replacement string."},
                      {"multi_replace",
                       R"(Default `false`. If `true`, replace ALL occurrences of `old_str`. If `false`, replace only the first occurrence.)"},
                  },
          },
      },
      {
          "filesystem_glob",
          ToolPrompt{
              .depict = "Find files and directories matching glob patterns.",
              .args =
                  {
                      {"file_patterns",
                       R"(Absolute path with glob patterns to match.

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
                      {"case_sensitive",
                       R"(Default `true`. If `false`, pattern matching is case-insensitive.)"},
                      {"max_depth",
                       R"(Maximum directory depth relative to the pattern's base directory.
Default `-1` (no limit). Example: `max_depth=1` matches only direct children.
Similar to `find -maxdepth`.)"},
                      {"sort",
                       R"(Default `false`. If `true`, sort results alphabetically.
Results are always deduplicated regardless of this setting.)"},
                      {
                          "timeout",
                          R"(Default `120` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "filesystem_grep",
          ToolPrompt{
              .depict =
                  R"(Search file contents using text or regular expressions. Supports glob-based file filtering.
Use this to locate code, find references, or search logs across a project.)",
              .args =
                  {
                      {"text_patterns_is_regex",
                       R"(Determines how `text_patterns` are interpreted.
`true`: Patterns are regular expressions.
`false`: Patterns are literal text strings.)"},
                      {"text_patterns",
                       R"(One or more search patterns (text or regex, depending on `text_patterns_is_regex`).
A match is found if ANY pattern matches.)"},
                      {"file_patterns",
                       R"(Absolute path with glob patterns to select which files to search.

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
`content`: Return matching lines with location (format: `file:line:content`).)"},
                      {"case_sensitive",
                       R"(Default `true`. If `false`, matching is case-insensitive (like `grep -i`).)"},
                      {"max_count_per_file",
                       R"(Default `0` (no limit). Maximum matches to report per file.
Similar to `grep -m N`. Example: `max_count_per_file=3` stops after 3 matches per file.)"},
                      {"context_lines",
                       R"(Default `0`. Number of context lines before and after each match.
Only applies to `content` output mode. Similar to `grep -C N`.
Context lines use `-` separator; match lines use `:` separator.)"},
                      {
                          "timeout",
                          R"(Default `120` seconds. Execution timeout in seconds. Set `0` for no limit.)",
                      },
                  },
          },
      },
      {
          "planning_write",
          ToolPrompt{
              .depict =
                  R"(Two-level task planning tool for complex multi-step work sessions.

=== Strategic Layer: `roadmap` (required) ===
A Mermaid stateDiagram-v2 capturing the OVERALL workflow — the big picture.
This is your roadmap: major phases, dependencies, error recovery paths, and the
start-to-finish flow. Update this diagram whenever the plan changes (new tasks,
completed phases, dead ends). After execution is completed, make an overall summary.

State diagram conventions:
- Use `[*]` for start/end pseudo-states
- Name state nodes like `phase_N_description` (e.g. `phase_1_search_codebase`)
- Status transitions: pending → in_progress → completed | failed
- Show branching: what happens on success vs failure
- Replace the entire diagram each call

=== Tactical Layer: `todos` (optional) ===
A short list of IMMEDIATE and NEXT-STEP tasks only. Do NOT list every state
from the diagram — only the tasks you are actively working on or about to start.
Each item records execution details, lessons learned, and issues encountered
to help with re-planning.

=== MEMO Layer: `notes` (optional) ===
Record any important information, tips, reminders, or identity/role-playing prompts.

Example for a "fix a bug" workflow:
- roadmap:
```mermaid
stateDiagram-v2
    [*] --> 1_reproduce_bug
    1_reproduce_bug --> 1_in_progress: start
    1_in_progress --> 1_completed: reproduced
    1_in_progress --> 1_failed: cannot reproduce
    1_completed --> 2_locate_root_cause
    2_locate_root_cause --> 2_in_progress: analyze
    2_in_progress --> 2_completed: found cause
    2_completed --> 3_implement_fix
    3_implement_fix --> 3_in_progress: coding
    3_in_progress --> 3_completed: fix works
    3_completed --> [*]
```
- todos (only current + next):
[
  {"state":"in_progress", "content":"Reproduce the crash with provided stack trace",
   "summary":"Found that it crashes on null pointer at line 342"},
  {"state":"pending", "content":"Locate root cause by tracing the null pointer source"}
]
- notes:
    - Follow user code style guide.
    - Add unit tests after change.
)",
              .args =
                  {
                      {"roadmap",
                       R"(STRATEGIC LAYER: Mermaid stateDiagram-v2 of the overall workflow.
Include ALL phases even if not yet started. Each phase gets state nodes for its
statuses (pending/in_progress/completed/failed) with transitions showing
dependencies and error recovery paths. Use `[*]` for start/end.
Replace the entire diagram each call.)"},
                      {"todos", R"(TACTICAL LAYER: Near-term task items.
Focus on what you are actively doing NOW and what comes NEXT.
Do NOT list all phases from the diagram — only immediate execution items.
Each item records what was tried, what worked, and what to watch out for.

Item struct:
{
    "state": "pending",   // enum: pending, in_progress, completed, failed
    "content": "",        // task description
    "summary": ""         // execution notes: methods tried, issues encountered,
                          // optimization suggestions for re-planning
}
)"},
                      {"notes",
                       R"(MEMO LAYER: Any additional notes.
Use this to record important information, tips, reminders, or identity/role-playing prompts.
)"},
                  },
          },
      },
      {
          "rag_search",
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
          "web_search",
          ToolPrompt{
              .depict =
                  R"(Perform a web search. Returns a markdown-formatted list of results.
Use `web_fetch_url_markdown` afterwards to retrieve full page content from a result.)",
              .args =
                  {
                      {"query", "The search query string."},
                  },
          },
      },
      {
          "web_fetch_url",
          ToolPrompt{
              .depict = "Perform an HTTP GET request and return the raw response body.",
              .args =
                  {
                      {"url", "Absolute HTTP/HTTPS URL to fetch."},
                      {"timeout", "Default `30` seconds. Request timeout in seconds."},
                  },
          },
      },
      {
          "web_fetch_url_markdown",
          ToolPrompt{
              .depict =
                  R"(Perform an HTTP GET request and return the page content converted to Markdown.
Commonly used after `web_search` to read a specific page.)",
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
                  },
          },
      },
      {
          "share_store",
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
`delete`: Remove stored text by its unique ID.
)"},
                      {"text", "The text content to store. Required for `insert` and `set`."},
                      {"line_offset",
                       R"(Optional for `insert`/`set`. Line offset for partial operations. Default `0` (no offset). Returns an error if offset exceeds the stored text's line count.)"},
                      {"line_limit",
                       R"(Optional for `insert`/`set`. Maximum lines to read. Range: [1, ∞]. Default `null` (no limit). Values exceeding line count are allowed without error.)"},
                      {"id", "The unique ID of the stored text. Required for `get`, `set`, and `delete`."},
                  },
          },
      },
      {
          "string_html_to_markdown",
          ToolPrompt{
              .depict = "Convert HTML content to Markdown format.",
              .args =
                  {
                      {"content", "The HTML string to convert."},
                  },
          },
      },
      {
          "string_regexp",
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
          "get_current_datetime",
          ToolPrompt{
              .depict = "Get the current date, time, and Unix timestamp.",
              .args = {},
          },
      },
      {
          "get_system_core_info",
          ToolPrompt{
              .depict =
                  R"(Get system resource usage: CPU utilization, memory usage, GPU utilization, and GPU memory usage.)",
              .args = {},
          },
      },
      {
          "codegraph_search",
          ToolPrompt{
              .depict =
                  R"(Search for code symbols (functions, classes, variables, etc.) by name using the codegraph index.
Returns matched symbols with their file locations and signatures.
Use this to quickly locate definitions across a large codebase.)",
              .args =
                  {
                      {"query",
                       "Symbol name to search for. Supports partial matching."},
                      {"limit", "Maximum number of results to return. Default: 20."},
                  },
          },
      },
      {
          "codegraph_context",
          ToolPrompt{
              .depict =
                  R"(Get rich context for a code symbol: its definition, callers, callees, and methods (for classes).
Useful for understanding how a function or class is used throughout the codebase.)",
              .args =
                  {
                      {"symbol", "Fully qualified symbol name (e.g. `MyClass::myMethod`)."},
                      {"limit", "Maximum results per category. Default: 10."},
                      {"max_depth",
                       "Maximum call-graph traversal depth. Default: 3."},
                  },
          },
      },
      {
          "codegraph_callers",
          ToolPrompt{
              .depict =
                  R"(Find all functions that call a given symbol (reverse call-graph traversal).
Use this to understand what depends on a function before modifying it.)",
              .args =
                  {
                      {"symbol", "Symbol name to find callers for."},
                      {"max_depth", "Maximum traversal depth. Default: 3."},
                  },
          },
      },
      {
          "codegraph_callees",
          ToolPrompt{
              .depict =
                  R"(Find all functions that a given symbol calls (forward call-graph traversal).
Use this to understand a function's dependencies.)",
              .args =
                  {
                      {"symbol", "Symbol name to find callees for."},
                      {"max_depth", "Maximum traversal depth. Default: 3."},
                  },
          },
      },
      {
          "codegraph_impact",
          ToolPrompt{
              .depict =
                  R"(Analyze the impact of modifying a symbol. Finds all downstream symbols that may be
affected (callers, references). Use this before refactoring to assess blast radius.)",
              .args =
                  {
                      {"symbol", "Symbol name to analyze impact for."},
                      {"max_depth", "Maximum traversal depth. Default: 5."},
                  },
          },
      },
      {
          "codegraph_status",
          ToolPrompt{
              .depict =
                  R"(Get codegraph index statistics: total nodes, edges, indexed files, and circular dependency count.)",
              .args = {},
          },
      },
      {
          "codegraph_index",
          ToolPrompt{
              .depict =
                  R"(Index a directory for code analysis. Parses source files and builds the symbol database
used by search, context, callers, callees, and impact queries.)",
              .args =
                  {
                      {"path", "Absolute path to the directory to index."},
                      {"incremental",
                       "Default `true`. If `true`, only re-index changed files. If `false`, full re-index."},
                  },
          },
      },
      {
          "codegraph_path",
          ToolPrompt{
              .depict =
                  R"(Find the call-chain path between two symbols in the call graph.
Use this to trace how execution flows from one function to another.)",
              .args =
                  {
                      {"from", "Starting symbol name."},
                      {"to", "Target symbol name."},
                      {"max_depth", "Maximum search depth. Default: 10."},
                  },
          },
      },
      {
          "ui_control_keyboard_mouse",
          ToolPrompt{
              .depict =
                  R"(Control mouse and keyboard on Windows. Accepts a list of UI commands and executes them sequentially.

## Actions

### Mouse
- `mouse_move`: Move cursor. Params: `x`, `y`
- `mouse_click`: Click. Params: `button` ("left"/"right"/"middle", default "left"), `x`, `y` (optional, move then click)
- `mouse_double_click`: Double-click. Params: same as mouse_click
- `mouse_scroll`: Scroll wheel. Params: `delta` (positive=up, negative=down, ±120 per notch), `x`, `y` (optional)
- `mouse_drag`: Drag. Params: `x1`, `y1`, `x2`, `y2`, `button` (default "left"), `duration_ms` (default 200)

### Keyboard
- `key_press`: Press and release a key. Params: `key`
- `key_down`: Hold a key down. Params: `key`
- `key_up`: Release a held key. Params: `key`
- `key_combo`: Press a key combination (e.g. Ctrl+C). Params: `keys` (array of key names)
- `key_type`: Type a text string. Params: `text`

### Utility
- `wait`: Pause execution. Params: `ms` (milliseconds, max 30000)
- `get_cursor_pos`: Get current cursor position. No params.
- `get_screen_size`: Get screen resolution. No params.

### Key Names
- Characters: "a"-"z", "0"-"9"
- Special: "enter", "tab", "escape", "backspace", "delete", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left", "right", "space"
- Modifiers: "shift", "ctrl", "alt", "win"
- Function keys: "f1"-"f12"
- Lock keys: "capslock", "numlock", "scrolllock"
- Other: "printscreen", "pause", "apps"

### Examples
```json
{"action": "mouse_click", "button": "left", "x": 100, "y": 200}
{"action": "key_combo", "keys": ["ctrl", "c"]}
{"action": "key_type", "text": "Hello World"}
{"action": "mouse_drag", "x1": 100, "y1": 100, "x2": 300, "y2": 300}
```)",
              .args =
                  {
                      {"commands",
                       "Ordered list of UI commands to execute sequentially."},
                      {"interval_ms",
                       "Delay between commands in milliseconds. Default: 50. Set `0` for no delay."},
                  },
          },
      },
  };

    // ----- Training serialization helpers -----
    // Serialize the entire AgentPrompt (including toolPrompt) to JSON for training save/load.

    neograph::json toJson() const;

    /// Overwrite the current prompt entirely from JSON (missing fields remain unchanged).
    void fromJson(const neograph::json& j);

    /// Merge via patch: only overwrite fields present in the JSON; absent fields stay as-is.
    /// - For an existing tool in toolPrompt, only the depict/args sub-fields present in JSON are
    /// overwritten.
    /// - For a tool not yet in toolPrompt, a new entry is inserted.
    void mergeFromJson(const neograph::json& j);

    /// Compute a hash of the entire prompt, used for training population deduplication.
    size_t promptHash() const;
};

} // namespace agent
} // namespace agentxx
