#pragma once

#include "utilxx_base/json.h"
#include <string>
#include <string_view>

namespace agentxx {
namespace util {

/// 根据工具的参数 JSON Schema 自动修正参数类型兼容性, 尽量让参数类型匹配声明:
/// - string -> 字符串数组: 参数声明为数组 (字符串数组) 而传入单个字符串时, 包装为 `[str]`
/// - object -> 对象数组: 参数声明为数组 (元素为对象) 而传入单个对象时, 包装为 `[obj]`
///   (LLM 少写一层数组的常见形态, 如 `edits`/`actions` 这类参数)
/// - string -> number/integer: 参数声明为数值而传入字符串时, 要求**整串**都是数值才转
///   (integer 允许小数写法并按“相近整数”取整, 如 `"3.5"` -> 3、`"49.9999999"` -> 50;
///   number 支持小数/指数; 前导 '+', 首尾空白会被容忍; `"100 lines"` 不转)
/// - number/integer -> string: 参数声明为字符串而传入数值时, 转为十进制字符串
/// - number(double) <-> integer: 参数声明的数值类型与传入数值类型不同时互相转换
///   (integer -> number 转为 double; number -> integer 取“相近整数”: 与向上取整相差
///   不超过 1e-6 时向上取整, 否则向下取整 —— `3.7` -> 3、`4.999999999` -> 5、
///   `-3.7` -> -4; 超出 int64 范围或 NaN/Inf 保持原样)
/// - bool -> string / string -> boolean: 布尔与字符串互相转换
///   (字符串忽略首尾空白与 ASCII 大小写: `true`/`yes`/`y`/`on`/`enable`/`enabled`/`1`
///   为真, `false`/`no`/`n`/`off`/`disable`/`disabled`/`0` 为假; 其它文本
///   保持原样, 由工具参数校验给出明确错误)
/// - [单字符串数组] -> string: 参数声明为字符串而传入单元素字符串数组时, 解包为字符串
/// - [单对象数组] -> object: 参数声明为对象而传入单元素对象数组时, 解包为对象
/// - 枚举字符串大小写: 参数值命中 schema `enum` 声明的字符串枚举项 (忽略大小写)
///   时改写为规范值, 如 LLM 传 "File" 而枚举声明为 `["file","dir"]` 时转为 "file"
/// - 仅当目标类型不包含参数当前类型时转换; 无法解析或类型不明确时保持原样
///
/// 执行侧 (工具真正拿到参数前, 见 [ToolcallWrapNode::autoFixArgsType]) 与展示侧
/// (工具展示行的参数文本, 见 [autoFixedToolArgsJson]) 共用本实现: 特化渲染器看到
/// 的参数形态与工具实际收到的完全一致, 渲染器不需要再写类型兼容分支。
///
/// - `args`:
///     - [parameters] 工具参数 JSON Schema (`neograph::ChatTool::parameters` 的业务形态);
///       非对象时不做任何转换
///     - [args] 待修正的参数 (原地修改); 不是 JSON 对象时不做任何转换
///     - [toolName] 仅用于日志
///
/// - `return` 是否发生了参数转换
bool autoFixToolArgsTypes(
    const utilxx_base::Json& parameters,
    utilxx_base::Json&       args,
    std::string_view         toolName = "?"
);

/// [autoFixToolArgsTypes] 的文本级入口: 输入输出都是参数 JSON 文本
/// - 供展示历史统一参数形态 (工具行写入 / 历史加载 / 分页读取), 让特化渲染器
///   直接按 schema 声明的类型读参数
/// - 未发生转换、参数为空、解析失败、不是 JSON 对象: 原样返回输入文本
/// - 发生转换时返回重新 dump 的文本 (紧凑格式)
///
/// - `args`:
///     - [parameters] 工具参数 JSON Schema; 非对象时原样返回
///     - [argsJson] 参数 JSON 文本
///
/// - `return` 参数文本 (无转换时即输入本身)
std::string autoFixedToolArgsJson(const utilxx_base::Json& parameters, std::string_view argsJson);

} // namespace util
} // namespace agentxx
