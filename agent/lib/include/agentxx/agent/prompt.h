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
  /// 工具描述。非 const 以便训练时修改
  std::string depict;
  /// 工具参数描述。非 const 以便训练时修改
  std::map<std::string, std::string, std::less<>> args;

  const std::string &getArg(std::string_view name) const {
    const auto it = args.find(name);
    assert(it != args.end());
    return it->second;
  }
};

/// 提示词汇总
/// - 将大部分 system prompt,tool prompt 汇集，方便自定义配置、自循环更新等功能
class AgentPrompt {
public:
  std::string systemPrompt = R"_(
You are a helpful, knowledgeable AI assistant.

## Core Behavior
- Understand the user's intent before responding
- Provide accurate, well-structured answers
- Use available tools when needed to gather information or perform actions
- Ask for clarification when the request is ambiguous

## Response Style
- Be concise and direct
- Use clear formatting (headings, lists, code blocks) when it improves readability
- Prefer concrete answers over vague generalities
)_";

  std::string systemPlanningPrompt = R"_(
## Planning

You have access to the `planning_write` tool to help you manage and plan complex objectives.
Use this tool for complex objectives to ensure that you are tracking each necessary step.
This tool is very helpful for planning complex objectives, and for breaking down these larger complex objectives into smaller steps.

It is critical that you mark todos as completed as soon as you are done with a step. Do not batch up multiple steps before marking them as completed.
For simple objectives that only require a few steps, it is better to just complete the objective directly and NOT use this tool.
Writing roadmap and todos takes time and tokens, use it when it is helpful for managing complex many-step problems! But not for simple few-step requests.

### Important To-Do List Usage Notes to Remember

- The `planning_write` tool should never be called multiple times in parallel.
- Don't be afraid to revise the To-Do list as you go. New information may reveal new tasks that need to be done, or old tasks that are irrelevant.

### Finishing a task

When you finish all work, write your final answer in the message AFTER your last `planning_write` call — not in the same turn as that call. Start the final message with the substantive content the user asked for — the data, computation, summary, or analysis. The user wants the result, not confirmation that the work is done.
)_";

  std::string systemSkillPrompt = R"_(
**How to Use Skills (Progressive Disclosure):**

Skills follow a **progressive disclosure** pattern - you see their name and description above, but only read full instructions when needed:

1. **Recognize when a skill applies**: Check if the user's task matches a skill's description
2. **Read the skill's full instructions**: Use toolcall `filesystem_read_text_file` on the path shown in the skill list above.
   Pass `line_limit=1000` since the default of 100 lines is too small for most skill files.
3. **Follow the skill's instructions**: SKILL.md contains step-by-step workflows, best practices, and examples
4. **Access supporting files**: Skills may include helper scripts, configs, or reference docs - use absolute paths

**When to Use Skills:**
- User's request matches a skill's domain (e.g., "analyse X" -> `data-analyse` skill)
- You need specialized knowledge or structured workflows
- A skill provides proven patterns for complex tasks

**Executing Skill Scripts:**
Skills may contain Python scripts or other executable files. Always use absolute paths from the skill list.

**Example Workflow:**

User: "Can you analyse the latest developments in quantum computing?"

1. Check available skills -> See "data-analyse" skill with its path
2. Read the full skill file by toolcall: `filesystem_read_text_file`
3. Follow the skill's research workflow (search -> organize -> synthesize)
4. Use any helper scripts with absolute paths

