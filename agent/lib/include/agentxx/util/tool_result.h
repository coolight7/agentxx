/// 工具执行结果的**状态位**与结果正文 (全项目唯一一份)
///
/// 背景: 工具结果是一段给模型读的文本, 「这次调用成功还是失败」不能靠读文本判断。
/// 本头把状态与文本拆开:
/// - 工具实现只返回 [ToolResult]: `status` 说结果类型, `text` 是正文;
/// - 状态到展示文本的格式化只在执行层做一份 (见
///   [toolcall.cpp](/agent/lib/src/nodes/toolcall.cpp) 的 `formatToolResultText`):
///   只有 `Failed` 会被套上 `[Error: ...]`, 其余状态原样透传;
/// - 插件 / JS 插件 / 客户端跨语言看到的都是状态位 (Wire 与插件 C ABI 的 `status`),
///   任何一方都**不允许**为了判断成败去看文本。
///
/// 文本约定: `Failed` 的 `text` 直接写错误内容 (`path does not exist`、
/// `timed out after 60 seconds`), 不加 `[Error]` / `[Plugin tool timeout]` 之类前缀,
/// 也不写模型不需要知道的对象名; 多行错误内容允许, 由执行层排版。
///
/// 批量操作里「部分条目失败」不算整次调用失败: 结果保持 `Ok`, 文本里逐条说明。
///
/// 本头只依赖标准库的字符串, 主程序与插件都能包含 (插件构建已包含
/// `agent/lib/include`, 见各插件 `CMakeLists.txt` 的 `AGENTXX_LIB_INCLUDE_DIR`)。
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace agentxx {
namespace util {

/// 一次工具调用的终态
enum class ToolStatus {
    Ok = 0,
    Failed,      ///< 调用失败 (参数错 / 执行错 / 权限拒绝 / 超时 / 工具不存在 ...)
    Interrupted, ///< 触发了中断 (等待用户确认等), 结果由中断恢复后补
    Canceled,    ///< 被取消 (会话取消 / 客户端断开)
};

/// 状态的名字 (wire / JSON 上的取值, 小写)
/// - `args`:
///     - [status] 终态
/// - `return` `ok` / `failed` / `interrupted` / `canceled`
inline std::string_view toolStatusName(ToolStatus status) noexcept {
    switch (status) {
        case ToolStatus::Ok:
            return "ok";
        case ToolStatus::Failed:
            return "failed";
        case ToolStatus::Interrupted:
            return "interrupted";
        case ToolStatus::Canceled:
            return "canceled";
    }
    return "ok";
}

/// 由名字还原状态 (大小写敏感, 取值见 [toolStatusName])
/// - **未知取值一律按 `Ok`**: 新版本新增的状态值在老版本上不会翻成失败
/// - `args`:
///     - [name] wire / JSON 上的状态名
/// - `return` 对应的终态
inline ToolStatus toolStatusFromName(std::string_view name) noexcept {
    if (name == "failed") {
        return ToolStatus::Failed;
    }
    if (name == "interrupted") {
        return ToolStatus::Interrupted;
    }
    if (name == "canceled") {
        return ToolStatus::Canceled;
    }
    return ToolStatus::Ok;
}

/// 工具执行结果: 状态位 + 给模型读的正文
struct ToolResult {
    /// 给模型读的正文 (成功时的结果文本; 失败时的**错误内容**, 不带 `[Error: ]` 这类包裹)
    std::string text;
    ToolStatus  status = ToolStatus::Ok;

    /// 成功结果
    static ToolResult ok(std::string text) {
        return ToolResult{std::move(text), ToolStatus::Ok};
    }

    /// 失败结果 (参数错 / 预期内的失败 / 超时 ...); [errorText] 只写错误内容
    static ToolResult fail(std::string errorText) {
        return ToolResult{std::move(errorText), ToolStatus::Failed};
    }

    /// 中断占位 (仅为消息结构完整; 语义由 `MessageFlag::Interrupt` 表示)
    static ToolResult interrupted(std::string text = {}) {
        return ToolResult{std::move(text), ToolStatus::Interrupted};
    }

    /// 取消 (会话取消 / 客户端断开)
    static ToolResult canceled(std::string text = {}) {
        return ToolResult{std::move(text), ToolStatus::Canceled};
    }

    bool isOk() const noexcept {
        return status == ToolStatus::Ok;
    }
};

/// 状态 -> 给模型读的文本 (全项目唯一实现)
///
/// 只有 `Failed` 会改变文本, 其余状态一律原样透传:
/// - `Ok`: 原样, 不做任何包裹
/// - `Failed`: 单行 `[Error: <text>]`; 含换行时首行单独 `[Error]`, 其后原样接 `<text>`;
///   `text` 为空时只有 `[Error]`
/// - `Canceled`: 文本原样; 未产出结果时补占位 `[Canceled]`
/// - `Interrupted`: 文本原样; 未产出结果时补占位 `[Interrupt]`
///
/// 调用方: 执行层写上下文时用它 (见 `nodes/toolcall.cpp`); 工具实现**不要**调它
/// —— 工具只给正文与状态, 不用关心模型侧的标记格式
///
/// - `args`:
///     - [result] 工具结果
/// - `return` 写进会话上下文的工具消息正文
inline std::string toolResultTextForModel(const ToolResult& result) {
    switch (result.status) {
        case ToolStatus::Failed:
            if (result.text.empty()) {
                return "[Error]";
            }
            if (result.text.find('\n') == std::string::npos) {
                return "[Error: " + result.text + "]";
            }
            return "[Error]\n" + result.text;
        case ToolStatus::Canceled:
            return result.text.empty() ? std::string{"[Canceled]"} : result.text;
        case ToolStatus::Interrupted:
            return result.text.empty() ? std::string{"[Interrupt]"} : result.text;
        case ToolStatus::Ok:
        default:
            return result.text;
    }
}

} // namespace util
} // namespace agentxx