Remember: Skills make you more capable and consistent. When in doubt, check if a skill exists for the task!
)_";

  /// toolcall
  std::map<std::string, ToolPrompt, std::less<>> toolPrompt{
      {
          "execute_linux_command",
          ToolPrompt{
              .depict = "Run linux shell commands in the terminal.",
              .args =
                  {
                      {
                          "command",
                          fmt::format(
                              R"(Command to execute.
Current System is {}{}, please use linux shell/bash commands.)",
                              agentxx::util::getSystemName(),
                              agentxx::util::isRunningInWSL() ? "/(WSL)" : ""),
                      },
                      {
                          "all_output",
                          R"(Default `true`. 
`true`: Return all output.
`false`: Truncate Output. Only return the stdout and stderr output when the command faild.)",
                      },
                      {
                          "timeout",
                          R"(Default `60` seconds. 
执行超时时间 (秒), 指定 0 表示不限时)",
                      },
                  },
          },
      },
      {
          "execute_windows_command",
          ToolPrompt{
              .depict = "Run windows commands in the terminal.",
              .args =
                  {
                      {
                          "command_process",
                          fmt::format(
                              R"({}

`command` 会传递到 `cmd.exe`执行，因此不需要你添加 `cmd.exe /c`

## Example:

- `powershell.exe`: PowerShell 命令行
- `explorer.exe`: 文件资源管理器
    - `explorer.exe path`: open windows explorer.exe and jump `path`
- `Taskmgr.exe`: 任务管理器，查看和管理正在运行的程序、进程和服务
- `Control.exe`: 控制面板
- `regedit.exe`: 注册表编辑器
- `calc.exe`: 计算器
- `notepad.exe`: 纯文本编辑器)",
                              agentxx::util::isRunningInWSL()
                                  ? R"(Command to execute, run in Linux(WSL)/Shell.
Current system is WSL, but can use this tool to execute windows command through cmd.exe.
Arg `command` is actually runs inside the windows terminal.
If the user enters a Windows file path (starting with drive letters such as `C:\` or `D:\\`), please convert it to a Linux file path (starting with `/mnt/c/` or `/mnt/d/`).)"
                                  : "Windows command to execute"),
                      },
                      {
                          "command_popen",
                          fmt::format(
                              R"({}

Example:
- `cmd.exe`: use windows CMD to run commands. 命令行/终端.
    - `cmd.exe /c "win_cmd_str"`: run `win_cmd_str` in windows terminal
    - `cmd.exe /c "echo hello"`
    - `cmd.exe /c "mkdir test"`
- `powershell.exe`: PowerShell 命令行
- `explorer.exe`: 文件资源管理器
    - `explorer.exe path`: open windows explorer.exe and jump `path`
- `Taskmgr.exe`: 任务管理器，查看和管理正在运行的程序、进程和服务
- `Control.exe`: 控制面板
- `regedit.exe`: 注册表编辑器
- `calc.exe`: 计算器
- `notepad.exe`: 纯文本编辑器)",
                              agentxx::util::isRunningInWSL()
                                  ? R"(Command to execute, run in Linux(WSL)/Shell.
Windows Command must be executed through `cmd.exe`. Write arg command: `cmd.exe /c "win_cmd_str"`.
- Current system is WSL, but can use this tool to execute windows command through cmd.exe, there are some notes:
    - Arg `command`(e.g. `cmd.exe /c "{win_cmd_str}"`) is executed in the Linux/WSL shell. However, the `win_cmd_str` actually runs inside the windows terminal.
    - All win_cmd_str must be executed through cmd.exe (`cmd.exe /c "{win_cmd_str}"`).
If the user enters a Windows file path (starting with drive letters such as `C:\` or `D:\\`), please convert it to a Linux file path (starting with `/mnt/c/` or `/mnt/d/`).)"
                                  : "Windows command to execute"),
                      },
                      {
                          "all_output",
                          R"(Default `true`. 
`true`: Return all output.
`false`: Truncate Output. Only return the stdout and stderr output when the command faild.)",
                      },
                      {
                          "timeout",
                          R"(Default `60` seconds. 
执行超时时间 (秒), 指定 0 表示不限时)",
                      },
                  },
          },
      },
      {
          "execute_python_command",
          ToolPrompt{
              .depict = "Run python commands in the terminal.",
              .args =
                  {
                      {"command", "Command to execute"},
                      {
                          "timeout",
                          R"(Default `60` seconds. 
执行超时时间 (秒), 指定 0 表示不限时)",
                      },
                  },
          },
      },
      {
          "execute_javascript_command",
          ToolPrompt{
              .depict = "Run javascript commands in the terminal.",
              .args =
                  {
                      {"command", "Command to execute"},
                      {
                          "timeout",
                          R"(Default `60` seconds. 
执行超时时间 (秒), 指定 0 表示不限时)",
                      },
                  },
          },
      },
      {
          "filesystem_list",
          ToolPrompt{
              .depict =
                  R"(列出文件夹内的文件和文件夹信息，包含文件大小/Bytes, 类型, 最后写入时间(时间戳/nanoseconds)
指定文件路径可以得到文件信息.
也可用于检查文件/文件夹是否存在.)",
              .args =
                  {
                      {"path", "文件或文件夹的绝对路径"},
                      {"recursive", "默认 `false`. 是否递归子目录"},
                      {"limit",
                       "默认 `100`. 限制列出的文件、文件夹数量，指定`limit <= "
                       "0`时不限制数量"},
                  },
          },
      },
      {
          "filesystem_read_text_file",
          ToolPrompt{
              .depict =
                  "Read text file (e.g.: .txt,.md,.json,.log) contents with "
                  "line numbers, supports offset/limit for large files.",
              .args =
                  {
                      {"path", "文件绝对路径"},
                      {"line_offset",
                       R"(文本偏移行数,默认`0`表示不偏移.如果偏移超出文件最大行数,将返回错误提示)"},
                      {"line_limit",
                       R"(读取文本行数限制,取值范围 [1, ~],默认`null`表示不限制.允许指定的限制值超出文件最大行数不报错)"},
                  },
          },
      },
      {
          "filesystem_read_binary_file",
          ToolPrompt{
              .depict =
                  R"(Read binary file (e.g.: .txt,.md,.json,.log) contents with byte offset.
Supports offset/limit for large files.
Returns binary content as base64 string.)",
              .args =
                  {
                      {"path", "文件绝对路径"},
                      {"byte_offset",
                       R"(起始读取字节偏移量,默认`0`表示不偏移.如果偏移超出文件大小,将返回错误提示)"},
                      {"byte_limit",
                       R"(读取字节数限制,取值范围 [1, ~],默认`null`表示不限制.允许指定的限制值超出文件大小不报错)"},
                  },
          },
      },
      {
          "filesystem_write_file",
          ToolPrompt{
              .depict = "创建新文件，或覆盖文件.",
              .args =
                  {
                      {"path", "文件绝对路径"},
                      {"content", "写入文件的内容"},
                      {"overwrite", R"(默认`false`,是否覆盖文件.
如果为`true`,若文件不存在则创建并写入,若文件已经存在,则覆盖文件内容.
如果为`false`,创建新文件并写入,若文件已存在则返回失败.)"},
                      {"is_binary",
                       R"(默认`false`,是否按二进制模式写入文件.
如果为`true`,参数`content`应当为base64编码的二进制数据.
如果为`false`,参数`content`视为普通文本,按字符串直接写入文件.)"},
                  },
          },
      },
      {
          "filesystem_edit_text_file",
          ToolPrompt{
              .depict = "Perform exact string replacements in text files(e.g. "
                        "*.txt,*.md,*.cpp).",
              .args =
                  {
                      {"path", "文件绝对路径"},
                      {"old_str", "待替换的旧字符串,精准匹配,不能为空"},
                      {"new_str", "新字符串"},
                      {"multi_replace", "是否替换所有匹配`old_str`的字符串."
                                        "默认`false`只替换第一个匹配"},
                  },
          },
      },
      {
          "filesystem_glob",
          ToolPrompt{
              .depict = "Find files matching patterns.",
              .args =
                  {
                      {"file_patterns",
                       R"(Absolute dir or file path and glob patterns.

| Wildcard | Matches | Example
|--- |--- |--- |
| `*` | any characters | `*.txt` matches all files with the txt extension |
| `**` | any name dir recursively | `include/**/*.txt` matches all files with the txt extension in dir `include` and children dirs |
| `?` | any one character | `???` matches files with 3 characters long |
| `[]` | any character listed in the brackets | `[ABC]*` matches files starting with A,B or C | 
| `[-]` | any character in the range listed in brackets | `[A-Z]*` matches files starting with capital letters |
| `[!]` | any character not listed in the brackets | `[!ABC]*` matches files that do not start with A,B or C |

e.g., `/upload/**/*.txt`,`/docx/*[0-9].txt`,`/usr/include/nc*.h`,`/output/file[0-9].*`,`C:/down/read/??.txt`.
)"},
                  },
          },
      },
      {
          "filesystem_grep",
          ToolPrompt{
              .depict =
                  "Searches file contents using regular expressions or text.",
              .args =
                  {
                      {"text_patterns_is_regex",
                       R"(The type of `text_patterns`.
`true`:  `text_patterns` are regex syntaxs.
`false`: `text_patterns` are crude text strings.)"},
                      {"text_patterns",
                       "String or regex syntax to search text content. The "
                       "text match type depends on the "
                       "`text_patterns_is_regex` parameter."},
                      {"file_patterns",
                       R"(Absolute dir or file path and glob pattern.

| Wildcard | Matches | Example
|--- |--- |--- |
| `*` | any characters | `*.txt` matches all files with the txt extension |
| `**` | any name dir recursively | `include/**/*.txt` matches all files with the txt extension in dir `include` and children dirs |
| `?` | any one character | `???` matches files with 3 characters long |
| `[]` | any character listed in the brackets | `[ABC]*` matches files starting with A,B or C | 
| `[-]` | any character in the range listed in brackets | `[A-Z]*` matches files starting with capital letters |
| `[!]` | any character not listed in the brackets | `[!ABC]*` matches files that do not start with A,B or C |

e.g., `/upload/**/*.txt`,`/docx/*[0-9].txt`,`/usr/include/nc*.h`,`/output/file[0-9].*`,`C:/down/read/??.txt`.)"},
                      {"output_mode", R"(Default: `files_with_matches`. 
Output format:
'files_with_matches': Only file paths containing matches and count with `file:match_count` format
'content': Matching lines with file:line:content format)"},
                  },
          },
      },
      {
          "planning_write",
          ToolPrompt{
              .depict =
                  R"(Two-level task planning tool for complex multi-step work sessions.

=== Strategic Layer: `roadmap` (required) ===
A Mermaid stateDiagram-v2 that captures the OVERALL workflow — the big picture.
This is your roadmap: major phases, dependencies between tasks, error recovery
paths, and the start-to-finish flow. Update this diagram whenever the plan
changes (new tasks, completed phases, dead ends). After the execution is completed, 
an overall summary should be made.

State diagram conventions:
- Use `[*]` for start/end pseudo-states
- Name state nodes like `phase_N_description` (e.g. `phase_1_search_codebase`)
- Status transitions: pending → in_progress → completed | failed
- Show branching: what happens on success vs failure
- Replace the entire diagram each call

=== Tactical Layer: `todos` (optional) ===
A short list of IMMEDIATE and NEXT-STEP tasks only. Do NOT list every state
from the diagram — only the tasks you are actively working on or about to
start. Each item records execution details, lessons learned, and issues
encountered to help with re-planning.

=== MEMO Layer: `notes` (optional) ===
Record any important information, tips, reminders or Identity role-playing prompt.

Example for a "fix a bug" workflow:
- roadmap:
```mermaid
stateDiagram-v2
    [*] --> phase_1_reproduce_bug
    phase_1_reproduce_bug --> phase_1_in_progress: start
    phase_1_in_progress --> phase_1_completed: reproduced
    phase_1_in_progress --> phase_1_failed: cannot reproduce
    phase_1_completed --> phase_2_locate_root_cause
    phase_2_locate_root_cause --> phase_2_in_progress: analyze
    phase_2_in_progress --> phase_2_completed: found cause
    phase_2_completed --> phase_3_implement_fix
    phase_3_implement_fix --> phase_3_in_progress: coding
    phase_3_in_progress --> phase_3_completed: fix works
    phase_3_completed --> [*]
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
This is the big-picture roadmap. Include ALL phases even if not yet started.
Each phase gets state nodes for its statuses (pending/in_progress/completed/failed)
with transitions showing dependencies and error recovery paths.
Use `[*]` for start/end. Replace the entire diagram each call.)"},
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
Use this to record any important information, tips, reminders or Identity role-playing prompt.
)"},
                  },
          },
      },
      {
          "rag_search",
          ToolPrompt{
              .depict =
                  R"(Search the knowledge base for relevant documents using semantic similarity. 
Use this tool to find information before answering questions. 
Returns the most relevant documents with their content, source, and similarity score.)",
              .args =
                  {
                      {"query", "Search query to find relevant documents"},
                      {"top_k", "Number of results to return (default: 3)"},
                  },
          },
      },
      {
          "web_search",
          ToolPrompt{
              .depict =
                  R"(进行网络搜索. 返回一个 markdown 列表结果. 
然后可以使用 fetch_url_markdown 工具拉取网页具体内容.)",
              .args =
                  {
                      {"query", "Search query."},
                  },
          },
      },
      {
          "web_fetch_url",
          ToolPrompt{
              .depict = "(Http GET) 发起网络请求,返回响应体原文.",
              .args =
                  {
                      {"url", "Absolute http/https URL."},
                      {"timeout", "GET requiest timeout, default 60 seconds."},
                  },
          },
      },
      {
          "web_fetch_url_markdown",
          ToolPrompt{
              .depict =
                  R"((Http GET) 拉取一个网页,返回其Markdown格式的页面内容. 
常用于在 web_search 之后获取具体页面内容.)",
              .args =
                  {
                      {"url", R"(Absolute http/https URL.
如果需要获取MD中的相对路径链接网页,应当结合本次传入的`url`. 例如:
- 网页`http://example.com/help/`内:
    - 包含相对路径`model/delete/`(非/开头为相对路径),则顺着当前网页末尾拼接得到的完整链接为`http://example.com/help/model/delete/`
    - 包含相对路径`./model/create/`(以.开头为相对路径),则顺着当前网页末尾拼接得到的完整链接为`http://example.com/help/model/create/`
    - 包含相对路径`../model/create/`(以..开头为相对路径，上一级目录),则顺着当前网页末尾拼接得到的完整链接为`http://example.com/model/create/`
    - 包含绝对路径`/model/view/`(以/开头为绝对路径),则替换网页路径得到`http://example.com/model/view/`
- 网页`http://example.com/help/what.html`内:
    - 包含相对路径`model/delete/`(非/开头为相对路径),则移除末尾文件名，顺着当前网页末尾拼接，得到的完整链接为`http://example.com/help/model/delete/`
)"},
                  },
          },
      },
      {
          "share_store",
          ToolPrompt{
              .depict =
                  R"(Store text, return a unique id, used to get text when need.
Insert text or get/set/delete text by unique id.)",
              .args =
                  {
                      {"opt", R"(operation.
`get` Get text by unique id
`insert` New store `text`, return unique id
`set` Modify `text` by unique id
`delete` Delete text by unique id
)"},
                      {"text", "待存储的文本内容"},
                      {"line_offset",
                       R"(`insert`,`set`时可选. 文本偏移行数,默认`0`表示不偏移.如果偏移超出文件最大行数,将返回错误提示)"},
                      {"line_limit",
                       R"(`insert`,`set`时可选. 读取文本行数限制,取值范围 [1, ~],默认`null`表示不限制.允许指定的限制值超出文件最大行数不报错)"},
                      {"id", "unique id to store text when opt is `get`,`set` "
                             "or `delete`"},
                  },
          },
      },
      {
          "string_html_to_markdown",
          ToolPrompt{
              .depict = "HTML to markdown",
              .args =
                  {
                      {"content", "HTML content."},
                  },
          },
      },
      {
          "string_regexp",
          ToolPrompt{
              .depict =
                  "text search,replace or remove by regexp(Regular Expression)",
              .args =
                  {
                      {"content", "text content."},
                      {"exps", "grep regexp array. Search succeeds if any of "
                               "the provided array match."},
                      {"opt", R"(match operator.
`search` return match result(s).
`replace` replace match result(s) with `replace_str`. return result text content.
`remove` remove match result(s) from content. return result text content.
)"},
                      {"replace_str",
                       R"(Default empty string. When opt is `replace`, replace string for match result(s).)"},
                  },
          },
      },
      {
          "get_current_datetime",
          ToolPrompt{
              .depict = "Get current date,time and timestamp.",
              .args = {},
          },
      },
      {
          "get_system_core_info",
          ToolPrompt{
              .depict =
                  R"(Get system CPU usage, Memory usage, GPU usage, GPU memeory usage.)",
              .args = {},
          },
      },
      {
          "codegraph_search",
          ToolPrompt{
              .depict =
                  R"(Search for code symbols (functions, classes, etc.) by name using 
codegraph index. Returns matched symbols with file locations and 
signatures.)",
              .args =
                  {
                      {"query",
                       "Symbol name to search for (supports partial match)."},
                      {"limit", "Max number of results to return, default 20."},
                  },
          },
      },
      {
          "codegraph_context",
          ToolPrompt{
              .depict =
                  R"(Get rich context for a code symbol: definition, callers, callees, 
and methods (for classes). Useful for understanding how a function 
or class is used in the codebase.)",
              .args =
                  {
                      {"symbol", "Symbol name to get context for (e.g. "
                                 "'MyClass::myMethod')."},
                      {"limit", "Max results per category, default 10."},
                      {"max_depth",
                       "Max call graph traversal depth, default 3."},
                  },
          },
      },
      {
          "codegraph_callers",
          ToolPrompt{
              .depict =
                  R"(Find all functions that call a given symbol. Traces the call graph 
backwards to find callers.)",
              .args =
                  {
                      {"symbol", "Symbol name to find callers for."},
                      {"max_depth", "Max traversal depth, default 3."},
                  },
          },
      },
      {
          "codegraph_callees",
          ToolPrompt{
              .depict =
                  R"(Find all functions that a given symbol calls. Traces the call graph 
forward to find callees.)",
              .args =
                  {
                      {"symbol", "Symbol name to find callees for."},
                      {"max_depth", "Max traversal depth, default 3."},
                  },
          },
      },
      {
          "codegraph_impact",
          ToolPrompt{
              .depict =
                  R"(Analyze the impact of modifying a symbol. Finds all downstream 
symbols that may be affected (callers, references).)",
              .args =
                  {
                      {"symbol", "Symbol name to analyze impact for."},
                      {"max_depth", "Max traversal depth, default 5."},
                  },
          },
      },
      {
          "codegraph_status",
          ToolPrompt{
              .depict =
                  R"(Get codegraph index statistics: total nodes, edges, files, and 
circular dependency count.)",
              .args = {},
          },
      },
      {
          "codegraph_index",
          ToolPrompt{
              .depict =
                  R"(Index a directory for code analysis. Analyzes source files and 
builds the symbol database for search and context queries.)",
              .args =
                  {
                      {"path", "Absolute path to the directory to index."},
                      {"incremental",
                       "Default `true`. Only index changed files."},
                  },
          },
      },
      {
          "codegraph_path",
          ToolPrompt{
              .depict = "Find the call chain path between two symbols in the "
                        "call graph.",
              .args =
                  {
                      {"from", "Starting symbol name."},
                      {"to", "Target symbol name."},
                      {"max_depth", "Max search depth, default 10."},
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
- `mouse_click`: Click. Params: `button`("left"/"right"/"middle", default "left"), `x`, `y`(optional, move then click)
- `mouse_double_click`: Double click. Params: same as mouse_click
- `mouse_scroll`: Scroll wheel. Params: `delta`(positive=up, negative=down, ±120 per notch), `x`, `y`(optional)
- `mouse_drag`: Drag. Params: `x1`, `y1`, `x2`, `y2`, `button`(default "left"), `duration_ms`(default 200)

### Keyboard
- `key_press`: Press and release a key. Params: `key`
- `key_down`: Hold a key down. Params: `key`
- `key_up`: Release a held key. Params: `key`
- `key_combo`: Press key combination (e.g. Ctrl+C). Params: `keys`(array of key names)
- `key_type`: Type a text string. Params: `text`

### Utility
- `wait`: Pause execution. Params: `ms`(milliseconds, max 30000)
- `get_cursor_pos`: Get current cursor position. No params
- `get_screen_size`: Get screen resolution. No params

### Key Names
Single characters: "a"-"z", "0"-"9"
Special keys: "enter", "tab", "escape", "backspace", "delete", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left", "right", "space"
Modifiers: "shift", "ctrl", "alt", "win"
F-keys: "f1"-"f12"
Lock keys: "capslock", "numlock", "scrolllock"
Other: "printscreen", "pause", "apps"

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
                       "Default interval between commands in milliseconds. "
                       "Default 50. Set 0 for no delay."},
                  },
          },
      },
  };

  // ----- 训练用序列化辅助 -----
  // 将整个 AgentPrompt（含 toolPrompt）序列化为 JSON，供训练保存/加载使用

  neograph::json toJson() const {
    neograph::json j;
    j["systemPrompt"] = systemPrompt;
    j["systemPlanningPrompt"] = systemPlanningPrompt;
    j["systemSkillPrompt"] = systemSkillPrompt;
    {
      neograph::json tools = neograph::json::object();
      for (const auto &kv : toolPrompt) {
        neograph::json tp;
        tp["depict"] = kv.second.depict;
        neograph::json args = neograph::json::object();
        for (const auto &a : kv.second.args) {
          args[a.first] = a.second;
        }
        tp["args"] = args;
        tools[kv.first] = tp;
      }
      j["toolPrompt"] = tools;
    }
    return j;
  }

  /// 从 JSON 整体覆盖当前 prompt（缺失字段保持不变）
  void fromJson(const neograph::json &j) { mergeFromJson(j); }

  /// 以 patch 方式合并：仅覆盖 JSON 中出现的字段，未出现的保持原样
  /// - toolPrompt 中某工具若已存在，仅覆盖 JSON 中出现的 depict/args 子字段
  /// - toolPrompt 中某工具若不存在，则插入新建
  void mergeFromJson(const neograph::json &j) {
    if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
      systemPrompt = j["systemPrompt"].get<std::string>();
    }
    if (j.contains("systemPlanningPrompt") &&
        j["systemPlanningPrompt"].is_string()) {
      systemPlanningPrompt = j["systemPlanningPrompt"].get<std::string>();
    }
    if (j.contains("systemSkillPrompt") && j["systemSkillPrompt"].is_string()) {
      systemSkillPrompt = j["systemSkillPrompt"].get<std::string>();
    }
    if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
      auto tools = j["toolPrompt"];
      for (const auto &item : tools.items()) {
        const auto &name = item.first;
        const auto &tp = item.second;
        auto &target = toolPrompt[name]; // 不存在则默认构造插入
        if (tp.contains("depict") && tp["depict"].is_string()) {
          target.depict = tp["depict"].get<std::string>();
        }
        if (tp.contains("args") && tp["args"].is_object()) {
          auto args = tp["args"];
          for (const auto &a : args.items()) {
            if (a.second.is_string()) {
              target.args[a.first] = a.second.get<std::string>();
            }
          }
        }
      }
    }
  }

  /// 计算整个 prompt 的 hash，用于训练种群去重
  size_t promptHash() const {
    size_t h = std::hash<std::string>{}(systemPrompt);
    h ^= std::hash<std::string>{}(systemPlanningPrompt) + 0x9e3779b9 +
         (h << 6) + (h >> 2);
    h ^= std::hash<std::string>{}(systemSkillPrompt) + 0x9e3779b9 + (h << 6) +
         (h >> 2);
    for (const auto &kv : toolPrompt) {
      h ^=
          std::hash<std::string>{}(kv.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<std::string>{}(kv.second.depict) + 0x9e3779b9 + (h << 6) +
           (h >> 2);
      for (const auto &a : kv.second.args) {
        h ^= std::hash<std::string>{}(a.first) + 0x9e3779b9 + (h << 6) +
             (h >> 2);
        h ^= std::hash<std::string>{}(a.second) + 0x9e3779b9 + (h << 6) +
             (h >> 2);
      }
    }
    return h;
  }
};

} // namespace agent
} // namespace agentxx